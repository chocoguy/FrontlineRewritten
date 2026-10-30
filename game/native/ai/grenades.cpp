// Rewritten's enhanced enemy AI: grenades.
//
// - Awareness: the original warns soldiers about a grenade only once it lies
//   still (CThrownBullet::CheckEnemies), once. Here each grenade's landing
//   spot is predicted while it flies, and after the profile's reaction time
//   every soldier near it gets the same events (0xAC/0xAD for the closest,
//   0xAE for the rest), including ones who walk up later. The scripts' own
//   reactions (run, catch, throw back) do the rest; running away uses the
//   predicted spot (cover.cpp).
// - Throwing: the original solver tries one 30-degree arc with two line
//   tests. Here 30, 45 and 20 degrees are tried along 8 segments each, and
//   the throw leaves at the slope that was clear.
// - Fairness: one enemy grenade at a time, with a cooldown between them.

#include "ai_internal.h"
#include "game/AIDoodad.h"
#include "game/BSGO_Basic.h"
#include "game/ISceneNode.h"
#include "game/Soldier_object.h"
#include "game/bsobject.h"
#include "game/bstypes.h"
#include "game/objcreate.h"
#include "game/thrownbullet.h"
#include "game/vector.h"

namespace nai {
namespace {

// The event data of the grenade events (thrownbullet.cpp's SGrenadeInfo);
// scripts read it by offset.
struct GrenadeInfo {
    /* 0x0 */ PTR32(BSObject) m_thrower;
    /* 0x4 */ PTR32(CThrownBullet) m_grenade;
    /* 0x8 */ int m_numFriends;
};

const int kMaxWatches = 16;
const int kMaxTold = 24;
const int kPredictSteps = 40;  // of 3 frames: 2 seconds of flight
const float kWarnRadius = 6.0f;

struct Watch {
    CThrownBullet* grenade;
    u32 lastFrame;
    u32 nextPredict;
    float born;
    bool hostileThrower;
    bool haveLanding;
    bool warned;
    Vec3 landing;
    BSObject* told[kMaxTold];
    int numTold;
};

Watch s_watches[kMaxWatches];
float s_nextEnemyGrenade;  // the cooldown
float s_reservedUntil;     // a soldier who passed the throw test is winding up

bool Live(const Watch& w) { return w.grenade != NULL && w.lastFrame + 2 >= Frame() && !w.grenade->m_dead; }

Watch* WatchOf(const CThrownBullet* grenade) {
    for (int i = 0; i < kMaxWatches; i++) {
        if (s_watches[i].grenade == grenade && s_watches[i].lastFrame + 2 >= Frame()) {
            return &s_watches[i];
        }
    }
    return NULL;
}

Watch* StartWatch(CThrownBullet* grenade) {
    Watch* w = NULL;
    for (int i = 0; i < kMaxWatches && w == NULL; i++) {
        if (!Live(s_watches[i])) {
            w = &s_watches[i];
        }
    }
    if (w == NULL) {
        return NULL;
    }
    memset(w, 0, sizeof(*w));
    w->grenade = grenade;
    w->born = Now();
    w->nextPredict = Frame();
    State* thrower = FindByNode(grenade->m_pFiredBy);
    w->hostileThrower = thrower != NULL && thrower->hostile;
    if (w->hostileThrower) {
        s_nextEnemyGrenade = Now() + g_profile.grenadeCooldown;
        s_reservedUntil = 0.0f;
        Trace("%p threw a grenade", thrower->ai);
    }
    return w;
}

bool EnemyGrenadeLive() {
    for (int i = 0; i < kMaxWatches; i++) {
        if (Live(s_watches[i]) && s_watches[i].hostileThrower) {
            return true;
        }
    }
    return false;
}

// Where the grenade will come down: its flight stepped as the game steps it,
// until the path hits something.
void Predict(Watch* w) {
    CThrownBullet* g = w->grenade;
    Vec3 pos(g->m_position.x, g->m_position.y, g->m_position.z);
    Vec3 vel(g->m_velocity.x, g->m_velocity.y, g->m_velocity.z);
    if (Length(vel) < 0.05f) {
        w->landing = pos;
        w->haveLanding = true;
        return;
    }
    for (int i = 0; i < kPredictSteps; i++) {
        Vec3 from = pos;
        StepGrenade(&pos, &vel, 3);
        float hit = HitDistance(from, pos);
        if (hit >= 0.0f) {
            w->landing = from + Normalized(pos - from) * hit;
            w->haveLanding = true;
            return;
        }
    }
    w->haveLanding = false;
}

bool Told(const Watch* w, const BSObject* script) {
    for (int i = 0; i < w->numTold; i++) {
        if (w->told[i] == script) {
            return true;
        }
    }
    return false;
}

void Remember(Watch* w, BSObject* script) {
    if (w->numTold < kMaxTold) {
        w->told[w->numTold++] = script;
    }
}

void Warn(Watch* w) {
    CThrownBullet* g = w->grenade;
    CSoldierObject* by = g->m_pFiredBy != NULL ? g->m_pFiredBy->AsSoldierObject() : NULL;
    BSGO_Basic* near[kMaxTold];
    float dist[kMaxTold];
    int count = 0;
    int closest = -1;
    for (BSGO_Basic* go = g_pGermanList; go != NULL && count < kMaxTold; go = go->m_pNext) {
        ISceneNode* node = go->GetSceneNode();
        CSoldierObject* soldier = node->AsSoldierObject();
        if (soldier == NULL || soldier == by || go->m_object == NULL || Told(w, go->m_object)) {
            continue;
        }
        CAIDoodad* doodad = node->GetAIDoodad();
        if (doodad == NULL || doodad->m_pFilter == NULL || !Alive(doodad->m_pFilter->m_pAIObject)) {
            continue;
        }
        Vec3 p = ToVec(doodad->m_pFilter->m_pAIObject->m_position);
        float d = DistanceXY(p, w->landing);
        if (d >= kWarnRadius || fabsf(p.z - w->landing.z) > 3.0f) {
            continue;
        }
        if (closest < 0 || d < dist[closest]) {
            closest = count;
        }
        near[count] = go;
        dist[count++] = d;
    }
    if (count == 0) {
        return;
    }
    GrenadeInfo info;
    info.m_thrower = g->m_pFiredBy != NULL ? g->m_pFiredBy->GetScriptObject() : NULL;
    info.m_grenade = g;
    info.m_numFriends = 0;
    u8 team = near[closest]->GetSceneNode()->GetAIDoodad()->m_pFilter->m_pAIObject->field_0x14;
    for (int i = 0; i < count; i++) {
        if (i == closest) {
            continue;
        }
        BSObjectTriggerEvent(near[i]->m_object, 0xAE, &info, NULL, true);
        if (!AIIsEnemyTeam(team, near[i]->GetSceneNode()->GetAIDoodad()->m_pFilter->m_pAIObject->field_0x14)) {
            info.m_numFriends++;
        }
        Remember(w, near[i]->m_object);
    }
    BSObjectTriggerEvent(near[closest]->m_object, dist[closest] < 3.0f ? 0xAC : 0xAD, &info, NULL, true);
    Remember(w, near[closest]->m_object);
    w->warned = true;
    Trace("grenade warning: %d soldiers near (%.1f, %.1f), closest %.1f, %.2f s after the throw", count,
          w->landing.x, w->landing.y, dist[closest], Now() - w->born);
}

// The original's arc test, but along 8 segments of the arc at `slope`.
bool ArcClear(const Vec3& from, const Vec3& to, float slope, CAIFilterObject* ignore) {
    Vec3 prev = from;
    for (int i = 1; i <= 8; i++) {
        Vec3 next = ArcPoint(from, to, slope, i / 8.0f);
        if (!ClearPath(prev, next, ignore)) {
            return false;
        }
        prev = next;
    }
    return true;
}

}  // namespace

void ResetGrenades() {
    memset(s_watches, 0, sizeof(s_watches));
    s_nextEnemyGrenade = 0.0f;
    s_reservedUntil = 0.0f;
}

bool GrenadeLanding(const ISceneNode* node, Vec3* out) {
    for (int i = 0; i < kMaxWatches; i++) {
        Watch& w = s_watches[i];
        if (Live(w) && w.haveLanding && (const ISceneNode*)w.grenade == node) {
            *out = w.landing;
            return true;
        }
    }
    return false;
}

}  // namespace nai

using namespace nai;

void NativeAIWatchGrenade(CThrownBullet* grenade) {
    if (grenade->m_type != 3 && grenade->m_type != 4) {
        return;
    }
    Watch* w = WatchOf(grenade);
    if (w == NULL && (w = StartWatch(grenade)) == NULL) {
        return;
    }
    w->lastFrame = Frame();
    if (grenade->m_dead) {
        return;
    }
    if (Frame() >= w->nextPredict) {
        Predict(w);
        w->nextPredict = Frame() + 6;
    }
    if (w->haveLanding && Now() - w->born >= g_profile.grenadeWarn) {
        Warn(w);
    }
}

bool NativeAIGrenadeWarned(CThrownBullet* grenade) {
    Watch* w = WatchOf(grenade);
    return w != NULL && w->warned;
}

float NativeAITestGrenadeShot(CAIObject* ai) {
    State* s = Find(ai);
    float now = Now();
    if (s != NULL && s->hostile && (now < s_nextEnemyGrenade || now < s_reservedUntil || EnemyGrenadeLive())) {
        Trace("%p holds its grenade (another is out or too recent)", ai);
        return -1.0f;
    }
    // The original's range and speed for 30 degrees.
    Vec3 me = ToVec(ai->m_position);
    Vec3 aim = ToVec(ai->m_targetPosition);
    float dist = DistanceXY(me, aim) + 3.464f * (aim.z - (0.5f + me.z));
    if (dist < 10.0f) {
        return -1.0f;
    }
    float t = sqrtf(0.95f * dist * 0.0062994817f);
    Vec3 from = Up(me, 0.5f);
    const float slopes[3] = {kTan30, 1.0f, 0.364f};  // 30, 45, 20 degrees
    for (int i = 0; i < 3; i++) {
        float slope = slopes[i];
        if (!ArcPossible(from, aim, slope) || !ArcClear(from, aim, slope, ai->m_pFilter)) {
            continue;
        }
        float speed = t * GrenadeSpeedFactor(slope) / GrenadeSpeedFactor(kTan30);
        if (s != NULL) {
            s->grenadeTan = slope;
            if (s->hostile) {
                s_reservedUntil = now + 2.5f;
            }
        }
        Trace("%p can throw a grenade %.0f away at %d degrees", ai, dist, i == 0 ? 30 : i == 1 ? 45 : 20);
        return 600.0f * (speed + RandomSigned(0.0f, 0.05f * speed * ai->m_inaccuracy));
    }
    return -1.0f;
}

float NativeAIGrenadeLaunchTan(ISceneNode* thrower) {
    State* s = FindByNode(thrower);
    if (s == NULL) {
        return kTan30;
    }
    float slope = s->grenadeTan;
    s->grenadeTan = kTan30;
    return slope;
}
