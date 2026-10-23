// The rest of the OS library the game calls: startup, reports, reset, the
// sound mode setting, and the CPU cache calls (no-ops: there's no DMA to keep
// coherent with). Each is a safe point, as any platform call is.

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include "os_internal.h"

using namespace frontline::os;

namespace {
u32 g_soundMode = 1;  // stereo
}

extern "C" {

void OSInit(void) {
    StartAlarms();
    OSGetArenaLo();
}

void OSReport(const char* msg, ...) {
    va_list args;
    va_start(args, msg);
    std::vfprintf(stderr, msg, args);
    va_end(args);
}

void OSPanic(const char* file, int line, const char* msg, ...) {
    std::fprintf(stderr, "OSPanic in %s:%d: ", file, line);
    va_list args;
    va_start(args, msg);
    std::vfprintf(stderr, msg, args);
    va_end(args);
    std::fprintf(stderr, "\n");
    __builtin_trap();
}

// The console restarts or goes back to its menu; natively the game ends.
void OSResetSystem(int reset, u32 resetCode, BOOL forceMenu) {
    std::fprintf(stderr, "frontline: OSResetSystem(%d, %#x, %d): exiting\n", reset, resetCode, forceMenu);
    std::exit(0);
}

u32 OSGetResetButtonState(void) {
    SafePoint();
    return 0;
}

u32 OSGetSoundMode(void) { return g_soundMode; }
void OSSetSoundMode(u32 mode) { g_soundMode = mode; }

void DCFlushRange(void*, u32) { SafePoint(); }
void DCFlushRangeNoSync(void*, u32) { SafePoint(); }
void DCStoreRange(void*, u32) { SafePoint(); }
void DCStoreRangeNoSync(void*, u32) { SafePoint(); }
void DCInvalidateRange(void*, u32) { SafePoint(); }
void ICInvalidateRange(void*, u32) { SafePoint(); }
void PPCSync(void) { SafePoint(); }

}  // extern "C"
