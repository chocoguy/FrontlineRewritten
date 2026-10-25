// The Swift app's calls into the game and the platform layer.

#include "FrontlineBridge.h"

#include <sys/stat.h>

#include <cstdio>

// The game's headers define BOOL as int; Objective-C's is bool.
#define BOOL FRONTLINE_GAME_BOOL
#include "frontline/display.h"
#include "frontline/platform.h"
#undef BOOL

extern int FrontlineGameMain();

namespace {

// No C++ allocation here: before the game starts, operator new is already the
// game's on this thread.
bool IsFolder(const char* folder, const char* sub) {
    char path[4096];
    std::snprintf(path, sizeof(path), "%s/%s", folder, sub);
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

}  // namespace

bool FrontlineIsDisc(const char* folder) { return IsFolder(folder, "sys") && IsFolder(folder, "files"); }

void FrontlineStart(const char* folder, const char* cardA) {
    frontline::SetDiscRoot(folder);
    frontline::SetCardFolder(0, cardA);
    frontline::StartGame(FrontlineGameMain);
}

id<MTLDevice> FrontlineDevice(void) { return frontline::MetalDevice(); }

void FrontlinePresent(id<CAMetalDrawable> drawable) { frontline::Present(drawable); }

void FrontlineSetPad(int port, FrontlinePad pad) {
    PADStatus status = {};
    status.button = pad.buttons;
    status.stickX = pad.stickX;
    status.stickY = pad.stickY;
    status.substickX = pad.substickX;
    status.substickY = pad.substickY;
    status.triggerLeft = pad.triggerLeft;
    status.triggerRight = pad.triggerRight;
    // 4-bit analog A and B, as the game reads them (CGCDevice scales by 0x11).
    status.analogA = (pad.buttons & FrontlineButtonA) != 0 ? 0x0F : 0;
    status.analogB = (pad.buttons & FrontlineButtonB) != 0 ? 0x0F : 0;
    status.err = pad.connected ? PAD_ERR_NONE : PAD_ERR_NO_CONTROLLER;
    frontline::SetPadState(port, status);
}

bool FrontlineRumble(int port) { return frontline::GetPadMotor(port) == PAD_MOTOR_RUMBLE; }
