// Rewritten's enhanced enemy AI: the soldier scripts' own decisions.
//
// The enemies' choices are rolls and timers in SoldierA_Roamer.cbs (the same
// file in every level; its subclasses run its code for the states they
// don't override). tools/bsdis.py found the ones below; each is a call of
// GetRandomInteger or RegisterTimerEvent at a word of the class's own code
// in one of its threads (0 Logic, 1 Anim). For an enemy of the player, the
// profile's column adjusts the result:
//   kChance  a roll compared "below a threshold" (the outcome): the roll is
//            pulled toward its low end by the factor (below 1: likelier)
//   kScale   a duration in frames, times the factor
//   kCount   a loop count (animation cycles), times the factor, at least 1
//   kTimer   a RegisterTimerEvent delay, times the factor
// A site only applies when the call's arguments are the ones listed, so a
// different script version is left alone.

#include <string.h>
#include <strings.h>

#include "ai_internal.h"
#include "game/AIDoodad.h"
#include "game/BSGO_Basic.h"
#include "game/ISceneNode.h"
#include "game/bsfile.h"
#include "game/bsmachin.h"
#include "game/bstypes.h"

namespace nai {
namespace {

enum Kind { kChance, kScale, kCount, kTimer };

struct Site {
    const char* cls;
    u8 thread;
    u16 word;
    u8 kind;
    s32 a, b;          // the roll's range, or a timer's delay and event
    float factor[3];   // easy, medium, hard
    const char* what;
};

const Site kSites[] = {
    // Logic thread
    {"SoldierA_Roamer", 0, 0x030B, kChance, 1, 100, {1.0f, 0.85f, 0.7f},
     "charge chance (30/50/80% by aggression; entering an attack, or from cover with the target out of sight)"},
    {"SoldierA_Roamer", 0, 0x0328, kChance, 0, 99, {1.25f, 1.5f, 1.75f},
     "stays put when a grenade lands by a squadmate (10/25/40%; otherwise runs)"},
    {"SoldierA_Roamer", 0, 0x220F, kScale, 180, 360, {1.0f, 0.8f, 0.6f},
     "AttackFromCoverPoint: target out of sight this long before charging or searching"},
    {"SoldierA_Roamer", 0, 0x238A, kTimer, 900, 256, {1.0f, 0.8f, 0.6f},
     "AdvanceToCoverPoint: stops using cover for 15 s after one it can't reach"},
    {"SoldierA_Roamer", 0, 0x24C3, kTimer, 600, 259, {1.0f, 0.75f, 0.5f},
     "ChargeTarget: stops charging for 10 s after failing to reach the target"},
    {"SoldierA_Roamer", 0, 0x25C7, kTimer, 600, 259, {1.0f, 0.75f, 0.5f},
     "ChargeAndAttackTarget: stops charging for 10 s after failing to reach the target"},
    // Anim thread: firing from cover, cycles of an aiming animation (flag 16)
    {"SoldierA_Roamer", 1, 0x1227, kCount, 2, 5, {1.0f, 1.25f, 1.5f}, "cover fire burst (state 102)"},
    {"SoldierA_Roamer", 1, 0x1305, kCount, 1, 4, {1.0f, 1.25f, 1.5f}, "cover fire burst (state 112)"},
    {"SoldierA_Roamer", 1, 0x14CD, kCount, 2, 5, {1.0f, 1.25f, 1.5f}, "cover fire burst (state 131)"},
    {"SoldierA_Roamer", 1, 0x1593, kCount, 1, 4, {1.0f, 1.25f, 1.5f}, "cover fire burst (state 139)"},
    {"SoldierA_Roamer", 1, 0x177B, kCount, 1, 4, {1.0f, 1.25f, 1.5f}, "cover fire burst (state 156)"},
    {"SoldierA_Roamer", 1, 0x17E0, kCount, 1, 4, {1.0f, 1.25f, 1.5f}, "cover fire burst (state 159)"},
    {"SoldierA_Roamer", 1, 0x183B, kCount, 2, 4, {1.0f, 1.25f, 1.5f}, "cover fire burst (state 162)"},
    {"SoldierA_Roamer", 1, 0x18A0, kCount, 1, 4, {1.0f, 1.25f, 1.5f}, "cover fire burst (state 165)"},
    {"SoldierA_Roamer", 1, 0x1968, kCount, 2, 4, {1.0f, 1.25f, 1.5f}, "cover fire burst (state 172)"},
    // Anim thread: waiting behind cover, cycles of the crouched idle (14453)
    {"SoldierA_Roamer", 1, 0x19C0, kCount, 2, 5, {1.0f, 0.8f, 0.6f}, "hiding behind cover (state 175)"},
    {"SoldierA_Roamer", 1, 0x19F1, kCount, 4, 8, {1.0f, 0.8f, 0.6f}, "hiding behind cover (state 176)"},
    {"SoldierA_Roamer", 1, 0x1D83, kCount, 3, 6, {1.0f, 0.8f, 0.6f}, "hiding behind cover (state 207)"},
    {"SoldierA_Roamer", 1, 0x1DB4, kCount, 4, 10, {1.0f, 0.8f, 0.6f}, "hiding behind cover (state 208)"},
    {"SoldierA_Roamer", 1, 0x1ED6, kCount, 4, 8, {1.0f, 0.8f, 0.6f}, "hiding behind cover (state 223)"},
};
const int kNumSites = sizeof(kSites) / sizeof(kSites[0]);

// Call sites already resolved, by instruction address (the code stays put
// for the level).
struct Cached {
    const u32* ip;
    BSClass_struct* owner;
    s16 thread;
    s32 word;
};
const int kCacheSize = 256;
Cached s_cache[kCacheSize];

BSClass_struct* FindClass(const char* name) {
    for (BSClass_struct* c = g_pcClassList; c != NULL; c = c->m_next) {
        if (strcasecmp(name, c->m_name) == 0) {
            return c;
        }
    }
    return NULL;
}

// The base class a class inherits code from (the name in its .cbs header).
BSClass_struct* BaseOf(BSClass_struct* c) {
    const char* name = (const char*)(void*)c->m_cbsFile + 0x24;
    return name[0] != 0 ? FindClass(name) : NULL;
}

// The running thread's code segment holding the instruction, the class that
// segment belongs to (along the base classes), and the word within it.
bool Resolve(const u32* ip, Cached* out) {
    BSObject* obj = g_pBSObject;
    BSMachineThread_struct* thread = g_pbmtThread;
    if (ip == NULL || obj == NULL || thread == NULL || obj->m_class == NULL || thread->m_code == NULL) {
        return false;
    }
    BSCode_struct* code = thread->m_code;
    BSCode_struct* threads = obj->m_class->m_threads;
    int t = (int)(code - threads);
    if (t < 0 || t >= obj->m_class->m_numThreads) {
        return false;
    }
    int segment = -1;
    const u32* base = NULL;
    for (int s = 0; s <= code->m_numInherited; s++) {
        const u32* start = ADDR_TO_PTR(u32, code->m_code[s]);
        if (start != NULL && start <= ip && (base == NULL || start > base)) {
            base = start;
            segment = s;
        }
    }
    if (segment < 0) {
        return false;
    }
    BSClass_struct* owner = obj->m_class;
    while (owner != NULL && owner->m_threads[t].m_numInherited != segment) {
        owner = BaseOf(owner);
    }
    if (owner == NULL) {
        return false;
    }
    out->owner = owner;
    out->thread = (s16)t;
    out->word = (s32)(ip - base);
    return true;
}

// Which kSites entry the running built-in was called from, or -1.
int SiteHere() {
    const char* cls;
    int thread;
    int word;
    if (!CallSite(&cls, &thread, &word)) {
        return -1;
    }
    for (int i = 0; i < kNumSites; i++) {
        const Site& s = kSites[i];
        if (s.thread == thread && s.word == word && strcasecmp(s.cls, cls) == 0) {
            return i;
        }
    }
    return -1;
}

// The running script's soldier fights the player.
bool HostileScript() {
    BSObject* obj = g_pBSObject;
    if (obj == NULL || obj->m_gameObject == NULL) {
        return false;
    }
    State* s = FindByNode(obj->m_gameObject->GetSceneNode());
    return s != NULL && s->soldier && s->hostile;
}

int Scaled(int value, float factor) {
    int v = (int)(value * factor + 0.5f);
    return v < 1 && value >= 1 ? 1 : v;
}

}  // namespace

bool CallSite(const char** cls, int* thread, int* word) {
    const u32* ip = NativeBSInstruction();
    Cached& c = s_cache[((uptr)ip >> 2) % kCacheSize];
    if (c.ip != ip) {
        c.ip = ip;
        if (!Resolve(ip, &c)) {
            c.owner = NULL;
        }
    }
    if (c.owner == NULL) {
        return false;
    }
    *cls = c.owner->m_name;
    *thread = c.thread;
    *word = c.word;
    return true;
}

void ResetScriptBias() { memset(s_cache, 0, sizeof(s_cache)); }

}  // namespace nai

using namespace nai;

int NativeAIScriptRandom(int lo, int hi, int value) {
    if (!g_profile.scriptBias) {
        return value;
    }
    int i = SiteHere();
    if (i < 0) {
        return value;
    }
    const Site& s = kSites[i];
    float factor = s.factor[g_level];
    if (s.kind == kTimer || s.a != lo || s.b != hi || factor == 1.0f || !HostileScript()) {
        return value;
    }
    int biased = value;
    switch (s.kind) {
    case kChance:
        biased = lo + (int)((value - lo) * factor);
        biased = biased < lo ? lo : biased > hi ? hi : biased;
        break;
    case kScale:
    case kCount:
        biased = Scaled(value, factor);
        break;
    }
    Trace("%s: %d -> %d", s.what, value, biased);
    return biased;
}

int NativeAIScriptTimer(int delay, int event) {
    if (!g_profile.scriptBias) {
        return delay;
    }
    int i = SiteHere();
    if (i < 0) {
        return delay;
    }
    const Site& s = kSites[i];
    float factor = s.factor[g_level];
    if (s.kind != kTimer || s.a != delay || s.b != event || factor == 1.0f || !HostileScript()) {
        return delay;
    }
    int biased = Scaled(delay, factor);
    Trace("%s: %d -> %d frames", s.what, delay, biased);
    return biased;
}
