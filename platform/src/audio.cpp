// AI/AR/ARQ/AX: audio, and the 16 MB of auxiliary RAM the sound banks live in.
//
// ARAM is a host buffer; ARQ transfers copy at once and complete through an
// interrupt, like the DMA. ARAM holds what the console's would, so its 16-bit
// samples are big-endian.
//
// AX is the DSP's mixer, done here on the CPU: every 5 ms frame (160 samples
// at 32 kHz) an interrupt mixes the running voices (DSP ADPCM, PCM16 or PCM8
// from ARAM, resampled, through their volume envelope) into the main and aux
// buses, runs the game's aux A callback, which takes the aux A send and returns
// its own CPU mix (EA's software voices and reverb), adds that return to the
// main mix and sends it to the speakers (audio_out.cpp). Then the frame
// callback runs, as on the console. Voices are read and updated in place in
// their parameter blocks, which the game also reads (play positions, stopped
// voices); the sync flags don't matter here.
//
// FRONTLINE_AUDIO_TRACE=1 prints each voice start, and the output buffer's
// state every 10 seconds.

#include <mach/mach_time.h>
#include <pthread.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dolphin/ai.h"
#include "dolphin/ar.h"
#include "dolphin/ax.h"
#include "audio_limiter.h"
#include "audio_out.h"
#include "native/hooks.h"
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
constexpr int kFrameSamples = 160;

AXVPB g_voices[AX_MAX_VOICES];
bool g_voiceUsed[AX_MAX_VOICES];
AXAuxCallback g_auxA;
void* g_auxAContext;
AXUserCallback g_userCallback;
bool g_axRunning;
int g_trace = -1;

// The buses: main, aux A and aux B, each left, right and surround.
enum { kMain, kAuxA, kAuxB };
s32 g_bus[3][3][kFrameSamples];

// Rewritten's limiter on the main mix (audio_limiter.h); FRONTLINE_LIMITER=off
// keeps the DSP's clipping, for comparison.
frontline::audio::Limiter g_limiter;
u32 g_limitedFrames;

bool LimiterOn() {
    static const bool off = [] {
        const char* env = std::getenv("FRONTLINE_LIMITER");
        return env != nullptr && std::strcmp(env, "off") == 0;
    }();
    return !off && NativeGameMode() == NATIVE_MODE_REWRITTEN;
}

inline u32 Join(u16 hi, u16 lo) { return (u32)hi << 16 | lo; }

inline s16 Clamp16(s32 v) { return (s16)(v < -0x8000 ? -0x8000 : v > 0x7FFF ? 0x7FFF : v); }

// The next sample of a running voice, as the DSP's accelerator reads it:
// addresses count samples (nibbles for ADPCM), the end address is the last
// sample, and at the end the voice loops or stops.
s16 NextSample(AXPB& pb) {
    u32 cur = Join(pb.addr.currentAddressHi, pb.addr.currentAddressLo);
    s32 v;
    switch (pb.addr.format) {
    case 0: {  // DSP ADPCM: 8-byte frames of a header byte and 14 nibbles
        if ((cur & 15) == 0) {
            pb.adpcm.pred_scale = g_aram[(cur >> 1) & (kAramSize - 1)];
            cur += 2;
        }
        u8 byte = g_aram[(cur >> 1) & (kAramSize - 1)];
        s32 nibble = (cur & 1) != 0 ? byte & 0xF : byte >> 4;
        if (nibble >= 8) {
            nibble -= 16;
        }
        u32 pred = (pb.adpcm.pred_scale >> 4) & 7;
        s32 c1 = (s16)pb.adpcm.a[pred][0], c2 = (s16)pb.adpcm.a[pred][1];
        v = (nibble << (pb.adpcm.pred_scale & 0xF)) + ((0x400 + c1 * (s16)pb.adpcm.yn1 + c2 * (s16)pb.adpcm.yn2) >> 11);
        v = Clamp16(v);
        pb.adpcm.yn2 = pb.adpcm.yn1;
        pb.adpcm.yn1 = (u16)v;
        break;
    }
    case 10: {  // PCM16, scaled by the gain (0x800 is 1)
        const u8* p = g_aram + ((cur * 2) & (kAramSize - 1));
        v = Clamp16(((s16)(p[0] << 8 | p[1]) * (s32)pb.adpcm.gain) >> 11);
        break;
    }
    case 25:  // PCM8
        v = Clamp16(((s8)g_aram[cur & (kAramSize - 1)] << 8) * (s32)pb.adpcm.gain >> 11);
        break;
    default:
        v = 0;
        break;
    }
    if (cur == Join(pb.addr.endAddressHi, pb.addr.endAddressLo)) {
        if (pb.addr.loopFlag) {
            cur = Join(pb.addr.loopAddressHi, pb.addr.loopAddressLo);
            if (pb.addr.format == 0) {
                pb.adpcm.pred_scale = pb.adpcmLoop.loop_pred_scale;
                pb.adpcm.yn1 = pb.adpcmLoop.loop_yn1;
                pb.adpcm.yn2 = pb.adpcmLoop.loop_yn2;
            }
        } else {
            pb.state = AX_PB_STATE_STOP;
        }
    } else {
        cur++;
    }
    pb.addr.currentAddressHi = (u16)(cur >> 16);
    pb.addr.currentAddressLo = (u16)cur;
    return (s16)v;
}

// One frame of a voice: resampled (linear) from its source rate, through the
// volume envelope, into each bus at its mix volume. Volumes are 1.15 fixed
// point (0x8000 is 1) and ramp by their deltas each sample.
void MixVoice(AXPB& pb) {
    s32 out[kFrameSamples];
    u32 ratio = Join(pb.src.ratioHi, pb.src.ratioLo);
    u32 frac = pb.src.currentAddressFrac;
    s32 s0 = (s16)pb.src.last_samples[2], s1 = (s16)pb.src.last_samples[3];
    s32 vol = pb.ve.currentVolume;
    for (int i = 0; i < kFrameSamples; i++) {
        s32 s = s0 + (((s1 - s0) * (s32)frac) >> 16);
        out[i] = (s * vol) >> 15;
        vol += pb.ve.currentDelta;
        vol = vol < 0 ? 0 : vol > 0xFFFF ? 0xFFFF : vol;
        for (frac += ratio; frac >= 0x10000; frac -= 0x10000) {
            s0 = s1;
            s1 = pb.state == AX_PB_STATE_RUN ? NextSample(pb) : 0;
        }
    }
    pb.src.currentAddressFrac = (u16)frac;
    pb.src.last_samples[2] = (u16)s0;
    pb.src.last_samples[3] = (u16)s1;
    pb.ve.currentVolume = (u16)vol;

    struct Send {
        u16* vol;
        u16* delta;
        s32* bus;
    } sends[] = {
        {&pb.mix.vL, &pb.mix.vDeltaL, g_bus[kMain][0]},          {&pb.mix.vR, &pb.mix.vDeltaR, g_bus[kMain][1]},
        {&pb.mix.vS, &pb.mix.vDeltaS, g_bus[kMain][2]},          {&pb.mix.vAuxAL, &pb.mix.vDeltaAuxAL, g_bus[kAuxA][0]},
        {&pb.mix.vAuxAR, &pb.mix.vDeltaAuxAR, g_bus[kAuxA][1]}, {&pb.mix.vAuxAS, &pb.mix.vDeltaAuxAS, g_bus[kAuxA][2]},
        {&pb.mix.vAuxBL, &pb.mix.vDeltaAuxBL, g_bus[kAuxB][0]}, {&pb.mix.vAuxBR, &pb.mix.vDeltaAuxBR, g_bus[kAuxB][1]},
        {&pb.mix.vAuxBS, &pb.mix.vDeltaAuxBS, g_bus[kAuxB][2]},
    };
    for (Send& send : sends) {
        s32 v = *send.vol, delta = (s16)*send.delta;
        if (v == 0 && delta <= 0) {
            continue;
        }
        for (int i = 0; i < kFrameSamples; i++) {
            send.bus[i] += (out[i] * v) >> 15;
            v += delta;
            v = v < 0 ? 0 : v > 0xFFFF ? 0xFFFF : v;
        }
        *send.vol = (u16)v;
    }
}

void TraceStart(const AXVPB& p) {
    const AXPB& pb = p.pb;
    std::fprintf(stderr,
                 "[ax] voice %u: format %u, %s %#x..%#x loop %#x, ratio %.3f, volume %#x, mix L %#x R %#x A %#x\n",
                 p.index, pb.addr.format, pb.addr.loopFlag ? "looping" : "once",
                 Join(pb.addr.currentAddressHi, pb.addr.currentAddressLo),
                 Join(pb.addr.endAddressHi, pb.addr.endAddressLo), Join(pb.addr.loopAddressHi, pb.addr.loopAddressLo),
                 Join(pb.src.ratioHi, pb.src.ratioLo) / 65536.0, pb.ve.currentVolume, pb.mix.vL, pb.mix.vR,
                 pb.mix.vAuxAL);
}

void RunFrame() {
    std::memset(g_bus, 0, sizeof(g_bus));
    for (AXVPB& p : g_voices) {
        if (p.pb.state == AX_PB_STATE_RUN) {
            MixVoice(p.pb);
        }
    }
    // The aux A callback replaces the send with what goes back to the main mix.
    if (g_auxA != nullptr) {
        void* data[3] = {g_bus[kAuxA][0], g_bus[kAuxA][1], g_bus[kAuxA][2]};
        g_auxA(data, g_auxAContext);
        for (int c = 0; c < 3; c++) {
            for (int i = 0; i < kFrameSamples; i++) {
                g_bus[kMain][c][i] += g_bus[kAuxA][c][i];
            }
        }
    }
    s16 frames[kFrameSamples][2];
    if (LimiterOn()) {
        // Rewritten: a soft limiter instead of the DSP's hard clipping.
        g_limiter.Process(g_bus[kMain][0], g_bus[kMain][1], frames, kFrameSamples);
        if (g_limiter.Gain() < 1.0f) {
            g_limitedFrames++;
        }
    } else {
        for (int i = 0; i < kFrameSamples; i++) {
            frames[i][0] = Clamp16(g_bus[kMain][0][i]);
            frames[i][1] = Clamp16(g_bus[kMain][1][i]);
        }
    }
    frontline::audio::Output(frames, kFrameSamples);
    static u32 count;
    if (g_trace > 0 && ++count % 2000 == 0) {
        frontline::audio::PrintStats();
        if (LimiterOn()) {
            std::fprintf(stderr, "[ax] limiter: %u of the last 2000 frames turned down\n", g_limitedFrames);
            g_limitedFrames = 0;
        }
    }
    if (g_userCallback != nullptr) {
        g_userCallback();
    }
}

// The frame interrupt. The aux callback enables interrupts, so a late frame
// can arrive inside one: it runs after, rather than over it.
void AxFrame(void*) {
    static bool running;
    static int late;
    if (!g_axRunning) {
        return;
    }
    if (running) {
        late++;
        return;
    }
    running = true;
    RunFrame();
    for (; late > 0; late--) {
        RunFrame();
    }
    running = false;
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
    if (g_trace < 0) {
        g_trace = std::getenv("FRONTLINE_AUDIO_TRACE") != nullptr;
    }
    frontline::audio::StartOutput();
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
void AXSetVoiceState(AXVPB* p, u16 state) {
    if (g_trace > 0 && state == AX_PB_STATE_RUN && p->pb.state != AX_PB_STATE_RUN) {
        TraceStart(*p);
    }
    p->pb.state = state;
}
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

// Rebooting (os/reboot.cpp): no voices or callbacks until AXInit, and ARAM's
// allocations start over. The frame clock and the output keep running (they
// play silence).
void frontline::os::ResetAudio() {
    g_axRunning = false;
    std::memset(g_voices, 0, sizeof(g_voices));
    std::memset(g_voiceUsed, 0, sizeof(g_voiceUsed));
    g_auxA = nullptr;
    g_auxAContext = nullptr;
    g_userCallback = nullptr;
    g_aramUsed = kAramBase;
    g_limiter.Reset();
}
