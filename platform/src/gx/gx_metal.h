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

// GXDrawDone: submits the work so far, and waits for the frame before it
// (not this one: the GPU never reads game memory, so it can draw while the
// game goes on).
void DrawDone();

#ifdef __OBJC__
// For the display (display.mm): the device and queue GX renders with, and
// the texture GX copied into the frame buffer at `addr`, or nil if it has none
// or the CPU wrote that frame buffer since. The copy may still be running on
// the GPU; work committed to the same queue later, like the display's, runs
// after it.
id<MTLDevice> Device();
id<MTLCommandQueue> Queue();
id<MTLTexture> XfbTexture(uint32_t addr);

// Whether the display shows frames smaller than they are rendered (a window
// smaller than the render), so frame copies make mipmaps for it.
void SetDisplayShrinks(bool shrinks);
bool DisplayShrinks();

// High frame rate (gx_record.h): the finished record of the frame GX copied
// into the frame buffer at `addr`, retained (ReleaseRecord), or null if it has
// none or the CPU wrote that frame buffer since.
struct FrameRecord;
FrameRecord* XfbRecord(uint32_t addr);
#endif

}  // namespace frontline::gx
