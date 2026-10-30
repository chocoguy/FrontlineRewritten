// Rewritten's enhanced enemy AI: cover and running away.
//
// The original takes the nearest cover point that passes its distance and
// angle rules (CAIFilterGlobal::ChooseCoverPointByType), with no sight test.
// Here the same rules pick the candidates, then a score picks among them:
// the walk there, whether the threat can see the spot, whether the peek spot
// can shoot back, crowding, the player's view, ground gained (by profile),
// and for flankers the angle round the player's side.
//
// Cover type 0 ("forward far") can never match in the original (its test
// contradicts the candidate filter); with the profile's forwardCover it means
// forward cover at least halfway to the threat.

#include "ai_internal.h"
#include "game/CoverPoint.h"
#include "game/ISceneNode.h"
#include "game/PathNode.h"
#include "game/PathNodeList.h"
#include "game/bstypes.h"
#include "game/matrix.h"
#include "game/propdat.h"
#include "game/quaternion.h"
#include "game/vector.h"

namespace nai {
namespace {

const u32 kCoverCrc = 0x9101C906;
const int kTriggerWaypoint = 5;
const int kMaxCovers = 1024;
const float kViewTime = 0.5f;   // sight lines from the threat are kept this long
const float kViewMove = 0.75f;  // or until the threat moves this far

// Sight lines from the threat's eye to a cover point, per peek type
// (left, right, over): -1 not tested yet.
struct View {
    Vec3 eye;
    float until;
    s8 hidden[3];  // 2 hidden, 1 partly, 0 seen
    s8 sees[3];    // the peek spot sees the threat
};

CCoverPoint* s_covers[kMaxCovers];
View s_views[kMaxCovers];
int s_numCovers = -1;  // -1: not gathered for this level yet

void Gather() {
    s_numCovers = 0;
    for (int i = 0; i < g_pNumTriggers && s_numCovers < kMaxCovers; i++) {
        TriggerObject_struct* t = &g_pTriggerObjects[i];
        TriggerObjectData_struct* data = t->m_data;
        if (data != NULL && data->m_type == kTriggerWaypoint && (u32)data->field_0x48 == kCoverCrc) {
            s_views[s_numCovers].until = -1.0f;
            s_covers[s_numCovers++] = (CCoverPoint*)t;
        }
    }
    Trace("%d cover points", s_numCovers);
}

int IndexOf(const CCoverPoint* cover) {
    if (s_numCovers < 0) {
        Gather();
    }
    for (int i = 0; i < s_numCovers; i++) {
        if (s_covers[i] == cover) {
            return i;
        }
    }
    return -1;
}

int TypeSlot(int type) { return type == COVERPOINT_LEFT ? 0 : type == COVERPOINT_RIGHT ? 1 : 2; }

u8 CoverTeam(const CCoverPoint* cover) { return ((const TriggerObject_struct*)cover)->m_data->m_team; }

// The peek types the cover's flags allow (CoverPoint.cpp's GetPeekTypes; the
// flags are little-endian, as the CPU).
u32 PeekTypes(const CCoverPoint* cover) {
    u32 flags = cover->m_data->flags;
    u32 types = 0;
    if (flags & (CPF_LEFT_0 | CPF_LEFT_1 | CPF_LEFT_2)) {
        types |= COVERPOINT_LEFT;
    }
    if (flags & (CPF_RIGHT_0 | CPF_RIGHT_1 | CPF_RIGHT_2)) {
        types |= COVERPOINT_RIGHT;
    }
    if (flags & (CPF_OVER_0 | CPF_OVER_1)) {
        types |= COVERPOINT_OVER;
    }
    return types;
}

Vec3 Facing(const CCoverPoint* cover) {
    CMatrix m;
    cover->m_data->orientation.GetMatrix(m);
    return Vec3(m.m[1][0], m.m[1][1], m.m[1][2]);
}

Vec3 StandSpot(const CCoverPoint* cover, int type) {
    CAIFilterRealVector3 spot;
    cover->GetModifiedPosition(spot, (COVERPOINT_TYPE)type);
    return ToVec(spot);
}

// Where a soldier peeks from for `type`: as CCoverPoint::GetPeekLocation,
// but for the side it claimed (the original always uses the first allowed).
Vec3 PeekSpot(const CCoverPoint* cover, int type) {
    Vec3 f = Facing(cover);
    Vec3 dir;
    if (type == COVERPOINT_LEFT) {
        dir = Vec3(-f.y, f.x, f.z);
    } else if (type == COVERPOINT_RIGHT) {
        dir = Vec3(f.y, -f.x, f.z);
    } else {
        dir = Vec3(0.0f, 0.0f, 1.0f);
    }
    return Up(StandSpot(cover, type) + dir * 0.8f, 1.0f);
}

View& ViewOf(int index, const Vec3& eye) {
    View& v = s_views[index];
    if (Now() > v.until || Length(eye - v.eye) > kViewMove) {
        v.eye = eye;
        v.until = Now() + kViewTime;
        for (int i = 0; i < 3; i++) {
            v.hidden[i] = -1;
            v.sees[i] = -1;
        }
    }
    return v;
}

// How well the stand spot for `type` hides from `eye`, at crouching heights.
int Hidden(int index, const CCoverPoint* cover, int type, const Vec3& spot, const Vec3& eye) {
    View& v = ViewOf(index, eye);
    s8& h = v.hidden[TypeSlot(type)];
    if (h < 0) {
        h = (s8)(!ClearSight(eye, Up(spot, 0.5f), NULL) + !ClearSight(eye, Up(spot, 0.95f), NULL));
    }
    return h;
}

// Whether the peek spot for `type` sees the threat (standing or crouched).
bool Sees(int index, const CCoverPoint* cover, int type, const Vec3& threat, const Vec3& eye) {
    View& v = ViewOf(index, eye);
    s8& s = v.sees[TypeSlot(type)];
    if (s < 0) {
        Vec3 peek = PeekSpot(cover, type);
        s = (s8)(ClearSight(peek, Up(threat, 1.0f), NULL) || ClearSight(peek, Up(threat, 0.5f), NULL));
    }
    return s != 0;
}

// The original's placement rules for `type`, relative to the line to the
// threat (along: cosine of the angle off it).
bool TypeMatches(int type, float along, float fromUs, float fromThreat, float threatDistSq) {
    switch (type) {
    case 0:
        return g_profile.forwardCover && along > 0.38f && fromUs >= 0.25f * threatDistSq && fromUs < threatDistSq;
    case 1:
        return along > 0.38f && fromUs < fromThreat && fromUs < threatDistSq;
    case 2:
        return fabsf(along) < 0.38f && fromUs < fromThreat && threatDistSq < fromUs;
    case 3:
        return fabsf(along) < 0.38f && fromUs < fromThreat && fromUs < threatDistSq;
    case 4:
        return along < -0.38f && fromUs < fromThreat && threatDistSq < fromUs;
    case 5:
        return along < -0.38f && fromUs < fromThreat && fromUs < threatDistSq;
    case 6:
        return true;
    }
    return false;
}

// Others of the side already there, or headed there.
float Crowding(const CAIObject* ai, const Vec3& spot) {
    float cost = 0.0f;
    for (int i = 0; i < NumStates(); i++) {
        CAIObject* other = StateAt(i)->ai;
        if (other == ai || !StateAt(i)->soldier || other->field_0x14 != ai->field_0x14 || !Alive(other)) {
            continue;
        }
        if (DistanceXY(ToVec(other->m_position), spot) < 2.5f) {
            cost += 2.0f;
        }
        if (other->m_pCoverPoint != NULL && DistanceXY(ToVec(other->m_coverPosition), spot) < 2.5f) {
            cost += 2.0f;
        }
    }
    return cost;
}

bool IsPlayerAt(const Vec3& pos) {
    CAIObject* player = PlayerAI();
    return player != NULL && Length(ToVec(player->m_position) - pos) < 0.5f;
}

}  // namespace

void ResetCover() { s_numCovers = -1; }

}  // namespace nai

using namespace nai;

CCoverPoint* NativeAIChooseCover(const CAIFilterRealPosition& pos, const CAIFilterRealPosition* threat, int type,
                                 float maxDistSq, float maxThreatDistSq, CAIObject* ai) {
    if (s_numCovers < 0) {
        Gather();
    }
    Vec3 me = ToVec(pos);
    Vec3 th = ToVec(*threat);
    float threatDistSq = Dot(th - me, th - me);
    if (threatDistSq < 36.0f) {
        return NULL;
    }
    float threatDist = sqrtf(threatDistSq);
    Vec3 toThreat = Normalized(th - me);
    bool atPlayer = IsPlayerAt(th);
    Vec3 eye = atPlayer ? PlayerEye() : Up(th, 1.5f);
    Vec3 look = atPlayer ? PlayerLook() : Vec3();

    State* s = Find(ai);
    Vec3 center;
    bool flank = s != NULL && s->role == ROLE_FLANK && SquadCenter(&center);
    float advance = g_profile.advance;
    if (flank && PlayerPressed()) {
        advance *= 2.0f;
    }
    u8 team = ai->field_0x14;

    CCoverPoint* best = NULL;
    float bestScore = 1e30f;
    int candidates = 0;
    for (int i = 0; i < s_numCovers; i++) {
        CCoverPoint* c = s_covers[i];
        int peek = c->GetAvailablePeekPosition();
        u8 coverTeam = CoverTeam(c);
        if (peek == 0 || (coverTeam != 0 && coverTeam != team)) {
            continue;
        }
        Vec3 spot = StandSpot(c, peek);
        float fromThreat = Dot(th - spot, th - spot);
        float fromUs = Dot(me - spot, me - spot);
        if (fromThreat >= maxThreatDistSq || fromUs >= maxDistSq || !(fromUs < fromThreat)) {
            continue;
        }
        float along = Dot(toThreat, Normalized(spot - me));
        if (!TypeMatches(type, along, fromUs, fromThreat, threatDistSq)) {
            continue;
        }
        CAIFilterRealPosition at = ToPos(spot);
        if (!c->ProvidesCover(at, threat, ai)) {
            continue;
        }
        candidates++;
        float score = sqrtf(fromUs);
        int hidden = Hidden(i, c, peek, spot, eye);
        score += hidden == 2 ? 0.0f : hidden == 1 ? 3.0f : 8.0f;
        if (!Sees(i, c, peek, th, eye)) {
            score += 4.0f;
        }
        score += Crowding(ai, spot);
        if (atPlayer && Dot(look, Normalized(spot - eye)) > 0.97f) {
            score += 2.0f;
        }
        score -= advance * 0.5f * (threatDist - sqrtf(fromThreat));
        if (flank) {
            Vec3 a = spot - th;
            Vec3 b = center - th;
            a.z = 0.0f;
            b.z = 0.0f;
            float cosine = Dot(Normalized(a), Normalized(b));
            score += cosine < 0.5f ? -6.0f : 4.0f * (cosine - 0.5f);
        }
        if (score < bestScore) {
            bestScore = score;
            best = c;
        }
    }
    if (candidates > 0) {
        Trace("%p cover type %d: %d candidates, picked %d (score %.1f)%s", ai, type, candidates,
              best != NULL ? IndexOf(best) : -1, bestScore, flank ? " flanking" : "");
    }
    return best;
}

// Of the free peek sides, the first (in the original's order: right, over,
// left) whose peek spot sees the target.
int NativeAIChoosePeek(CAIObject* ai, CCoverPoint* cover, int original) {
    u32 avail = PeekTypes(cover) & ~cover->m_occupied & 7;
    CAIObject* target = ai->m_pTarget;
    if (target == NULL || (avail & (avail - 1)) == 0) {
        return original;
    }
    int index = IndexOf(cover);
    if (index < 0) {
        return original;
    }
    Vec3 th = ToVec(target->m_position);
    Vec3 eye = EyeOf(target);
    const int order[3] = {COVERPOINT_RIGHT, COVERPOINT_OVER, COVERPOINT_LEFT};
    for (int i = 0; i < 3; i++) {
        if ((avail & order[i]) && Sees(index, cover, order[i], th, eye)) {
            if (order[i] != original) {
                Trace("%p peeks %s instead (sees the target)", ai,
                      order[i] == COVERPOINT_LEFT ? "left" : order[i] == COVERPOINT_RIGHT ? "right" : "over");
            }
            return order[i];
        }
    }
    return original;
}

CAIFilterRealPosition NativeAIPeekLocation(CCoverPoint* cover, int type) {
    if (type != COVERPOINT_LEFT && type != COVERPOINT_RIGHT && type != COVERPOINT_OVER) {
        return cover->GetPeekLocation();
    }
    return ToPos(PeekSpot(cover, type));
}

// Seen at its cover (stand spot visible at both crouching heights) for
// 1.5 s; then 3 s of grace while it moves.
bool NativeAICoverExposed(CAIObject* ai, const CAIFilterRealPosition& spot) {
    State* s = Find(ai);
    CAIObject* target = ai->m_pTarget;
    float now = Now();
    if (s == NULL || target == NULL || s->exposedSince > now) {
        return false;
    }
    Vec3 eye = EyeOf(target);
    Vec3 at = ToVec(spot);
    if (!ClearSight(eye, Up(at, 0.5f), NULL) || !ClearSight(eye, Up(at, 0.95f), NULL)) {
        s->exposedSince = -1.0f;
        return false;
    }
    if (s->exposedSince < 0.0f) {
        s->exposedSince = now;
        return false;
    }
    if (now - s->exposedSince < 1.5f) {
        return false;
    }
    s->exposedSince = now + 3.0f;
    Trace("%p is exposed at its cover: moves", ai);
    return true;
}

// A breadth-first walk over the path nodes from the soldier, keeping the
// nearest (by path) nodes far enough from the danger, then preferring one
// the player can't see. (The original walks greedily away and can get stuck.)
bool NativeAIFindRunAwayPoint(CAIObject* ai, const ISceneNode* threat, float dist, CAIFilterRealPosition* out) {
    const int kMaxNodes = 8192;
    const int kMaxVisit = 256;
    const int kKeep = 6;
    static u32 stamp[kMaxNodes];
    static u32 generation;
    CPathNodeList* list = g_aigAIFilterGlobalObject.m_pathNodes;
    if (list == NULL || list->m_numNodes <= 0 || list->m_numNodes > kMaxNodes || threat == NULL) {
        return false;
    }
    Vec3 danger;
    if (!GrenadeLanding(threat, &danger)) {
        CVector3 tp;
        threat->GetPosition(tp);
        danger = Vec3(tp.x, tp.y, tp.z);
    }
    CPathNode* start = list->GetClosestNode(ai->m_position, false);
    if (start == NULL) {
        return false;
    }
    if (++generation == 0) {
        memset(stamp, 0, sizeof(stamp));
        generation = 1;
    }
    int queue[kMaxVisit];
    float cost[kMaxVisit];
    int head = 0;
    int tail = 0;
    queue[tail] = (int)(start - list->m_nodes);
    cost[tail++] = DistanceXY(ToVec(ai->m_position), Vec3(start->m_pos[0], start->m_pos[1], start->m_pos[2]));
    stamp[queue[0]] = generation;
    int keep[kKeep];
    float keepCost[kKeep];
    int kept = 0;
    float limit = 3.0f * dist + 8.0f;
    float need = dist + 0.75f;
    while (head < tail) {
        int index = queue[head];
        float c = cost[head++];
        CPathNode* node = &list->m_nodes[index];
        Vec3 p(node->m_pos[0], node->m_pos[1], node->m_pos[2]);
        if (node->m_enabled && DistanceXY(p, danger) >= need) {
            // Keep the cheapest few, in order.
            int at = kept < kKeep ? kept++ : kKeep;
            while (at > 0 && keepCost[at - 1] > c) {
                if (at < kKeep) {
                    keep[at] = keep[at - 1];
                    keepCost[at] = keepCost[at - 1];
                }
                at--;
            }
            if (at < kKeep) {
                keep[at] = index;
                keepCost[at] = c;
            }
            continue;  // no need to go further this way
        }
        s16* link = node->m_links;
        for (int i = 0; i < node->m_numLinks && tail < kMaxVisit; i++) {
            int next = (u16)link[i];
            if (next >= list->m_numNodes || stamp[next] == generation) {
                continue;
            }
            CPathNode* n = &list->m_nodes[next];
            float nc = c + Length(Vec3(n->m_pos[0], n->m_pos[1], n->m_pos[2]) - p);
            if (nc > limit || !n->m_enabled) {
                continue;
            }
            stamp[next] = generation;
            queue[tail] = next;
            cost[tail++] = nc;
        }
    }
    if (kept == 0) {
        return false;
    }
    CAIObject* player = PlayerAI();
    int best = keep[0];
    float bestScore = 1e30f;
    for (int i = 0; i < kept; i++) {
        CPathNode* node = &list->m_nodes[keep[i]];
        Vec3 p(node->m_pos[0], node->m_pos[1], node->m_pos[2]);
        float score = keepCost[i];
        if (player != NULL && ClearSight(PlayerEye(), Up(p, 1.0f), NULL)) {
            score += 4.0f;
        }
        if (score < bestScore) {
            bestScore = score;
            best = keep[i];
        }
    }
    CPathNode* node = &list->m_nodes[best];
    out->field_0x0 = node->m_pos[0];
    out->field_0x4 = node->m_pos[1];
    out->field_0x8 = node->m_pos[2];
    Trace("%p runs %.1f away from danger (path %.1f, %d nodes searched)", ai,
          DistanceXY(ToVec(*out), danger), bestScore, tail);
    return true;
}
