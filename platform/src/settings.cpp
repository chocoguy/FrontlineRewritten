// The run's settings: Classic or Rewritten, and Rewritten's picture (render
// resolution, widescreen, high frame rate, MSAA and draw distance). The app sets them from its
// setup screen before the game starts; environment variables override them,
// for headless runs. Rewritten's field of view comes from the app's Settings
// and can change while the game runs.

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "frontline/platform.h"
#include "native/hooks.h"

namespace {

int g_mode = NATIVE_MODE_CLASSIC;
int g_lines = 480;
float g_aspect;
bool g_highFrameRate;
int g_msaa = 1;
float g_drawDistance = 1.0f;
// Degrees across at 4:3; the app's Settings change it while the game runs.
std::atomic<float> g_fieldOfView{70.0f};

// FRONTLINE_FOV=<degrees across>, or 0.
float FieldOfViewOverride() {
    static const float degrees = [] {
        const char* fov = std::getenv("FRONTLINE_FOV");
        return fov != nullptr ? (float)std::atof(fov) : 0.0f;
    }();
    return degrees;
}
bool g_overridden;

// FRONTLINE_MODE, FRONTLINE_RESOLUTION, FRONTLINE_WIDESCREEN, FRONTLINE_HFR,
// FRONTLINE_MSAA, FRONTLINE_DRAW_DISTANCE, once, over whatever the app set.
void ApplyOverrides() {
    if (g_overridden) {
        return;
    }
    g_overridden = true;
    if (const char* mode = std::getenv("FRONTLINE_MODE")) {
        g_mode = std::strcmp(mode, "rewritten") == 0 ? NATIVE_MODE_REWRITTEN : NATIVE_MODE_CLASSIC;
    }
    if (const char* lines = std::getenv("FRONTLINE_RESOLUTION")) {
        g_lines = std::atoi(lines);
    }
    if (const char* aspect = std::getenv("FRONTLINE_WIDESCREEN")) {
        float w = 0, h = 0;
        g_aspect = std::sscanf(aspect, "%f:%f", &w, &h) == 2 && h > 0 ? w / h : 0;
    }
    if (const char* hfr = std::getenv("FRONTLINE_HFR")) {
        g_highFrameRate = std::atoi(hfr) != 0;
    }
    if (const char* msaa = std::getenv("FRONTLINE_MSAA")) {
        g_msaa = std::atoi(msaa);
    }
    if (const char* distance = std::getenv("FRONTLINE_DRAW_DISTANCE")) {
        g_drawDistance = (float)std::atof(distance);
    }
}

bool Rewritten() {
    ApplyOverrides();
    return g_mode == NATIVE_MODE_REWRITTEN;
}

}  // namespace

void frontline::SetGameMode(int mode) { g_mode = mode; }
void frontline::SetRenderLines(int lines) { g_lines = lines; }
void frontline::SetWidescreen(float aspect) { g_aspect = aspect; }
void frontline::SetHighFrameRate(bool on) { g_highFrameRate = on; }
void frontline::SetMsaa(int samples) { g_msaa = samples; }
void frontline::SetDrawDistance(float times) { g_drawDistance = times; }

void frontline::SetFieldOfView(float degrees) { g_fieldOfView.store(degrees, std::memory_order_relaxed); }

bool frontline::HighFrameRate() { return Rewritten() && g_highFrameRate; }

int frontline::MsaaSamples() { return Rewritten() && g_msaa >= 4 ? 4 : 1; }

float frontline::RenderScale() {
    if (!Rewritten() || g_lines <= 480) {
        return 1.0f;
    }
    return (g_lines > 2160 ? 2160 : g_lines) / 480.0f;
}

extern "C" int NativeGameMode(void) { return Rewritten() ? NATIVE_MODE_REWRITTEN : NATIVE_MODE_CLASSIC; }

// The aspect's width on the 480-line screen, to a multiple of 16 (the frame
// buffer's stride), between 4:3 and about 21:9.
extern "C" int NativeRenderWidth(void) {
    if (!Rewritten() || g_aspect <= 4.0f / 3.0f) {
        return 640;
    }
    int width = (int)std::lround(480 * g_aspect / 16) * 16;
    return width < 640 ? 640 : width > 1136 ? 1136 : width;
}

extern "C" int NativeHighFrameRate(void) { return frontline::HighFrameRate() ? 1 : 0; }

extern "C" float NativeBaseFov(void) {
    if (!Rewritten()) {
        return 35.0f;
    }
    float degrees = FieldOfViewOverride() > 0 ? FieldOfViewOverride() : g_fieldOfView.load(std::memory_order_relaxed);
    return 0.5f * std::fmin(std::fmax(degrees, frontline::kMinFieldOfView), frontline::kMaxFieldOfView);
}

extern "C" float NativeDrawDistance(void) {
    if (!Rewritten() || !(g_drawDistance > 1.0f)) {
        return 1.0f;
    }
    return g_drawDistance < 4.0f ? g_drawDistance : 4.0f;
}
