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
    uint32_t pad[3];
};

extern const char kShaderSource[];

}  // namespace frontline::gx
