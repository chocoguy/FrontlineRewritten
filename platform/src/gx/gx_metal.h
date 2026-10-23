// The Metal backend's entry points for the GX API (gx.cpp).
#pragma once

#include <cstdint>

namespace frontline::gx {

// GXCopyDisp: the EFB into the frame buffer at `dest`; clears the EFB after.
void CopyDisp(void* dest, bool clear);
void SetCopySource(uint16_t left, uint16_t top, uint16_t width, uint16_t height);

// GXDrawDone: submits the work so far and waits for it.
void DrawDone();

}  // namespace frontline::gx
