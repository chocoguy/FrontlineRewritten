// Rewritten's mods (native/mods.h): what the mods share.
#pragma once

#include "types.h"

class CAIObject;
class CAISoldierObject;
class CAnimObject;
class CPlayerObject;
class CSoldierObject;

namespace nmod {

// Whether `mod` (NATIVE_MOD_*) is on for this level: the app's choice as the
// level booted, in single player.
bool On(int mod);

// The AI's side of things.
bool Enemy(const CAIObject* ai);  // fights the player (team 2)
bool Alive(const CAIObject* ai);
CAIObject* PlayerAI(CPlayerObject* player);
CSoldierObject* SoldierOf(const CAIObject* ai);  // a soldier AI's soldier, or null
CAIObject* AIOf(CSoldierObject* soldier);         // and back
float Now();                                      // the AI clock, in seconds

// Randomness of the mods' own, so the game's sequence stays as it was.
float Random();  // [0, 1)

// The mods, called from native/mods.h's hooks (mods.cpp).
void KnivesUpdate(CAISoldierObject* ai);  // enemy_knives.cpp
void FistsUpdate(CAISoldierObject* ai);   // enemy_fists.cpp
bool MeleeOn();                       // Fist Fighters, or Unfair AI's strikes
bool InMelee(const CAIObject* ai);    // striking the player now
bool FistsBlockAnimation(CSoldierObject* soldier, CAIObject* ai, int state);
bool FistsScriptValue(bool timer, int a, int b, int* value);
bool FlyUpdate(CPlayerObject* player, float dt, bool grounded, bool jump);  // flying.cpp

}  // namespace nmod
