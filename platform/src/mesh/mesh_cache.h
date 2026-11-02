// The Modern renderer's meshes: display lists decoded once into vertices and
// triangles in GPU memory, by the list's address. An entry is good while the
// list is the same size, its first bytes are the same, the vertex format is
// the one it was decoded with, and the arrays it reads without setting
// itself are where they were. Compartments streaming into a block another
// one used forget theirs (NativeMeshForget); rebooting forgets all of them.
#pragma once

#include <cstdint>

#ifdef __OBJC__
#import <Metal/Metal.h>
#endif

namespace frontline::mesh {

// The position matrix rows a mesh's vertices use, when they say (PNMTXIDX).
struct MatrixRows {
    uint32_t lo, hi;        // rows (& 63) from lo to hi, each a matrix's first
    uint32_t nrmLo, nrmHi;  // normal matrices' first floats ((row & 31) * 3)
};

struct Mesh {
    uint32_t list;  // the list's address (0: an empty entry)
    uint32_t size;
    uint64_t format;  // FormatKey when decoded
    uint64_t guard;   // a hash of the list's first bytes
    bool gxOnly;      // it can't be drawn natively (GX draws it)
    bool stale;       // forgotten: make it again

    // Array bases and strides the list reads without setting them itself
    // (bit per attribute, gx_state.h Attr), and what they were.
    uint32_t inheritedBases, inheritedStrides;
    uint32_t inheritedBase[4], inheritedStride[4];  // POS, NRM, CLR0, TEX0

    // The list's array writes, made again when it's drawn from here.
    uint8_t numWrites;
    uint8_t writeReg[16];
    uint32_t writeValue[16];

    // The mesh in GPU memory.
    uint32_t chunk;
    uint32_t vertexOffset, indexOffset;  // bytes
    uint32_t numVertices, numIndices;
    bool index32;
    uint32_t flags;  // kMeshHasNormals, kMeshCurrentMtx
    MatrixRows rows;  // (unless kMeshCurrentMtx)
};

// The mesh for the display list at `list`, made now if it has to be (within
// the frame's budget unless `unlimited`), with the list's array writes made
// as running it would. Null if GX must draw it: not drawable natively, or
// over budget (`*overBudget`). Its array writes are made either way.
Mesh* FindMesh(const uint8_t* list, uint32_t size, bool unlimited, bool* overBudget);

// FRONTLINE_NATIVE_MESHCHECK: decodes the list again and says if the mesh
// differs (its data changed without the cache knowing).
void CheckMesh(const Mesh& mesh, const uint8_t* list);

// Whether the vertex format puts vertex data in the list itself (direct
// attributes): such lists (morph meshes, written anew each frame) aren't
// kept, but drawn with TransientMesh.
bool DirectData();

#ifdef __OBJC__
// The list decoded for this draw only, into the frame's vertex memory
// (gx_efb.h EfbUpload, with the pass open): `mesh` describes it and
// `buffer` holds it. Its array writes are made either way. False if GX must
// draw it.
bool TransientMesh(const uint8_t* list, uint32_t size, Mesh* mesh, id<MTLBuffer> __strong* buffer);
#endif

// Forgets the mesh made from the list at `list`.
void ForgetMesh(uint32_t list);

// Forgets every mesh (a reboot), after the GPU is done with them.
void ForgetMeshes();

#ifdef __OBJC__
// The GPU memory a mesh is in.
id<MTLBuffer> MeshBuffer(const Mesh& mesh);
#endif

}  // namespace frontline::mesh
