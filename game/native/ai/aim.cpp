// Rewritten's enhanced enemy AI: sight and aim.
//
// - Sight: the original sets m_targetVisible when a target is picked and
//   clears it only in some script checks, so a soldier's aim followed the
//   player through walls. Each soldier now checks every 0.2 s, and aims where
//   it last saw a hidden target.
// - Aim error: restarts when the target comes back into sight after a
//   while, so the first shots at a player who breaks cover miss and then
//   close in while the player stays exposed.
// - Lead: enemies aim ahead of a moving target by part of its travel during
//   the bullet's flight (the original computes the bullet speed but never
//   uses it).
// - Attack tokens (director.cpp): soldiers without one aim off the player
//   by up to their initial aim error, so only a few shoot accurately at once.
// - Friendly fire: soldiers hold fire while a squadmate is in the way.

#include "ai_internal.h"
#include "game/AISoldierDoodad.h"
#include "game/AISoldierObject.h"
#include "game/Soldier_object.h"
#include "game/vector.h"
#include "game/weapon.h"

namespace nai {
namespace {

const float kSightPeriod = 0.2f;
// Range of the sight refresh: far enough that only walls hide a target the
// script has already picked.
const float kSightRange = 200.0f;

// As CAIObject::CalculateTargetPosition starts it.
void RestartAimError(CAIObject* ai) {
    float k = ai->m_inaccuracy;
    ai->m_aimError.m_pos.field_0x0 = 0.05f * RandomSigned(k, 2.0f * k);
    ai->m_aimError.m_pos.field_0x4 = 0.05f * RandomSigned(k, 2.0f * k);
    ai->m_aimError.m_pos.field_0x8 = 0.05f * RandomSigned(k, 2.0f * k);
    ai->m_aimError.m_vel.field_0x0 = 0.0f;
    ai->m_aimError.m_vel.field_0x4 = 0.0f;
    ai->m_aimError.m_vel.field_0x8 = 0.0f;
}

}  // namespace
}  // namespace nai

using namespace nai;

void NativeAISoldierUpdate(CAISoldierObject* ai) {
    State* s = Find(ai);
    CAIObject* target = ai->m_pTarget;
    if (s == NULL) {
        return;
    }
    if (target == NULL) {
        s->hiddenSince = -1.0f;
        return;
    }
    float now = Now();
    if (now < s->nextSight) {
        return;
    }
    s->nextSight = now + kSightPeriod;
    if (ai->IsTargetInLineOfSight(kSightRange)) {
        if (s->hiddenSince >= 0.0f && s->hostile && g_profile.reacquire > 0.0f &&
            now - s->hiddenSince >= g_profile.reacquire) {
            RestartAimError(ai);
            Trace("%p sees its target again after %.1f s: aim error restarts", ai, now - s->hiddenSince);
        }
        s->hiddenSince = -1.0f;
        ai->m_lastSeenPosition.field_0x0 = target->m_position.field_0x0;
        ai->m_lastSeenPosition.field_0x4 = target->m_position.field_0x4;
        ai->m_lastSeenPosition.field_0x8 = target->m_position.field_0x8;
        ai->m_targetVisible = 1;
        ai->m_targetSeen = 1;
    } else {
        if (s->hiddenSince < 0.0f) {
            s->hiddenSince = now;
            Trace("%p lost sight of its target", ai);
        }
        ai->m_targetVisible = 0;
    }
}

void NativeAIAdjustAim(CAIObject* ai, float dt, float dist) {
    State* s = Find(ai);
    CAIObject* target = ai->m_pTarget;
    if (s == NULL || !s->soldier || !s->hostile || target == NULL) {
        return;
    }
    Vec3 aim = ToVec(ai->m_targetPosition);
    if (g_profile.lead > 0.0f && ai->m_targetVisible && ai->m_bulletSpeed > 0.01f) {
        // m_velocity is per AI update; the bullet's speed per frame.
        float frames = dt * 60.0f;
        if (frames < 1.0f) {
            frames = 1.0f;
        }
        float flight = dist / ai->m_bulletSpeed;
        if (flight > 90.0f) {
            flight = 90.0f;
        }
        aim += ToVec(target->m_velocity) * (g_profile.lead * flight / frames);
    }
    if (g_profile.tokens > 0 && !s->token && target == PlayerAI()) {
        float now = Now();
        if (now >= s->missUntil) {
            s->miss = Normalized(Vec3(RandomRange(-1.0f, 1.0f), RandomRange(-1.0f, 1.0f), 0.5f * RandomRange(-1.0f, 1.0f)));
            s->missScale = RandomRange(0.3f, 1.0f);
            s->missUntil = now + RandomRange(0.5f, 1.0f);
        }
        // The initial aim error averages 0.075 * inaccuracy per axis.
        aim += s->miss * (0.075f * ai->m_inaccuracy * g_profile.missSpread * s->missScale * dist);
    }
    ai->m_targetPosition = ToPos(aim);
}

bool NativeAIHoldFire(CAIObject* ai, const CVector3& from, const CVector3& to) {
    State* s = Find(ai);
    if (s == NULL || !s->soldier) {
        return false;
    }
    // Grenades go over.
    CSoldierObject* soldier = ((CAISoldierDoodad*)ai->m_pFilter->m_pDoodad)->m_pSoldier;
    CWeapon* weapon = soldier->m_curWeapon >= 0 ? soldier->m_pWeapons[soldier->m_curWeapon] : NULL;
    if (weapon == NULL || !weapon->m_pProperties->m_isProjectile) {
        return false;
    }
    Vec3 a(from.x, from.y, from.z);
    Vec3 b(to.x, to.y, to.z);
    float len = Length(b - a);
    if (len < 1.0f) {
        return false;
    }
    for (int i = 0; i < NumStates(); i++) {
        CAIObject* other = StateAt(i)->ai;
        if (other == ai || other == ai->m_pTarget || other->field_0x14 != ai->field_0x14 || !Alive(other)) {
            continue;
        }
        float along;
        float d = SegmentDistance(a, b, Up(ToVec(other->m_position), 1.0f), &along);
        if (d < 0.6f && along > 0.02f && along * len < len - 0.75f) {
            Trace("%p holds fire: %p is in the way", ai, other);
            return true;
        }
    }
    return false;
}

// A dead target, or one out of sight for 3 s, no longer gets the original's
// preference over a closer one that can be seen.
bool NativeAIKeepTarget(CAIObject* ai, CAIObject* target) {
    if (!Alive(target)) {
        return false;
    }
    State* s = Find(ai);
    return s == NULL || s->hiddenSince < 0.0f || Now() - s->hiddenSince < 3.0f;
}
