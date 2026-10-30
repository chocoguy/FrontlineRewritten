// The video output on screen, for the app (Objective-C++).
#pragma once

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

namespace frontline {

// The device the game renders with; a view that shows the game must use it.
id<MTLDevice> MetalDevice();

// Draws the frame the video output shows now into `drawable` and presents it:
// scaled to the largest 4:3 picture that fits, with black bars, its lines
// placed on a 480-line NTSC screen. `time` is when the drawable will be on
// screen, in seconds on CACurrentMediaTime's clock (a display link's target
// presentation time). Call from one thread (the app's display thread), at
// the display's refresh rate.
void Present(id<CAMetalDrawable> drawable, double time);

}  // namespace frontline
