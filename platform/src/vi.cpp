// VI: video timing (docs/architecture/0002-threads-interrupts-and-time.md).
//
// A host thread ticks the retrace at NTSC's 60000/1001 Hz from the monotonic
// clock, never the display's refresh, so game time can't run fast on a 120 Hz
// screen. Each tick is an interrupt: the pre-retrace callback runs (the game
// queues its finished frame there), the flushed frame buffer becomes the
// displayed one, and threads in VIWaitForRetrace wake.

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

std::atomic<uint64_t> g_displayed;  // the displayed frame buffer's address << 1 | black
std::atomic<uint32_t> g_displayedRetrace;

void Retrace(void*) {
    g_retraceCount++;
    if (g_preCallback != nullptr) {
        g_preCallback(g_retraceCount);
    }
    PadSample();
    g_displayed.store((uint64_t)PTR_TO_ADDR(g_flushedFb) << 1 | (g_flushedBlack ? 1 : 0), std::memory_order_release);
    g_displayedRetrace.store(g_retraceCount, std::memory_order_release);
    OSWakeupThread(&g_retraceQueue);
}

void* RetraceThread(void*) {
    pthread_setname_np("frontline retrace");
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    // 1001/60 ms in host ticks, kept exact over time by counting from the start.
    uint64_t start = mach_absolute_time();
    for (uint64_t n = 1;; n++) {
        uint64_t ns = n * 1001000000ull / 60;
        mach_wait_until(start + ns * tb.denom / tb.numer);
        frontline::os::RaiseInterrupt(Retrace, nullptr);
    }
    return nullptr;
}

}  // namespace

frontline::DisplayedFrame frontline::GetDisplayedFrame() {
    uint64_t v = g_displayed.load(std::memory_order_acquire);
    return {(uint32_t)(v >> 1), (v & 1) != 0, g_displayedRetrace.load(std::memory_order_acquire)};
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

void VIConfigure(const GXRenderModeObj*) { frontline::os::SafePoint(); }

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
