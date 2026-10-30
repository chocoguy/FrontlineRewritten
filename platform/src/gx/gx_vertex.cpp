// The vertex stage (gx_vertex.h).
//
// GX vertex data is big-endian: display lists and arrays come from the disc,
// and the immediate-mode pipe (gx.cpp) writes big-endian too, as the console's
// CPU does. The exception is the indexed matrix arrays (21..24), which the game
// builds at run time in host order (docs/architecture/0003-byte-order.md).
// Scratch memory is malloc'd: operator new belongs to the game.

#include "gx_vertex.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gx_state.h"
#include "types.h"

namespace frontline::gx {
namespace {

uint16_t Be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint32_t Be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
float BeF32(const uint8_t* p) {
    uint32_t v = Be32(p);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

uint32_t ComponentSize(uint32_t type) { return type == 4 ? 4 : type >= 2 ? 2 : 1; }

float ReadComponent(const uint8_t* p, uint32_t type, float scale) {
    switch (type) {
    case 0:
        return p[0] * scale;
    case 1:
        return (int8_t)p[0] * scale;
    case 2:
        return Be16(p) * scale;
    case 3:
        return (int16_t)Be16(p) * scale;
    default:
        return BeF32(p);
    }
}

uint32_t ColorSize(uint32_t type) {
    static const uint8_t kSize[8] = {2, 3, 4, 2, 3, 4, 4, 4};
    return kSize[type & 7];
}

void ReadColor(const uint8_t* p, uint32_t type, float out[4]) {
    uint32_t r = 255, g = 255, b = 255, a = 255;
    switch (type) {
    case 0: {  // RGB565
        uint32_t v = Be16(p);
        r = (v >> 11) * 255 / 31;
        g = ((v >> 5) & 63) * 255 / 63;
        b = (v & 31) * 255 / 31;
        break;
    }
    case 1:  // RGB8
    case 2:  // RGBX8
        r = p[0], g = p[1], b = p[2];
        break;
    case 3: {  // RGBA4
        uint32_t v = Be16(p);
        r = (v >> 12) * 17, g = ((v >> 8) & 15) * 17, b = ((v >> 4) & 15) * 17, a = (v & 15) * 17;
        break;
    }
    case 4: {  // RGBA6
        uint32_t v = (uint32_t)p[0] << 16 | p[1] << 8 | p[2];
        r = (v >> 18) * 255 / 63, g = ((v >> 12) & 63) * 255 / 63, b = ((v >> 6) & 63) * 255 / 63,
        a = (v & 63) * 255 / 63;
        break;
    }
    default:  // RGBA8
        r = p[0], g = p[1], b = p[2], a = p[3];
        break;
    }
    out[0] = r / 255.0f;
    out[1] = g / 255.0f;
    out[2] = b / 255.0f;
    out[3] = a / 255.0f;
}

// A decoded vertex, before transform.
struct RawVertex {
    uint32_t posMtx;
    uint32_t texMtx[8];
    float pos[3];
    float nrm[3];
    float color[2][4];
    float tex[8][2];
    bool hasNrm, hasColor[2];
};

// Malloc'd scratch arrays, grown as needed.
template <class T>
struct Scratch {
    T* data = nullptr;
    uint32_t capacity = 0;
    T* Get(uint32_t n) {
        if (n > capacity) {
            capacity = n > capacity * 2 ? n : capacity * 2;
            data = static_cast<T*>(std::realloc(data, capacity * sizeof(T)));
        }
        return data;
    }
};
Scratch<float> g_vertices;
Scratch<uint16_t> g_indices;  // a draw has at most 65535 vertices

const uint8_t* AttrData(uint32_t attr, const uint8_t*& p, uint32_t desc) {
    if (desc == 1) {
        return p;
    }
    uint32_t index;
    if (desc == 2) {
        index = *p++;
    } else {
        index = Be16(p);
        p += 2;
    }
    return ADDR_TO_PTR(const uint8_t, g_state.arrayBase[attr] + index * g_state.arrayStride[attr]);
}

// Decodes one vertex; returns the bytes it took.
const uint8_t* Decode(const uint8_t* p, uint32_t fmt, RawVertex& v) {
    const uint32_t* desc = g_state.vtxDesc;
    const AttrFmt* vat = g_state.vat[fmt];
    v.posMtx = desc[kPnMtxIdx] ? *p++ : g_state.currentPosMtx;
    for (int i = 0; i < 8; i++) {
        v.texMtx[i] = desc[kTex0MtxIdx + i] ? *p++ : g_state.currentTexMtx[i];
    }
    if (desc[kPos]) {
        const uint8_t* d = AttrData(kPos, p, desc[kPos]);
        uint32_t n = vat[kPos].cnt ? 3 : 2;
        float scale = 1.0f / (1 << vat[kPos].frac);
        uint32_t size = ComponentSize(vat[kPos].type);
        for (uint32_t i = 0; i < n; i++) {
            v.pos[i] = ReadComponent(d + i * size, vat[kPos].type, scale);
        }
        if (n == 2) {
            v.pos[2] = 0.0f;
        }
        if (desc[kPos] == 1) {
            p += n * size;
        }
    }
    v.hasNrm = desc[kNrm] != 0;
    if (desc[kNrm]) {
        uint32_t type = vat[kNrm].type;
        float scale = type == 1 ? 1.0f / 64 : type == 3 ? 1.0f / 16384 : 1.0f;
        uint32_t size = ComponentSize(type);
        uint32_t n = vat[kNrm].cnt ? 9 : 3;
        if (vat[kNrm].cnt == 2 && desc[kNrm] != 1) {  // NBT3: three indices
            for (int k = 0; k < 3; k++) {
                const uint8_t* d = AttrData(kNrm, p, desc[kNrm]);
                if (k == 0) {
                    for (int i = 0; i < 3; i++) {
                        v.nrm[i] = ReadComponent(d + i * size, type, scale);
                    }
                }
            }
        } else {
            const uint8_t* d = AttrData(kNrm, p, desc[kNrm]);
            for (int i = 0; i < 3; i++) {
                v.nrm[i] = ReadComponent(d + i * size, type, scale);
            }
            if (desc[kNrm] == 1) {
                p += n * size;
            }
        }
    }
    for (int c = 0; c < 2; c++) {
        uint32_t attr = kClr0 + c;
        v.hasColor[c] = desc[attr] != 0;
        if (desc[attr]) {
            const uint8_t* d = AttrData(attr, p, desc[attr]);
            ReadColor(d, vat[attr].type, v.color[c]);
            if (desc[attr] == 1) {
                p += ColorSize(vat[attr].type);
            }
        } else {
            v.color[c][0] = v.color[c][1] = v.color[c][2] = v.color[c][3] = 1.0f;
        }
    }
    for (int t = 0; t < 8; t++) {
        uint32_t attr = kTex0 + t;
        v.tex[t][0] = v.tex[t][1] = 0.0f;
        if (desc[attr]) {
            const uint8_t* d = AttrData(attr, p, desc[attr]);
            uint32_t n = vat[attr].cnt ? 2 : 1;
            float scale = 1.0f / (1 << vat[attr].frac);
            uint32_t size = ComponentSize(vat[attr].type);
            for (uint32_t i = 0; i < n; i++) {
                v.tex[t][i] = ReadComponent(d + i * size, vat[attr].type, scale);
            }
            if (desc[attr] == 1) {
                p += n * size;
            }
        }
    }
    return p;
}

void MulMtx34(const float* m, const float in[3], float w, float out[3]) {
    for (int r = 0; r < 3; r++) {
        out[r] = m[r * 4] * in[0] + m[r * 4 + 1] * in[1] + m[r * 4 + 2] * in[2] + m[r * 4 + 3] * w;
    }
}

float Clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

// The console's lighting for one channel (color: 3 components, or alpha: 1).
void LightChannel(const ChanCtrl& ctrl, int chan, bool alpha, const float eyePos[3], const float nrm[3],
                  const RawVertex& rv, float* out) {
    int first = alpha ? 3 : 0;
    int count = alpha ? 1 : 3;
    float mat[4], amb[4];
    for (int i = 0; i < 4; i++) {
        mat[i] = ctrl.matSrc ? rv.color[chan][i] : g_state.matColor[chan][i] / 255.0f;
        amb[i] = ctrl.ambSrc ? rv.color[chan][i] : g_state.ambColor[chan][i] / 255.0f;
    }
    if (!ctrl.lighting) {
        for (int i = first; i < first + count; i++) {
            out[i] = mat[i];
        }
        return;
    }
    float accum[4] = {amb[0], amb[1], amb[2], amb[3]};
    for (int l = 0; l < 8; l++) {
        if (!(ctrl.lightMask & (1 << l))) {
            continue;
        }
        const Light& light = g_state.lights[l];
        float ldir[3] = {light.pos[0] - eyePos[0], light.pos[1] - eyePos[1], light.pos[2] - eyePos[2]};
        float dist2 = ldir[0] * ldir[0] + ldir[1] * ldir[1] + ldir[2] * ldir[2];
        float dist = std::sqrt(dist2);
        float inv = dist > 0.0f ? 1.0f / dist : 0.0f;
        for (float& c : ldir) {
            c *= inv;
        }
        float attn = 1.0f;
        if (ctrl.attnFn == 1) {  // spot
            float cosAng = ldir[0] * light.dir[0] + ldir[1] * light.dir[1] + ldir[2] * light.dir[2];
            cosAng = cosAng > 0.0f ? cosAng : 0.0f;
            float aattn = light.a[0] + light.a[1] * cosAng + light.a[2] * cosAng * cosAng;
            float dattn = light.k[0] + light.k[1] * dist + light.k[2] * dist2;
            attn = (aattn > 0.0f ? aattn : 0.0f) / (dattn != 0.0f ? dattn : 1.0f);
        } else if (ctrl.attnFn == 0) {  // specular: the light's direction is the half angle
            float ndoth = nrm[0] * light.dir[0] + nrm[1] * light.dir[1] + nrm[2] * light.dir[2];
            ndoth = ndoth > 0.0f ? ndoth : 0.0f;
            float aattn = light.a[0] + light.a[1] * ndoth + light.a[2] * ndoth * ndoth;
            float dattn = light.k[0] + light.k[1] * ndoth + light.k[2] * ndoth * ndoth;
            attn = (aattn > 0.0f ? aattn : 0.0f) / (dattn != 0.0f ? dattn : 1.0f);
            for (int i = 0; i < 3; i++) {
                ldir[i] = -light.dir[i];
            }
        }
        float diff = 1.0f;
        if (ctrl.diffFn != 0) {
            diff = ldir[0] * nrm[0] + ldir[1] * nrm[1] + ldir[2] * nrm[2];
            if (ctrl.diffFn == 2 && diff < 0.0f) {
                diff = 0.0f;
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

// Transforms and lights a vertex into `out` (gx_vertex.h's layout), with
// `numTexGens` texture coordinates.
void Transform(const RawVertex& rv, uint32_t numTexGens, float* out) {
    const State& s = g_state;
    const float* m = &s.xfMtx[(rv.posMtx & 63) * 4];
    float eye[3];
    MulMtx34(m, rv.pos, 1.0f, eye);
    float nrm[3] = {0.0f, 0.0f, 1.0f};
    if (rv.hasNrm) {
        const float* n = &s.xfNrm[(rv.posMtx & 31) * 3];
        for (int r = 0; r < 3; r++) {
            nrm[r] = n[r * 3] * rv.nrm[0] + n[r * 3 + 1] * rv.nrm[1] + n[r * 3 + 2] * rv.nrm[2];
        }
    }
    // Projection: z' / w lands in [-1, 0] near to far; Metal wants [0, 1].
    const float* p = s.projection;
    float* pos = out;
    if (p[0] == 0.0f) {  // perspective
        pos[0] = p[1] * eye[0] + p[2] * eye[2];
        pos[1] = p[3] * eye[1] + p[4] * eye[2];
        pos[2] = p[5] * eye[2] + p[6];
        pos[3] = -eye[2];
    } else {
        pos[0] = p[1] * eye[0] + p[2];
        pos[1] = p[3] * eye[1] + p[4];
        pos[2] = p[5] * eye[2] + p[6];
        pos[3] = 1.0f;
    }
    pos[2] += pos[3];

    float* colors = out + 4;  // color0, color1
    for (int c = 0; c < 2; c++) {
        float* color = colors + c * 4;
        if ((uint32_t)c < s.numChans) {
            LightChannel(s.chan[c], c, false, eye, nrm, rv, color);
            LightChannel(s.chan[c + 2], c, true, eye, nrm, rv, color);
        } else {
            color[0] = color[1] = color[2] = color[3] = 0.0f;
        }
    }

    // Coordinates past numTexGens aren't stored: the shader reads (0, 0, 1).
    for (uint32_t t = 0; t < numTexGens; t++) {
        float* tc = out + kVertexBase + t * 3;
        const TexGen& g = s.texGen[t];
        float src[4] = {0.0f, 0.0f, 1.0f, 1.0f};
        if (g.src == 0) {
            src[0] = rv.pos[0], src[1] = rv.pos[1], src[2] = rv.pos[2];
        } else if (g.src == 1) {
            src[0] = rv.nrm[0], src[1] = rv.nrm[1], src[2] = rv.nrm[2];
        } else if (g.src >= 4 && g.src <= 11) {
            src[0] = rv.tex[g.src - 4][0], src[1] = rv.tex[g.src - 4][1];
        } else if (g.src >= 12 && g.src <= 18) {  // TEXCOORD0..6 (the movie player's)
            src[0] = rv.tex[g.src - 12][0], src[1] = rv.tex[g.src - 12][1];
        } else if (g.src == 19 || g.src == 20) {
            src[0] = colors[(g.src - 19) * 4], src[1] = colors[(g.src - 19) * 4 + 1];
        }
        if (g.func == 10) {  // SRTG: color0's red and green
            tc[0] = colors[0], tc[1] = colors[1], tc[2] = 1.0f;
            continue;
        }
        uint32_t mtx = g.func == 0 || g.func == 1 ? (g.mtx == 60 ? 60 : g.mtx) : 60;
        if (mtx == 60) {
            tc[0] = src[0], tc[1] = src[1], tc[2] = g.func == 0 ? src[2] : 1.0f;
        } else {
            const float* tm = &s.xfMtx[(mtx & 63) * 4];
            tc[0] = tm[0] * src[0] + tm[1] * src[1] + tm[2] * src[2] + tm[3] * src[3];
            tc[1] = tm[4] * src[0] + tm[5] * src[1] + tm[6] * src[2] + tm[7] * src[3];
            tc[2] = g.func == 0 ? tm[8] * src[0] + tm[9] * src[1] + tm[10] * src[2] + tm[11] * src[3] : 1.0f;
        }
        if (g.normalize) {
            float len = std::sqrt(tc[0] * tc[0] + tc[1] * tc[1] + tc[2] * tc[2]);
            if (len > 0.0f) {
                for (int i = 0; i < 3; i++) {
                    tc[i] /= len;
                }
            }
        }
    }
}

}  // namespace

uint32_t VertexSize(uint32_t fmt) {
    const uint32_t* desc = g_state.vtxDesc;
    const AttrFmt* vat = g_state.vat[fmt & 7];
    auto indexOr = [&](uint32_t attr, uint32_t direct) { return desc[attr] == 1 ? direct : desc[attr] == 2 ? 1 : 2; };
    uint32_t size = 0;
    for (uint32_t a = kPnMtxIdx; a <= kTex0MtxIdx + 7; a++) {
        size += desc[a] ? 1 : 0;
    }
    if (desc[kPos]) {
        size += indexOr(kPos, (vat[kPos].cnt ? 3 : 2) * ComponentSize(vat[kPos].type));
    }
    if (desc[kNrm]) {
        uint32_t n = vat[kNrm].cnt ? 9 : 3;
        if (vat[kNrm].cnt == 2 && desc[kNrm] != 1) {
            size += 3 * (desc[kNrm] == 2 ? 1 : 2);
        } else {
            size += indexOr(kNrm, n * ComponentSize(vat[kNrm].type));
        }
    }
    for (uint32_t c = kClr0; c <= kClr1; c++) {
        if (desc[c]) {
            size += indexOr(c, ColorSize(vat[c].type));
        }
    }
    for (uint32_t t = kTex0; t <= kTex0 + 7; t++) {
        if (desc[t]) {
            size += indexOr(t, (vat[t].cnt ? 2 : 1) * ComponentSize(vat[t].type));
        }
    }
    return size;
}

uint32_t Draw(uint32_t prim, uint32_t fmt, const uint8_t* data, uint32_t count) {
    fmt &= 7;
    const uint32_t numTexGens = g_state.numTexGens < 8 ? g_state.numTexGens : 8;
    const uint32_t stride = kVertexBase + numTexGens * 3;
    float* verts = g_vertices.Get(count * stride);
    const uint8_t* p = data;
    for (uint32_t i = 0; i < count; i++) {
        RawVertex rv;
        p = Decode(p, fmt, rv);
        Transform(rv, numTexGens, verts + i * stride);
    }
    uint32_t consumed = (uint32_t)(p - data);

    uint16_t* idx = g_indices.Get(count * 3 + 6);
    uint32_t n = 0;
    switch (prim & 0xF8) {
    case 0x80:  // quads
    case 0x88:
        for (uint32_t i = 0; i + 3 < count; i += 4) {
            idx[n++] = i, idx[n++] = i + 1, idx[n++] = i + 2;
            idx[n++] = i, idx[n++] = i + 2, idx[n++] = i + 3;
        }
        break;
    case 0x90:  // triangles
        for (uint32_t i = 0; i + 2 < count; i += 3) {
            idx[n++] = i, idx[n++] = i + 1, idx[n++] = i + 2;
        }
        break;
    case 0x98:  // strip
        for (uint32_t i = 2; i < count; i++) {
            if (i & 1) {
                idx[n++] = i - 1, idx[n++] = i - 2, idx[n++] = i;
            } else {
                idx[n++] = i - 2, idx[n++] = i - 1, idx[n++] = i;
            }
        }
        break;
    case 0xA0:  // fan
        for (uint32_t i = 2; i < count; i++) {
            idx[n++] = 0, idx[n++] = i - 1, idx[n++] = i;
        }
        break;
    default: {  // lines and points: not drawn yet
        static bool warned;
        if (!warned) {
            warned = true;
            std::fprintf(stderr, "frontline: GX primitive %#x isn't drawn yet\n", prim);
        }
        return consumed;
    }
    }
    if (n != 0) {
        SubmitTriangles(verts, stride, count, idx, n);
    }
    return consumed;
}

namespace {

void LoadXf(uint32_t addr, const uint8_t* values, uint32_t count, bool bigEndian) {
    for (uint32_t i = 0; i < count; i++, addr++) {
        float f;
        if (bigEndian) {
            f = BeF32(values + i * 4);
        } else {
            std::memcpy(&f, values + i * 4, 4);
        }
        if (addr < 0x100) {
            g_state.xfMtx[addr] = f;
        } else if (addr >= 0x400 && addr < 0x460) {
            g_state.xfNrm[addr - 0x400] = f;
        } else if (addr >= 0x500 && addr < 0x600) {
            g_state.xfPostMtx[addr - 0x500] = f;
        }
    }
}

}  // namespace

void RunDisplayList(const uint8_t* list, uint32_t size) {
    const uint8_t* p = list;
    const uint8_t* end = list + size;
    while (p < end) {
        uint8_t op = *p++;
        if (op == 0x00 || op == 0x48) {  // nop, invalidate the vertex cache
            continue;
        }
        if (op == 0x08) {  // CP register
            uint8_t reg = p[0];
            uint32_t value = Be32(p + 1);
            p += 5;
            if (reg >= 0xA0 && reg < 0xB0) {
                g_state.arrayBase[kPos + (reg - 0xA0)] = value;
            } else if (reg >= 0xB0 && reg < 0xC0) {
                g_state.arrayStride[kPos + (reg - 0xB0)] = value & 0xFF;
            }
            continue;
        }
        if (op == 0x10) {  // XF registers
            uint32_t n = Be16(p) + 1;
            uint32_t addr = Be16(p + 2);
            LoadXf(addr, p + 4, n, true);
            p += 4 + n * 4;
            continue;
        }
        if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {  // indexed XF loads
            uint32_t v = Be32(p);
            p += 4;
            uint32_t array = kPosMtxArray + ((op - 0x20) >> 3);
            uint32_t index = v >> 16;
            uint32_t n = ((v >> 12) & 0xF) + 1;
            uint32_t addr = v & 0xFFF;
            const uint8_t* src =
                ADDR_TO_PTR(const uint8_t, g_state.arrayBase[array] + index * g_state.arrayStride[array]);
            LoadXf(addr, src, n, false);
            continue;
        }
        if (op == 0x40) {  // call another list
            RunDisplayList(ADDR_TO_PTR(const uint8_t, Be32(p)), Be32(p + 4));
            p += 8;
            continue;
        }
        if (op == 0x61) {  // BP register: not used by the game's lists
            p += 4;
            continue;
        }
        if (op >= 0x80) {
            uint32_t count = Be16(p);
            p += 2;
            p += Draw(op & 0xF8, op & 7, p, count);
            continue;
        }
        std::fprintf(stderr, "frontline: unknown display list command %#x\n", op);
        return;
    }
}

}  // namespace frontline::gx
