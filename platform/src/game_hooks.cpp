// Loading the game image, and what the units left out of the native build
// (tools/gen_sources.py) provided.
//
// The game leaves a level by booting a stub program from the disc, which boots
// the game again; the shell's state rides along in a memory block the reboot
// keeps (LoadElf). Natively the game reboots in place (os/reboot.cpp), the
// block kept where it is. With FRONTLINE_RESTART_EXEC=1 (or without a game
// image) the process execs itself instead, with the block in a temporary file
// named by FRONTLINE_RESTART.

#include <crt_externs.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "frontline/platform.h"
#include "game/loadelf.h"
#include "os/os_internal.h"

namespace frontline {
extern const char* g_gameImagePath;
void ForgetImages();
}  // namespace frontline

frontline::GameMain frontline::LoadGame(const char* path) {
    static char resolved[4096];
    if (realpath(path, resolved) == nullptr) {
        std::fprintf(stderr, "frontline: no game image at %s\n", path);
        return nullptr;
    }
    // The address map finds the image by its name as its initializers run.
    g_gameImagePath = resolved;
    ForgetImages();
    void* image = dlopen(resolved, RTLD_NOW | RTLD_LOCAL);
    if (image == nullptr) {
        std::fprintf(stderr, "frontline: can't load the game image: %s\n", dlerror());
        return nullptr;
    }
    auto entry = reinterpret_cast<GameMain>(dlsym(image, "FrontlineGameEntry"));
    auto free = reinterpret_cast<void (*)(void*)>(dlsym(image, "FrontlineGameFree"));
    if (entry == nullptr || free == nullptr) {
        std::fprintf(stderr, "frontline: the game image has no entry points\n");
        return nullptr;
    }
    os::SetGameFree(free);
    return entry;
}

namespace {

alignas(32) u8 g_loaderBlock[sizeof(LoadElf)];
bool g_loaded;

}  // namespace

LoadElf* LoadElf::Get() {
    if (!g_loaded) {
        g_loaded = true;
        if (const char* path = std::getenv("FRONTLINE_RESTART")) {
            if (FILE* f = std::fopen(path, "rb")) {
                std::fread(g_loaderBlock, 1, sizeof(g_loaderBlock), f);
                std::fclose(f);
            }
            unlink(path);
            unsetenv("FRONTLINE_RESTART");
        }
    }
    return reinterpret_cast<LoadElf*>(g_loaderBlock);
}

void LoadElf::RunModule(const char* name) {
    const char* exec = std::getenv("FRONTLINE_RESTART_EXEC");
    if (frontline::os::CanReboot() && (exec == nullptr || std::strcmp(exec, "1") != 0)) {
        std::fprintf(stderr, "frontline: rebooting in place (the console boots %s)\n", name);
        frontline::os::Reboot();
    }
    std::fprintf(stderr, "frontline: restarting (the console boots %s)\n", name);
    char path[] = "/tmp/frontline-restart-XXXXXX";
    int fd = mkstemp(path);
    if (fd >= 0) {
        write(fd, this, sizeof(LoadElf));
        close(fd);
        setenv("FRONTLINE_RESTART", path, 1);
    }
    char exe[4096];
    uint32_t size = sizeof(exe);
    if (_NSGetExecutablePath(exe, &size) == 0) {
        execv(exe, *_NSGetArgv());
    }
    std::perror("frontline: restart");
    std::exit(1);
}

extern "C" void DEBUG_break(void) {}
