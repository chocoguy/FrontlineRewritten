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

// Runs the game's main() on the console's default thread (a game thread at
// priority 16) and returns; the calling thread (the app's main thread) never
// runs game code (docs/architecture/0002-threads-interrupts-and-time.md).
void StartGame(int (*gameMain)());

// The extracted disc: the folder holding sys/ and files/. Without it, DVDInit
// falls back to the FRONTLINE_DISC environment variable.
void SetDiscRoot(const char* folder);

// The frame buffer the video output shows now (updated at each retrace).
struct DisplayedFrame {
    uint32_t xfb;  // 32-bit address of the frame buffer (0: none yet)
    bool black;    // VISetBlack
    uint32_t retrace;
    uint16_t width, height;  // frame buffer size (VIConfigure)
};
DisplayedFrame GetDisplayedFrame();

// The displayed frame as rendered (RGBA8, width x height), for tools; false if
// that frame buffer hasn't been drawn by GX. Pass null `rgba` for the size.
bool ReadDisplayedFrame(uint8_t* rgba, uint32_t* width, uint32_t* height);

// Memory cards: slot 0 (A) or 1 (B) holds the card kept in `folder` (created
// if missing), as .gci files; null leaves the slot empty. Set before the game
// starts. FRONTLINE_CARD_A and FRONTLINE_CARD_B override it.
void SetCardFolder(int slot, const char* folder);

// Sound: whether the DSP's mix plays on the default output device. On unless
// turned off before the game starts (headless turns it off).
void SetAudioOutput(bool enabled);

// Controllers: the app's latest input for a port (err PAD_ERR_NO_CONTROLLER
// when nothing is connected), and the rumble the game asks for.
void SetPadState(int port, const PADStatus& status);
uint32_t GetPadMotor(int port);

}  // namespace frontline
