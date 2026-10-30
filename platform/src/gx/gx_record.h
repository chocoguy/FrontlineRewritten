// Recorded frames, for Rewritten's high frame rate (frontline::HighFrameRate).
//
// The display shows up to 120 frames a second while the game makes 59.94. The
// game's own drawing can't run twice (it moves the game on: bones, random
// numbers, timers), so the frames in between are drawn again from what GX
// recorded: each draw's vertices as the vertex stage left them (clip space),
// its state and textures, and the frame's passes and copy. A replay can move
// the positions to another camera, per class of draw (the vertex shader's
// mode 1; frame_motion.h works out how).
//
// A record is built on the game thread through a frame and finished at the
// frame's end (GXDrawDone after its copy). From then on it doesn't change, and
// any thread may replay it. Records are reference-counted; the vertex memory
// they use goes back to the pool when the last record using it is released.
// Nothing here uses operator new (the game owns it on game threads).
#pragma once

#include <cstdint>

#import <Metal/Metal.h>

#include "frame_motion.h"
#include "gx_shader.h"

namespace frontline::gx {

// What a draw's positions are relative to, which says how a replay moves it.
enum DrawClass : uint32_t {
    kDrawWorld,      // the scene, through the camera
    kDrawSky,        // the sky box, through the camera's rotation only
    kDrawViewModel,  // the weapon in hand: fixed to the camera
    kDrawScreen,     // orthographic (HUD, text, fades, movies): fixed to the screen
    kNumDrawClasses,
};
static_assert(kNumDrawClasses == kMotionClasses);

// One draw, as the backend made it (gx_metal.mm SubmitTriangles).
struct RecordedDraw {
    // Pipelines, depth states and samplers live as long as their caches: for
    // the run.
    __unsafe_unretained id<MTLRenderPipelineState> pipeline;
    __unsafe_unretained id<MTLDepthStencilState> depth;
    __unsafe_unretained id<MTLSamplerState> samplers[8];
    CFTypeRef textures[8];  // retained by the record (RecordDraw)
    uint32_t textureMask;
    uint32_t vertexChunk, indexChunk;  // indices into the record's vertex memory
    uint32_t vertexOffset, indexOffset, numIndices;
    uint32_t stride, numTexGens;
    uint32_t fragUniforms;  // index of the record's FragUniforms
    MTLCullMode cull;
    MTLViewport viewport;
    MTLScissorRect scissor;
    float projection[7];  // the projection the positions were made with
    DrawClass cls;
};

struct FrameRecord;

// The game thread's, through a frame. `samples`: a pixel, for MSAA (the
// record's pipelines are made for that many).
FrameRecord* NewRecord(uint32_t efbWidth, uint32_t efbHeight, uint32_t samples);
// A pass starts: the EFB's planes the last copy cleared are cleared first.
void RecordPass(FrameRecord* r, bool clearColor, const uint8_t color[4], bool clearDepth, uint32_t clearZ);
// Copies `size` bytes into vertex memory the record keeps, for a draw; nil if
// it's too big. `chunk` is for RecordedDraw.
id<MTLBuffer> RecordUpload(FrameRecord* r, const void* data, NSUInteger size, NSUInteger* offset, uint32_t* chunk);
void RecordDraw(FrameRecord* r, const RecordedDraw& draw, const FragUniforms& frag);
// A frame copy: the EFB's rectangle (in its pixels) that becomes the frame.
void RecordCopy(FrameRecord* r, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
void FinishRecord(FrameRecord* r);

void RetainRecord(FrameRecord* r);
void ReleaseRecord(FrameRecord* r);

uint32_t RecordDraws(const FrameRecord* r);

// The camera the frame was drawn with and the one before it (frame_motion.h),
// set as the frame ends.
void SetRecordViews(FrameRecord* r, const FrameView& now, const FrameView& prev);
void RecordViews(const FrameRecord* r, FrameView* now, FrameView* prev);

// What a replay draws into: an EFB's color and depth, and the frame copied
// out of it; with MSAA, the color's samples too, which resolve into `color`.
// One set for each thread that replays.
struct ReplayTargets {
    id<MTLTexture> color, depth, frame, msaa;
};

// Encodes a finished record into `commands`, moved by `motion` (null: as
// recorded), and returns the frame, the size of the record's copy. With
// `mipmapped` (renders above the original resolution) the frame has
// mipmaps, made when `makeMipmaps` (the display shows it smaller). The record
// is kept until `commands` completes.
id<MTLTexture> ReplayFrame(id<MTLCommandBuffer> commands, FrameRecord* r, const ReplayMotion* motion,
                           ReplayTargets& targets, bool mipmapped, bool makeMipmaps);

}  // namespace frontline::gx
