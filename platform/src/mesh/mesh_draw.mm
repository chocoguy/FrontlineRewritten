// Drawing the Modern renderer's meshes (mesh.h): a display list a render
// bin calls is drawn from its mesh (mesh_cache.h) by the native vertex stage
// (mesh_shader.cpp) and GX's own TEV fragment stage, into the EFB pass GX
// draws in (gx_efb.h), with the GX state of the moment as its material.

#import <Metal/Metal.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// The game's headers define BOOL as int; Objective-C's is bool.
#define BOOL FRONTLINE_GAME_BOOL
#include "gx/gx_efb.h"
#include "gx/gx_metal.h"
#include "gx/gx_state.h"
#include "gx/gx_vertex.h"
#include "mesh.h"
#include "mesh_cache.h"
#include "mesh_shader.h"
#include "native/hooks.h"
#include "native/render.h"
#include "os/os_internal.h"
#undef BOOL

namespace frontline::mesh {
namespace {

using gx::g_state;

// The native vertex stages, for gx_efb.h: lit per vertex (mesh_vertex), or
// handing on what the TEV shader's lit fragment stage lights per pixel from
// (mesh_vertex_lit).
constexpr uint32_t kVariantMesh = 1;
constexpr uint32_t kVariantLit = 2;

// Settings for the run, read at the first call.
bool g_configured;
bool g_bins[NATIVE_BIN_COUNT];  // FRONTLINE_NATIVE_BINS: which bins draw natively
bool g_meshCheck;               // FRONTLINE_NATIVE_MESHCHECK
bool g_tint;                    // FRONTLINE_NATIVE_DEBUG=native
bool g_testLight;               // FRONTLINE_NATIVE_DEBUG=pointlight: a red light at the camera (corrected)
bool g_perPixel;                // lit draws are lit per pixel (FRONTLINE_NATIVE_LIGHTING=vertex: per vertex, as GX)
bool g_alternate;               // FRONTLINE_NATIVE_LIGHTING=alternate: per vertex and per pixel by turns, 5 s each
bool g_corrected;               // corrected lighting (NativeCorrectedLighting), with per-pixel lighting
bool g_reuse;                   // reuse what the last draw took from the GX state (FRONTLINE_NATIVE_REUSE=off: never)
bool g_reuseAlternate;          // FRONTLINE_NATIVE_REUSE=alternate: off and on by turns, 5 s each

// The scene's point lights this frame (NativeSceneLights), for corrected lighting.
float g_points[NATIVE_MAX_POINT_LIGHTS][2][4];
uint32_t g_numPoints;
uint32_t g_pointsSerial;  // changes with each NativeSceneLights

void Configure() {
    g_configured = true;
    if (NativeModernRender() == 0) {
        return;
    }
    // Every bin by default. FRONTLINE_NATIVE_BINS=world,prop,character,viewmodel (or all) for some.
    const char* bins = std::getenv("FRONTLINE_NATIVE_BINS");
    if (bins == nullptr) {
        bins = "all";
    }
    static const char* kNames[NATIVE_BIN_COUNT] = {"", "world", "prop", "character", "viewmodel"};
    for (int b = NATIVE_BIN_WORLD; b < NATIVE_BIN_COUNT; b++) {
        const char* p = std::strstr(bins, kNames[b]);
        g_bins[b] = p != nullptr || std::strcmp(bins, "all") == 0;
    }
    g_meshCheck = std::getenv("FRONTLINE_NATIVE_MESHCHECK") != nullptr;
    const char* debug = std::getenv("FRONTLINE_NATIVE_DEBUG");
    g_tint = debug != nullptr && std::strcmp(debug, "native") == 0;
    g_testLight = debug != nullptr && std::strcmp(debug, "pointlight") == 0;
    const char* lighting = std::getenv("FRONTLINE_NATIVE_LIGHTING");
    g_perPixel = lighting == nullptr || std::strcmp(lighting, "vertex") != 0;
    g_alternate = lighting != nullptr && std::strcmp(lighting, "alternate") == 0;
    g_corrected = NativeCorrectedLighting() != 0;
    const char* reuse = std::getenv("FRONTLINE_NATIVE_REUSE");
    g_reuse = reuse == nullptr || std::strcmp(reuse, "off") != 0;
    g_reuseAlternate = reuse != nullptr && std::strcmp(reuse, "alternate") == 0;
}

// The vertex stage's library, compiled in the background from the first
// call; GX draws until it's ready.
id<MTLLibrary> g_library;
id<MTLBuffer> g_unorm;  // i / 255 for each byte, as GX's vertex stage reads colors
dispatch_group_t g_libraryCompile;
std::atomic<bool> g_libraryDone;
dispatch_once_t g_libraryOnce;

void StartLibrary() {
    dispatch_once(&g_libraryOnce, ^{
        id<MTLDevice> device = gx::Device();
        float unorm[256];
        for (int i = 0; i < 256; i++) {
            unorm[i] = i / 255.0f;
        }
        g_unorm = [device newBufferWithBytes:unorm length:sizeof(unorm) options:MTLResourceStorageModeShared];
        g_libraryCompile = dispatch_group_create();
        dispatch_group_async(g_libraryCompile, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            const uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            MTLCompileOptions* options = [MTLCompileOptions new];
            options.mathMode = MTLMathModeSafe;  // as GX's vertex stage computes, but for rounding
            NSError* error = nil;
            g_library = [device newLibraryWithSource:[NSString stringWithUTF8String:kMeshShaderSource]
                                             options:options
                                               error:&error];
            if (g_library == nil) {
                std::fprintf(stderr, "frontline: mesh shader: %s\n", error.localizedDescription.UTF8String);
            }
            if (std::getenv("FRONTLINE_PERF") != nullptr) {
                std::fprintf(stderr, "[perf] mesh shader library compiled in %.0f ms\n",
                             (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start) * 1e-6);
            }
            g_libraryDone.store(true, std::memory_order_release);
        });
        gx::EfbVertexFunctions(^id<MTLFunction>(uint32_t variant, uint32_t numTexGens) {
            dispatch_group_wait(g_libraryCompile, DISPATCH_TIME_FOREVER);
            MTLFunctionConstantValues* values = [MTLFunctionConstantValues new];
            [values setConstantValue:&numTexGens type:MTLDataTypeUInt atIndex:3];
            NSError* error = nil;
            id<MTLFunction> vertex =
                [g_library newFunctionWithName:variant == kVariantLit ? @"mesh_vertex_lit" : @"mesh_vertex"
                                constantValues:values
                                         error:&error];
            if (vertex == nil) {
                std::fprintf(stderr, "frontline: mesh vertex function: %s\n", error.localizedDescription.UTF8String);
            }
            return vertex;
        });
    });
}

// Whether the library is ready; with pipelines compiled on the spot, waits
// for it (delivering interrupts meanwhile, as GX's Init does).
bool LibraryReady() {
    StartLibrary();
    if (!g_libraryDone.load(std::memory_order_acquire) && gx::EfbSyncShaders()) {
        while (dispatch_group_wait(g_libraryCompile, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_MSEC)) != 0) {
            os::SafePoint();
        }
    }
    return g_libraryDone.load(std::memory_order_acquire) && g_library != nil;
}

// The GX state a mesh can be drawn with: one or two colour channels, up to
// three texture coordinates from the position, normal or coordinate 0 (or
// color 0's red and green), and any TEV setup (GX's own fragment stage).
bool Drawable(const gx::State& s) {
    if (s.cullMode == 3 || s.numChans > 2 || s.numTexGens > 3) {
        return false;
    }
    for (uint32_t t = 0; t < s.numTexGens; t++) {
        const gx::TexGen& g = s.texGen[t];
        if (g.func > 10 || (g.func != 10 && g.src != 0 && g.src != 1 && g.src != 4)) {
            return false;
        }
        if (g.func <= 1 && g.mtx != 60 && (g.mtx & 63) * 4 + 12 > 256) {
            return false;
        }
    }
    return true;
}

// What a draw took from the GX state, kept for the next one: each side is
// taken again only when GX changed that side (g_pixelGeneration,
// g_vertexGeneration) or the draw needs it otherwise. A take's `serial`
// changes each time, so Bound can tell that it's bound already.
uint32_t g_serial;

struct PixelTaken {
    bool valid;
    uint32_t generation, serial;
    gx::FragUniforms frag;
    id<MTLTexture> textures[8];
    id<MTLSamplerState> samplers[8];
    id<MTLDepthStencilState> depth;
    MTLCullMode cull;
    // The pipeline for some texture coordinates and lighting per pixel
    // (pipelineKey); nil until there's one.
    uint32_t pipelineKey;
    id<MTLRenderPipelineState> pipeline;
} g_pixel;

struct VertexTaken {
    bool valid;
    uint32_t generation, serial;
    // What else it was taken for.
    uint32_t flags;
    MatrixRows rows;
    int bin;
    bool lit, perPixel;
    uint32_t points;  // g_pointsSerial
    MeshUniforms u;
    uint32_t matrixBytes;
    float matrices[256 + 96];
    MeshLight lights[8];
    gx::LitUniforms litUniforms;  // per pixel
} g_vertex;

void TakePixel(PixelTaken& px) {
    const gx::State& s = g_state;
    for (int m = 0; m < 8; m++) {
        px.textures[m] = nil;
        px.samplers[m] = nil;
    }
    gx::EfbFragUniforms(&px.frag, px.textures, px.samplers);
    px.depth = gx::EfbDepthState();
    px.cull = s.cullMode == 1 ? MTLCullModeFront : s.cullMode == 2 ? MTLCullModeBack : MTLCullModeNone;
    px.pipeline = nil;
    px.generation = gx::g_pixelGeneration;
    px.serial = ++g_serial;
    px.valid = true;
}

void TakeVertex(VertexTaken& vx, uint32_t flags, const MatrixRows& rows, bool lit, bool perPixel) {
    const gx::State& s = g_state;
    MeshUniforms& u = vx.u;
    std::memset(&u, 0, sizeof(u));
    std::memcpy(u.proj, s.projection, sizeof(s.projection));
    for (int c = 0; c < 4; c++) {
        const gx::ChanCtrl& ch = s.chan[c];
        u.chan[c][0] = (ch.lighting ? 1u : 0u) | (ch.ambSrc != 0 ? 2u : 0u) | (ch.matSrc != 0 ? 4u : 0u);
        u.chan[c][1] = ch.lightMask;
        u.chan[c][2] = ch.diffFn;
        u.chan[c][3] = ch.attnFn;
    }
    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < 4; i++) {
            u.amb[c][i] = s.ambColor[c][i] / 255.0f;
            u.mat[c][i] = s.matColor[c][i] / 255.0f;
        }
    }
    for (uint32_t t = 0; t < s.numTexGens; t++) {
        const gx::TexGen& g = s.texGen[t];
        u.texGen[t][0] = g.func;
        u.texGen[t][1] = g.src;
        u.texGen[t][2] = g.func <= 1 ? g.mtx : 60;
        u.texGen[t][3] = g.normalize ? 1 : 0;
        if (g.func <= 1 && g.mtx != 60) {
            std::memcpy(u.texMtx[t], &s.xfMtx[(g.mtx & 63) * 4], sizeof(u.texMtx[t]));
        }
    }
    u.numChans = s.numChans;
    u.numTexGens = s.numTexGens;
    u.flags = flags;
    u.currentMtx = s.currentPosMtx;
    u.mtxBase = rows.lo;
    u.nrmBase = rows.nrmLo;
    const uint32_t numRows = rows.hi + 3 - rows.lo, numNrm = rows.nrmHi + 9 - rows.nrmLo;
    u.nrmStart = numRows * 4;
    std::memcpy(vx.matrices, &s.xfMtx[rows.lo * 4], numRows * 16);
    std::memcpy(vx.matrices + numRows * 4, &s.xfNrm[rows.nrmLo], numNrm * 4);
    vx.matrixBytes = (numRows * 4 + numNrm) * 4;
    MeshLight* lights = vx.lights;
    std::memset(vx.lights, 0, sizeof(vx.lights));
    if (lit) {
        for (int l = 0; l < 8; l++) {
            const gx::Light& light = s.lights[l];
            std::memcpy(lights[l].color, light.color, sizeof(light.color));
            std::memcpy(lights[l].pos, light.pos, sizeof(light.pos));
            std::memcpy(lights[l].dir, light.dir, sizeof(light.dir));
            std::memcpy(lights[l].a, light.a, sizeof(light.a));
            std::memcpy(lights[l].k, light.k, sizeof(light.k));
        }
    }
    // Per pixel: the same channels and lights, for the fragment stage.
    if (perPixel) {
        static_assert(sizeof(MeshLight) == sizeof(gx::LitLight));
        gx::LitUniforms& litUniforms = vx.litUniforms;
        std::memset(&litUniforms, 0, sizeof(litUniforms));
        std::memcpy(litUniforms.chan, u.chan, sizeof(u.chan));
        std::memcpy(litUniforms.amb, u.amb, sizeof(u.amb));
        std::memcpy(litUniforms.mat, u.mat, sizeof(u.mat));
        litUniforms.numChans = u.numChans;
        std::memcpy(litUniforms.lights, lights, sizeof(vx.lights));
        // Corrected lighting, for what the original lights with directions
        // loaded as positions: characters and the weapon (their directions
        // are in eye space already) and props (in model space: turned into
        // eye space here, and their normals by the model-view too, where the
        // original leaves them in model space). The world's lights are points
        // already, and stay GX's.
        if (g_corrected && g_bin != NATIVE_BIN_WORLD) {
            litUniforms.mode = 1;
            litUniforms.numPoints = g_numPoints;
            std::memcpy(litUniforms.points, g_points, sizeof(g_points[0]) * g_numPoints);
            if (g_testLight && litUniforms.numPoints < NATIVE_MAX_POINT_LIGHTS) {
                static const float kTest[2][4] = {{0.0f, 0.0f, 0.0f, 1000.0f}, {1.0f, 0.0f, 0.0f, 0.0f}};
                std::memcpy(litUniforms.points[litUniforms.numPoints++], kTest, sizeof(kTest));
            }
            if (g_bin == NATIVE_BIN_PROP) {
                const float* m = &s.xfMtx[(rows.lo & 63) * 4];
                for (int l = 0; l < 8; l++) {
                    const float* v = lights[l].pos;
                    float* out = litUniforms.lights[l].pos;
                    for (int r = 0; r < 3; r++) {
                        out[r] = m[r * 4] * v[0] + m[r * 4 + 1] * v[1] + m[r * 4 + 2] * v[2];
                    }
                }
                u.flags |= kMeshNormalsFromPosition;
            }
        }
    }
    vx.flags = flags;
    vx.rows = rows;
    vx.bin = g_bin;
    vx.lit = lit;
    vx.perPixel = perPixel;
    vx.points = g_pointsSerial;
    vx.generation = gx::g_vertexGeneration;
    vx.serial = ++g_serial;
    vx.valid = true;
}

// What's bound in the encoder the meshes draw into, while nothing else
// bound anything there (EfbPass stamp).
struct Bound {
    bool valid;
    uint32_t stamp;
    uint32_t pixelSerial, vertexSerial;  // the takes bound (their uniforms compared)
    __unsafe_unretained id<MTLRenderCommandEncoder> encoder;
    __unsafe_unretained id<MTLRenderPipelineState> pipeline;
    __unsafe_unretained id<MTLDepthStencilState> depth;
    __unsafe_unretained id<MTLTexture> textures[8];
    __unsafe_unretained id<MTLSamplerState> samplers[8];
    __unsafe_unretained id<MTLBuffer> vertices;
    MTLCullMode cull;
    bool viewportSet, scissorSet, fragSet, uniformsSet, lightsSet, litSet;
    MTLViewport viewport;
    MTLScissorRect scissor;
    gx::FragUniforms frag;
    MeshUniforms uniforms;
    MeshLight lights[8];
    gx::LitUniforms lit;
    uint32_t matrixBytes;
    float matrices[256 + 96];
} g_bound;

}  // namespace

bool Call(const uint8_t* list, uint32_t size) {
    if (!g_configured) {
        Configure();
    }
    if (!g_bins[g_bin]) {
        return false;
    }
    gx::NativeCounts* counts = gx::EfbNativeCounts();
    const gx::State& s = g_state;
    if (!Drawable(s)) {
        if (counts != nullptr) {
            counts->fallbacks[0]++;
        }
        return false;
    }
    if (!LibraryReady()) {
        if (counts != nullptr) {
            counts->fallbacks[1]++;
        }
        return false;
    }
    const bool verify = gx::EfbNativeVerify();
    // Lists with their vertex data in them (morph meshes) are decoded for
    // this draw alone; the rest are kept.
    Mesh transient;
    id<MTLBuffer> transientBuffer = nil;
    const Mesh* mesh = nullptr;
    if (DirectData()) {
        gx::EfbOpen();
        if (TransientMesh(list, size, &transient, &transientBuffer)) {
            mesh = &transient;
        } else if (counts != nullptr) {
            counts->fallbacks[0]++;
        }
        if (mesh == nullptr) {
            return false;
        }
    } else {
        bool overBudget;
        mesh = FindMesh(list, size, verify || gx::EfbSyncShaders(), &overBudget);
        if (mesh == nullptr) {
            if (counts != nullptr) {
                counts->fallbacks[overBudget ? 2 : 0]++;
            }
            return false;
        }
        if (g_meshCheck) {
            CheckMesh(*mesh, list);
        }
    }

    // The matrices the vertices use.
    MatrixRows rows = mesh->rows;
    if (mesh->flags & kMeshCurrentMtx) {
        rows.lo = rows.hi = s.currentPosMtx & 63;
        rows.nrmLo = rows.nrmHi = (s.currentPosMtx & 31) * 3;
    }
    if (rows.hi + 3 > 64 || rows.nrmHi + 9 > 96) {
        if (counts != nullptr) {
            counts->fallbacks[0]++;
        }
        return false;
    }

    gx::EfbPass pass = gx::EfbOpen();
    MTLViewport viewport;
    MTLScissorRect scissor;
    if (!gx::EfbRect(&viewport, &scissor)) {
        return true;  // nothing would be drawn
    }
    // Lit draws are lit per pixel (unlit ones would come out the same, so
    // they stay per vertex), unless a coordinate comes from the lit color.
    bool lit = false, srtg = false;
    for (int c = 0; c < 4; c++) {
        lit |= s.chan[c].lighting && (uint32_t)(c & 1) < s.numChans;
    }
    for (uint32_t t = 0; t < s.numTexGens; t++) {
        srtg |= s.texGen[t].func == 10;
    }
    if (g_alternate) {
        // To compare their cost on the same picture ("[native]" lines say which).
        const bool pixel = (gx::EfbFrame() / 300) % 2 == 1;
        if (pixel != g_perPixel) {
            g_perPixel = pixel;
            std::fprintf(stderr, "[native] lighting per %s from frame %u\n", pixel ? "pixel" : "vertex", gx::EfbFrame());
        }
    }
    const bool perPixel = g_perPixel && lit && !srtg && !g_tint;
    if (g_reuseAlternate) {
        // To measure what reusing saves on the same picture.
        const bool reuse = (gx::EfbFrame() / 300) % 2 == 1;
        if (reuse != g_reuse) {
            g_reuse = reuse;
            std::fprintf(stderr, "[native] reuse %s from frame %u\n", reuse ? "on" : "off", gx::EfbFrame());
        }
    }

    PixelTaken& px = g_pixel;
    if (!g_reuse) {
        px.valid = g_vertex.valid = false;
    }
    if (!px.valid || px.generation != gx::g_pixelGeneration) {
        TakePixel(px);
    }
    const uint32_t pipelineKey = s.numTexGens | (perPixel ? 1u << 8 : 0u);
    if (px.pipeline == nil || px.pipelineKey != pipelineKey) {
        px.pipeline = gx::EfbPipeline(px.frag, s.numTexGens, perPixel ? kVariantLit : kVariantMesh, perPixel);
        px.pipelineKey = pipelineKey;
        if (px.pipeline == nil) {
            if (counts != nullptr) {
                counts->fallbacks[1]++;
            }
            return false;
        }
    }
    VertexTaken& vx = g_vertex;
    const uint32_t flags = mesh->flags | (g_tint ? kMeshTint : 0);
    if (!vx.valid || vx.generation != gx::g_vertexGeneration || vx.flags != flags ||
        std::memcmp(&vx.rows, &rows, sizeof(rows)) != 0 || vx.bin != g_bin || vx.lit != lit ||
        vx.perPixel != perPixel || vx.points != g_pointsSerial) {
        TakeVertex(vx, flags, rows, lit, perPixel);
    }
    const gx::FragUniforms& frag = px.frag;
    id<MTLRenderPipelineState> pipeline = px.pipeline;
    const MeshUniforms& u = vx.u;

    // Into the EFB, or for FRONTLINE_NATIVE_VERIFY the second EFB (GX draws the EFB's).
    id<MTLRenderCommandEncoder> e = verify ? pass.verify : pass.encoder;
    if (e == nil) {
        return false;
    }
    Bound& b = g_bound;
    if (!b.valid || b.stamp != pass.stamp || b.encoder != e) {
        std::memset(&b, 0, sizeof(b));
        b.valid = true;
        b.stamp = pass.stamp;
        b.encoder = e;
        b.cull = (MTLCullMode)~0u;
        [e setVertexBuffer:g_unorm offset:0 atIndex:kBufferUnorm];
    }
    if (b.pipeline != pipeline) {
        [e setRenderPipelineState:pipeline];
        b.pipeline = pipeline;
    }
    if (b.depth != px.depth) {
        [e setDepthStencilState:px.depth];
        b.depth = px.depth;
    }
    if (b.cull != px.cull) {
        [e setCullMode:px.cull];
        b.cull = px.cull;
    }
    if (!b.viewportSet || std::memcmp(&b.viewport, &viewport, sizeof(viewport)) != 0) {
        [e setViewport:viewport];
        b.viewport = viewport;
        b.viewportSet = true;
    }
    if (!b.scissorSet || std::memcmp(&b.scissor, &scissor, sizeof(scissor)) != 0) {
        [e setScissorRect:scissor];
        b.scissor = scissor;
        b.scissorSet = true;
    }
    if (b.pixelSerial != px.serial) {
        for (uint32_t m = 0; m < 8; m++) {
            if (!(frag.texValid & (1u << m))) {
                continue;
            }
            if (b.textures[m] != px.textures[m]) {
                [e setFragmentTexture:px.textures[m] atIndex:m];
                b.textures[m] = px.textures[m];
            }
            if (b.samplers[m] != px.samplers[m]) {
                [e setFragmentSamplerState:px.samplers[m] atIndex:m];
                b.samplers[m] = px.samplers[m];
            }
        }
        if (!b.fragSet || std::memcmp(&b.frag, &frag, sizeof(frag)) != 0) {
            [e setFragmentBytes:&frag length:sizeof(frag) atIndex:0];
            b.frag = frag;
            b.fragSet = true;
        }
        b.pixelSerial = px.serial;
    }
    if (b.vertexSerial != vx.serial) {
        if (!b.uniformsSet || std::memcmp(&b.uniforms, &u, sizeof(u)) != 0) {
            [e setVertexBytes:&u length:sizeof(u) atIndex:kBufferUniforms];
            b.uniforms = u;
            b.uniformsSet = true;
        }
        if (b.matrixBytes != vx.matrixBytes || std::memcmp(b.matrices, vx.matrices, vx.matrixBytes) != 0) {
            [e setVertexBytes:vx.matrices length:vx.matrixBytes atIndex:kBufferMatrices];
            b.matrixBytes = vx.matrixBytes;
            std::memcpy(b.matrices, vx.matrices, vx.matrixBytes);
        }
        if (!b.lightsSet || std::memcmp(b.lights, vx.lights, sizeof(vx.lights)) != 0) {
            [e setVertexBytes:vx.lights length:sizeof(vx.lights) atIndex:kBufferLights];
            std::memcpy(b.lights, vx.lights, sizeof(vx.lights));
            b.lightsSet = true;
        }
        if (perPixel && (!b.litSet || std::memcmp(&b.lit, &vx.litUniforms, sizeof(vx.litUniforms)) != 0)) {
            [e setFragmentBytes:&vx.litUniforms length:sizeof(vx.litUniforms) atIndex:1];
            b.lit = vx.litUniforms;
            b.litSet = true;
        }
        b.vertexSerial = vx.serial;
    }
    id<MTLBuffer> buffer = transientBuffer != nil ? transientBuffer : MeshBuffer(*mesh);
    if (b.vertices != buffer) {
        [e setVertexBuffer:buffer offset:mesh->vertexOffset atIndex:kBufferVertices];
        b.vertices = buffer;
    } else {
        [e setVertexBufferOffset:mesh->vertexOffset atIndex:kBufferVertices];
    }
    [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                  indexCount:mesh->numIndices
                   indexType:mesh->index32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                 indexBuffer:buffer
           indexBufferOffset:mesh->indexOffset];
    if (counts != nullptr) {
        counts->draws++;
        counts->triangles += mesh->numIndices / 3;
    }
    if (!verify && pass.verify != nil) {
        // FRONTLINE_GX_VERIFY: the second EFB gets every draw, this one alike.
        id<MTLRenderCommandEncoder> v = pass.verify;
        [v setRenderPipelineState:pipeline];
        [v setDepthStencilState:px.depth];
        [v setCullMode:px.cull];
        [v setViewport:viewport];
        [v setScissorRect:scissor];
        for (uint32_t m = 0; m < 8; m++) {
            if (frag.texValid & (1u << m)) {
                [v setFragmentTexture:px.textures[m] atIndex:m];
                [v setFragmentSamplerState:px.samplers[m] atIndex:m];
            }
        }
        [v setFragmentBytes:&frag length:sizeof(frag) atIndex:0];
        [v setVertexBytes:&u length:sizeof(u) atIndex:kBufferUniforms];
        [v setVertexBytes:vx.matrices length:vx.matrixBytes atIndex:kBufferMatrices];
        [v setVertexBytes:vx.lights length:sizeof(vx.lights) atIndex:kBufferLights];
        if (perPixel) {
            [v setFragmentBytes:&vx.litUniforms length:sizeof(vx.litUniforms) atIndex:1];
        }
        [v setVertexBuffer:g_unorm offset:0 atIndex:kBufferUnorm];
        [v setVertexBuffer:buffer offset:mesh->vertexOffset atIndex:kBufferVertices];
        [v drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                      indexCount:mesh->numIndices
                       indexType:mesh->index32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                     indexBuffer:buffer
               indexBufferOffset:mesh->indexOffset];
    }

    if (verify) {
        // GX draws the same list into the EFB alone, to compare.
        gx::EfbNativeDrew();
        gx::EfbMainOnly(true);
        gx::RunDisplayList(list, size);
        gx::EfbMainOnly(false);
    } else {
        gx::EfbForeign();
    }
    return true;
}

}  // namespace frontline::mesh

extern "C" void NativeSceneLights(const NativePointLight* lights, int count) {
    using namespace frontline::mesh;
    g_numPoints = 0;
    g_pointsSerial++;
    for (int i = 0; i < count && i < NATIVE_MAX_POINT_LIGHTS; i++) {
        const NativePointLight& l = lights[i];
        float(&p)[2][4] = g_points[g_numPoints++];
        p[0][0] = l.pos[0], p[0][1] = l.pos[1], p[0][2] = l.pos[2], p[0][3] = l.radius;
        p[1][0] = l.color[0], p[1][1] = l.color[1], p[1][2] = l.color[2], p[1][3] = 0.0f;
    }
    if (frontline::gx::NativeCounts* counts = frontline::gx::EfbNativeCounts()) {
        counts->pointLights = std::max<uint64_t>(counts->pointLights, g_numPoints);
    }
}
