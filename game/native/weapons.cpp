// Rewritten's player guns (native/weapons.h): faster fire, semi-automatics
// that fire a shot a trigger pull, and the Full Auto Springfield.
//
// A shot's path: the fire button sends the Player script event 0x10 each
// frame it's held (CPlayerObject's firing). The script plays the firing
// animation, whose event 0x7D calls PlayerFireMyWeapon, which sets the
// weapon shooting (WEAPON_SHOOT_HOLD). CWeapon::Update fires whenever the
// refire wait is over, and sends 0x15, on which the script fires again if
// the button is still held. Releasing it sends 0x94 and stops the weapon.
//
// A semi-automatic's pull is cut short here: once the shot is fired, the
// held button reads as released (0x94, the weapon stopped) until it really
// is. The refire wait is all but gone, and a new pull during the firing
// animation (about half a second) gets the 0x15 that fires again, so pulls
// fire as fast as they come.

#include "game/ISceneNode.h"
#include "game/ShellMenu.h"
#include "game/bsobject.h"
#include "game/bstypes.h"
#include "game/player.h"
#include "game/player_weapon_object.h"
#include "game/weapon.h"
#include "native/hooks.h"
#include "native/weapons.h"

namespace {

// The player's guns, by weapon type (EWeaponTypes, as the pickup scripts
// give them; the same table in every level and difficulty).
enum {
    kColt45 = 6,
    kWaltherP38 = 7,
    kSilencedPistol = 8,
    kM1Garand = 14,
    kSpringfield = 15,
    kGewehr43 = 16,
    kThompson = 20,
    kMP40 = 21,
    kStG44 = 24,
    kBAR = 25,
    kShotgun = 29,
    kBazooka = 30,
    kPanzerschreck = 32,
};

// Frames between shots (at 60 a second), and the original's.
struct Rate {
    int type;
    int frames;
    bool semi;  // a shot a trigger pull
};
const Rate kRates[] = {
    {kColt45, 2, true},          // 20
    {kWaltherP38, 2, true},      // 20
    {kSilencedPistol, 2, true},  // 40
    {kM1Garand, 2, true},        // 30
    {kGewehr43, 2, true},        // 40
    {kSpringfield, 60, false},   // 90, a bolt action
    {kThompson, 3, false},       // 4
    {kMP40, 4, false},           // 5
    {kStG44, 6, false},          // 8
    {kBAR, 7, false},            // 9
    {kShotgun, 40, false},       // 55, a pump action
    {kBazooka, 140, false},      // 200
    {kPanzerschreck, 140, false},  // 200
};

// The Player script's state while a shot's firing animation plays (8, the
// same in every level; its subclasses run Player's code). It fires again on
// 0x15 if the button is held then, and otherwise waits out the animation.
const int kFiringState = 8;

// The Full Auto Springfield: about the BAR's rate.
const int kFullAutoSpringfield = 7;

// Rewritten's rate for the player's `weapon`, or null for its own.
const Rate* RateOf(const CWeapon* weapon) {
    if (NativeGameMode() != NATIVE_MODE_REWRITTEN || g_bInMultiplayerMode || weapon == NULL ||
        weapon->m_pHolder == NULL || weapon->m_pHolder->AsPlayerObject() == NULL) {
        return NULL;
    }
    for (const Rate& r : kRates) {
        if (r.type == weapon->m_weaponType) {
            return &r;
        }
    }
    return NULL;
}

bool SemiAuto(const CWeapon* weapon) {
    const Rate* r = RateOf(weapon);
    return r != NULL && r->semi;
}

const CWeapon* HeldWeapon(const CPlayerObject* player) {
    if (player->m_onMG) {
        return player->m_pMGWeapon;
    }
    if (player->m_curWeapon >= 0) {
        return player->m_weapons[player->m_curWeapon]->m_pWeapon;
    }
    return NULL;
}

// A player's trigger: held since the last frame it was up, whether this pull
// has fired, and, for a pull let go before its shot (a quick click), how
// many frames it has been kept on since.
struct Trigger {
    const CPlayerObject* player;
    bool held;
    bool spent;
    float pull;
    int kept;
};

// How long a quick click is kept on for its shot: the shot comes the frame
// after the press, or later if the gun is still cycling.
const int kKeepPull = 12;
Trigger s_triggers[4];

Trigger* TriggerOf(const CPlayerObject* player) {
    Trigger* free = NULL;
    for (Trigger& t : s_triggers) {
        if (t.player == player) {
            return &t;
        }
        if (t.player == NULL && free == NULL) {
            free = &t;
        }
    }
    if (free == NULL) {
        free = &s_triggers[0];
    }
    *free = Trigger{player, false, false, 0.0f, 0};
    return free;
}

}  // namespace

int NativeRefireRate(const CWeapon* weapon) {
    const Rate* r = RateOf(weapon);
    if (r == NULL) {
        return weapon->m_pProperties->m_refireRate;
    }
    if (r->type == kSpringfield && NativeFullAutoSpringfield()) {
        return kFullAutoSpringfield;
    }
    return r->frames;
}

void NativeWeaponFired(const CWeapon* weapon) {
    if (SemiAuto(weapon)) {
        TriggerOf(weapon->m_pHolder->AsPlayerObject())->spent = true;
    }
}

float NativeTriggerInput(const CPlayerObject* player, float fire) {
    if (NativeGameMode() != NATIVE_MODE_REWRITTEN) {
        return fire;
    }
    Trigger* t = TriggerOf(player);
    bool semi = SemiAuto(HeldWeapon(player));
    if (fire != 0.0f && !t->held) {
        t->spent = false;
        t->pull = fire;
        t->kept = 0;
        // A pull during the last shot's firing animation fires now, as the
        // script would on 0x15 with the button held through it.
        BSObject* script = player->m_pScriptObject;
        if (semi && script != NULL && script->m_threads[0].m_state == kFiringState) {
            BSObjectTriggerEvent(script, 0x10, ADDR_TO_PTR(void, *(u32*)&fire), NULL, true);
            BSObjectTriggerEvent(script, 0x15, NULL, NULL, true);
        }
    }
    t->held = fire != 0.0f;
    if (!semi || fire != 0.0f) {
        return semi && t->spent ? 0.0f : fire;
    }
    // Let go: a pull that hasn't fired yet stays on until it does.
    if (!t->spent && t->pull != 0.0f && t->kept < kKeepPull) {
        t->kept++;
        return t->pull;
    }
    t->pull = 0.0f;
    return 0.0f;
}
