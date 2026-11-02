// The TEV ubershader (Metal Shading Language) and the uniforms it reads.
#pragma once

#include <cstdint>

namespace frontline::gx {

// Per stage, 8 words: color inputs, alpha inputs, color op, alpha op, order,
// constants and swaps, indirect, unused. See PackStage in gx_metal.mm.
struct FragUniforms {
    uint32_t stages[16][8];
    float konst[4][4];
    float regs[4][4];  // PREV, REG0..2 at the start of each pixel
    uint32_t swap[4][4];
    uint32_t numStages;
    uint32_t alphaComp[5];
    uint32_t fogType;
    uint32_t texValid;  // bit per texture map
    float fog[4];       // a, B mantissa, c, B shift
    float fogColor[4];
    float indMtx[3][2][4];
    uint32_t indOrder[4][4];  // texcoord, map, scale s, scale t
    float texSize[8][4];      // width, height
    // Each texture coordinate becomes texels at one size: the texture paired
    // with it by the last TEV stage that uses it (else an indirect stage), as
    // the SDK sets the rasterizer's coordinate scale. Every lookup through the
    // coordinate uses that size.
    float coordSize[8][4];
    uint32_t numIndStages;
    uint32_t alphaCoverage;  // the alpha test as alpha to coverage (gx_metal.mm AlphaCoverage)
    uint32_t pad[2];
};

// The vertex shader's (gx_shader.cpp VertexUniforms, which says what they
// mean). Mode 0 draws the recorded positions as they are.
struct VertexUniforms {
    uint32_t stride;
    uint32_t numTexGens;
    uint32_t mode;
    uint32_t pad;
    float from[8];
    float zoom[4];
    float move[3][4];
};

// The lit fragment stage's (gx_shader.cpp tev_fragment_lit, the Modern
// renderer's per-pixel lighting): the channels and lights as GX's vertex
// stage reads them, and for corrected lighting (mode 1) the scene's point
// lights. 1296 bytes.
struct LitLight {
    float color[4];
    float pos[4];
    float dir[4];
    float a[4];
    float k[4];
};
struct LitUniforms {
    uint32_t chan[4][4];  // color 0, color 1, alpha 0, alpha 1: lighting | ambSrc << 1 | matSrc << 2, mask, diffFn, attnFn
    float amb[2][4];      // register colors, 0..1
    float mat[2][4];
    uint32_t numChans;
    // 0: GX's lighting. 1: corrected: the lights' positions are directions
    // toward them in eye space, normals are unit length, and the point
    // lights light the color channels too.
    uint32_t mode;
    uint32_t numPoints;
    uint32_t pad;
    LitLight lights[8];
    float points[16][2][4];  // eye-space position and radius; color
};
static_assert(sizeof(LitUniforms) == 1296);

extern const char kShaderSource[];

}  // namespace frontline::gx
