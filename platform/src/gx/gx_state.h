// The GX state the renderer works from: what the GX* calls and display-list
// register writes set, laid out for the vertex stage (gx_vertex.cpp) and the
// TEV shader (gx_metal.mm). Values are the SDK's enums.
#pragma once

#include <cstdint>

#include "dolphin/gx.h"

namespace frontline::gx {

enum Attr : uint32_t {
    kPnMtxIdx = 0,
    kTex0MtxIdx = 1,  // .. kTex7MtxIdx = 8
    kPos = 9,
    kNrm = 10,
    kClr0 = 11,
    kClr1 = 12,
    kTex0 = 13,  // .. kTex7 = 20
    kPosMtxArray = 21,
    kNrmMtxArray = 22,
    kTexMtxArray = 23,
    kLightArray = 24,
    kNumAttrs = 25,
};

struct AttrFmt {
    uint32_t cnt;
    uint32_t type;
    uint8_t frac;
};

struct Light {
    float color[4];  // 0..1
    float pos[3];
    float dir[3];
    float a[3];  // angle attenuation
    float k[3];  // distance attenuation
};

// GXLightObj's 64 bytes natively.
struct LightObj {
    uint8_t color[4];
    float pos[3];
    float dir[3];
    float a[3];
    float k[3];
};
static_assert(sizeof(LightObj) <= sizeof(GXLightObj));

struct ChanCtrl {
    bool lighting;
    uint32_t ambSrc;  // 0 register, 1 vertex
    uint32_t matSrc;
    uint32_t lightMask;
    uint32_t diffFn;  // 0 none, 1 sign, 2 clamp
    uint32_t attnFn;  // 0 specular, 1 spot, 2 none
};

struct TexGen {
    uint32_t func;  // 0 MTX3x4, 1 MTX2x4, 2..9 bump, 10 SRTG
    uint32_t src;   // 0 pos, 1 nrm, 4..11 tex0..7, 19/20 color
    uint32_t mtx;   // texture matrix id (30, 33, ...; 60 identity)
    bool normalize;
    uint32_t postMtx;
};

struct TevStage {
    uint32_t colorIn[4];
    uint32_t alphaIn[4];
    uint32_t colorOp, colorBias, colorScale, colorClamp, colorOut;
    uint32_t alphaOp, alphaBias, alphaScale, alphaClamp, alphaOut;
    uint32_t texCoord;  // 0xFF none
    uint32_t texMap;    // 0xFF none
    uint32_t chan;      // GXChannelID
    uint32_t kcsel, kasel;
    uint32_t rasSwap, texSwap;
    // Indirect
    uint32_t indStage, indFormat, indBias, indMtx, indWrapS, indWrapT;
    bool indAddPrev, indUtcLod;
    uint32_t indAlphaSel;
    bool indirect;
};

// GXTexObj's 32 bytes natively.
struct TexObj {
    uint32_t image;  // 32-bit address
    uint16_t width, height;
    uint8_t format;
    uint8_t wrapS, wrapT;
    uint8_t mipmap;
    uint8_t isCI;
    uint8_t minFilter, magFilter;
    uint8_t maxLod;
    uint32_t tlut;  // TLUT name
};
static_assert(sizeof(TexObj) <= sizeof(GXTexObj));

struct Tlut {
    uint32_t addr;
    uint32_t format;
    uint32_t count;
};

struct State {
    // Vertex format
    uint32_t vtxDesc[kNumAttrs];  // 0 none, 1 direct, 2 index8, 3 index16
    AttrFmt vat[8][kNumAttrs];
    uint32_t arrayBase[kNumAttrs];  // 32-bit addresses
    uint32_t arrayStride[kNumAttrs];

    // Transform memory: position/texture matrices (word addresses 0..255) and
    // normal matrices (0..95), as the XF unit keeps them.
    float xfMtx[256];
    float xfNrm[96];
    float xfPostMtx[256];
    uint32_t currentPosMtx;
    uint32_t currentTexMtx[8];
    float projection[7];  // type, then the 6 terms (GXGetProjectionv)

    float viewport[6];  // left, top, width, height, near, far
    uint32_t scissor[4];

    // Lighting
    uint32_t numChans;
    ChanCtrl chan[4];  // color0, color1, alpha0, alpha1
    uint8_t ambColor[2][4];
    uint8_t matColor[2][4];
    Light lights[8];

    // Texture coordinates
    uint32_t numTexGens;
    TexGen texGen[8];

    // TEV
    uint32_t numTevStages;
    TevStage tev[16];
    uint8_t kColor[4][4];
    uint8_t swapTable[4][4];
    uint32_t alphaComp[5];  // comp0, ref0, op, comp1, ref1

    // Indirect
    uint32_t numIndStages;
    uint32_t indOrder[4][2];  // texcoord, texmap
    uint32_t indScale[4][2];
    float indMtx[3][2][3];
    int indMtxScale[3];

    // Pixel engine
    uint32_t blend[4];  // type, src, dst, op
    bool zEnable;
    uint32_t zFunc;
    bool zUpdate;
    bool zBeforeTex;
    bool colorUpdate;
    uint32_t cullMode;
    uint32_t fogType;
    float fog[4];  // startz, endz, nearz, farz
    uint8_t fogColor[4];
    uint8_t clearColor[4];
    uint32_t clearZ;

    // Textures
    TexObj texMap[8];
    Tlut tluts[20];
};

extern State g_state;

void ResetState();

}  // namespace frontline::gx
