// The game as the Swift app sees it (its bridging header). Plain C and
// Objective-C, so Swift can import it.
#pragma once

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Whether `folder` holds an extracted disc (sys/ and files/).
bool FrontlineIsDisc(const char* folder);

// The way to play and its picture, for the whole run; call before
// FrontlineStart. Classic always renders the original 640 x 448 at 4:3.
//   lines: the render's height (480 original, 720, 1080, 1440, 2160)
//   aspect: the screen's width / height for widescreen, 0 for 4:3
enum {
    FrontlineModeClassic,
    FrontlineModeRewritten,
};
void FrontlineSetGameMode(int mode);
void FrontlineSetRenderLines(int lines);
void FrontlineSetWidescreen(float aspect);
// Rewritten's high frame rate: frames drawn between the game's, up to the
// screen's refresh rate.
void FrontlineSetHighFrameRate(bool on);
// Rewritten's anti-aliasing: samples a pixel, 1 (off) or 4 (4x MSAA).
void FrontlineSetMsaa(int samples);
// Rewritten's draw distance: times the level's own far plane and fog.
void FrontlineSetDrawDistance(float times);
// Rewritten's field of view, degrees across at 4:3 (70, the original, to
// 110). It can change while the game runs.
void FrontlineSetFieldOfView(float degrees);

// Starts the game, reading the extracted disc in `folder`, with the memory
// card in slot A kept in `cardA` (.gci files; null for no card). Call once, on
// the main thread; the game runs on threads of its own. False if the game
// image couldn't be loaded.
bool FrontlineStart(const char* folder, const char* cardA);

// The device the game renders with; the view has to use it.
id<MTLDevice> FrontlineDevice(void);

// Draws the frame the game shows now into `drawable` and presents it. `time`
// is when it will be on screen (a display link's target presentation time,
// in CACurrentMediaTime's seconds). Call from one thread, once a refresh.
void FrontlinePresent(id<CAMetalDrawable> drawable, double time);

// A GameCube controller's state (the SDK's PADStatus): buttons, sticks in
// about -100..100 (up and right positive), and analog triggers in 0..255.
typedef struct {
    bool connected;
    uint16_t buttons;
    int8_t stickX, stickY;
    int8_t substickX, substickY;
    uint8_t triggerLeft, triggerRight;
} FrontlinePad;

// The buttons' bits (PAD_BUTTON_* and PAD_TRIGGER_*).
enum {
    FrontlineButtonLeft = 0x0001,
    FrontlineButtonRight = 0x0002,
    FrontlineButtonDown = 0x0004,
    FrontlineButtonUp = 0x0008,
    FrontlineButtonZ = 0x0010,
    FrontlineButtonR = 0x0020,
    FrontlineButtonL = 0x0040,
    FrontlineButtonA = 0x0100,
    FrontlineButtonB = 0x0200,
    FrontlineButtonX = 0x0400,
    FrontlineButtonY = 0x0800,
    FrontlineButtonStart = 0x1000,
};

// Hands the game controller `port`'s latest state (0-3).
void FrontlineSetPad(int port, FrontlinePad pad);

// Whether the game runs controller `port`'s rumble motor now.
bool FrontlineRumble(int port);

// Keyboard and mouse actions: what the player does, rather than controller
// buttons (NATIVE_ACTION_* in the game's native/hooks.h). The game picks the
// playing or the menu actions by what's on screen.
enum {
    FrontlineActionForward = 1u << 0,
    FrontlineActionBack = 1u << 1,
    FrontlineActionLeft = 1u << 2,
    FrontlineActionRight = 1u << 3,
    FrontlineActionWalk = 1u << 4,
    FrontlineActionJump = 1u << 5,
    FrontlineActionCrouch = 1u << 6,
    FrontlineActionAim = 1u << 7,
    FrontlineActionFire = 1u << 8,
    FrontlineActionReload = 1u << 9,
    FrontlineActionNextWeapon = 1u << 10,
    FrontlineActionPrevWeapon = 1u << 11,
    FrontlineActionMelee = 1u << 12,
    FrontlineActionUse = 1u << 13,
    FrontlineActionCenterView = 1u << 14,
    FrontlineActionHint = 1u << 15,
    FrontlineActionScores = 1u << 16,
    FrontlineActionPause = 1u << 17,
    FrontlineActionMenuUp = 1u << 18,
    FrontlineActionMenuDown = 1u << 19,
    FrontlineActionMenuLeft = 1u << 20,
    FrontlineActionMenuRight = 1u << 21,
    FrontlineActionMenuA = 1u << 22,
    FrontlineActionMenuB = 1u << 23,
    FrontlineActionMenuX = 1u << 24,
    FrontlineActionMenuY = 1u << 25,
    FrontlineActionMenuZ = 1u << 26,
    FrontlineActionMenuL = 1u << 27,
    FrontlineActionMenuR = 1u << 28,
    FrontlineActionMenuStart = 1u << 29,
};

// Hands the game the keyboard and mouse driving controller `port`: the
// actions held now, and the mouse movement (radians, right and up positive)
// and wheel notches (up positive) since the last call.
void FrontlineSetKeyboardMouse(int port, uint32_t actions, float lookRight, float lookUp, int wheel);
// Mouse movement as it comes (radians, right and up positive), on top of
// FrontlineSetKeyboardMouse's: the high frame rate turns the view by it at
// once.
void FrontlineAddMouseLook(int port, float lookRight, float lookUp);
// One press of `actions` at the game's next look, as if keys bound to them were
// tapped. Pause does nothing unless the player is playing.
void FrontlinePressAction(int port, uint32_t actions);

// Whether controller 1's player is playing (in a level, not paused, no menu
// up): the mouse looks around then.
bool FrontlineIsPlaying(void);

// The keys prompts and menu icons name (NATIVE_LABEL_* in native/hooks.h).
enum {
    FrontlineLabelUse,
    FrontlineLabelCrouch,
    FrontlineLabelAim,
    FrontlineLabelPause,
    FrontlineLabelHint,
    FrontlineLabelScopeZoom,
    FrontlineLabelMenuA,
    FrontlineLabelMenuB,
    FrontlineLabelMenuX,
    FrontlineLabelMenuY,
};

// Tells the game what the key for `label` is called ("F", "Right Mouse
// Button"). Call before FrontlineStart.
void FrontlineSetKeyLabel(int label, const char* text);

#ifdef __cplusplus
}
#endif
