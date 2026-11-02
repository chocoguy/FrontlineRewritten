// The vertex stage: decodes GX vertices, transforms and lights them on the
// CPU (as the console's XF unit), and hands triangles to the Metal backend.
#pragma once

#include <cstdint>

namespace frontline::gx {

// A vertex as the TEV shader gets it (gx_shader.cpp), in floats: the position
// in clip space (Metal conventions), two colors, then the generated texture
// coordinates, three each (s, t, q). kVertexBase + 3 * numTexGens in all.
constexpr uint32_t kVertexBase = 12;

// Draws `count` vertices of primitive `prim` (0x80 quads .. 0xB8 points) in
// vertex format `fmt` from a big-endian stream (a display list or the
// immediate-mode pipe). Returns the bytes consumed.
uint32_t Draw(uint32_t prim, uint32_t fmt, const uint8_t* data, uint32_t count);

// A vertex as the data has it, before transform: the matrix indices (or the
// current matrices'), and its attributes as floats (colors 0..1; white where
// the vertex has none).
struct DecodedVertex {
    uint32_t posMtx;
    uint32_t texMtx[8];
    float pos[3];
    float nrm[3];
    float color[2][4];
    float tex[8][2];
    bool hasNrm, hasColor[2];
};

// Decodes `count` vertices of format `fmt` with the current descriptors and
// arrays into `out`, as Draw does before transforming them. Returns the bytes
// consumed.
uint32_t DecodeVertices(uint32_t fmt, const uint8_t* data, uint32_t count, DecodedVertex* out);

// Whether primitive `prim` is made of triangles (quads, triangles, strips,
// fans); lines and points aren't drawn yet.
bool Triangles(uint32_t prim);

// The triangles of `count` vertices of primitive `prim`, numbered from
// `first`, into `idx` (room for count * 3 + 6). Returns the indices written.
template <class Index>
uint32_t TriangleIndices(uint32_t prim, uint32_t count, Index first, Index* idx);

// The bytes one vertex of format `fmt` takes with the current descriptors.
uint32_t VertexSize(uint32_t fmt);

// What a display list's walk (ScanDisplayList) hands on: its draws, its
// writes of an array's base or stride, and that it loads XF registers or has
// a command not known. Its CP and XF loads are applied to the GX state as
// they come, whoever walks it.
struct ListVisitor {
    // A draw of `count` vertices; returns the bytes they take.
    virtual uint32_t Draw(uint32_t prim, uint32_t fmt, const uint8_t* data, uint32_t count) = 0;
    virtual void SetsArray(uint32_t reg, uint32_t value) {}  // CP register 0xA0..0xBF
    virtual void LoadsXf() {}
    virtual void Unknown() {}
};
void ScanDisplayList(const uint8_t* list, uint32_t size, ListVisitor& visitor);

// Runs a display list: register loads and draws.
void RunDisplayList(const uint8_t* list, uint32_t size);

// The backend (gx_metal.mm).
void SubmitTriangles(const float* vertices, uint32_t stride, uint32_t numVertices, const uint16_t* indices,
                     uint32_t numIndices);

}  // namespace frontline::gx
