// VI: video timing (docs/architecture/0002-threads-interrupts-and-time.md).
//
// A host thread ticks the retrace at NTSC's 60000/1001 Hz from the monotonic
// clock, never the display's refresh, so game time can't run fast on a 120 Hz
// screen. Each tick is an interrupt: the pre-retrace callback runs (the game
// queues its finished frame there), the flushed frame buffer becomes the
// displayed one, and threads in VIWaitForRetrace wake.
//
// Retrace n is due exactly n * 1001/60 ms after the clock started. The
// display places the frames it presents by those times (GetDisplayedFrame):
// the retrace that latched the frame shown, and the next, for a frame
// flushed since.

#include <mach/mach_time.h>
#include <pthread.h>

#include <atomic>

#include "dolphin/gx.h"
#include "dolphin/vi.h"
#include "frontline/platform.h"
#include "os/os_internal.h"

void PadSample();  // pad.cpp: the SI samples the controllers each retrace

namespace {

VIRetraceCallback g_preCallback;
OSThreadQueue g_retraceQueue;
u32 g_retraceCount;

void* g_nextFb;  // VISetNextFrameBuffer
bool g_nextBlack = true;
void* g_flushedFb;  // VIFlush latches these for the next retrace
bool g_flushedBlack = true;
// VIConfigure's frame buffer size: 640 x 448 until the game configures it.
std::atomic<uint32_t> g_fbSize{640u << 16 | 448u};

// What the display reads, together (a sequence lock: the game's threads
// write it, one at a time; the display retries a read that overlapped a
// write). Relaxed atomics, ordered by the sequence.
struct Shown {
    std::atomic<uint32_t> sequence;  // odd while being written
    std::atomic<uint32_t> xfb;       // the displayed frame buffer's address
    std::atomic<bool> black;
    std::atomic<uint32_t> retrace;  // the latest retrace
    std::atomic<uint32_t> latch;    // the retrace that latched the displayed frame
    std::atomic<uint32_t> next;     // flushed since, to show at the next retrace (0: none)
    std::atomic<bool> nextBlack;
} g_shown;

template <class Write>
void WriteShown(Write write) {
    g_shown.sequence.fetch_add(1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    write();
    g_shown.sequence.fetch_add(1, std::memory_order_release);
}

uint64_t g_startTicks;  // when retrace 0 was due (mach_absolute_time)
uint64_t g_startNanos;  // the same in nanoseconds

uint64_t HostNanos(uint64_t ticks) {
    static mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t;
        mach_timebase_info(&t);
        return t;
    }();
    return (uint64_t)((unsigned __int128)ticks * tb.numer / tb.denom);
}

// 1001/60 ms a retrace, kept exact over time by counting from the start.
uint64_t RetraceNanos(uint64_t n) { return g_startNanos + n * 1001000000ull / 60; }

void Retrace(void*) {
    g_retraceCount++;
    if (g_preCallback != nullptr) {
        g_preCallback(g_retraceCount);
    }
    PadSample();
    WriteShown([] {
        g_shown.retrace.store(g_retraceCount, std::memory_order_relaxed);
        // A frame flushed since the last retrace is latched now; with none,
        // the game missed this one and the same frame stays.
        if (g_shown.next.load(std::memory_order_relaxed) != 0) {
            g_shown.xfb.store(g_shown.next.load(std::memory_order_relaxed), std::memory_order_relaxed);
            g_shown.black.store(g_shown.nextBlack.load(std::memory_order_relaxed), std::memory_order_relaxed);
            g_shown.latch.store(g_retraceCount, std::memory_order_relaxed);
            g_shown.next.store(0, std::memory_order_relaxed);
        } else if (g_shown.black.load(std::memory_order_relaxed) != g_flushedBlack) {
            g_shown.black.store(g_flushedBlack, std::memory_order_relaxed);  // (VISetBlack before any frame)
        }
    });
    OSWakeupThread(&g_retraceQueue);
}

void* RetraceThread(void*) {
    pthread_setname_np("frontline retrace");
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    for (uint64_t n = 1;; n++) {
        uint64_t ns = n * 1001000000ull / 60;
        mach_wait_until(g_startTicks + ns * tb.denom / tb.numer);
        frontline::os::RaiseInterrupt(Retrace, nullptr);
    }
    return nullptr;
}

}  // namespace

frontline::DisplayedFrame frontline::GetDisplayedFrame() {
    DisplayedFrame f = {};
    for (;;) {
        const uint32_t sequence = g_shown.sequence.load(std::memory_order_acquire);
        if (sequence & 1) {
            continue;
        }
        f.xfb = g_shown.xfb.load(std::memory_order_relaxed);
        f.black = g_shown.black.load(std::memory_order_relaxed);
        f.retrace = g_shown.retrace.load(std::memory_order_relaxed);
        f.latchRetrace = g_shown.latch.load(std::memory_order_relaxed);
        f.nextXfb = g_shown.next.load(std::memory_order_relaxed);
        f.nextBlack = g_shown.nextBlack.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_shown.sequence.load(std::memory_order_relaxed) == sequence) {
            break;
        }
    }
    const uint32_t size = g_fbSize.load(std::memory_order_relaxed);
    f.width = (uint16_t)(size >> 16);
    f.height = (uint16_t)size;
    f.latchTime = RetraceNanos(f.latchRetrace) * 1e-9;
    f.nextLatchTime = RetraceNanos(f.retrace + 1) * 1e-9;
    return f;
}

extern "C" {

void VIInit(void) {
    static bool started;
    frontline::os::SafePoint();
    if (started) {
        return;
    }
    started = true;
    OSInitThreadQueue(&g_retraceQueue);
    g_startTicks = mach_absolute_time();
    g_startNanos = HostNanos(g_startTicks);
    pthread_t thread;
    pthread_create(&thread, nullptr, RetraceThread, nullptr);
    pthread_detach(thread);
}

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback callback) {
    BOOL level = OSDisableInterrupts();
    VIRetraceCallback prev = g_preCallback;
    g_preCallback = callback;
    OSRestoreInterrupts(level);
    return prev;
}

void VIConfigure(const GXRenderModeObj* mode) {
    frontline::os::SafePoint();
    g_fbSize.store((uint32_t)mode->fbWidth << 16 | mode->xfbHeight, std::memory_order_relaxed);
}

void VISetNextFrameBuffer(void* fb) {
    BOOL level = OSDisableInterrupts();
    g_nextFb = fb;
    OSRestoreInterrupts(level);
}

void VISetBlack(BOOL black) {
    BOOL level = OSDisableInterrupts();
    g_nextBlack = black != FALSE;
    OSRestoreInterrupts(level);
}

void VIFlush(void) {
    BOOL level = OSDisableInterrupts();
    g_flushedFb = g_nextFb;
    g_flushedBlack = g_nextBlack;
    WriteShown([] {
        g_shown.next.store(PTR_TO_ADDR(g_flushedFb), std::memory_order_relaxed);
        g_shown.nextBlack.store(g_flushedBlack, std::memory_order_relaxed);
    });
    OSRestoreInterrupts(level);
}

void VIWaitForRetrace(void) {
    frontline::os::SafePoint();
    BOOL level = OSDisableInterrupts();
    u32 count = g_retraceCount;
    while (count == g_retraceCount) {
        OSSleepThread(&g_retraceQueue);
    }
    OSRestoreInterrupts(level);
}

// The field the next retrace shows (interlaced: they alternate).
u32 VIGetNextField(void) { return (g_retraceCount + 1) & 1; }

u32 VIGetTvFormat(void) { return 0; }  // NTSC

u32 VIGetCurrentLine(void) { return 0; }

}  // extern "C"

// Rebooting (os/reboot.cpp): no callback or waiters, and a black screen
// until the game shows a frame again. The retrace clock keeps running.
void frontline::os::ResetVi() {
    g_preCallback = nullptr;
    OSInitThreadQueue(&g_retraceQueue);
    g_nextFb = nullptr;
    g_flushedFb = nullptr;
    g_nextBlack = g_flushedBlack = true;
    WriteShown([] {
        g_shown.next.store(0, std::memory_order_relaxed);
        g_shown.nextBlack.store(true, std::memory_order_relaxed);
        g_shown.black.store(true, std::memory_order_relaxed);
    });
}
