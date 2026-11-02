// Rewritten's changes to the level scripts' code, made as each class loads
// (BSFileReadClass). A patch names a class, a thread and a word of the
// class's own code, and the words it expects there (tools/bsdis.py lists
// them), so a different script version is left alone. Classic loads the
// scripts as they are.

#include <string.h>
#include <strings.h>

#include "game/bsfile.h"
#include "game/bstypes.h"
#include "native/hooks.h"

namespace {

// The M1 Garand reloads like the other guns. On the reload button (Player,
// thread 0, state 0 on event 0x11, the same in every level) the player's
// script reloads when there's ammo in reserve and the clip isn't full, but
// the Garand (weapon 14) only once its clip is empty:
//   LG 3; LM 1; PUSH 14; NE.ii; BNE +5; PUSH 10; NEXTSTATE   (state 10 reloads)
// A weapon no gun is, in place of 14, sends every gun to state 10. The
// rounds left in the clip stay (CWeapon::DoneReloading tops it up).
const u32 kGarandReload[] = {
    0x00030018, 0x0001001B, 0x00000011, 14, 0x0202001E, 0x00050000, 0x00000011, 10, 0x00000017,
};

struct Patch {
    const char* cls;
    u8 thread;
    u16 word;            // where `expect` starts in the class's own code
    const u32* expect;
    int count;
    int at;              // the word of `expect` to replace, and its new value
    u32 value;
};

const Patch kPatches[] = {
    {"Player", 0, 0x00AE, kGarandReload, sizeof(kGarandReload) / sizeof(u32), 3, 0xFFFFFFFF},
};

}  // namespace

void NativeScriptClassLoaded(BSClass_struct* cls) {
    if (NativeGameMode() != NATIVE_MODE_REWRITTEN) {
        return;
    }
    for (const Patch& p : kPatches) {
        if (p.thread >= cls->m_numThreads || strcasecmp(p.cls, cls->m_name) != 0) {
            continue;
        }
        BSCode_struct* code = &cls->m_threads[p.thread];
        u32* words = ADDR_TO_PTR(u32, code->m_code[code->m_numInherited]);
        // The .cbs stores each thread's word count just before its code.
        if (words == NULL || words[-1] < (u32)(p.word + p.count) ||
            memcmp(words + p.word, p.expect, p.count * sizeof(u32)) != 0) {
            continue;
        }
        words[p.word + p.at] = p.value;
    }
}
