# 0004: Compiler, types and shared-source rules

**Status:** accepted (Phase 0, 2026-10-21) · **Covers:** ROADMAP gotchas 2 and 7, plus the conventions for editing shared source

## Editing the shared source

- **The native build defines `FRONTLINE_NATIVE=1`; the GameCube build never does.** Code only the native build needs goes under `#ifdef FRONTLINE_NATIVE`.
- **The GameCube code path must stay byte-for-byte the same.** After any edit in `FrontlineDecompiled/`, rebuild it and run `python3 tools/regress.py`. It has to report **0 dropped, 0 missing**.
- **Prefer a helper in a header over `#ifdef`s in game code.** Examples: `PTR32(T)`, `FromBigEndian()`, `OSSafePoint()`. Each one compiles to nothing, or to the original code, on GameCube.
- **Make portability edits in the submodule** and commit them there (see the README's workflow). This repo only holds code that has no GameCube equivalent: the platform layer, the renderer, the app shells and tools.

## Integer types

`types.h` defines `u32`/`s32` as `unsigned long`/`long`. That's 32 bits on PowerPC but **64 bits on Apple arm64**.

- **Native:** `u32`/`s32` become `uint32_t`/`int32_t`.
- **GameCube:** they stay `unsigned long`/`long`. C++ name mangling depends on them, and so does the match baseline.
- **Format strings:** a `%ld`/`%lu` given a `u32` is fixed where it turns up.

## Compiler flags (game code)

| Flag | Why |
|---|---|
| `-fno-strict-aliasing` | The code type-puns all the time (`*(u32*)&float`, the `RELOCATE` macro). |
| `-fwrapv` | Signed overflow wraps, as on CodeWarrior. |
| `-ffp-contract=off` | No fused multiply-adds. That matches the EA libraries' `-fp_contract off`. The simulation also shouldn't depend on what the optimizer decides to fuse (gotcha 9). |
| `-fsigned-char` | CodeWarrior's `char` is signed. It already is on Apple arm64; the flag just states it. |
| C11 / gnu++14 | Close to what CodeWarrior accepts. Pre-C++17 means `register` and `throw()` specs still compile. Port code in this repo can use newer C++. |

## Known differences from CodeWarrior

- **Class layout.** CodeWarrior places the vtable pointer differently. That only matters for structs laid over file data, and those must not have virtual functions.
- **Bitfield order.** CodeWarrior on PowerPC fills bitfields from the most significant bit, clang on arm64 from the least. That matters only when a bitfield struct is laid over file data or read as an integer. There are 25 to check.
- **Shifts by 32 or more.** PowerPC gives 0; ARM wraps the count. Once the game links (Phase 1), Debug builds turn on `-fsanitize=shift` to find these.
- **Static constructor order** follows link order. The generated source list keeps `configure.py`'s unit order, which is the original link order.
- **Enums** are int-sized on both (CodeWarrior's `-enum int`).
- **Stricter C++:** missing return values, narrowing in braces, string literals assigned to `char*`. These get fixed in the source; they're real bugs either way.

## How the native build finds headers

The decomp's `include/` folder has its own minimal C library headers (`string.h`, `math.h`, `stdio.h`, …). The native build adds that folder with `-idirafter`, so Apple's C library headers win and the game's own headers (`game/`, `ea/`, `dolphin/`, …) are still found. Where the game relies on a CodeWarrior C library function Apple doesn't have (`stricmp`, `__sys_alloc`, …), the platform layer provides it.
