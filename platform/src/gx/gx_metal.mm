// The GX backend on Metal.
//
// Draws go into an offscreen EFB (the console's embedded frame buffer) as the
// game makes them, on the game thread. GXCopyDisp copies the EFB into a texture
// kept for that frame buffer's address, which the video output (VI) shows.
// Textures decode once into a cache. Nothing here uses operator new (the game
// owns it on game threads): tables are malloc'd, and Objective-C collections
// allocate with malloc.
//
// The GPU never reads the game's memory: each draw copies its vertices,
// indices and uniforms into Metal memory, and textures are decoded into Metal
// textures. So the GPU can draw a frame while the game builds the next one
// (DrawDone).
//
// With Rewritten's high frame rate, frames are recorded instead (gx_record.h)
// and the display draws them, as often as the screen refreshes.

#import <Metal/Metal.h>
#include <os/lock.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// The game's headers define BOOL as int; Objective-C's is bool.
#define BOOL FRONTLINE_GAME_BOOL
#include "frontline/platform.h"
#include "gx_metal.h"
#include "gx_record.h"
#include "gx_shader.h"
#include "gx_state.h"
#include "gx_texture.h"
#include "gx_vertex.h"
#include "native/hooks.h"
#include "os/os_internal.h"
#include "pad_internal.h"
#include "types.h"

// What @autoreleasepool compiles to: GX runs on game threads, which have no
// run loop to drain a pool, so DrawDone drains one a frame.
extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void* pool);
#undef BOOL

namespace frontline::gx {
namespace {

constexpr NSUInteger kVertexBufferSize = 32u << 20;

// The EFB in the console's pixels (as the game addresses it): 640 x 528, or
// wider for Rewritten's widescreen (NativeRenderWidth). Its textures are
// g_scale times that (RenderScale: Rewritten's resolution), and every pixel
// position the game hands over is scaled where it reaches Metal. Both are
// fixed for the run, so at 1 and 640 wide (Classic) nothing changes.
uint32_t g_efbWidth = 640;
constexpr uint32_t kEfbHeight = 528;
float g_scale = 1.0f;

// Rewritten's picture, fixed for the run (MakeEfb):
// - Samples a pixel (MsaaSamples): with more than one, the EFB is drawn
//   multisampled into g_efbMsaa and resolved into g_efbColor as each pass
//   ends, so copies read it as before.
// - Better texture filtering: mipmaps made for textures the game gave none,
//   and trilinear and anisotropic filtering for linear ones (Sampler).
uint32_t g_samples = 1;
bool g_filtering;

id<MTLDevice> g_device;
id<MTLCommandQueue> g_queue;
id<MTLLibrary> g_library;
id<MTLTexture> g_efbColor;  // (none when frames are only recorded: the display has its own)
id<MTLTexture> g_efbDepth;
id<MTLTexture> g_efbMsaa;   // multisampled color (MSAA only)
bool g_efbMade;
uint32_t g_efbPixels[2];  // the EFB's size in its textures' pixels
// The ubershader's functions (no function constants), and its pipelines by
// blend key. Pipeline and sampler states by the keys DepthState and Sampler
// make.
id<MTLFunction> g_uberVertex, g_uberFragment;
id<MTLRenderPipelineState> g_pipelines[1 << 10];
id<MTLDepthStencilState> g_depthStates[1 << 5];
id<MTLSamplerState> g_samplers[1 << 13];
NSMutableArray<id<MTLBuffer>>* g_freeBuffers;
os_unfair_lock g_bufferLock = OS_UNFAIR_LOCK_INIT;

// Frame buffer textures by address (the game uses two). The console's copy
// writes the frame into the frame buffer's memory; this one writes only a
// stamp there, so a frame the CPU wrote since (the movie player's YUV) shows
// instead of the texture (see XfbTexture).
struct Xfb {
    uint32_t addr;
    uint32_t stamp;
    id<MTLTexture> texture;  // nil when frames are only recorded
    // High frame rate: the recorded frame the copy made (finished), and the
    // frame being recorded that copied here (finished at its end, DrawDone).
    FrameRecord* record;
    FrameRecord* pending;
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
id<MTLCommandBuffer> g_previousFrame;  // committed by the last DrawDone

// What the open encoder has bound, so a draw sets only what changed. The
// pointers are compared, not owned: the caches and the command buffer keep
// the objects alive while the encoder is open.
struct Bound {
    __unsafe_unretained id<MTLRenderPipelineState> pipeline;
    __unsafe_unretained id<MTLDepthStencilState> depth;
    __unsafe_unretained id<MTLTexture> textures[8];
    __unsafe_unretained id<MTLSamplerState> samplers[8];
    MTLCullMode cull;
    bool viewportSet, scissorSet, uniformsSet;
    VertexUniforms vertexUniforms;  // (0 stride: unset)
    MTLViewport viewport;
    MTLScissorRect scissor;
    FragUniforms uniforms;
} g_bound;
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
uint32_t g_copyCount;  // frame copies so far
std::atomic<bool> g_displayShrinks;  // SetDisplayShrinks

// Rewritten's high frame rate (gx_record.h), decided at the first draw with
// the EFB. Frames are recorded for the display, and only recorded unless a
// check needs them drawn here too (FRONTLINE_GX_VERIFY, FRONTLINE_GX_HASH,
// FRONTLINE_GX_SNAP, FRONTLINE_HFR_VERIFY).
bool g_hfr, g_recordOnly;
FrameRecord* g_record;   // the frame being recorded
bool g_passOpen;         // recording only: a pass has begun since the last copy
bool g_copiedSinceDraw;  // a frame copy since the last draw: GXDrawDone ends the frame
DrawClass g_drawClass = kDrawWorld;  // NativeDrawClass

// What the game said about the frame's camera (NativeFrameView,
// NativePlayerView), kept with its record when it ends.
FrameView g_frameView;
uint32_t g_frameViews;  // scenes drawn (more than one: split screen)
FrameView g_lastView;   // the last frame's
double g_lastTurn;      // radians a retrace the camera turned at the last frame (NativeCullTan)
uint32_t g_playerRetrace;  // the retrace at the last NativePlayerView
double g_cullWiden[2];     // radians the frame's culling frustum is widened by, x and y (NativeFrameView)
double g_lookPeak[2];      // the mouse's recent top rate (radians a retrace), fading

// FRONTLINE_HFR_TRACE=1: a line a frame on its camera and how the mouse
// turned it, to check the frames drawn between the game's turn it alike.
bool g_hfrTrace;

// FRONTLINE_PERF=1: a line a second on stderr saying where the frames' time
// went. A frame ends at GXDrawDone (CScreen::Flip's, in the game loop). The
// CPU's share is what's left of the frame after waiting for the GPU and for
// the retrace (SPIN_WAIT in CScreen::Wait).
struct Perf {
    bool on;
    uint64_t secondStart, lastFrame, spinStart;
    uint32_t frames;
    uint64_t frameSum, frameMax, drawDoneWait;
    uint64_t draws, uberDraws, coverageDraws, triangles, vertices, hashes, decodes;
    std::atomic<uint64_t> gpu;  // GPU time of the finished command buffers
    double gpuLastEnd;          // (completion handlers only)
} g_perf;

// A finished command buffer's GPU time, less any part spent alongside the
// one before (frames overlap on the GPU). Completion handlers run in order.
void PerfGpu(double start, double end) {
    double from = std::max(start, g_perf.gpuLastEnd);
    if (end > from) {
        g_perf.gpu.fetch_add((uint64_t)((end - from) * 1e9), std::memory_order_relaxed);
    }
    g_perf.gpuLastEnd = std::max(g_perf.gpuLastEnd, end);
}

uint32_t g_compiling;  // specialized pipelines started and not handed over yet (Specialized)

uint64_t NowNanos() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

void PerfFrame(uint64_t start, uint64_t waited) {
    Perf& p = g_perf;
    if (p.lastFrame != 0) {
        uint64_t frame = start - p.lastFrame;
        p.frames++;
        p.frameSum += frame;
        p.frameMax = std::max(p.frameMax, frame);
        p.drawDoneWait += waited;
    }
    p.lastFrame = start;
    if (p.secondStart == 0) {
        p.secondStart = start;
        p.spinStart = os::SpinWaitNanos();
        return;
    }
    if (start - p.secondStart < 1000000000ull || p.frames == 0) {
        return;
    }
    const double n = p.frames, ms = 1e-6;
    uint64_t spin = os::SpinWaitNanos() - p.spinStart;
    double cpu = (double)(p.frameSum - std::min(p.frameSum, p.drawDoneWait + spin)) / n;
    std::fprintf(stderr,
                 "[perf] %.1f fps | frame %.2f ms (max %.2f) | cpu %.2f | gpu %.2f | waits: drawdone %.2f, "
                 "retrace %.2f | %.0f draws, %.0fk tris, %.0fk verts | tex %.0f hashes, %.0f decodes | "
                 "%.0f ubershader draws, %u pipelines compiling, %.0f alpha-to-coverage draws\n",
                 n * 1e9 / (start - p.secondStart), p.frameSum / n * ms, p.frameMax * ms, cpu * ms,
                 p.gpu.exchange(0) / n * ms, p.drawDoneWait / n * ms, spin / n * ms, p.draws / n,
                 p.triangles / n / 1000, p.vertices / n / 1000, p.hashes / n, p.decodes / n, p.uberDraws / n,
                 g_compiling, p.coverageDraws / n);
    p.secondStart = start;
    p.spinStart = os::SpinWaitNanos();
    p.frames = 0;
    p.frameSum = p.frameMax = p.drawDoneWait = 0;
    p.draws = p.uberDraws = p.coverageDraws = p.triangles = p.vertices = p.hashes = p.decodes = 0;
}

// FRONTLINE_GX_UBER=1: draw everything with the ubershader, never with
// specialized pipelines. FRONTLINE_GX_SYNC_SHADERS=1: compile specialized
// pipelines on the spot instead of in the background, so no draw falls back
// to the ubershader while one compiles.
bool g_uber, g_syncShaders;

// FRONTLINE_GX_VERIFY=1: each draw is also drawn with the ubershader into a
// shadow EFB, and each frame copy compares the two pictures ("[gxverify]"
// lines). Shaders compile on the spot.
bool g_verify;
id<MTLTexture> g_shadowColor, g_shadowDepth, g_shadowMsaa;
id<MTLCommandBuffer> g_shadowCommands;
id<MTLRenderCommandEncoder> g_shadowEncoder;
id<MTLBuffer> g_verifyMine, g_verifyTheirs;  // the copied rectangles, read back
uint32_t g_verifyCopies, g_verifyDiffering;

// FRONTLINE_GX_SYNC=1: GXDrawDone waits for the GPU to finish the frame, as
// on the console, instead of letting it run while the game goes on.
bool g_sync;

// FRONTLINE_GX_ORIGINAL_FILTERING=1: Rewritten filters textures as the game
// set them (no made mipmaps, trilinear or anisotropic), for comparison.
bool g_originalFiltering;

// FRONTLINE_GX_TEXCHECK=1: hash textures at every draw instead of once a
// frame copy, and say when one changed within a copy (which the once-a-copy
// check would miss).
bool g_texCheck;

// FRONTLINE_GX_HASH=1: a line per frame copy with a hash of the copied frame,
// "[gxhash] <copy> <width>x<height> <hash>", to check that a change to the
// renderer leaves the picture alone.
bool g_hash;

// FRONTLINE_GX_SNAP (SnapshotCheck).
bool g_snap;

// FRONTLINE_HFR_VERIFY=1: frames are drawn here as well as recorded, and each
// frame's record is replayed unmoved and compared with the frame drawn
// ("[hfrverify] lines"): they should be the same picture.
bool g_hfrVerify;
uint32_t g_hfrFrames, g_hfrDiffering;

// Setting up runs once, in two steps. The first makes the device and starts
// compiling the shader library in the background: the app asks for the device
// on the main thread when its window opens, and the compile can take seconds
// (more with a cold shader cache), so it overlaps the boot instead of holding
// up the main thread. The second waits for the library; drawing needs it.
// (The game thread's first GX call used to find the device set while the main
// thread was still compiling, go on, and abort in Metal on the nil function.)
dispatch_once_t g_deviceOnce, g_shadersOnce;
dispatch_group_t g_libraryCompile;
NSString* g_libraryError;
std::atomic<bool> g_shadersReady;

void InitDevice() {
    dispatch_once(&g_deviceOnce, ^{
        g_device = MTLCreateSystemDefaultDevice();
        g_queue = [g_device newCommandQueue];
        g_libraryCompile = dispatch_group_create();
        dispatch_group_async(g_libraryCompile, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            NSError* error = nil;
            g_library = [g_device newLibraryWithSource:[NSString stringWithUTF8String:kShaderSource]
                                               options:nil
                                                 error:&error];
            if (g_library == nil) {
                g_libraryError = error.localizedDescription;
            }
        });
        g_freeBuffers = [NSMutableArray array];
        g_perf.on = std::getenv("FRONTLINE_PERF") != nullptr;
        g_hash = std::getenv("FRONTLINE_GX_HASH") != nullptr;
        g_snap = std::getenv("FRONTLINE_GX_SNAP") != nullptr;
        g_hfrVerify = std::getenv("FRONTLINE_HFR_VERIFY") != nullptr;
        g_hfrTrace = std::getenv("FRONTLINE_HFR_TRACE") != nullptr;
        g_texCheck = std::getenv("FRONTLINE_GX_TEXCHECK") != nullptr;
        g_sync = std::getenv("FRONTLINE_GX_SYNC") != nullptr;
        g_originalFiltering = std::getenv("FRONTLINE_GX_ORIGINAL_FILTERING") != nullptr;
        g_uber = std::getenv("FRONTLINE_GX_UBER") != nullptr;
        g_verify = !g_uber && std::getenv("FRONTLINE_GX_VERIFY") != nullptr;
        g_syncShaders = g_verify || std::getenv("FRONTLINE_GX_SYNC_SHADERS") != nullptr;
    });
}

void Init() {
    InitDevice();
    if (g_shadersReady.load(std::memory_order_acquire)) {
        return;
    }
    // The game's interrupts keep coming while the game thread waits, and their
    // queue fills in about a second, so deliver them between short waits. Not
    // inside dispatch_once: a handler or another game thread may come here too.
    while (dispatch_group_wait(g_libraryCompile, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_MSEC)) != 0) {
        os::SafePoint();
    }
    dispatch_once(&g_shadersOnce, ^{
        if (g_library == nil) {
            std::fprintf(stderr, "frontline: TEV shader: %s\n", g_libraryError.UTF8String);
            std::abort();
        }
        NSError* error = nil;
        MTLFunctionConstantValues* none = [MTLFunctionConstantValues new];
        g_uberVertex = [g_library newFunctionWithName:@"tev_vertex" constantValues:none error:&error];
        g_uberFragment = [g_library newFunctionWithName:@"tev_fragment" constantValues:none error:&error];
        if (g_uberVertex == nil || g_uberFragment == nil) {
            std::fprintf(stderr, "frontline: TEV shader: %s\n", error.localizedDescription.UTF8String);
            std::abort();
        }
        g_shadersReady.store(true, std::memory_order_release);
    });
}

// The EFB's textures, made at the first draw: the app may want the device
// before it has set the run's settings.
void MakeEfb() {
    if (g_efbMade) {
        return;
    }
    g_efbMade = true;
    g_efbWidth = (uint32_t)NativeRenderWidth();
    g_scale = RenderScale();
    g_efbPixels[0] = (uint32_t)std::lround(g_efbWidth * g_scale);
    g_efbPixels[1] = (uint32_t)std::lround(kEfbHeight * g_scale);
    g_hfr = HighFrameRate();
    g_samples = (uint32_t)MsaaSamples();
    if (g_samples > 1 && ![g_device supportsTextureSampleCount:g_samples]) {
        g_samples = 1;
    }
    g_filtering = NativeGameMode() == NATIVE_MODE_REWRITTEN && !g_originalFiltering;
    g_recordOnly = g_hfr && !g_verify && !g_hash && !g_snap && !g_hfrVerify;
    if (g_recordOnly) {
        return;
    }
    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                    width:g_efbPixels[0]
                                                                                   height:g_efbPixels[1]
                                                                                mipmapped:NO];
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModePrivate;
    g_efbColor = [g_device newTextureWithDescriptor:desc];
    if (g_verify) {
        g_shadowColor = [g_device newTextureWithDescriptor:desc];
    }
    if (g_samples > 1) {
        desc.textureType = MTLTextureType2DMultisample;
        desc.sampleCount = g_samples;
        desc.usage = MTLTextureUsageRenderTarget;
        g_efbMsaa = [g_device newTextureWithDescriptor:desc];
        if (g_verify) {
            g_shadowMsaa = [g_device newTextureWithDescriptor:desc];
        }
    }
    desc.pixelFormat = MTLPixelFormatDepth32Float;
    desc.usage = MTLTextureUsageRenderTarget;
    g_efbDepth = [g_device newTextureWithDescriptor:desc];
    if (g_verify) {
        g_shadowDepth = [g_device newTextureWithDescriptor:desc];
    }
}

// A position or size in the console's pixels, in the EFB's.
uint32_t Scaled(uint32_t v) { return g_scale == 1.0f ? v : (uint32_t)std::lround(v * g_scale); }

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

// The frame being recorded, from its first pass.
FrameRecord* Recording() {
    if (g_record == nullptr) {
        g_record = NewRecord(g_efbPixels[0], g_efbPixels[1], g_samples);
    }
    return g_record;
}

void BeginPass() {
    if (g_encoder != nil || (g_recordOnly && g_passOpen)) {
        return;
    }
    Init();
    MakeEfb();
    if (g_hfr && !g_passOpen) {
        RecordPass(Recording(), g_clearColorPending, g_pendingClearColor, g_clearDepthPending, g_pendingClearZ);
        g_passOpen = true;
    }
    if (g_recordOnly) {
        g_clearColorPending = g_clearDepthPending = false;
        return;
    }
    if (g_commands == nil) {
        g_commands = [g_queue commandBuffer];
        g_frameBuffers = [NSMutableArray array];
        g_vertexBuffer = nil;
    }
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    if (g_efbMsaa != nil) {
        pass.colorAttachments[0].texture = g_efbMsaa;
        pass.colorAttachments[0].resolveTexture = g_efbColor;
        pass.colorAttachments[0].storeAction = MTLStoreActionUnknown;  // EndPass decides
    } else {
        pass.colorAttachments[0].texture = g_efbColor;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    pass.depthAttachment.texture = g_efbDepth;
    pass.depthAttachment.storeAction = MTLStoreActionUnknown;  // EndPass decides
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
    [g_encoder setFrontFacingWinding:MTLWindingClockwise];
    if (g_verify) {
        if (g_shadowCommands == nil) {
            g_shadowCommands = [g_queue commandBuffer];
        }
        MTLRenderPassDescriptor* shadow = [pass copy];
        if (g_shadowMsaa != nil) {
            shadow.colorAttachments[0].texture = g_shadowMsaa;
            shadow.colorAttachments[0].resolveTexture = g_shadowColor;
        } else {
            shadow.colorAttachments[0].texture = g_shadowColor;
        }
        shadow.depthAttachment.texture = g_shadowDepth;
        g_shadowEncoder = [g_shadowCommands renderCommandEncoderWithDescriptor:shadow];
        [g_shadowEncoder setFrontFacingWinding:MTLWindingClockwise];
    }
    std::memset(&g_bound, 0, sizeof(g_bound));
    g_bound.cull = (MTLCullMode)~0u;
}

// Ends the pass. `depthUnused`, `colorUnused`: the next pass clears depth or
// color, so this one needn't store it. (The color is always resolved, for
// the copy; only MSAA's samples can go.)
void EndPass(bool depthUnused = false, bool colorUnused = false) {
    g_passOpen = false;
    const MTLStoreAction depth = depthUnused ? MTLStoreActionDontCare : MTLStoreActionStore;
    const MTLStoreAction color =
        colorUnused ? MTLStoreActionMultisampleResolve : MTLStoreActionStoreAndMultisampleResolve;
    if (g_encoder != nil) {
        [g_encoder setDepthStoreAction:depth];
        if (g_efbMsaa != nil) {
            [g_encoder setColorStoreAction:color atIndex:0];
        }
        [g_encoder endEncoding];
        g_encoder = nil;
    }
    if (g_shadowEncoder != nil) {
        [g_shadowEncoder setDepthStoreAction:depth];
        if (g_shadowMsaa != nil) {
            [g_shadowEncoder setColorStoreAction:color atIndex:0];
        }
        [g_shadowEncoder endEncoding];
        g_shadowEncoder = nil;
    }
}

void Commit() {
    EndPass();
    if (g_commands == nil) {
        return;
    }
    if (g_record != nullptr) {  // drawn here as well as recorded: its vertex memory is the record's
        FrameRecord* r = g_record;
        RetainRecord(r);
        [g_commands addCompletedHandler:^(id<MTLCommandBuffer>) {
            ReleaseRecord(r);
        }];
    }
    if (g_shadowCommands != nil) {
        [g_shadowCommands commit];
        g_shadowCommands = nil;
    }
    NSArray<id<MTLBuffer>>* used = g_frameBuffers;
    const bool perf = g_perf.on;
    [g_commands addCompletedHandler:^(id<MTLCommandBuffer> done) {
        os_unfair_lock_lock(&g_bufferLock);
        [g_freeBuffers addObjectsFromArray:used];
        os_unfair_lock_unlock(&g_bufferLock);
        if (perf) {
            PerfGpu(done.GPUStartTime, done.GPUEndTime);
        }
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

// An open-addressed hash table in malloc'd memory, of entries with a 64-bit
// `key` (0: empty) that are empty when zeroed.
template <class Entry>
struct Table {
    Entry* entries;
    uint32_t mask;  // the capacity, a power of two, less one
    uint32_t count;

    uint32_t Slot(uint64_t key) const { return (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 32) & mask; }

    // The entry for `key`, zeroed but for the key if it's new. A new entry
    // may move the others.
    Entry& operator[](uint64_t key) {
        if ((count + 1) * 4 > (entries != nullptr ? mask + 1 : 0) * 3) {
            Entry* old = entries;
            const uint32_t oldCapacity = old != nullptr ? mask + 1 : 0;
            const uint32_t capacity = oldCapacity != 0 ? oldCapacity * 2 : 1024;
            entries = static_cast<Entry*>(std::calloc(capacity, sizeof(Entry)));
            mask = capacity - 1;
            for (uint32_t i = 0; i < oldCapacity; i++) {
                if (old[i].key != 0) {
                    uint32_t j = Slot(old[i].key);
                    while (entries[j].key != 0) {
                        j = (j + 1) & mask;
                    }
                    entries[j] = old[i];
                }
            }
            std::free(old);
        }
        for (uint32_t i = Slot(key);; i = (i + 1) & mask) {
            Entry& e = entries[i];
            if (e.key == key) {
                return e;
            }
            if (e.key == 0) {
                e.key = key;
                count++;
                return e;
            }
        }
    }
};

// With MSAA, whether the draw's alpha test becomes alpha to coverage, so its
// edges are smoothed like the triangles': an unblended draw whose test is one
// threshold, a > ref or a >= ref (the game's fences, leaves and wire). The
// shader then writes the share of the pixel past it as alpha. (The levels'
// alpha-tested materials blend ONE, ZERO: not blending at all.)
bool AlphaCoverage() {
    const State& s = g_state;
    const uint32_t type = s.blend[0] & 3;
    const bool blends = type == 3 || (type == 1 && ((s.blend[1] & 7) != 1 || (s.blend[2] & 7) != 0));
    if (g_samples <= 1 || blends) {
        return false;
    }
    const uint32_t c0 = s.alphaComp[0] & 7, op = s.alphaComp[2] & 3, c1 = s.alphaComp[3] & 7;
    auto threshold = [](uint32_t c) { return c == 4 || c == 6; };  // GX_GREATER, GX_GEQUAL
    return op == 0 && ((threshold(c0) && c1 == 7) || (c0 == 7 && threshold(c1)));  // AND, GX_ALWAYS
}

// The blend state as a pipeline key, with alpha to coverage.
uint32_t BlendKey() {
    const State& s = g_state;
    return (s.blend[0] & 3) | (s.blend[1] & 7) << 2 | (s.blend[2] & 7) << 5 | (s.colorUpdate ? 1u : 0u) << 8 |
           (AlphaCoverage() ? 1u : 0u) << 9;
}

// A pipeline for blend key `key`, without its functions.
MTLRenderPipelineDescriptor* Descriptor(uint32_t key) {
    const uint32_t type = key & 3, src = (key >> 2) & 7, dst = (key >> 5) & 7;
    const bool colorUpdate = (key >> 8) & 1;
    MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
    desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    desc.rasterSampleCount = g_samples;
    desc.alphaToCoverageEnabled = (key >> 9) & 1;
    // RGB8 EFB: no destination alpha to write.
    desc.colorAttachments[0].writeMask = colorUpdate ? MTLColorWriteMaskRed | MTLColorWriteMaskGreen |
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
            att.sourceRGBBlendFactor = att.sourceAlphaBlendFactor = SrcFactor(src);
            att.destinationRGBBlendFactor = att.destinationAlphaBlendFactor = DstFactor(dst);
        }
    }
    return desc;
}

// The ubershader's pipeline for the blend state.
id<MTLRenderPipelineState> Pipeline() {
    const uint32_t key = BlendKey();
    id<MTLRenderPipelineState> pipeline = g_pipelines[key];
    if (pipeline != nil) {
        return pipeline;
    }
    MTLRenderPipelineDescriptor* desc = Descriptor(key);
    desc.vertexFunction = g_uberVertex;
    desc.fragmentFunction = g_uberFragment;
    NSError* error = nil;
    pipeline = [g_device newRenderPipelineStateWithDescriptor:desc error:&error];
    if (pipeline == nil) {
        std::fprintf(stderr, "frontline: pipeline: %s\n", error.localizedDescription.UTF8String);
        std::abort();
    }
    g_pipelines[key] = pipeline;
    return pipeline;
}

// A TEV setup's shape, which a specialized pipeline is compiled for: the
// function constants gx_shader.cpp reads, and the blend state. Constant
// colors, swap tables, fog, alpha references and indirect matrices stay
// uniforms, so setups that differ only in those share a pipeline.
struct Shape {
    uint32_t blend;
    uint32_t numStages, numIndStages, numTexGens, alphaFuncs, fogType, texValid, indOrder, indScale;
    uint32_t stages[16][7];
};

void MakeShape(const FragUniforms& u, uint32_t numTexGens, Shape& shape) {
    std::memset(&shape, 0, sizeof(shape));
    shape.blend = BlendKey();
    shape.numStages = u.numStages;
    for (uint32_t i = 0; i < u.numStages; i++) {
        std::memcpy(shape.stages[i], u.stages[i], sizeof(shape.stages[i]));
    }
    shape.numIndStages = std::min(u.numIndStages, 4u);
    for (uint32_t i = 0; i < shape.numIndStages; i++) {
        shape.indOrder |= ((u.indOrder[i][0] & 15) | (u.indOrder[i][1] & 15) << 4) << (i * 8);
        shape.indScale |= ((u.indOrder[i][2] & 15) | (u.indOrder[i][3] & 15) << 4) << (i * 8);
    }
    shape.numTexGens = numTexGens;
    shape.alphaFuncs = (u.alphaComp[0] & 15) | (u.alphaComp[2] & 15) << 4 | (u.alphaComp[3] & 15) << 8;
    shape.fogType = (u.fogType & 7) >= 2 ? u.fogType & 7 : 0;
    shape.texValid = u.texValid;
}

// A hash of the shape. The stages come last, and unused ones are zero.
uint64_t ShapeKey(const Shape& shape) {
    const uint32_t* words = reinterpret_cast<const uint32_t*>(&shape);
    const size_t used = offsetof(Shape, stages) / 4 + shape.numStages * 7;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < used; i++) {
        h = (h ^ words[i]) * 0x100000001b3ull;
    }
    h ^= h >> 29;
    return h != 0 ? h : 1;
}

// Specialized pipelines by ShapeKey. Each is compiled the first time its
// shape is drawn, on another thread; draws use the ubershader until it's
// ready (Finished hands it over).
struct Specialization {
    uint64_t key;
    bool started;
    CFTypeRef pipeline;  // owned; null while compiling, or if compiling failed
};
Table<Specialization> g_specializations;

struct Finish {
    uint64_t key;
    CFTypeRef pipeline;
};
os_unfair_lock g_finishedLock = OS_UNFAIR_LOCK_INIT;
Finish* g_finished;  // malloc'd
uint32_t g_numFinished, g_finishedCapacity;
std::atomic<uint32_t> g_finishedWaiting;

// From the compiling thread.
void Finished(uint64_t key, id<MTLRenderPipelineState> pipeline) {
    os_unfair_lock_lock(&g_finishedLock);
    if (g_numFinished == g_finishedCapacity) {
        g_finishedCapacity = g_finishedCapacity != 0 ? g_finishedCapacity * 2 : 64;
        g_finished = static_cast<Finish*>(std::realloc(g_finished, g_finishedCapacity * sizeof(Finish)));
    }
    g_finished[g_numFinished++] = {key, pipeline != nil ? CFBridgingRetain(pipeline) : nullptr};
    g_finishedWaiting.store(g_numFinished, std::memory_order_release);
    os_unfair_lock_unlock(&g_finishedLock);
}

void TakeFinished() {
    if (g_finishedWaiting.load(std::memory_order_acquire) == 0) {
        return;
    }
    os_unfair_lock_lock(&g_finishedLock);
    for (uint32_t i = 0; i < g_numFinished; i++) {
        g_specializations[g_finished[i].key].pipeline = g_finished[i].pipeline;
        g_compiling--;
    }
    g_numFinished = 0;
    g_finishedWaiting.store(0, std::memory_order_release);
    os_unfair_lock_unlock(&g_finishedLock);
}

void CompileSpecialized(const Shape& shape, uint64_t key) {
    MTLFunctionConstantValues* values = [MTLFunctionConstantValues new];
    const uint32_t scalars[9] = {shape.numStages, shape.numIndStages, shape.numTexGens,
                                 shape.alphaFuncs, shape.fogType,      shape.texValid,
                                 shape.indOrder,   shape.indScale,     (shape.blend >> 9) & 1};
    [values setConstantValues:scalars type:MTLDataTypeUInt withRange:NSMakeRange(1, 9)];
    for (uint32_t i = 0; i < 16; i++) {
        [values setConstantValues:shape.stages[i] type:MTLDataTypeUInt withRange:NSMakeRange(16 + i * 8, 7)];
    }
    MTLRenderPipelineDescriptor* desc = Descriptor(shape.blend);
    void (^compile)(void) = ^{
        NSError* error = nil;
        id<MTLFunction> vertex = [g_library newFunctionWithName:@"tev_vertex" constantValues:values error:&error];
        id<MTLFunction> fragment =
            vertex != nil ? [g_library newFunctionWithName:@"tev_fragment" constantValues:values error:&error] : nil;
        id<MTLRenderPipelineState> pipeline = nil;
        if (fragment != nil) {
            desc.vertexFunction = vertex;
            desc.fragmentFunction = fragment;
            pipeline = [g_device newRenderPipelineStateWithDescriptor:desc error:&error];
        }
        if (pipeline == nil) {
            std::fprintf(stderr, "frontline: specialized pipeline: %s\n", error.localizedDescription.UTF8String);
        }
        Finished(key, pipeline);
    };
    g_compiling++;
    if (g_syncShaders) {
        compile();
        TakeFinished();
    } else {
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), compile);
    }
}

// The specialized pipeline for `shape`, or nil while it compiles.
id<MTLRenderPipelineState> Specialized(const Shape& shape) {
    TakeFinished();
    const uint64_t key = ShapeKey(shape);
    Specialization* spec = &g_specializations[key];
    if (!spec->started) {
        spec->started = true;
        CompileSpecialized(shape, key);
        spec = &g_specializations[key];  // compiling on the spot may have moved it
    }
    return (__bridge id<MTLRenderPipelineState>)spec->pipeline;
}

id<MTLDepthStencilState> DepthState() {
    const State& s = g_state;
    uint32_t key = (s.zEnable ? 1u : 0u) | (s.zFunc & 7) << 1 | (s.zUpdate ? 1u : 0u) << 4;
    id<MTLDepthStencilState> state = g_depthStates[key];
    if (state == nil) {
        MTLDepthStencilDescriptor* desc = [MTLDepthStencilDescriptor new];
        desc.depthCompareFunction = s.zEnable ? (MTLCompareFunction)(s.zFunc & 7) : MTLCompareFunctionAlways;
        desc.depthWriteEnabled = s.zEnable && s.zUpdate;
        state = [g_device newDepthStencilStateWithDescriptor:desc];
        g_depthStates[key] = state;
    }
    return state;
}

MTLSamplerAddressMode Wrap(uint32_t w) {
    return w == 1 ? MTLSamplerAddressModeRepeat : w == 2 ? MTLSamplerAddressModeMirrorRepeat
                                                         : MTLSamplerAddressModeClampToEdge;
}

id<MTLSamplerState> Sampler(const TexObj& t, bool mipmapped) {
    uint32_t key = (t.wrapS & 3) | (t.wrapT & 3) << 2 | (t.minFilter & 15) << 4 | (t.magFilter & 15) << 8 |
                   (mipmapped ? 1u : 0u) << 12;
    id<MTLSamplerState> sampler = g_samplers[key];
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
        if (g_filtering && mipmapped && (t.minFilter & 1)) {
            // Rewritten: trilinear and anisotropic. (The console's goes to 4x
            // anisotropic, and the game asks for 2x at most.)
            desc.mipFilter = MTLSamplerMipFilterLinear;
            desc.maxAnisotropy = 16;
        }
        sampler = [g_device newSamplerStateWithDescriptor:desc];
        g_samplers[key] = sampler;
    }
    return sampler;
}

// A hash of texture data: every word up to 32 KB, else 4096 words spread
// over it.
uint64_t Hash(const uint8_t* p, uint32_t size) {
    uint64_t h = 0xcbf29ce484222325ull ^ size;
    const uint32_t words = size / 8, step = words > 4096 ? words / 4096 : 1;
    for (uint32_t i = 0; i < words; i += step) {
        uint64_t w;
        std::memcpy(&w, p + (size_t)i * 8, 8);
        h = (h ^ w) * 0x100000001b3ull;
    }
    for (uint32_t i = words * 8; i < size; i++) {
        h = (h ^ p[i]) * 0x100000001b3ull;
    }
    return h;
}

// The decoded textures, by address, format, size, levels and palette. Each
// entry owns its texture.
struct TexEntry {
    uint64_t key;
    uint64_t contents;  // Hash of the texels and palette
    uint32_t checked;   // the frame copy (g_copyCount) that last hashed them
    CFTypeRef texture;
};
Table<TexEntry> g_textures;

// The decoded texture for a texture map, cached by address, format, size and
// palette. The contents are hashed once a frame copy: the movie player
// rewrites its textures in place every frame, so a changed texture is decoded
// again into a new one (an in-flight frame may still read the old one).
// Rewritten makes mipmaps for linearly filtered textures the game gave none
// (the levels' textures and models have none, so they shimmer in the
// distance); `levelsOut` counts them.
id<MTLTexture> Texture(const TexObj& t, uint32_t* levelsOut) {
    if (t.image == 0 || t.width == 0 || t.height == 0) {
        return nil;
    }
    const uint8_t* src = ADDR_TO_PTR(const uint8_t, t.image);
    const uint32_t stored = t.mipmap ? t.maxLod + 1u : 1u;  // levels in memory
    uint32_t levels = stored;
    if (g_filtering && stored == 1 && (t.minFilter & 1) && !t.linear) {
        for (uint32_t size = std::max(t.width, t.height); size > 1; size /= 2) {
            levels++;
        }
    }
    const Tlut* tlut = t.isCI ? &g_state.tluts[t.tlut % 20] : nullptr;
    uint64_t key = ((uint64_t)t.image << 32) ^ ((uint64_t)t.format << 24) ^ ((uint64_t)t.width << 12) ^
                   ((uint64_t)t.height) ^ ((uint64_t)levels << 56) ^ ((uint64_t)t.linear << 63);
    if (tlut != nullptr && tlut->addr != 0) {
        key ^= ((uint64_t)tlut->addr << 8) * 31 + tlut->format;
    }
    key = key != 0 ? key : 1;
    *levelsOut = levels;
    TexEntry& entry = g_textures[key];
    if (entry.texture != nullptr && entry.checked == g_copyCount && !g_texCheck) {
        return (__bridge id<MTLTexture>)entry.texture;
    }
    uint32_t total = 0;
    for (uint32_t l = 0, w = t.width, h = t.height; l < stored; l++, w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1) {
        total += t.linear ? LinearLevelSize(t.format, w, h) : TextureLevelSize(t.format, w, h);
    }
    uint64_t contents = Hash(src, total);
    if (tlut != nullptr && tlut->addr != 0) {
        contents ^= Hash(ADDR_TO_PTR(const uint8_t, tlut->addr), tlut->count * 2) * 31;
    }
    g_perf.hashes++;
    if (entry.texture != nullptr && entry.checked == g_copyCount) {  // FRONTLINE_GX_TEXCHECK
        if (contents != entry.contents) {
            std::fprintf(stderr, "frontline: texture %#x (%ux%u, format %u) changed within frame copy %u\n", t.image,
                         t.width, t.height, t.format, g_copyCount);
        }
        return (__bridge id<MTLTexture>)entry.texture;
    }
    entry.checked = g_copyCount;
    if (entry.texture != nullptr && entry.contents == contents) {
        return (__bridge id<MTLTexture>)entry.texture;
    }
    g_perf.decodes++;
    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                    width:t.width
                                                                                   height:t.height
                                                                                mipmapped:levels > 1];
    desc.mipmapLevelCount = levels;
    desc.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [g_device newTextureWithDescriptor:desc];
    const size_t texels = (size_t)t.width * t.height;
    uint8_t* rgba = static_cast<uint8_t*>(std::malloc(texels * 4 * (levels > stored ? 2 : 1)));
    const uint8_t* palette = tlut != nullptr && tlut->addr != 0 ? ADDR_TO_PTR(const uint8_t, tlut->addr) : nullptr;
    uint8_t zeros[2 * 16384] = {};
    uint32_t w = t.width, h = t.height;
    for (uint32_t l = 0; l < stored; l++) {
        if (t.linear) {
            DecodeLinearTexture(src, t.format, w, h, palette != nullptr ? palette : zeros,
                                tlut != nullptr ? tlut->format : 0, rgba);
        } else {
            DecodeTexture(src, t.format, w, h, palette != nullptr ? palette : zeros,
                          tlut != nullptr ? tlut->format : 0, rgba);
        }
        [texture replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:l withBytes:rgba bytesPerRow:w * 4];
        if (l + 1 < stored) {
            src += TextureLevelSize(t.format, w, h);
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
        }
    }
    if (levels > stored) {
        const float coverage = AlphaTestCoverage(rgba, w * h);
        uint8_t *from = rgba, *to = rgba + texels * 4;
        for (uint32_t l = 1; l < levels; l++) {
            Downsample(from, w, h, to);
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
            if (coverage > 0.0f) {
                // (from the unscaled level above, so the scaling doesn't compound)
                std::memcpy(from, to, (size_t)w * h * 4);
                KeepCoverage(to, w * h, coverage);
                [texture replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:l withBytes:to bytesPerRow:w * 4];
                continue;
            }
            [texture replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:l withBytes:to bytesPerRow:w * 4];
            std::swap(from, to);
        }
    }
    std::free(rgba);
    if (entry.texture != nullptr) {
        CFRelease(entry.texture);
    }
    entry.texture = CFBridgingRetain(texture);
    entry.contents = contents;
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
        const uint32_t width = (uint32_t)g_efbColor.width, height = (uint32_t)g_efbColor.height;
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                        width:width
                                                                                       height:height
                                                                                    mipmapped:NO];
        desc.storageMode = MTLStorageModeShared;
        id<MTLTexture> copy = [g_device newTextureWithDescriptor:desc];
        id<MTLBlitCommandEncoder> blit = [g_commands blitCommandEncoder];
        [blit copyFromTexture:g_efbColor toTexture:copy];
        [blit endEncoding];
        Commit();
        [g_lastCommitted waitUntilCompleted];
        std::vector<uint8_t> rgba((size_t)width * height * 4);
        [copy getBytes:rgba.data() bytesPerRow:width * 4 fromRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0];
        const char* dir = std::getenv("FRONTLINE_GX_SNAP_DIR");
        char path[1024];
        std::snprintf(path, sizeof(path), "%s/snap-%d-%u.ppm", dir != nullptr ? dir : ".", frame, g_drawCalls);
        if (FILE* f = std::fopen(path, "wb")) {
            std::fprintf(f, "P6\n%u %u\n255\n", width, height);
            for (size_t p = 0; p < rgba.size(); p += 4) {
                std::fwrite(&rgba[p], 1, 3, f);
            }
            std::fclose(f);
        }
    }
}

}  // namespace

void SubmitTriangles(const float* vertices, uint32_t stride, uint32_t numVertices, const uint16_t* indices,
                     uint32_t numIndices) {
    if (g_trace > 1 && numIndices <= 12) {
        // FRONTLINE_GX_TRACE=2: small draws covering most of the screen (overlays).
        float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
        for (uint32_t i = 0; i < numVertices; i++) {
            const float* pos = vertices + i * stride;
            float w = pos[3] != 0.0f ? pos[3] : 1.0f;
            minx = std::min(minx, pos[0] / w);
            maxx = std::max(maxx, pos[0] / w);
            miny = std::min(miny, pos[1] / w);
            maxy = std::max(maxy, pos[1] / w);
        }
        if (maxx - minx < 1.6f || maxy - miny < 1.6f) {
            goto traced;
        }
        const float* c = numVertices > 0 ? vertices + 4 : nullptr;  // color0
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
    g_perf.draws++;
    g_perf.triangles += numIndices / 3;
    g_perf.vertices += numVertices;
    if (s.cullMode == 3) {
        return;
    }
    BeginPass();
    g_copiedSinceDraw = false;
    NSUInteger vOffset, iOffset;
    uint32_t vChunk = 0, iChunk = 0;
    id<MTLBuffer> vb, ib;
    if (g_hfr) {
        vb = RecordUpload(Recording(), vertices, numVertices * stride * sizeof(float), &vOffset, &vChunk);
        ib = RecordUpload(Recording(), indices, numIndices * 2, &iOffset, &iChunk);
    } else {
        vb = Upload(vertices, numVertices * stride * sizeof(float), &vOffset);
        ib = Upload(indices, numIndices * 2, &iOffset);
    }
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
    u.alphaCoverage = AlphaCoverage() ? 1u : 0u;
    g_perf.coverageDraws += u.alphaCoverage;
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
    id<MTLTexture> textures[8];
    id<MTLSamplerState> samplers[8];
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
        textures[m] = texture;
        samplers[m] = Sampler(s.texMap[m], levels > 1);
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

    VertexUniforms vertexUniforms;
    std::memset(&vertexUniforms, 0, sizeof(vertexUniforms));
    vertexUniforms.stride = stride;
    vertexUniforms.numTexGens = (stride - kVertexBase) / 3;
    id<MTLRenderPipelineState> pipeline = nil;
    if (!g_uber) {
        Shape shape;
        MakeShape(u, vertexUniforms.numTexGens, shape);
        pipeline = Specialized(shape);
    }
    if (pipeline == nil) {
        pipeline = Pipeline();
        g_perf.uberDraws++;
    }
    id<MTLDepthStencilState> depth = DepthState();
    const MTLCullMode cull = s.cullMode == 1 ? MTLCullModeFront : s.cullMode == 2 ? MTLCullModeBack : MTLCullModeNone;
    const double k = g_scale;
    MTLViewport vp = {s.viewport[0] * k, s.viewport[1] * k, s.viewport[2] * k, s.viewport[3] * k, s.viewport[4],
                      s.viewport[5]};
    uint32_t sx = s.scissor[0] < g_efbWidth ? s.scissor[0] : g_efbWidth - 1;
    uint32_t sy = s.scissor[1] < kEfbHeight ? s.scissor[1] : kEfbHeight - 1;
    uint32_t sw = sx + s.scissor[2] <= g_efbWidth ? s.scissor[2] : g_efbWidth - sx;
    uint32_t sh = sy + s.scissor[3] <= kEfbHeight ? s.scissor[3] : kEfbHeight - sy;
    if (sw == 0 || sh == 0) {
        return;
    }
    if (g_scale != 1.0f) {
        // Every pixel the console's rectangle touches.
        uint32_t x0 = (uint32_t)std::floor(sx * k), y0 = (uint32_t)std::floor(sy * k);
        uint32_t x1 = std::min((uint32_t)std::ceil((sx + sw) * k), g_efbPixels[0]);
        uint32_t y1 = std::min((uint32_t)std::ceil((sy + sh) * k), g_efbPixels[1]);
        sx = x0, sy = y0, sw = x1 - x0, sh = y1 - y0;
    }
    const MTLScissorRect scissor = {sx, sy, sw, sh};
    if (g_hfr) {
        RecordedDraw d;
        std::memset(&d, 0, sizeof(d));
        d.pipeline = pipeline;
        d.depth = depth;
        for (uint32_t m = 0; m < 8; m++) {
            if (u.texValid & (1u << m)) {
                d.textures[m] = (__bridge CFTypeRef)textures[m];
                d.samplers[m] = samplers[m];
            }
        }
        d.textureMask = u.texValid;
        d.vertexChunk = vChunk;
        d.indexChunk = iChunk;
        d.vertexOffset = (uint32_t)vOffset;
        d.indexOffset = (uint32_t)iOffset;
        d.numIndices = numIndices;
        d.stride = vertexUniforms.stride;
        d.numTexGens = vertexUniforms.numTexGens;
        d.cull = cull;
        d.viewport = vp;
        d.scissor = scissor;
        std::memcpy(d.projection, s.projection, sizeof(d.projection));
        d.cls = s.projection[0] != 0.0f ? kDrawScreen : g_drawClass;
        RecordDraw(Recording(), d, u);
    }
    if (g_recordOnly) {
        return;
    }
    if (g_bound.pipeline != pipeline) {
        [g_encoder setRenderPipelineState:pipeline];
        g_bound.pipeline = pipeline;
    }
    if (g_bound.depth != depth) {
        [g_encoder setDepthStencilState:depth];
        g_bound.depth = depth;
    }
    if (g_bound.cull != cull) {
        [g_encoder setCullMode:cull];
        g_bound.cull = cull;
    }
    if (!g_bound.viewportSet || std::memcmp(&g_bound.viewport, &vp, sizeof(vp)) != 0) {
        [g_encoder setViewport:vp];
        g_bound.viewport = vp;
        g_bound.viewportSet = true;
    }
    for (uint32_t m = 0; m < 8; m++) {
        if (!(u.texValid & (1u << m))) {
            continue;
        }
        if (g_bound.textures[m] != textures[m]) {
            [g_encoder setFragmentTexture:textures[m] atIndex:m];
            g_bound.textures[m] = textures[m];
        }
        if (g_bound.samplers[m] != samplers[m]) {
            [g_encoder setFragmentSamplerState:samplers[m] atIndex:m];
            g_bound.samplers[m] = samplers[m];
        }
    }
    if (!g_bound.scissorSet || std::memcmp(&g_bound.scissor, &scissor, sizeof(scissor)) != 0) {
        [g_encoder setScissorRect:scissor];
        g_bound.scissor = scissor;
        g_bound.scissorSet = true;
    }
    [g_encoder setVertexBuffer:vb offset:vOffset atIndex:0];
    if (std::memcmp(&g_bound.vertexUniforms, &vertexUniforms, sizeof(vertexUniforms)) != 0) {
        [g_encoder setVertexBytes:&vertexUniforms length:sizeof(vertexUniforms) atIndex:1];
        g_bound.vertexUniforms = vertexUniforms;
    }
    if (!g_bound.uniformsSet || std::memcmp(&g_bound.uniforms, &u, sizeof(u)) != 0) {
        [g_encoder setFragmentBytes:&u length:sizeof(u) atIndex:0];
        g_bound.uniforms = u;
        g_bound.uniformsSet = true;
    }
    [g_encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                          indexCount:numIndices
                           indexType:MTLIndexTypeUInt16
                         indexBuffer:ib
                   indexBufferOffset:iOffset];

    if (g_shadowEncoder != nil) {  // FRONTLINE_GX_VERIFY: the same draw with the ubershader
        id<MTLRenderCommandEncoder> e = g_shadowEncoder;
        [e setRenderPipelineState:Pipeline()];
        [e setDepthStencilState:depth];
        [e setCullMode:cull];
        [e setViewport:vp];
        [e setScissorRect:scissor];
        [e setVertexBuffer:vb offset:vOffset atIndex:0];
        [e setVertexBytes:&vertexUniforms length:sizeof(vertexUniforms) atIndex:1];
        [e setFragmentBytes:&u length:sizeof(u) atIndex:0];
        for (uint32_t m = 0; m < 8; m++) {
            if (u.texValid & (1u << m)) {
                [e setFragmentTexture:textures[m] atIndex:m];
                [e setFragmentSamplerState:samplers[m] atIndex:m];
            }
        }
        [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                      indexCount:numIndices
                       indexType:MTLIndexTypeUInt16
                     indexBuffer:ib
               indexBufferOffset:iOffset];
    }
    SnapshotCheck();
}

// FRONTLINE_GX_VERIFY: compares a frame copy drawn with specialized
// pipelines (`mine`) with the ubershader's (`theirs`), RGB of RGBA pixels.
void VerifyCompare(const uint8_t* mine, const uint8_t* theirs, uint32_t pixels) {
    uint32_t differ = 0, most = 0, first = 0;
    for (uint32_t i = 0; i < pixels; i++) {
        uint32_t d = 0;
        for (int c = 0; c < 3; c++) {
            d = std::max(d, (uint32_t)std::abs(mine[i * 4 + c] - theirs[i * 4 + c]));
        }
        if (d != 0 && differ++ == 0) {
            first = i;
        }
        most = std::max(most, d);
    }
    g_verifyCopies++;
    if (differ != 0) {
        g_verifyDiffering++;
        const uint8_t *m = mine + first * 4, *t = theirs + first * 4;
        std::fprintf(stderr,
                     "[gxverify] copy %u: %u of %u pixels differ, by up to %u (first: pixel %u, %u %u %u, not "
                     "%u %u %u)\n",
                     g_copyCount, differ, pixels, most, first, m[0], m[1], m[2], t[0], t[1], t[2]);
    }
    if (g_verifyCopies % 300 == 0) {
        std::fprintf(stderr, "[gxverify] %u frame copies compared, %u differed\n", g_verifyCopies, g_verifyDiffering);
    }
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
    MakeEfb();
    const uint32_t copyWidth = Scaled(g_copySrc[2]), copyHeight = Scaled(g_copySrc[3]);
    // Above the original resolution the frame has mipmaps, made while the
    // display shows it smaller than it is, so it still looks smooth
    // (display.mm).
    const bool mipmapped = g_scale > 1.0f;
    bool found = false;
    os_unfair_lock_lock(&g_xfbLock);
    for (Xfb& x : g_xfbs) {
        if (x.addr == addr || x.addr == 0) {
            // A new copy size (widescreen starting in a level) gets a new texture.
            if (!g_recordOnly &&
                (x.texture == nil || x.texture.width != copyWidth || x.texture.height != copyHeight)) {
                MTLTextureDescriptor* desc =
                    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                       width:copyWidth
                                                                      height:copyHeight
                                                                   mipmapped:mipmapped];
                desc.usage = MTLTextureUsageShaderRead;
                desc.storageMode = MTLStorageModePrivate;
                x.texture = [g_device newTextureWithDescriptor:desc];
            }
            x.addr = addr;
            x.stamp = g_copyCount;
            target = x.texture;
            if (g_hfr && x.pending != Recording()) {
                RetainRecord(g_record);
                ReleaseRecord(x.pending);
                x.pending = g_record;
            }
            found = true;
            break;
        }
    }
    os_unfair_lock_unlock(&g_xfbLock);
    if (!found) {
        return;
    }
    // A copy that clears Z leaves the EFB's depth unused: the next pass clears
    // it, so this one needn't store it.
    const bool clearDepth = clear && g_state.zUpdate;
    BeginPass();  // so a copy with no draws still clears
    EndPass(clearDepth, clear && g_state.colorUpdate);
    g_copiedSinceDraw = true;
    if (g_hfr) {
        RecordCopy(g_record, Scaled(g_copySrc[0]), Scaled(g_copySrc[1]), copyWidth, copyHeight);
    }
    if (g_recordOnly) {
        g_clearColorPending = clear && g_state.colorUpdate;
        g_clearDepthPending = clearDepth;
        std::memcpy(g_pendingClearColor, g_state.clearColor, 4);
        g_pendingClearZ = g_state.clearZ;
        uint32_t* words = static_cast<uint32_t*>(dest);
        words[1] = g_copyCount;
        words[0] = kXfbMagic;
        return;
    }
    id<MTLBlitCommandEncoder> blit = [g_commands blitCommandEncoder];
    uint32_t x0 = Scaled(g_copySrc[0]), y0 = Scaled(g_copySrc[1]);
    uint32_t w = std::min({copyWidth, (uint32_t)target.width, (uint32_t)g_efbColor.width - x0});
    uint32_t h = std::min({copyHeight, (uint32_t)target.height, (uint32_t)g_efbColor.height - y0});
    [blit copyFromTexture:g_efbColor
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(x0, y0, 0)
               sourceSize:MTLSizeMake(w, h, 1)
                toTexture:target
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    if (target.mipmapLevelCount > 1 && g_displayShrinks.load(std::memory_order_relaxed)) {
        [blit generateMipmapsForTexture:target];
    }
    if (g_hash) {
        const NSUInteger tw = target.width, th = target.height;
        id<MTLBuffer> pixels = [g_device newBufferWithLength:tw * th * 4 options:MTLResourceStorageModeShared];
        [blit copyFromTexture:target
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(tw, th, 1)
                            toBuffer:pixels
                   destinationOffset:0
              destinationBytesPerRow:tw * 4
            destinationBytesPerImage:tw * th * 4];
        const uint32_t copy = g_copyCount;
        [g_commands addCompletedHandler:^(id<MTLCommandBuffer>) {
            const uint64_t* words = static_cast<const uint64_t*>(pixels.contents);
            uint64_t h = 0xcbf29ce484222325ull;
            for (NSUInteger i = 0; i < tw * th / 2; i++) {
                h = (h ^ words[i]) * 0x100000001b3ull;
            }
            std::fprintf(stderr, "[gxhash] %u %lux%lu %016llx\n", copy, (unsigned long)tw, (unsigned long)th, h);
        }];
    }
    // FRONTLINE_GX_VERIFY: the copied rectangle of both EFBs, to compare.
    id<MTLCommandBuffer> shadow = g_shadowCommands;
    if (shadow != nil) {
        if (g_verifyMine.length < (NSUInteger)w * h * 4) {  // (each copy waits for the last)
            g_verifyMine = [g_device newBufferWithLength:(NSUInteger)w * h * 4 options:MTLResourceStorageModeShared];
            g_verifyTheirs = [g_device newBufferWithLength:(NSUInteger)w * h * 4 options:MTLResourceStorageModeShared];
        }
        auto readBack = [&](id<MTLBlitCommandEncoder> encoder, id<MTLTexture> efb, id<MTLBuffer> to) {
            [encoder copyFromTexture:efb
                             sourceSlice:0
                             sourceLevel:0
                            sourceOrigin:MTLOriginMake(x0, y0, 0)
                              sourceSize:MTLSizeMake(w, h, 1)
                                toBuffer:to
                       destinationOffset:0
                  destinationBytesPerRow:w * 4
                destinationBytesPerImage:(NSUInteger)w * h * 4];
        };
        readBack(blit, g_efbColor, g_verifyMine);
        id<MTLBlitCommandEncoder> shadowBlit = [shadow blitCommandEncoder];
        readBack(shadowBlit, g_shadowColor, g_verifyTheirs);
        [shadowBlit endEncoding];
    }
    [blit endEncoding];
    g_clearColorPending = clear && g_state.colorUpdate;
    g_clearDepthPending = clearDepth;
    std::memcpy(g_pendingClearColor, g_state.clearColor, 4);
    g_pendingClearZ = g_state.clearZ;
    Commit();
    if (shadow != nil) {
        [shadow waitUntilCompleted];
        [g_lastCommitted waitUntilCompleted];
        VerifyCompare(static_cast<const uint8_t*>(g_verifyMine.contents),
                      static_cast<const uint8_t*>(g_verifyTheirs.contents), w * h);
    }
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

// The console's GXDrawDone waits for the GPU to finish. Here the frame's
// work is only submitted: the GPU doesn't read game memory, so the game can
// go on to the next frame while it draws. Waiting for the frame before keeps
// at most two in flight.
// FRONTLINE_HFR_VERIFY: replays a finished record unmoved and compares it with
// the frame drawn here, `drawn`.
void HfrVerify(FrameRecord* r, id<MTLTexture> drawn) {
    static ReplayTargets targets;
    id<MTLCommandBuffer> commands = [g_queue commandBuffer];
    id<MTLTexture> replayed = ReplayFrame(commands, r, nullptr, targets, false, false);
    const NSUInteger w = std::min(drawn.width, replayed.width), h = std::min(drawn.height, replayed.height);
    id<MTLBuffer> mine = [g_device newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
    id<MTLBuffer> theirs = [g_device newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    for (int i = 0; i < 2; i++) {
        [blit copyFromTexture:i == 0 ? replayed : drawn
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(w, h, 1)
                            toBuffer:i == 0 ? mine : theirs
                   destinationOffset:0
              destinationBytesPerRow:w * 4
            destinationBytesPerImage:w * h * 4];
    }
    [blit endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    const uint8_t* a = static_cast<const uint8_t*>(mine.contents);
    const uint8_t* b = static_cast<const uint8_t*>(theirs.contents);
    uint32_t differing = 0, maxDiff = 0;
    for (NSUInteger i = 0; i < w * h; i++) {
        uint32_t d = 0;
        for (int c = 0; c < 3; c++) {
            d = std::max(d, (uint32_t)std::abs(a[i * 4 + c] - b[i * 4 + c]));
        }
        differing += d != 0;
        maxDiff = std::max(maxDiff, d);
    }
    g_hfrFrames++;
    if (differing != 0) {
        g_hfrDiffering++;
        if (g_hfrDiffering <= 20) {
            std::fprintf(stderr, "[hfrverify] copy %u: %u of %lu pixels differ (by up to %u), %u draws\n", g_copyCount,
                         differing, (unsigned long)(w * h), maxDiff, RecordDraws(r));
        }
    }
    if (g_hfrFrames % 600 == 0) {
        std::fprintf(stderr, "[hfrverify] %u frames, %u differing\n", g_hfrFrames, g_hfrDiffering);
    }
}

// FRONTLINE_HFR_TRACE: the camera's heading and elevation (radians) and how
// they changed since the last frame, beside the mouse look the game took
// for it (the last take before the last frame's camera: drawn now).
void TraceView(const FrameView& now, const FrameView& prev) {
    auto angles = [](const FrameView& v, double* heading, double* elevation) {
        // Forward is the eye's -z: the third row of world to eye, negated.
        const double f[3] = {-v.view[2][0], -v.view[2][1], -v.view[2][2]};
        *heading = std::atan2(f[1], f[0]);
        *elevation = std::asin(std::clamp(f[2], -1.0, 1.0));
    };
    double h, e, ph, pe;
    angles(now, &h, &e);
    angles(prev, &ph, &pe);
    float right = 0, up = 0;
    if (prev.valid) {
        PendingLook(prev.lookFrom, &right, &up);  // (approximate: takes since then too)
    }
    double dh = std::remainder(h - ph, 2 * M_PI);
    std::fprintf(stderr,
                 "[hfrtrace] retrace %u%s flags %u: heading %.4f elevation %.4f, turned %+.5f %+.5f; game look "
                 "yaw %+.5f pitch %+.5f (scale %.3f, pitch %.3f)\n",
                 now.retrace, now.valid ? "" : " (no view)", now.flags, h, e, prev.valid ? dh : 0.0,
                 prev.valid ? e - pe : 0.0, -prev.lookScale * right, -prev.lookScale * up, now.lookScale, now.pitch);
}

// The frame's end (GXDrawDone after its copy): its record is finished, and is
// what the frame buffers it was copied to show from now on.
void EndRecordedFrame() {
    FrameRecord* r = g_record;
    g_record = nullptr;
    FrameView view = g_frameView;
    view.valid = g_frameViews == 1;
    view.retrace = GetDisplayedFrame().retrace;
    view.cullWiden[0] = (float)g_cullWiden[0];
    view.cullWiden[1] = (float)g_cullWiden[1];
    FrameChange(g_lastView, view);
    SetRecordViews(r, view, g_lastView);
    if (g_hfrTrace) {
        TraceView(view, g_lastView);
    }
    g_lastTurn = TurnRate(view, g_lastView);
    g_lastView = view;
    std::memset(&g_frameView, 0, sizeof(g_frameView));
    g_frameViews = 0;
    FinishRecord(r);
    id<MTLTexture> drawn = nil;
    os_unfair_lock_lock(&g_xfbLock);
    for (Xfb& x : g_xfbs) {
        if (x.pending == r) {
            ReleaseRecord(x.record);
            x.record = r;
            x.pending = nullptr;
            drawn = x.texture;
        }
    }
    os_unfair_lock_unlock(&g_xfbLock);
    if (g_hfrVerify && drawn != nil) {
        HfrVerify(r, drawn);
    }
    ReleaseRecord(r);
}

void DrawDone() {
    const uint64_t start = g_perf.on ? NowNanos() : 0;
    // Metal hands back command buffers, encoders and pass descriptors
    // autoreleased. Without a pool, a game thread would keep every one (and
    // the buffers they reference) until it ended, which with the game
    // rebooting in place is never.
    static thread_local void* pool;
    if (pool != nullptr) {
        objc_autoreleasePoolPop(pool);
    }
    pool = objc_autoreleasePoolPush();
    Commit();
    if (g_record != nullptr && g_copiedSinceDraw) {
        EndRecordedFrame();
    }
    if (g_sync && g_lastCommitted != nil) {
        [g_lastCommitted waitUntilCompleted];
    } else if (g_previousFrame != nil) {
        [g_previousFrame waitUntilCompleted];
    }
    g_previousFrame = g_lastCommitted;
    if (g_perf.on) {
        PerfFrame(start, NowNanos() - start);
    }
}

id<MTLDevice> Device() {
    InitDevice();
    return g_device;
}

id<MTLCommandQueue> Queue() {
    InitDevice();
    return g_queue;
}

void SetDisplayShrinks(bool shrinks) { g_displayShrinks.store(shrinks, std::memory_order_relaxed); }
bool DisplayShrinks() { return g_displayShrinks.load(std::memory_order_relaxed); }

FrameRecord* XfbRecord(uint32_t addr) {
    FrameRecord* r = nullptr;
    os_unfair_lock_lock(&g_xfbLock);
    for (Xfb& x : g_xfbs) {
        if (x.addr == addr && addr != 0 && x.record != nullptr && Stamped(x)) {
            r = x.record;
            RetainRecord(r);
        }
    }
    os_unfair_lock_unlock(&g_xfbLock);
    return r;
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

// Rebooting (os/reboot.cpp): the decoded textures go, since the next run's
// are elsewhere and the cache would only grow. The pipelines, which take
// longest to make, stay; so do the frame buffers' textures. The game calls
// GXInit again for the rest.
void frontline::os::ResetGx() {
    using namespace frontline::gx;
    if (g_device == nil) {
        return;
    }
    Commit();
    if (g_lastCommitted != nil) {
        [g_lastCommitted waitUntilCompleted];
    }
    g_previousFrame = nil;
    if (g_textures.entries != nullptr) {
        for (uint32_t i = 0; i <= g_textures.mask; i++) {
            if (g_textures.entries[i].texture != nullptr) {
                CFRelease(g_textures.entries[i].texture);
            }
        }
        std::memset(g_textures.entries, 0, (g_textures.mask + 1) * sizeof(TexEntry));
        g_textures.count = 0;
    }
}

extern "C" void NativeDrawClass(int cls) {
    static_assert(NATIVE_DRAW_WORLD == frontline::gx::kDrawWorld && NATIVE_DRAW_SKY == frontline::gx::kDrawSky &&
                  NATIVE_DRAW_VIEWMODEL == frontline::gx::kDrawViewModel);
    frontline::gx::g_drawClass = (frontline::gx::DrawClass)cls;
}

extern "C" void NativeFrameView(const float worldToEye[3][4], float scaleX, float scaleY) {
    using namespace frontline::gx;
    if (!g_hfr) {
        return;
    }
    std::memcpy(g_frameView.view, worldToEye, sizeof(g_frameView.view));
    g_frameView.scale[0] = scaleX;
    g_frameView.scale[1] = scaleY;
    g_frameViews++;

    // How far the frames drawn from this one may turn past it, for culling
    // (NativeCullTan, next): it's shown for up to about two retraces from
    // now. With the mouse, that's the movement not drawn yet plus about two
    // and a half retraces more at its recent rate; otherwise the last turn,
    // carried on as long.
    // Flicks come in bursts, so the mouse's rate is its recent top rate,
    // fading over about half a second; a flick from rest still starts inside
    // the base margin.
    double x = 2.5 * g_lastTurn, y = x, base = 0.03;
    if ((g_frameView.flags & kViewLook) != 0) {
        float right = 0, up = 0, rateRight = 0, rateUp = 0;
        frontline::PendingLook(g_frameView.lookFrom, &right, &up);
        frontline::LookRate(&rateRight, &rateUp);
        const double scale = g_frameView.lookScale;
        const double fade = std::pow(0.95, std::max(1u, g_frameView.step));
        g_lookPeak[0] = std::max(scale * rateRight, g_lookPeak[0] * fade);
        g_lookPeak[1] = std::max(scale * rateUp, g_lookPeak[1] * fade);
        x = std::max(x, scale * std::fabs(right) + 3.0 * g_lookPeak[0]);
        y = std::max(y, scale * std::fabs(up) + 3.0 * g_lookPeak[1]);
        base = 0.06;
    } else {
        g_lookPeak[0] = g_lookPeak[1] = 0;
    }
    // A turn about the world's up tilts what's at the edges too when looking
    // up or down, and the other way round: some of each, plus a little for
    // walking.
    g_cullWiden[0] = std::min(1.0, x + 0.5 * y + base);
    g_cullWiden[1] = std::min(1.0, y + 0.5 * x + base);
}

extern "C" void NativePlayerView(int flags, float lookScale, float pitch) {
    static_assert(NATIVE_VIEW_ATTACHED == frontline::gx::kViewAttached && NATIVE_VIEW_ON_GUN == frontline::gx::kViewOnGun &&
                  NATIVE_VIEW_LOOK == frontline::gx::kViewLook);
    if (!frontline::gx::g_hfr) {
        return;
    }
    frontline::gx::g_frameView.flags = (uint32_t)flags;
    frontline::gx::g_frameView.lookScale = lookScale;
    frontline::gx::g_frameView.pitch = pitch;
    // The retraces since the last call are the game's step before this
    // frame's update (its loop starts as a frame is shown): this camera moves
    // on by the next ones.
    const uint32_t retrace = frontline::GetDisplayedFrame().retrace;
    const uint32_t last = frontline::gx::g_playerRetrace;
    frontline::gx::g_frameView.step = last != 0 && retrace - last <= 6 ? retrace - last : 0;
    frontline::gx::g_playerRetrace = retrace;
    // This frame's camera was set before the update that applies the take
    // just made: it and everything after are still to be drawn.
    const uint32_t takes = frontline::LookTakes();
    frontline::gx::g_frameView.lookFrom = takes > 0 ? takes - 1 : 0;
}

// Wider by what the frames between might turn (NativeFrameView works it out).
extern "C" float NativeCullTan(float t, int vertical) {
    if (!frontline::gx::g_hfr || t <= 0.0f) {
        return t;
    }
    const double widen = frontline::gx::g_cullWiden[vertical != 0 ? 1 : 0];
    return (float)std::tan(std::min(std::atan((double)t) + widen, 1.45));
}

static bool ReadDisplayedFrameInPool(uint8_t* rgba, uint32_t* width, uint32_t* height);

bool frontline::ReadDisplayedFrame(uint8_t* rgba, uint32_t* width, uint32_t* height) {
    // The callers' threads have no pool of their own: each command buffer
    // below would keep its 1 MB pixel buffer.
    @autoreleasepool {
        return ReadDisplayedFrameInPool(rgba, width, height);
    }
}

static bool ReadDisplayedFrameInPool(uint8_t* rgba, uint32_t* width, uint32_t* height) {
    namespace gx = frontline::gx;
    const uint32_t xfb = frontline::GetDisplayedFrame().xfb;
    id<MTLTexture> texture = gx::XfbTexture(xfb);
    if (texture == nil) {
        // Only recorded: drawn as recorded (one caller at a time).
        gx::FrameRecord* r = gx::XfbRecord(xfb);
        if (r == nullptr) {
            return false;
        }
        static gx::ReplayTargets targets;
        id<MTLCommandBuffer> commands = [gx::Queue() commandBuffer];
        texture = gx::ReplayFrame(commands, r, nullptr, targets, false, false);
        [commands commit];
        gx::ReleaseRecord(r);
    }
    const NSUInteger w = texture.width, h = texture.height;
    *width = (uint32_t)w;
    *height = (uint32_t)h;
    if (rgba != nullptr) {
        // The frame texture is the GPU's alone: copy it out. On the same queue,
        // this runs after the copy that made the frame.
        id<MTLBuffer> pixels = [gx::Device() newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> commands = [gx::Queue() commandBuffer];
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        [blit copyFromTexture:texture
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(w, h, 1)
                            toBuffer:pixels
                   destinationOffset:0
              destinationBytesPerRow:w * 4
            destinationBytesPerImage:w * h * 4];
        [blit endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        std::memcpy(rgba, pixels.contents, w * h * 4);
    }
    return true;
}
