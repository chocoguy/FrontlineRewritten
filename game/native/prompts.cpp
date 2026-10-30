// The message table's prompts that name controller buttons, shown for the
// device in use: the keyboard's keys after the keyboard or mouse was used
// last ("Press F to Open Door"), the original text after a controller was
// ("Press ACTION to Open Door"). See NativePromptDevice in native/hooks.h.
//
// The game keeps pointers into the table (hints, objectives) and copies a
// prompt only when it shows it, so each rewritten message gets a slot big
// enough for both versions, and a device change copies the right one in.

#include <string.h>

#include "ea/real.h"
#include "game/memory.h"
#include "game/pumhandler.h"
#include "game/toilet.h"
#include "native/hooks.h"

namespace {

// A rewritten message: its slot in the table and both versions.
struct Prompt {
    UMEntry* entry;
    char* original;
    char* keyboard;
};

// The message tables loaded now: the shell's and the level's.
struct Table {
    CUserMessageTable* table;
    Prompt* prompts;
    int count;
    int device;
};
Table s_tables[4];
unsigned s_seenChanges;

// The controller-configuration screen's labels (MOVE ... ACTION, ACCEPT),
// which name actions, not buttons: ConfigGC copies 0x2E5..0x2F3, 0x2F7,
// 0x2F8 and 0x2FA (InitializeButtonMovementArray).
bool IsConfigLabel(int i) {
    return (i >= 0x2E5 && i <= 0x2F3) || i == 0x2F7 || i == 0x2F8 || i == 0x2FA;
}

void Show(Table& t, int device) {
    t.device = device;
    for (int i = 0; i < t.count; i++) {
        Prompt& p = t.prompts[i];
        const char* text = device == NATIVE_DEVICE_KEYBOARD ? p.keyboard : p.original;
        strcpy(p.entry->m_string, text);
        p.entry->m_length = (u8)strlen(text);
    }
}

}  // namespace

void NativePromptsLoaded(CUserMessageTable* table) {
    if (table->m_entries == NULL || table->m_text == NULL || table->m_count <= 0) {
        return;
    }
    Table* t = NULL;
    for (Table& candidate : s_tables) {
        if (candidate.table == NULL || candidate.table == table) {
            t = &candidate;
            break;
        }
    }
    if (t == NULL) {
        return;
    }

    // Sizes: every message, with room for both versions of the rewritten
    // ones, then those versions themselves.
    char keyboard[256];
    int rewritten = 0;
    u32 size = 0;
    for (int i = 0; i < table->m_count; i++) {
        int length = table->m_entries[i].m_length;
        if (!IsConfigLabel(i) && NativeKeyboardPrompt(table->m_entries[i].m_string, keyboard, sizeof(keyboard))) {
            int other = (int)strlen(keyboard);
            size += (length > other ? length : other) + 1 + length + 1 + other + 1;
            rewritten++;
        } else {
            size += length + 1;
        }
    }
    if (rewritten == 0) {
        return;
    }

    char* text = (char*)DWI_alloc("Native prompts", size, 0x400);
    Prompt* prompts = (Prompt*)DWI_alloc("Native prompts", rewritten * sizeof(Prompt), 0x400);
    char* at = text;
    int n = 0;
    for (int i = 0; i < table->m_count; i++) {
        UMEntry& entry = table->m_entries[i];
        const char* original = entry.m_string;
        int length = entry.m_length;
        char* slot = at;
        if (!IsConfigLabel(i) && NativeKeyboardPrompt(original, keyboard, sizeof(keyboard))) {
            int other = (int)strlen(keyboard);
            at += (length > other ? length : other) + 1;
            Prompt& p = prompts[n++];
            p.entry = &entry;
            p.original = at;
            memcpy(at, original, length + 1);
            at += length + 1;
            p.keyboard = at;
            memcpy(at, keyboard, other + 1);
            at += other + 1;
        } else {
            at += length + 1;
        }
        memcpy(slot, original, length + 1);
        entry.m_string = slot;
        entry.m_gap = 0;
    }

    TLT_CloseFile(table->m_text);
    table->m_text = text;
    t->table = table;
    t->prompts = prompts;
    t->count = rewritten;
    Show(*t, NativePromptDevice());
}

void NativePromptsUnloaded(CUserMessageTable* table) {
    for (Table& t : s_tables) {
        if (t.table == table) {
            MEM_free(t.prompts);
            t = Table();
        }
    }
}

void NativePromptsUpdate() {
    unsigned changes = NativePromptDeviceChanges();
    if (changes == s_seenChanges) {
        return;
    }
    s_seenChanges = changes;
    int device = NativePromptDevice();
    for (Table& t : s_tables) {
        if (t.table != NULL && t.device != device) {
            Show(t, device);
        }
    }
}
