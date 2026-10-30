// Rebooting the game in place between levels.
//
// The game leaves a level by booting a stub program that boots the game again
// (LoadElf::RunModule): on the console everything starts over but the memory
// block LoadElf keeps. Natively, the game image's static data and the arena
// are put back as they were when the game's static initializers had run (the
// snapshot StartGame takes), the platform layer's parts go back to how the
// game found them, and main() starts again on a new default thread. The
// window, the Metal device and its pipelines, and the sound output stay, so
// changing levels is a black screen for a moment instead of a new process.
//
// The snapshot (TakeSnapshot):
// - the game image's writable segments (__DATA, __DATA_DIRTY; not
//   __DATA_CONST, which dyld makes read-only once it's bound);
// - the arena's pages that aren't all zero, found among the resident ones;
// - the OS heaps and arena bounds, and the default thread's OSThread.
//
// FRONTLINE_RESTART_EXEC=1 restarts the process instead, as before
// (game_hooks.cpp).

#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "frontline/platform.h"
#include "native/address.h"
#include "os_internal.h"

namespace frontline {
extern const char* g_gameImagePath;  // address.cpp
}

namespace frontline::os {
namespace {

constexpr size_t kPage = 0x4000;
constexpr uint32_t kArenaGuard = 0x10000;  // address.cpp: the arena starts this far into the window

struct Segment {
    uint8_t* addr;
    size_t size;
    uint8_t* copy;
};
Segment g_segments[8];
int g_numSegments;

uint8_t* g_arenaLo;  // the arena's usable range, as ArenaInit gave it
size_t g_arenaSize;
uint32_t* g_arenaPages;  // indexes of the pages kept, from g_arenaLo
uint8_t* g_arenaCopy;
size_t g_numArenaPages;

int (*g_gameMain)();
bool g_taken;
unsigned g_reboots;

void SaveImage() {
    const mach_header_64* header = nullptr;
    intptr_t slide = 0;
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        if (g_gameImagePath != nullptr && std::strcmp(_dyld_get_image_name(i), g_gameImagePath) == 0) {
            header = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(i));
            slide = _dyld_get_image_vmaddr_slide(i);
        }
    }
    if (header == nullptr) {
        return;  // linked into the executable: no image to put back (the exec fallback reboots)
    }
    auto* cmd = reinterpret_cast<const load_command*>(header + 1);
    for (uint32_t i = 0; i < header->ncmds; i++) {
        if (cmd->cmd == LC_SEGMENT_64) {
            auto* seg = reinterpret_cast<const segment_command_64*>(cmd);
            const bool writable = (seg->initprot & VM_PROT_WRITE) != 0;
            const bool sealed = std::strcmp(seg->segname, "__DATA_CONST") == 0 ||
                                std::strcmp(seg->segname, "__AUTH_CONST") == 0 ||
                                std::strcmp(seg->segname, SEG_LINKEDIT) == 0;
            if (writable && !sealed && seg->vmsize != 0 && g_numSegments < 8) {
                Segment& s = g_segments[g_numSegments++];
                s.addr = reinterpret_cast<uint8_t*>(seg->vmaddr + slide);
                s.size = seg->vmsize;
                s.copy = static_cast<uint8_t*>(std::malloc(s.size));
                std::memcpy(s.copy, s.addr, s.size);
            }
        }
        cmd = reinterpret_cast<const load_command*>(reinterpret_cast<const uint8_t*>(cmd) + cmd->cmdsize);
    }
}

bool AllZero(const uint8_t* p) {
    const uint64_t* w = reinterpret_cast<const uint64_t*>(p);
    for (size_t i = 0; i < kPage / 8; i++) {
        if (w[i] != 0) {
            return false;
        }
    }
    return true;
}

void SaveArena() {
    g_arenaLo = reinterpret_cast<uint8_t*>(g_nativeArenaBase + kArenaGuard);
    g_arenaSize = g_nativeArenaSize - kArenaGuard;
    const size_t pages = g_arenaSize / kPage;
    char* resident = static_cast<char*>(std::malloc(pages));
    if (mincore(g_arenaLo, g_arenaSize, resident) != 0) {
        std::memset(resident, 1, pages);  // look at every page
    }
    g_arenaPages = static_cast<uint32_t*>(std::malloc(pages * sizeof(uint32_t)));
    for (size_t i = 0; i < pages; i++) {
        if ((resident[i] & 1) && !AllZero(g_arenaLo + i * kPage)) {
            g_arenaPages[g_numArenaPages++] = (uint32_t)i;
        }
    }
    std::free(resident);
    g_arenaCopy = static_cast<uint8_t*>(std::malloc(g_numArenaPages * kPage));
    for (size_t i = 0; i < g_numArenaPages; i++) {
        std::memcpy(g_arenaCopy + i * kPage, g_arenaLo + (size_t)g_arenaPages[i] * kPage, kPage);
    }
}

void RestoreArena() {
    // New zero pages in place (the 4 GB window stays reserved around them).
    if (mmap(g_arenaLo, g_arenaSize, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) ==
        MAP_FAILED) {
        std::perror("frontline: reboot: arena mmap");
        std::abort();
    }
    for (size_t i = 0; i < g_numArenaPages; i++) {
        std::memcpy(g_arenaLo + (size_t)g_arenaPages[i] * kPage, g_arenaCopy + i * kPage, kPage);
    }
}

void* Rebooter(void*) {
    pthread_setname_np("frontline reboot");
    const bool trace = std::getenv("FRONTLINE_REBOOT_TRACE") != nullptr;
    const uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    StopGameThreads();
    // Work the host threads have in flight finishes or is dropped before the
    // interrupts are, and before the memory it would land in starts over.
    ResetDvd();
    ResetAudio();
    ResetVi();
    ResetPad();
    ResetCard();
    ResetAlarms();
    ResetGx();
    ResetThreads();
    RestoreMemoryState();
    RestoreArena();
    for (int i = 0; i < g_numSegments; i++) {
        std::memcpy(g_segments[i].addr, g_segments[i].copy, g_segments[i].size);
    }
    g_reboots++;
    if (trace) {
        std::fprintf(stderr, "frontline: reboot %u in %.1f ms\n", g_reboots,
                     (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start) / 1e6);
    }
    RestartMainThread(g_gameMain);
    return nullptr;
}

}  // namespace

void TakeSnapshot(int (*gameMain)()) {
    if (g_taken) {
        return;
    }
    g_taken = true;
    g_gameMain = gameMain;
    SaveImage();
    SaveArena();
    SaveMemoryState();
    SaveMainThread();
    if (std::getenv("FRONTLINE_REBOOT_TRACE") != nullptr) {
        size_t image = 0;
        for (int i = 0; i < g_numSegments; i++) {
            image += g_segments[i].size;
        }
        std::fprintf(stderr, "frontline: reboot snapshot: %zu KB of the game image, %zu KB of the arena\n",
                     image / 1024, g_numArenaPages * kPage / 1024);
    }
}

bool CanReboot() { return g_taken && g_numSegments > 0; }

void Reboot() {
    pthread_t thread;
    pthread_create(&thread, nullptr, Rebooter, nullptr);
    pthread_detach(thread);
    LeaveForReboot();
}

}  // namespace frontline::os
