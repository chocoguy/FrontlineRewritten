// What the GX backend (gx_metal.mm) shares with the Modern renderer
// (platform/src/mesh/), which draws into the same EFB pass between GX's own
// draws: the open pass, the EFB's format, and the GX state of the moment as
// Metal state. Everything here reads g_state; the game thread only.
#pragma once

#ifdef __OBJC__
#import <Metal/Metal.h>

#include <cstdint>

#include "gx_shader.h"

namespace frontline::gx {

// The EFB pass, begun if it isn't (with the clears a frame copy left
// pending). `verify` is the second EFB's encoder (FRONTLINE_GX_VERIFY,
// FRONTLINE_NATIVE_VERIFY), or nil. `stamp` changes whenever GX binds
// anything in them (a new pass, a GX draw): while it stays the same, what a
// caller bound is still bound.
struct EfbPass {
    __unsafe_unretained id<MTLRenderCommandEncoder> encoder;
    __unsafe_unretained id<MTLRenderCommandEncoder> verify;
    uint32_t stamp;
};
EfbPass EfbOpen();

// FRONTLINE_NATIVE_VERIFY: the Modern renderer draws into the second EFB
// only, and GX draws the same lists into the EFB alone (EfbMainOnly(true)
// around them).
bool EfbNativeVerify();
void EfbMainOnly(bool on);
void EfbNativeDrew();  // so the frame copy compares the two

// Whether pipelines compile on the spot (FRONTLINE_GX_SYNC_SHADERS, and both
// verify checks), so nothing waits for one.
bool EfbSyncShaders();

// GXDrawDone calls so far: the frame.
uint32_t EfbFrame();

// After drawing into the pass with other bindings: GX binds everything again
// at its next draw.
void EfbForeign();

// Copies `size` bytes into the frame's vertex memory, as GX's own draws'
// (with the pass open, EfbOpen); returns the buffer and the offset (a
// multiple of 256), or nil if it doesn't fit.
id<MTLBuffer> EfbUpload(const void* data, uint32_t size, uint32_t* offset);

// The EFB's samples a pixel (1, or 4 with MSAA). Its color is RGBA8Unorm and
// its depth Depth32Float.
uint32_t EfbSamples();

// The viewport and scissor GX draws with now, in the EFB's pixels; false if
// the scissor is empty (nothing would be drawn).
bool EfbRect(MTLViewport* viewport, MTLScissorRect* scissor);

// The depth state, the blend state as a pipeline key, and a pipeline
// descriptor for that key (blend, write mask, samples and alpha to coverage
// as GX's own pipelines have them), without its functions.
id<MTLDepthStencilState> EfbDepthState();
uint32_t EfbBlendKey();
MTLRenderPipelineDescriptor* EfbPipelineDescriptor(uint32_t key);

// The TEV shader's uniforms for the TEV setup now, with the textures and
// samplers it reads (index m valid if u->texValid has bit m).
void EfbFragUniforms(FragUniforms* u, id<MTLTexture> __strong textures[8], id<MTLSamplerState> __strong samplers[8]);

// Native vertex stages for GX's TEV shader: `make` gives the vertex function
// for a variant (1 and up) and a number of generated texture coordinates. It
// must write the TEV shader's vertex output (gx_shader.cpp VOut) with just
// those coordinates, and may be called on any thread.
void EfbVertexFunctions(id<MTLFunction> (^make)(uint32_t variant, uint32_t numTexGens));

// The specialized pipeline for TEV uniforms `u` (EfbFragUniforms) with
// `numTexGens` coordinates and native vertex stage `variant`, with the blend
// state now; nil while it compiles (on another thread, as GX's own do).
// `lit`: the fragment stage lights per pixel (gx_shader.cpp
// tev_fragment_lit: LitUniforms at fragment buffer 1), and the vertex stage
// writes its VOutLit.
id<MTLRenderPipelineState> EfbPipeline(const FragUniforms& u, uint32_t numTexGens, uint32_t variant, bool lit);

// FRONTLINE_PERF's counts of the Modern renderer's work, or null with
// FRONTLINE_PERF off. `bytes` is a total; the rest count this second.
struct NativeCounts {
    uint64_t draws, triangles;
    uint64_t fallbacks[3];  // the GX state unsupported, a pipeline compiling, past the frame's budget for making meshes
    uint64_t conversions, convertNanos;
    uint64_t transients;  // lists with their vertex data in them (morph meshes), decoded at each draw
    uint64_t pointLights; // the most the scene had this second (corrected lighting)
    uint64_t bytes;       // of meshes
};
NativeCounts* EfbNativeCounts();

}  // namespace frontline::gx
#endif
