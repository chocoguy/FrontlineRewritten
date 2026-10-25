// The TEV ubershader: every stage's inputs, operation and output register come
// from the uniforms, so one pipeline per blend state covers every draw. The
// math follows the console's TEV (docs: the GX TEV stage equation), in floats.

#include "gx_shader.h"

namespace frontline::gx {

const char kShaderSource[] = R"msl(
#include <metal_stdlib>
using namespace metal;

struct Vertex {
    float4 pos;
    float4 color[2];
    float4 tex[8];
    float pad[4];
};

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
    uint pad[3];
};

struct VOut {
    float4 pos [[position]];
    float4 color0;
    float4 color1;
    float4 tex0;
    float4 tex1;
    float4 tex2;
    float4 tex3;
    float4 tex4;
    float4 tex5;
    float4 tex6;
    float4 tex7;
};

vertex VOut tev_vertex(uint vid [[vertex_id]], const device Vertex* verts [[buffer(0)]]) {
    Vertex v = verts[vid];
    VOut o;
    o.pos = v.pos;
    o.color0 = v.color[0];
    o.color1 = v.color[1];
    o.tex0 = v.tex[0];
    o.tex1 = v.tex[1];
    o.tex2 = v.tex[2];
    o.tex3 = v.tex[3];
    o.tex4 = v.tex[4];
    o.tex5 = v.tex[5];
    o.tex6 = v.tex[6];
    o.tex7 = v.tex[7];
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

fragment float4 tev_fragment(VOut in [[stage_in]], constant Uniforms& u [[buffer(0)]],
                             array<texture2d<float>, 8> textures [[texture(0)]],
                             array<sampler, 8> samplers [[sampler(0)]]) {
    float4 texcoords[8] = {in.tex0, in.tex1, in.tex2, in.tex3, in.tex4, in.tex5, in.tex6, in.tex7};
    float4 regs[4] = {u.regs[0], u.regs[1], u.regs[2], u.regs[3]};

    // Indirect lookups: the texel's A, B, G as an offset source (8-bit format).
    float3 ind[4];
    for (uint i = 0; i < u.numIndStages && i < 4; i++) {
        uint4 o = u.indOrder[i];
        float2 texel = texcoords[o.x].xy / texcoords[o.x].z * u.coordSize[o.x].xy;
        texel /= float2(1u << o.z, 1u << o.w);
        float4 t = (u.texValid & (1u << o.y)) ? textures[o.y].sample(samplers[o.y], texel / u.texSize[o.y].xy)
                                              : float4(0.0);
        ind[i] = round(t.abg * 255.0);
    }

    uint last = 0;
    float2 prevCoord = float2(0.0);  // the previous stage's coordinate, in texels
    for (uint s = 0; s < u.numStages; s++) {
        uint cin = u.stages[s][0], ain = u.stages[s][1], cop = u.stages[s][2], aop = u.stages[s][3];
        uint order = u.stages[s][4], ks = u.stages[s][5], indw = u.stages[s][6];
        uint coord = order & 0xFF, map = (order >> 8) & 0xFF, chan = (order >> 16) & 0xFF;

        float4 ras = chan == 0 ? in.color0 : chan == 1 ? in.color1 : float4(0.0);
        ras = Swap(ras, u.swap[(ks >> 16) & 3]);

        float4 tex = float4(1.0);
        if (map < 8 && coord < 8) {
            float2 st = texcoords[coord].xy / texcoords[coord].z * u.coordSize[coord].xy / u.texSize[map].xy;
            if (indw >> 28) {
                float2 size = u.texSize[map].xy;
                uint indStage = indw & 3, bias = (indw >> 8) & 7, mtx = (indw >> 12) & 15;
                float3 v = ind[indStage];
                if (bias & 1) v.x -= 128.0;
                if (bias & 2) v.y -= 128.0;
                if (bias & 4) v.z -= 128.0;
                float2 offset = float2(0.0);
                if (mtx >= 1 && mtx <= 3) {
                    offset = float2(dot(u.indMtx[mtx - 1][0].xyz, v), dot(u.indMtx[mtx - 1][1].xyz, v)) *
                             u.indMtx[mtx - 1][0].w;
                }
                float2 coordT = Wrap(st * size, uint2((indw >> 16) & 7, (indw >> 20) & 7)) + offset;  // texels
                if ((indw >> 24) & 1) coordT += prevCoord;
                prevCoord = coordT;
                // GX coordinates are fixed point and a nearest lookup floors them; a
                // computed whole texel must not round down to its neighbor here.
                st = (coordT + 1.0 / 256.0) / size;
            } else {
                prevCoord = st * u.texSize[map].xy;
            }
            if (u.texValid & (1u << map)) {
                tex = textures[map].sample(samplers[map], st);
            }
            tex = Swap(tex, u.swap[(ks >> 24) & 3]);
        }

        float4 konst = float4(KonstColor(ks & 0xFF, u), KonstAlpha((ks >> 8) & 0xFF, u));

        // Color
        float3 a = ColorArg(cin & 15, regs, tex, ras, konst);
        float3 b = ColorArg((cin >> 4) & 15, regs, tex, ras, konst);
        float3 c = ColorArg((cin >> 8) & 15, regs, tex, ras, konst);
        float3 d = ColorArg((cin >> 12) & 15, regs, tex, ras, konst);
        uint op = cop & 15;
        float3 color;
        if (op < 2) {
            float3 l = a * (1.0 - c) + b * c;
            color = (d + (op == 1 ? -l : l) + Bias((cop >> 4) & 3)) * Scale((cop >> 8) & 3);
        } else {
            bool3 cmp;
            uint3 ia = uint3(U8(a.r), U8(a.g), U8(a.b)), ib = uint3(U8(b.r), U8(b.g), U8(b.b));
            switch (op) {
            case 8: cmp = bool3(ia.r > ib.r); break;
            case 9: cmp = bool3(ia.r == ib.r); break;
            case 10: cmp = bool3((ia.g << 8 | ia.r) > (ib.g << 8 | ib.r)); break;
            case 11: cmp = bool3((ia.g << 8 | ia.r) == (ib.g << 8 | ib.r)); break;
            case 12: cmp = bool3((ia.b << 16 | ia.g << 8 | ia.r) > (ib.b << 16 | ib.g << 8 | ib.r)); break;
            case 13: cmp = bool3((ia.b << 16 | ia.g << 8 | ia.r) == (ib.b << 16 | ib.g << 8 | ib.r)); break;
            case 14: cmp = ia > ib; break;
            default: cmp = ia == ib; break;
            }
            color = d + select(float3(0.0), c, cmp);
        }
        color = ((cop >> 12) & 1) ? saturate(color) : clamp(color, -4.0, 4.0);

        // Alpha
        float aa = AlphaArg(ain & 15, regs, tex, ras, konst);
        float ab = AlphaArg((ain >> 4) & 15, regs, tex, ras, konst);
        float ac = AlphaArg((ain >> 8) & 15, regs, tex, ras, konst);
        float ad = AlphaArg((ain >> 12) & 15, regs, tex, ras, konst);
        uint aopv = aop & 15;
        float alpha;
        if (aopv < 2) {
            float l = aa * (1.0 - ac) + ab * ac;
            alpha = (ad + (aopv == 1 ? -l : l) + Bias((aop >> 4) & 3)) * Scale((aop >> 8) & 3);
        } else {
            uint ia = U8(aa), ib = U8(ab);
            bool cmp = (aopv & 1) ? ia == ib : ia > ib;
            alpha = ad + (cmp ? ac : 0.0);
        }
        alpha = ((aop >> 12) & 1) ? saturate(alpha) : clamp(alpha, -4.0, 4.0);

        regs[(cop >> 16) & 3].rgb = color;
        regs[(aop >> 16) & 3].a = alpha;
        last = s;
    }
    float4 result = float4(regs[(u.stages[last][2] >> 16) & 3].rgb, regs[(u.stages[last][3] >> 16) & 3].a);
    result = saturate(result);

    // Alpha compare
    uint a8 = U8(result.a);
    bool p0 = Compare(u.alphaComp[0], a8, u.alphaComp[1]);
    bool p1 = Compare(u.alphaComp[3], a8, u.alphaComp[4]);
    bool pass;
    switch (u.alphaComp[2]) {
    case 0: pass = p0 && p1; break;
    case 1: pass = p0 || p1; break;
    case 2: pass = p0 != p1; break;
    default: pass = p0 == p1; break;
    }
    if (!pass) discard_fragment();

    // Fog, from the pixel's 24-bit screen depth as the hardware does it
    // (GXSetFog): ze = a * 2^24 / (B mantissa - (Z >> B shift)), fog = ze - c.
    uint ft = u.fogType & 7;
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
