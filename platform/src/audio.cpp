// AI/AR/ARQ/AX: audio, and the 16 MB of auxiliary RAM the sound banks live in.
//
// ARAM is a host buffer; ARQ transfers copy at once and complete through an
// interrupt, like the DMA. AX keeps the voices' parameter blocks and runs the
// game's frame callbacks every 5 ms (the DSP's frame), so EA's sound driver
// thread runs as on the console. Mixing the voices to Core Audio comes later;
// until then the frames are silent.

#include <mach/mach_time.h>
#include <pthread.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dolphin/ai.h"
#include "dolphin/ar.h"
#include "dolphin/ax.h"
#include "os/os_internal.h"

namespace {

constexpr u32 kAramSize = 16u << 20;
constexpr u32 kAramBase = 0x4000;  // the SDK reserves the start for the DSP
u8* g_aram;
u32 g_aramUsed = kAramBase;

void Transfer(u32 type, u32 mainAddr, u32 aramAddr, u32 length) {
    if (aramAddr + length > kAramSize) {
        std::fprintf(stderr, "frontline: ARAM transfer past the end (%#x + %#x)\n", aramAddr, length);
        std::abort();
    }
    u8* main = ADDR_TO_PTR(u8, mainAddr);
    if (type == ARQ_TYPE_MRAM_TO_ARAM) {
        std::memcpy(g_aram + aramAddr, main, length);
    } else {
        std::memcpy(main, g_aram + aramAddr, length);
    }
}

void ArqDone(void* arg) {
    ARQRequest* request = static_cast<ARQRequest*>(arg);
    if (request->callback != nullptr) {
        request->callback(PTR_TO_ADDR(request));
    }
}

// AX
AXVPB g_voices[AX_MAX_VOICES];
bool g_voiceUsed[AX_MAX_VOICES];
AXAuxCallback g_auxA;
void* g_auxAContext;
AXUserCallback g_userCallback;
s32 g_auxBuffers[3][160];  // left, right, surround: one 5 ms frame at 32 kHz
bool g_axRunning;

void AxFrame(void*) {
    if (!g_axRunning) {
        return;
    }
    if (g_auxA != nullptr) {
        std::memset(g_auxBuffers, 0, sizeof(g_auxBuffers));
        void* data[3] = {g_auxBuffers[0], g_auxBuffers[1], g_auxBuffers[2]};
        g_auxA(data, g_auxAContext);
    }
    if (g_userCallback != nullptr) {
        g_userCallback();
    }
}

void* FrameThread(void*) {
    pthread_setname_np("frontline audio frames");
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    uint64_t start = mach_absolute_time();
    for (uint64_t n = 1;; n++) {
        mach_wait_until(start + n * 5000000ull * tb.denom / tb.numer);
        frontline::os::RaiseInterrupt(AxFrame, nullptr);
    }
    return nullptr;
}

}  // namespace

extern "C" {

// AR
u32 ARInit(u32*, u32) {
    if (g_aram == nullptr) {
        g_aram = static_cast<u8*>(std::calloc(1, kAramSize));
    }
    return kAramBase;
}
BOOL ARCheckInit(void) { return g_aram != nullptr; }
u32 ARGetBaseAddress(void) { return kAramBase; }
u32 ARGetSize(void) { return kAramSize; }
u32 ARAlloc(u32 length) {
    u32 addr = g_aramUsed;
    g_aramUsed += length;
    return addr;
}
void ARStartDMA(u32 type, u32 mainmem_addr, u32 aram_addr, u32 length) {
    Transfer(type, mainmem_addr, aram_addr, length);
}

// ARQ
void ARQInit(void) { ARInit(nullptr, 0); }
BOOL ARQCheckInit(void) { return g_aram != nullptr; }
void ARQPostRequest(ARQRequest* request, u32 owner, u32 type, u32 priority, u32 source, u32 dest, u32 length,
                    ARQCallback callback) {
    frontline::os::SafePoint();
    request->next = nullptr;
    request->owner = owner;
    request->type = type;
    request->priority = priority;
    request->source = source;
    request->dest = dest;
    request->length = length;
    request->callback = callback;
    if (type == ARQ_TYPE_MRAM_TO_ARAM) {
        Transfer(type, source, dest, length);
    } else {
        Transfer(type, dest, source, length);
    }
    frontline::os::RaiseInterrupt(ArqDone, request);
}
void ARQRemoveRequest(ARQRequest*) {}

// AI
void AIInit(u8*) {}

// AX
void AXInit(void) {
    static bool started;
    std::memset(g_voices, 0, sizeof(g_voices));
    std::memset(g_voiceUsed, 0, sizeof(g_voiceUsed));
    for (u32 i = 0; i < AX_MAX_VOICES; i++) {
        g_voices[i].index = i;
    }
    g_axRunning = true;
    if (!started) {
        started = true;
        pthread_t thread;
        pthread_create(&thread, nullptr, FrameThread, nullptr);
        pthread_detach(thread);
    }
}

void AXQuit(void) { g_axRunning = false; }

AXVPB* AXAcquireVoice(u32 priority, AXVoiceCallback callback, u32 userContext) {
    frontline::os::SafePoint();
    for (u32 i = 0; i < AX_MAX_VOICES; i++) {
        if (!g_voiceUsed[i]) {
            g_voiceUsed[i] = true;
            AXVPB* p = &g_voices[i];
            std::memset(&p->pb, 0, sizeof(p->pb));
            p->priority = priority;
            p->callback = callback;
            p->userContext = userContext;
            p->index = i;
            return p;
        }
    }
    return nullptr;
}

void AXFreeVoice(AXVPB* p) {
    g_voiceUsed[p->index] = false;
    p->pb.state = AX_PB_STATE_STOP;
}

void AXSetVoiceSrcType(AXVPB* p, u32 type) { p->pb.srcSelect = (u16)type; }
void AXSetVoiceState(AXVPB* p, u16 state) { p->pb.state = state; }
void AXSetVoiceAddr(AXVPB* p, AXPBADDR* addr) { p->pb.addr = *addr; }
void AXSetVoiceAdpcm(AXVPB* p, AXPBADPCM* adpcm) { p->pb.adpcm = *adpcm; }
void AXSetVoiceAdpcmLoop(AXVPB* p, AXPBADPCMLOOP* loop) { p->pb.adpcmLoop = *loop; }
void AXSetVoiceSrcRatio(AXVPB* p, f32 ratio) {
    u32 r = (u32)(ratio * 65536.0f);
    p->pb.src.ratioHi = (u16)(r >> 16);
    p->pb.src.ratioLo = (u16)r;
}
void AXSetMode(u32) {}

void AXRegisterAuxACallback(AXAuxCallback callback, void* context) {
    BOOL level = OSDisableInterrupts();
    g_auxA = callback;
    g_auxAContext = context;
    OSRestoreInterrupts(level);
}

AXUserCallback AXRegisterCallback(AXUserCallback callback) {
    AXUserCallback prev = g_userCallback;
    g_userCallback = callback;
    return prev;
}

}  // extern "C"
