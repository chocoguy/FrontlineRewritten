// Checks Rewritten's made mipmaps (platform/src/gx/gx_texture.h) without the
// game:
//   build/ninja/tests/textest/frontline_textest

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "gx_texture.h"

namespace {

using namespace frontline::gx;

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

struct Rgba {
    uint8_t r, g, b, a;
};

// A `size` x `size` texture from a function of (x, y).
template <typename F>
std::vector<uint8_t> Make(uint32_t size, F texel) {
    std::vector<uint8_t> out(size * size * 4);
    for (uint32_t y = 0; y < size; y++) {
        for (uint32_t x = 0; x < size; x++) {
            const Rgba c = texel(x, y);
            uint8_t* p = &out[(y * size + x) * 4];
            p[0] = c.r, p[1] = c.g, p[2] = c.b, p[3] = c.a;
        }
    }
    return out;
}

std::vector<uint8_t> Down(const std::vector<uint8_t>& src, uint32_t size) {
    std::vector<uint8_t> out((size / 2) * (size / 2) * 4);
    Downsample(src.data(), size, size, out.data());
    return out;
}

// A level stands for the light its texels send, not the average of their
// stored colors.
void TestLight() {
    std::printf("light\n");
    const auto checks = Make(8, [](uint32_t x, uint32_t y) { return (x + y) & 1 ? Rgba{255, 255, 255, 255} : Rgba{0, 0, 0, 255}; });
    const auto half = Down(checks, 8);
    for (uint32_t i = 0; i < 16; i++) {
        EXPECT(half[i * 4] == 188 && half[i * 4 + 1] == 188 && half[i * 4 + 2] == 188, "texel %u is %u", i, half[i * 4]);
        EXPECT(half[i * 4 + 3] == 255, "alpha %u", half[i * 4 + 3]);
    }
    // Every flat color comes through as it was.
    for (int v = 0; v < 256; v++) {
        const auto flat = Make(2, [v](uint32_t, uint32_t) { return Rgba{(uint8_t)v, (uint8_t)(255 - v), 7, 255}; });
        const auto one = Down(flat, 2);
        EXPECT(one[0] == v && one[1] == 255 - v && one[2] == 7, "%d became %u", v, one[0]);
    }
    // A gradient's levels stay inside the texels they cover.
    const auto ramp = Make(4, [](uint32_t x, uint32_t) { return Rgba{(uint8_t)(x * 60), 0, 0, 255}; });
    const auto ramped = Down(ramp, 4);
    EXPECT(ramped[0] > 30 && ramped[0] <= 60, "%u", ramped[0]);
    EXPECT(ramped[4] > 150 && ramped[4] <= 180, "%u", ramped[4]);
}

// See-through texels' colors don't bleed into the ones beside them.
void TestAlpha() {
    std::printf("alpha\n");
    const auto edge = Make(2, [](uint32_t x, uint32_t) { return x == 0 ? Rgba{200, 100, 50, 255} : Rgba{0, 255, 0, 0}; });
    const auto one = Down(edge, 2);
    EXPECT(one[0] == 200 && one[1] == 100 && one[2] == 50, "%u %u %u", one[0], one[1], one[2]);
    EXPECT(one[3] == 128, "alpha %u", one[3]);
    // All see-through: still an average, in light.
    const auto clear = Make(2, [](uint32_t x, uint32_t) { return x == 0 ? Rgba{255, 0, 0, 0} : Rgba{0, 0, 0, 0}; });
    const auto none = Down(clear, 2);
    EXPECT(none[0] == 188 && none[3] == 0, "%u %u", none[0], none[3]);
}

// An alpha-tested texture keeps the share of texels that pass the test.
void TestCoverage() {
    std::printf("coverage\n");
    // A fence: one texel in four is solid.
    const auto fence = Make(16, [](uint32_t x, uint32_t) { return x % 4 == 0 ? Rgba{90, 90, 90, 255} : Rgba{90, 90, 90, 0}; });
    const float coverage = AlphaTestCoverage(fence.data(), 16 * 16);
    EXPECT(coverage > 0.24f && coverage < 0.26f, "%f", coverage);
    auto half = Down(fence, 16);
    KeepCoverage(half.data(), 8 * 8, coverage);
    int passing = 0;
    for (uint32_t i = 0; i < 64; i++) {
        passing += half[i * 4 + 3] > 160;
    }
    EXPECT(passing >= 16, "%d of 64 pass", passing);
    // A blended texture isn't one.
    const auto glass = Make(4, [](uint32_t, uint32_t) { return Rgba{255, 255, 255, 128}; });
    EXPECT(AlphaTestCoverage(glass.data(), 16) < 0.0f, "blended");
}

}  // namespace

int main() {
    TestLight();
    TestAlpha();
    TestCoverage();
    std::printf(g_failures == 0 ? "ok\n" : "%d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
