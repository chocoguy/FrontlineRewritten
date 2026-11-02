// Rewritten's Modern renderer (native/render.h): the world, props and
// characters drawn natively from meshes kept on the GPU, with per-pixel
// lighting. The render bins still set GX up as on the console; inside a bin's
// scope (NativeMeshBin) the renderer takes over its display lists.
#pragma once

#include <cstdint>

namespace frontline::mesh {

// The render bin linking now (NATIVE_BIN_*), set by NativeMeshBin in every
// mode. Nothing draws differently unless the Modern renderer is on.
extern int g_bin;

// GXCallDisplayList inside a bin's scope (mesh_draw.mm): draws the list
// natively, true, or false for GX to run it (the Modern renderer off, the bin
// not drawn natively, a GX state it can't draw, its pipeline compiling).
bool Call(const uint8_t* list, uint32_t size);

}  // namespace frontline::mesh
