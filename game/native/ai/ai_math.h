// Rewritten's enemy AI: the geometry it uses, without the game, so
// tests/aitest can check it.
#ifndef FRONTLINE_GAME_NATIVE_AI_MATH_H
#define FRONTLINE_GAME_NATIVE_AI_MATH_H

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace nai {

struct Vec3 {
    float x, y, z;
    Vec3() : x(0), y(0), z(0) {}
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator*(float k) const { return Vec3(x * k, y * k, z * k); }
    Vec3& operator+=(const Vec3& o) {
        x += o.x;
        y += o.y;
        z += o.z;
        return *this;
    }
};

inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float Length(const Vec3& v) { return sqrtf(Dot(v, v)); }
inline float LengthXY(const Vec3& v) { return sqrtf(v.x * v.x + v.y * v.y); }
inline float DistanceXY(const Vec3& a, const Vec3& b) { return LengthXY(b - a); }
inline Vec3 Up(const Vec3& v, float dz) { return Vec3(v.x, v.y, v.z + dz); }

inline Vec3 Normalized(const Vec3& v) {
    float len = Length(v);
    return len > 0.0f ? v * (1.0f / len) : v;
}

// Distance from `p` to the segment a-b; `*along` is how far along it the
// nearest point is (0 at a, 1 at b).
inline float SegmentDistance(const Vec3& a, const Vec3& b, const Vec3& p, float* along) {
    Vec3 ab = b - a;
    float len2 = Dot(ab, ab);
    float t = len2 > 0.0f ? Dot(p - a, ab) / len2 : 0.0f;
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    if (along != NULL) {
        *along = t;
    }
    return Length(p - (a + ab * t));
}

// Grenades, as CThrownBullet flies them: velocity per frame, gravity per
// frame squared.
const float kGrenadeGravity = 0.005455351f;
const float kTan30 = 0.577f;  // the slope soldiers throw at originally (weapon.cpp)

// The original solver (CAIObject::TestGrenadeShot) gives the speed of a
// 30-degree throw. For the same range at another launch slope, flat ground,
// the speed scales by sqrt(sin 60 / sin 2a).
inline float GrenadeSpeedFactor(float slope) {
    float a = atanf(slope);
    float s = sinf(2.0f * a);
    return s > 0.01f ? sqrtf(0.8660254f / s) : 0.0f;
}

// A point of the arc leaving `from` at `slope` and passing through `to`, at
// fraction `s` of the horizontal distance. Valid only when ArcPossible.
inline Vec3 ArcPoint(const Vec3& from, const Vec3& to, float slope, float s) {
    Vec3 d = to - from;
    float range = LengthXY(d);
    float rise = range * slope;
    return Vec3(from.x + d.x * s, from.y + d.y * s, from.z + s * rise - s * s * (rise - d.z));
}

// Whether a throw at `slope` can reach `to` (the arc rises at least as
// steeply as the straight line, with some room).
inline bool ArcPossible(const Vec3& from, const Vec3& to, float slope) {
    Vec3 d = to - from;
    return LengthXY(d) * slope > d.z + 0.25f;
}

// A grenade's position and velocity `frames` later, stepped as
// CThrownBullet::AttemptUpdate does (gravity, then the move), without
// collisions.
inline void StepGrenade(Vec3* pos, Vec3* vel, int frames) {
    for (int i = 0; i < frames; i++) {
        vel->z -= kGrenadeGravity;
        *pos += *vel;
    }
}

// "name=value,name=value": calls `set` for each pair (skipping malformed
// ones); returns how many were read.
template <typename F>
inline int ParseTunes(const char* text, F set) {
    int count = 0;
    while (text != NULL && *text != 0) {
        const char* comma = strchr(text, ',');
        const char* end = comma != NULL ? comma : text + strlen(text);
        const char* eq = (const char*)memchr(text, '=', (size_t)(end - text));
        if (eq != NULL && eq > text) {
            char name[32];
            size_t len = (size_t)(eq - text);
            if (len >= sizeof(name)) {
                len = sizeof(name) - 1;
            }
            memcpy(name, text, len);
            name[len] = 0;
            char* parsed = NULL;
            float value = strtof(eq + 1, &parsed);
            if (parsed != eq + 1 && parsed <= end) {
                set(name, value);
                count++;
            }
        }
        text = comma != NULL ? comma + 1 : NULL;
    }
    return count;
}

}  // namespace nai

#endif
