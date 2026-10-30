// Rewritten: the bodies of the dead stay where they fell (native/corpses.h).
//
// A body is the dead soldier's own animated object, out of the scene and
// drawn by NativeCorpsesDraw in its last pose (nothing updates it, so its
// animation stays on the death animation's last frame). It stays for
// kHoldSeconds from the script's FadeOut, then fades as the original does
// (CSoldierObject::Draw's fade, run by NativeCorpsesUpdate) and goes. Everything else the
// soldier held goes back as CleanUp would give it: the hit volumes, the
// weapons, a helmet object (its mesh is kept on the head as a bare mesh), the
// AI. Its two bounding boxes come from a pool too, so the body uses copies of
// its own below.

#include <stdlib.h>
#include <string.h>

#include "game/AnimObjFactory.h"
#include "game/Soldier_object.h"
#include "game/box.h"
#include "game/draw_context.h"
#include "game/mainloop.h"
#include "game/objcreate.h"
#include "game/static_obj.h"
#include "game/staticobjfactory.h"
#include "native/corpses.h"
#include "native/hooks.h"

bool g_nativeCorpsesOn;

namespace {

// Bodies kept at most (and the factory's extra slots): enough for a level's
// fights to stay strewn behind the player. The animation matrix caches
// (animskinned.cpp, animmorph.cpp) have a slot for each of these and the 26
// living ones, or bodies in view would draw with others' bones.
const int kMaxCorpses = 64;

// How long a body stays (from the script's FadeOut), and how long it then
// takes to fade (the original's fades take 3 to 4 seconds).
const float kHoldSeconds = 90.0f;
const float kFadeSeconds = 4.0f;

// A fade duration no body reaches: it holds.
const float kHoldDuration = 1.0e30f;

struct Corpse {
    CSoldierObject* soldier;
    CVolBox localBound;
    CVolBox worldBound;
    unsigned int drawn;  // the draw pass it was last in view
};

// Oldest first.
Corpse s_corpses[kMaxCorpses];
int s_numCorpses;
unsigned int s_drawPass;

void Release(int i) {
    g_AnimObjFactory.ReleaseObject(s_corpses[i].soldier);
    s_numCorpses--;
    for (; i < s_numCorpses; i++) {
        s_corpses[i].soldier = s_corpses[i + 1].soldier;
        s_corpses[i].localBound = s_corpses[i + 1].localBound;
        s_corpses[i].worldBound = s_corpses[i + 1].worldBound;
        s_corpses[i].drawn = s_corpses[i + 1].drawn;
        s_corpses[i].soldier->m_pLocalBound = &s_corpses[i].localBound;
        s_corpses[i].soldier->m_pWorldBound = &s_corpses[i].worldBound;
    }
}

// Makes room for one more: the oldest body not seen in the last draw, or
// failing that the oldest.
void MakeRoom() {
    for (int i = 0; i < s_numCorpses; i++) {
        if (s_corpses[i].drawn != s_drawPass) {
            Release(i);
            return;
        }
    }
    Release(0);
}

}  // namespace

void NativeCorpsesLevelStart(bool multiplayer) {
    const char* env = getenv("FRONTLINE_CORPSES");
    bool off = env != NULL && strcmp(env, "off") == 0;
    g_nativeCorpsesOn = NativeGameMode() == NATIVE_MODE_REWRITTEN && !multiplayer && !off;
    s_numCorpses = 0;
}

int NativeCorpseSlots() {
    return g_nativeCorpsesOn ? kMaxCorpses : 0;
}

void NativeCorpseFadeOut(CSoldierObject* soldier) {
    // Its time still counts from now (BeginUpdate, then NativeCorpsesUpdate).
    soldier->m_fadeDuration = kHoldDuration;
}

bool NativeCorpseKeep(CSoldierObject* soldier) {
    // Only bodies the script was fading: soldiers removed alive (out of sight
    // to make room for others) still go.
    if (!g_nativeCorpsesOn || !soldier->m_fading) {
        return false;
    }
    if (s_numCorpses == kMaxCorpses) {
        MakeRoom();
    }
    Corpse& c = s_corpses[s_numCorpses++];
    c.soldier = soldier;
    c.drawn = s_drawPass;

    // CSoldierObject::CleanUp, but for the bounding boxes and the helmet.
    if (soldier->m_pLocalVolume != NULL) {
        FreeAnimatedVolume(soldier->m_pLocalVolume);
    }
    if (soldier->m_pWorldVolume != NULL) {
        FreeAnimatedVolume(soldier->m_pWorldVolume);
    }
    soldier->m_pLocalVolume = NULL;
    soldier->m_pWorldVolume = NULL;
    soldier->m_pHitVolume = NULL;
    c.localBound = *(CVolBox*)soldier->m_pLocalBound;
    c.worldBound = *(CVolBox*)soldier->m_pWorldBound;
    FreeBoxVolume(soldier->m_pLocalBound);
    FreeBoxVolume(soldier->m_pWorldBound);
    soldier->m_pLocalBound = &c.localBound;
    soldier->m_pWorldBound = &c.worldBound;
    soldier->RemoveWeapons();
    u32 head = soldier->FindAttachPoint("head");
    if (head != (u32)-1 && soldier->m_attachType[head] == SOLDIER_ATTACH_OBJECT) {
        CStaticObject* helmet = (CStaticObject*)soldier->m_attached[head];
        if (helmet->IsDrawEnabled()) {
            soldier->m_attached[head] = helmet->m_pMesh;
            soldier->m_attachType[head] = SOLDIER_ATTACH_MESH;
        } else {
            soldier->m_attached[head] = NULL;
            soldier->m_attachType[head] = SOLDIER_ATTACH_NONE;
        }
        CStaticObjectFactory::DeallocateStaticObject(helmet);
    }
    soldier->m_aiDoodad.CleanUp();
    // Its script is destroyed with it.
    soldier->m_pScriptObject = NULL;
    return true;
}

void NativeCorpsesUpdate(float dt) {
    float seconds = dt / 60.0f;
    for (int i = 0; i < s_numCorpses;) {
        CSoldierObject* soldier = s_corpses[i].soldier;
        soldier->m_fadeTime += seconds;
        if (soldier->m_fadeDuration == kHoldDuration && soldier->m_fadeTime >= kHoldSeconds) {
            // Draw fades over the second half of the duration.
            soldier->m_fadeDuration = 2.0f * kFadeSeconds;
            soldier->m_fadeTime = kFadeSeconds;
        }
        if (soldier->m_fadeTime >= soldier->m_fadeDuration) {
            Release(i);
        } else {
            i++;
        }
    }
}

void NativeCorpsesDraw(CDrawContext& ctx) {
    s_drawPass++;
    for (int i = 0; i < s_numCorpses; i++) {
        CSoldierObject* soldier = s_corpses[i].soldier;
        if (soldier->IsDrawEnabled() && soldier->IsVisible(ctx)) {
            s_corpses[i].drawn = s_drawPass;
            soldier->Draw(ctx);
        }
    }
}

void NativeCorpsesReset() {
    s_numCorpses = 0;
}
