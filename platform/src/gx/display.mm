// The video output on screen: what the TV would show.
//
// The displayed frame buffer (VI) is either a frame GX copied, kept as a
// texture, or one the CPU wrote (the movie player): YUV 4:2:2 in the frame
// buffer's memory, two pixels a word (Y0 U Y1 V), decoded here. With
// Rewritten's high frame rate, a frame GX made is drawn here from its record
// (gx_record.h) for each refresh of the screen.

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <mach/mach_time.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// The game's headers define BOOL as int; Objective-C's is bool.
#define BOOL FRONTLINE_GAME_BOOL
#include "frontline/display.h"
#include "frontline/platform.h"
#include "gx_metal.h"
#include "gx_record.h"
#include "pad_internal.h"
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

// Frames rendered above the original resolution have mipmaps, for windows
// smaller than the render; the original's have one level.
fragment float4 display_rgb(Out in [[stage_in]], texture2d<float> frame [[texture(0)]]) {
    constexpr sampler s(filter::linear, mip_filter::linear, address::clamp_to_edge);
    return float4(frame.sample(s, in.uv).rgb, 1.0);
}

// The YUV frame as a texture of words: r = V, g = Y1, b = U, a = Y0.
static float3 yuv_pixel(texture2d<float> frame, int x, int y, uint width, uint height) {
    x = clamp(x, 0, int(width) - 1);
    y = clamp(y, 0, int(height) - 1);
    float4 w = frame.read(uint2(x / 2, y)) * 255.0;
    float c = 1.164 * (((x & 1) != 0 ? w.g : w.a) - 16.0);
    float u = w.b - 128.0, v = w.r - 128.0;
    return saturate(float3(c + 1.596 * v, c - 0.392 * u - 0.813 * v, c + 2.017 * u) / 255.0);
}

// Filtered between the four nearest pixels, as the RGB frame is.
fragment float4 display_yuv(Out in [[stage_in]], texture2d<float> frame [[texture(0)]]) {
    uint width = frame.get_width() * 2, height = frame.get_height();
    float2 p = in.uv * float2(width, height) - 0.5;
    int2 i = int2(floor(p));
    float2 f = p - floor(p);
    float3 top = mix(yuv_pixel(frame, i.x, i.y, width, height), yuv_pixel(frame, i.x + 1, i.y, width, height), f.x);
    float3 bottom =
        mix(yuv_pixel(frame, i.x, i.y + 1, width, height), yuv_pixel(frame, i.x + 1, i.y + 1, width, height), f.x);
    return float4(mix(top, bottom, f.y), 1.0);
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

// `drawn`: the frame drawn from its record (DrawRecorded), else nil.
void Encode(id<MTLCommandBuffer> commands, id<MTLTexture> target, const frontline::DisplayedFrame& frame,
            id<MTLTexture> drawn) {
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];

    if (frame.xfb != 0 && !frame.black && frame.width != 0 && frame.height != 0) {
        id<MTLTexture> texture = drawn != nil ? drawn : frontline::gx::XfbTexture(frame.xfb);
        bool yuv = texture == nil;
        if (yuv) {
            texture = YuvTexture(frame);
        }
        // The screen (4:3, or wider for a widescreen level: the frame's width
        // on the 480-line screen, at whatever resolution it was rendered),
        // and the frame's lines on its 480.
        double w = target.width, h = target.height;
        double screenW = std::min(w, h * 4 / 3), screenH = screenW * 3 / 4;
        uint64_t wide = yuv ? 0 : (uint64_t)texture.width * frame.height, tall = (uint64_t)texture.height * 480;
        if (wide * 3 > tall * 4) {
            double aspect = (double)wide / tall;
            screenW = std::min(w, h * aspect);
            screenH = screenW / aspect;
        }
        double frameH = screenH * std::min(1.0, frame.height / 480.0);
        if (!yuv) {
            frontline::gx::SetDisplayShrinks(screenW < texture.width || frameH < texture.height);
        }
        [encoder setViewport:(MTLViewport){(w - screenW) / 2, (h - frameH) / 2, screenW, frameH, 0, 1}];
        [encoder setRenderPipelineState:yuv ? g_yuv : g_rgb];
        [encoder setFragmentTexture:texture atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    [encoder endEncoding];
}

// The display's picture of `frame` (`drawn`: from its record), at `width` x
// `height`, written to `path` as a PPM image.
void WritePicture(const frontline::DisplayedFrame& frame, id<MTLTexture> drawn, MTLPixelFormat format,
                  uint32_t width, uint32_t height, id<MTLCommandBuffer> commands, const char* path) {
    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                    width:width
                                                                                   height:height
                                                                                mipmapped:NO];
    desc.storageMode = MTLStorageModeShared;
    desc.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [frontline::gx::Device() newTextureWithDescriptor:desc];
    Encode(commands, target, frame, drawn);
    [commands commit];
    [commands waitUntilCompleted];
    uint32_t w = (uint32_t)target.width, h = (uint32_t)target.height;
    uint8_t* bgra = static_cast<uint8_t*>(std::malloc((size_t)w * h * 4));
    [target getBytes:bgra bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
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

// FRONTLINE_DISPLAY_DUMP=<folder>: what the window shows, as a PPM image every
// second (for checking the display without a screen capture).
void DumpCheck(const frontline::DisplayedFrame& frame, id<MTLTexture> drawable, id<MTLTexture> drawn, double time) {
    static const char* folder = std::getenv("FRONTLINE_DISPLAY_DUMP");
    static uint32_t dumps;
    static double next;
    if (folder == nullptr || time < next) {
        return;
    }
    next = next == 0 ? time + 1 : next + 1;
    char path[1024];
    std::snprintf(path, sizeof(path), "%s/display%03u.ppm", folder, ++dumps);
    WritePicture(frame, drawn, drawable.pixelFormat, (uint32_t)drawable.width, (uint32_t)drawable.height,
                 [frontline::gx::Queue() commandBuffer], path);
}

// FRONTLINE_PERF=1: a line a second on how the display shows the game's
// frames: presents a second, and how many presents each frame got (2 at
// 120 Hz; 1 at 60 Hz, with a 2 now and then for the 59.94/60 beat).
// With the high frame rate, also what drawing the frames from their records
// costs (the CPU's time encoding them and the GPU's time drawing them), and
// how evenly the camera drawn moves from present to present: steps that
// differ by more than half from the one before ("uneven"), and steps back
// against it ("back").
struct DisplayPerf {
    bool on = std::getenv("FRONTLINE_PERF") != nullptr;
    double start, lastTime, maxGap;
    uint32_t presents, lastLatch, shownTimes;
    uint32_t shown[4];  // once, twice, three times, more
    uint32_t replays;
    uint64_t encode;            // nanoseconds
    std::atomic<uint64_t> gpu;  // nanoseconds, from completion handlers
    // The camera drawn: its forward and position, and their last steps.
    bool camera, stepped;
    double forward[3], position[3], turnStep[3], moveStep[3];
    uint32_t uneven[2], back[2];  // turning, moving
    uint32_t overruns;  // presents turned past the culling the game did (gaps at the edges)
    double overrun;     // by how far at most (radians)
} g_displayPerf;

// FRONTLINE_HFR_TRACE=2: a line a present on the camera drawn.
const bool g_presentTrace = [] {
    const char* trace = std::getenv("FRONTLINE_HFR_TRACE");
    return trace != nullptr && std::atoi(trace) >= 2;
}();

// One step of the camera drawn against the last (`kind`: 0 turning, 1
// moving): counts it if uneven.
void PerfStep(int kind, const double step[3], double last[3], double threshold) {
    DisplayPerf& p = g_displayPerf;
    double dot = 0, size = 0, lastSize = 0, change = 0;
    for (int i = 0; i < 3; i++) {
        dot += step[i] * last[i];
        size += step[i] * step[i];
        lastSize += last[i] * last[i];
        change += (step[i] - last[i]) * (step[i] - last[i]);
    }
    const double big = std::sqrt(std::max(size, lastSize));
    if (p.stepped && big > threshold) {
        if (dot < 0) {
            p.back[kind]++;
        } else if (std::sqrt(change) > 0.5 * big) {
            p.uneven[kind]++;
        }
    }
    std::memcpy(last, step, sizeof(double) * 3);
}

// `v`, the camera drawn from frame `frame`, `ticks` retraces after it was due.
void PerfCamera(const frontline::gx::FrameView& v, const frontline::gx::FrameView& frame, double time, double ticks) {
    DisplayPerf& p = g_displayPerf;
    if (!p.on && !g_presentTrace) {
        return;
    }
    // Where it looks, in the frame's eye space (forward is -z), against the
    // frame's culling.
    double e[3];
    for (int i = 0; i < 3; i++) {
        e[i] = -(frame.view[i][0] * v.view[2][0] + frame.view[i][1] * v.view[2][1] + frame.view[i][2] * v.view[2][2]);
    }
    const double past = std::max(std::fabs(std::atan2(e[0], -e[2])) - frame.cullWiden[0],
                                 std::fabs(std::atan2(e[1], -e[2])) - frame.cullWiden[1]);
    if (past > 0) {
        p.overruns++;
        p.overrun = std::max(p.overrun, past);
    }
    double forward[3], position[3];
    for (int i = 0; i < 3; i++) {
        forward[i] = -v.view[2][i];
        position[i] = -(v.view[0][i] * v.view[0][3] + v.view[1][i] * v.view[1][3] + v.view[2][i] * v.view[2][3]);
    }
    if (p.camera) {
        double turn[3], move[3];
        for (int i = 0; i < 3; i++) {
            turn[i] = forward[i] - p.forward[i];
            move[i] = position[i] - p.position[i];
        }
        PerfStep(0, turn, p.turnStep, 3e-4);  // radians
        PerfStep(1, move, p.moveStep, 3e-4);  // meters
        p.stepped = true;
    }
    if (g_presentTrace) {
        std::fprintf(stderr,
                     "[hfrpresent] %.4f frame %u +%.3f flags %u: position %.4f %.4f %.4f forward %.5f %.5f %.5f "
                     "(frame %.4f %.4f %.4f)\n",
                     time, frame.retrace, ticks, frame.flags, position[0], position[1], position[2], forward[0],
                     forward[1], forward[2], -(frame.view[0][0] * frame.view[0][3] + frame.view[1][0] * frame.view[1][3] +
                                               frame.view[2][0] * frame.view[2][3]),
                     -(frame.view[0][1] * frame.view[0][3] + frame.view[1][1] * frame.view[1][3] +
                       frame.view[2][1] * frame.view[2][3]),
                     -(frame.view[0][2] * frame.view[0][3] + frame.view[1][2] * frame.view[1][3] +
                       frame.view[2][2] * frame.view[2][3]));
    }
    std::memcpy(p.forward, forward, sizeof(forward));
    std::memcpy(p.position, position, sizeof(position));
    p.camera = true;
}

void PerfPresent(const frontline::DisplayedFrame& frame, double time) {
    DisplayPerf& p = g_displayPerf;
    if (!p.on) {
        return;
    }
    if (p.start == 0) {
        p.start = time;
    }
    p.presents++;
    if (p.lastTime != 0) {
        p.maxGap = std::max(p.maxGap, time - p.lastTime);
    }
    p.lastTime = time;
    if (frame.latchRetrace != p.lastLatch) {
        if (p.shownTimes != 0) {
            p.shown[std::min(p.shownTimes, 4u) - 1]++;
        }
        p.lastLatch = frame.latchRetrace;
        p.shownTimes = 0;
    }
    p.shownTimes++;
    if (time - p.start >= 1.0) {
        const double n = std::max(p.presents, 1u);
        std::fprintf(stderr,
                     "[perf] display: %.1f presents/s (longest gap %.1f ms); frames shown once %u, twice %u, 3x %u, "
                     "more %u | %u replayed: encode %.2f ms, gpu %.2f ms a present; camera steps uneven %u turning, %u "
                     "moving, back %u turning, %u moving; past the culling %u (by up to %.1f deg)\n",
                     p.presents / (time - p.start), p.maxGap * 1e3, p.shown[0], p.shown[1], p.shown[2], p.shown[3],
                     p.replays, p.encode / n * 1e-6, p.gpu.exchange(0) / n * 1e-6, p.uneven[0], p.uneven[1], p.back[0],
                     p.back[1], p.overruns, p.overrun * 57.29578);
        p.start = time;
        p.presents = 0;
        p.maxGap = 0;
        p.replays = 0;
        p.encode = 0;
        std::memset(p.uneven, 0, sizeof(p.uneven));
        std::memset(p.back, 0, sizeof(p.back));
        p.overruns = 0;
        p.overrun = 0;
        std::memset(p.shown, 0, sizeof(p.shown));
    }
}

// The high frame rate's frames: the displayed frame drawn again from its
// record, moved on to `time`. FRONTLINE_HFR_INTERPOLATE=1 moves it between the
// last two frames instead (a retrace behind), and FRONTLINE_HFR_LOOK=0 leaves
// the mouse's turns to the game's frames, to compare.
struct HfrOptions {
    frontline::gx::MotionLimits limits;
    bool look = true;
    HfrOptions() {
        limits.interpolate = std::getenv("FRONTLINE_HFR_INTERPOLATE") != nullptr;
        const char* look = std::getenv("FRONTLINE_HFR_LOOK");
        this->look = look == nullptr || std::atoi(look) != 0;
    }
} g_hfr;

// How the game's mouse look turns the camera: its yaw is the heading (about
// the world's up) and its pitch the elevation's opposite, as
// FRONTLINE_HFR_TRACE shows.
constexpr float kYawToHeading = 1.0f, kPitchToElevation = -1.0f;

bool Motion(frontline::gx::FrameRecord* r, const frontline::DisplayedFrame& frame, double time,
            frontline::gx::ReplayMotion& motion, frontline::gx::FrameView* shown = nullptr) {
    using namespace frontline::gx;
    FrameView now, prev;
    RecordViews(r, &now, &prev);
    LookTurn look;
    const LookTurn* turn = nullptr;
    if (g_hfr.look && (now.flags & kViewLook) != 0 && frontline::IsPlaying()) {
        float right = 0, up = 0;
        if (frontline::PendingLook(now.lookFrom, &right, &up)) {
            // As AddMouseLook (player.cpp) will add it; the game keeps its
            // pitch within +-1.45.
            look.yaw = kYawToHeading * -now.lookScale * right;
            look.pitch = kPitchToElevation * -now.lookScale * up;
            const float a = kPitchToElevation * (-1.45f - now.pitch), b = kPitchToElevation * (1.45f - now.pitch);
            look.pitchMin = std::min(a, b);
            look.pitchMax = std::max(a, b);
            turn = &look;
        }
    }
    if (turn == nullptr) {
        now.flags &= ~kViewLook;
    }
    return FrameMotion(now, prev, (time - frame.latchTime) / frontline::kRetraceSeconds, turn, g_hfr.limits, motion,
                       shown);
}

frontline::gx::ReplayTargets g_targets;

// FRONTLINE_HFR_DUMP=<folder>: every second, the displayed frame drawn for 0,
// 1/4, 1/2 and 3/4 of a retrace on, as hfr<second>_<quarter>.ppm.
void HfrDumpCheck(frontline::gx::FrameRecord* r, const frontline::DisplayedFrame& frame, double time) {
    static const char* folder = std::getenv("FRONTLINE_HFR_DUMP");
    static uint32_t dumps;
    static double next;
    static frontline::gx::ReplayTargets targets;
    if (folder == nullptr || time < next) {
        return;
    }
    next = next == 0 ? time + 1 : next + 1;
    dumps++;
    for (int quarter = 0; quarter < 4; quarter++) {
        id<MTLCommandBuffer> commands = [frontline::gx::Queue() commandBuffer];
        frontline::gx::ReplayMotion motion;
        const double at = frame.latchTime + quarter * 0.25 * frontline::kRetraceSeconds;
        const bool moved = Motion(r, frame, at, motion);
        id<MTLTexture> drawn = frontline::gx::ReplayFrame(commands, r, moved ? &motion : nullptr, targets, false, false);
        char path[1024];
        std::snprintf(path, sizeof(path), "%s/hfr%03u_%d.ppm", folder, dumps, quarter);
        WritePicture(frame, drawn, MTLPixelFormatBGRA8Unorm, 960, 720, commands, path);
    }
}

// Draws the frame for `time` from its record, into `commands`. That's the
// frame flushed for the next retrace if it's due by then (as a screen showing
// it as soon as the console would), else the one shown. `frame` becomes the
// one drawn. Nil when it has no record (then the frame GX copied shows).
id<MTLTexture> DrawRecorded(id<MTLCommandBuffer> commands, frontline::DisplayedFrame& frame, double time) {
    frontline::gx::FrameRecord* r = nullptr;
    if (frame.nextXfb != 0 && !frame.nextBlack && time >= frame.nextLatchTime) {
        r = frontline::gx::XfbRecord(frame.nextXfb);
        if (r != nullptr) {
            frame.xfb = frame.nextXfb;
            frame.black = false;
            frame.latchRetrace = frame.retrace + 1;
            frame.latchTime = frame.nextLatchTime;
        }
    }
    if (r == nullptr && !frame.black && frame.xfb != 0) {
        r = frontline::gx::XfbRecord(frame.xfb);
    }
    if (r == nullptr) {
        return nil;
    }
    const uint64_t start = g_displayPerf.on ? mach_continuous_time() : 0;
    frontline::gx::ReplayMotion motion;
    frontline::gx::FrameView shown;
    const bool moved = Motion(r, frame, time, motion, &shown);
    if (shown.valid && (g_displayPerf.on || g_presentTrace)) {
        frontline::gx::FrameView now, prev;
        frontline::gx::RecordViews(r, &now, &prev);
        PerfCamera(shown, now, time, (time - frame.latchTime) / frontline::kRetraceSeconds);
    }
    id<MTLTexture> drawn = frontline::gx::ReplayFrame(commands, r, moved ? &motion : nullptr, g_targets,
                                                      frontline::RenderScale() > 1.0f, frontline::gx::DisplayShrinks());
    HfrDumpCheck(r, frame, time);
    frontline::gx::ReleaseRecord(r);
    if (g_displayPerf.on) {
        static mach_timebase_info_data_t tb;
        if (tb.denom == 0) {
            mach_timebase_info(&tb);
        }
        g_displayPerf.replays++;
        g_displayPerf.encode += (mach_continuous_time() - start) * tb.numer / tb.denom;
    }
    return drawn;
}

// Draws the frame for `time` into `target`; presents `drawable` if there is one.
void Show(id<MTLTexture> target, id<CAMetalDrawable> drawable, double time) {
    MakePipelines(target.pixelFormat);
    frontline::DisplayedFrame frame = frontline::GetDisplayedFrame();
    id<MTLCommandBuffer> commands = [frontline::gx::Queue() commandBuffer];
    id<MTLTexture> drawn = DrawRecorded(commands, frame, time);
    Encode(commands, target, frame, drawn);
    if (drawable != nil) {
        [commands presentDrawable:drawable];
    }
    if (g_displayPerf.on) {
        [commands addCompletedHandler:^(id<MTLCommandBuffer> done) {
            g_displayPerf.gpu.fetch_add((uint64_t)((done.GPUEndTime - done.GPUStartTime) * 1e9));
        }];
    }
    [commands commit];
    PerfPresent(frame, time);
    DumpCheck(frame, target, drawn, time);
    if (drawable == nil) {
        [commands waitUntilCompleted];  // offscreen: one frame at a time, as drawables would allow
    }
}

}  // namespace

id<MTLDevice> frontline::MetalDevice() { return gx::Device(); }

void frontline::Present(id<CAMetalDrawable> drawable, double time) {
    if (drawable != nil) {
        Show(drawable.texture, drawable, time);
    }
}

void frontline::StartOffscreenDisplay(double rate) {
    static double s_rate;
    s_rate = rate;
    pthread_t thread;
    pthread_create(
        &thread, nullptr,
        [](void*) -> void* {
            pthread_setname_np("frontline display");
            MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                            width:1280
                                                                                           height:960
                                                                                        mipmapped:NO];
            desc.usage = MTLTextureUsageRenderTarget;
            desc.storageMode = MTLStorageModePrivate;
            id<MTLTexture> target = [gx::Device() newTextureWithDescriptor:desc];
            mach_timebase_info_data_t tb;
            mach_timebase_info(&tb);
            const uint64_t start = mach_absolute_time();
            for (uint64_t n = 1;; n++) {
                const uint64_t ns = (uint64_t)(n * 1e9 / s_rate);
                const uint64_t due = start + ns * tb.denom / tb.numer;
                mach_wait_until(due);
                @autoreleasepool {
                    Show(target, nil, (double)due * tb.numer / tb.denom * 1e-9);
                }
            }
            return nullptr;
        },
        nullptr);
    pthread_detach(thread);
}
