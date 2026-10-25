// The DSP's output to the speakers: 32 kHz stereo frames from the AX mixer
// (audio.cpp) into a buffer that Core Audio's default output unit plays,
// converting to the device's rate.
//
// The mixer runs on the game's 5 ms clock, the device on its own, so the
// buffer absorbs the difference: playback waits for kPrime frames before it
// starts (and again after running dry), and frames that would overfill it are
// dropped.
//
// FRONTLINE_AUDIO_DUMP=<file.wav> also records everything mixed.

#include <AudioToolbox/AudioToolbox.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "audio_out.h"

namespace {

constexpr uint32_t kRate = 32000;
constexpr uint32_t kCapacity = 1u << 14;  // frames: half a second
constexpr uint32_t kPrime = kRate / 20;    // 50 ms
constexpr uint32_t kMaxQueued = kRate / 4;

bool g_enabled = true;
AudioComponentInstance g_unit;
float g_ring[kCapacity][2];
std::atomic<uint32_t> g_write, g_read;  // frame counts; the ring index is the count mod kCapacity
bool g_playing;                         // render thread only
std::atomic<uint32_t> g_underruns, g_drops;

OSStatus Render(void*, AudioUnitRenderActionFlags* flags, const AudioTimeStamp*, UInt32, UInt32 frames,
                AudioBufferList* buffers) {
    float* out = static_cast<float*>(buffers->mBuffers[0].mData);
    uint32_t read = g_read.load(std::memory_order_relaxed);
    uint32_t queued = g_write.load(std::memory_order_acquire) - read;
    if (!g_playing && queued >= kPrime) {
        g_playing = true;
    }
    uint32_t n = 0;
    if (g_playing) {
        n = queued < frames ? queued : frames;
        for (uint32_t i = 0; i < n; i++) {
            const float* f = g_ring[(read + i) % kCapacity];
            out[i * 2] = f[0];
            out[i * 2 + 1] = f[1];
        }
        g_read.store(read + n, std::memory_order_release);
        if (n < frames) {
            g_playing = false;  // ran dry: wait for the buffer to fill again
            g_underruns.fetch_add(1, std::memory_order_relaxed);
        }
    }
    std::memset(out + n * 2, 0, (frames - n) * 2 * sizeof(float));
    if (n == 0) {
        *flags |= kAudioUnitRenderAction_OutputIsSilence;
    }
    return noErr;
}

void Fail(const char* what, OSStatus status) {
    std::fprintf(stderr, "frontline: audio output: %s failed (%d); continuing without sound\n", what, (int)status);
    if (g_unit != nullptr) {
        AudioComponentInstanceDispose(g_unit);
        g_unit = nullptr;
    }
}

void OpenDevice() {
    AudioComponentDescription desc = {};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_DefaultOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent component = AudioComponentFindNext(nullptr, &desc);
    OSStatus status = component != nullptr ? AudioComponentInstanceNew(component, &g_unit) : -1;
    if (status != noErr) {
        return Fail("opening the default output", status);
    }
    AudioStreamBasicDescription format = {};
    format.mSampleRate = kRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mChannelsPerFrame = 2;
    format.mBitsPerChannel = 32;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = 8;
    format.mBytesPerPacket = 8;
    status = AudioUnitSetProperty(g_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format,
                                  sizeof(format));
    if (status != noErr) {
        return Fail("setting the stream format", status);
    }
    AURenderCallbackStruct callback = {Render, nullptr};
    status = AudioUnitSetProperty(g_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback,
                                  sizeof(callback));
    if (status != noErr) {
        return Fail("setting the render callback", status);
    }
    if ((status = AudioUnitInitialize(g_unit)) != noErr) {
        return Fail("initializing", status);
    }
    if ((status = AudioOutputUnitStart(g_unit)) != noErr) {
        return Fail("starting", status);
    }
}

// FRONTLINE_AUDIO_DUMP: a 16-bit stereo WAV file, its sizes kept current.
FILE* g_dump;
uint32_t g_dumpFrames;

void WriteLE(FILE* f, uint32_t v, int bytes) {
    for (int i = 0; i < bytes; i++) {
        std::fputc((int)(v >> (i * 8)) & 0xFF, f);
    }
}

void OpenDump() {
    const char* path = std::getenv("FRONTLINE_AUDIO_DUMP");
    if (path == nullptr || (g_dump = std::fopen(path, "wb")) == nullptr) {
        return;
    }
    std::fwrite("RIFF\0\0\0\0WAVEfmt ", 1, 16, g_dump);
    WriteLE(g_dump, 16, 4);
    WriteLE(g_dump, 1, 2);  // PCM
    WriteLE(g_dump, 2, 2);
    WriteLE(g_dump, kRate, 4);
    WriteLE(g_dump, kRate * 4, 4);
    WriteLE(g_dump, 4, 2);
    WriteLE(g_dump, 16, 2);
    std::fwrite("data\0\0\0\0", 1, 8, g_dump);
}

void Dump(const int16_t (*frames)[2], uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        WriteLE(g_dump, (uint16_t)frames[i][0], 2);
        WriteLE(g_dump, (uint16_t)frames[i][1], 2);
    }
    g_dumpFrames += count;
    if (g_dumpFrames % kRate < count) {  // about once a second
        long end = std::ftell(g_dump);
        std::fseek(g_dump, 4, SEEK_SET);
        WriteLE(g_dump, 36 + g_dumpFrames * 4, 4);
        std::fseek(g_dump, 40, SEEK_SET);
        WriteLE(g_dump, g_dumpFrames * 4, 4);
        std::fseek(g_dump, end, SEEK_SET);
        std::fflush(g_dump);
    }
}

}  // namespace

void frontline::SetAudioOutput(bool enabled) { g_enabled = enabled; }

namespace frontline::audio {

void StartOutput() {
    static bool started;
    if (started) {
        return;
    }
    started = true;
    OpenDump();
    if (g_enabled && std::getenv("FRONTLINE_NO_AUDIO") == nullptr) {
        OpenDevice();
    }
}

void Output(const int16_t (*frames)[2], uint32_t count) {
    if (g_dump != nullptr) {
        Dump(frames, count);
    }
    if (g_unit == nullptr) {
        return;
    }
    uint32_t write = g_write.load(std::memory_order_relaxed);
    if (write - g_read.load(std::memory_order_acquire) + count > kMaxQueued) {
        g_drops.fetch_add(1, std::memory_order_relaxed);
        return;  // the device is behind: drop these
    }
    for (uint32_t i = 0; i < count; i++) {
        g_ring[(write + i) % kCapacity][0] = frames[i][0] / 32768.0f;
        g_ring[(write + i) % kCapacity][1] = frames[i][1] / 32768.0f;
    }
    g_write.store(write + count, std::memory_order_release);
}

void PrintStats() {
    if (g_unit == nullptr) {
        return;
    }
    uint32_t queued = g_write.load(std::memory_order_relaxed) - g_read.load(std::memory_order_relaxed);
    std::fprintf(stderr, "[ax] output: %.1f ms buffered, %u underruns, %u frames dropped\n", queued * 1000.0 / kRate,
                 g_underruns.load(std::memory_order_relaxed), g_drops.load(std::memory_order_relaxed));
}

}  // namespace frontline::audio
