// Checks the platform layer's OS library against the console's rules
// (docs/architecture/0002-threads-interrupts-and-time.md), without the game:
//   build/ninja/tests/ostest/frontline_ostest

#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "dolphin/os.h"
#include "frontline/platform.h"
#include "types.h"

namespace {

int g_failures;

#define EXPECT(cond, ...)                        \
    do {                                         \
        if (!(cond)) {                           \
            std::printf("  FAIL %s: ", #cond);   \
            std::printf(__VA_ARGS__);            \
            std::printf("\n");                   \
            g_failures++;                        \
        }                                        \
    } while (0)

char g_log[64];
int g_logLen;
void Log(char c) {
    g_log[g_logLen++] = c;
    g_log[g_logLen] = 0;
}

OSThread g_threads[4];
u8 g_stacks[4][0x1000];

void Create(int i, void* (*func)(void*), int priority) {
    OSCreateThread(&g_threads[i], func, nullptr, g_stacks[i] + sizeof(g_stacks[i]), sizeof(g_stacks[i]), priority, 0);
}

// Blocks the running thread for `ms`, so lower-priority threads get to run.
void SleepMs(int ms) {
    static OSThreadQueue queue;
    OSInitThreadQueue(&queue);
    OSAlarm alarm;
    OSCreateAlarm(&alarm);
    OSSetAlarm(&alarm, OSMillisecondsToTicks(ms), [](OSAlarm*, OSContext*) { OSWakeupThread(&queue); });
    OSSleepThread(&queue);
}

// A higher-priority thread runs as soon as it's resumed; a lower one only
// when the running thread blocks.
void TestPriorities() {
    g_logLen = 0;
    Create(0, [](void*) -> void* { Log('H'); return nullptr; }, 10);
    Create(1, [](void*) -> void* { Log('L'); return nullptr; }, 20);
    Log('1');
    OSResumeThread(&g_threads[1]);
    Log('2');
    OSResumeThread(&g_threads[0]);
    Log('3');
    SleepMs(5);
    Log('4');
    EXPECT(std::strcmp(g_log, "12H3L4") == 0, "order %s", g_log);
}

OSMessageQueue g_mq;
OSMessage g_msgs[2];

// A blocking queue of 2: the sender blocks while it's full, and the (lower
// priority) receiver drains it whenever the sender waits.
void TestMessages() {
    g_logLen = 0;
    OSInitMessageQueue(&g_mq, g_msgs, 2);
    Create(0, [](void*) -> void* {
        OSMessage msg;
        for (;;) {
            OSReceiveMessage(&g_mq, &msg, OS_MESSAGE_BLOCK);
            if ((sptr)msg == 0) {
                return nullptr;
            }
            Log((char)(sptr)msg);
        }
    }, 20);
    OSResumeThread(&g_threads[0]);
    for (char c : {'a', 'b', 'c', 'd', 'e'}) {
        OSSendMessage(&g_mq, (OSMessage)(sptr)c, OS_MESSAGE_BLOCK);
    }
    OSSendMessage(&g_mq, (OSMessage)0, OS_MESSAGE_BLOCK);
    for (int i = 0; i < 100 && g_threads[0].state != OS_THREAD_STATE_MORIBUND; i++) {
        SleepMs(1);
    }
    EXPECT(std::strcmp(g_log, "abcde") == 0, "received %s", g_log);
}

OSThreadQueue g_wakeQueue;
std::atomic<int> g_fired;

// While every thread waits, an alarm still fires (from the idle loop) and
// wakes one.
void TestAlarmWhileIdle() {
    OSInitThreadQueue(&g_wakeQueue);
    OSAlarm alarm;
    OSCreateAlarm(&alarm);
    OSTime start = OSGetTime();
    OSSetAlarm(&alarm, OSMillisecondsToTicks(20), [](OSAlarm*, OSContext*) {
        g_fired = 1;
        OSWakeupThread(&g_wakeQueue);
    });
    OSSleepThread(&g_wakeQueue);
    OSTime ms = OSTicksToMilliseconds(OSGetTime() - start);
    EXPECT(g_fired && ms >= 19 && ms < 200, "woke after %lld ms (fired %d)", ms, g_fired.load());
}

std::atomic<int> g_ticks;

// A periodic alarm delivered at safe points while the thread is busy, and
// held back while interrupts are disabled.
void TestPeriodicAndDisable() {
    OSAlarm alarm;
    OSCreateAlarm(&alarm);
    g_ticks = 0;
    OSSetPeriodicAlarm(&alarm, OSGetTime(), OSMillisecondsToTicks(10), [](OSAlarm*, OSContext*) { g_ticks++; });
    OSTime start = OSGetTime();
    while (OSTicksToMilliseconds(OSGetTime() - start) < 105) {
    }
    int busy = g_ticks;
    EXPECT(busy >= 9 && busy <= 12, "%d ticks in 105 ms of safe points", busy);

    BOOL level = OSDisableInterrupts();
    int before = g_ticks;
    start = OSGetTime();
    while (OSTicksToMilliseconds(OSGetTime() - start) < 50) {
    }
    int held = g_ticks - before;
    OSRestoreInterrupts(level);
    int after = g_ticks - before;
    EXPECT(held == 0 && after >= 1, "%d ticks while disabled, %d after", held, after);
    OSCancelAlarm(&alarm);
}

OSMutex g_mutex;

// A low-priority owner inherits the priority of a high thread waiting on its
// mutex, so it runs before a middle-priority thread. Without inheritance the
// order would be "lMuH".
void TestMutexInheritance() {
    g_logLen = 0;
    OSInitMutex(&g_mutex);
    OSInitThreadQueue(&g_wakeQueue);
    Create(0, [](void*) -> void* {
        OSLockMutex(&g_mutex);
        Log('l');
        OSSleepThread(&g_wakeQueue);
        Log('u');
        OSUnlockMutex(&g_mutex);
        return nullptr;
    }, 25);
    Create(1, [](void*) -> void* {
        OSLockMutex(&g_mutex);
        Log('H');
        OSUnlockMutex(&g_mutex);
        return nullptr;
    }, 5);
    Create(2, [](void*) -> void* { Log('M'); return nullptr; }, 17);
    OSResumeThread(&g_threads[0]);
    SleepMs(5);                     // low takes the mutex and waits
    OSResumeThread(&g_threads[1]);  // high waits for the mutex: low goes to 5
    EXPECT(g_threads[0].priority == 5, "owner at %d", g_threads[0].priority);
    OSResumeThread(&g_threads[2]);  // middle is ready, below main
    OSWakeupThread(&g_wakeQueue);   // low (5) runs at once, then high
    SleepMs(5);                     // then middle
    EXPECT(std::strcmp(g_log, "luHM") == 0, "order %s", g_log);
}

// Locals have 32-bit addresses: game thread stacks are in the arena.
void TestStackAddresses() {
    int local = 0;
    u32 addr = PTR_TO_ADDR(&local);
    EXPECT(ADDR_TO_PTR(int, addr) == &local, "local at %p maps to %#x", (void*)&local, addr);
}

// Plain numbers survive a round trip through a pointer.
void TestValueRoundTrip() {
    f32 f = 1.5f;
    u32 bits;
    std::memcpy(&bits, &f, 4);
    for (u32 v : {bits, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 5u}) {
        EXPECT(PTR_TO_ADDR(ADDR_TO_PTR(void, v)) == v, "%#x", v);
    }
}

// OS heaps: allocations are 32-byte aligned and come back after a free.
void TestHeap() {
    void* lo = OSInitAlloc(OSGetArenaLo(), OSGetArenaHi(), 1);
    OSHeapHandle heap = OSCreateHeap(lo, (u8*)lo + 0x10000);
    void* a = OSAllocFromHeap(heap, 100);
    void* b = OSAllocFromHeap(heap, 0x8000);
    EXPECT(a != nullptr && b != nullptr && ((uptr)a & 0x1F) == 0 && ((uptr)b & 0x1F) == 0, "%p %p", a, b);
    EXPECT(OSAllocFromHeap(heap, 0x8000) == nullptr, "over-allocated");
    OSFreeToHeap(heap, b);
    OSFreeToHeap(heap, a);
    EXPECT(OSAllocFromHeap(heap, 0xF000) != nullptr, "freed blocks didn't merge");
}

int TestMain() {
    TestStackAddresses();
    TestValueRoundTrip();
    TestHeap();
    TestPriorities();
    TestAlarmWhileIdle();
    TestMessages();
    TestPeriodicAndDisable();
    TestMutexInheritance();
    std::printf("ostest: %s (%d failures)\n", g_failures ? "FAILED" : "ok", g_failures);
    std::exit(g_failures ? 1 : 0);
}

}  // namespace

int main() {
    frontline::StartGame(TestMain);
    for (;;) {
        pause();
    }
}
