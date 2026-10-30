// The game's 32-bit address map (docs/architecture/0001-pointers-and-memory.md):
// the arena that holds every game heap, plus two images: the game's
// (FrontlineGame.so, at NATIVE_IMAGE_ADDR, which native/address.h's fast paths
// cover) and then the one holding this platform layer, whose statics the game
// keeps pointers to too (the default thread's OSThread). Run without a game
// image (the tests link the platform alone), the map has only the second.

#include "frontline/platform.h"

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <sys/mman.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
uintptr_t g_nativeArenaBase;
uint32_t g_nativeArenaSize;
uintptr_t g_nativeImageBase;
uint32_t g_nativeImageSize;
}

namespace frontline {
// The game image's path, set as LoadGame loads it, so the map finds it while
// its static initializers run.
const char* g_gameImagePath;
}

namespace {

// The first 64 KB of the arena stay inaccessible, so offset 0 (null) and
// small offsets never alias a real object. So does everything past the arena
// in the 4 GB window.
constexpr uint32_t kArenaGuard = 0x10000;

// The second image: the one holding this code, after the game's.
uintptr_t g_hostImageBase;
uint32_t g_hostImageSize;
uint32_t g_hostImageAddr;  // its 32-bit address

// An image's address range (code, constants, statics), from its first
// segment after __PAGEZERO to the end of its last.
void ImageRange(const mach_header_64* header, uintptr_t* base, uint32_t* size) {
    uintptr_t slide = 0;
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        if (_dyld_get_image_header(i) == reinterpret_cast<const mach_header*>(header)) {
            slide = _dyld_get_image_vmaddr_slide(i);
            break;
        }
    }
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    auto* cmd = reinterpret_cast<const load_command*>(header + 1);
    for (uint32_t i = 0; i < header->ncmds; i++) {
        if (cmd->cmd == LC_SEGMENT_64) {
            auto* seg = reinterpret_cast<const segment_command_64*>(cmd);
            if (std::strcmp(seg->segname, SEG_PAGEZERO) != 0 && std::strcmp(seg->segname, SEG_LINKEDIT) != 0) {
                lo = std::min<uintptr_t>(lo, seg->vmaddr + slide);
                hi = std::max<uintptr_t>(hi, seg->vmaddr + slide + seg->vmsize);
            }
        }
        cmd = reinterpret_cast<const load_command*>(reinterpret_cast<const uint8_t*>(cmd) + cmd->cmdsize);
    }
    if (hi - lo >= 0x40000000u) {
        std::fprintf(stderr, "frontline: an image is too large for the address map\n");
        std::abort();
    }
    *base = lo;
    *size = static_cast<uint32_t>(hi - lo);
}

const mach_header_64* ImageNamed(const char* path) {
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        if (std::strcmp(_dyld_get_image_name(i), path) == 0) {
            return reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(i));
        }
    }
    return nullptr;
}

bool g_imagesFound;

void FindImage() {
    if (g_imagesFound) {
        return;
    }
    g_imagesFound = true;
    Dl_info info;
    if (dladdr(reinterpret_cast<const void*>(&FindImage), &info) == 0) {
        std::fprintf(stderr, "frontline: can't find the executable image\n");
        std::abort();
    }
    const mach_header_64* game =
        frontline::g_gameImagePath != nullptr ? ImageNamed(frontline::g_gameImagePath) : nullptr;
    if (game != nullptr) {
        ImageRange(game, &g_nativeImageBase, &g_nativeImageSize);
    }
    ImageRange(static_cast<const mach_header_64*>(info.dli_fbase), &g_hostImageBase, &g_hostImageSize);
    g_hostImageAddr = NATIVE_IMAGE_ADDR + ((g_nativeImageSize + 0xFFFFu) & ~0xFFFFu);
    if (game == nullptr) {
        // No game image: this one is the map's first.
        g_nativeImageBase = g_hostImageBase;
        g_nativeImageSize = g_hostImageSize;
        g_hostImageAddr = NATIVE_IMAGE_ADDR;
    }
}

}  // namespace

extern "C" uint32_t NativePtrToAddrSlow(const void* p) {
    if (p == nullptr) {
        return 0;
    }
    FindImage();
    uintptr_t v = reinterpret_cast<uintptr_t>(p);
    if (v - g_nativeImageBase < g_nativeImageSize) {
        return NATIVE_IMAGE_ADDR + static_cast<uint32_t>(v - g_nativeImageBase);
    }
    if (v - g_hostImageBase < g_hostImageSize) {
        return g_hostImageAddr + static_cast<uint32_t>(v - g_hostImageBase);
    }
    // macOS maps nothing in the low 4 GB (__PAGEZERO), so this is an integer
    // the game carries in a pointer (event data, a flag): keep its value.
    if (v <= 0xFFFFFFFFu) {
        return static_cast<uint32_t>(v);
    }
    std::fprintf(stderr,
                 "frontline: pointer %p has no 32-bit address (not in the arena or the image).\n"
                 "A stack or host allocation was stored where the game keeps 32-bit pointers.\n",
                 p);
    __builtin_trap();
}

extern "C" void* NativeAddrToPtrSlow(uint32_t addr) {
    FindImage();
    if (addr - NATIVE_IMAGE_ADDR < g_nativeImageSize) {
        return reinterpret_cast<void*>(g_nativeImageBase + (addr - NATIVE_IMAGE_ADDR));
    }
    if (addr - g_hostImageAddr < g_hostImageSize) {
        return reinterpret_cast<void*>(g_hostImageBase + (addr - g_hostImageAddr));
    }
    if (g_nativeArenaBase != 0) {
        return reinterpret_cast<void*>(g_nativeArenaBase + addr);
    }
    std::fprintf(stderr, "frontline: address %#x used before the arena was set up\n", addr);
    __builtin_trap();
}

namespace frontline {

void ForgetImages() { g_imagesFound = false; }

void* ArenaInit(uint32_t size) {
    if (g_nativeArenaBase != 0) {
        return reinterpret_cast<void*>(g_nativeArenaBase + kArenaGuard);
    }
    if (size >= NATIVE_IMAGE_ADDR) {
        std::fprintf(stderr, "frontline: arena size %#x is too large\n", size);
        std::abort();
    }
    // Reserve the 4 GB window (native/address.h), then open up the arena at
    // its bottom; pages are only committed when touched.
    void* base = mmap(nullptr, NATIVE_WINDOW_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) {
        std::perror("frontline: address window mmap");
        std::abort();
    }
    if (mprotect(static_cast<uint8_t*>(base) + kArenaGuard, size - kArenaGuard, PROT_READ | PROT_WRITE) != 0) {
        std::perror("frontline: arena mprotect");
        std::abort();
    }
    g_nativeArenaBase = reinterpret_cast<uintptr_t>(base);
    g_nativeArenaSize = size;
    FindImage();
    return static_cast<uint8_t*>(base) + kArenaGuard;
}

void* ArenaLo() { return reinterpret_cast<void*>(g_nativeArenaBase + kArenaGuard); }
void* ArenaHi() { return reinterpret_cast<void*>(g_nativeArenaBase + g_nativeArenaSize); }

}  // namespace frontline
