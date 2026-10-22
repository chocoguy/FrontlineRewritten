# 0003: Byte order

**Status:** accepted (Phase 0, 2026-10-21) · **Covers:** ROADMAP gotcha 3

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
