// The Metal backend's entry points for the GX API (gx.cpp).
#pragma once

#include <cstdint>

#ifdef __OBJC__
#import <Metal/Metal.h>
#endif

namespace frontline::gx {

// GXCopyDisp: the EFB into the frame buffer at `dest`; clears the EFB after.
void CopyDisp(void* dest, bool clear);
void SetCopySource(uint16_t left, uint16_t top, uint16_t width, uint16_t height);

// GXDrawDone: submits the work so far and waits for it.
void DrawDone();

#ifdef __OBJC__
// For the display (display.mm): the device and queue GX renders with, and
// the texture GX copied into the frame buffer at `addr`, or nil if it has none
// or the CPU wrote that frame buffer since.
id<MTLDevice> Device();
id<MTLCommandQueue> Queue();
id<MTLTexture> XfbTexture(uint32_t addr);
#endif

}  // namespace frontline::gx
