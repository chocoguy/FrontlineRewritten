// The TEV shader. Compiled with no function constants it is the ubershader:
// every stage's inputs, operation and output register come from the
// uniforms, so one pipeline per blend state covers every draw. Given the TEV
// setup's shape as function constants (gx_metal.mm: Shape), the same code
// folds into a shader for just that setup: the stage loop unrolls, the input
// and operation switches resolve, and unused texture coordinates aren't
// interpolated. The math follows the console's TEV (docs: the GX TEV stage
// equation), in floats.

#include "gx_shader.h"

namespace frontline::gx {

const char kShaderSource[] = R"msl(
#include <metal_stdlib>
using namespace metal;

// A function constant's value if the pipeline was specialized, else the
// ubershader's run-time value.
#define FC(name, fallback) (is_function_constant_defined(name) ? name : (fallback))

constant uint kNumStagesFC [[function_constant(1)]];
constant uint kNumIndStagesFC [[function_constant(2)]];
constant uint kNumTexGensFC [[function_constant(3)]];
constant uint kAlphaFuncsFC [[function_constant(4)]];  // comp0 | op << 4 | comp1 << 8
constant uint kFogTypeFC [[function_constant(5)]];
constant uint kTexValidFC [[function_constant(6)]];
constant uint kIndOrderFC [[function_constant(7)]];  // 8 bits an indirect stage: texcoord | map << 4
constant uint kIndScaleFC [[function_constant(8)]];  // 8 bits an indirect stage: s | t << 4
constant uint kAlphaCoverageFC [[function_constant(9)]];

constant bool kSpecialized = is_function_constant_defined(kNumStagesFC);

// Stage s's packed words (PackStage in gx_metal.mm), constants 16 + s * 8 + w.
#define STAGE_CONSTANTS(s) \
    constant uint kStage##s##W0 [[function_constant(16 + s * 8 + 0)]]; \
    constant uint kStage##s##W1 [[function_constant(16 + s * 8 + 1)]]; \
    constant uint kStage##s##W2 [[function_constant(16 + s * 8 + 2)]]; \
    constant uint kStage##s##W3 [[function_constant(16 + s * 8 + 3)]]; \
    constant uint kStage##s##W4 [[function_constant(16 + s * 8 + 4)]]; \
    constant uint kStage##s##W5 [[function_constant(16 + s * 8 + 5)]]; \
    constant uint kStage##s##W6 [[function_constant(16 + s * 8 + 6)]];
STAGE_CONSTANTS(0) STAGE_CONSTANTS(1) STAGE_CONSTANTS(2) STAGE_CONSTANTS(3)
STAGE_CONSTANTS(4) STAGE_CONSTANTS(5) STAGE_CONSTANTS(6) STAGE_CONSTANTS(7)
STAGE_CONSTANTS(8) STAGE_CONSTANTS(9) STAGE_CONSTANTS(10) STAGE_CONSTANTS(11)
STAGE_CONSTANTS(12) STAGE_CONSTANTS(13) STAGE_CONSTANTS(14) STAGE_CONSTANTS(15)

// The texture coordinates that reach the fragment stage: all eight in the
// ubershader, the generated ones when specialized.
constant uint kTexGens = FC(kNumTexGensFC, 8u);
constant bool kHasTex0 = kTexGens > 0;
constant bool kHasTex1 = kTexGens > 1;
constant bool kHasTex2 = kTexGens > 2;
constant bool kHasTex3 = kTexGens > 3;
constant bool kHasTex4 = kTexGens > 4;
constant bool kHasTex5 = kTexGens > 5;
constant bool kHasTex6 = kTexGens > 6;
constant bool kHasTex7 = kTexGens > 7;

struct Uniforms {
    uint stages[16][8];
    float4 konst[4];
    float4 regs[4];
    uint4 swap[4];
    uint numStages;
    uint alphaComp[5];
    uint fogType;
    uint texValid;
    float4 fog;
    float4 fogColor;
    float4 indMtx[3][2];
    uint4 indOrder[4];
    float4 texSize[8];
    float4 coordSize[8];
    uint numIndStages;
    uint alphaCoverage;
    uint pad[2];
};

// A vertex (gx_vertex.cpp): position, two colors, then `numTexGens`
// coordinates of three floats, `stride` floats in all. The position is in clip
// space; a replayed frame (gx_record.h, mode 1) moves it to another camera:
// back to eye space through the projection it was made with (`from`: type,
// then the 6 terms), through `move`, and on through the same projection with
// its scales multiplied by `zoom`.
struct VertexUniforms {
    uint stride;
    uint numTexGens;
    uint mode;
    uint pad;
    float from[8];
    float4 zoom;
    float4 move[3];
};

static float4 Moved(float4 c, constant VertexUniforms& vu) {
    constant float* p = vu.from;
    float3 e;
    if (p[0] == 0.0) {  // perspective: w = -z
        e.z = -c.w;
        e.x = (c.x - p[2] * e.z) / p[1];
        e.y = (c.y - p[4] * e.z) / p[3];
    } else {
        e = float3((c.x - p[2]) / p[1], (c.y - p[4]) / p[3], (c.z - c.w - p[6]) / p[5]);
    }
    e = float3(dot(vu.move[0], float4(e, 1.0)), dot(vu.move[1], float4(e, 1.0)), dot(vu.move[2], float4(e, 1.0)));
    const float sx = p[1] * vu.zoom.x, sy = p[3] * vu.zoom.y;
    float4 o;
    if (p[0] == 0.0) {
        o = float4(sx * e.x + p[2] * e.z, sy * e.y + p[4] * e.z, p[5] * e.z + p[6], -e.z);
    } else {
        o = float4(sx * e.x + p[2], sy * e.y + p[4], p[5] * e.z + p[6], 1.0);
    }
    o.z += o.w;  // Metal's depth (gx_vertex.cpp Transform)
    return o;
}

struct VOut {
    float4 pos [[position]];
    float4 color0;
    float4 color1;
    float4 tex0 [[function_constant(kHasTex0)]];
    float4 tex1 [[function_constant(kHasTex1)]];
    float4 tex2 [[function_constant(kHasTex2)]];
    float4 tex3 [[function_constant(kHasTex3)]];
    float4 tex4 [[function_constant(kHasTex4)]];
    float4 tex5 [[function_constant(kHasTex5)]];
    float4 tex6 [[function_constant(kHasTex6)]];
    float4 tex7 [[function_constant(kHasTex7)]];
};

vertex VOut tev_vertex(uint vid [[vertex_id]], const device float* verts [[buffer(0)]],
                       constant VertexUniforms& vu [[buffer(1)]]) {
    const device float* v = verts + vid * vu.stride;
    const uint n = FC(kNumTexGensFC, vu.numTexGens);
    VOut o;
    o.pos = float4(v[0], v[1], v[2], v[3]);
    if (vu.mode != 0) {
        o.pos = Moved(o.pos, vu);
    }
    o.color0 = float4(v[4], v[5], v[6], v[7]);
    o.color1 = float4(v[8], v[9], v[10], v[11]);
    // Coordinates past the generated ones read (0, 0, 1).
#define TEX_OUT(i) \
    if (kHasTex##i) { \
        o.tex##i = i < n ? float4(v[12 + i * 3], v[13 + i * 3], v[14 + i * 3], 0.0) : float4(0.0, 0.0, 1.0, 0.0); \
    }
    TEX_OUT(0) TEX_OUT(1) TEX_OUT(2) TEX_OUT(3) TEX_OUT(4) TEX_OUT(5) TEX_OUT(6) TEX_OUT(7)
    return o;
}

static float4 Swap(float4 c, uint4 table) {
    return float4(c[table.x], c[table.y], c[table.z], c[table.w]);
}

static float3 ColorArg(uint a, thread float4* regs, float4 tex, float4 ras, float4 konst) {
    switch (a) {
    case 0: return regs[0].rgb;
    case 1: return regs[0].aaa;
    case 2: return regs[1].rgb;
    case 3: return regs[1].aaa;
    case 4: return regs[2].rgb;
    case 5: return regs[2].aaa;
    case 6: return regs[3].rgb;
    case 7: return regs[3].aaa;
    case 8: return tex.rgb;
    case 9: return tex.aaa;
    case 10: return ras.rgb;
    case 11: return ras.aaa;
    case 12: return float3(1.0);
    case 13: return float3(0.5);
    case 14: return konst.rgb;
    default: return float3(0.0);
    }
}

static float AlphaArg(uint a, thread float4* regs, float4 tex, float4 ras, float4 konst) {
    switch (a) {
    case 0: return regs[0].a;
    case 1: return regs[1].a;
    case 2: return regs[2].a;
    case 3: return regs[3].a;
    case 4: return tex.a;
    case 5: return ras.a;
    case 6: return konst.a;
    default: return 0.0;
    }
}

static float KonstValue(uint sel) {
    switch (sel) {
    case 0: return 1.0;
    case 1: return 7.0 / 8.0;
    case 2: return 3.0 / 4.0;
    case 3: return 5.0 / 8.0;
    case 4: return 1.0 / 2.0;
    case 5: return 3.0 / 8.0;
    case 6: return 1.0 / 4.0;
    default: return 1.0 / 8.0;
    }
}

static float3 KonstColor(uint sel, constant Uniforms& u) {
    if (sel < 8) return float3(KonstValue(sel));
    if (sel >= 0x0C && sel < 0x10) return u.konst[sel - 0x0C].rgb;
    if (sel >= 0x10) return float3(u.konst[sel & 3][(sel - 0x10) >> 2]);
    return float3(0.0);
}

static float KonstAlpha(uint sel, constant Uniforms& u) {
    if (sel < 8) return KonstValue(sel);
    if (sel >= 0x10) return u.konst[sel & 3][(sel - 0x10) >> 2];
    return 0.0;
}

static float Bias(uint b) { return b == 1 ? 0.5 : b == 2 ? -0.5 : 0.0; }
static float Scale(uint s) { return s == 1 ? 2.0 : s == 2 ? 4.0 : s == 3 ? 0.5 : 1.0; }

static uint U8(float v) { return uint(clamp(round(v * 255.0), 0.0, 255.0)); }

static bool Compare(uint fn, uint a, uint ref) {
    switch (fn) {
    case 0: return false;
    case 1: return a < ref;
    case 2: return a == ref;
    case 3: return a <= ref;
    case 4: return a > ref;
    case 5: return a != ref;
    case 6: return a >= ref;
    default: return true;
    }
}

static float2 Wrap(float2 c, uint2 w) {
    float2 r = c;
    for (int i = 0; i < 2; i++) {
        if (w[i] == 6) r[i] = 0.0;
        else if (w[i] != 0) { float m = float(256 >> (w[i] - 1)); r[i] = c[i] - m * floor(c[i] / m); }
    }
    return r;
}

// One TEV stage from its packed words: color inputs, alpha inputs, color op,
// alpha op, order, constants and swaps, indirect.
#define TEV_STAGE(CIN, AIN, COP, AOP, ORDER, KS, INDW) { \
    uint cin = (CIN), ain = (AIN), cop = (COP), aop = (AOP), order = (ORDER), ks = (KS), indw = (INDW); \
    uint coord = order & 0xFF, map = (order >> 8) & 0xFF, chan = (order >> 16) & 0xFF; \
    \
    float4 ras = chan == 0 ? in.color0 : chan == 1 ? in.color1 : float4(0.0); \
    ras = Swap(ras, u.swap[(ks >> 16) & 3]); \
    \
    float4 tex = float4(1.0); \
    if (map < 8 && coord < 8) { \
        float2 st = texcoords[coord].xy / texcoords[coord].z * u.coordSize[coord].xy / u.texSize[map].xy; \
        if (indw >> 28) { \
            float2 size = u.texSize[map].xy; \
            uint indStage = indw & 3, bias = (indw >> 8) & 7, mtx = (indw >> 12) & 15; \
            float3 v = ind[indStage]; \
            if (bias & 1) v.x -= 128.0; \
            if (bias & 2) v.y -= 128.0; \
            if (bias & 4) v.z -= 128.0; \
            float2 offset = float2(0.0); \
            if (mtx >= 1 && mtx <= 3) { \
                offset = float2(dot(u.indMtx[mtx - 1][0].xyz, v), dot(u.indMtx[mtx - 1][1].xyz, v)) * \
                         u.indMtx[mtx - 1][0].w; \
            } \
            float2 coordT = Wrap(st * size, uint2((indw >> 16) & 7, (indw >> 20) & 7)) + offset; /* texels */ \
            if ((indw >> 24) & 1) coordT += prevCoord; \
            prevCoord = coordT; \
            /* GX coordinates are fixed point and a nearest lookup floors them; a */ \
            /* computed whole texel must not round down to its neighbor here. */ \
            st = (coordT + 1.0 / 256.0) / size; \
        } else { \
            prevCoord = st * u.texSize[map].xy; \
        } \
        if (texValid & (1u << map)) { \
            tex = textures[map].sample(samplers[map], st); \
        } \
        tex = Swap(tex, u.swap[(ks >> 24) & 3]); \
    } \
    \
    float4 konst = float4(KonstColor(ks & 0xFF, u), KonstAlpha((ks >> 8) & 0xFF, u)); \
    \
    /* Color */ \
    float3 a = ColorArg(cin & 15, regs, tex, ras, konst); \
    float3 b = ColorArg((cin >> 4) & 15, regs, tex, ras, konst); \
    float3 c = ColorArg((cin >> 8) & 15, regs, tex, ras, konst); \
    float3 d = ColorArg((cin >> 12) & 15, regs, tex, ras, konst); \
    uint op = cop & 15; \
    float3 color; \
    if (op < 2) { \
        float3 l = a * (1.0 - c) + b * c; \
        color = (d + (op == 1 ? -l : l) + Bias((cop >> 4) & 3)) * Scale((cop >> 8) & 3); \
    } else { \
        bool3 cmp; \
        uint3 ia = uint3(U8(a.r), U8(a.g), U8(a.b)), ib = uint3(U8(b.r), U8(b.g), U8(b.b)); \
        switch (op) { \
        case 8: cmp = bool3(ia.r > ib.r); break; \
        case 9: cmp = bool3(ia.r == ib.r); break; \
        case 10: cmp = bool3((ia.g << 8 | ia.r) > (ib.g << 8 | ib.r)); break; \
        case 11: cmp = bool3((ia.g << 8 | ia.r) == (ib.g << 8 | ib.r)); break; \
        case 12: cmp = bool3((ia.b << 16 | ia.g << 8 | ia.r) > (ib.b << 16 | ib.g << 8 | ib.r)); break; \
        case 13: cmp = bool3((ia.b << 16 | ia.g << 8 | ia.r) == (ib.b << 16 | ib.g << 8 | ib.r)); break; \
        case 14: cmp = ia > ib; break; \
        default: cmp = ia == ib; break; \
        } \
        color = d + select(float3(0.0), c, cmp); \
    } \
    color = ((cop >> 12) & 1) ? saturate(color) : clamp(color, -4.0, 4.0); \
    \
    /* Alpha */ \
    float aa = AlphaArg(ain & 15, regs, tex, ras, konst); \
    float ab = AlphaArg((ain >> 4) & 15, regs, tex, ras, konst); \
    float ac = AlphaArg((ain >> 8) & 15, regs, tex, ras, konst); \
    float ad = AlphaArg((ain >> 12) & 15, regs, tex, ras, konst); \
    uint aopv = aop & 15; \
    float alpha; \
    if (aopv < 2) { \
        float l = aa * (1.0 - ac) + ab * ac; \
        alpha = (ad + (aopv == 1 ? -l : l) + Bias((aop >> 4) & 3)) * Scale((aop >> 8) & 3); \
    } else { \
        uint ia = U8(aa), ib = U8(ab); \
        bool cmp = (aopv & 1) ? ia == ib : ia > ib; \
        alpha = ad + (cmp ? ac : 0.0); \
    } \
    alpha = ((aop >> 12) & 1) ? saturate(alpha) : clamp(alpha, -4.0, 4.0); \
    \
    regs[(cop >> 16) & 3].rgb = color; \
    regs[(aop >> 16) & 3].a = alpha; \
    lastCop = cop; \
    lastAop = aop; \
}

#define STAGE_WORD(s, w) FC(kStage##s##W##w, u.stages[s][w])
#define SPECIALIZED_STAGE(s) \
    if (s < numStages) { \
        TEV_STAGE(STAGE_WORD(s, 0), STAGE_WORD(s, 1), STAGE_WORD(s, 2), STAGE_WORD(s, 3), STAGE_WORD(s, 4), \
                  STAGE_WORD(s, 5), STAGE_WORD(s, 6)) \
    }

fragment float4 tev_fragment(VOut in [[stage_in]], constant Uniforms& u [[buffer(0)]],
                             array<texture2d<float>, 8> textures [[texture(0)]],
                             array<sampler, 8> samplers [[sampler(0)]]) {
    const float4 none = float4(0.0, 0.0, 1.0, 0.0);  // a coordinate that wasn't generated
    float4 texcoords[8] = {kHasTex0 ? in.tex0 : none, kHasTex1 ? in.tex1 : none, kHasTex2 ? in.tex2 : none,
                           kHasTex3 ? in.tex3 : none, kHasTex4 ? in.tex4 : none, kHasTex5 ? in.tex5 : none,
                           kHasTex6 ? in.tex6 : none, kHasTex7 ? in.tex7 : none};
    float4 regs[4] = {u.regs[0], u.regs[1], u.regs[2], u.regs[3]};
    const uint numStages = FC(kNumStagesFC, u.numStages);
    const uint texValid = FC(kTexValidFC, u.texValid);

    // Indirect lookups: the texel's A, B, G as an offset source (8-bit format).
    float3 ind[4];
    const uint numIndStages = FC(kNumIndStagesFC, u.numIndStages);
    for (uint i = 0; i < numIndStages && i < 4; i++) {
        uint4 o = is_function_constant_defined(kIndOrderFC)
                      ? uint4((kIndOrderFC >> (i * 8)) & 15, (kIndOrderFC >> (i * 8 + 4)) & 15,
                              (kIndScaleFC >> (i * 8)) & 15, (kIndScaleFC >> (i * 8 + 4)) & 15)
                      : u.indOrder[i];
        float2 texel = texcoords[o.x].xy / texcoords[o.x].z * u.coordSize[o.x].xy;
        texel /= float2(1u << o.z, 1u << o.w);
        float4 t = (texValid & (1u << o.y)) ? textures[o.y].sample(samplers[o.y], texel / u.texSize[o.y].xy)
                                            : float4(0.0);
        ind[i] = round(t.abg * 255.0);
    }

    uint lastCop = u.stages[0][2], lastAop = u.stages[0][3];
    float2 prevCoord = float2(0.0);  // the previous stage's coordinate, in texels
    if (kSpecialized) {
        SPECIALIZED_STAGE(0) SPECIALIZED_STAGE(1) SPECIALIZED_STAGE(2) SPECIALIZED_STAGE(3)
        SPECIALIZED_STAGE(4) SPECIALIZED_STAGE(5) SPECIALIZED_STAGE(6) SPECIALIZED_STAGE(7)
        SPECIALIZED_STAGE(8) SPECIALIZED_STAGE(9) SPECIALIZED_STAGE(10) SPECIALIZED_STAGE(11)
        SPECIALIZED_STAGE(12) SPECIALIZED_STAGE(13) SPECIALIZED_STAGE(14) SPECIALIZED_STAGE(15)
    } else {
        for (uint s = 0; s < numStages; s++) {
            TEV_STAGE(u.stages[s][0], u.stages[s][1], u.stages[s][2], u.stages[s][3], u.stages[s][4],
                      u.stages[s][5], u.stages[s][6])
        }
    }
    float4 result = float4(regs[(lastCop >> 16) & 3].rgb, regs[(lastAop >> 16) & 3].a);
    result = saturate(result);

    // Alpha compare
    const uint funcs = FC(kAlphaFuncsFC, u.alphaComp[0] | u.alphaComp[2] << 4 | u.alphaComp[3] << 8);
    if (FC(kAlphaCoverageFC, u.alphaCoverage) != 0) {
        // Rewritten's MSAA: a one-threshold test (a > ref, a >= ref) becomes
        // the share of the pixel past the threshold, as alpha to coverage, so
        // its edges are smoothed too: the alpha's distance past it over how
        // much it changes across the pixel. Alpha isn't blended or written
        // here.
        const bool first = (funcs & 15) != 7;
        const uint fn = first ? funcs & 15 : (funcs >> 8) & 15;
        const float ref = float(first ? u.alphaComp[1] : u.alphaComp[4]) + (fn == 4 ? 0.5 : -0.5);
        const float a = result.a * 255.0;
        const float share = saturate((a - ref) / max(fwidth(a), 0.01) + 0.5);
        if (share <= 0.0) discard_fragment();
        result.a = share;
    } else {
        uint a8 = U8(result.a);
        bool p0 = Compare(funcs & 15, a8, u.alphaComp[1]);
        bool p1 = Compare((funcs >> 8) & 15, a8, u.alphaComp[4]);
        bool pass;
        switch ((funcs >> 4) & 15) {
        case 0: pass = p0 && p1; break;
        case 1: pass = p0 || p1; break;
        case 2: pass = p0 != p1; break;
        default: pass = p0 == p1; break;
        }
        if (!pass) discard_fragment();
    }

    // Fog, from the pixel's 24-bit screen depth as the hardware does it
    // (GXSetFog): ze = a * 2^24 / (B mantissa - (Z >> B shift)), fog = ze - c.
    uint ft = FC(kFogTypeFC, u.fogType) & 7;
    if (ft >= 2) {
        float z = min(floor(in.pos.z * 16777216.0), 16777215.0);
        float ze = u.fog.x * 16777216.0 / (u.fog.y - floor(z / exp2(u.fog.w)));
        float f = clamp(ze - u.fog.z, 0.0, 1.0);
        switch (ft) {
        case 4: f = 1.0 - exp2(-8.0 * f); break;
        case 5: f = 1.0 - exp2(-8.0 * f * f); break;
        case 6: f = exp2(-8.0 * (1.0 - f)); break;
        case 7: f = exp2(-8.0 * (1.0 - f) * (1.0 - f)); break;
        default: break;
        }
        result.rgb = mix(result.rgb, u.fogColor.rgb, f);
    }
    return result;
}

struct BlitOut {
    float4 pos [[position]];
    float2 uv;
};

vertex BlitOut blit_vertex(uint vid [[vertex_id]]) {
    float2 uv = float2((vid << 1) & 2, vid & 2);
    BlitOut o;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}

fragment float4 blit_fragment(BlitOut in [[stage_in]], texture2d<float> tex [[texture(0)]]) {
    constexpr sampler s(filter::linear);
    return float4(tex.sample(s, in.uv).rgb, 1.0);
}
)msl";

}  // namespace frontline::gx
