// Crash reports for the port: on SIGSEGV/SIGBUS/SIGILL/SIGTRAP/SIGABRT, prints
// the fault address (and its 32-bit game address, when it is in the window),
// the thread, and a backtrace to stderr, then lets the default action run (so
// the system crash report and debuggers still see the crash). macOS throttles
// its own reports for a process that crashes often, which this one does
// during porting.

#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "native/address.h"
#include "os/os_internal.h"

namespace {

void Write(const char* s) { write(STDERR_FILENO, s, std::strlen(s)); }

void Handler(int sig, siginfo_t* info, void*) {
    char line[256];
    char name[64] = "";
    pthread_getname_np(pthread_self(), name, sizeof(name));
    uintptr_t addr = reinterpret_cast<uintptr_t>(info->si_addr);
    std::snprintf(line, sizeof(line), "\nfrontline: %s at %#lx on thread \"%s\"\n", strsignal(sig),
                  static_cast<unsigned long>(addr), name);
    Write(line);
    if (g_nativeArenaBase != 0 && addr >= g_nativeArenaBase && addr - g_nativeArenaBase < NATIVE_WINDOW_SIZE) {
        std::snprintf(line, sizeof(line), "frontline: that is game address %#x\n",
                      static_cast<unsigned>(addr - g_nativeArenaBase));
        Write(line);
    }
    void* frames[64];
    int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    // Run the default action on return (the faulting instruction runs again).
    signal(sig, SIG_DFL);
    if (sig == SIGABRT || sig == SIGTRAP) {
        raise(sig);
    }
}

}  // namespace

void frontline::os::InstallCrashHandler() {
    struct sigaction sa = {};
    sa.sa_sigaction = Handler;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGABRT}) {
        sigaction(sig, &sa, nullptr);
    }
}
