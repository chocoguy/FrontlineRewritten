// What the units left out of the native build (tools/gen_sources.py) provided.
//
// The game leaves a level by booting a stub program from the disc, which boots
// the game again; the shell's state rides along in a memory block the reboot
// keeps (LoadElf). Natively the process execs itself, with the block in a
// temporary file named by FRONTLINE_RESTART.

#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game/loadelf.h"

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
