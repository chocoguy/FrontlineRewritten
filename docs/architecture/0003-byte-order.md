# 0003: Byte order

**Status:** accepted (Phase 0, 2026-10-21); revised in Phase 1 (see the end) · **Covers:** ROADMAP gotcha 3

## Problem

The GameCube is big-endian and Apple silicon is little-endian. The game's data doesn't have one byte order:

- **Data files shared with the PS2 version are little-endian.** The GameCube build swaps them after loading with `EndianSwap()` from [endian.h](../../FrontlineDecompiled/include/game/endian.h), which it always does.
- **Memory the graphics chip reads is big-endian.** That covers:
  - vertex, normal, texture-coordinate and matrix arrays (`GXSetArray`);
  - display lists (`GXCallDisplayList`);
  - textures and palettes (`GXInitTexObj`, `GXInitTlutObj`).
- **Some of that GPU memory comes from disc, and some is filled by the CPU at runtime.** In [dmesh.cpp](../../FrontlineDecompiled/src/game/dmesh.cpp:118), the positions come from the file but the skinning matrices are computed every frame. The movie player's swizzlers build textures and palettes on the fly. On the GameCube a CPU store is big-endian, so the two match. On a Mac they wouldn't.
- **Some GameCube-only formats are read by the CPU without a swap**, so they're big-endian too.

## Decision

The rule depends on **who reads the data**:

| Data | Byte order in memory | Native handling | GameCube build |
|---|---|---|---|
| Shared (PS2-era) files the CPU reads | little-endian | `EndianSwap()` does nothing | unchanged (swaps) |
| Anything the GPU reads: arrays, display lists, textures, palettes | **big-endian, as on hardware** | The Metal GX layer decodes it as big-endian and caches the result | unchanged |
| GPU data the CPU writes at runtime | big-endian | Written with `GXStore16/32/F32` helpers that store big-endian | helpers are plain stores |
| GameCube-only formats the CPU reads | big-endian | `FromBigEndian()` swaps it when loaded | `FromBigEndian()` does nothing |
| Saves | native | Plain files in host order | memory card as before |

**Why the GPU side stays big-endian:**
- The GX layer only gets a pointer. It can't tell a buffer loaded from disc from one the CPU filled in.
- Converting disc data to host order at load time would mean parsing display lists (which depends on the vertex format) inside every loader.
- Keeping GPU memory exactly as the hardware sees it is what Dolphin and aurora do. Data from disc needs no changes at all.
- Only the few places where the CPU writes GPU data need edits:
  - dmesh skinning matrices;
  - the movie swizzlers' textures and palettes;
  - anything else that calls the four entry points above on a buffer the CPU filled. A grep finds those.

**Cost:** the GX layer converts data the first time it uploads it, and caches the result by pointer, size and a content hash. Static meshes and textures are converted once. Per-frame data (matrices, movie frames) is converted every frame, but it's small.

**Saves:** native saves aren't compatible with GameCube memory card saves. A converter can come later if anyone wants one.

## Rejected

- **Host order everywhere, converting disc data at load.** Every loader would need format knowledge (display lists can't be swapped without the vertex format). Getting one wrong produces subtle graphics corruption far from the cause.
- **Tagging each buffer's byte order at runtime.** More edits than the CPU-write helpers, and easy to get wrong silently.

## Consequences

- The GX → Metal layer has one rule, the same as the hardware's, and can be checked against Dolphin directly.
- When CPU-built GPU data is wrong, it shows up as garbage in a known place (movies, skinned meshes), which makes it easy to trace.

## Revisions from Phase 1

**Big-endian GameCube-only formats are common, not the exception.** Checking the disc data showed that most of the level and model formats the CPU reads are big-endian GameCube data:

| Byte order | Formats |
|---|---|
| **Big-endian** (swapped at load natively) | `.cdb`, `.cpt`, `.msh`, `.mpk`, `.dmf` (version 6), `.tpk`, `.gsh` headers, `.gfn` fonts, `.scr` UI screens, `.aem` sound event systems, `.abk` module banks and the `BNKb` sample banks inside them, `.mpf` music maps |
| **Little-endian, PS2-era** (the GameCube's swaps turn off natively) | `.emt`, `.lfc`, `.bpd`, `.som`, `.skl`, `.mvd`, `.cls`, `.cbs`/`.sin` scripts, `bsbifunc.dat` (built-ins), `weapbul_*.dat` (bullet tables) |
| **Bytes** (no order) | `.bum` message text and index, script and UI bytecode (read a byte at a time), sample data (the DSP decoder reads it as is) |

How it's handled:
- **Swap passes.** Each big-endian loader gets a native swap pass (`CDBFromBigEndian`, `CPTFromBigEndian`, `StaticMeshFromBigEndian`, `DMeshFromBigEndian`, `TexturePackFromBigEndian`, `ShapeFileFromBigEndian`, …) that runs before relocation. It swaps exactly the fields the CPU reads; GPU payloads stay big-endian.
- **Shared records are swapped once.** Files that can be loaded more than once mark themselves as swapped (`SHPG` → `SHPN`, `DMF` → `DMN`), and shared records go through `NativeSwapOnce`.
- **Bitfields in file structs** are declared in reverse natively, because clang allocates from the low bit and CodeWarrior from the high bit.
- **CPU code that reads or writes GPU-format data** uses `GX_LOAD16` and `GX_STORE16/32` from `game/gxstore.h`. That covers the display-list setup commands written by the compartment and mesh loaders, and the morph code.
- **Exception:** indexed matrix arrays (`GXSetArray(21..24)`) are always CPU-built skinning matrices, so the GX layer reads them in host order.

**Swaps that follow how the code reads the data.** A few formats mix word, half-word and byte fields in ways only the reading code shows, so the swap pass mirrors it:
- **AEMS sound modules.** Each component kind's parameters are swapped as its update function reads them: as words, as two 16-bit halves, or not at all for flag bytes. The game code is unchanged.
- **Sample banks.** These are tag streams parsed a byte at a time, so only the header, the envelopes and the DSP ADPCM coefficients and loop state are swapped.
- **UI screens.** Everything below the script bytecode is a word except the typed records (event and list entries, the two plugin object types, string text). The bytecode is read big-endian a byte at a time and stays as is.

**Formats that mark themselves.** A swapped file changes its magic where one exists: `BNKb` becomes EA's own `BNKl`, `FNTG` becomes `FNTL`, `AE`/`AB` read back reversed. A screen file is recognized by its control count, which reads as `n << 24` while still big-endian.

**Disc quirks found while checking:**
- One music map (`6_1M.mpf`) is an older little-endian version. The game's version check turns it down on both builds.
- Music maps have an empty fade table, so `PATH_fade`'s default curve reads the 4 bytes after the map, on GameCube too.
- One sound bank's random-weight table runs 8 bytes into its sample data. The game stops reading once its running sum passes its roll.

`tests/datacheck` loads every file of the converted formats from the user's disc through the native swaps and relocation, and validates the results.
