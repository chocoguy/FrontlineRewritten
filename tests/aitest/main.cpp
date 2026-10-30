// Checks the geometry of Rewritten's enemy AI (game/native/ai/ai_math.h)
// without the game:
//   build/ninja/tests/aitest/frontline_aitest

#include <cmath>
#include <cstdio>
#include <cstring>

#include "ai_math.h"

namespace {

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

using nai::Vec3;

// Where a grenade thrown along `slope` at `speed` (per frame) from the
// origin comes back down to its start height, stepped as the game steps it.
float FlatRange(float slope, float speed) {
    float angle = std::atan(slope);
    Vec3 pos;
    Vec3 vel(speed * std::cos(angle), 0.0f, speed * std::sin(angle));
    for (int frame = 0; frame < 10000; frame++) {
        Vec3 prev = pos;
        nai::StepGrenade(&pos, &vel, 1);
        if (pos.z < 0.0f) {
            float f = prev.z / (prev.z - pos.z);
            return prev.x + (pos.x - prev.x) * f;
        }
    }
    return -1.0f;
}

// The original solver's launch speed for a 30-degree throw to `dist`
// (CAIObject::TestGrenadeShot, before its x600 power scale).
float OriginalSpeed(float dist) { return std::sqrt(0.95f * dist * 0.0062994817f); }

void TestOriginalSolver() {
    std::printf("original 30-degree solver\n");
    // It aims 5% short (the grenade rolls on).
    for (float dist = 10.0f; dist <= 40.0f; dist += 10.0f) {
        float range = FlatRange(nai::kTan30, OriginalSpeed(dist));
        EXPECT(std::fabs(range - 0.95f * dist) < 0.05f * dist, "dist %.0f lands at %.2f", dist, range);
    }
}

void TestOtherAngles() {
    std::printf("45 and 20 degrees reach the same spot\n");
    EXPECT(std::fabs(nai::GrenadeSpeedFactor(nai::kTan30) - 1.0f) < 0.002f, "30-degree factor %.4f",
           nai::GrenadeSpeedFactor(nai::kTan30));
    const float slopes[2] = {1.0f, 0.364f};
    for (float slope : slopes) {
        for (float dist = 10.0f; dist <= 40.0f; dist += 10.0f) {
            float speed = OriginalSpeed(dist) * nai::GrenadeSpeedFactor(slope) / nai::GrenadeSpeedFactor(nai::kTan30);
            float want = FlatRange(nai::kTan30, OriginalSpeed(dist));
            float range = FlatRange(slope, speed);
            EXPECT(std::fabs(range - want) < 0.04f * want, "slope %.3f dist %.0f lands at %.2f, not %.2f", slope,
                   dist, range, want);
        }
    }
}

void TestArc() {
    std::printf("arc points\n");
    Vec3 from(1.0f, 2.0f, 0.5f);
    Vec3 to(13.0f, 7.0f, 1.5f);
    Vec3 a = nai::ArcPoint(from, to, 1.0f, 0.0f);
    Vec3 b = nai::ArcPoint(from, to, 1.0f, 1.0f);
    EXPECT(nai::Length(a - from) < 1e-4f, "starts at (%.2f %.2f %.2f)", a.x, a.y, a.z);
    EXPECT(nai::Length(b - to) < 1e-4f, "ends at (%.2f %.2f %.2f)", b.x, b.y, b.z);
    // The slope leaving `from` is the launch slope.
    Vec3 c = nai::ArcPoint(from, to, 1.0f, 0.001f);
    float slope = (c.z - from.z) / nai::DistanceXY(from, c);
    EXPECT(std::fabs(slope - 1.0f) < 0.01f, "leaves at slope %.3f", slope);
    // It rises above the straight line.
    Vec3 mid = nai::ArcPoint(from, to, 1.0f, 0.5f);
    EXPECT(mid.z > 0.5f * (from.z + to.z) + 1.0f, "middle at %.2f", mid.z);
    EXPECT(nai::ArcPossible(from, to, 0.364f), "a 20-degree throw slightly upward");
    EXPECT(!nai::ArcPossible(Vec3(), Vec3(10.0f, 0.0f, 9.0f), 0.577f), "not steeper than the arc");
}

void TestSegmentDistance() {
    std::printf("segment distance\n");
    float along;
    float d = nai::SegmentDistance(Vec3(), Vec3(10.0f, 0.0f, 0.0f), Vec3(4.0f, 1.0f, 0.0f), &along);
    EXPECT(std::fabs(d - 1.0f) < 1e-5f && std::fabs(along - 0.4f) < 1e-5f, "d %.3f along %.3f", d, along);
    d = nai::SegmentDistance(Vec3(), Vec3(10.0f, 0.0f, 0.0f), Vec3(-3.0f, 4.0f, 0.0f), &along);
    EXPECT(std::fabs(d - 5.0f) < 1e-5f && along == 0.0f, "before the start: d %.3f along %.3f", d, along);
}

void TestTunes() {
    std::printf("FRONTLINE_AI_TUNE parsing\n");
    char names[4][32];
    float values[4];
    int n = 0;
    int read = nai::ParseTunes("lead=1.5,tokens=4,bad,miss=0.25", [&](const char* name, float value) {
        if (n < 4) {
            std::strcpy(names[n], name);
            values[n++] = value;
        }
    });
    EXPECT(read == 3 && n == 3, "read %d", read);
    EXPECT(n >= 2 && std::strcmp(names[0], "lead") == 0 && values[0] == 1.5f, "first %s", names[0]);
    EXPECT(n >= 2 && std::strcmp(names[1], "tokens") == 0 && values[1] == 4.0f, "second %s", names[1]);
    EXPECT(nai::ParseTunes(NULL, [](const char*, float) {}) == 0, "no text");
}

}  // namespace

int main() {
    TestOriginalSolver();
    TestOtherAngles();
    TestArc();
    TestSegmentDistance();
    TestTunes();
    if (g_failures != 0) {
        std::printf("%d failed\n", g_failures);
        return 1;
    }
    std::printf("all passed\n");
    return 0;
}
