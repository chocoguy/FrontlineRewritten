// Rewritten's mods (native/mods.h): the catalog the app's Mods list shows,
// and which the player has on. The app's choice can change at any time; the
// game takes it as each level boots. FRONTLINE_MODS=<key>,<key> (or "none")
// overrides it, for headless runs.

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "frontline/platform.h"
#include "native/hooks.h"
#include "native/mods.h"

namespace {

struct Mod {
    const char* key;  // stable: the app keeps the choice under it
    const char* title;
    const char* summary;
};

// By NATIVE_MOD_*.
const Mod kMods[NATIVE_NUM_MODS] = {
    {"enemy-knives", "Knife Throwers",
     "Enemies throw knives at you instead of shooting: rapidly, and from any distance they can see you."},
    {"enemy-fists", "Fist Fighters",
     "Enemies never fire: they run at you and punch, very fast, once they're in reach."},
    {"flying", "Flying",
     "Jump, then press Jump again in the air to fly where you look. Hold Jump to climb and Crouch to descend; "
     "touch the ground to walk again."},
    {"ignore-player", "Enemies Ignore You",
     "Enemies never notice you: they don't see or hear you, and won't target you even when you shoot them. They still fight "
     "your allies."},
};

std::atomic<unsigned int> g_mods{0};

// The mods FRONTLINE_MODS names, or -1 when it's unset.
long ModsOverride() {
    static const long mods = [] {
        const char* list = std::getenv("FRONTLINE_MODS");
        if (list == nullptr) {
            return -1L;
        }
        long on = 0;
        while (*list != '\0') {
            size_t length = std::strcspn(list, ",");
            for (int i = 0; i < NATIVE_NUM_MODS; i++) {
                if (std::strlen(kMods[i].key) == length && std::strncmp(kMods[i].key, list, length) == 0) {
                    on |= 1L << i;
                }
            }
            list += length;
            list += *list == ',';
        }
        return on;
    }();
    return mods;
}

}  // namespace

int frontline::ModCount() { return NATIVE_NUM_MODS; }
const char* frontline::ModKey(int mod) { return mod >= 0 && mod < NATIVE_NUM_MODS ? kMods[mod].key : nullptr; }
const char* frontline::ModTitle(int mod) { return mod >= 0 && mod < NATIVE_NUM_MODS ? kMods[mod].title : nullptr; }
const char* frontline::ModSummary(int mod) {
    return mod >= 0 && mod < NATIVE_NUM_MODS ? kMods[mod].summary : nullptr;
}

void frontline::SetModOn(int mod, bool on) {
    if (mod < 0 || mod >= NATIVE_NUM_MODS) {
        return;
    }
    if (on) {
        g_mods.fetch_or(1u << mod, std::memory_order_relaxed);
    } else {
        g_mods.fetch_and(~(1u << mod), std::memory_order_relaxed);
    }
}

extern "C" unsigned int NativeMods(void) {
    if (NativeGameMode() != NATIVE_MODE_REWRITTEN) {
        return 0;
    }
    return ModsOverride() >= 0 ? (unsigned int)ModsOverride() : g_mods.load(std::memory_order_relaxed);
}
