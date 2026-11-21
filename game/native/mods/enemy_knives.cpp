// The "Knife Throwers" mod (native/mods.h): enemy soldiers throw knives at
// the player instead of shooting, rapidly and from any distance.
//
// - No bullets: an enemy's gun and mounted-gun shots at its target are
//   dropped (NativeModHoldFire in mods.cpp, from CAISoldierDoodad). Its grenades still
//   fly, and its script still aims and plays its firing animations.
// - Knives: every living enemy whose target is the player throws a knife at
//   the player's chest every kInterval seconds while it has a clear line of
//   sight, whatever its script is doing (KnivesUpdate, from
//   NativeModSoldierUpdate). The knife
//   is the level's own (thrown bullet type 5: the KNIFETHROWN model every
//   level has, and its damage, 15 in every level and difficulty), thrown
//   from the soldier's gun muzzle.
// - Unlimited range: a knife flies straight (type 5 has no gravity) and
//   lives as long as it takes to reach the player and half a second more,
//   rather than the level's 4 seconds. It gets there from anywhere, and a
//   miss (a knife stops where it lands) is soon gone: the 64 thrown bullets
//   are shared with the grenades.

#include "game/AIFilter.h"
#include "game/AIObject.h"
#include "game/AISoldierDoodad.h"
#include "game/AISoldierObject.h"
#include "game/AIStatus.h"
#include "game/Soldier_object.h"
#include "game/bulletfactory.h"
#include "game/mainloop.h"
#include "game/matrix.h"
#include "game/player.h"
#include "game/scene.h"
#include "game/thrownbullet.h"
#include "game/vector.h"
#include "mods_internal.h"
#include "native/mods.h"

using namespace nmod;

namespace {

const int kKnife = 5;              // EThrownBulletTypes
const float kSpeed = 0.75f;        // per frame (45 a second; the level's knife throws at most 0.8)
const float kInterval = 0.5f;      // seconds between one soldier's knives
const float kJitter = 0.2f;        // up to this much later, so a squad doesn't throw as one
const float kBlockedRetry = 0.2f;  // seconds before looking again when the player is out of sight
const float kSpread = 0.015f;      // radians off the line to the player, at most
const float kMarginFrames = 30.0f; // a knife's life past the player
const float kChestDrop = 0.4f;     // below the player's eye
const int kMaxKnives = 32;         // in flight at once (of the 64 thrown bullets)
// The collision class of a soldier's thrown rounds (CWeapon::Init).
const int kEnemyThrown = 10;
// Full power (Init's strength 1: the speed as given).
const float kFullPower = 512.0f;

// Each soldier's next throw, by AI object (objects are reused, so a slot
// not seen for a while is taken over).
struct Thrower {
    const CAIObject* ai;
    float next;
    float seen;
};
const int kMaxThrowers = 64;
Thrower s_throwers[kMaxThrowers];

// When each knife in flight is gone, in the AI's seconds.
float s_knifeGone[kMaxKnives];

Thrower* ThrowerFor(const CAIObject* ai, float now) {
    Thrower* stale = NULL;
    for (int i = 0; i < kMaxThrowers; i++) {
        Thrower& t = s_throwers[i];
        if (t.ai == ai && t.seen <= now && now - t.seen < 1.0f) {
            t.seen = now;
            return &t;
        }
        if (stale == NULL && (t.ai == NULL || t.seen > now || now - t.seen >= 1.0f)) {
            stale = &t;
        }
    }
    if (stale != NULL) {
        // A newcomer waits up to one interval, so a squad that spots the
        // player together doesn't throw as one.
        stale->ai = ai;
        stale->seen = now;
        stale->next = now + kInterval * nmod::Random();
    }
    return stale;
}

// A free place among the knives in flight, or -1.
int FreeKnife(float now) {
    for (int i = 0; i < kMaxKnives; i++) {
        if (s_knifeGone[i] <= now || s_knifeGone[i] > now + 600.0f) {
            return i;
        }
    }
    return -1;
}

CAIFilterRealPosition Position(const CVector3& v) {
    CAIFilterRealPosition p;
    p.field_0x0 = v.x;
    p.field_0x4 = v.y;
    p.field_0x8 = v.z;
    return p;
}

}  // namespace

void nmod::KnivesUpdate(CAISoldierObject* ai) {
    if (!Enemy(ai) || !On(NATIVE_MOD_ENEMY_KNIVES) || !Alive(ai)) {
        return;
    }
    CPlayerObject* player = g_scene.GetPlayer(0);
    if (player == NULL || ai->m_pTarget == NULL || ai->m_pTarget != PlayerAI(player) || ai->m_pFilter == NULL) {
        return;
    }
    float now = Now();
    Thrower* thrower = ThrowerFor(ai, now);
    if (thrower == NULL || now < thrower->next) {
        return;
    }
    int slot = FreeKnife(now);
    if (slot < 0) {
        return;
    }
    CSoldierObject* soldier = SoldierOf(ai);
    if (soldier == NULL) {
        return;
    }

    CMatrix emitter;
    soldier->GetBulletEmitterMatrix(emitter);
    CVector3 from(emitter.m[3][0], emitter.m[3][1], emitter.m[3][2]);
    CVector3 to;
    player->m_camera.GetPosition(to);
    to.z -= kChestDrop;
    if (CAIFilterGlobal::CheckVisionLineToEnvironment(Position(from), Position(to), ai->m_pFilter)) {
        thrower->next = now + kBlockedRetry;
        return;
    }
    thrower->next = now + kInterval + kJitter * Random();

    CVector3 dir(to.x - from.x, to.y - from.y, to.z - from.z);
    float distance = dir.Length();
    if (!(distance > 0.0f)) {
        return;
    }
    float spread = kSpread * distance;
    dir.x += spread * (2.0f * Random() - 1.0f);
    dir.y += spread * (2.0f * Random() - 1.0f);
    dir.z += spread * (2.0f * Random() - 1.0f);
    CBullet* bullet = g_pBulletFactory->CreateBullet(kKnife, false, from, dir, kSpeed, (EClsnId)kEnemyThrown,
                                                     kFullPower, soldier);
    if (bullet == NULL || bullet->AsThrown() == NULL) {
        return;
    }
    float frames = distance / kSpeed + kMarginFrames;
    bullet->AsThrown()->field_0x90 = frames;  // its life (Init set the level's)
    s_knifeGone[slot] = now + frames / 60.0f;
}
