# Frontline: Rewritten
## A native MOH Frontline port for macOS, iOS, and tvOS

Work in progress. See [ROADMAP.md](ROADMAP.md) for the plan and [docs/architecture/](docs/architecture/README.md) for the decisions behind it.

This repository contains **no game data**. You need your own copy of the game (USA, `GMFE69`).

## Status

**Phase 0 (groundwork) is done; Phase 1 is in progress.**
- The game links and boots natively from your disc: it loads its data, starts its threads, and runs its frame loop at 59.94 Hz up to the "Press Start" screen.
- Nothing is drawn yet: the GX → Metal renderer is next. Sound runs silently, and there are no memory cards yet.
- The data checker validates the converted loaders against your disc (12 big-endian formats).
- Details: [docs/port/phase1-status.md](docs/port/phase1-status.md).
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

**Data check:** loads the converted file formats from your disc through the native loaders and validates them:

```sh
cmake --build --preset ninja --target frontline_datacheck
build/ninja/tests/datacheck/frontline_datacheck FrontlineDecompiled/orig/GMFE69/disc
```

**Headless run:** boots the game with no window or sound for a number of seconds (a smoke test), optionally saving the displayed frame each second:

```sh
cmake --build --preset ninja --target frontline_headless
build/ninja/apps/headless/frontline_headless FrontlineDecompiled/orig/GMFE69/disc 10
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
| `tests/` | `datacheck` (file formats on your disc), `ostest` (the OS layer) |
| `tools/` | `gen_sources.py` (unit list from the decomp), `build_report.py` (error work list) |
| `docs/architecture/` | Decisions on pointers, threads, byte order and compiler rules |
| `docs/port/` | Generated reports |

## Legal

Medal of Honor: Frontline is © Electronic Arts. This project isn't affiliated with or endorsed by Electronic Arts or Nintendo. It's distributed as source only; build it with your own copy of the game.
