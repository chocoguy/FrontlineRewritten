// Rewritten's enhanced enemy AI: what its parts share. The hooks the game
// calls are in the decomp's native/ai.h; docs/port/phase2-status.md ("Enemy
// AI") describes the design.
//
// Parts: state.cpp (profiles, the per-object table, randomness, tracing),
// aim.cpp (aim and sight), cover.cpp (cover and running away), grenades.cpp,
// director.cpp (callouts, attack tokens, flanking roles) and script_bias.cpp
// (the soldier scripts' own rolls and timers).
#ifndef FRONTLINE_GAME_NATIVE_AI_INTERNAL_H
#define FRONTLINE_GAME_NATIVE_AI_INTERNAL_H

#include "ai_math.h"
#include "game/AIFilter.h"
#include "game/AIObject.h"
#include "native/ai.h"

class CPlayerObject;

namespace nai {

// One difficulty's settings. Easy and Medium keep the aggression ones at the
// original game's values; Hard raises them. FRONTLINE_AI_TUNE overrides them
// by name (kTuneNames in state.cpp).
struct Profile {
    const char* name;
    // Aim (enemies of the player only)
    float inaccuracy;      // scales each soldier's m_inaccuracy (1: original)
    float aimTimeCap;      // caps m_aimTime, the aim's settling time (0: original)
    float lead;            // how much of a moving target's travel during the bullet's flight is led
    float reacquire;       // after this long out of sight (s), the aim error restarts (0: never)
    // Perception
    float reaction;        // scales the reaction delays of target searches and heard sounds
    float proximity;       // radius of the "the player is right there" check (5 originally)
    int scheduleBatch;     // perception checks polled at once (5 originally)
    // The squad
    int tokens;            // soldiers allowed to aim accurately at the player at once
    float tokenHold;       // s before a token passes to another soldier
    float missSpread;      // the others' extra aim error, times the initial aim error
    float calloutRadius;   // squadmates within this of a soldier who sees the player are told
    float calloutDelay;    // s before they hear it
    int flankers;          // soldiers sent round the side in a fight of 3 or more
    // Cover
    float advance;         // weight of cover that gains ground on the threat
    int forwardCover;      // cover type 0 ("forward, at least halfway") can match
    // Grenades
    float grenadeCooldown; // s between enemy grenades
    float grenadeWarn;     // s after a throw before soldiers near where it lands react
    // The soldier scripts' own decisions (script_bias.cpp): 0 leaves them alone
    int scriptBias;
};

extern Profile g_profile;
extern int g_level;  // the profile's row: 0 easy, 1 medium, 2 hard
extern bool g_trace;

// What the AI keeps per AI object (soldiers, tanks, the player).
enum Role {
    ROLE_NONE,
    ROLE_SUPPRESS,  // holds cover that sees the player
    ROLE_FLANK,     // takes cover round the player's side
};

struct State {
    CAIObject* ai;
    bool soldier;  // infantry
    bool hostile;  // fights the player (team 2)
    bool squad;    // a Soldier* script: hears callouts
    bool roamer;   // a *_Roamer script: free to take flanking cover
    float nextSight;     // next refresh of m_targetVisible
    float hiddenSince;   // when the target went out of sight (-1: in sight)
    bool token;          // aims accurately
    float tokenSince;
    float tokenRestUntil;
    Vec3 miss;           // the extra aim error's direction, without a token
    float missScale;
    float missUntil;
    int role;
    float roleSince;
    float calloutAt;     // when a pending callout is heard (0: none)
    float quietUntil;    // no new callout before
    float exposedSince;  // at cover, seen by the target since (-1: hidden)
    float grenadeTan;    // launch slope of the soldier's next grenade
    float nearMissUntil;
};

State* Find(const CAIObject* ai);
State* FindByNode(const ISceneNode* node);
int NumStates();
State* StateAt(int i);

// The AI clock (seconds) and frame.
float Now();
u32 Frame();

// Randomness of its own (seeded per level), so the game's own sequence is
// what the original draws.
float Random();                              // [0, 1)
float RandomRange(float lo, float hi);       // [lo, hi)
float RandomSigned(float lo, float hi);      // +-[lo, hi)

// The player (single player) and its AI object, or NULL.
CPlayerObject* Player();
CAIObject* PlayerAI();
Vec3 PlayerEye();
Vec3 PlayerLook();  // unit

// Whether the line between two points is clear for seeing (counts the test).
bool ClearSight(const Vec3& from, const Vec3& to, CAIFilterObject* ignore);
bool ClearPath(const Vec3& from, const Vec3& to, CAIFilterObject* ignore);
float HitDistance(const Vec3& from, const Vec3& to);  // to the first hit, or -1

bool Alive(const CAIObject* ai);
BSObject* ScriptOf(const CAIObject* ai);

// Where `target` can be seen from: the player's camera, a soldier's eye.
Vec3 EyeOf(const CAIObject* target);

// Prints a line with the AI time when FRONTLINE_AI_TRACE is set.
void Trace(const char* format, ...);
void CountLineTest();

// Per-part resets for a new level or a restart.
void ResetCover();
void ResetGrenades();
void ResetDirector();
void ResetScriptBias();

// script_bias.cpp: where the running built-in was called from: the class
// whose code it is, the thread and the word in that class's own code.
bool CallSite(const char** cls, int* thread, int* word);

// grenades.cpp: where a grenade near `node` will land, if it is one.
bool GrenadeLanding(const ISceneNode* node, Vec3* out);

// director.cpp: the middle of the squad fighting the player (false if none);
// whether the player is reloading or hurt (flankers press harder).
bool SquadCenter(Vec3* out);
bool PlayerPressed();

inline Vec3 ToVec(const CAIFilterRealPosition& p) { return Vec3(p.field_0x0, p.field_0x4, p.field_0x8); }
inline Vec3 ToVec(const CAIFilterRealVector3& p) { return Vec3(p.field_0x0, p.field_0x4, p.field_0x8); }
inline CAIFilterRealPosition ToPos(const Vec3& v) {
    CAIFilterRealPosition p;
    p.field_0x0 = v.x;
    p.field_0x4 = v.y;
    p.field_0x8 = v.z;
    return p;
}

}  // namespace nai

#endif
