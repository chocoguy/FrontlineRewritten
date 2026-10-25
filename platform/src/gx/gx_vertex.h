// The vertex stage: decodes GX vertices, transforms and lights them on the
// CPU (as the console's XF unit), and hands triangles to the Metal backend.
#pragma once

#include <cstdint>

namespace frontline::gx {

// A vertex as the TEV shader gets it.
struct Vertex {
    float pos[4];  // clip space (Metal conventions)
    float color[2][4];
    float tex[8][4];  // s, t, q
    float pad[4];
};

// Draws `count` vertices of primitive `prim` (0x80 quads .. 0xB8 points) in
// vertex format `fmt` from a big-endian stream (a display list or the
// immediate-mode pipe). Returns the bytes consumed.
uint32_t Draw(uint32_t prim, uint32_t fmt, const uint8_t* data, uint32_t count);

// The bytes one vertex of format `fmt` takes with the current descriptors.
uint32_t VertexSize(uint32_t fmt);

// Runs a display list: register loads and draws.
void RunDisplayList(const uint8_t* list, uint32_t size);

// The backend (gx_metal.mm).
void SubmitTriangles(const Vertex* vertices, uint32_t numVertices, const uint32_t* indices, uint32_t numIndices);

}  // namespace frontline::gx
