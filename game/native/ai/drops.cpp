// Rewritten: an enemy's drop (the ammo or health its level-editor entry
// names) appears the moment it dies, not when its death animation ends.
//
// The soldier scripts mark the soldier dead (AISetStatusVar(13, 15)) as they
// enter a death state, and call CreateDumpObject when its animation is over.
// At the death states below, which all end in a drop, the drop is made at
// once, and the script's later CreateDumpObject for that soldier is skipped.
// Deaths that never drop (a scripted kill by message) are left alone, as are
// the other scripts that drop things (crates, barrels, bottles).
//
// Not tied to FRONTLINE_AI: it holds in Rewritten whatever the AI does.

#include <string.h>
#include <strings.h>

#include "ai_internal.h"
#include "game/ISceneNode.h"
#include "game/ShellMenu.h"
#include "game/bstypes.h"
#include "game/objcreate.h"
#include "game/quaternion.h"
#include "game/vector.h"
#include "native/hooks.h"

namespace nai {
namespace {

// AISetStatusVar(13, 15) calls on the way to a drop (tools/bsdis.py
// --sites AISetStatusVar,CreateDumpObject).
struct DeathSite {
    const char* cls;
    u8 thread;
    u16 word;
};

const DeathSite kDeathSites[] = {
    {"SoldierA_Roamer", 0, 0x1952},          // state 26, drops in 28
    {"SoldierA_Roamer", 0, 0x2743},          // state 67, drops in 68
    {"SoldierA_Roamer", 0, 0x2779},          // state 70, drops in 72
    {"SoldierA_MachineGunNest", 0, 0x048F},  // state 94
    {"SoldierA_MachineGunNest", 0, 0x0865},  // state 104
    {"SoldierA_MachineGunNest", 0, 0x0B94},  // state 115
};

// Who has dropped already (the script object, and its id: objects are reused).
struct Dropped {
    BSObject* script;
    int id;
    float when;
};
const int kMaxDropped = 64;
Dropped s_dropped[kMaxDropped];
int s_next;

bool Rewritten() { return NativeGameMode() == NATIVE_MODE_REWRITTEN && !g_bInMultiplayerMode; }

bool AtDeathSite() {
    const char* cls;
    int thread;
    int word;
    if (!CallSite(&cls, &thread, &word)) {
        return false;
    }
    for (size_t i = 0; i < sizeof(kDeathSites) / sizeof(kDeathSites[0]); i++) {
        const DeathSite& d = kDeathSites[i];
        if (d.thread == thread && d.word == word && strcasecmp(d.cls, cls) == 0) {
            return true;
        }
    }
    return false;
}

Dropped* FindDropped(BSObject* script) {
    for (int i = 0; i < kMaxDropped; i++) {
        if (s_dropped[i].script == script && s_dropped[i].id == script->m_uniqueID) {
            return &s_dropped[i];
        }
    }
    return NULL;
}

// As BIFunc_CreateDumpObject: what the trigger names, at the soldier.
bool Drop(ISceneNode* self, TriggerObjectData_struct* data) {
    int dumpType;
    int id;
    switch (data->m_type) {
    case 3:
        dumpType = data->field_0x71;
        id = data->field_0x90;
        break;
    case 8:
    case 2:
        dumpType = data->field_0x71;
        id = data->field_0x74;
        break;
    default:
        return false;
    }
    if (id == 0 || id == -1) {
        return false;
    }
    CQuaternion q = *(CQuaternion*)data->m_orientation;
    CVector3 pos;
    self->GetPosition(pos);
    CreateDumpObject(pos, q, id, dumpType);
    return true;
}

}  // namespace
}  // namespace nai

using namespace nai;

void NativeEnemyDied(ISceneNode* self) {
    if (!Rewritten() || self == NULL) {
        return;
    }
    BSObject* script = self->GetScriptObject();
    if (script == NULL || script->m_trigger == NULL || script->m_trigger->m_data == NULL ||
        script->m_trigger->m_data->m_team != 2 || FindDropped(script) != NULL || !AtDeathSite()) {
        return;
    }
    if (Drop(self, script->m_trigger->m_data)) {
        Dropped& d = s_dropped[s_next];
        s_next = (s_next + 1) % kMaxDropped;
        d.script = script;
        d.id = script->m_uniqueID;
        d.when = Now();
        Trace("%s dropped its item as it died", script->m_class->m_name);
    }
}

bool NativeEnemyDropDone(ISceneNode* self) {
    if (!Rewritten() || self == NULL) {
        return false;
    }
    BSObject* script = self->GetScriptObject();
    Dropped* d = script != NULL ? FindDropped(script) : NULL;
    if (d == NULL) {
        return false;
    }
    d->script = NULL;
    Trace("%s: its animation's drop skipped, %.1f s after it died", script->m_class->m_name, Now() - d->when);
    return true;
}
