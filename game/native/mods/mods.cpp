// Rewritten's mods (native/mods.h): which are on for this level, what the
// mods share, and the hooks, passed on to the mods that use them.

#include "game/AIDoodad.h"
#include "game/AIFilter.h"
#include "game/AIObject.h"
#include "game/AISoldierDoodad.h"
#include "game/AISoldierObject.h"
#include "game/AIStatus.h"
#include "game/ShellMenu.h"
#include "game/Soldier_object.h"
#include "game/player.h"
#include "mods_internal.h"
#include "native/mods.h"

namespace {

// The app's choice, taken the first time the game asks after booting. The
// game's statics start over at each reboot (platform/src/os/reboot.cpp), and
// every level is a new boot, so a level keeps the mods it started with.
bool s_taken;
unsigned int s_mods;

u32 s_random = 0x6B1FE37Du;

}  // namespace

bool nmod::On(int mod) {
    if (!s_taken) {
        s_taken = true;
        s_mods = NativeMods();
    }
    return !g_bInMultiplayerMode && (s_mods >> mod & 1) != 0;
}

bool nmod::Enemy(const CAIObject* ai) { return ai->field_0x14 == 2; }

bool nmod::Alive(const CAIObject* ai) {
    return ai->m_status.m_vars[AISTATUS_HEALTH] > 0 && ai->m_status.m_vars[AISTATUS_DEAD] == 0;
}

CAIObject* nmod::PlayerAI(CPlayerObject* player) {
    CAIDoodad* doodad = player->GetAIDoodad();
    return doodad != NULL && doodad->m_pFilter != NULL ? doodad->m_pFilter->m_pAIObject : NULL;
}

CSoldierObject* nmod::SoldierOf(const CAIObject* ai) {
    return ai->m_pFilter != NULL && ai->m_pFilter->m_pDoodad != NULL
               ? ((CAISoldierDoodad*)ai->m_pFilter->m_pDoodad)->m_pSoldier
               : NULL;
}

CAIObject* nmod::AIOf(CSoldierObject* soldier) {
    CAIDoodad* doodad = soldier->GetAIDoodad();
    return doodad != NULL && doodad->m_pFilter != NULL ? doodad->m_pFilter->m_pAIObject : NULL;
}

float nmod::Now() { return g_aigAIFilterGlobalObject.m_time; }

// xorshift32
float nmod::Random() {
    u32 x = s_random;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_random = x;
    return (x >> 8) * (1.0f / 16777216.0f);
}

bool NativeModHoldFire(CAIObject* ai, bool bullets) {
    if (ai == NULL || !nmod::Enemy(ai)) {
        return false;
    }
    // Fist Fighters hold their grenades too; Knife Throwers only their guns;
    // Unfair AI's enemies their guns while they strike.
    return nmod::On(NATIVE_MOD_FIST_FIGHTERS) ||
           (bullets && (nmod::On(NATIVE_MOD_ENEMY_KNIVES) || (nmod::MeleeOn() && nmod::InMelee(ai))));
}

void NativeModSoldierUpdate(CAISoldierObject* ai) {
    nmod::KnivesUpdate(ai);
    nmod::FistsUpdate(ai);
}

bool NativeModBlockAnimation(CAnimObject* obj, int state) {
    if (!nmod::MeleeOn()) {
        return false;
    }
    CSoldierObject* soldier = obj->AsSoldierObject();
    CAIObject* ai = soldier != NULL ? nmod::AIOf(soldier) : NULL;
    return ai != NULL && nmod::FistsBlockAnimation(soldier, ai, state);
}

int NativeModScriptRandom(int lo, int hi, int value) {
    nmod::FistsScriptValue(false, lo, hi, &value);
    return value;
}

int NativeModScriptTimer(int delay, int event) {
    nmod::FistsScriptValue(true, delay, event, &delay);
    return delay;
}

bool NativeModNoCover(CAIObject* ai) {
    return ai != NULL && nmod::Enemy(ai) && nmod::On(NATIVE_MOD_FIST_FIGHTERS);
}

bool NativeModIgnores(const CAIObject* ai, const CAIObject* other) {
    // Enemies Ignore You: the enemy side never targets or hears the player.
    return ai != NULL && other != NULL && other->IsPlayer() && nmod::Enemy(ai) &&
           nmod::On(NATIVE_MOD_IGNORE_PLAYER);
}

bool NativeModPlayerFly(CPlayerObject* player, float dt, bool grounded, bool jump) {
    return nmod::FlyUpdate(player, dt, grounded, jump);
}
