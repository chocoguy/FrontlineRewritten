# Frontline: Rewritten — Roadmap

A native port of Medal of Honor: Frontline (GameCube, `GMFE69`) to macOS, iOS/iPadOS and tvOS.

**Goals**
- Native Metal rendering
- Enhancements and upgrades over the original (not an exact port)
- A mod/sandbox system with lots of cheats
- More robust multiplayer: AI opponents and LAN play

**MVP:** a simple port with Metal rendering.

The game code in `FrontlineDecompiled/` is already finished: all 558 units build from source as functionally equivalent C/C++. So this roadmap doesn't start with decompiling. It starts with making that code run on 64-bit, little-endian Apple hardware.

## What the code tells us

- **Not much talks to the hardware.** The game calls about 200 GameCube system-library functions: 83 graphics (GX), 43 OS, 21 memory card, 12 audio, plus a handful for video, disc, controllers and audio RAM. Most of the drawing code sits in about 14 files (`compartment`, `staticmesh`, `dmesh`, `sprite`, `font`, `particlesystem`, `skybox`, `decal`, …) plus the movie player.
- **The hardest graphics feature is never used.** The game never copies the framebuffer into a texture (`GXCopyTex`), which is usually the painful part of translating GameCube graphics.
- **The data files are little-endian.** That's left over from the PS2 original. The GameCube build byte-swaps them on load ([endian.h](FrontlineDecompiled/include/game/endian.h)); on Apple hardware those swaps just get switched off.
- **Very little assembly:** paired-single math in [matrix.cpp](FrontlineDecompiled/src/game/matrix.cpp) and the movie IDCT (`gc_idct.asm`).
- **Hooks for later phases already exist:**
  - 2–4 player split-screen multiplayer.
  - A cheat system, `CCheats`: Bullet Shield, Rubber Grenades, Silver Bullet, Achilles' Head, Invisible Enemies, infinite ammo, and more.
  - A script interpreter that runs level logic from compiled script files (`.cbs`) inside each level archive, with built-in functions looked up by a hash of their name.
- **Audio uses Nintendo's standard audio library (AX)**, so there's no custom audio-chip code to reverse-engineer. Replacing it means writing a small software mixer.

## Phases

Effort is relative: S, M, L, XL.

### Phase 0 — Groundwork (S)
- **Repo shape.** Keep the decomp as the single source of game code (pulled in as a submodule). This repo then adds the platform layer, the renderer, the app shells and tools.
  - Make every portability edit in the shared source, and check it against the GameCube build with `tools/regress.py`.
  - On GameCube a pointer-sized integer is the same 32 bits as before, so a correct fix leaves the match baseline unchanged. That safety net is the project's biggest asset; keep the GameCube build alive.
- **Native build.** Use CMake generating Xcode projects, arm64 only. Compile the game code with clang against stub headers; the list of build errors becomes the real work list.
- **Make three architecture calls up front:** how to handle 64-bit pointers, the threading model, and the endianness rules (gotchas 1–4).
- **Status: done (2026-10-21).**
  - The decomp is a submodule, and the GameCube reference build passes `tools/regress.py`.
  - The CMake build has `xcode` and `ninja` presets for arm64 on the macOS 27 SDK.
  - The macOS app shell opens a Metal window.
  - The first native compile: 141 of 379 units compile, with 490 unique errors ([work list](docs/port/build-errors.md)).
  - Decisions: [docs/architecture/](docs/architecture/README.md).

### Phase 1 — MVP: macOS with Metal (XL, the critical path)
1. **Portability pass:**
   - per-platform integer types
   - pointer casts → `uintptr_t`
   - pointer fix-ups in loaded files (`RELOCATE`, `offsetPtr`) → 32-bit offsets into one memory pool ("arena")
   - assembly → C
   - drop the boot loader, disc-error screen and debugger units
2. **Replacements for the GameCube system libraries:**
   - **Memory and threads:** one big memory arena holds all the game's heaps; threads and timers.
   - **Disc:** read from the extracted disc folder.
   - **Video timing:** an emulated 60 Hz vertical-retrace signal.
   - **Controllers:** Apple's GameController framework.
   - **Saves:** plain files.
   - **Audio:** a software mixer that decodes the GameCube's ADPCM format, resamples, and feeds Core Audio through a lock-free buffer.
3. **Graphics translated to Metal**, limited to the 83 functions the game calls:
   - **Geometry:** decode vertex formats, and convert display lists into cached Metal buffers.
   - **Shaders:** generate Metal shaders from the GameCube's colour-combiner settings (TEV).
   - **Fixed-function features:** up to 3 hardware lights per object, fog, alpha test, and palette and compressed textures.
   - **Resolution:** render scaling.
   - **Movies:** give the movie colour conversion its own shader. It's the only user of indirect texturing.
   - **Reference:** use [aurora](https://github.com/encounter/aurora) (MIT-licensed, the GameCube graphics layer behind Metaforce and Dusklight) as the guide to how GX behaves. Skip its WebGPU/Dawn dependency, since the goal is native Metal.
4. **macOS app shell:** SwiftUI with a Metal layer, a disc-folder picker with a hash check, and settings.
- **Done when:** every mission can be finished, split-screen works, and side-by-side screenshots against Dolphin running the GameCube build look clean.

### Phase 2 — iPhone, iPad and Apple TV (M)
- Same game code, new app shells.
- **Controls:** touch controls and gyro on iPhone; game controllers everywhere.
- **Getting the game data on:** import it through the Files app on iOS. On tvOS, bundle the ~1.3 GB of disc data into the app at build time.
- **System integration:** frame pacing with `CAMetalDisplayLink`, handling interruptions and backgrounding, safe areas and TV overscan, and MetalFX (Apple's upscaler) where available.

### Phase 3 — Enhancements (L)
- **Widescreen:** done in `CCamera` so the field of view, culling, weapon model and HUD all agree.
- **Image quality:** native resolution, anti-aliasing (MSAA), anisotropic filtering, MetalFX.
- **Frame rate:** locked 60 fps, then 120 Hz on ProMotion screens.
- **Controls:** remapping and gyro aim. Real mouse aim feeds camera rotation directly instead of faking stick input.
- **Audio:** 48 kHz and spatial audio.
- **Textures:** HD texture replacement. Using Dolphin's texture-hash naming means existing Dolphin packs would load.
- **Visuals and quality of life:** longer draw distance, skippable intros, checkpoint restart.
- **Optional:** move the world and character rendering off the GameCube-style path onto a modern Metal renderer, for per-pixel lighting and shadows.

### Phase 4 — Mods and sandbox (L)
- **Cheat and sandbox menu:**
  - all the existing `CCheats`
  - god mode, noclip, free camera and photo mode
  - time scale and low gravity
  - big heads and one-hit kills
  - spawn soldiers, tanks or weapons at the crosshair
  - turn AI factions on or off, level and checkpoint select
  - a developer console
- **File overrides:** mods replace entries in the `.viv` archives by name (hook EA's file system), with load order and a manifest plus hash.
- **Scripting, two layers:**
  - Lua for engine-level mods.
  - A disassembler/assembler for the game's own script bytecode, built from the decompiled interpreter. That lets mods write mission logic in the original scripting system and add new built-in functions.
- **Tools and delivery:** glTF/PNG export and import, and a level viewer. Mods reach iOS through Files and Apple TV through a push over the local network.

### Phase 5 — Multiplayer (XL)
- **Cheap groundwork during Phases 1–3:**
  - Send all player input through one per-tick command struct.
  - Add a fixed-tick simulation mode.
  - Add input-only replays that hash the game state every tick. Replays test determinism without any networking.
- **LAN using deterministic lockstep, built on the existing split-screen simulation.** Each machine simulates every player but draws only its own view full-screen, and only inputs travel over the network.
  - Discovery and lobby use Bonjour, with a check that everyone has the same build and mods.
  - Raise the 4-player limit.
- **Bots:** reuse `AISoldierObject` if the multiplayer maps kept the single-player path and cover nodes. Otherwise generate a navigation mesh with Recast/Detour. In lockstep, bots use no bandwidth at all.
- **Co-op campaign:** a separate milestone, because the single-player scripts assume one player.
- **Internet play:** a stretch goal. Rollback is feasible by snapshotting the memory arena. A full client–server rewrite is only worth it if people need to join mid-game over the internet.

## Gotchas

### These will hit during the MVP
1. **64-bit pointers stored in 32-bit file fields.**
   - **Where:** loaders write pointers into 4-byte fields in place: [animemot.cpp:410](FrontlineDecompiled/src/game/animemot.cpp:410), `offsetPtr` casting through `int`, the skeleton and sound-bank fix-ups, and the script stack's 32-bit slots. There are also 330 `(u32)` casts and 264 struct-layout asserts.
   - **No shortcut:** Apple has no 32-bit ABI on macOS, iOS or tvOS, and the bottom 4 GB of address space is reserved, so the heap can't be parked low.
   - **Plan:**
     - One arena holds all game memory.
     - Structs loaded from files store 32-bit offsets into it behind a `ptr32<T>` wrapper, which is just a plain pointer in the GameCube build.
     - Everything else uses normal pointers and drops its layout asserts.
2. **`long` is 64-bit on Apple arm64.** [types.h](FrontlineDecompiled/include/types.h) defines `u32` as `unsigned long`. Use `uint32_t` in the native build, but keep `unsigned long` on GameCube so C++ name mangling still matches. This also breaks `%ld` format strings.
3. **Mixed byte order.** Data the CPU reads is little-endian, so the GameCube swaps simply turn off. But anything the graphics chip reads directly (display lists, vertex arrays, textures, palettes) is big-endian and has to be swapped by the Metal layer. So does any GameCube-only data the CPU reads without a swap.
4. **Code written for a single CPU core.** EA's system and sound code uses threads, timers and "disable interrupts" as its lock, on a one-core machine with strict priorities. On a multi-core Mac those become data races. The fix:
   - Let only one game thread run at a time, under a single global lock.
   - Deliver the timer and audio callbacks at safe points.
   - Keep the Core Audio thread away from that lock.
   - Fix spin-waits like [Screen.cpp:172](FrontlineDecompiled/src/game/Screen.cpp:172).
5. **Game time is counted in whole 60 Hz frames.** `CScreen::Wait()` returns a whole number of retraces, and the entire simulation scales by it. Drive that signal from a 60 Hz clock, not the screen refresh, or a 120 Hz ProMotion display runs the game at double speed.
6. **Graphics accuracy is death by a thousand cuts.** Colour-combiner maths, light falloff and fixed-point vertex formats all produce "almost right" bugs. Keep Dolphin running as the visual reference, and compile shaders ahead of time from a recorded list so they don't stutter on first use.
7. **Compiler and ABI differences:**
   - Class layout differs from CodeWarrior's.
   - Static constructors run in link order.
   - Shifting by 32 or more gives 0 on PowerPC but wraps on ARM.
   - Bitfield bit order is reversed (25 bitfields to check).
   - Build with `-fno-strict-aliasing -fwrapv`.

### These will hit later
8. **Widescreen culling.** `CCamera::ASPECT_4x3` also feeds frustum culling and the weapon model. Widening only the projection makes objects pop in at the screen edges.
9. **Lockstep needs bit-identical simulation on every machine:**
   - Everyone runs the same build ID.
   - Game maths goes through the decomp's own maths library, not Apple's.
   - FP contraction is off for simulation code.
   - No simulation work happens inside drawing code.
   - The RNG gets a shared seed (`Moh2.cpp` seeds it from `OSGetTick()`).
10. **Texture compression.** The GameCube's compressed textures map to BC1, but BC formats only exist on Macs and recent iPad/iPhone GPUs, not Apple TV or most iPhones. Decode to RGBA8; the textures are tiny.
11. **Local networking on iOS/tvOS.** The local-network permission prompt is mandatory. UDP broadcast needs an entitlement Apple restricts, so use Bonjour with declared service types.

### Platform and legal
12. **No App Store.** EA owns the IP and the trademark, and the code is derived from their binary.
    - Distribute source only; people build it with their own disc.
    - iOS and tvOS installs signed with a free Apple account expire after 7 days; a paid account lasts a year.
    - Keep "Medal of Honor" and EA art out of app names and icons.
13. **Apple TV storage.** There's no Files app, only about 500 KB of guaranteed persistent storage, and caches can be purged. Put the game data inside the app bundle and keep saves small, or in iCloud.
14. **Keep EA's data out of git.** `FrontlineDecompiled/` has no `.gitignore` of its own, and `orig/` holds 1.3 GB of disc data. Add ignores before it becomes a repo or submodule.

## Open decisions
1. ~~**Repo shape**~~ **Decided:** the decomp is a submodule, and the GameCube build stays as the reference.
2. ~~**MVP scope**~~ **Decided:** macOS first, targeting macOS 27.
3. **"Play against AI":** deathmatch bots, co-op campaign, or both. This sets the order within Phase 5.
4. **Internet play:** if it's a must-have someday, revisit lockstep vs. client–server before Phase 5.
5. **Device floor:** keep the core renderer on baseline Metal features so older Apple TVs work, and switch on extras like MetalFX only where the device supports them.

## References
- [aurora (GitHub)](https://github.com/encounter/aurora)
- [Metal feature set tables](https://developer.apple.com/metal/feature-sets)
- [WebKit bug on BC texture support on iOS](https://bugs.webkit.org/show_bug.cgi?id=295566)
- [tvOS local storage limits (Apple forums)](https://developer.apple.com/forums/thread/16967)
- [Local Network Privacy FAQ](https://developer.apple.com/forums/thread/663875)
- [Multicast entitlement](https://developer.apple.com/forums/thread/655920)
