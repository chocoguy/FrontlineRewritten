// The "Fist Fighters" mod (native/mods.h): enemy soldiers never fire. They
// run at the player and, once in reach, strike at a very fast rate, yelling
// each time. The strikes are Unfair AI's too (FistsUpdate below).
//
// - No fire: an enemy's gun, mounted-gun and grenade throws at its target
//   are all dropped (NativeModHoldFire in mods.cpp).
// - The charge is the soldier scripts' own. SoldierA_Roamer, which 94% of
//   the enemies' scripts are or derive from, has ChargeTarget (logic state
//   59), which runs at the target with the soldier's own animations and
//   pathing. Its Attack state (39) goes there on event 0x7A, which its
//   animation thread sends now and then, but only if the soldier may charge
//   (its instance word 8, set by its behaviour) and hasn't given up (its
//   global 25). And the charge stops at 0.9 times the soldier's attack
//   distance (instance word 21). So for an enemy whose target is the player:
//   - it finds no cover (NativeModNoCover), so it attacks in the open;
//   - word 8 is kept at 1 and word 21 at kChargeTo, so it charges right up
//     to the player;
//   - it's sent 0x7A every kNudge seconds while out of reach (in other
//     states the script has no handler for it, so nothing happens there);
//   - the script's charge roll always charges, and its "can't reach the
//     target: stop charging for 10 s" timers end at once (kSites).
//   Other scripts (machine-gun posts, the bar in 2_2, D-Day's MMGs) aren't
//   touched: they don't charge, but still fight with their fists in reach.
// - The fists: an enemy within kReach of the player strikes every
//   kStrikeInterval seconds. It plays the scripts' own melee strikes (14053
//   and 14052: pathing animations in every level, so the soldier turns toward
//   its move target, set to the player, as it swings). A strike that faces
//   the player lands the scripts' melee hit, 10 damage (CImpact: twice that
//   from behind, the player's hit marker and rumble). While in reach (and
//   briefly after), the script's own animation requests are ignored, except
//   deaths (FistsBlockAnimation). The strikes still send the script its
//   "animation done" event, so its animation thread carries on.
// - The yell: each strike sends the script the event its own melee strike
//   sends (37 with 9, which its sound thread plays as voice 7), and the
//   sound thread's 8 to 15 s wait before that voice can play again is a
//   frame (kSites).
// - Unfair AI (Hard with the setup screen's Unfair AI): its enemies keep
//   their guns and their own ways, but an enemy in reach of the player
//   strikes the same way (and holds its gun fire while it does), without the
//   yell.

#include <math.h>
#include <string.h>
#include <strings.h>

#include "../ai/ai_internal.h"
#include "game/AIObject.h"
#include "game/AISoldierObject.h"
#include "game/BSGO_Basic.h"
#include "game/Soldier_object.h"
#include "game/anim.h"
#include "game/animdb.h"
#include "game/bsfile.h"
#include "game/bsmachin.h"
#include "game/bsobject.h"
#include "game/bstypes.h"
#include "game/impact.h"
#include "game/player.h"
#include "game/scene.h"
#include "game/vector.h"
#include "mods_internal.h"
#include "native/mods.h"

using namespace nmod;

namespace {

// The soldier scripts' melee strikes (their animation states 290 and 397).
const u16 kStrikes[2] = {14053, 14052};
const float kReach = 2.2f;           // to the player, across the ground
const float kReachUp = 1.5f;         // and up or down
const float kStrikeInterval = 0.35f; // seconds between strikes
const float kFacing = 0.7f;          // a strike lands facing the player within about 45 degrees
const float kHold = 0.5f;            // the script's animations stay ignored this long after leaving reach
// The scripts' melee hit: CreateImpact(3, 0, 0, 0, 0, 10, 20).
const float kHitRadius = 3.0f;
const float kHitDamage = 10.0f;
const float kHitB = 20.0f;
const int kMeleeBullet = 0x15;       // EScriptBulletType, as CreateImpact gives
const int kAnimDone = 27;            // the event a finished animation sends (as PlayAnimation asks)
const int kDeathCallback = 3;        // death_cb's place in the callback table (AnimCallback_Init)

// SoldierA_Roamer's instance words and events.
const char* const kRoamer = "SoldierA_Roamer";
const int kMayCharge = 8;            // 1: the soldier's behaviour lets it charge
const int kAttackDistance = 21;      // ChargeTarget charges until within 0.9 times this
const float kChargeTo = 1.5f;
const int kChargeEvent = 0x7A;       // Attack (and state 45): charge, if it may
const float kNudge = 0.5f;           // seconds between those, while out of reach
const int kVoiceEvent = 37;          // its sound thread goes to the state the data names
const int kMeleeVoice = 9;           // that state plays the melee voice (7)

// The soldier scripts' decisions the mod makes for an enemy (checked by
// tools/bsdis.py --check-sites game/native/mods/enemy_fists.cpp).
enum Kind { kChance, kScale, kCount, kTimer };
struct Site {
    const char* cls;
    u8 thread;
    u16 word;
    u8 kind;
    s32 a, b;   // the roll's range, or a timer's delay and event
    s32 value;  // what it gives
    const char* what;
};
const Site kSites[] = {
    {"SoldierA_Roamer", 0, 0x030B, kChance, 1, 100, 1, "charge chance: always (the roll's low end charges)"},
    {"SoldierA_Roamer", 0, 0x220F, kScale, 180, 360, 1, "AttackFromCoverPoint: no wait before charging"},
    {"SoldierA_Roamer", 0, 0x24C3, kTimer, 600, 259, 1, "ChargeTarget: never stops charging"},
    {"SoldierA_Roamer", 0, 0x25C7, kTimer, 600, 259, 1, "ChargeAndAttackTarget: never stops charging"},
    {"SoldierA_Roamer", 2, 0x0182, kScale, 480, 900, 1, "the melee voice: no wait before it plays again"},
};
const int kNumSites = sizeof(kSites) / sizeof(kSites[0]);

// Each enemy in a fist fight, by AI object (objects are reused, so a slot not
// seen for a while is taken over).
struct Brawler {
    const CAIObject* ai;
    float seen;
    float nextStrike;
    float nextNudge;
    float holdUntil;  // the script's animations are ignored until then
    int strike;
};
const int kMaxBrawlers = 64;
Brawler s_brawlers[kMaxBrawlers];

bool Fresh(const Brawler& b, float now) { return b.ai != NULL && b.seen <= now && now - b.seen < 1.0f; }

Brawler* Find(const CAIObject* ai, float now) {
    for (int i = 0; i < kMaxBrawlers; i++) {
        if (s_brawlers[i].ai == ai && Fresh(s_brawlers[i], now)) {
            return &s_brawlers[i];
        }
    }
    return NULL;
}

Brawler* FindOrAdd(const CAIObject* ai, float now) {
    Brawler* b = Find(ai, now);
    if (b == NULL) {
        for (int i = 0; i < kMaxBrawlers && b == NULL; i++) {
            if (!Fresh(s_brawlers[i], now)) {
                b = &s_brawlers[i];
                memset(b, 0, sizeof(*b));
                b->ai = ai;
            }
        }
    }
    if (b != NULL) {
        b->seen = now;
    }
    return b;
}

// Whether the running script is an enemy's.
bool HostileScript() {
    BSObject* obj = g_pBSObject;
    return obj != NULL && obj->m_trigger != NULL && obj->m_trigger->m_data != NULL &&
           obj->m_trigger->m_data->m_team == 2;
}

BSClass_struct* FindClass(const char* name) {
    for (BSClass_struct* c = g_pcClassList; c != NULL; c = c->m_next) {
        if (strcasecmp(name, c->m_name) == 0) {
            return c;
        }
    }
    return NULL;
}

// Whether `c` is SoldierA_Roamer or derives from it (the base's name is in
// the .cbs header), remembered by class.
bool IsRoamer(BSClass_struct* c) {
    struct Known {
        BSClass_struct* cls;
        bool roamer;
    };
    static Known s_known[32];
    static int s_numKnown;
    for (int i = 0; i < s_numKnown; i++) {
        if (s_known[i].cls == c) {
            return s_known[i].roamer;
        }
    }
    bool roamer = false;
    BSClass_struct* at = c;
    for (int depth = 0; at != NULL && depth < 8 && !roamer; depth++) {
        roamer = strcasecmp(at->m_name, kRoamer) == 0;
        const char* base = (const char*)(void*)at->m_cbsFile + 0x24;
        at = base[0] != 0 ? FindClass(base) : NULL;
    }
    if (s_numKnown < 32) {
        s_known[s_numKnown].cls = c;
        s_known[s_numKnown].roamer = roamer;
        s_numKnown++;
    }
    return roamer;
}

// The soldier's script, if it's SoldierA_Roamer's kind.
BSObject* RoamerScript(CSoldierObject* soldier) {
    BSObject* script = soldier->GetScriptObject();
    return script != NULL && script->m_class != NULL && script->m_gameObject != NULL && IsRoamer(script->m_class)
               ? script
               : NULL;
}

// Unfair AI: Rewritten's enemy AI on its "unfair" row.
bool Unfair() { return NativeAIOn() && nai::g_level == 3; }

}  // namespace

bool nmod::MeleeOn() { return On(NATIVE_MOD_FIST_FIGHTERS) || Unfair(); }

bool nmod::InMelee(const CAIObject* ai) {
    Brawler* b = Find(ai, Now());
    return b != NULL && Now() < b->holdUntil;
}

void nmod::FistsUpdate(CAISoldierObject* ai) {
    if (!Enemy(ai) || !Alive(ai)) {
        return;
    }
    bool fists = On(NATIVE_MOD_FIST_FIGHTERS);
    if (!fists && !Unfair()) {
        return;
    }
    CPlayerObject* player = g_scene.GetPlayer(0);
    if (player == NULL || ai->m_pTarget == NULL || ai->m_pTarget != PlayerAI(player)) {
        return;
    }
    CSoldierObject* soldier = SoldierOf(ai);
    if (soldier == NULL) {
        return;
    }
    float now = Now();
    Brawler* b = FindOrAdd(ai, now);
    if (b == NULL) {
        return;
    }
    BSObject* roamer = fists ? RoamerScript(soldier) : NULL;
    if (roamer != NULL) {
        u32* words = (u32*)roamer->m_gameObject->GetScriptData();
        words[kMayCharge] = 1;
        ((f32*)words)[kAttackDistance] = kChargeTo;
    }

    CVector3 me;
    CVector3 you;
    soldier->GetPosition(me);
    player->GetPosition(you);
    float dx = you.x - me.x;
    float dy = you.y - me.y;
    float across = sqrtf(dx * dx + dy * dy);
    if (across > kReach || fabsf(you.z - me.z) > kReachUp) {
        if (roamer != NULL && now >= b->nextNudge) {
            b->nextNudge = now + kNudge;
            BSObjectTriggerEvent(roamer, kChargeEvent, NULL, roamer, false);
        }
        return;
    }
    b->holdUntil = now + kHold;
    // Face the player: the strikes are pathing animations, which turn the
    // soldier toward its move target.
    soldier->SetMoveTarget(you);
    soldier->m_facingTarget = 0;
    if (now < b->nextStrike) {
        return;
    }
    b->nextStrike = now + kStrikeInterval;
    b->strike ^= 1;
    soldier->field_0x2458 = 0;
    soldier->field_0x2459 = 1;
    soldier->SetAnimationState(kStrikes[b->strike], 1.0f, kAnimDone, NULL);
    if (roamer != NULL) {
        BSObjectTriggerEvent(roamer, kVoiceEvent, ADDR_TO_PTR(void, kMeleeVoice), roamer, false);
    }

    CVector3 forward;
    soldier->GetForward(forward);
    float flat = sqrtf(forward.x * forward.x + forward.y * forward.y);
    if (across > 0.01f && flat > 0.0f && (forward.x * dx + forward.y * dy) / (across * flat) < kFacing) {
        return;
    }
    CImpact impact(me, kHitRadius, 0.0f, kHitDamage, kHitB, (EScriptBulletType)kMeleeBullet, soldier);
    impact.m_pClosest = player;
    impact.m_closestDist = across;
    impact.m_hitPos = you;
    impact.m_hitPos.z += 0.75f;
    impact.DamageClosestObject();
}

bool nmod::FistsBlockAnimation(CSoldierObject* soldier, CAIObject* ai, int state) {
    if (!Enemy(ai) || !Alive(ai) || ((CAISoldierObject*)ai)->m_health <= 0.0f || !InMelee(ai)) {
        return false;
    }
    return g_AnimDB.GetStateCallbackID((u16)state, soldier->m_anim.m_hdr.objectType) != kDeathCallback;
}

bool nmod::FistsScriptValue(bool timer, int a, int b, int* value) {
    if (!On(NATIVE_MOD_FIST_FIGHTERS) || !HostileScript()) {
        return false;
    }
    const char* cls;
    int thread;
    int word;
    if (!nai::CallSite(&cls, &thread, &word)) {
        return false;
    }
    for (int i = 0; i < kNumSites; i++) {
        const Site& s = kSites[i];
        if (s.thread == thread && s.word == word && (s.kind == kTimer) == timer && s.a == a && s.b == b &&
            strcasecmp(s.cls, cls) == 0) {
            *value = s.value;
            return true;
        }
    }
    return false;
}
