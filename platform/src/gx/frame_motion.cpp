// How a frame drawn between the game's moves (frame_motion.h).
//
// A camera is its world-to-eye rotation R and translation t (eye = R w + t);
// its position is -R^T t, and its orientation in the world M = R^T. The game
// is Z-up: yaw turns about the world's Z, pitch about the eye's X (right).

#include "frame_motion.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace frontline::gx {
namespace {

struct Mat3 {
    double m[3][3];
};

Mat3 Mul(const Mat3& a, const Mat3& b) {
    Mat3 r;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
        }
    }
    return r;
}

Mat3 Transpose(const Mat3& a) {
    Mat3 r;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            r.m[i][j] = a.m[j][i];
        }
    }
    return r;
}

void MulVec(const Mat3& a, const double v[3], double out[3]) {
    for (int i = 0; i < 3; i++) {
        out[i] = a.m[i][0] * v[0] + a.m[i][1] * v[1] + a.m[i][2] * v[2];
    }
}

Mat3 Rotation(const FrameView& v) {
    Mat3 r;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            r.m[i][j] = v.view[i][j];
        }
    }
    return r;
}

// The camera's position in the world.
void Position(const FrameView& v, double out[3]) {
    const double t[3] = {v.view[0][3], v.view[1][3], v.view[2][3]};
    MulVec(Transpose(Rotation(v)), t, out);
    out[0] = -out[0], out[1] = -out[1], out[2] = -out[2];
}

// A rotation's angle (radians) and unit axis.
double AxisAngle(const Mat3& r, double axis[3]) {
    const double c = std::clamp((r.m[0][0] + r.m[1][1] + r.m[2][2] - 1.0) * 0.5, -1.0, 1.0);
    const double angle = std::acos(c);
    axis[0] = r.m[2][1] - r.m[1][2];
    axis[1] = r.m[0][2] - r.m[2][0];
    axis[2] = r.m[1][0] - r.m[0][1];
    const double len = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (len < 1e-12) {
        axis[0] = 0, axis[1] = 0, axis[2] = 1;
        return 0.0;  // (a half turn's axis is lost too; the limits never carry one on)
    }
    axis[0] /= len, axis[1] /= len, axis[2] /= len;
    return angle;
}

// The rotation by `angle` about the unit `axis` (Rodrigues).
Mat3 AxisRotation(const double axis[3], double angle) {
    const double c = std::cos(angle), s = std::sin(angle), k = 1.0 - c;
    const double x = axis[0], y = axis[1], z = axis[2];
    return {{{c + x * x * k, x * y * k - z * s, x * z * k + y * s},
             {y * x * k + z * s, c + y * y * k, y * z * k - x * s},
             {z * x * k - y * s, z * y * k + x * s, c + z * z * k}}};
}

double Length(const float v[3]) { return std::sqrt((double)v[0] * v[0] + (double)v[1] * v[1] + (double)v[2] * v[2]); }

// Whether change `b` carries on the way change `a` went: within 60 degrees of
// it, or `a` too small to say.
bool Continues(const float a[3], const float b[3]) {
    const double la = Length(a), lb = Length(b);
    if (la < 1e-6) {
        return true;
    }
    return (double)a[0] * b[0] + (double)a[1] * b[1] + (double)a[2] * b[2] > 0.5 * la * lb;
}

// The retraces between `prev`'s camera and `now`'s: the game's step after
// `prev`, else the frames' ends.
uint32_t Retraces(const FrameView& now, const FrameView& prev) {
    if (prev.step != 0) {
        return prev.step;
    }
    return std::max(1u, now.retrace - prev.retrace);
}

void SetMove(float out[3][4], const Mat3& r, const double t[3]) {
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            out[i][j] = (float)r.m[i][j];
        }
        out[i][3] = (float)t[i];
    }
}

}  // namespace

void FrameChange(const FrameView& prev, FrameView& now) {
    now.changed = prev.valid && now.valid;
    std::memset(now.turn, 0, sizeof(now.turn));
    std::memset(now.shift, 0, sizeof(now.shift));
    if (!now.changed) {
        return;
    }
    double axis[3], cNow[3], cPrev[3];
    const double angle = AxisAngle(Mul(Transpose(Rotation(now)), Rotation(prev)), axis);  // M_now M_prev^T
    Position(now, cNow);
    Position(prev, cPrev);
    for (int i = 0; i < 3; i++) {
        now.turn[i] = (float)(axis[i] * angle);
        now.shift[i] = (float)(cNow[i] - cPrev[i]);
    }
}

double TurnRate(const FrameView& now, const FrameView& prev) {
    if (!now.valid || !prev.valid) {
        return 0.0;
    }
    double axis[3];
    const double angle = AxisAngle(Mul(Transpose(Rotation(now)), Rotation(prev)), axis);
    return angle / Retraces(now, prev);
}

bool FrameMotion(const FrameView& now, const FrameView& prev, double ticks, const LookTurn* look,
                 const MotionLimits& limits, ReplayMotion& out, FrameView* shown) {
    std::memset(&out, 0, sizeof(out));
    if (shown != nullptr) {
        *shown = now;
    }
    if (!now.valid || (now.flags & kViewOnGun) != 0) {
        return false;
    }
    const Mat3 rNow = Rotation(now);
    double cNow[3];
    Position(now, cNow);

    // How far on, in frames: the last change took the game's step after
    // `prev`, in retraces.
    const uint32_t retraces = prev.valid ? Retraces(now, prev) : 1;
    double s = std::min(std::max(ticks, 0.0), limits.maxAhead) / retraces;
    if (limits.interpolate) {
        s -= 1.0;
    }
    const bool carried = prev.valid && now.changed && (now.flags & (kViewAttached | kViewOnGun)) == 0 &&
                         (prev.flags & (kViewAttached | kViewOnGun)) == 0;
    const bool looking = look != nullptr && (now.flags & kViewLook) != 0;

    double c[3] = {cNow[0], cNow[1], cNow[2]};
    Mat3 m = Transpose(rNow);  // the camera's orientation in the world
    if (carried && s != 0.0) {
        const double step = Length(now.shift);
        if (step <= limits.maxStep * retraces) {
            // Carried on only while it keeps going the same way: a reversal
            // (head bob, a stop) isn't thrown on past where the game drew it.
            if (!prev.changed || limits.interpolate || Continues(prev.shift, now.shift)) {
                for (int i = 0; i < 3; i++) {
                    c[i] += s * now.shift[i];
                }
            }
            // The turn from the last frame, in the world, carried on likewise
            // (not camera shake), unless the mouse turns the view: then the
            // mouse says how.
            const double angle = Length(now.turn);
            if (!looking && angle > limits.minTurn * retraces && angle <= limits.maxTurn * retraces &&
                (!prev.changed || limits.interpolate || Continues(prev.turn, now.turn))) {
                const double axis[3] = {now.turn[0] / angle, now.turn[1] / angle, now.turn[2] / angle};
                m = Mul(AxisRotation(axis, angle * s), m);
            }
        }
    }
    if (looking) {
        // Yaw about the world's up, then pitch about the eye's right.
        const double up[3] = {0.0, 0.0, 1.0}, right[3] = {1.0, 0.0, 0.0};
        const double pitch = std::clamp((double)look->pitch, (double)look->pitchMin, (double)look->pitchMax);
        m = Mul(Mul(AxisRotation(up, look->yaw), m), AxisRotation(right, pitch));
    }

    // The new world to eye, and the move from the frame's eye space to it.
    const Mat3 r = Transpose(m);
    double t[3];
    MulVec(r, c, t);
    t[0] = -t[0], t[1] = -t[1], t[2] = -t[2];
    if (shown != nullptr) {
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                shown->view[i][j] = (float)r.m[i][j];
            }
            shown->view[i][3] = (float)t[i];
        }
    }
    const Mat3 turn = Mul(r, Transpose(rNow));
    const double tNow[3] = {now.view[0][3], now.view[1][3], now.view[2][3]};
    double moved[3];
    MulVec(turn, tNow, moved);
    const double shift[3] = {t[0] - moved[0], t[1] - moved[1], t[2] - moved[2]};
    const double none[3] = {0.0, 0.0, 0.0};

    // The zoom (scope, FOV changes) carried on too.
    float zoom[2] = {1.0f, 1.0f};
    if (carried && s != 0.0) {
        for (int i = 0; i < 2; i++) {
            if (now.scale[i] > 0.0f && prev.scale[i] > 0.0f) {
                const double scale = now.scale[i] + s * (now.scale[i] - prev.scale[i]);
                zoom[i] = (float)std::clamp(scale / now.scale[i], 0.5, 2.0);
            }
        }
    }

    double axis[3];
    const double turned = AxisAngle(turn, axis);
    const double shifted = std::sqrt(shift[0] * shift[0] + shift[1] * shift[1] + shift[2] * shift[2]);
    if (turned < 1e-7 && shifted < 1e-6 && zoom[0] == 1.0f && zoom[1] == 1.0f) {
        return false;
    }
    const int kWorld = 0, kSky = 1;  // DrawClass
    out.moves[kWorld] = out.moves[kSky] = true;
    SetMove(out.move[kWorld], turn, shift);
    SetMove(out.move[kSky], turn, none);
    for (int k : {kWorld, kSky}) {
        out.zoom[k][0] = zoom[0];
        out.zoom[k][1] = zoom[1];
    }
    return true;
}

}  // namespace frontline::gx
