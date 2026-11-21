// The sound output (audio_out.cpp), fed by the AX mixer (audio.cpp).
#pragma once

#include <cstdint>

#include "frontline/platform.h"

namespace frontline::audio {

// Opens the output device (once), unless the app turned sound off.
void StartOutput();

// Queues mixed 32 kHz stereo frames for playback.
void Output(const int16_t (*frames)[2], uint32_t count);

// Prints how full the output buffer is, and how often it ran dry or overflowed.
void PrintStats();

// FRONTLINE_AUDIO_LOG: since the last call, how often the output ran dry
// (a gap in the sound), how many mixer frames it dropped for being too far
// ahead (a skip), and the least and most it had queued when a frame came, in
// milliseconds. `open`: there is an output device.
struct OutputStats {
    bool open;
    uint32_t underruns, drops;
    float leastMs, mostMs;
};
OutputStats TakeStats();

}  // namespace frontline::audio
