// The GX backend on Metal.
//
// Draws go into an offscreen EFB (the console's embedded frame buffer) as the
// game makes them, on the game thread. GXCopyDisp copies the EFB into a texture
// kept for that frame buffer's address, which the video output (VI) shows.
// Textures decode once into a cache. Nothing here uses operator new (the game
// owns it on game threads); Objective-C collections allocate with malloc.

#import <Metal/Metal.h>
#include <os/lock.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// The game's headers define BOOL as int; Objective-C's is bool.
#define BOOL FRONTLINE_GAME_BOOL
#include "frontline/platform.h"
#include "gx_metal.h"
#include "gx_shader.h"
#include "gx_state.h"
#include "gx_texture.h"
#include "gx_vertex.h"
#include "types.h"
#undef BOOL

namespace frontline::gx {
namespace {

constexpr uint32_t kEfbWidth = 640;
constexpr uint32_t kEfbHeight = 528;
constexpr NSUInteger kVertexBufferSize = 32u << 20;

id<MTLDevice> g_device;
id<MTLCommandQueue> g_queue;
id<MTLLibrary> g_library;
id<MTLTexture> g_efbColor;
id<MTLTexture> g_efbDepth;
NSMutableDictionary<NSNumber*, id<MTLRenderPipelineState>>* g_pipelines;
NSMutableDictionary<NSNumber*, id<MTLDepthStencilState>>* g_depthStates;
NSMutableDictionary<NSNumber*, id<MTLSamplerState>>* g_samplers;
NSMutableDictionary<NSNumber*, NSArray*>* g_textures;  // key -> [contents hash, texture]
NSMutableArray<id<MTLBuffer>>* g_freeBuffers;
os_unfair_lock g_bufferLock = OS_UNFAIR_LOCK_INIT;

// Frame buffer textures by address (the game uses two). The console's copy
// writes the frame into the frame buffer's memory; this one writes only a
// stamp there, so a frame the CPU wrote since (the movie player's YUV) shows
// instead of the texture (see XfbTexture).
struct Xfb {
    uint32_t addr;
    uint32_t stamp;
    id<MTLTexture> texture;
};
constexpr uint32_t kXfbMagic = 0x46584742;  // "FXGB"

bool Stamped(const Xfb& x) {
    const uint32_t* words = ADDR_TO_PTR(const uint32_t, x.addr);
    return words[0] == kXfbMagic && words[1] == x.stamp;
}
Xfb g_xfbs[4];
os_unfair_lock g_xfbLock = OS_UNFAIR_LOCK_INIT;

id<MTLCommandBuffer> g_commands;
id<MTLRenderCommandEncoder> g_encoder;
id<MTLCommandBuffer> g_lastCommitted;
NSMutableArray<id<MTLBuffer>>* g_frameBuffers;  // vertex memory used by g_commands
id<MTLBuffer> g_vertexBuffer;
NSUInteger g_vertexUsed;
// A frame copy's clear, done when the next pass starts. As on the console it
// clears only the planes being updated: the game clears just Z mid-frame by
// copying with color updates off (CDMRenderBin::Link, before the weapon).
bool g_clearColorPending = true, g_clearDepthPending = true;
uint8_t g_pendingClearColor[4];
uint32_t g_pendingClearZ = 0xFFFFFF;

// FRONTLINE_GX_TRACE=1: a line per frame copy with what was drawn.
int g_trace = -1;
uint32_t g_drawCalls, g_triangles;
State g_tracePersp;  // the state at the frame's first perspective draw
bool g_tracePerspSet;
uint16_t g_copySrc[4] = {0, 0, 640, 448};

void Init() {
    if (g_device != nil) {
        return;
    }
    g_device = MTLCreateSystemDefaultDevice();
    g_queue = [g_device newCommandQueue];
    NSError* error = nil;
    g_library = [g_device newLibraryWithSource:[NSString stringWithUTF8String:kShaderSource] options:nil error:&error];
    if (g_library == nil) {
        std::fprintf(stderr, "frontline: TEV shader: %s\n", error.localizedDescription.UTF8String);
        std::abort();
    }
    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                    width:kEfbWidth
                                                                                   height:kEfbHeight
                                                                                mipmapped:NO];
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModePrivate;
    g_efbColor = [g_device newTextureWithDescriptor:desc];
    desc.pixelFormat = MTLPixelFormatDepth32Float;
    desc.usage = MTLTextureUsageRenderTarget;
    g_efbDepth = [g_device newTextureWithDescriptor:desc];
    g_pipelines = [NSMutableDictionary dictionary];
    g_depthStates = [NSMutableDictionary dictionary];
    g_samplers = [NSMutableDictionary dictionary];
    g_textures = [NSMutableDictionary dictionary];
    g_freeBuffers = [NSMutableArray array];
}

id<MTLBuffer> AcquireBuffer() {
    os_unfair_lock_lock(&g_bufferLock);
    id<MTLBuffer> buffer = g_freeBuffers.lastObject;
    if (buffer != nil) {
        [g_freeBuffers removeLastObject];
    }
    os_unfair_lock_unlock(&g_bufferLock);
    if (buffer == nil) {
        buffer = [g_device newBufferWithLength:kVertexBufferSize options:MTLResourceStorageModeShared];
    }
    [g_frameBuffers addObject:buffer];
    return buffer;
}

// Copies `size` bytes into this frame's vertex memory; returns the buffer and offset.
id<MTLBuffer> Upload(const void* data, NSUInteger size, NSUInteger* offset) {
    size = (size + 255) & ~NSUInteger(255);
    if (g_vertexBuffer == nil || g_vertexUsed + size > kVertexBufferSize) {
        if (size > kVertexBufferSize) {
            return nil;
        }
        g_vertexBuffer = AcquireBuffer();
        g_vertexUsed = 0;
    }
    *offset = g_vertexUsed;
    std::memcpy(static_cast<uint8_t*>(g_vertexBuffer.contents) + g_vertexUsed, data, size);
    g_vertexUsed += size;
    return g_vertexBuffer;
}

void BeginPass() {
    if (g_encoder != nil) {
        return;
    }
    Init();
    if (g_commands == nil) {
        g_commands = [g_queue commandBuffer];
        g_frameBuffers = [NSMutableArray array];
        g_vertexBuffer = nil;
    }
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = g_efbColor;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.depthAttachment.texture = g_efbDepth;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    if (g_clearColorPending) {
        const uint8_t* c = g_pendingClearColor;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(c[0] / 255.0, c[1] / 255.0, c[2] / 255.0, 1.0);
    } else {
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    }
    if (g_clearDepthPending) {
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.clearDepth = (g_pendingClearZ & 0xFFFFFF) / 16777215.0;
    } else {
        pass.depthAttachment.loadAction = MTLLoadActionLoad;
    }
    g_clearColorPending = g_clearDepthPending = false;
    g_encoder = [g_commands renderCommandEncoderWithDescriptor:pass];
}

void EndPass() {
    if (g_encoder != nil) {
        [g_encoder endEncoding];
        g_encoder = nil;
    }
}

void Commit() {
    EndPass();
    if (g_commands == nil) {
        return;
    }
    NSArray<id<MTLBuffer>>* used = g_frameBuffers;
    [g_commands addCompletedHandler:^(id<MTLCommandBuffer>) {
        os_unfair_lock_lock(&g_bufferLock);
        [g_freeBuffers addObjectsFromArray:used];
        os_unfair_lock_unlock(&g_bufferLock);
    }];
    [g_commands commit];
    g_lastCommitted = g_commands;
    g_commands = nil;
    g_frameBuffers = nil;
    g_vertexBuffer = nil;
}

MTLBlendFactor SrcFactor(uint32_t f) {
    static const MTLBlendFactor kMap[8] = {
        MTLBlendFactorZero,           MTLBlendFactorOne,
        MTLBlendFactorDestinationColor, MTLBlendFactorOneMinusDestinationColor,
        MTLBlendFactorSourceAlpha,    MTLBlendFactorOneMinusSourceAlpha,
        MTLBlendFactorDestinationAlpha, MTLBlendFactorOneMinusDestinationAlpha,
    };
    return kMap[f & 7];
}

MTLBlendFactor DstFactor(uint32_t f) {
    static const MTLBlendFactor kMap[8] = {
        MTLBlendFactorZero,      MTLBlendFactorOne,
        MTLBlendFactorSourceColor, MTLBlendFactorOneMinusSourceColor,
        MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha,
        MTLBlendFactorDestinationAlpha, MTLBlendFactorOneMinusDestinationAlpha,
    };
    return kMap[f & 7];
}

id<MTLRenderPipelineState> Pipeline() {
    const State& s = g_state;
    uint32_t type = s.blend[0];
    uint32_t key = (type & 3) | (s.blend[1] & 7) << 2 | (s.blend[2] & 7) << 5 | (s.colorUpdate ? 1u : 0u) << 8;
    NSNumber* k = @(key);
    id<MTLRenderPipelineState> pipeline = g_pipelines[k];
    if (pipeline != nil) {
        return pipeline;
    }
    MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction = [g_library newFunctionWithName:@"tev_vertex"];
    desc.fragmentFunction = [g_library newFunctionWithName:@"tev_fragment"];
    desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    // RGB8 EFB: no destination alpha to write.
    desc.colorAttachments[0].writeMask = s.colorUpdate ? MTLColorWriteMaskRed | MTLColorWriteMaskGreen |
                                                             MTLColorWriteMaskBlue
                                                       : MTLColorWriteMaskNone;
    if (type == 1 || type == 3) {
        auto* att = desc.colorAttachments[0];
        att.blendingEnabled = YES;
        if (type == 3) {
            att.sourceRGBBlendFactor = att.sourceAlphaBlendFactor = MTLBlendFactorOne;
            att.destinationRGBBlendFactor = att.destinationAlphaBlendFactor = MTLBlendFactorOne;
            att.rgbBlendOperation = att.alphaBlendOperation = MTLBlendOperationReverseSubtract;
        } else {
            att.sourceRGBBlendFactor = att.sourceAlphaBlendFactor = SrcFactor(s.blend[1]);
            att.destinationRGBBlendFactor = att.destinationAlphaBlendFactor = DstFactor(s.blend[2]);
        }
    }
    NSError* error = nil;
    pipeline = [g_device newRenderPipelineStateWithDescriptor:desc error:&error];
    if (pipeline == nil) {
        std::fprintf(stderr, "frontline: pipeline: %s\n", error.localizedDescription.UTF8String);
        std::abort();
    }
    g_pipelines[k] = pipeline;
    return pipeline;
}

id<MTLDepthStencilState> DepthState() {
    const State& s = g_state;
    uint32_t key = (s.zEnable ? 1u : 0u) | (s.zFunc & 7) << 1 | (s.zUpdate ? 1u : 0u) << 4;
    NSNumber* k = @(key);
    id<MTLDepthStencilState> state = g_depthStates[k];
    if (state == nil) {
        MTLDepthStencilDescriptor* desc = [MTLDepthStencilDescriptor new];
        desc.depthCompareFunction = s.zEnable ? (MTLCompareFunction)(s.zFunc & 7) : MTLCompareFunctionAlways;
        desc.depthWriteEnabled = s.zEnable && s.zUpdate;
        state = [g_device newDepthStencilStateWithDescriptor:desc];
        g_depthStates[k] = state;
    }
    return state;
}

MTLSamplerAddressMode Wrap(uint32_t w) {
    return w == 1 ? MTLSamplerAddressModeRepeat : w == 2 ? MTLSamplerAddressModeMirrorRepeat
                                                         : MTLSamplerAddressModeClampToEdge;
}

id<MTLSamplerState> Sampler(const TexObj& t, bool mipmapped) {
    uint32_t key = t.wrapS | t.wrapT << 2 | t.minFilter << 4 | t.magFilter << 8 | (mipmapped ? 1u : 0u) << 12;
    NSNumber* k = @(key);
    id<MTLSamplerState> sampler = g_samplers[k];
    if (sampler == nil) {
        MTLSamplerDescriptor* desc = [MTLSamplerDescriptor new];
        desc.sAddressMode = Wrap(t.wrapS);
        desc.tAddressMode = Wrap(t.wrapT);
        desc.magFilter = t.magFilter == 0 ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
        // GX min filters: 0 near, 1 linear, 2 near-mip-near, 3 lin-mip-near, 4 near-mip-lin, 5 lin-mip-lin.
        desc.minFilter = (t.minFilter & 1) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        desc.mipFilter = !mipmapped || t.minFilter < 2 ? MTLSamplerMipFilterNotMipmapped
                         : t.minFilter >= 4            ? MTLSamplerMipFilterLinear
                                                       : MTLSamplerMipFilterNearest;
        sampler = [g_device newSamplerStateWithDescriptor:desc];
        g_samplers[k] = sampler;
    }
    return sampler;
}

uint64_t Hash(const uint8_t* p, uint32_t size) {
    uint64_t h = 0xcbf29ce484222325ull;
    uint32_t step = size > 4096 ? size / 1024 : 1;
    for (uint32_t i = 0; i < size; i += step) {
        h = (h ^ p[i]) * 0x100000001b3ull;
    }
    return h;
}

// The decoded texture for a texture map, cached by address, format, size and
// palette. A sample of the contents is checked each time: the movie player
// rewrites its textures in place every frame, so a changed texture is decoded
// again into a new one (an in-flight frame may still read the old one).
id<MTLTexture> Texture(const TexObj& t, uint32_t* levelsOut) {
    if (t.image == 0 || t.width == 0 || t.height == 0) {
        return nil;
    }
    const uint8_t* src = ADDR_TO_PTR(const uint8_t, t.image);
    uint32_t levels = t.mipmap ? t.maxLod + 1u : 1u;
    uint32_t total = 0;
    for (uint32_t l = 0, w = t.width, h = t.height; l < levels; l++, w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1) {
        total += TextureLevelSize(t.format, w, h);
    }
    const Tlut* tlut = t.isCI ? &g_state.tluts[t.tlut % 20] : nullptr;
    uint64_t key = ((uint64_t)t.image << 32) ^ ((uint64_t)t.format << 24) ^ ((uint64_t)t.width << 12) ^
                   ((uint64_t)t.height) ^ ((uint64_t)levels << 60);
    uint64_t contents = Hash(src, total);
    if (tlut != nullptr && tlut->addr != 0) {
        key ^= ((uint64_t)tlut->addr << 8) * 31 + tlut->format;
        contents ^= Hash(ADDR_TO_PTR(const uint8_t, tlut->addr), tlut->count * 2) * 31;
    }
    NSNumber* k = @(key);
    NSArray* entry = g_textures[k];
    *levelsOut = levels;
    if (entry != nil && [entry[0] unsignedLongLongValue] == contents) {
        return entry[1];
    }
    id<MTLTexture> texture;
    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                    width:t.width
                                                                                   height:t.height
                                                                                mipmapped:levels > 1];
    desc.mipmapLevelCount = levels;
    desc.usage = MTLTextureUsageShaderRead;
    texture = [g_device newTextureWithDescriptor:desc];
    uint8_t* rgba = static_cast<uint8_t*>(std::malloc((size_t)t.width * t.height * 4));
    const uint8_t* palette = tlut != nullptr && tlut->addr != 0 ? ADDR_TO_PTR(const uint8_t, tlut->addr) : nullptr;
    uint8_t zeros[2 * 16384] = {};
    for (uint32_t l = 0, w = t.width, h = t.height; l < levels; l++) {
        DecodeTexture(src, t.format, w, h, palette != nullptr ? palette : zeros, tlut != nullptr ? tlut->format : 0,
                      rgba);
        [texture replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:l withBytes:rgba bytesPerRow:w * 4];
        src += TextureLevelSize(t.format, w, h);
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    std::free(rgba);
    g_textures[k] = @[ @(contents), texture ];
    return texture;
}

void PackStage(const TevStage& t, uint32_t* out) {
    out[0] = t.colorIn[0] | t.colorIn[1] << 4 | t.colorIn[2] << 8 | t.colorIn[3] << 12;
    out[1] = t.alphaIn[0] | t.alphaIn[1] << 4 | t.alphaIn[2] << 8 | t.alphaIn[3] << 12;
    out[2] = t.colorOp | t.colorBias << 4 | t.colorScale << 8 | t.colorClamp << 12 | t.colorOut << 16;
    out[3] = t.alphaOp | t.alphaBias << 4 | t.alphaScale << 8 | t.alphaClamp << 12 | t.alphaOut << 16;
    uint32_t chan = t.chan == 0 || t.chan == 2 || t.chan == 4 ? 0 : t.chan == 1 || t.chan == 3 || t.chan == 5 ? 1 : 2;
    uint32_t coord = t.texCoord < 8 ? t.texCoord : 0xFF;
    uint32_t map = t.texMap < 8 ? t.texMap : 0xFF;
    out[4] = coord | map << 8 | chan << 16;
    out[5] = (t.kcsel & 0xFF) | (t.kasel & 0xFF) << 8 | (t.rasSwap & 3) << 16 | (t.texSwap & 3) << 24;
    out[6] = (t.indStage & 3) | (t.indFormat & 3) << 4 | (t.indBias & 7) << 8 | (t.indMtx & 15) << 12 |
             (t.indWrapS & 7) << 16 | (t.indWrapT & 7) << 20 | (t.indAddPrev ? 1u : 0u) << 24 |
             (t.indirect ? 1u : 0u) << 28;
    out[7] = 0;
}

// FRONTLINE_GX_SNAP=copy:draw,draw,... writes the EFB after those draws of
// the frame that ends with that frame copy (counting from 1) to
// $FRONTLINE_GX_SNAP_DIR/snap-<copy>-<draw>.ppm, to see a frame being built.
uint32_t g_copyCount;
void SnapshotCheck() {
    static int frame = -2;
    static uint32_t draws[32];
    static int numDraws;
    if (frame == -2) {
        frame = -1;
        if (const char* spec = std::getenv("FRONTLINE_GX_SNAP")) {
            frame = std::atoi(spec);
            for (const char* p = std::strchr(spec, ':'); p != nullptr && numDraws < 32; p = std::strchr(p + 1, ',')) {
                draws[numDraws++] = (uint32_t)std::atoi(p + 1);
            }
        }
    }
    if (frame < 0 || g_copyCount + 1 != (uint32_t)frame) {
        return;
    }
    for (int i = 0; i < numDraws; i++) {
        if (draws[i] != g_drawCalls) {
            continue;
        }
        EndPass();
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                        width:kEfbWidth
                                                                                       height:kEfbHeight
                                                                                    mipmapped:NO];
        desc.storageMode = MTLStorageModeShared;
        id<MTLTexture> copy = [g_device newTextureWithDescriptor:desc];
        id<MTLBlitCommandEncoder> blit = [g_commands blitCommandEncoder];
        [blit copyFromTexture:g_efbColor toTexture:copy];
        [blit endEncoding];
        Commit();
        [g_lastCommitted waitUntilCompleted];
        std::vector<uint8_t> rgba((size_t)kEfbWidth * kEfbHeight * 4);
        [copy getBytes:rgba.data()
           bytesPerRow:kEfbWidth * 4
            fromRegion:MTLRegionMake2D(0, 0, kEfbWidth, kEfbHeight)
           mipmapLevel:0];
        const char* dir = std::getenv("FRONTLINE_GX_SNAP_DIR");
        char path[1024];
        std::snprintf(path, sizeof(path), "%s/snap-%d-%u.ppm", dir != nullptr ? dir : ".", frame, g_drawCalls);
        if (FILE* f = std::fopen(path, "wb")) {
            std::fprintf(f, "P6\n%u %u\n255\n", kEfbWidth, kEfbHeight);
            for (size_t p = 0; p < rgba.size(); p += 4) {
                std::fwrite(&rgba[p], 1, 3, f);
            }
            std::fclose(f);
        }
    }
}

}  // namespace

void SubmitTriangles(const Vertex* vertices, uint32_t numVertices, const uint32_t* indices, uint32_t numIndices) {
    if (g_trace > 1 && numIndices <= 12) {
        // FRONTLINE_GX_TRACE=2: small draws covering most of the screen (overlays).
        float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
        for (uint32_t i = 0; i < numVertices; i++) {
            float w = vertices[i].pos[3] != 0.0f ? vertices[i].pos[3] : 1.0f;
            minx = std::min(minx, vertices[i].pos[0] / w);
            maxx = std::max(maxx, vertices[i].pos[0] / w);
            miny = std::min(miny, vertices[i].pos[1] / w);
            maxy = std::max(maxy, vertices[i].pos[1] / w);
        }
        if (maxx - minx < 1.6f || maxy - miny < 1.6f) {
            goto traced;
        }
        const float* c = numVertices > 0 ? vertices[0].color[0] : nullptr;
        std::fprintf(stderr, "[gx]   overlay draw %u tris, x %g..%g y %g..%g, blend %u %u %u %u, color0 %g %g %g %g\n",
                     numIndices / 3, minx, maxx, miny, maxy, g_state.blend[0], g_state.blend[1], g_state.blend[2],
                     g_state.blend[3], c ? c[0] : 0, c ? c[1] : 0, c ? c[2] : 0, c ? c[3] : 0);
    }
traced:
    if (g_trace > 0 && !g_tracePerspSet && g_state.projection[0] == 0.0f) {
        g_tracePersp = g_state;
        g_tracePerspSet = true;
    }
    const State& s = g_state;
    g_drawCalls++;
    g_triangles += numIndices / 3;
    if (s.cullMode == 3) {
        return;
    }
    BeginPass();
    NSUInteger vOffset, iOffset;
    id<MTLBuffer> vb = Upload(vertices, numVertices * sizeof(Vertex), &vOffset);
    id<MTLBuffer> ib = Upload(indices, numIndices * 4, &iOffset);
    if (vb == nil || ib == nil) {
        return;
    }

    FragUniforms u;
    std::memset(&u, 0, sizeof(u));
    u.numStages = s.numTevStages < 1 ? 1 : s.numTevStages > 16 ? 16 : s.numTevStages;
    for (uint32_t i = 0; i < u.numStages; i++) {
        PackStage(s.tev[i], u.stages[i]);
    }
    for (int i = 0; i < 4; i++) {
        for (int c = 0; c < 4; c++) {
            u.konst[i][c] = s.kColor[i][c] / 255.0f;
            u.swap[i][c] = s.swapTable[i][c];
        }
    }
    std::memcpy(u.alphaComp, s.alphaComp, sizeof(u.alphaComp));
    u.fogType = s.fogType;
    std::memcpy(u.fog, s.fog, sizeof(u.fog));
    for (int c = 0; c < 4; c++) {
        u.fogColor[c] = s.fogColor[c] / 255.0f;
    }
    u.numIndStages = s.numIndStages;
    for (int m = 0; m < 3; m++) {
        for (int r = 0; r < 2; r++) {
            for (int c = 0; c < 3; c++) {
                u.indMtx[m][r][c] = s.indMtx[m][r][c];
            }
            u.indMtx[m][r][3] = (float)(1 << (s.indMtxScale[m] + 17)) / (float)(1 << 17);
        }
    }
    for (int i = 0; i < 4; i++) {
        u.indOrder[i][0] = s.indOrder[i][0] & 7;
        u.indOrder[i][1] = s.indOrder[i][1] & 7;
        u.indOrder[i][2] = s.indScale[i][0];
        u.indOrder[i][3] = s.indScale[i][1];
    }

    // Texture maps the stages (or indirect stages) read.
    uint32_t used = 0;
    for (uint32_t i = 0; i < u.numStages; i++) {
        if (s.tev[i].texMap < 8) {
            used |= 1u << s.tev[i].texMap;
        }
    }
    for (uint32_t i = 0; i < s.numIndStages && i < 4; i++) {
        used |= 1u << (s.indOrder[i][1] & 7);
    }
    for (uint32_t m = 0; m < 8; m++) {
        if (!(used & (1u << m))) {
            continue;
        }
        uint32_t levels = 1;
        id<MTLTexture> texture = Texture(s.texMap[m], &levels);
        if (texture == nil) {
            continue;
        }
        u.texValid |= 1u << m;
        u.texSize[m][0] = s.texMap[m].width;
        u.texSize[m][1] = s.texMap[m].height;
        [g_encoder setFragmentTexture:texture atIndex:m];
        [g_encoder setFragmentSamplerState:Sampler(s.texMap[m], levels > 1) atIndex:m];
    }

    // Texture coordinate sizes (see FragUniforms::coordSize).
    for (uint32_t i = 0; i < s.numIndStages && i < 4; i++) {
        uint32_t coord = s.indOrder[i][0] & 7, map = s.indOrder[i][1] & 7;
        u.coordSize[coord][0] = s.texMap[map].width;
        u.coordSize[coord][1] = s.texMap[map].height;
    }
    for (uint32_t i = 0; i < u.numStages; i++) {
        uint32_t coord = s.tev[i].texCoord, map = s.tev[i].texMap;
        if (coord < 8 && map < 8) {
            u.coordSize[coord][0] = s.texMap[map].width;
            u.coordSize[coord][1] = s.texMap[map].height;
        }
    }
    for (int c = 0; c < 8; c++) {
        if (u.coordSize[c][0] == 0.0f) {
            u.coordSize[c][0] = u.coordSize[c][1] = 1.0f;
        }
    }
    for (int m = 0; m < 8; m++) {
        if (u.texSize[m][0] == 0.0f) {
            u.texSize[m][0] = u.texSize[m][1] = 1.0f;
        }
    }

    [g_encoder setRenderPipelineState:Pipeline()];
    [g_encoder setDepthStencilState:DepthState()];
    [g_encoder setFrontFacingWinding:MTLWindingClockwise];
    [g_encoder setCullMode:s.cullMode == 1 ? MTLCullModeFront : s.cullMode == 2 ? MTLCullModeBack : MTLCullModeNone];
    MTLViewport vp = {s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3], s.viewport[4], s.viewport[5]};
    [g_encoder setViewport:vp];
    uint32_t sx = s.scissor[0] < kEfbWidth ? s.scissor[0] : kEfbWidth - 1;
    uint32_t sy = s.scissor[1] < kEfbHeight ? s.scissor[1] : kEfbHeight - 1;
    uint32_t sw = sx + s.scissor[2] <= kEfbWidth ? s.scissor[2] : kEfbWidth - sx;
    uint32_t sh = sy + s.scissor[3] <= kEfbHeight ? s.scissor[3] : kEfbHeight - sy;
    if (sw == 0 || sh == 0) {
        return;
    }
    [g_encoder setScissorRect:(MTLScissorRect){sx, sy, sw, sh}];
    [g_encoder setVertexBuffer:vb offset:vOffset atIndex:0];
    [g_encoder setFragmentBytes:&u length:sizeof(u) atIndex:0];
    [g_encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                          indexCount:numIndices
                           indexType:MTLIndexTypeUInt32
                         indexBuffer:ib
                   indexBufferOffset:iOffset];
    SnapshotCheck();
}

void CopyDisp(void* dest, bool clear) {
    Init();
    uint32_t addr = PTR_TO_ADDR(dest);
    if (g_trace < 0) {
        g_trace = std::getenv("FRONTLINE_GX_TRACE") != nullptr ? std::atoi(std::getenv("FRONTLINE_GX_TRACE")) : 0;
    }
    if (g_trace) {
        const State& st = g_tracePersp;
        std::fprintf(stderr,
                     "[gx] copy to %#x (clear %d): %u draws, %u triangles; projection %g [%g %g %g %g %g %g] "
                     "viewport [%g %g %g %g %g %g] scissor [%u %u %u %u]\n",
                     addr, clear, g_drawCalls, g_triangles, st.projection[0], st.projection[1], st.projection[2],
                     st.projection[3], st.projection[4], st.projection[5], st.projection[6], st.viewport[0],
                     st.viewport[1], st.viewport[2], st.viewport[3], st.viewport[4], st.viewport[5], st.scissor[0],
                     st.scissor[1], st.scissor[2], st.scissor[3]);
    }
    g_drawCalls = g_triangles = 0;
    g_copyCount++;
    g_tracePerspSet = false;
    id<MTLTexture> target = nil;
    os_unfair_lock_lock(&g_xfbLock);
    for (Xfb& x : g_xfbs) {
        if (x.addr == addr || x.addr == 0) {
            if (x.texture == nil) {
                MTLTextureDescriptor* desc =
                    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                       width:g_copySrc[2]
                                                                      height:g_copySrc[3]
                                                                   mipmapped:NO];
                desc.usage = MTLTextureUsageShaderRead;
                desc.storageMode = MTLStorageModeShared;
                x.texture = [g_device newTextureWithDescriptor:desc];
            }
            x.addr = addr;
            x.stamp = g_copyCount;
            target = x.texture;
            break;
        }
    }
    os_unfair_lock_unlock(&g_xfbLock);
    if (target == nil) {
        return;
    }
    BeginPass();  // so a copy with no draws still clears
    EndPass();
    id<MTLBlitCommandEncoder> blit = [g_commands blitCommandEncoder];
    uint32_t w = g_copySrc[2] < target.width ? g_copySrc[2] : (uint32_t)target.width;
    uint32_t h = g_copySrc[3] < target.height ? g_copySrc[3] : (uint32_t)target.height;
    [blit copyFromTexture:g_efbColor
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(g_copySrc[0], g_copySrc[1], 0)
               sourceSize:MTLSizeMake(w, h, 1)
                toTexture:target
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    g_clearColorPending = clear && g_state.colorUpdate;
    g_clearDepthPending = clear && g_state.zUpdate;
    std::memcpy(g_pendingClearColor, g_state.clearColor, 4);
    g_pendingClearZ = g_state.clearZ;
    Commit();
    // After the commit, so the display never takes the texture before its copy.
    uint32_t* words = static_cast<uint32_t*>(dest);
    words[1] = g_copyCount;
    words[0] = kXfbMagic;
}

void SetCopySource(uint16_t left, uint16_t top, uint16_t width, uint16_t height) {
    g_copySrc[0] = left;
    g_copySrc[1] = top;
    g_copySrc[2] = width;
    g_copySrc[3] = height;
}

void DrawDone() {
    Commit();
    if (g_lastCommitted != nil) {
        [g_lastCommitted waitUntilCompleted];
    }
}

id<MTLDevice> Device() {
    Init();
    return g_device;
}

id<MTLCommandQueue> Queue() {
    Init();
    return g_queue;
}

id<MTLTexture> XfbTexture(uint32_t addr) {
    id<MTLTexture> texture = nil;
    os_unfair_lock_lock(&g_xfbLock);
    for (Xfb& x : g_xfbs) {
        if (x.addr == addr && addr != 0 && Stamped(x)) {
            texture = x.texture;
        }
    }
    os_unfair_lock_unlock(&g_xfbLock);
    return texture;
}

}  // namespace frontline::gx

bool frontline::ReadDisplayedFrame(uint8_t* rgba, uint32_t* width, uint32_t* height) {
    id<MTLTexture> texture = gx::XfbTexture(GetDisplayedFrame().xfb);
    if (texture == nil) {
        return false;
    }
    *width = (uint32_t)texture.width;
    *height = (uint32_t)texture.height;
    if (rgba != nullptr) {
        [texture getBytes:rgba
              bytesPerRow:texture.width * 4
               fromRegion:MTLRegionMake2D(0, 0, texture.width, texture.height)
              mipmapLevel:0];
    }
    return true;
}
