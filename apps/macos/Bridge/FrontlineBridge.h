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

// Starts the game, reading the extracted disc in `folder`, with the memory
// card in slot A kept in `cardA` (.gci files; null for no card). Call once, on
// the main thread; the game runs on threads of its own.
void FrontlineStart(const char* folder, const char* cardA);

// The device the game renders with; the view has to use it.
id<MTLDevice> FrontlineDevice(void);

// Draws the frame the game shows now into `drawable` and presents it.
void FrontlinePresent(id<CAMetalDrawable> drawable);

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

#ifdef __cplusplus
}
#endif
