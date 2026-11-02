// Rewritten's blob shadows (native/shadows.h).
//
// CCompartment::DrawShadows queues, for each character's shadow, the visible
// compartment nodes its slab reaches (CCharacterShadow::Draw builds the slab
// under the character and fades it with distance); each queued entry is the
// shadow and a node. Here every chunk of those nodes is drawn again over
// itself, darkening what's there by the blob: the level's shadow texture (a
// soft disk) projected straight down through the slab (a texture matrix on
// the world position) and fading out above and below it, times the shadow's
// fade, wherever the chunk's own texture passes the opaque bin's alpha test
// (so a fence darkens only where it's solid). The redraw is in the world bin's scope (native/render.h), so with
// the Modern renderer the chunks come from the same meshes at the same
// depths, and the depth test (less or equal, no write) keeps the shadow on
// what's in front.

#include "dolphin/gx.h"
#include "game/box.h"
#include "game/compartment.h"
#include "game/gameconfig.h"
#include "game/renderbin.h"
#include "game/shadow.h"
#include "game/texture.h"
#include "native/hooks.h"
#include "native/render.h"
#include "native/shadows.h"

namespace {

// What CCptShadowBin::Render queues: a shadow and a node it reaches.
struct Queued {
    PTR32(CCharacterShadow) shadow;
    PTR32(CPTNode) node;
};

// The opaque bin's alpha test (CCPTMatBin::Link: a > 0xA0).
const u8 kAlphaTest = 0xA0;

// GX's state for the redraw, but for the textures, the texture matrix, the
// fade and each chunk's vertex format and cull mode.
void SetUp(const float view[3][4]) {
    GXLoadPosMtxImm(view, 0);  // the compartments' positions are in world space
    GXSetCurrentMtx(0);
    GXSetVtxAttrFmt(0, 9, 1, 4, 0);   // xyz f32
    GXSetVtxAttrFmt(0, 10, 0, 1, 0);  // normal s8
    GXSetVtxAttrFmt(0, 11, 1, 5, 0);  // rgba8
    GXSetVtxAttrFmt(0, 13, 1, 4, 0);  // st f32
    GXSetNumChans(0);
    GXSetNumTexGens(3);
    GXSetTexCoordGen2(0, 1, 0, 30, 0, 125);  // 0: the blob's, GX_TEXMTX0 x the position
    GXSetTexCoordGen2(1, 1, 4, 60, 0, 125);  // 1: the chunk's own (TEX0)
    GXSetTexCoordGen2(2, 1, 0, 33, 0, 125);  // 2: the height in the slab, GX_TEXMTX1 x the position
    // The level's blob is a soft disk in its color, white in the middle and
    // dark at the rim, and its alpha marks the disk with a hard edge. Around
    // the disk the color isn't black: a CMPR block's transparent texels keep
    // the average of its two colors. So the blob's strength is its red (read
    // as alpha through swap table 1) times its alpha, and with the clamp,
    // everything past the slab reads the transparent edge and gets nothing.
    GXSetTevSwapModeTable(0, 0, 1, 2, 3);
    GXSetTevSwapModeTable(1, 0, 1, 2, 0);
    GXSetNumTevStages(6);
    for (int stage = 0; stage < 6; stage++) {  // (the color goes unused: the blend reads alpha)
        GXSetTevColorIn(stage, 15, 15, 15, 15);
        GXSetTevColorOp(stage, 0, 0, 0, 1, 0);
        GXSetTevSwapMode(stage, 0, stage == 1 || stage == 3 ? 1 : 0);
    }
    // 0: the chunk texture's alpha (else 1) into APREV.
    GXSetTevOrder(0, 1, 1, 0xFF);
    GXSetTevAlphaOp(0, 0, 0, 0, 1, 0);
    GXSetTevKAlphaSel(0, 0x00);  // 1
    // 1, 2: the blob's red times the fade (K0's alpha), times its alpha,
    // into A1.
    GXSetTevOrder(1, 0, 0, 0xFF);
    GXSetTevAlphaIn(1, 7, 4, 6, 7);
    GXSetTevKAlphaSel(1, 0x1C);  // K0's alpha
    GXSetTevAlphaOp(1, 0, 0, 0, 1, 2);
    GXSetTevOrder(2, 0, 0, 0xFF);
    GXSetTevAlphaIn(2, 7, 2, 4, 7);
    GXSetTevAlphaOp(2, 0, 0, 0, 1, 2);
    // 3, 4: A1 times the blob's middle column at the height in the slab
    // (red, then alpha): the same falloff up and down, so what's well above
    // or below (a sea floor seen through water) gets nothing.
    GXSetTevOrder(3, 2, 0, 0xFF);
    GXSetTevAlphaIn(3, 7, 2, 4, 7);
    GXSetTevAlphaOp(3, 0, 0, 0, 1, 2);
    GXSetTevOrder(4, 2, 0, 0xFF);
    GXSetTevAlphaIn(4, 7, 2, 4, 7);
    GXSetTevAlphaOp(4, 0, 0, 0, 1, 2);
    // 5: A1 where APREV passes the alpha test (K1's alpha), else nothing.
    GXSetTevOrder(5, 0xFF, 0xFF, 0xFF);
    GXSetTevAlphaIn(5, 0, 6, 2, 7);
    GXSetTevKAlphaSel(5, 0x1D);  // K1's alpha
    GXSetTevAlphaOp(5, 14, 0, 0, 1, 0);  // GX_TEV_COMP_A8_GT
    GXColor test = {0, 0, 0, kAlphaTest};
    GXSetTevKColor(1, test);
    GXSetAlphaCompare(4, 0, 0, 7, 0);   // drawn where it darkens at all
    GXSetBlendMode(1, 0, 5, 0);         // what's there times (1 - alpha)
    GXSetZMode(1, 3, 0);                // less or equal, no write
    GXSetZCompLoc(1);
    GXSetColorUpdate(1);
    GXColor none = {0, 0, 0, 0};
    GXSetFog(0, 0.0f, 0.0f, 0.0f, 0.0f, none);
}

// The blob's coordinates for `shadow`: across its slab, 0..1 along the box's
// x and y axes (straight down through it along z), into GX_TEXMTX0; the
// height in the slab, 0..1 along its z axis down the blob's middle column,
// into GX_TEXMTX1; and its fade into K0.
void SetShadow(const CCharacterShadow& shadow) {
    const CVolBox& box = shadow.m_box;
    const float width = 2.0f * box.m_halfSize[0], depth = 2.0f * box.m_halfSize[1],
                height = 2.0f * box.m_halfSize[2];
    if (!(width > 0.0f) || !(depth > 0.0f) || !(height > 0.0f)) {
        return;
    }
    const CVector3& c = box.m_center;
    const CVector3& x = box.m_axisX;
    const CVector3& y = box.m_axisY;
    const CVector3& z = box.m_axisZ;
    f32 m[2][4] = {
        {x.x / width, x.y / width, x.z / width, 0.5f - (c.x * x.x + c.y * x.y + c.z * x.z) / width},
        {y.x / depth, y.y / depth, y.z / depth, 0.5f - (c.x * y.x + c.y * y.y + c.z * y.z) / depth},
    };
    GXLoadTexMtxImm(m, 30, 1);  // GX_TEXMTX0, GX_MTX2x4
    f32 h[2][4] = {
        {0.0f, 0.0f, 0.0f, 0.5f},
        {z.x / height, z.y / height, z.z / height, 0.5f - (c.x * z.x + c.y * z.y + c.z * z.z) / height},
    };
    GXLoadTexMtxImm(h, 33, 1);  // GX_TEXMTX1, GX_MTX2x4
    float alpha = shadow.m_alpha;
    alpha = alpha < 0.0f ? 0.0f : alpha > 1.0f ? 1.0f : alpha;
    GXColor fade = {0, 0, 0, (u8)(255.0f * alpha + 0.5f)};
    GXSetTevKColor(0, fade);
}

// A node's chunks again, darkened.
void DrawNode(const CPTNode& node) {
    static int indexType;  // the vertex descriptors' (1 8-bit, 2 16-bit indices)
    CPTChunk* chunk = node.m_chunks;
    for (int i = 0; i < node.m_numChunks; i++, chunk++) {
        CPTDisplayList* list = chunk->m_list;
        CCPTMatBin* material = chunk->m_material;
        // Not on blended materials (water, glass): darkening them darkens
        // what's seen through them too. (m_type: the bin, native/shadows.h.)
        if (list == NULL || material == NULL || material->m_type == 4) {
            continue;
        }
        if (material->m_shape != NULL) {
            if (material->m_hasTlut != 0) {
                GXLoadTlut(&material->m_tlut, 0);
            }
            GXLoadTexObj(&material->m_texObj, 1);
            GXSetTevAlphaIn(0, 7, 7, 7, 4);  // its alpha
        } else {
            GXSetTevAlphaIn(0, 7, 7, 7, 6);  // 1
        }
        GXSetCullMode((material->m_flags & CPTMAT_TWO_SIDED) ^ 1);
        const int type = (list->m_flags & 1) ? 2 : 1;
        if (type != indexType) {
            const u32 desc = type == 2 ? 3 : 2;  // GX_INDEX16, GX_INDEX8
            GXClearVtxDesc();
            GXSetVtxDesc(9, desc);
            GXSetVtxDesc(10, desc);
            GXSetVtxDesc(11, desc);
            GXSetVtxDesc(13, desc);
            indexType = type;
        }
        GXCallDisplayList(list->m_list, list->m_size);
    }
    indexType = 0;  // (GX's state goes back after the bin)
}

}  // namespace

void NativeBlobShadowLevelStart() { g_config.noCharacterShadows = NativeBlobShadows() == 0 ? 1 : 0; }

void NativeBlobShadowRender(CDmaPacket& packet, CPTNode* node, CCharacterShadow* shadow) {
    if (NativeBlobShadows() == 0 || node == NULL || shadow == NULL) {
        return;
    }
    Queued* queued = (Queued*)packet.m_ptr;
    queued->shadow = shadow;
    queued->node = node;
    packet.m_ptr += sizeof(Queued);
}

void NativeBlobShadowLink(CDmaTag* tag, CTexture* texture, const float view[3][4]) {
    if (NativeBlobShadows() == 0 || tag == NULL || texture == NULL) {
        return;
    }
    NativeGxSaveState();
    NativeMeshBin(NATIVE_BIN_WORLD);
    SetUp(view);
    // The blob, clamped: it's drawn over whole chunks, and past the slab the
    // clamp reads its edge, which is empty (SetUp). Not mipmapped, and
    // nearest when it's smaller than its texels: the smaller levels
    // (Rewritten makes them, and filters them anisotropically) blur the disk
    // into its edge, and far off it would run out in streaks.
    GXTexObj blob = texture->m_texObj;
    GXInitTexObjWrapMode(&blob, 0, 0);                           // GX_CLAMP
    GXInitTexObjLOD(&blob, 0, 1, 0.0f, 0.0f, 0.0f, 0, 0, 0);     // GX_NEAR, GX_LINEAR, no levels
    if (texture->m_isCI != 0) {
        GXInitTexObjTlut(&blob, 1);  // the chunks' palettes go to 0 (DrawNode)
        GXLoadTlut(&texture->m_tlut, 1);
    }
    GXLoadTexObj(&blob, 0);
    CCharacterShadow* current = NULL;
    for (; tag != NULL; tag = tag->m_next) {
        const Queued* queued = (const Queued*)tag->m_data;
        CCharacterShadow* shadow = queued->shadow;
        if (shadow != current) {
            SetShadow(*shadow);
            current = shadow;
        }
        DrawNode(*(CPTNode*)queued->node);
    }
    NativeMeshBin(NATIVE_BIN_NONE);
    NativeGxRestoreState();
}
