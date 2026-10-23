// GX: the graphics API. Each call updates the renderer's state (gx_state.h);
// draws go through the vertex stage (gx_vertex.cpp) to Metal (gx_metal.mm).
// Immediate-mode vertices arrive through the write-gather pipe proxy (gx.h),
// collected big-endian as the console's CPU writes them into the FIFO.

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "dolphin/gx.h"
#include "gx_metal.h"
#include "gx_state.h"
#include "gx_vertex.h"
#include "os/os_internal.h"

namespace frontline::gx {

State g_state;

namespace {

u8 g_fifo[0x80];  // the GXFifoObj GXInit returns
u8* g_pipeRedirect;  // GXRedirectWriteGatherPipe's destination

// The immediate-mode draw being collected (GXBegin).
struct Immediate {
    uint32_t prim, fmt, count, size, used;
    uint8_t* data;
    uint32_t capacity;
} g_imm;

void PipeBytes(const uint8_t* bytes, uint32_t n) {
    if (g_imm.size == 0) {
        return;  // no GXBegin: nothing to collect
    }
    if (g_imm.used + n > g_imm.capacity) {
        g_imm.capacity = (g_imm.used + n) * 2;
        g_imm.data = static_cast<uint8_t*>(std::realloc(g_imm.data, g_imm.capacity));
    }
    std::memcpy(g_imm.data + g_imm.used, bytes, n);
    g_imm.used += n;
    if (g_imm.used >= g_imm.size) {
        Draw(g_imm.prim, g_imm.fmt, g_imm.data, g_imm.count);
        g_imm.size = 0;
    }
}

void SetTevPreset(TevStage& t, uint32_t stage, uint32_t mode) {
    // GXSetTevOp: stage 0 starts from the rasterized color, later stages from PREV.
    uint32_t c = stage == 0 ? 10 : 0;  // RASC : CPREV
    uint32_t a = stage == 0 ? 5 : 0;   // RASA : APREV
    static const uint32_t kZero = 15, kZeroA = 7;
    switch (mode) {
    case 0:  // MODULATE
        t = TevStage{.colorIn = {kZero, 8, c, kZero}, .alphaIn = {kZeroA, 4, a, kZeroA}};
        break;
    case 1:  // DECAL
        t = TevStage{.colorIn = {c, 8, 9, kZero}, .alphaIn = {kZeroA, kZeroA, kZeroA, a}};
        break;
    case 2:  // BLEND
        t = TevStage{.colorIn = {c, 12, 8, kZero}, .alphaIn = {kZeroA, 4, a, kZeroA}};
        break;
    case 3:  // REPLACE
        t = TevStage{.colorIn = {kZero, kZero, kZero, 8}, .alphaIn = {kZeroA, kZeroA, kZeroA, 4}};
        break;
    default:  // PASSCLR
        t = TevStage{.colorIn = {kZero, kZero, kZero, c}, .alphaIn = {kZeroA, kZeroA, kZeroA, a}};
        break;
    }
}

TevStage& Stage(uint32_t stage) { return g_state.tev[stage & 15]; }

}  // namespace

void ResetState() {
    State& s = g_state;
    TexObj maps[8];
    std::memcpy(maps, s.texMap, sizeof(maps));
    std::memset(&s, 0, sizeof(s));
    s.vtxDesc[kPos] = 1;
    for (int id = 0; id * 4 + 11 < 256; id += 3) {  // identity matrices everywhere
        s.xfMtx[id * 4] = s.xfMtx[id * 4 + 5] = s.xfMtx[id * 4 + 10] = 1.0f;
    }
    for (int i = 0; i + 8 < 96; i += 9) {
        s.xfNrm[i] = s.xfNrm[i + 4] = s.xfNrm[i + 8] = 1.0f;
    }
    for (int i = 0; i < 8; i++) {
        s.currentTexMtx[i] = 60;
    }
    s.viewport[2] = 640, s.viewport[3] = 448, s.viewport[5] = 1.0f;
    s.scissor[2] = 640, s.scissor[3] = 448;
    s.numChans = 1;
    for (ChanCtrl& c : s.chan) {
        c.matSrc = 1;
    }
    s.numTexGens = 1;
    for (TexGen& g : s.texGen) {
        g = {1, 4, 60, false, 125};
    }
    s.numTevStages = 1;
    for (uint32_t i = 0; i < 16; i++) {
        SetTevPreset(s.tev[i], i, 4);
        s.tev[i].colorClamp = s.tev[i].alphaClamp = 1;
        s.tev[i].texCoord = i < 8 ? i : 0xFF;
        s.tev[i].texMap = i < 8 ? i : 0xFF;
        s.tev[i].chan = 4;
        s.tev[i].kcsel = 6;  // 1/4, the SDK's default
        s.tev[i].kasel = 6;
    }
    const uint8_t swaps[4][4] = {{0, 1, 2, 3}, {0, 0, 0, 3}, {1, 1, 1, 3}, {2, 2, 2, 3}};
    std::memcpy(s.swapTable, swaps, sizeof(swaps));
    s.alphaComp[0] = 7, s.alphaComp[3] = 7;  // always
    s.zEnable = true, s.zFunc = 3, s.zUpdate = true;
    s.colorUpdate = true;
    s.cullMode = 2;
    s.clearZ = 0xFFFFFF;
    std::memcpy(s.texMap, maps, sizeof(maps));
}

}  // namespace frontline::gx

using namespace frontline::gx;

GXNativeWGPipe GXNativeWGFifo;

extern "C" {

GXRenderModeObj GXNtsc480IntDf = {
    0, 640, 480, 480, 40, 0, 640, 480, 1, 0, 0,
    {{6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}},
    {8, 8, 10, 12, 10, 8, 8},
};
GXRenderModeObj GXPal528IntDf = {
    4, 640, 528, 528, 40, 23, 640, 528, 1, 0, 0,
    {{6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}},
    {8, 8, 10, 12, 10, 8, 8},
};
GXRenderModeObj GXMpal480IntDf = {
    8, 640, 480, 480, 40, 0, 640, 480, 1, 0, 0,
    {{6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}},
    {8, 8, 10, 12, 10, 8, 8},
};

// The write-gather pipe. Redirected (the movie player building a frame), the
// words land in memory in host order, like the rest of that code's stores.
// Otherwise they're vertex data, kept big-endian.
void GXNativePipe8(u8 v) {
    if (g_pipeRedirect != nullptr) {
        *g_pipeRedirect++ = v;
        return;
    }
    PipeBytes(&v, 1);
}
void GXNativePipe16(u16 v) {
    if (g_pipeRedirect != nullptr) {
        std::memcpy(g_pipeRedirect, &v, 2);
        g_pipeRedirect += 2;
        return;
    }
    u8 b[2] = {(u8)(v >> 8), (u8)v};
    PipeBytes(b, 2);
}
void GXNativePipe32(u32 v) {
    if (g_pipeRedirect != nullptr) {
        std::memcpy(g_pipeRedirect, &v, 4);
        g_pipeRedirect += 4;
        return;
    }
    u8 b[4] = {(u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v};
    PipeBytes(b, 4);
}
void GXNativePipeF32(f32 v) {
    u32 bits;
    std::memcpy(&bits, &v, 4);
    GXNativePipe32(bits);
}

volatile void* GXRedirectWriteGatherPipe(void* ptr) {
    g_pipeRedirect = static_cast<u8*>(ptr);
    return g_pipeRedirect;
}
void GXRestoreWriteGatherPipe(void) { g_pipeRedirect = nullptr; }

void* GXInit(void*, u32) {
    ResetState();
    return g_fifo;
}

void GXAdjustForOverscan(const GXRenderModeObj* rmin, GXRenderModeObj* rmout, u16 hor, u16 ver) {
    u16 hor2 = hor * 2;
    u16 ver2 = ver * 2;
    if (rmin != rmout) {
        *rmout = *rmin;
    }
    rmout->fbWidth = rmin->fbWidth - hor2;
    u32 verf = (ver2 * rmin->efbHeight) / rmin->xfbHeight;
    rmout->efbHeight = rmin->efbHeight - verf;
    if (rmin->xFBmode == 0 && (rmin->viTVmode & 2) != 2) {
        rmout->xfbHeight = rmin->xfbHeight - ver;
    } else {
        rmout->xfbHeight = rmin->xfbHeight - ver2;
    }
    rmout->viWidth = rmin->viWidth - hor2;
    rmout->viHeight = rmin->viHeight - ver2;
    rmout->viXOrigin = rmin->viXOrigin + hor;
    rmout->viYOrigin = rmin->viYOrigin + ver;
}

f32 GXGetYScaleFactor(u16 efbHeight, u16 xfbHeight) { return (f32)xfbHeight / (f32)efbHeight; }
u32 GXSetDispCopyYScale(f32 vscale) { return (u32)(448.0f * vscale); }
void GXSetDispCopySrc(u16 left, u16 top, u16 wd, u16 ht) { SetCopySource(left, top, wd, ht); }
void GXSetDispCopyDst(u16, u16) {}
void GXSetCopyFilter(u8, const u8[12][2], u8, const u8[7]) {}
void GXSetPixelFmt(u32, u32) {}
void GXSetDispCopyGamma(u32) {}
void GXCopyDisp(void* dest, u8 clear) {
    frontline::os::SafePoint();
    CopyDisp(dest, clear != 0);
}
void GXDrawDone(void) {
    frontline::os::SafePoint();
    DrawDone();
}
void GXInvalidateVtxCache(void) {}
void GXSetColorUpdate(u8 update) { g_state.colorUpdate = update != 0; }
void GXSetCopyClear(GXColor color, u32 z) {
    std::memcpy(g_state.clearColor, &color, 4);
    g_state.clearZ = z;
}

// Textures
void GXInitTexObj(GXTexObj* obj, void* image, u16 width, u16 height, u32 format, u32 wrapS, u32 wrapT, u8 mipmap) {
    std::memset(obj, 0, sizeof(*obj));
    TexObj* t = reinterpret_cast<TexObj*>(obj);
    t->image = PTR_TO_ADDR(image);
    t->width = width;
    t->height = height;
    t->format = (u8)format;
    t->wrapS = (u8)wrapS;
    t->wrapT = (u8)wrapT;
    t->mipmap = mipmap;
    t->minFilter = mipmap ? 5 : 1;
    t->magFilter = 1;
    u32 size = width > height ? width : height;
    u8 lod = 0;
    while (size > 1) {
        size >>= 1;
        lod++;
    }
    t->maxLod = mipmap ? lod : 0;
}
void GXInitTexObjCI(GXTexObj* obj, void* image, u16 width, u16 height, u32 format, u32 wrapS, u32 wrapT, u8 mipmap,
                    u32 tlutName) {
    GXInitTexObj(obj, image, width, height, format, wrapS, wrapT, mipmap);
    TexObj* t = reinterpret_cast<TexObj*>(obj);
    t->isCI = 1;
    t->tlut = tlutName;
}
void GXInitTexObjLOD(GXTexObj* obj, u32 minFilt, u32 magFilt, f32, f32 maxLod, f32, u8, u8, u32) {
    TexObj* t = reinterpret_cast<TexObj*>(obj);
    t->minFilter = (u8)minFilt;
    t->magFilter = (u8)magFilt;
    if (t->mipmap) {
        u8 lod = (u8)(maxLod < 0.0f ? 0 : maxLod > 10.0f ? 10 : (int)maxLod);
        t->maxLod = lod < t->maxLod ? lod : t->maxLod;
    }
}
u16 GXGetTexObjWidth(GXTexObj* obj) { return reinterpret_cast<TexObj*>(obj)->width; }
u16 GXGetTexObjHeight(GXTexObj* obj) { return reinterpret_cast<TexObj*>(obj)->height; }
u32 GXGetTexObjFmt(GXTexObj* obj) { return reinterpret_cast<TexObj*>(obj)->format; }
void GXInitTlutObj(GXTlutObj* obj, void* lut, u32 format, u16 numEntries) {
    obj->dummy[0] = PTR_TO_ADDR(lut);
    obj->dummy[1] = format;
    obj->dummy[2] = numEntries;
}
void GXLoadTexObj(GXTexObj* obj, u32 id) {
    if (id < 8) {
        std::memcpy(&g_state.texMap[id], obj, sizeof(TexObj));
    }
}
void GXLoadTlut(GXTlutObj* obj, u32 name) {
    g_state.tluts[name % 20] = {obj->dummy[0], obj->dummy[1], obj->dummy[2]};
}

// Lights (the SDK's attenuation formulas)
void GXInitLightColor(GXLightObj* lt, GXColor color) { std::memcpy(reinterpret_cast<LightObj*>(lt)->color, &color, 4); }
void GXInitLightPos(GXLightObj* lt, f32 x, f32 y, f32 z) {
    LightObj* l = reinterpret_cast<LightObj*>(lt);
    l->pos[0] = x, l->pos[1] = y, l->pos[2] = z;
}
void GXInitLightDir(GXLightObj* lt, f32 nx, f32 ny, f32 nz) {
    LightObj* l = reinterpret_cast<LightObj*>(lt);
    l->dir[0] = -nx, l->dir[1] = -ny, l->dir[2] = -nz;
}
void GXInitLightSpot(GXLightObj* lt, f32 cutoff, u32 fn) {
    LightObj* l = reinterpret_cast<LightObj*>(lt);
    float a0 = 1.0f, a1 = 0.0f, a2 = 0.0f;
    if (cutoff > 0.0f && cutoff <= 90.0f) {
        float cr = std::cos(cutoff * 3.14159265f / 180.0f);
        float d;
        switch (fn) {
        case 1:  // flat
            a0 = -1000.0f * cr, a1 = 1000.0f;
            break;
        case 2:  // cos
            a0 = -cr / (1.0f - cr), a1 = 1.0f / (1.0f - cr);
            break;
        case 3:  // cos2
            a1 = -cr / (1.0f - cr), a2 = 1.0f / (1.0f - cr);
            break;
        case 4:  // sharp
            d = (1.0f - cr) * (1.0f - cr);
            a0 = cr * (cr - 2.0f) / d, a1 = 2.0f / d, a2 = -1.0f / d;
            break;
        case 5:  // ring1
            d = (1.0f - cr) * (1.0f - cr);
            a0 = -4.0f * cr / d, a1 = 4.0f * (1.0f + cr) / d, a2 = -4.0f / d;
            break;
        case 6:  // ring2
            d = (1.0f - cr) * (1.0f - cr);
            a0 = 1.0f - 2.0f * cr * cr / d, a1 = 4.0f * cr / d, a2 = -2.0f / d;
            break;
        }
    }
    l->a[0] = a0, l->a[1] = a1, l->a[2] = a2;
}
void GXInitLightDistAttn(GXLightObj* lt, f32 refDist, f32 refBright, u32 fn) {
    LightObj* l = reinterpret_cast<LightObj*>(lt);
    float k0 = 1.0f, k1 = 0.0f, k2 = 0.0f;
    if (refDist >= 0.0f && refBright > 0.0f && refBright < 1.0f) {
        float r = 1.0f - refBright;
        switch (fn) {
        case 1:  // gentle
            k1 = r / (refBright * refDist);
            break;
        case 2:  // medium
            k1 = 0.5f * r / (refBright * refDist), k2 = 0.5f * r / (refBright * refDist * refDist);
            break;
        case 3:  // steep
            k2 = r / (refBright * refDist * refDist);
            break;
        }
    }
    l->k[0] = k0, l->k[1] = k1, l->k[2] = k2;
}
void GXLoadLightObjImm(GXLightObj* lt, u32 light) {
    if (light == 0) {
        return;
    }
    const LightObj* l = reinterpret_cast<const LightObj*>(lt);
    Light& dst = g_state.lights[__builtin_ctz(light) & 7];
    for (int i = 0; i < 4; i++) {
        dst.color[i] = l->color[i] / 255.0f;
    }
    for (int i = 0; i < 3; i++) {
        dst.pos[i] = l->pos[i], dst.dir[i] = l->dir[i], dst.a[i] = l->a[i], dst.k[i] = l->k[i];
    }
}

// Indirect texturing
void GXSetIndTexCoordScale(u32 stage, u32 s, u32 t) {
    g_state.indScale[stage & 3][0] = s;
    g_state.indScale[stage & 3][1] = t;
}
void GXSetIndTexMtx(u32 id, f32 offset[2][3], s8 scaleExp) {
    if (id < 1 || id > 3) {
        return;
    }
    for (int r = 0; r < 2; r++) {
        for (int c = 0; c < 3; c++) {
            g_state.indMtx[id - 1][r][c] = offset[r][c];
        }
    }
    g_state.indMtxScale[id - 1] = scaleExp;
}
void GXSetNumIndStages(u8 n) { g_state.numIndStages = n; }
void GXSetIndTexOrder(u32 stage, u32 coord, u32 map) {
    g_state.indOrder[stage & 3][0] = coord;
    g_state.indOrder[stage & 3][1] = map;
}
void GXSetTevIndirect(u32 stage, u32 indStage, u32 format, u32 bias, u32 mtx, u32 wrapS, u32 wrapT, u8 addPrev,
                      u8 utcLod, u32 alphaSel) {
    TevStage& t = Stage(stage);
    t.indStage = indStage, t.indFormat = format, t.indBias = bias, t.indMtx = mtx;
    t.indWrapS = wrapS, t.indWrapT = wrapT, t.indAddPrev = addPrev != 0, t.indUtcLod = utcLod != 0;
    t.indAlphaSel = alphaSel;
    t.indirect = true;
}
void GXSetTevIndRepeat(u32 stage) { GXSetTevIndirect(stage, 0, 0, 0, 0, 6, 6, 1, 0, 0); }
void GXSetTevDirect(u32 stage) { Stage(stage).indirect = false; }

// Vertex formats and arrays
void GXGetVtxAttrFmt(u32 fmt, u32 attr, u32* cnt, u32* type, u8* frac) {
    const AttrFmt& f = g_state.vat[fmt & 7][attr < kNumAttrs ? attr : 0];
    *cnt = f.cnt;
    *type = f.type;
    *frac = f.frac;
}
void GXSetVtxAttrFmt(u32 fmt, u32 attr, u32 cnt, u32 type, u8 frac) {
    if (attr < kNumAttrs) {
        g_state.vat[fmt & 7][attr] = {cnt, type, frac};
    }
}
void GXClearVtxDesc(void) {
    std::memset(g_state.vtxDesc, 0, sizeof(g_state.vtxDesc));
    g_state.vtxDesc[kPos] = 1;
}
void GXSetVtxDesc(u32 attr, u32 type) {
    if (attr < kNumAttrs) {
        g_state.vtxDesc[attr] = type;
    }
}
void GXSetArray(u32 attr, const void* base, u8 stride) {
    if (attr < kNumAttrs) {
        g_state.arrayBase[attr] = PTR_TO_ADDR(base);
        g_state.arrayStride[attr] = stride;
    }
}

// TEV
void GXSetTevKColor(u32 id, GXColor color) { std::memcpy(g_state.kColor[id & 3], &color, 4); }
void GXSetTevKColorSel(u32 stage, u32 sel) { Stage(stage).kcsel = sel; }
void GXSetTevSwapMode(u32 stage, u32 ras, u32 tex) {
    Stage(stage).rasSwap = ras;
    Stage(stage).texSwap = tex;
}
void GXSetTevSwapModeTable(u32 table, u32 r, u32 g, u32 b, u32 a) {
    uint8_t* t = g_state.swapTable[table & 3];
    t[0] = (u8)r, t[1] = (u8)g, t[2] = (u8)b, t[3] = (u8)a;
}
void GXSetNumTevStages(u8 n) { g_state.numTevStages = n; }
void GXSetTevOrder(u32 stage, u32 coord, u32 map, u32 color) {
    TevStage& t = Stage(stage);
    t.texCoord = coord;
    t.texMap = (map & 0x100) ? 0xFF : map;
    t.chan = color;
}
void GXSetTevOp(u32 stage, u32 mode) {
    TevStage& t = Stage(stage);
    TevStage preset;
    SetTevPreset(preset, stage, mode);
    std::memcpy(t.colorIn, preset.colorIn, sizeof(t.colorIn));
    std::memcpy(t.alphaIn, preset.alphaIn, sizeof(t.alphaIn));
    t.colorOp = t.alphaOp = 0;
    t.colorBias = t.alphaBias = 0;
    t.colorScale = t.alphaScale = 0;
    t.colorClamp = t.alphaClamp = 1;
    t.colorOut = t.alphaOut = 0;
}
void GXSetTevColorIn(u32 stage, u32 a, u32 b, u32 c, u32 d) {
    TevStage& t = Stage(stage);
    t.colorIn[0] = a, t.colorIn[1] = b, t.colorIn[2] = c, t.colorIn[3] = d;
}
void GXSetTevAlphaIn(u32 stage, u32 a, u32 b, u32 c, u32 d) {
    TevStage& t = Stage(stage);
    t.alphaIn[0] = a, t.alphaIn[1] = b, t.alphaIn[2] = c, t.alphaIn[3] = d;
}
void GXSetTevColorOp(u32 stage, u32 op, u32 bias, u32 scale, u8 clamp, u32 out) {
    TevStage& t = Stage(stage);
    t.colorOp = op, t.colorBias = bias, t.colorScale = scale, t.colorClamp = clamp, t.colorOut = out;
}
void GXSetTevAlphaOp(u32 stage, u32 op, u32 bias, u32 scale, u8 clamp, u32 out) {
    TevStage& t = Stage(stage);
    t.alphaOp = op, t.alphaBias = bias, t.alphaScale = scale, t.alphaClamp = clamp, t.alphaOut = out;
}
void GXSetAlphaCompare(u32 comp0, u8 ref0, u32 op, u32 comp1, u8 ref1) {
    u32* a = g_state.alphaComp;
    a[0] = comp0, a[1] = ref0, a[2] = op, a[3] = comp1, a[4] = ref1;
}

// Transform
void GXSetProjection(const f32 mtx[4][4], GXProjectionType type) {
    f32* p = g_state.projection;
    p[0] = (f32)type;
    p[1] = mtx[0][0];
    p[3] = mtx[1][1];
    p[5] = mtx[2][2];
    p[6] = mtx[2][3];
    if (type == GX_PERSPECTIVE) {
        p[2] = mtx[0][2];
        p[4] = mtx[1][2];
    } else {
        p[2] = mtx[0][3];
        p[4] = mtx[1][3];
    }
}
void GXGetProjectionv(f32* p) { std::memcpy(p, g_state.projection, sizeof(g_state.projection)); }
void GXSetProjectionv(const f32* p) { std::memcpy(g_state.projection, p, sizeof(g_state.projection)); }
void GXSetCurrentMtx(u32 id) { g_state.currentPosMtx = id; }
void GXLoadPosMtxImm(const f32 mtx[3][4], u32 id) { std::memcpy(&g_state.xfMtx[(id & 63) * 4], mtx, 48); }
void GXLoadNrmMtxImm(const f32 mtx[3][4], u32 id) {
    f32* n = &g_state.xfNrm[(id & 31) * 3];
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            n[r * 3 + c] = mtx[r][c];
        }
    }
}
void GXLoadTexMtxImm(const f32 mtx[][4], u32 id, u32 type) {
    u32 rows = type == 1 ? 2 : 3;
    if (id >= 64) {
        std::memcpy(&g_state.xfPostMtx[((id - 64) & 63) * 4], mtx, rows * 16);
        return;
    }
    std::memcpy(&g_state.xfMtx[(id & 63) * 4], mtx, rows * 16);
    if (rows == 2) {  // a 2x4 matrix's third row reads as (0, 0, 0, 1)
        f32* m = &g_state.xfMtx[(id & 63) * 4 + 8];
        if ((id & 63) * 4 + 11 < 256) {
            m[0] = m[1] = m[2] = 0.0f;
            m[3] = 1.0f;
        }
    }
}
void GXLoadPosMtxIndx(u16 index, u32 id) {
    const u8* src = ADDR_TO_PTR(const u8, g_state.arrayBase[kPosMtxArray] + index * g_state.arrayStride[kPosMtxArray]);
    std::memcpy(&g_state.xfMtx[(id & 63) * 4], src, 48);
}
void GXSetViewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz) {
    f32* v = g_state.viewport;
    v[0] = left, v[1] = top, v[2] = wd, v[3] = ht, v[4] = nearz, v[5] = farz;
}
void GXSetViewportJitter(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz, u32) {
    GXSetViewport(left, top, wd, ht, nearz, farz);
}
void GXSetScissor(u32 left, u32 top, u32 wd, u32 ht) {
    u32* s = g_state.scissor;
    s[0] = left, s[1] = top, s[2] = wd, s[3] = ht;
}

// Lighting channels and texture coordinates
void GXSetNumChans(u8 n) { g_state.numChans = n; }
void GXSetChanCtrl(u32 chan, u8 enable, u32 ambSrc, u32 matSrc, u32 lightMask, u32 diffFn, u32 attnFn) {
    ChanCtrl c = {enable != 0, ambSrc, matSrc, lightMask, diffFn, attnFn};
    if (chan == 4 || chan == 5) {
        g_state.chan[chan - 4] = c;
        g_state.chan[chan - 2] = c;
    } else if (chan < 4) {
        g_state.chan[chan] = c;
    }
}
static void SetChanColor(u8 (*dst)[4], u32 chan, GXColor color) {
    const u8 c[4] = {color.r, color.g, color.b, color.a};
    u32 index = chan & 1;
    if (chan == 0 || chan == 1 || chan >= 4) {
        std::memcpy(dst[index], c, 3);
    }
    if (chan == 2 || chan == 3 || chan >= 4) {
        dst[index][3] = c[3];
    }
}
void GXSetChanAmbColor(u32 chan, GXColor color) { SetChanColor(g_state.ambColor, chan, color); }
void GXSetChanMatColor(u32 chan, GXColor color) { SetChanColor(g_state.matColor, chan, color); }
void GXSetNumTexGens(u8 n) { g_state.numTexGens = n; }
void GXSetTexCoordGen2(u32 dst, u32 func, u32 src, u32 mtx, u8 normalize, u32 postMtx) {
    if (dst < 8) {
        g_state.texGen[dst] = {func, src, mtx, normalize != 0, postMtx};
    }
}

// Pixel engine
void GXSetBlendMode(u32 type, u32 src, u32 dst, u32 op) {
    u32* b = g_state.blend;
    b[0] = type, b[1] = src, b[2] = dst, b[3] = op;
}
void GXSetZMode(u8 enable, u32 func, u8 update) {
    g_state.zEnable = enable != 0;
    g_state.zFunc = func;
    g_state.zUpdate = update != 0;
}
void GXSetZCompLoc(u8 beforeTex) { g_state.zBeforeTex = beforeTex != 0; }
void GXSetCullMode(u32 mode) { g_state.cullMode = mode; }
void GXSetFog(u32 type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color) {
    g_state.fogType = type;
    f32* f = g_state.fog;
    f[0] = startz, f[1] = endz, f[2] = nearz, f[3] = farz;
    std::memcpy(g_state.fogColor, &color, 4);
}

// Drawing
void GXBegin(u32 type, u32 fmt, u16 nverts) {
    frontline::os::SafePoint();
    g_imm.prim = type;
    g_imm.fmt = fmt & 7;
    g_imm.count = nverts;
    g_imm.used = 0;
    g_imm.size = nverts * VertexSize(fmt);
}
void GXCallDisplayList(void* list, u32 nbytes) {
    frontline::os::SafePoint();
    RunDisplayList(static_cast<const u8*>(list), nbytes);
}

}  // extern "C"
