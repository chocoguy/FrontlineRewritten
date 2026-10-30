// Checks how the high frame rate moves frames drawn between the game's
// (platform/src/gx/frame_motion.h) without the game:
//   build/ninja/tests/hfrtest/frontline_hfrtest

#include <cmath>
#include <cstdio>
#include <cstring>

#include "frame_motion.h"

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

// A camera at `pos` looking along `heading` (about the world's up, Z) and
// `elevation`, as the game's world to eye: eye x right, y up, z backward.
FrameView View(double x, double y, double z, double heading, double elevation, uint32_t retrace) {
    const double f[3] = {std::cos(elevation) * std::cos(heading), std::cos(elevation) * std::sin(heading),
                         std::sin(elevation)};
    double r[3] = {f[1], -f[0], 0.0};  // forward x up
    const double len = std::sqrt(r[0] * r[0] + r[1] * r[1]);
    r[0] /= len, r[1] /= len;
    const double u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    const double rows[3][3] = {{r[0], r[1], r[2]}, {u[0], u[1], u[2]}, {-f[0], -f[1], -f[2]}};
    const double pos[3] = {x, y, z};
    FrameView v;
    std::memset(&v, 0, sizeof(v));
    v.valid = true;
    v.retrace = retrace;
    v.scale[0] = v.scale[1] = 1.0f;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            v.view[i][j] = (float)rows[i][j];
        }
        v.view[i][3] = (float)-(rows[i][0] * pos[0] + rows[i][1] * pos[1] + rows[i][2] * pos[2]);
    }
    return v;
}

// `v` as the frame after `prev`: with its change, as a frame's end sets it.
FrameView After(const FrameView& prev, FrameView v) {
    FrameChange(prev, v);
    return v;
}

void ToEye(const float m[3][4], const double in[3], double out[3]) {
    for (int i = 0; i < 3; i++) {
        out[i] = m[i][0] * in[0] + m[i][1] * in[1] + m[i][2] * in[2] + m[i][3];
    }
}

// Whether `motion` takes world point `w`, as frame `now` saw it, to where
// `expected` sees it, for class `cls`.
bool Lands(const ReplayMotion& motion, int cls, const FrameView& now, const FrameView& expected, const double w[3]) {
    double eye[3], moved[3], want[3];
    ToEye(now.view, w, eye);
    if (motion.moves[cls]) {
        ToEye(motion.move[cls], eye, moved);
    } else {
        std::memcpy(moved, eye, sizeof(moved));
    }
    ToEye(expected.view, w, want);
    const double d = std::fabs(moved[0] - want[0]) + std::fabs(moved[1] - want[1]) + std::fabs(moved[2] - want[2]);
    if (d > 1e-3) {
        std::printf("    (%g %g %g) -> (%g %g %g), want (%g %g %g)\n", w[0], w[1], w[2], moved[0], moved[1], moved[2],
                    want[0], want[1], want[2]);
    }
    return d <= 1e-3;
}

const double kPoints[][3] = {{10, 2, 1}, {-3, 7, 0.5}, {4, -6, 3}, {0.5, 0.5, -2}};
constexpr int kWorld = 0, kSky = 1, kViewModel = 2;

void TestStill() {
    std::printf("still: nothing moves\n");
    const FrameView a = View(1, 2, 1.6, 0.3, 0.1, 100), b = After(a, View(1, 2, 1.6, 0.3, 0.1, 101));
    ReplayMotion m;
    EXPECT(!FrameMotion(b, a, 0.5, nullptr, MotionLimits(), m), "a still camera moved");
    EXPECT(!FrameMotion(b, a, 0.0, nullptr, MotionLimits(), m), "no time moved it");
}

void TestWalk() {
    std::printf("walking: carried on, sky turned only\n");
    const FrameView a = View(0, 0, 1.6, 0.4, 0.0, 100), b = After(a, View(0.08, 0.02, 1.6, 0.4, 0.0, 101));
    ReplayMotion m;
    EXPECT(FrameMotion(b, a, 0.5, nullptr, MotionLimits(), m), "no motion");
    const FrameView want = View(0.12, 0.03, 1.6, 0.4, 0.0, 0);
    for (const auto& w : kPoints) {
        EXPECT(Lands(m, kWorld, b, want, w), "world point");
    }
    EXPECT(!m.moves[kViewModel], "the weapon moved");
    // The sky doesn't move with the camera's position.
    double eye[3], moved[3];
    ToEye(b.view, kPoints[0], eye);
    ToEye(m.move[kSky], eye, moved);
    EXPECT(std::fabs(moved[0] - eye[0]) + std::fabs(moved[1] - eye[1]) + std::fabs(moved[2] - eye[2]) < 1e-5,
           "the sky moved with the camera");
}

void TestTurn() {
    std::printf("turning: carried on about the world's up\n");
    FrameView a = View(5, 5, 1.6, 1.0, 0.2, 100);
    const FrameView b = After(a, View(5, 5, 1.6, 1.05, 0.2, 101));
    ReplayMotion m;
    EXPECT(FrameMotion(b, a, 1.0, nullptr, MotionLimits(), m), "no motion");
    const FrameView want = View(5, 5, 1.6, 1.1, 0.2, 0);
    for (const auto& w : kPoints) {
        EXPECT(Lands(m, kWorld, b, want, w), "world point");
    }
    // Two retraces between frames: half the turn a retrace.
    const FrameView c = After(a, View(5, 5, 1.6, 1.05, 0.2, 102));
    EXPECT(FrameMotion(c, a, 1.0, nullptr, MotionLimits(), m), "no motion");
    const FrameView half = View(5, 5, 1.6, 1.075, 0.2, 0);
    EXPECT(Lands(m, kWorld, c, half, kPoints[0]), "slow frames");
    // The game's own step says so, whenever the frames happened to end.
    a.step = 2;
    EXPECT(FrameMotion(b, a, 1.0, nullptr, MotionLimits(), m), "no motion");
    EXPECT(Lands(m, kWorld, b, half, kPoints[1]), "the game's step");
}

void TestCut() {
    std::printf("cuts and limits\n");
    const FrameView a = View(0, 0, 1.6, 0.0, 0.0, 100), b = After(a, View(20, 0, 1.6, 2.0, 0.0, 101));
    ReplayMotion m;
    EXPECT(!FrameMotion(b, a, 0.5, nullptr, MotionLimits(), m), "a jump was carried on");
    // Far ahead (a late frame): no further than maxAhead.
    const FrameView c = After(a, View(0.1, 0, 1.6, 0.0, 0.0, 101));
    EXPECT(FrameMotion(c, a, 10.0, nullptr, MotionLimits(), m), "no motion");
    EXPECT(Lands(m, kWorld, c, View(0.25, 0, 1.6, 0.0, 0.0, 0), kPoints[1]), "past maxAhead");
    // Carried by something, or on a gun: as drawn.
    FrameView d = c;
    d.flags = kViewAttached;
    EXPECT(!FrameMotion(d, a, 0.5, nullptr, MotionLimits(), m), "attached moved");
    d.flags = kViewOnGun;
    EXPECT(!FrameMotion(d, a, 0.5, nullptr, MotionLimits(), m), "on a gun moved");
    // No frame before: nothing to carry on.
    FrameView none = a;
    none.valid = false;
    EXPECT(!FrameMotion(c, none, 0.5, nullptr, MotionLimits(), m), "moved with no last frame");
}

void TestInterpolate() {
    std::printf("interpolating: a frame behind\n");
    const FrameView a = View(0, 0, 1.6, 0.0, 0.0, 100), b = After(a, View(0.1, 0, 1.6, 0.1, 0.0, 101));
    MotionLimits limits;
    limits.interpolate = true;
    ReplayMotion m;
    EXPECT(FrameMotion(b, a, 0.0, nullptr, limits, m), "no motion");
    EXPECT(Lands(m, kWorld, b, a, kPoints[2]), "at 0 it's the last frame");
    EXPECT(!FrameMotion(b, a, 1.0, nullptr, limits, m), "at 1 it's this frame");
}

void TestLook() {
    std::printf("mouse look: turns by the mouse, not the last turn\n");
    FrameView a = View(0, 0, 1.6, 0.0, 0.1, 100), b = After(a, View(0, 0, 1.6, 0.2, 0.1, 101));
    b.flags = kViewLook;
    LookTurn look = {0.05f, 0.03f, -1.0f, 1.0f};
    ReplayMotion m;
    EXPECT(FrameMotion(b, a, 0.5, &look, MotionLimits(), m), "no motion");
    for (const auto& w : kPoints) {
        EXPECT(Lands(m, kWorld, b, View(0, 0, 1.6, 0.25, 0.13, 0), w), "world point");
    }
    // The pitch stops at the game's limit.
    look = {0.0f, 0.5f, -1.0f, 0.02f};
    EXPECT(FrameMotion(b, a, 0.5, &look, MotionLimits(), m), "no motion");
    EXPECT(Lands(m, kWorld, b, View(0, 0, 1.6, 0.2, 0.12, 0), kPoints[0]), "pitch limit");
    // Attached: still turned by the mouse, but not carried on.
    b.flags = kViewLook | kViewAttached;
    look = {0.05f, 0.0f, -1.0f, 1.0f};
    FrameView moving = After(a, View(1, 0, 1.6, 0.2, 0.1, 101));
    moving.flags = b.flags;
    EXPECT(FrameMotion(moving, a, 0.5, &look, MotionLimits(), m), "no motion");
    EXPECT(Lands(m, kWorld, moving, View(1, 0, 1.6, 0.25, 0.1, 0), kPoints[3]), "attached");
}

void TestZoom() {
    std::printf("zoom carried on\n");
    FrameView a = View(0, 0, 1.6, 0.0, 0.0, 100), b = After(a, View(0, 0, 1.6, 0.0, 0.0, 101));
    a.scale[0] = a.scale[1] = 2.0f;
    b.scale[0] = b.scale[1] = 2.5f;
    ReplayMotion m;
    EXPECT(FrameMotion(b, a, 0.5, nullptr, MotionLimits(), m), "no motion");
    EXPECT(std::fabs(m.zoom[kWorld][0] - 2.75f / 2.5f) < 1e-5 && std::fabs(m.zoom[kSky][1] - 2.75f / 2.5f) < 1e-5,
           "zoom %g", m.zoom[kWorld][0]);
}

void TestShake() {
    std::printf("shake and bob: a reversal isn't carried on\n");
    const FrameView z = View(0, 0, 1.6, 0.0, 0.0, 99);
    const FrameView a = After(z, View(0, 0, 1.65, 0.01, 0.0, 100));
    const FrameView back = After(a, View(0, 0, 1.6, 0.0, 0.0, 101));
    ReplayMotion m;
    EXPECT(!FrameMotion(back, a, 0.5, nullptr, MotionLimits(), m), "a reversal was carried on");
    // The same way again: carried on.
    const FrameView on = After(a, View(0, 0, 1.7, 0.02, 0.0, 101));
    EXPECT(FrameMotion(on, a, 0.5, nullptr, MotionLimits(), m), "no motion");
    EXPECT(Lands(m, kWorld, on, View(0, 0, 1.725, 0.025, 0.0, 0), kPoints[0]), "carried on");
    // Turning slower than camera shake: not carried on.
    const FrameView slow = After(a, View(0, 0, 1.65, 0.012, 0.0, 101));
    EXPECT(!FrameMotion(slow, a, 0.5, nullptr, MotionLimits(), m), "a shake-sized turn was carried on");
    // A reversal in turning only: the move still goes on.
    const FrameView turnBack = After(a, View(0, 0, 1.7, 0.0, 0.0, 101));
    EXPECT(FrameMotion(turnBack, a, 0.5, nullptr, MotionLimits(), m), "no motion");
    EXPECT(Lands(m, kWorld, turnBack, View(0, 0, 1.725, 0.0, 0.0, 0), kPoints[2]), "the move alone");
}

}  // namespace

int main() {
    TestStill();
    TestWalk();
    TestTurn();
    TestCut();
    TestInterpolate();
    TestLook();
    TestZoom();
    TestShake();
    if (g_failures != 0) {
        std::printf("%d failed\n", g_failures);
        return 1;
    }
    std::printf("all passed\n");
    return 0;
}
