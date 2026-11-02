// The Modern renderer's vertex stage (mesh_shader.cpp): what it reads, laid
// out as the shader declares it.
#pragma once

#include <cstdint>

namespace frontline::mesh {

extern const char kMeshShaderSource[];

// A mesh vertex, as decoded from the display list (gx_vertex.h
// DecodedVertex): 40 bytes.
struct MeshVertex {
    float pos[3];
    uint8_t mtx;  // the position matrix's row (PNMTXIDX), unless the mesh uses the current one
    uint8_t pad[3];
    float nrm[3];
    uint8_t color[4];  // color 0
    float st[2];       // texture coordinate 0
};
static_assert(sizeof(MeshVertex) == 40);

// A GX light (gx_state.h Light), in float4s.
struct MeshLight {
    float color[4];
    float pos[4];
    float dir[4];
    float a[4];
    float k[4];
};

enum : uint32_t {
    kMeshHasNormals = 1,    // else the normal is (0, 0, 1) in eye space, as GX's
    kMeshCurrentMtx = 2,    // every vertex uses `currentMtx`
    kMeshTint = 4,          // FRONTLINE_NATIVE_DEBUG=native: tinted, to see what's drawn natively
    kMeshNormalsFromPosition = 8,  // corrected lighting: normals by the position matrix's inverse transpose
};

// What a draw needs from the GX state besides the TEV's (FragUniforms): the
// projection, lighting and texture coordinate generation (up to three). 384
// bytes.
struct MeshUniforms {
    float proj[8];          // type, then the 6 terms
    uint32_t chan[4][4];    // color 0, color 1, alpha 0, alpha 1: lighting | ambSrc << 1 | matSrc << 2, mask, diffFn, attnFn
    float amb[2][4];        // register colors, 0..1
    float mat[2][4];
    uint32_t texGen[3][4];  // func, src, matrix (60: none), normalize
    float texMtx[3][3][4];  // the texture matrices' rows
    uint32_t numChans, numTexGens, flags, currentMtx;
    uint32_t mtxBase;   // the first position matrix row in the matrix buffer
    uint32_t nrmBase;   // the first normal matrix float there ((row & 31) * 3)
    uint32_t nrmStart;  // where the normal matrices start in it, in floats
    uint32_t pad;
};
static_assert(sizeof(MeshUniforms) == 384);

// Buffers: 0 the mesh's vertices, 1 MeshUniforms, 2 the lights (8 MeshLights),
// 3 the matrices (position rows of 4 floats from mtxBase, then normal
// matrices' floats from nrmBase), 4 the 256 colors of a byte (i / 255).
enum : uint32_t {
    kBufferVertices,
    kBufferUniforms,
    kBufferLights,
    kBufferMatrices,
    kBufferUnorm,
};

}  // namespace frontline::mesh
