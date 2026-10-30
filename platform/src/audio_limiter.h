// Rewritten's soft limiter on the speakers' mix: the game's own mix overloads
// the output when loud effects land on music and dialogue (D-Day's beach), and
// the DSP clips it hard. Classic keeps that clipping.
//
// A stereo-linked peak limiter with lookahead, plain math so it's tested
// without the game (tests/limitertest):
// - each frame needs gain ceiling / peak when its louder channel is past the
//   ceiling, else 1;
// - the gain takes the smallest need over the next kLookahead frames, recovers
//   with a kReleaseSeconds time constant, and is smoothed over kLookahead
//   frames, so it has reached a peak's need by the time that peak comes out;
// - the sound comes out kLookahead - 1 frames late (1.5 ms).
// While nothing nears the ceiling the gain is exactly 1, so the samples come
// out unchanged, only delayed.
#pragma once

#include <cstdint>

namespace frontline::audio {

class Limiter {
public:
    static constexpr int kLookahead = 48;  // frames: 1.5 ms at 32 kHz
    static constexpr float kCeiling = 30934.0f;  // -0.5 dBFS of 16-bit full scale
    static constexpr float kReleaseSeconds = 0.08f;

    explicit Limiter(float rate = 32000.0f);

    // Limits `count` frames of the mix (16-bit scale, unclipped) into `out`.
    void Process(const int32_t* left, const int32_t* right, int16_t (*out)[2], int count);

    // Back to silence, with no gain reduction.
    void Reset();

    // The gain applied to the last frame out.
    float Gain() const { return m_gain; }

private:
    // The share of the way back to the need taken each frame. Double, as is
    // the released gain: in float, steps that small stall short of 1.
    double m_release;
    int m_pos = 0;    // ring position of the oldest frame
    int32_t m_delay[kLookahead][2];
    float m_need[kLookahead];  // each frame's own need, for the sliding minimum
    double m_held[kLookahead];  // the released gain, for the smoothing average
    double m_heldSum = 0;
    int m_heldBelow = 0;  // entries of m_held below 1: none means the gain is exactly 1
    double m_released = 1.0;
    float m_gain = 1.0f;
};

}  // namespace frontline::audio
