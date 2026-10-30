// The Swift app's calls into the game and the platform layer.

#include "FrontlineBridge.h"

#include <sys/stat.h>

#include <cstdio>

// The game's headers define BOOL as int; Objective-C's is bool.
#define BOOL FRONTLINE_GAME_BOOL
#include "frontline/display.h"
#include "frontline/platform.h"
#include "native/hooks.h"
#undef BOOL

namespace {

bool IsFolder(const char* folder, const char* sub) {
    char path[4096];
    std::snprintf(path, sizeof(path), "%s/%s", folder, sub);
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

}  // namespace

bool FrontlineIsDisc(const char* folder) { return IsFolder(folder, "sys") && IsFolder(folder, "files"); }

static_assert(FrontlineModeClassic == NATIVE_MODE_CLASSIC && FrontlineModeRewritten == NATIVE_MODE_REWRITTEN);

void FrontlineSetGameMode(int mode) { frontline::SetGameMode(mode); }
void FrontlineSetRenderLines(int lines) { frontline::SetRenderLines(lines); }
void FrontlineSetWidescreen(float aspect) { frontline::SetWidescreen(aspect); }
void FrontlineSetHighFrameRate(bool on) { frontline::SetHighFrameRate(on); }
void FrontlineSetMsaa(int samples) { frontline::SetMsaa(samples); }
void FrontlineSetDrawDistance(float times) { frontline::SetDrawDistance(times); }
void FrontlineSetFieldOfView(float degrees) { frontline::SetFieldOfView(degrees); }

bool FrontlineStart(const char* folder, const char* cardA) {
    frontline::SetDiscRoot(folder);
    frontline::SetCardFolder(0, cardA);
    // The game image in the app's Frameworks folder, else where the build made it.
    NSString* bundled = [NSBundle.mainBundle.privateFrameworksPath stringByAppendingPathComponent:@"FrontlineGame.so"];
    const char* image = [NSFileManager.defaultManager fileExistsAtPath:bundled] ? bundled.fileSystemRepresentation
                                                                               : FRONTLINE_GAME_IMAGE;
    frontline::GameMain gameMain = frontline::LoadGame(image);
    if (gameMain == nullptr) {
        return false;
    }
    frontline::StartGame(gameMain);
    return true;
}

id<MTLDevice> FrontlineDevice(void) { return frontline::MetalDevice(); }

void FrontlinePresent(id<CAMetalDrawable> drawable, double time) { frontline::Present(drawable, time); }

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

// The action bits are the game's.
constexpr uint32_t kActionBits[][2] = {
    {FrontlineActionForward, NATIVE_ACTION_FORWARD},
    {FrontlineActionBack, NATIVE_ACTION_BACK},
    {FrontlineActionLeft, NATIVE_ACTION_LEFT},
    {FrontlineActionRight, NATIVE_ACTION_RIGHT},
    {FrontlineActionWalk, NATIVE_ACTION_WALK},
    {FrontlineActionJump, NATIVE_ACTION_JUMP},
    {FrontlineActionCrouch, NATIVE_ACTION_CROUCH},
    {FrontlineActionAim, NATIVE_ACTION_AIM},
    {FrontlineActionFire, NATIVE_ACTION_FIRE},
    {FrontlineActionReload, NATIVE_ACTION_RELOAD},
    {FrontlineActionNextWeapon, NATIVE_ACTION_NEXT_WEAPON},
    {FrontlineActionPrevWeapon, NATIVE_ACTION_PREV_WEAPON},
    {FrontlineActionMelee, NATIVE_ACTION_MELEE},
    {FrontlineActionUse, NATIVE_ACTION_USE},
    {FrontlineActionCenterView, NATIVE_ACTION_CENTER_VIEW},
    {FrontlineActionHint, NATIVE_ACTION_HINT},
    {FrontlineActionScores, NATIVE_ACTION_SCORES},
    {FrontlineActionPause, NATIVE_ACTION_PAUSE},
    {FrontlineActionMenuUp, NATIVE_ACTION_MENU_UP},
    {FrontlineActionMenuDown, NATIVE_ACTION_MENU_DOWN},
    {FrontlineActionMenuLeft, NATIVE_ACTION_MENU_LEFT},
    {FrontlineActionMenuRight, NATIVE_ACTION_MENU_RIGHT},
    {FrontlineActionMenuA, NATIVE_ACTION_MENU_A},
    {FrontlineActionMenuB, NATIVE_ACTION_MENU_B},
    {FrontlineActionMenuX, NATIVE_ACTION_MENU_X},
    {FrontlineActionMenuY, NATIVE_ACTION_MENU_Y},
    {FrontlineActionMenuZ, NATIVE_ACTION_MENU_Z},
    {FrontlineActionMenuL, NATIVE_ACTION_MENU_L},
    {FrontlineActionMenuR, NATIVE_ACTION_MENU_R},
    {FrontlineActionMenuStart, NATIVE_ACTION_MENU_START},
};
constexpr bool SameActionBits() {
    for (const auto& bits : kActionBits) {
        if (bits[0] != bits[1]) {
            return false;
        }
    }
    return true;
}
static_assert(SameActionBits());
static_assert(FrontlineLabelUse == NATIVE_LABEL_USE && FrontlineLabelMenuY == NATIVE_LABEL_MENU_Y &&
              FrontlineLabelMenuY + 1 == NATIVE_NUM_LABELS);

void FrontlineSetKeyboardMouse(int port, uint32_t actions, float lookRight, float lookUp, int wheel) {
    frontline::SetKeyboardMouse(port, actions, lookRight, lookUp, wheel);
}

void FrontlineAddMouseLook(int port, float lookRight, float lookUp) { frontline::AddMouseLook(port, lookRight, lookUp); }

void FrontlinePressAction(int port, uint32_t actions) { frontline::PressAction(port, actions); }

bool FrontlineIsPlaying(void) { return frontline::IsPlaying(); }

void FrontlineSetKeyLabel(int label, const char* text) { frontline::SetKeyLabel(label, text); }
