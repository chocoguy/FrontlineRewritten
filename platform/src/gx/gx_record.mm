// Recorded frames (gx_record.h).

#include "gx_record.h"

#include <os/lock.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>

#include "gx_metal.h"

namespace frontline::gx {
namespace {

// Vertex memory, in chunks shared by the records that drew from them.
constexpr NSUInteger kChunkSize = 16u << 20;

struct Chunk {
    CFTypeRef buffer;  // id<MTLBuffer>, owned
    // The records using it, plus one while uploads go into it.
    std::atomic<uint32_t> refs;
};

os_unfair_lock g_chunkLock = OS_UNFAIR_LOCK_INIT;
Chunk** g_freeChunks;  // malloc'd
uint32_t g_numFree, g_freeCapacity;

// Where uploads go now (the game thread's).
Chunk* g_current;
NSUInteger g_currentUsed;

template <class T>
void Grow(T*& items, uint32_t count, uint32_t& capacity) {
    if (count == capacity) {
        capacity = capacity != 0 ? capacity * 2 : 64;
        items = static_cast<T*>(std::realloc(items, capacity * sizeof(T)));
    }
}

void ReleaseChunk(Chunk* c) {
    if (c->refs.fetch_sub(1, std::memory_order_acq_rel) != 1) {
        return;
    }
    os_unfair_lock_lock(&g_chunkLock);
    Grow(g_freeChunks, g_numFree, g_freeCapacity);
    g_freeChunks[g_numFree++] = c;
    os_unfair_lock_unlock(&g_chunkLock);
}

Chunk* AcquireChunk() {
    Chunk* c = nullptr;
    os_unfair_lock_lock(&g_chunkLock);
    if (g_numFree != 0) {
        c = g_freeChunks[--g_numFree];
    }
    os_unfair_lock_unlock(&g_chunkLock);
    if (c == nullptr) {
        c = new (std::calloc(1, sizeof(Chunk))) Chunk();
        c->buffer = CFBridgingRetain([Device() newBufferWithLength:kChunkSize options:MTLResourceStorageModeShared]);
    }
    c->refs.store(1, std::memory_order_relaxed);
    return c;
}

struct RecordedPass {
    uint32_t firstDraw;
    bool clearColor, clearDepth;
    uint8_t color[4];
    uint32_t clearZ;
};

}  // namespace

struct FrameRecord {
    std::atomic<uint32_t> refs;
    bool finished;
    uint32_t efbWidth, efbHeight;
    uint32_t samples;  // a pixel (MSAA)
    uint32_t copy[4];  // x, y, width, height in the EFB's pixels
    FrameView view, prevView;
    RecordedDraw* draws;
    uint32_t numDraws, drawCapacity;
    FragUniforms* frag;
    uint32_t numFrag, fragCapacity;
    RecordedPass* passes;
    uint32_t numPasses, passCapacity;
    Chunk** chunks;
    uint32_t numChunks, chunkCapacity;
};

FrameRecord* NewRecord(uint32_t efbWidth, uint32_t efbHeight, uint32_t samples) {
    FrameRecord* r = new (std::calloc(1, sizeof(FrameRecord))) FrameRecord();
    r->refs.store(1, std::memory_order_relaxed);
    r->efbWidth = efbWidth;
    r->efbHeight = efbHeight;
    r->samples = samples;
    r->copy[2] = efbWidth;
    r->copy[3] = efbHeight;
    return r;
}

void RecordPass(FrameRecord* r, bool clearColor, const uint8_t color[4], bool clearDepth, uint32_t clearZ) {
    Grow(r->passes, r->numPasses, r->passCapacity);
    RecordedPass& p = r->passes[r->numPasses++];
    p.firstDraw = r->numDraws;
    p.clearColor = clearColor;
    p.clearDepth = clearDepth;
    std::memcpy(p.color, color, 4);
    p.clearZ = clearZ;
}

id<MTLBuffer> RecordUpload(FrameRecord* r, const void* data, NSUInteger size, NSUInteger* offset, uint32_t* chunk) {
    size = (size + 255) & ~NSUInteger(255);
    if (size > kChunkSize) {
        return nil;
    }
    if (g_current == nullptr || g_currentUsed + size > kChunkSize) {
        if (g_current != nullptr) {
            ReleaseChunk(g_current);
        }
        g_current = AcquireChunk();
        g_currentUsed = 0;
    }
    if (r->numChunks == 0 || r->chunks[r->numChunks - 1] != g_current) {
        Grow(r->chunks, r->numChunks, r->chunkCapacity);
        r->chunks[r->numChunks++] = g_current;
        g_current->refs.fetch_add(1, std::memory_order_relaxed);
    }
    id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)g_current->buffer;
    *offset = g_currentUsed;
    *chunk = r->numChunks - 1;
    std::memcpy(static_cast<uint8_t*>(buffer.contents) + g_currentUsed, data, size);
    g_currentUsed += size;
    return buffer;
}

void RecordDraw(FrameRecord* r, const RecordedDraw& draw, const FragUniforms& frag) {
    if (r->numFrag == 0 || std::memcmp(&r->frag[r->numFrag - 1], &frag, sizeof(frag)) != 0) {
        Grow(r->frag, r->numFrag, r->fragCapacity);
        r->frag[r->numFrag++] = frag;
    }
    Grow(r->draws, r->numDraws, r->drawCapacity);
    RecordedDraw& d = r->draws[r->numDraws++];
    d = draw;
    d.fragUniforms = r->numFrag - 1;
    for (int m = 0; m < 8; m++) {
        if (d.textureMask & (1u << m)) {
            CFRetain(d.textures[m]);
        }
    }
}

void RecordCopy(FrameRecord* r, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    r->copy[0] = x;
    r->copy[1] = y;
    r->copy[2] = width;
    r->copy[3] = height;
}

void FinishRecord(FrameRecord* r) { r->finished = true; }

void RetainRecord(FrameRecord* r) { r->refs.fetch_add(1, std::memory_order_relaxed); }

void ReleaseRecord(FrameRecord* r) {
    if (r == nullptr || r->refs.fetch_sub(1, std::memory_order_acq_rel) != 1) {
        return;
    }
    for (uint32_t i = 0; i < r->numDraws; i++) {
        const RecordedDraw& d = r->draws[i];
        for (int m = 0; m < 8; m++) {
            if (d.textureMask & (1u << m)) {
                CFRelease(d.textures[m]);
            }
        }
    }
    for (uint32_t i = 0; i < r->numChunks; i++) {
        ReleaseChunk(r->chunks[i]);
    }
    std::free(r->draws);
    std::free(r->frag);
    std::free(r->passes);
    std::free(r->chunks);
    r->~FrameRecord();
    std::free(r);
}

uint32_t RecordDraws(const FrameRecord* r) { return r->numDraws; }

void SetRecordViews(FrameRecord* r, const FrameView& now, const FrameView& prev) {
    r->view = now;
    r->prevView = prev;
}

void RecordViews(const FrameRecord* r, FrameView* now, FrameView* prev) {
    *now = r->view;
    *prev = r->prevView;
}

namespace {

id<MTLTexture> Target(id<MTLTexture> current, MTLPixelFormat format, uint32_t width, uint32_t height,
                      NSUInteger levels, MTLTextureUsage usage, uint32_t samples = 1) {
    if (current != nil && current.width == width && current.height == height && current.mipmapLevelCount == levels &&
        current.sampleCount == samples) {
        return current;
    }
    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                    width:width
                                                                                   height:height
                                                                                mipmapped:levels > 1];
    desc.usage = usage;
    desc.storageMode = MTLStorageModePrivate;
    if (samples > 1) {
        desc.textureType = MTLTextureType2DMultisample;
        desc.sampleCount = samples;
    }
    return [Device() newTextureWithDescriptor:desc];
}

// What the encoder has bound, so a draw sets only what changed.
struct ReplayBound {
    __unsafe_unretained id<MTLRenderPipelineState> pipeline;
    __unsafe_unretained id<MTLDepthStencilState> depth;
    __unsafe_unretained id<MTLTexture> textures[8];
    __unsafe_unretained id<MTLSamplerState> samplers[8];
    MTLCullMode cull;
    bool viewportSet, scissorSet, vertexSet;
    uint32_t frag;
    MTLViewport viewport;
    MTLScissorRect scissor;
    VertexUniforms vertex;
};

}  // namespace

id<MTLTexture> ReplayFrame(id<MTLCommandBuffer> commands, FrameRecord* r, const ReplayMotion* motion,
                           ReplayTargets& targets, bool mipmapped, bool makeMipmaps) {
    const uint32_t samples = r->samples > 1 ? r->samples : 1;
    targets.color = Target(targets.color, MTLPixelFormatRGBA8Unorm, r->efbWidth, r->efbHeight, 1,
                           MTLTextureUsageRenderTarget);
    targets.depth = Target(targets.depth, MTLPixelFormatDepth32Float, r->efbWidth, r->efbHeight, 1,
                           MTLTextureUsageRenderTarget, samples);
    if (samples > 1) {
        targets.msaa = Target(targets.msaa, MTLPixelFormatRGBA8Unorm, r->efbWidth, r->efbHeight, 1,
                              MTLTextureUsageRenderTarget, samples);
    }
    const uint32_t copyW = r->copy[2], copyH = r->copy[3];
    NSUInteger levels = 1;
    if (mipmapped) {
        for (uint32_t size = std::max(copyW, copyH); size > 1; size /= 2) {
            levels++;
        }
    }
    targets.frame = Target(targets.frame, MTLPixelFormatRGBA8Unorm, copyW, copyH, levels, MTLTextureUsageShaderRead);

    const uint32_t numPasses = r->numPasses != 0 ? r->numPasses : 1;
    for (uint32_t p = 0; p < numPasses; p++) {
        static const RecordedPass kClear = {0, true, true, {0, 0, 0, 0}, 0xFFFFFF};
        const RecordedPass& pass = r->numPasses != 0 ? r->passes[p] : kClear;
        const uint32_t end = p + 1 < r->numPasses ? r->passes[p + 1].firstDraw : r->numDraws;
        MTLRenderPassDescriptor* desc = [MTLRenderPassDescriptor renderPassDescriptor];
        if (samples > 1) {
            // Resolved as each pass ends; the samples are kept unless the next
            // pass clears them (the last pass's stay for the next replay).
            const bool nextClearsColor = p + 1 < r->numPasses && r->passes[p + 1].clearColor;
            desc.colorAttachments[0].texture = targets.msaa;
            desc.colorAttachments[0].resolveTexture = targets.color;
            desc.colorAttachments[0].storeAction = nextClearsColor ? MTLStoreActionMultisampleResolve
                                                                   : MTLStoreActionStoreAndMultisampleResolve;
        } else {
            desc.colorAttachments[0].texture = targets.color;
            desc.colorAttachments[0].storeAction = MTLStoreActionStore;
        }
        if (pass.clearColor) {
            const uint8_t* c = pass.color;
            desc.colorAttachments[0].loadAction = MTLLoadActionClear;
            desc.colorAttachments[0].clearColor = MTLClearColorMake(c[0] / 255.0, c[1] / 255.0, c[2] / 255.0, 1.0);
        } else {
            // As on the console, the EFB keeps what was drawn last: here, the
            // last replay's picture.
            desc.colorAttachments[0].loadAction = MTLLoadActionLoad;
        }
        desc.depthAttachment.texture = targets.depth;
        if (pass.clearDepth) {
            desc.depthAttachment.loadAction = MTLLoadActionClear;
            desc.depthAttachment.clearDepth = (pass.clearZ & 0xFFFFFF) / 16777215.0;
        } else {
            desc.depthAttachment.loadAction = MTLLoadActionLoad;
        }
        const bool nextClearsDepth = p + 1 < r->numPasses && r->passes[p + 1].clearDepth;
        desc.depthAttachment.storeAction = nextClearsDepth ? MTLStoreActionDontCare : MTLStoreActionStore;
        id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:desc];
        [encoder setFrontFacingWinding:MTLWindingClockwise];

        ReplayBound bound;
        std::memset(&bound, 0, sizeof(bound));
        bound.cull = (MTLCullMode)~0u;
        bound.frag = ~0u;
        for (uint32_t i = r->numPasses != 0 ? pass.firstDraw : 0; i < end; i++) {
            const RecordedDraw& d = r->draws[i];
            if (bound.pipeline != d.pipeline) {
                [encoder setRenderPipelineState:d.pipeline];
                bound.pipeline = d.pipeline;
            }
            if (bound.depth != d.depth) {
                [encoder setDepthStencilState:d.depth];
                bound.depth = d.depth;
            }
            if (bound.cull != d.cull) {
                [encoder setCullMode:d.cull];
                bound.cull = d.cull;
            }
            if (!bound.viewportSet || std::memcmp(&bound.viewport, &d.viewport, sizeof(d.viewport)) != 0) {
                [encoder setViewport:d.viewport];
                bound.viewport = d.viewport;
                bound.viewportSet = true;
            }
            if (!bound.scissorSet || std::memcmp(&bound.scissor, &d.scissor, sizeof(d.scissor)) != 0) {
                [encoder setScissorRect:d.scissor];
                bound.scissor = d.scissor;
                bound.scissorSet = true;
            }
            for (int m = 0; m < 8; m++) {
                if (!(d.textureMask & (1u << m))) {
                    continue;
                }
                id<MTLTexture> texture = (__bridge id<MTLTexture>)d.textures[m];
                if (bound.textures[m] != texture) {
                    [encoder setFragmentTexture:texture atIndex:m];
                    bound.textures[m] = texture;
                }
                if (bound.samplers[m] != d.samplers[m]) {
                    [encoder setFragmentSamplerState:d.samplers[m] atIndex:m];
                    bound.samplers[m] = d.samplers[m];
                }
            }
            VertexUniforms vu;
            std::memset(&vu, 0, sizeof(vu));
            vu.stride = d.stride;
            vu.numTexGens = d.numTexGens;
            if (motion != nullptr && motion->moves[d.cls]) {
                vu.mode = 1;
                std::memcpy(vu.from, d.projection, sizeof(d.projection));
                vu.zoom[0] = motion->zoom[d.cls][0];
                vu.zoom[1] = motion->zoom[d.cls][1];
                std::memcpy(vu.move, motion->move[d.cls], sizeof(vu.move));
            }
            if (!bound.vertexSet || std::memcmp(&bound.vertex, &vu, sizeof(vu)) != 0) {
                [encoder setVertexBytes:&vu length:sizeof(vu) atIndex:1];
                bound.vertex = vu;
                bound.vertexSet = true;
            }
            if (bound.frag != d.fragUniforms) {
                [encoder setFragmentBytes:&r->frag[d.fragUniforms] length:sizeof(FragUniforms) atIndex:0];
                bound.frag = d.fragUniforms;
            }
            [encoder setVertexBuffer:(__bridge id<MTLBuffer>)r->chunks[d.vertexChunk]->buffer
                              offset:d.vertexOffset
                             atIndex:0];
            [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                indexCount:d.numIndices
                                 indexType:MTLIndexTypeUInt16
                               indexBuffer:(__bridge id<MTLBuffer>)r->chunks[d.indexChunk]->buffer
                         indexBufferOffset:d.indexOffset];
        }
        [encoder endEncoding];
    }

    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    const uint32_t x0 = std::min(r->copy[0], r->efbWidth), y0 = std::min(r->copy[1], r->efbHeight);
    const uint32_t w = std::min(copyW, r->efbWidth - x0), h = std::min(copyH, r->efbHeight - y0);
    if (w != 0 && h != 0) {
        [blit copyFromTexture:targets.color
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(x0, y0, 0)
                   sourceSize:MTLSizeMake(w, h, 1)
                    toTexture:targets.frame
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
    }
    if (levels > 1 && makeMipmaps) {
        [blit generateMipmapsForTexture:targets.frame];
    }
    [blit endEncoding];
    RetainRecord(r);
    [commands addCompletedHandler:^(id<MTLCommandBuffer>) {
        ReleaseRecord(r);
    }];
    return targets.frame;
}

}  // namespace frontline::gx
