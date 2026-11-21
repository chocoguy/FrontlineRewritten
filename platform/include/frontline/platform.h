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
// After StopGame, it starts the stopped game again instead (`gameMain` is
// the same), as a cold boot.
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

// Called after each change to a memory card's files: the .gci at `path` was
// written in full, or `removed`. It runs on the game's thread, so it must
// return at once (the tvOS app backs the card up in its user defaults).
using CardChangeCallback = void (*)(const char* path, bool removed);
void SetCardChangeCallback(CardChangeCallback callback);

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
//   renderer: the Modern renderer (platform/src/mesh/) draws the world,
//     props and characters natively, with per-pixel lighting; else GX draws
//     everything. Modern turns the high frame rate off.
//   corrected lighting: Modern lights with true directional lights instead
//     of the original's (which light characters from the camera)
//   blob shadows: the shadow under characters the game was designed with,
//     which the GameCube version leaves out
//   fast smoke: heavy smoke and spray are drawn into a buffer about the
//     console's size and laid over the picture (gx_metal.mm, Rewritten's
//     particles), instead of at the render's resolution; on unless set.
//     FRONTLINE_FAST_SMOKE=0|1 overrides it.
//   bicubic textures: textures drawn larger than they are get a sharper,
//     smoother filter than the original's bilinear (gx_shader.cpp
//     SampleBicubic); on unless set. FRONTLINE_TEXTURE_FILTER=
//     bilinear|bicubic overrides it.
//   ambient occlusion: creases and the ground under things are darkened,
//     from the picture's depth (gx_metal.mm OccludeScene); off unless
//     set, and off with the high frame rate. FRONTLINE_AO=0|1 overrides it.
//   flash lights: gunfire and explosions light what's around them for a
//     moment (game/native/render/flash_lights.cpp); off unless set.
//     FRONTLINE_FLASH_LIGHTS=0|1 overrides it.
//   Unfair AI: Hard's enemies aim perfectly, sense everything, press the
//     attack and take a third of the damage (game/native/ai/)
void SetGameMode(int mode);
void SetRenderLines(int lines);
void SetWidescreen(float aspect);
void SetHighFrameRate(bool on);
void SetMsaa(int samples);
void SetDrawDistance(float times);
void SetModernRenderer(bool on);
void SetCorrectedLighting(bool on);
void SetBlobShadows(bool on);
void SetFastSmoke(bool on);
void SetBicubicTextures(bool on);
void SetAmbientOcclusion(bool on);
void SetFlashLights(bool on);
void SetUnfairAI(bool on);

// Rewritten's field of view, in degrees across at 4:3 (the original is 70),
// from kMinFieldOfView to kMaxFieldOfView. Unlike the settings above it can
// change while the game runs (FRONTLINE_FOV=<degrees> overrides it).
constexpr float kMinFieldOfView = 70.0f;
constexpr float kMaxFieldOfView = 110.0f;
void SetFieldOfView(float degrees);

// Rewritten's Full Auto Springfield: the player's Springfield fires for as
// long as the trigger is held. It can change while the game runs
// (FRONTLINE_FULL_AUTO=0|1 overrides it).
void SetFullAutoSpringfield(bool on);

// Rewritten's mods (native/mods.h, platform/src/mods.cpp): the catalog, by
// NATIVE_MOD_* (a key the app keeps the choice under, a title and a
// sentence), and which are on. The choice can change at any time; the game
// takes it as each level starts (FRONTLINE_MODS=<key>,<key> overrides it).
int ModCount();
const char* ModKey(int mod);
const char* ModTitle(int mod);
const char* ModSummary(int mod);
void SetModOn(int mod, bool on);

// Whether the high frame rate is on for this run (Rewritten only).
bool HighFrameRate();

// Whether heavy smoke goes to the particle buffer (Rewritten only).
bool FastSmoke();

// Whether magnified textures are filtered bicubically (Rewritten only).
bool BicubicTextures();

// Whether ambient occlusion is on for this run (Rewritten only).
bool AmbientOcclusion();

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
// Look by touch (iOS), the same as AddMouseLook's, but the prompts stay the
// controller's: the touch controls' buttons stand in for its buttons.
void AddTouchLook(int port, float lookRight, float lookUp);
// One press of `action` at the game's next take, as if a key bound to it was
// tapped (the app pauses the game this way when it goes to the background).
void PressAction(int port, uint32_t action);
// Game actions a controller's buttons are bound to (the app's controller
// bindings), held now. They add to the keyboard's, but count as the
// controller being used, so the on-screen controls stay the controller's.
void SetPadActions(int port, uint32_t held);

// What the keys bound to an action are called (NATIVE_LABEL_* in
// native/hooks.h), for the on-screen controls. Set before the game starts.
void SetKeyLabel(int label, const char* text);

// Whether controller 1's player is playing now (in a level, not paused, past
// the intro screens), when the mouse looks around rather than the menus
// being up.
bool IsPlaying();

// Exit Game, back to the app's setup screen. With it offered, the main menu
// shows "B EXIT GAME", and B there makes TakeExitGameRequest true once. The
// app asks the player, and StopGame stops the game in place: it waits for the
// game to halt (true), or gives up after a few seconds with the game still
// running (false). StartGame then starts it again, from a cold boot, with
// the settings set since.
void SetExitGameOffered(bool offered);
bool TakeExitGameRequest();
bool StopGame();
// Whether StopGame stopped the game, and it hasn't been started again.
bool GameStopped();

}  // namespace frontline
