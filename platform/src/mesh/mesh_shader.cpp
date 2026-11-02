// The Modern renderer's vertex stage: a mesh's vertices transformed on the
// GPU and either lit there (mesh_vertex), written as the TEV shader's vertex
// output (gx_shader.cpp VOut) so GX's own fragment stage draws them, or
// handed on to be lit per pixel (mesh_vertex_lit, for tev_fragment_lit). The
// math is GX's vertex stage (gx_vertex.cpp Transform and LightChannel) line
// for line, so per vertex the two agree but for rounding; the library
// compiles without fast math.

#include "mesh_shader.h"

namespace frontline::mesh {

const char kMeshShaderSource[] = R"msl(
#include <metal_stdlib>
using namespace metal;

// The TEV shader's vertex output, with the generated texture coordinates
// only (function constant 3, as the fragment stage it's linked with has).
constant uint kNumTexGensFC [[function_constant(3)]];
constant uint kTexGens = is_function_constant_defined(kNumTexGensFC) ? kNumTexGensFC : 8u;
constant bool kHasTex0 = kTexGens > 0;
constant bool kHasTex1 = kTexGens > 1;
constant bool kHasTex2 = kTexGens > 2;
constant bool kHasTex3 = kTexGens > 3;
constant bool kHasTex4 = kTexGens > 4;
constant bool kHasTex5 = kTexGens > 5;
constant bool kHasTex6 = kTexGens > 6;
constant bool kHasTex7 = kTexGens > 7;

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

struct MeshVertex {
    packed_float3 pos;
    uchar mtx;
    uchar pad0, pad1, pad2;
    packed_float3 nrm;
    uchar4 color;
    packed_float2 st;
};

struct Light {
    float4 color;
    float4 pos;
    float4 dir;
    float4 a;
    float4 k;
};

struct MeshUniforms {
    float proj[8];
    uint4 chan[4];
    float4 amb[2];
    float4 mat[2];
    uint4 texGen[3];
    float4 texMtx[3][3];
    uint numChans, numTexGens, flags, currentMtx;
    uint mtxBase, nrmBase, nrmStart, pad;
};

constant uint kHasNormals = 1;
constant uint kCurrentMtx = 2;
constant uint kTint = 4;
constant uint kNormalsFromPosition = 8;

static float Clamp01(float v) { return v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v; }

// The console's lighting for one channel: its color (3 components) or alpha.
static void LightChannel(uint4 ctrl, bool alpha, float4 vtx, float4 regAmb, float4 regMat, float3 eye, float3 nrm,
                         constant Light* lights, thread float4& out) {
    const int first = alpha ? 3 : 0, count = alpha ? 1 : 3;
    const bool lighting = ctrl.x & 1, ambSrc = ctrl.x & 2, matSrc = ctrl.x & 4;
    const float4 mat = matSrc ? vtx : regMat;
    const float4 amb = ambSrc ? vtx : regAmb;
    if (!lighting) {
        for (int i = first; i < first + count; i++) {
            out[i] = mat[i];
        }
        return;
    }
    float4 accum = amb;
    for (uint l = 0; l < 8; l++) {
        if (!(ctrl.y & (1u << l))) {
            continue;
        }
        const Light light = lights[l];
        float3 ldir = float3(light.pos.x - eye.x, light.pos.y - eye.y, light.pos.z - eye.z);
        const float dist2 = ldir.x * ldir.x + ldir.y * ldir.y + ldir.z * ldir.z;
        const float dist = precise::sqrt(dist2);
        const float inv = dist > 0.0 ? precise::divide(1.0f, dist) : 0.0;
        ldir *= inv;
        float attn = 1.0;
        if (ctrl.w == 1) {  // spot
            float cosAng = ldir.x * light.dir.x + ldir.y * light.dir.y + ldir.z * light.dir.z;
            cosAng = cosAng > 0.0 ? cosAng : 0.0;
            const float aattn = light.a.x + light.a.y * cosAng + light.a.z * cosAng * cosAng;
            const float dattn = light.k.x + light.k.y * dist + light.k.z * dist2;
            attn = precise::divide(aattn > 0.0 ? aattn : 0.0, dattn != 0.0 ? dattn : 1.0);
        } else if (ctrl.w == 0) {  // specular: the light's direction is the half angle
            float ndoth = nrm.x * light.dir.x + nrm.y * light.dir.y + nrm.z * light.dir.z;
            ndoth = ndoth > 0.0 ? ndoth : 0.0;
            const float aattn = light.a.x + light.a.y * ndoth + light.a.z * ndoth * ndoth;
            const float dattn = light.k.x + light.k.y * ndoth + light.k.z * ndoth * ndoth;
            attn = precise::divide(aattn > 0.0 ? aattn : 0.0, dattn != 0.0 ? dattn : 1.0);
            ldir = -light.dir.xyz;
        }
        float diff = 1.0;
        if (ctrl.z != 0) {
            diff = ldir.x * nrm.x + ldir.y * nrm.y + ldir.z * nrm.z;
            if (ctrl.z == 2 && diff < 0.0) {
                diff = 0.0;
            }
        }
        for (int i = first; i < first + count; i++) {
            accum[i] += attn * diff * light.color[i];
        }
    }
    for (int i = first; i < first + count; i++) {
        out[i] = mat[i] * Clamp01(accum[i]);
    }
}

// A vertex in eye space and clip space, with its color 0.
struct Transformed {
    float4 pos;
    float3 eye;
    float3 nrm;  // as GX transforms it: not renormalized
    float4 vtx;
};

static Transformed Transform(MeshVertex v, constant MeshUniforms& u, constant float* matrices,
                             constant float* unorm) {
    const float3 p = float3(v.pos);
    const uint row = (u.flags & kCurrentMtx) != 0 ? u.currentMtx : uint(v.mtx);
    Transformed t;

    // Eye space.
    constant float* m = matrices + ((row & 63) - u.mtxBase) * 4;
    for (int r = 0; r < 3; r++) {
        t.eye[r] = m[r * 4] * p.x + m[r * 4 + 1] * p.y + m[r * 4 + 2] * p.z + m[r * 4 + 3] * 1.0;
    }
    t.nrm = float3(0.0, 0.0, 1.0);
    if ((u.flags & (kHasNormals | kNormalsFromPosition)) == (kHasNormals | kNormalsFromPosition)) {
        // Corrected lighting for meshes whose bin leaves the normal matrix
        // alone (static meshes): the position matrix's inverse transpose, up to
        // scale (its cofactors, the sign kept for a mirroring matrix).
        const float3 a = float3(m[0], m[1], m[2]), b = float3(m[4], m[5], m[6]), c = float3(m[8], m[9], m[10]);
        const float3 vn = float3(v.nrm);
        const float3 bc = cross(b, c);
        t.nrm = float3(dot(bc, vn), dot(cross(c, a), vn), dot(cross(a, b), vn)) * sign(dot(a, bc));
    } else if ((u.flags & kHasNormals) != 0) {
        constant float* n = matrices + u.nrmStart + ((row & 31) * 3 - u.nrmBase);
        const float3 vn = float3(v.nrm);
        for (int r = 0; r < 3; r++) {
            t.nrm[r] = n[r * 3] * vn.x + n[r * 3 + 1] * vn.y + n[r * 3 + 2] * vn.z;
        }
    }

    // Clip space: z' / w lands in [-1, 0] near to far; Metal wants [0, 1].
    const float3 eye = t.eye;
    if (u.proj[0] == 0.0) {  // perspective
        t.pos = float4(u.proj[1] * eye.x + u.proj[2] * eye.z, u.proj[3] * eye.y + u.proj[4] * eye.z,
                       u.proj[5] * eye.z + u.proj[6], -eye.z);
    } else {
        t.pos = float4(u.proj[1] * eye.x + u.proj[2], u.proj[3] * eye.y + u.proj[4], u.proj[5] * eye.z + u.proj[6], 1.0);
    }
    t.pos.z += t.pos.w;
    t.vtx = float4(unorm[v.color.r], unorm[v.color.g], unorm[v.color.b], unorm[v.color.a]);
    return t;
}

// Texture coordinates: from the position, normal or texture coordinate 0,
// or color 0's red and green (SRTG).
static void TexGens(MeshVertex v, constant MeshUniforms& u, float4 color0, thread float3* tcs) {
    for (uint t = 0; t < 3 && t < u.numTexGens; t++) {
        const uint4 g = u.texGen[t];
        float4 src = float4(0.0, 0.0, 1.0, 1.0);
        if (g.y == 0) {
            src.xyz = float3(v.pos);
        } else if (g.y == 1) {
            src.xyz = float3(v.nrm);
        } else if (g.y == 4) {
            src.xy = float2(v.st);
        }
        float3 tc;
        if (g.x == 10) {
            tc = float3(color0.r, color0.g, 1.0);
        } else if (g.x > 1 || g.z == 60) {
            tc = float3(src.x, src.y, g.x == 0 ? src.z : 1.0);
        } else {
            const float4 r0 = u.texMtx[t][0], r1 = u.texMtx[t][1], r2 = u.texMtx[t][2];
            tc.x = r0.x * src.x + r0.y * src.y + r0.z * src.z + r0.w * src.w;
            tc.y = r1.x * src.x + r1.y * src.y + r1.z * src.z + r1.w * src.w;
            tc.z = g.x == 0 ? r2.x * src.x + r2.y * src.y + r2.z * src.z + r2.w * src.w : 1.0;
        }
        if (g.w != 0 && g.x != 10) {
            const float len = precise::sqrt(tc.x * tc.x + tc.y * tc.y + tc.z * tc.z);
            if (len > 0.0) {
                tc = float3(precise::divide(tc.x, len), precise::divide(tc.y, len), precise::divide(tc.z, len));
            }
        }
        tcs[t] = tc;
    }
}

#define TEX_OUT(o, tcs, u) \
    { \
        const float4 none = float4(0.0, 0.0, 1.0, 0.0); \
        if (kHasTex0) o.tex0 = u.numTexGens > 0 ? float4(tcs[0], 0.0) : none; \
        if (kHasTex1) o.tex1 = u.numTexGens > 1 ? float4(tcs[1], 0.0) : none; \
        if (kHasTex2) o.tex2 = u.numTexGens > 2 ? float4(tcs[2], 0.0) : none; \
        if (kHasTex3) o.tex3 = none; \
        if (kHasTex4) o.tex4 = none; \
        if (kHasTex5) o.tex5 = none; \
        if (kHasTex6) o.tex6 = none; \
        if (kHasTex7) o.tex7 = none; \
    }

// Lit per vertex, as GX lights.
vertex VOut mesh_vertex(uint vid [[vertex_id]], const device MeshVertex* verts [[buffer(0)]],
                        constant MeshUniforms& u [[buffer(1)]], constant Light* lights [[buffer(2)]],
                        constant float* matrices [[buffer(3)]], constant float* unorm [[buffer(4)]]) {
    const MeshVertex v = verts[vid];
    const Transformed t = Transform(v, u, matrices, unorm);
    VOut o;
    o.pos = t.pos;
    // Colors: the vertex's color 0 (color 1 is white: meshes have none).
    float4 colors[2];
    for (uint c = 0; c < 2; c++) {
        colors[c] = float4(0.0);
        if (c < u.numChans) {
            const float4 vtx = c == 0 ? t.vtx : float4(1.0);
            LightChannel(u.chan[c], false, vtx, u.amb[c], u.mat[c], t.eye, t.nrm, lights, colors[c]);
            LightChannel(u.chan[c + 2], true, vtx, u.amb[c], u.mat[c], t.eye, t.nrm, lights, colors[c]);
        }
    }
    if ((u.flags & kTint) != 0) {
        colors[0].rgb *= float3(0.6, 1.0, 0.6);
    }
    o.color0 = colors[0];
    o.color1 = colors[1];
    float3 tcs[3] = {float3(0.0, 0.0, 1.0), float3(0.0, 0.0, 1.0), float3(0.0, 0.0, 1.0)};
    TexGens(v, u, colors[0], tcs);
    TEX_OUT(o, tcs, u)
    return o;
}

// Lit per pixel: the TEV shader's lit fragment stage (gx_shader.cpp
// tev_fragment_lit) lights from the eye-space position and normal and the
// color handed on here. (No SRTG coordinates: they need the lit color.)
struct VOutLit {
    float4 pos [[position]];
    float4 tex0 [[function_constant(kHasTex0)]];
    float4 tex1 [[function_constant(kHasTex1)]];
    float4 tex2 [[function_constant(kHasTex2)]];
    float4 tex3 [[function_constant(kHasTex3)]];
    float4 tex4 [[function_constant(kHasTex4)]];
    float4 tex5 [[function_constant(kHasTex5)]];
    float4 tex6 [[function_constant(kHasTex6)]];
    float4 tex7 [[function_constant(kHasTex7)]];
    float3 eye;
    float3 nrm;
    float nrmLength;
    float4 vtx;
};

vertex VOutLit mesh_vertex_lit(uint vid [[vertex_id]], const device MeshVertex* verts [[buffer(0)]],
                               constant MeshUniforms& u [[buffer(1)]], constant float* matrices [[buffer(3)]],
                               constant float* unorm [[buffer(4)]]) {
    const MeshVertex v = verts[vid];
    const Transformed t = Transform(v, u, matrices, unorm);
    VOutLit o;
    o.pos = t.pos;
    o.eye = t.eye;
    o.nrm = t.nrm;
    o.nrmLength = precise::sqrt(t.nrm.x * t.nrm.x + t.nrm.y * t.nrm.y + t.nrm.z * t.nrm.z);
    o.vtx = t.vtx;
    float3 tcs[3] = {float3(0.0, 0.0, 1.0), float3(0.0, 0.0, 1.0), float3(0.0, 0.0, 1.0)};
    TexGens(v, u, float4(0.0), tcs);
    TEX_OUT(o, tcs, u)
    return o;
}
)msl";

}  // namespace frontline::mesh
