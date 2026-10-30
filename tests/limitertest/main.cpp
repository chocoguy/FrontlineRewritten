// Checks Rewritten's soft limiter on the speakers' mix
// (platform/src/audio_limiter.h) without the game:
//   build/ninja/tests/limitertest/frontline_limitertest

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "audio_limiter.h"

namespace {

using frontline::audio::Limiter;

int g_failures;

#define EXPECT(cond, ...)                      \
    do {                                       \
        if (!(cond)) {                         \
            std::printf("  FAIL %s: ", #cond); \
            std::printf(__VA_ARGS__);          \
            std::printf("\n");                 \
            g_failures++;                      \
        }                                      \
    } while (0)

constexpr int kRate = 32000;
constexpr int kDelay = Limiter::kLookahead - 1;
constexpr int kBlock = 160;  // the mixer's 5 ms frames

struct Signal {
    std::vector<int32_t> left, right;
};

// Runs `in` through a fresh limiter in the mixer's blocks.
std::vector<int16_t> Run(const Signal& in, std::vector<float>* gains = nullptr) {
    Limiter limiter;
    size_t n = in.left.size();
    std::vector<int16_t> out(n * 2);
    for (size_t at = 0; at < n; at += kBlock) {
        int count = (int)std::min<size_t>(kBlock, n - at);
        int16_t block[kBlock][2];
        for (int i = 0; i < count; i++) {
            limiter.Process(&in.left[at + i], &in.right[at + i], &block[i], 1);
            out[(at + i) * 2] = block[i][0];
            out[(at + i) * 2 + 1] = block[i][1];
            if (gains != nullptr) {
                gains->push_back(limiter.Gain());
            }
        }
    }
    return out;
}

Signal Sine(int frames, double amplitude, double hz, double rightScale = 1.0) {
    Signal s;
    for (int i = 0; i < frames; i++) {
        double v = amplitude * std::sin(2 * M_PI * hz * i / kRate);
        s.left.push_back((int32_t)std::lrint(v));
        s.right.push_back((int32_t)std::lrint(v * rightScale));
    }
    return s;
}

void QuietIsUnchanged() {
    std::printf("quiet: delayed, not changed\n");
    Signal s = Sine(kRate, 20000, 440);
    std::vector<int16_t> out = Run(s);
    int differing = 0;
    for (size_t i = kDelay; i < s.left.size(); i++) {
        differing += out[i * 2] != s.left[i - kDelay] || out[i * 2 + 1] != s.right[i - kDelay];
    }
    EXPECT(differing == 0, "%d frames changed", differing);
    for (int i = 0; i < kDelay; i++) {
        EXPECT(out[i * 2] == 0 && out[i * 2 + 1] == 0, "frame %d before the delay isn't silent", i);
    }
}

void LoudStaysUnderCeiling() {
    std::printf("loud: never past the ceiling, never clipped\n");
    // Twice full scale, with a sudden step, square-ish bursts and noise.
    Signal s = Sine(kRate, 65000, 100);
    for (int i = kRate / 2; i < kRate / 2 + 400; i++) {
        s.left[i] = (i & 8) ? 90000 : -90000;
        s.right[i] = -s.left[i];
    }
    srand(1);
    for (int i = 0; i < kRate; i += 997) {
        s.left[i] = (rand() % 200000) - 100000;
    }
    std::vector<int16_t> out = Run(s);
    int over = 0, clipped = 0;
    for (int16_t v : out) {
        over += std::abs((int)v) > (int)Limiter::kCeiling + 1;
        clipped += v == 32767 || v == -32768;
    }
    EXPECT(over == 0, "%d samples past the ceiling", over);
    EXPECT(clipped == 0, "%d samples clipped", clipped);
}

void Linked() {
    std::printf("stereo: one gain for both channels\n");
    Signal s = Sine(kRate / 4, 50000, 200, 0.25);
    std::vector<int16_t> out = Run(s);
    // The quiet right channel keeps its ratio to the loud left one.
    int bad = 0;
    for (size_t i = kDelay; i < s.left.size(); i++) {
        int l = out[i * 2], r = out[i * 2 + 1];
        if (std::abs(l) > 4000 && std::abs(r - l * 0.25) > 2) {
            bad++;
        }
    }
    EXPECT(bad == 0, "%d frames with the channels turned down differently", bad);
}

void AttackAndRelease() {
    std::printf("timing: down before the peak, back up over about 80 ms\n");
    Signal s;
    s.left.assign(2 * kRate, 1000);
    s.right.assign(2 * kRate, 1000);
    int peak = kRate / 4;
    s.left[peak] = s.right[peak] = 61868;  // needs a gain of 1/2
    std::vector<float> gains;
    std::vector<int16_t> out = Run(s, &gains);
    // The peak comes out kDelay frames later, at no more than the ceiling.
    EXPECT(std::abs(out[(peak + kDelay) * 2]) <= (int)Limiter::kCeiling + 1, "peak came out at %d",
           out[(peak + kDelay) * 2]);
    // Nothing turned down before the lookahead saw it.
    EXPECT(gains[peak - 1] == 1.0f, "gain %f before the peak", gains[peak - 1]);
    // About 63% of the way back after one release time constant, all the way after a few.
    int release = (int)(Limiter::kReleaseSeconds * kRate);
    float half = 0.5f, mid = gains[peak + kDelay + release];
    EXPECT(mid > half + 0.25f && mid < half + 0.4f, "gain %f one time constant on", mid);
    EXPECT(gains[peak + kDelay + 12 * release] == 1.0f, "gain %f long after", gains[peak + kDelay + 12 * release]);
    // And then exact again.
    int last = 2 * kRate - 1;
    EXPECT(out[last * 2] == 1000, "sample %d long after", out[last * 2]);
}

}  // namespace

int main() {
    QuietIsUnchanged();
    LoudStaysUnderCeiling();
    Linked();
    AttackAndRelease();
    if (g_failures != 0) {
        std::printf("%d failures\n", g_failures);
        return 1;
    }
    std::printf("all passed\n");
    return 0;
}
