# Frontline: Rewritten
## A native MOH Frontline port for macOS, iOS, and tvOS

Work in progress. See [ROADMAP.md](ROADMAP.md) for the plan and [docs/architecture/](docs/architecture/README.md) for the decisions behind it.

This repository contains **no game data**. You need your own copy of the game (USA, `GMFE69`).

## Status

**Phases 0 (groundwork) and 1 (the macOS port) are done; Phase 2 (enhancements) is in progress.**
- The game runs natively from your disc in a macOS window: menus, movies, every mission, with sound and memory card saves.
- The data checker validates the converted loaders against your disc (12 big-endian formats).
- Mouse and keyboard controls, in both Classic and Rewritten.
- Details: [docs/port/phase1-status.md](docs/port/phase1-status.md), [docs/port/phase2-status.md](docs/port/phase2-status.md).
- The GameCube build stays unchanged.

## Requirements

- macOS 27 on Apple silicon, with Xcode 27
- CMake 3.31 or later, Ninja and Python 3: `brew install cmake ninja`

## Setup

1. **Clone with the decomp submodule:**

   ```sh
   git clone --recursive <this repo>
   ```

   (or `git submodule update --init` in an existing clone)

2. **Build the GameCube reference** by following [FrontlineDecompiled/README.md](FrontlineDecompiled/README.md): extract the disc into `FrontlineDecompiled/orig/GMFE69/disc`, then run `configure.py` and `ninja`. This matters for two reasons:
   - It's the correctness check for every edit to the shared game code (`tools/regress.py`).
   - It extracts a few binary pieces (the built-in font, the memory card icon) that the native build also compiles in.

## Building

**Xcode** (day-to-day work):

```sh
cmake --preset xcode
open build/xcode/FrontlineRewritten.xcodeproj
```

Run the `FrontlineMac` scheme. To build from the command line instead, use `cmake --build --preset xcode --target FrontlineMac`.

**For playing** (and for benchmarks and Instruments), build optimized. The presets above are Debug builds, which run the game code unoptimized; a Debug app run from Xcode also has Metal API validation on, which slows every draw.

```sh
cmake --preset release
cmake --build --preset release --target FrontlineMac
open "build/release/apps/macos/Frontline Rewritten.app"
```

In Xcode, `cmake --build --preset xcode-release` builds the same configuration (`RelWithDebInfo`). Set `FRONTLINE_PERF=1` to print a line a second saying where each frame's time went: frame rate, CPU, GPU, the waits, and draw and texture counts.

Each launch opens the **setup screen**:
- **Mode:** Classic (the original game, as close to the GameCube version as possible) or Rewritten (deliberate changes). Rewritten also filters textures better (mipmaps, trilinear and anisotropic) and scales movies smoothly.
- **Resolution:** Original (640×448), 720p, 1080p, 1440p or 4K. Rewritten only; 4K is demanding.
- **Widescreen:** levels fill your display's shape, with the HUD at the edges; menus and movies stay 4:3. Rewritten only.
- **Frame rate (experimental, on hold):** 60 (the original) or up to 120, as fast as your screen refreshes. The game still runs at 60; the frames in between are drawn again with the camera moved on and the mouse's latest turn. Rewritten only; it needs a screen faster than 60 Hz.
- **Anti-aliasing:** off (the original) or 4× MSAA, which also smooths the edges of fences, wire and leaves. Rewritten only, on by default.
- **Draw distance:** Original, Far (2×, the default) or Farthest (4×): the level's far plane and fog move out together, so you see farther through thinner haze. Rewritten only.
- **Disc:** your extracted disc (the folder holding `sys` and `files`), remembered once chosen. You can also pass it with `-disc <folder>` or the `FRONTLINE_DISC` environment variable.

Press Play to start. The choices hold until you quit. Between levels the game reboots in place, as the console does, without leaving the window (`FRONTLINE_RESTART_EXEC=1` relaunches the app instead, as it used to). Headless runs take `FRONTLINE_MODE=rewritten`, `FRONTLINE_RESOLUTION=1080`, `FRONTLINE_WIDESCREEN=16:9`, `FRONTLINE_MSAA=4`, `FRONTLINE_DRAW_DISTANCE=2` and `FRONTLINE_HFR=1` (with `FRONTLINE_HFR_RATE=120` to show it as a 120 Hz screen would).

Saves go to the memory card in slot A: `~/Library/Application Support/Frontline Rewritten/Memory Card A`, one `.gci` file per save (the format of Dolphin's GCI folders and GameCube save tools), so saves can be copied to and from Dolphin or a console. `-memoryCard <folder>` uses another folder, and `FRONTLINE_CARD_A` / `FRONTLINE_CARD_B` set either slot.

**Controls:** a game controller (left and right sticks are the main and C sticks, triggers are L and R, a shoulder button is Z, Menu is START), or the keyboard and mouse. The keyboard and mouse work the same in Classic and Rewritten, whatever controller layout is picked in the game's options.

| In a level | Keys |
|---|---|
| Move / walk | W A S D / hold Left Shift |
| Look | Mouse |
| Fire / aim | Left button / hold right button |
| Jump / crouch | Space / C or Left Control |
| Action / reload / melee | F / R / V or middle button |
| Next / previous weapon | Wheel, or Q / E |
| Scope zoom | Wheel |
| Hint from HQ / frag counts | H / hold Tab |
| Pause / continue | Esc |

| In menus | Keys |
|---|---|
| D-pad | Arrow keys |
| A / B | Return / Esc or Delete |
| START | Space |
| X Y Z L R | X Y Z L R |

On-screen controls follow what you used last: after a key press or mouse movement, prompts and menu icons name the keys ("Press F to Open Door", a return key for A); after you touch a controller, they're the original GameCube ones.

While you play, the window captures the mouse pointer; it's let go when the game pauses or a menu comes up, or when you switch away. Switching to another app, or minimizing the window, pauses the game (unless turned off in Settings), and the display stays awake while you play.

**Settings** (⌘,), which apply while you play:
- **Field of view:** 70° (the original) to 110°, across at 4:3; widescreen widens it further. Rewritten only.
- **Mouse:** look speed and inverted look.
- **Keyboard and mouse:** rebind each in-level action to up to two keys or mouse buttons (the side buttons too); the menu keys stay as above. Prompts name your keys from the next screen or level.

**Data check:** loads the converted file formats from your disc through the native loaders and validates them:

```sh
cmake --build --preset ninja --target frontline_datacheck
build/ninja/tests/datacheck/frontline_datacheck FrontlineDecompiled/orig/GMFE69/disc
```

**Headless run:** boots the game with no window or sound for a number of seconds (a smoke test), optionally saving the displayed frame each second (and, with `FRONTLINE_AUDIO_DUMP=<file.wav>`, the sound):

```sh
cmake --build --preset ninja --target frontline_headless
build/ninja/apps/headless/frontline_headless FrontlineDecompiled/orig/GMFE69/disc 10
```

**High frame rate test:** checks how frames drawn between the game's move the camera:

```sh
cmake --build --preset ninja --target frontline_hfrtest
build/ninja/tests/hfrtest/frontline_hfrtest
```

**OS test:** checks the platform's threads, interrupts, alarms and heaps against the console's rules:

```sh
cmake --build --preset ninja --target frontline_ostest
build/ninja/tests/ostest/frontline_ostest
```

**Native build report:** compiles the game code, collects every error and rewrites `docs/port/build-errors.md`:

```sh
python3 tools/build_report.py
```

Add `--sizes` to list the structs whose native layout differs from the GameCube's.

## Editing the game code

The game code lives only in the `FrontlineDecompiled` submodule, and the GameCube build has to keep working. The full rules are in [0004](docs/architecture/0004-compiler-rules.md). In short:

1. Make the edit in `FrontlineDecompiled/`. Put native-only code under `FRONTLINE_NATIVE`, preferably behind a helper in a header.
2. Rebuild the GameCube version (`ninja` in the submodule) and run `python3 tools/regress.py`. It must report 0 dropped and 0 missing.
3. Rerun `python3 tools/build_report.py` to see the native error count fall.
4. Commit in the submodule, push it, then commit the new submodule pointer here.

## Layout

| Path | Contents |
|---|---|
| `FrontlineDecompiled/` | Submodule: the decompiled game, the only copy of the game code |
| `game/` | CMake target that compiles the decomp's Game, Movie, EA System and EA Sound units with clang |
| `platform/` | The GameCube system libraries, natively: OS, DVD, VI, PAD, CARD, audio, GX |
| `apps/macos/` | SwiftUI + Metal app shell |
| `apps/headless/` | The game without a window, for smoke tests |
| `tests/` | `datacheck` (file formats on your disc), `ostest` (the OS layer), `aitest`, `hfrtest`, `limitertest` |
| `tools/` | `gen_sources.py` (unit list from the decomp), `build_report.py` (error work list) |
| `docs/architecture/` | Decisions on pointers, threads, byte order and compiler rules |
| `docs/port/` | Generated reports |

## Legal

Medal of Honor: Frontline is © Electronic Arts. This project isn't affiliated with or endorsed by Electronic Arts or Nintendo. It's distributed as source only; build it with your own copy of the game.
