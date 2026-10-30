// The native platform layer: what replaces the GameCube system libraries.
#pragma once

#include <cstdint>

#include "dolphin/pad.h"
#include "native/address.h"

namespace frontline {

// Default arena size: about 10x the GameCube's 24 MB of main RAM, since
// native structs are bigger (docs/architecture/0001-pointers-and-memory.md).
constexpr uint32_t kDefaultArenaSize = 256u << 20;

// Reserves the arena (once) and returns its usable start. Every game heap
// lives in it; OSGetArenaLo/Hi report its bounds.
void* ArenaInit(uint32_t size = kDefaultArenaSize);
void* ArenaLo();
void* ArenaHi();

// Loads the game image (game/FrontlineGame.so) at `path`. Its static
// initializers run here, on the calling thread, which is the console's default
// thread until StartGame. Returns the game's main(), or null (with the reason
// printed).
using GameMain = int (*)();
GameMain LoadGame(const char* path);

// Runs the game's main() on the console's default thread (a game thread at
// priority 16) and returns; the calling thread (the app's main thread) never
// runs game code (docs/architecture/0002-threads-interrupts-and-time.md).
void StartGame(int (*gameMain)());

// The extracted disc: the folder holding sys/ and files/. Without it, DVDInit
// falls back to the FRONTLINE_DISC environment variable.
void SetDiscRoot(const char* folder);

// The frame buffer the video output shows now (updated at each retrace), and
// the one flushed to show at the next. Times are when retraces were due, in
// seconds on CACurrentMediaTime's clock.
struct DisplayedFrame {
    uint32_t xfb;  // 32-bit address of the frame buffer (0: none yet)
    bool black;    // VISetBlack
    uint32_t retrace;  // the latest retrace
    uint16_t width, height;  // frame buffer size (VIConfigure)
    // The retrace that latched the frame shown: the game missed the ones
    // since, if any.
    uint32_t latchRetrace;
    double latchTime;
    // Flushed since (VIFlush), to show at the next retrace (0: none).
    uint32_t nextXfb;
    bool nextBlack;
    double nextLatchTime;
};

// A retrace's length: NTSC's 1001/60000 s.
constexpr double kRetraceSeconds = 1001.0 / 60000.0;
DisplayedFrame GetDisplayedFrame();

// The displayed frame as rendered (RGBA8, width x height), for tools; false if
// that frame buffer hasn't been drawn by GX. Pass null `rgba` for the size.
bool ReadDisplayedFrame(uint8_t* rgba, uint32_t* width, uint32_t* height);

// For runs without a window (headless): shows the game, from now on, as a
// screen refreshing `rate` times a second would, into a picture nobody sees.
// FRONTLINE_PERF and FRONTLINE_DISPLAY_DUMP report on it as on a window's.
void StartOffscreenDisplay(double rate);

// Memory cards: slot 0 (A) or 1 (B) holds the card kept in `folder` (created
// if missing), as .gci files; null leaves the slot empty. Set before the game
// starts. FRONTLINE_CARD_A and FRONTLINE_CARD_B override it.
void SetCardFolder(int slot, const char* folder);

// The way to play and its picture, for the whole run (set before the game
// starts; FRONTLINE_MODE=classic|rewritten, FRONTLINE_RESOLUTION=<lines>,
// FRONTLINE_WIDESCREEN=<w>:<h>, FRONTLINE_HFR=0|1, FRONTLINE_MSAA=<samples>
// and FRONTLINE_DRAW_DISTANCE=<times> override them). Classic always renders
// the original 640 x 448 at 4:3 at 59.94 frames a second, whatever is set.
//   mode: NATIVE_MODE_* (native/hooks.h)
//   lines: the render's height on a 480-line screen: 480 (the original), 720,
//     1080, 1440, 2160
//   aspect: the screen's width / height for widescreen, 0 for 4:3
//   high frame rate: the display draws frames between the game's
//     (gx_record.mm), up to the screen's refresh rate
//   MSAA: samples a pixel, 1 (off) or 4
//   draw distance: times the level's own (its far plane and fog), 1 to 4
void SetGameMode(int mode);
void SetRenderLines(int lines);
void SetWidescreen(float aspect);
void SetHighFrameRate(bool on);
void SetMsaa(int samples);
void SetDrawDistance(float times);

// Rewritten's field of view, in degrees across at 4:3 (the original is 70),
// from kMinFieldOfView to kMaxFieldOfView. Unlike the settings above it can
// change while the game runs (FRONTLINE_FOV=<degrees> overrides it).
constexpr float kMinFieldOfView = 70.0f;
constexpr float kMaxFieldOfView = 110.0f;
void SetFieldOfView(float degrees);

// Whether the high frame rate is on for this run (Rewritten only).
bool HighFrameRate();

// Samples a pixel the GX layer renders with (1 in Classic).
int MsaaSamples();

// How many times the original resolution the GX layer renders at (1 in
// Classic).
float RenderScale();

// Sound: whether the DSP's mix plays on the default output device. On unless
// turned off before the game starts (headless turns it off).
void SetAudioOutput(bool enabled);

// Controllers: the app's latest input for a port (err PAD_ERR_NO_CONTROLLER
// when nothing is connected), and the rumble the game asks for.
void SetPadState(int port, const PADStatus& status);
uint32_t GetPadMotor(int port);

// The keyboard and mouse driving a port, as game actions (NATIVE_ACTION_* in
// native/hooks.h): what's held now, and the mouse movement (radians, right
// and up positive) and wheel notches (up positive) since the last call.
void SetKeyboardMouse(int port, uint32_t held, float lookRight, float lookUp, int wheel);
// More mouse movement since the last take, as it comes.
void AddMouseLook(int port, float lookRight, float lookUp);
// One press of `action` at the game's next take, as if a key bound to it was
// tapped (the app pauses the game this way when it goes to the background).
void PressAction(int port, uint32_t action);

// What the keys bound to an action are called (NATIVE_LABEL_* in
// native/hooks.h), for the on-screen controls. Set before the game starts.
void SetKeyLabel(int label, const char* text);

// Whether controller 1's player is playing now (in a level, not paused, past
// the intro screens), when the mouse looks around rather than the menus
// being up.
bool IsPlaying();

}  // namespace frontline
