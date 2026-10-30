// Rewritten's enhanced enemy AI: whether it runs, the difficulty profiles,
// what it keeps per AI object, its own randomness and tracing.
//
// Environment variables, read at each level start:
//   FRONTLINE_AI=off|easy|medium|hard  off, or a profile whatever the
//                                      difficulty (Rewritten only)
//   FRONTLINE_AI_TUNE=lead=1,tokens=4  profile settings by name (kTunes)
//   FRONTLINE_AI_TRACE=1               a line per decision on stderr

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai_internal.h"
#include "game/AIDoodad.h"
#include "game/AIStatus.h"
#include "game/ISceneNode.h"
#include "game/bstypes.h"
#include "game/camera.h"
#include "game/player.h"
#include "game/scene.h"
#include "game/vector.h"
#include "native/hooks.h"

bool g_nativeAIOn;

namespace nai {

// Starting values, to be tuned by playing. Easy keeps the original's
// aggression (one accurate shooter at a time makes it a little gentler);
// Medium and Hard raise it step by step.
const Profile kProfiles[3] = {
    // name     inacc aimCap lead reacq react prox batch tok hold miss callR callD flank adv  fwd gCool gWarn bias
    {"easy",    1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 5.0f, 8,   1,  4.0f, 1.6f, 10.0f, 1.5f, 0,  0.0f, 0, 12.0f, 0.8f, 1},
    {"medium",  0.9f, 1.5f, 0.4f, 1.0f, 0.75f, 6.0f, 10, 2,  4.0f, 1.3f, 20.0f, 1.0f, 1,  0.5f, 1, 9.0f,  0.5f, 1},
    {"hard",    0.7f, 1.0f, 0.8f, 1.0f, 0.4f, 8.0f, 12,  3,  5.0f, 1.0f, 30.0f, 0.6f, 2,  1.0f, 1, 7.0f,  0.3f, 1},
};

Profile g_profile = kProfiles[0];
int g_level;
bool g_trace;

namespace {

const int kMaxStates = 192;
State s_states[kMaxStates];
int s_numStates;

u32 s_random = 1;
int s_lineTests;
float s_lineTestsSince;

// FRONTLINE_AI_TUNE names.
struct Tune {
    const char* name;
    float* f;
    int* i;
};

void SetTune(const char* name, float value) {
    Profile& p = g_profile;
    const Tune tunes[] = {
        {"inaccuracy", &p.inaccuracy, NULL},
        {"aimtime", &p.aimTimeCap, NULL},
        {"lead", &p.lead, NULL},
        {"reacquire", &p.reacquire, NULL},
        {"reaction", &p.reaction, NULL},
        {"proximity", &p.proximity, NULL},
        {"batch", NULL, &p.scheduleBatch},
        {"tokens", NULL, &p.tokens},
        {"tokenhold", &p.tokenHold, NULL},
        {"miss", &p.missSpread, NULL},
        {"callout", &p.calloutRadius, NULL},
        {"calloutdelay", &p.calloutDelay, NULL},
        {"flankers", NULL, &p.flankers},
        {"advance", &p.advance, NULL},
        {"forwardcover", NULL, &p.forwardCover},
        {"grenadecooldown", &p.grenadeCooldown, NULL},
        {"grenadewarn", &p.grenadeWarn, NULL},
        {"scriptbias", NULL, &p.scriptBias},
    };
    for (size_t i = 0; i < sizeof(tunes) / sizeof(tunes[0]); i++) {
        if (strcmp(name, tunes[i].name) == 0) {
            if (tunes[i].f != NULL) {
                *tunes[i].f = value;
            } else {
                *tunes[i].i = (int)value;
            }
            return;
        }
    }
    fprintf(stderr, "FRONTLINE_AI_TUNE: no setting \"%s\"\n", name);
}

// The profile for FRONTLINE_AI, or -1 for off, or 0 to follow the difficulty.
int ProfileOverride() {
    const char* env = getenv("FRONTLINE_AI");
    if (env == NULL || *env == 0) {
        return 0;
    }
    if (strcmp(env, "off") == 0 || strcmp(env, "classic") == 0) {
        return -1;
    }
    for (int i = 0; i < 3; i++) {
        if (strcmp(env, kProfiles[i].name) == 0) {
            return i + 1;
        }
    }
    fprintf(stderr, "FRONTLINE_AI: \"%s\" is not off, easy, medium or hard\n", env);
    return 0;
}

bool NameHas(const CAIObject* ai, const char* prefix, const char* part) {
    BSObject* script = ScriptOf(ai);
    if (script == NULL || script->m_class == NULL) {
        return false;
    }
    const char* name = script->m_class->m_name;
    if (prefix != NULL && strncmp(name, prefix, strlen(prefix)) != 0) {
        return false;
    }
    return part == NULL || strstr(name, part) != NULL;
}

}  // namespace

State* Find(const CAIObject* ai) {
    for (int i = 0; i < s_numStates; i++) {
        if (s_states[i].ai == ai) {
            return &s_states[i];
        }
    }
    return NULL;
}

State* FindByNode(const ISceneNode* node) {
    if (node == NULL) {
        return NULL;
    }
    CAIDoodad* doodad = ((ISceneNode*)node)->GetAIDoodad();
    if (doodad == NULL || doodad->m_pFilter == NULL) {
        return NULL;
    }
    return Find(doodad->m_pFilter->m_pAIObject);
}

int NumStates() { return s_numStates; }
State* StateAt(int i) { return &s_states[i]; }

float Now() { return g_aigAIFilterGlobalObject.m_time; }
u32 Frame() { return g_aigAIFilterGlobalObject.m_frame; }

// xorshift32
float Random() {
    u32 x = s_random;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_random = x;
    return (x >> 8) * (1.0f / 16777216.0f);
}

float RandomRange(float lo, float hi) { return lo + (hi - lo) * Random(); }

float RandomSigned(float lo, float hi) {
    float v = RandomRange(lo, hi);
    return Random() < 0.5f ? -v : v;
}

CPlayerObject* Player() { return g_scene.GetPlayer(0); }

CAIObject* PlayerAI() {
    CPlayerObject* player = Player();
    if (player == NULL) {
        return NULL;
    }
    CAIDoodad* doodad = player->GetAIDoodad();
    if (doodad == NULL || doodad->m_pFilter == NULL) {
        return NULL;
    }
    return doodad->m_pFilter->m_pAIObject;
}

Vec3 PlayerEye() {
    CVector3 v;
    Player()->m_camera.GetPosition(v);
    return Vec3(v.x, v.y, v.z);
}

Vec3 PlayerLook() {
    CVector3 v;
    Player()->m_camera.GetForward(v);
    return Normalized(Vec3(v.x, v.y, v.z));
}

bool ClearSight(const Vec3& from, const Vec3& to, CAIFilterObject* ignore) {
    CountLineTest();
    return !CAIFilterGlobal::CheckVisionLineToEnvironment(ToPos(from), ToPos(to), ignore);
}

bool ClearPath(const Vec3& from, const Vec3& to, CAIFilterObject* ignore) {
    CountLineTest();
    return !CAIFilterGlobal::CheckCollisionLineToEnvironment(ToPos(from), ToPos(to), ignore);
}

float HitDistance(const Vec3& from, const Vec3& to) {
    CountLineTest();
    return CAIFilterGlobal::GetCollisionDistLineToEnvironment(ToPos(from), ToPos(to), NULL, Length(to - from));
}

bool Alive(const CAIObject* ai) {
    return ai->m_status.m_vars[AISTATUS_HEALTH] > 0 && ai->m_status.m_vars[AISTATUS_DEAD] == 0;
}

BSObject* ScriptOf(const CAIObject* ai) { return ai->m_pFilter != NULL ? ai->GetScript() : NULL; }

Vec3 EyeOf(const CAIObject* target) {
    if (target == PlayerAI()) {
        return PlayerEye();
    }
    return Up(ToVec(target->m_position), ((CAIObject*)target)->IsCrouching() ? 1.0f : 1.5f);
}

void Trace(const char* format, ...) {
    if (!g_trace) {
        return;
    }
    va_list args;
    va_start(args, format);
    fprintf(stderr, "[ai %7.2f] ", Now());
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}

// Traced once a second: how many line tests the AI added.
void CountLineTest() {
    s_lineTests++;
    if (g_trace && Now() - s_lineTestsSince >= 1.0f) {
        Trace("line tests: %d in %.1f s", s_lineTests, Now() - s_lineTestsSince);
        s_lineTests = 0;
        s_lineTestsSince = Now();
    }
}

}  // namespace nai

using namespace nai;

void NativeAILevelStart(int difficulty, bool multiplayer) {
    int level = difficulty < 1 ? 1 : difficulty > 3 ? 3 : difficulty;
    int forced = ProfileOverride();
    g_nativeAIOn = NativeGameMode() == NATIVE_MODE_REWRITTEN && !multiplayer && forced >= 0;
    if (forced > 0) {
        level = forced;
    }
    g_profile = kProfiles[level - 1];
    g_level = level - 1;
    ParseTunes(getenv("FRONTLINE_AI_TUNE"), SetTune);
    const char* trace = getenv("FRONTLINE_AI_TRACE");
    g_trace = g_nativeAIOn && trace != NULL && *trace != 0 && strcmp(trace, "0") != 0;
    s_random = 0x2545F491u ^ (u32)level;
    NativeAIReset();
    if (g_nativeAIOn) {
        fprintf(stderr, "Rewritten AI: %s profile\n", g_profile.name);
    }
}

void NativeAIReset() {
    s_numStates = 0;
    ResetCover();
    ResetGrenades();
    ResetDirector();
    ResetScriptBias();
}

// Infantry fighting the player get the profile's aim.
void NativeAIAdded(CAIObject* ai) {
    if (Find(ai) != NULL) {
        return;
    }
    if (s_numStates == kMaxStates) {
        Trace("state table full: %p not tracked", ai);
        return;
    }
    State* s = &s_states[s_numStates++];
    memset(s, 0, sizeof(*s));
    s->ai = ai;
    s->soldier = ai->field_0x15 == 4;
    s->hostile = ai->field_0x14 == 2;
    s->squad = s->soldier && NameHas(ai, "Soldier", NULL);
    s->roamer = s->squad && NameHas(ai, NULL, "_Roamer");
    s->nextSight = Now() + RandomRange(0.0f, 0.2f);
    s->hiddenSince = -1.0f;
    s->exposedSince = -1.0f;
    s->grenadeTan = kTan30;
    if (s->soldier && s->hostile) {
        ai->m_inaccuracy *= g_profile.inaccuracy;
        if (g_profile.aimTimeCap > 0.0f && ai->m_aimTime > g_profile.aimTimeCap) {
            ai->m_aimTime = g_profile.aimTimeCap;
        }
    }
    BSObject* script = ScriptOf(ai);
    Trace("+ %p %s team %d kind %d inaccuracy %.2f aim time %.1f%s", ai,
          script != NULL && script->m_class != NULL ? script->m_class->m_name : "?", ai->field_0x14, ai->field_0x15,
          ai->m_inaccuracy, ai->m_aimTime, s->roamer ? " (roamer)" : "");
}

void NativeAIRemoved(CAIObject* ai) {
    State* s = Find(ai);
    if (s != NULL) {
        *s = s_states[--s_numStates];
    }
}

int NativeAIReactionFrames(int frames) {
    if (frames <= 0) {
        return frames;
    }
    int scaled = (int)(frames * g_profile.reaction + 0.5f);
    return scaled < 1 ? 1 : scaled;
}

float NativeAIProximityRange() { return g_profile.proximity; }

int NativeAIScheduleBatch(int original) { return NativeAIOn() ? g_profile.scheduleBatch : original; }
