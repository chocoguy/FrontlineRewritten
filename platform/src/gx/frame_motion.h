// How a frame drawn between the game's moves (Rewritten's high frame rate,
// gx_record.h): its camera moved on from where the game's frame drew it, as
// one matrix per class of draw. Plain math, tested without the game
// (tests/hfrtest).
#pragma once

#include <cstdint>

namespace frontline::gx {

// How a replay moves each class of draw (gx_record.h DrawClass): positions in
// eye space through `move` (3x4 rows), and the projection's x and y scales
// times `zoom`. A class that doesn't move draws its positions as recorded.
constexpr int kMotionClasses = 4;
struct ReplayMotion {
    bool moves[kMotionClasses];
    float move[kMotionClasses][3][4];
    float zoom[kMotionClasses][2];
};

// The camera a game frame was drawn with (NativeFrameView).
struct FrameView {
    bool valid;        // one scene camera in the frame (not split screen, not a menu)
    uint32_t flags;    // NATIVE_VIEW_*
    uint32_t retrace;  // the retrace the frame ended at
    // Retraces the game stepped after drawing this camera: the update that
    // moves it to the next frame's (NativePlayerView; 0: not known).
    uint32_t step;
    float view[3][4];  // world to eye: eye = view * (world, 1)
    float scale[2];    // the projection's x and y scales
    // How the mouse turns it (NativePlayerView): the game's scale for the
    // mouse, its pitch, and the first of the input takes not drawn in it.
    float lookScale, pitch;
    uint32_t lookFrom;
    // How it changed from the frame before (FrameChange): the turn in the
    // world (axis times radians) and the move (meters).
    bool changed;
    float turn[3], shift[3];
    // How far past it, left and right and up and down, the game culled for
    // (radians; NativeCullTan): drawn turned further, its edges show gaps.
    float cullWiden[2];
};

// The mouse movement the game hasn't drawn yet, to turn the camera by now
// (radians; the game's yaw and pitch, as it will add them).
struct LookTurn {
    float yaw, pitch;
    float pitchMin, pitchMax;  // the game's limits, around the current pitch
};

struct MotionLimits {
    double maxStep = 1.5;           // meters a retrace: past it the camera jumped (a cut)
    double maxTurn = 0.6;           // radians a retrace: past it the turn isn't carried on
    double minTurn = 0.004;         // radians a retrace: slower isn't carried on (camera shake)
    double maxAhead = 1.5;          // retraces past the frame at most
    bool interpolate = false;       // between the last two frames instead (a retrace behind)
};

// Sets `now`'s change from `prev` (as a frame ends).
void FrameChange(const FrameView& prev, FrameView& now);

// The motion that takes frame `now`'s draws to its camera `ticks` retraces
// later, plus `look` (may be null). The camera carries on at the speed of its
// change from `prev`, as long as the change keeps the direction of the one
// before it (so camera shake and head bob aren't thrown further). False if
// nothing moves. `shown` (may be null) gets the camera drawn.
bool FrameMotion(const FrameView& now, const FrameView& prev, double ticks, const LookTurn* look,
                 const MotionLimits& limits, ReplayMotion& out, FrameView* shown = nullptr);

// How much a camera turns a retrace from `prev` to `now`, in radians (0 if
// either isn't valid).
double TurnRate(const FrameView& now, const FrameView& prev);

// Flags for FrameView (the game's NATIVE_VIEW_*).
enum : uint32_t {
    kViewAttached = 1,  // carried by something: drawn as the game drew it, but for the mouse
    kViewOnGun = 2,     // on a mounted gun, which turns with the view: as the game drew it
    kViewLook = 4,      // the mouse turns the view (LookTurn applies)
};

}  // namespace frontline::gx
