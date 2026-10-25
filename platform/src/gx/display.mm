// The video output on screen: what the TV would show.
//
// The displayed frame buffer (VI) is either a frame GX copied, kept as a
// texture, or one the CPU wrote (the movie player): YUV 4:2:2 in the frame
// buffer's memory, two pixels a word (Y0 U Y1 V), decoded here.

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

// The game's headers define BOOL as int; Objective-C's is bool.
#define BOOL FRONTLINE_GAME_BOOL
#include "frontline/display.h"
#include "frontline/platform.h"
#include "gx_metal.h"
#undef BOOL

namespace {

const char kDisplayShaders[] = R"msl(
#include <metal_stdlib>
using namespace metal;

struct Out {
    float4 pos [[position]];
    float2 uv;
};

// One triangle that covers the viewport.
vertex Out display_vertex(uint id [[vertex_id]]) {
    float2 p = float2((id << 1) & 2, id & 2);
    Out o;
    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2(p.x, 1.0 - p.y);
    return o;
}

fragment float4 display_rgb(Out in [[stage_in]], texture2d<float> frame [[texture(0)]]) {
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    return float4(frame.sample(s, in.uv).rgb, 1.0);
}

// The YUV frame as a texture of words: r = V, g = Y1, b = U, a = Y0.
fragment float4 display_yuv(Out in [[stage_in]], texture2d<float> frame [[texture(0)]]) {
    uint width = frame.get_width() * 2, height = frame.get_height();
    uint x = min(uint(in.uv.x * width), width - 1), y = min(uint(in.uv.y * height), height - 1);
    float4 w = frame.read(uint2(x / 2, y)) * 255.0;
    float c = 1.164 * (((x & 1) != 0 ? w.g : w.a) - 16.0);
    float u = w.b - 128.0, v = w.r - 128.0;
    return float4(saturate(float3(c + 1.596 * v, c - 0.392 * u - 0.813 * v, c + 2.017 * u) / 255.0), 1.0);
}
)msl";

id<MTLRenderPipelineState> g_rgb, g_yuv;
MTLPixelFormat g_format = MTLPixelFormatInvalid;
id<MTLTexture> g_yuvFrames[3];  // a ring, so the CPU never writes one the GPU is reading
uint32_t g_yuvNext;

bool MakePipelines(MTLPixelFormat format) {
    if (format == g_format) {
        return true;
    }
    id<MTLDevice> device = frontline::gx::Device();
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:@(kDisplayShaders) options:nil error:&error];
    if (library == nil) {
        std::fprintf(stderr, "frontline: display shaders: %s\n", error.localizedDescription.UTF8String);
        std::abort();
    }
    MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction = [library newFunctionWithName:@"display_vertex"];
    desc.colorAttachments[0].pixelFormat = format;
    desc.fragmentFunction = [library newFunctionWithName:@"display_rgb"];
    g_rgb = [device newRenderPipelineStateWithDescriptor:desc error:&error];
    desc.fragmentFunction = [library newFunctionWithName:@"display_yuv"];
    g_yuv = [device newRenderPipelineStateWithDescriptor:desc error:&error];
    if (g_rgb == nil || g_yuv == nil) {
        std::fprintf(stderr, "frontline: display pipelines: %s\n", error.localizedDescription.UTF8String);
        std::abort();
    }
    g_format = format;
    return true;
}

// The CPU-written frame buffer as a texture of words.
id<MTLTexture> YuvTexture(const frontline::DisplayedFrame& frame) {
    uint32_t words = frame.width / 2;
    id<MTLTexture> texture = g_yuvFrames[g_yuvNext];
    if (texture == nil || texture.width != words || texture.height != frame.height) {
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                        width:words
                                                                                       height:frame.height
                                                                                    mipmapped:NO];
        desc.storageMode = MTLStorageModeShared;
        desc.usage = MTLTextureUsageShaderRead;
        texture = [frontline::gx::Device() newTextureWithDescriptor:desc];
        g_yuvFrames[g_yuvNext] = texture;
    }
    g_yuvNext = (g_yuvNext + 1) % 3;
    [texture replaceRegion:MTLRegionMake2D(0, 0, words, frame.height)
               mipmapLevel:0
                 withBytes:ADDR_TO_PTR(const void, frame.xfb)
               bytesPerRow:words * 4];
    return texture;
}

void Encode(id<MTLCommandBuffer> commands, id<MTLTexture> target, const frontline::DisplayedFrame& frame) {
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];

    if (frame.xfb != 0 && !frame.black && frame.width != 0 && frame.height != 0) {
        id<MTLTexture> texture = frontline::gx::XfbTexture(frame.xfb);
        bool yuv = texture == nil;
        if (yuv) {
            texture = YuvTexture(frame);
        }
        // The 4:3 screen, and the frame's lines on its 480.
        double w = target.width, h = target.height;
        double screenW = std::min(w, h * 4 / 3), screenH = screenW * 3 / 4;
        double frameH = screenH * std::min(1.0, frame.height / 480.0);
        [encoder setViewport:(MTLViewport){(w - screenW) / 2, (h - frameH) / 2, screenW, frameH, 0, 1}];
        [encoder setRenderPipelineState:yuv ? g_yuv : g_rgb];
        [encoder setFragmentTexture:texture atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    [encoder endEncoding];
}

// FRONTLINE_DISPLAY_DUMP=<folder>: what the window shows, as a PPM image every
// second (for checking the display without a screen capture).
void DumpCheck(const frontline::DisplayedFrame& frame, id<MTLTexture> drawable) {
    static const char* folder = std::getenv("FRONTLINE_DISPLAY_DUMP");
    static uint32_t presents, dumps;
    if (folder == nullptr || ++presents % 60 != 0) {
        return;
    }
    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:drawable.pixelFormat
                                                                                    width:drawable.width
                                                                                   height:drawable.height
                                                                                mipmapped:NO];
    desc.storageMode = MTLStorageModeShared;
    desc.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [frontline::gx::Device() newTextureWithDescriptor:desc];
    id<MTLCommandBuffer> commands = [frontline::gx::Queue() commandBuffer];
    Encode(commands, target, frame);
    [commands commit];
    [commands waitUntilCompleted];
    uint32_t w = (uint32_t)target.width, h = (uint32_t)target.height;
    uint8_t* bgra = static_cast<uint8_t*>(std::malloc((size_t)w * h * 4));
    [target getBytes:bgra bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    char path[1024];
    std::snprintf(path, sizeof(path), "%s/display%03u.ppm", folder, ++dumps);
    if (FILE* f = std::fopen(path, "wb")) {
        std::fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (size_t i = 0; i < (size_t)w * h; i++) {
            uint8_t rgb[3] = {bgra[i * 4 + 2], bgra[i * 4 + 1], bgra[i * 4]};
            std::fwrite(rgb, 1, 3, f);
        }
        std::fclose(f);
    }
    std::free(bgra);
}

}  // namespace

id<MTLDevice> frontline::MetalDevice() { return gx::Device(); }

void frontline::Present(id<CAMetalDrawable> drawable) {
    if (drawable == nil) {
        return;
    }
    MakePipelines(drawable.texture.pixelFormat);
    DisplayedFrame frame = GetDisplayedFrame();
    id<MTLCommandBuffer> commands = [gx::Queue() commandBuffer];
    Encode(commands, drawable.texture, frame);
    [commands presentDrawable:drawable];
    [commands commit];
    DumpCheck(frame, drawable.texture);
}
