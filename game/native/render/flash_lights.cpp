// Rewritten's flash lighting (native/lighting.h): gunfire and explosions
// light what's around them for a moment.
//
// The game has dynamic point lights (CLight): the world bin lights each
// compartment node with up to three of them over its baked color, and
// characters and props take the strongest (or, with corrected lighting, all
// of them per pixel). Levels place a few as lamps and fires, but nothing
// makes one for a shot or a blast. Here each gets one of the game's own
// instanced lights, playing a short animation that fades out and ends the
// light (CPropertyAnimLight::BeginUpdate, ANIMLIGHT_ONCE). They come from
// the instanced pool (10), so the scene's light list can't grow past what
// it was sized for; with the pool full a flash simply has no light.

#include <stdlib.h>

#include "game/light.h"
#include "game/matrix.h"
#include "game/vector.h"
#include "native/hooks.h"
#include "native/lighting.h"

namespace {

// A light's animation: a color (0..255) and a strength a frame.
enum { kMuzzleFrames = 4, kBlastFrames = 26 };
AnimLightColor g_muzzleColors[kMuzzleFrames];
f32 g_muzzleStrengths[kMuzzleFrames];
AnimLightColor g_blastColors[kBlastFrames];
f32 g_blastStrengths[kBlastFrames];
MOH_animatedLight_Struct g_muzzle, g_blast;
bool g_made;

// World units. A soldier stands about 3 tall.
const f32 kMuzzleRadius = 7.0f;
const f32 kMuzzleAhead = 0.6f;   // in front of the muzzle, so the gun itself is lit from ahead
const f32 kPlayerAhead = 1.6f;   // the player's shots leave from the eye
const f32 kBlastRadiusTimes = 3.0f, kBlastRadiusMin = 10.0f, kBlastRadiusMax = 36.0f;
const f32 kBlastRise = 0.8f;     // above the ground it went off on

void Animation(MOH_animatedLight_Struct& info, AnimLightColor* colors, f32* strengths, int frames, f32 radius) {
    info.mode = ANIMLIGHT_ONCE;
    info.numFrames = (s16)frames;
    info.radius = (s16)(radius * 16.0f);
    info.colors = colors;
    info.intensities = strengths;
}

void Make() {
    if (g_made) {
        return;
    }
    g_made = true;
    // A shot: a warm white that is gone in four frames.
    static const f32 muzzle[kMuzzleFrames] = {1.0f, 0.7f, 0.35f, 0.12f};
    for (int i = 0; i < kMuzzleFrames; i++) {
        g_muzzleColors[i][0] = 255, g_muzzleColors[i][1] = 208, g_muzzleColors[i][2] = 140, g_muzzleColors[i][3] = 255;
        g_muzzleStrengths[i] = muzzle[i];
    }
    Animation(g_muzzle, g_muzzleColors, g_muzzleStrengths, kMuzzleFrames, kMuzzleRadius);
    // FRONTLINE_FLASH_HOLD=<frames>: a shot's light stays at full strength
    // that long, to catch it in a frame dump.
    if (const char* hold = getenv("FRONTLINE_FLASH_HOLD")) {
        static AnimLightColor colors[600];
        static f32 strengths[600];
        int frames = atoi(hold);
        frames = frames < 1 ? 1 : frames > 600 ? 600 : frames;
        for (int i = 0; i < frames; i++) {
            for (int c = 0; c < 4; c++) {
                colors[i][c] = g_muzzleColors[0][c];
            }
            strengths[i] = 1.0f;
        }
        Animation(g_muzzle, colors, strengths, frames, kMuzzleRadius);
    }
    // A blast: white-hot for two frames, then orange dying away to red.
    for (int i = 0; i < kBlastFrames; i++) {
        const f32 t = (f32)i / (kBlastFrames - 1);
        g_blastColors[i][0] = 255;
        g_blastColors[i][1] = (u8)(i < 2 ? 236 : 190.0f - 90.0f * t);
        g_blastColors[i][2] = (u8)(i < 2 ? 200 : 110.0f - 90.0f * t);
        g_blastColors[i][3] = 255;
        g_blastStrengths[i] = i < 2 ? 1.0f : (1.0f - t) * (1.0f - t) * 0.9f;
    }
    Animation(g_blast, g_blastColors, g_blastStrengths, kBlastFrames, kBlastRadiusMin);
}

}  // namespace

void NativeMuzzleFlashLight(const CVector3& muzzle, const CVector3& dir, bool player) {
    Make();
    CLight* light = CLight::CreateFlash(&g_muzzle);
    if (light == NULL) {
        return;
    }
    const f32 ahead = player ? kPlayerAhead : kMuzzleAhead;
    light->SetPosition(CVector3(muzzle.x + dir.x * ahead, muzzle.y + dir.y * ahead, muzzle.z + dir.z * ahead));
}

void NativeExplosionLight(const CVector3& center, float radius) {
    Make();
    CLight* light = CLight::CreateFlash(&g_blast);
    if (light == NULL) {
        return;
    }
    f32 reach = radius * kBlastRadiusTimes;
    reach = reach < kBlastRadiusMin ? kBlastRadiusMin : reach > kBlastRadiusMax ? kBlastRadiusMax : reach;
    light->m_radius = reach;
    light->SetPosition(CVector3(center.x, center.y, center.z + kBlastRise));
}

// FRONTLINE_FLASH_TEST=<units>: every four seconds a shot's light that far
// in front of the camera, reaching 16 units and pure green, for two seconds:
// to see what the lights do without anyone having to fire.
void NativeFlashLightTest(const CMatrix& worldToCamera) {
    static const f32 ahead = getenv("FRONTLINE_FLASH_TEST") != NULL ? (f32)atof(getenv("FRONTLINE_FLASH_TEST")) : 0.0f;
    static int frame;
    if (ahead <= 0.0f || NativeFlashLights() == 0 || frame++ % 240 != 0) {
        return;
    }
    Make();
    f32 view[3][4];
    worldToCamera.GetGAMECUBEMatrix34(view);
    // The eye-space point back in the world: R^T (e - t).
    const f32 e[3] = {0.0f, 0.0f, -ahead};
    CVector3 at;
    f32* out[3] = {&at.x, &at.y, &at.z};
    for (int c = 0; c < 3; c++) {
        *out[c] = 0.0f;
        for (int r = 0; r < 3; r++) {
            *out[c] += view[r][c] * (e[r] - view[r][3]);
        }
    }
    // Pure green, so what it lights can't be mistaken for anything else.
    static AnimLightColor colors[120];
    static f32 strengths[120];
    static MOH_animatedLight_Struct test;
    for (int i = 0; i < 120; i++) {
        colors[i][0] = 0, colors[i][1] = 255, colors[i][2] = 0, colors[i][3] = 255;
        strengths[i] = 1.0f;
    }
    Animation(test, colors, strengths, 120, 16.0f);
    CLight* light = CLight::CreateFlash(&test);
    if (light != NULL) {
        light->SetPosition(at);
    }
}
