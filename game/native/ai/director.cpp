// Rewritten's enhanced enemy AI: the squad director, once a frame after the
// AI updates. The scripts still decide what each soldier does; the director
// shares what they see and hands out jobs.
//
// - Callouts: squadmates near a soldier who sees the player, and not
//   fighting yet, hear a noise at the player's position after a moment (the
//   scripts' Alert handling turns them, finds cover and engages).
// - Attack tokens: only a few soldiers who see the player aim accurately
//   (aim.cpp), the best placed, passed round every few seconds.
// - Roles: in a fight of three or more, the Roamers furthest off to the
//   side flank (cover.cpp scores cover round the player's side for them);
//   the rest hold cover that sees the player.
// - Near misses: a soldier the player's bullet passes close to hears where
//   it came from.

#include "ai_internal.h"
#include "game/AIDoodad.h"
#include "game/ISceneNode.h"
#include "game/player.h"
#include "game/projectilebullet.h"
#include "game/vector.h"
#include "game/weapon.h"

namespace nai {
namespace {

const int kMaxFighters = 64;
const float kTokenRest = 1.5f;   // s before a soldier can hold a token again
const float kRoleKeep = 12.0f;   // s a flanker keeps its role
const float kQuiet = 8.0f;       // s between callouts to the same soldier

float s_nextCallouts;
float s_nextTokens;
float s_nextRoles;

// The noise the player makes, heard by one soldier.
void TellWhereThePlayerIs(CAIObject* ai, CAIObject* player, int delay) {
    CAIFilterSoundInfo info;
    info.m_source = (ISceneNode*)Player();
    info.m_sourceFilter = player->m_pFilter;
    info.m_position = player->m_position;
    info.m_type = 0x34;
    info.m_sourceScript = ScriptOf(player);
    info.m_radius = 10.0f;
    info.m_volume = 3.0f;
    info.field_0x28 = true;
    info.field_0x2C = 10;
    NativeAITellHeard(ai, info, delay);
}

void Callouts(float now, CAIObject* player, State** seeing, int numSeeing) {
    if (g_profile.calloutRadius <= 0.0f) {
        return;
    }
    if (numSeeing > 0 && now >= s_nextCallouts) {
        s_nextCallouts = now + 0.25f;
        for (int i = 0; i < NumStates(); i++) {
            State* s = StateAt(i);
            if (!s->squad || !s->hostile || s->ai->m_pTarget != NULL || s->calloutAt > 0.0f || now < s->quietUntil ||
                !Alive(s->ai)) {
                continue;
            }
            for (int j = 0; j < numSeeing; j++) {
                if (DistanceXY(ToVec(s->ai->m_position), ToVec(seeing[j]->ai->m_position)) <= g_profile.calloutRadius) {
                    s->calloutAt = now + g_profile.calloutDelay * RandomRange(0.75f, 1.25f);
                    Trace("%p calls out the player to %p", seeing[j]->ai, s->ai);
                    break;
                }
            }
        }
    }
    for (int i = 0; i < NumStates(); i++) {
        State* s = StateAt(i);
        if (s->calloutAt <= 0.0f || now < s->calloutAt) {
            continue;
        }
        s->calloutAt = 0.0f;
        s->quietUntil = now + kQuiet;
        if (s->ai->m_pTarget == NULL && Alive(s->ai)) {
            TellWhereThePlayerIs(s->ai, player, 1);
        }
    }
}

void Tokens(float now, State** fighters, int numFighters, int numSeeing, CAIObject* player) {
    int held = 0;
    for (int i = 0; i < numFighters; i++) {
        State* s = fighters[i];
        if (!s->token) {
            continue;
        }
        if (!s->ai->m_targetVisible || (now - s->tokenSince >= g_profile.tokenHold && numSeeing > g_profile.tokens)) {
            s->token = false;
            s->tokenRestUntil = now + kTokenRest;
            Trace("%p passes its token on", s->ai);
        } else {
            held++;
        }
    }
    Vec3 at = ToVec(player->m_position);
    while (held < g_profile.tokens) {
        // The closest soldier who sees the player and isn't resting, or
        // failing that the closest who sees the player.
        State* best = NULL;
        float bestDist = 1e30f;
        for (int pass = 0; pass < 2 && best == NULL; pass++) {
            for (int i = 0; i < numFighters; i++) {
                State* s = fighters[i];
                if (s->token || !s->ai->m_targetVisible || (pass == 0 && now < s->tokenRestUntil)) {
                    continue;
                }
                float d = Length(ToVec(s->ai->m_position) - at);
                if (d < bestDist) {
                    bestDist = d;
                    best = s;
                }
            }
        }
        if (best == NULL) {
            break;
        }
        best->token = true;
        best->tokenSince = now;
        held++;
        Trace("%p takes a token (%.0f from the player)", best->ai, bestDist);
    }
}

void Roles(float now, State** fighters, int numFighters, CAIObject* player) {
    if (g_profile.flankers <= 0 || numFighters < 3) {
        for (int i = 0; i < numFighters; i++) {
            fighters[i]->role = ROLE_NONE;
        }
        return;
    }
    Vec3 center;
    SquadCenter(&center);
    Vec3 at = ToVec(player->m_position);
    Vec3 axis = center - at;
    axis.z = 0.0f;
    axis = Normalized(axis);
    int flanking = 0;
    for (int i = 0; i < numFighters; i++) {
        State* s = fighters[i];
        if (s->role == ROLE_FLANK && s->roamer && now - s->roleSince < kRoleKeep) {
            flanking++;
        } else if (s->role != ROLE_SUPPRESS) {
            s->role = ROLE_SUPPRESS;
            s->roleSince = now;
        }
    }
    while (flanking < g_profile.flankers) {
        // The Roamer furthest off the line from the player to the squad.
        State* best = NULL;
        float bestSide = -1.0f;
        for (int i = 0; i < numFighters; i++) {
            State* s = fighters[i];
            if (s->role == ROLE_FLANK || !s->roamer) {
                continue;
            }
            Vec3 d = ToVec(s->ai->m_position) - at;
            float side = fabsf(axis.x * d.y - axis.y * d.x);
            if (side > bestSide) {
                bestSide = side;
                best = s;
            }
        }
        if (best == NULL) {
            break;
        }
        best->role = ROLE_FLANK;
        best->roleSince = now;
        flanking++;
        Trace("%p flanks (%.1f off the line)", best->ai, bestSide);
    }
}

}  // namespace

void ResetDirector() {
    s_nextCallouts = 0.0f;
    s_nextTokens = 0.0f;
    s_nextRoles = 0.0f;
}

bool SquadCenter(Vec3* out) {
    CAIObject* player = PlayerAI();
    Vec3 sum;
    int count = 0;
    for (int i = 0; i < NumStates(); i++) {
        State* s = StateAt(i);
        if (s->soldier && s->hostile && player != NULL && s->ai->m_pTarget == player && Alive(s->ai)) {
            sum += ToVec(s->ai->m_position);
            count++;
        }
    }
    if (count == 0) {
        return false;
    }
    *out = sum * (1.0f / count);
    return true;
}

bool PlayerPressed() {
    CPlayerObject* player = Player();
    CAIObject* ai = PlayerAI();
    if (player == NULL || ai == NULL) {
        return false;
    }
    CWeapon* weapon = player->GetCurrentWeapon();
    return (weapon != NULL && weapon->m_bReloading) || ai->m_status.m_vars[AISTATUS_HEALTH] <= 5;
}

}  // namespace nai

using namespace nai;

void NativeAIDirectorUpdate() {
    CAIObject* player = PlayerAI();
    if (player == NULL || !Alive(player)) {
        return;
    }
    float now = Now();
    State* fighters[kMaxFighters];
    State* seeing[kMaxFighters];
    int numFighters = 0;
    int numSeeing = 0;
    for (int i = 0; i < NumStates(); i++) {
        State* s = StateAt(i);
        if (s->soldier && s->hostile && s->ai->m_pTarget == player && Alive(s->ai) && numFighters < kMaxFighters) {
            fighters[numFighters++] = s;
            if (s->ai->m_targetVisible) {
                seeing[numSeeing++] = s;
            }
        } else {
            s->token = false;
            s->role = ROLE_NONE;
        }
    }
    Callouts(now, player, seeing, numSeeing);
    if (now >= s_nextTokens) {
        s_nextTokens = now + 0.25f;
        Tokens(now, fighters, numFighters, numSeeing, player);
    }
    if (now >= s_nextRoles) {
        s_nextRoles = now + 2.0f;
        Roles(now, fighters, numFighters, player);
    }
}

void NativeAIBulletFlying(CProjectileBullet* bullet, float dt) {
    // (Compared, not asked: a soldier's bullet can outlive him.)
    CAIObject* player = PlayerAI();
    if (player == NULL || bullet->m_pFiredBy == NULL || bullet->m_pFiredBy != (ISceneNode*)Player()) {
        return;
    }
    Vec3 from(bullet->m_basePos.x, bullet->m_basePos.y, bullet->m_basePos.z);
    Vec3 dir(bullet->m_forward.x, bullet->m_forward.y, bullet->m_forward.z);
    Vec3 to = from + dir * (bullet->m_speed * dt);
    float now = Now();
    for (int i = 0; i < NumStates(); i++) {
        State* s = StateAt(i);
        if (!s->squad || !s->hostile || s->ai->m_pTarget != NULL || now < s->nearMissUntil || !Alive(s->ai)) {
            continue;
        }
        if (SegmentDistance(from, to, Up(ToVec(s->ai->m_position), 1.0f), NULL) < 1.5f) {
            s->nearMissUntil = now + 2.0f;
            TellWhereThePlayerIs(s->ai, player, (int)RandomRange(4.0f, 12.0f));
            Trace("%p: a bullet passed close", s->ai);
        }
    }
}
