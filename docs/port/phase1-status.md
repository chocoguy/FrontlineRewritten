# Phase 1 status

Hand-maintained. The generated compile and link work list is [build-errors.md](build-errors.md).

## Done

**Every game unit compiles natively** (381: the game's 379 plus CodeWarrior's `rand.c` and `qsort.c`). The GameCube build is unchanged (`tools/regress.py`: 0 dropped, 0 missing).

**Portability edits in the shared source:**

| Area | What changed |
|---|---|
| Integer types | `u32` is `uint32_t` natively |
| Pointers in 32-bit slots | 32-bit address map, `PTR32` fields, `PTR_TO_ADDR`/`ADDR_TO_PTR` on about 700 casts. Every 32-bit value round-trips (numbers passed as pointers too) |
| Layout checks | Split into GameCube-only and checked-on-both (`FILE_SIZE_ASSERT`) |
| GameCube-sized allocations | `GC_SIZE(literal, native)` where runtime structs are allocated or cleared with literal sizes; `PTR32` where the layout itself is fixed |
| Raw offsets into objects | Field accesses instead (`m_pScriptObject`, `m_bones`, ...); `GCVolBox()` for boxes stored with the GameCube layout |
| Assembly | The paired-single matrix routines and the movie IDCT are rewritten in C |
| C++ strictness | Missing return values, specialization order, callback types |
| C library | CodeWarrior's `rand()` and `qsort()` compiled natively (`MSL_rand`, `MSL_qsort`) |

**File formats converted and checked against the disc** (`tests/datacheck`):

| Format | Byte order | Disc data checked |
|---|---|---|
| `.cdb` collision | big | 247 files, 1.12 M triangles |
| `.cpt` level geometry | big | 247 files, 190 k chunks, 30 k materials, 9 k shapes |
| `.msh`/`.mpk` static and animated meshes | big | 2,539 meshes |
| `.dmf` skinned meshes | big | 558 meshes, 2.3 M display-list vertices walked big-endian |
| `.aem` sound event systems | big | 28 systems, 476 events |
| `.abk` module banks | big | 301 banks, 522 modules, 23 k components |
| `BNKb` sample banks (inside `.abk`) | big | 282 banks, 12 k patches, 4,681 ADPCM channels |
| `.scr` UI screens | big (bytecode: bytes) | 71 screens, 1,555 controls, 2,871 plugin objects |
| `.gfn` fonts, and the built-in font | big | 234 fonts, 44 k characters, 21 k kern pairs |
| `.mpf` music maps | big | 26 maps, 522 nodes (one older little-endian map, which the game turns down) |

**Converted but not yet in the checker** (all little-endian, so the risk is layout, not byte order):
- `.emt` animations
- `.skl` skeletons
- `.lfc` level resources
- `.bpd` level objects
- `.som` sound tables
- `.tpk` texture packs
- `.cbs`/`.sin` scripts
- `bsbifunc.dat` built-ins
- `weapbul_*.dat` bullet tables
- path nodes

**The game runs natively into every mission**, in the macOS app's window and in `apps/headless`: intro movies, shell menus, a new game, the level load, the D-Day landing cutscenes and first-person play on Omaha Beach for 5 minutes, and (with the BALLOWAX password, which unlocks every level) the first stage of each of the other five missions, D-Day's Into the Breach, and the second stages of missions 2 and 3, for 3 minutes each, with enemies and allies spawning, without a crash. Headless play is a scripted walk and a few shots, so most of each level hasn't run yet.
- macOS app: a SwiftUI window with a Metal view showing the displayed frame buffer at 4:3 (a frame GX copied, or the movie player's YUV, told apart by a stamp each GX copy writes into the frame buffer's memory), keyboard input through `GCKeyboard`, game controllers through `GCController` (with rumble), and the disc folder picked on first launch. Swift reaches the game through `apps/macos/Bridge`.
- Platform layer: OS (threads, interrupts, alarms, mutexes, messages, heaps, time; checked by `tests/ostest`), DVD (from the disc folder), VI, PAD, AR/ARQ, MTX, and the restart between levels (`LoadElf`, which re-execs the process). Interrupts are queued under their own lock, not the scheduler's: macOS mutexes aren't fair, so a game thread polling in a tight loop (the shell's fade-out before a mission) took the scheduler lock so often that the host threads raising interrupts rarely got it, and the fade ran for minutes until the queue overflowed.
- GX → Metal renderer: CPU vertex stage (matrices, lighting, texgen), TEV ubershader with indirect texturing, GX texture formats and TLUTs, the EFB (color and depth), frame copies to per-frame-buffer textures. Copy clears honor the color and Z update masks (the game clears Z alone before drawing the weapon). Fog follows the hardware equation on each pixel's screen depth (`ze = A / (B - z)` from `GXSetFog`'s packed constants), not eye distance: the game leaves fog on for 2D, which the console leaves clear because it sits at the front of the depth range, so the eye-distance version tinted letterboxes, subtitles, the death screen and a disc over the compass with the fog color.
- Sound: AX is mixed on the CPU (`platform/src/audio.cpp`): DSP ADPCM, PCM16 and PCM8 voices from ARAM, linear resampling, the volume envelope and per-bus ramps, and the aux A round trip through EA's CPU mixer (streamed music and movie audio, reverb). The mix goes to Core Audio's default output through a buffer that keeps about 45 ms queued (`audio_out.cpp`). Movies, shell music, menu sounds and the D-Day level all play.
- Memory cards (`platform/src/card.cpp`): each slot is a folder of `.gci` files (Dolphin's GCI folder format: the SDK's 64-byte directory entry, big-endian, then the blocks), acting as a 251-block card. CARD follows the SDK's rules: access by the disc's game and maker codes (`DVDGetCurrentDiskID` reads `sys/boot.bin`), public files readable by others, read and write alignment, the icon offsets in `CARDStat`, and timestamps in seconds since 2000 (local time). Each change rewrites the file through a temporary and a rename. The save game is written in the console's byte order with its CRC over those bytes (`SwapShellSave` in `ShellMenu.cpp`), so saves move between this port, Dolphin and a GameCube. The app puts slot A in Application Support; headless has no card unless `FRONTLINE_CARD_A`/`_B` is set.

**Hazards fixed while running the game** (each a pattern worth grepping for in code that hasn't run yet):

| Pattern | Examples |
|---|---|
| Console-sized buffers holding native objects | Mesh objects built into `u8 m_mesh[0x10]`; AI filters placed at a fixed offset in soldier and tank slots; pool element sizes (`0x50`, `0x7C0`, decoder states); the sound voice array (`numVoices << 7`: a shift, which a search for size literals misses) and the 100 Hz server's walk over it (`offset += 0x80`, which left fades and envelopes stuck, so the shell music's fade-out never ended and no mission could start); the reverb's filter state sizes; the script instance pools, whose element sizes count a script thread as 0x20 bytes (a class's threads overflowed into the next instance as soon as a level spawned many script objects); the light lists the compartment draw copies onto the scratch stack (`size * 4` bytes for 8-byte pointers: a child node's list overwrote half of its parent's entry, so with one light in view the next sibling read a pointer made of two copies of its low word, which crashed Clipping Their Wings); and the 16 KB scratch pad itself, which holds pointers for all three of its users (now 32 KB natively) |
| Overlays of runtime structs on byte regions | The render and cluster objects inside `CAnimated`; scene-node views (`SSceneNodeView`, the static and anim list links) |
| Structs read by offset from files or by scripts | Trigger message lists, decal types, weapon properties, proximity data, event data (`AISoundHeardData`, hit info, the grenade warning and collision damage events: a native pointer at word 0 moved every later field, so a soldier warned of a grenade read the thrower pointer's high half as the grenade and crashed). Scripts read event data with `LM` at console word offsets, so every struct passed as event data needs `PTR32` fields |
| MWCC layout | `BSGO_Basic`'s vtable pointer comes after its members, so "the first word is the script object" reads need a native accessor |
| Little-endian data swapped by the console code | Weapon tables, poly path points: the swap must be a no-op natively |
| Big-endian fields read before their swap | The `.dmf` format check, `.cpt`/`.cdb` sizes while streaming |
| A second view of a file struct | `SClsnPoly` (a collision contact's view of a `CDBTri`) kept the console's bitfield order after `CDBTri` got the native one, so every surface type read wrong: shoot-through surfaces stopped bullets (bunker guns hit their own firing slits, so nothing hit the player), and impact effects and footstep sounds used the wrong surface. Grep for every type that casts to the same memory |
| Console byte offsets handed to scripts | `AIGetSoldierMatchListStruct` gives scripts the address of the match-list block in `CAIDoodad`, which they read and write at console offsets. Its four buffer pointers took 8 bytes natively and moved the fields after them, so a script stored through a null slot when allied soldiers spawned. The pointers are now `PTR32`, and an assert pins the block's span. The built-ins that take a match list (`AISetupTargetMatchList`, `RegisterTargetSearch`) get the address of one of those 32-bit slots, so they read it as a 32-bit address, not a native pointer |
| Varargs called with packed words | `UISStringFormat` passed its parameters to `sprintf` as an array of 32-bit words, which only works with the console's calling convention; natively it formats each conversion with a typed argument |
| Swapped words a string ends in | The `.scr` swap left each UI string's `maxLength` bytes alone but not its terminator, so strings whose length is a multiple of 4 (and empty ones) gained "ÿÿÿ" ("BACKÿÿÿ" on the password screen). `tests/datacheck` now checks every string is still terminated |
| A second view of a runtime struct | Scene nodes wrote their bounding extents through `CSceneNodeData::SExtentLink` (two pointers, then the value), while the scene hands out `ISceneNode::SExtent` entries (a pointer, a byte, then the value). The value is at +8 in both on the console but not natively, so every extent stayed 0: every node overlapped every other in the broad phase. Collisions still worked (the narrow phase is exact), but crowded levels (Into the Breach, with script-fired tracers) overflowed the scene's 0x800-pair lists, which nothing checks, and crashed. `SExtentLink` is now the `SExtent` it points to |
| Reads past a buffer that the console tolerates | The memory card header writes send two sectors from a one-sector buffer (stack or static); on the console the extra sector is harmless leftovers, natively the read ran off the top of the game thread's stack. The buffers cover both sectors natively |
| Big-endian data read in place | Stream chunk fields, PCM16 samples and XA packet state (read big-endian natively, not swapped) |
| Integers carried in pointers | Script branch offsets (signed), event data, animation state arguments. Pointers below 4 GB pass through `PTR_TO_ADDR` unchanged, since macOS maps nothing there |
| `unsigned long` holding a 32-bit id | `unsigned long` is 32 bits on the GameCube but 64 natively, and the headers keep it wherever the retail symbols have it. The light and particle lookups by id (`CLight::Create`, `CParticleSystem::Create`) took one, and the script built-ins pass them a signed `int`, so an id with the top bit set sign-extended to `0xFFFFFFFF........` and never matched the table's 32-bit id. An alarm's flashing light came back NULL and crashed as soon as a soldier raised the alarm (The Golden Lion, Eye of the Storm, A Chance Meeting), and 646 of the 1,620 particle effect calls in the levels' scripts never appeared. They take `u32` now, which is `unsigned long` on the GameCube, so nothing changes there. Grep for `unsigned long` parameters given an `int` and compared with a `u32` |
| Tags with default addresses | `NULL + value` in the sound patch parser |

**Tools:**
- `apps/headless`: runs the game with a button and stick script and dumps the displayed frame each second. A crash prints the fault address (and its game address) and a backtrace.
- Sound: `FRONTLINE_AUDIO_DUMP=<file.wav>` records the mix (headless too, which plays nothing); `FRONTLINE_AUDIO_TRACE=1` prints each DSP voice start and the output buffer's state; `FRONTLINE_NO_AUDIO=1` keeps the app silent.
- `FRONTLINE_INPUT="3:A,15:START+1"` scripts controller 1 (seconds after start, `+` holds) in the app as well as in headless. `FRONTLINE_DISPLAY_DUMP=<folder>` saves what the app's window shows each second (there's no screen capture from the shell, and synthetic key events are blocked).
- GX debugging: `FRONTLINE_GX_TRACE=1` prints a line per frame copy (draws, triangles, projection, viewport); `=2` adds full-screen draws. `FRONTLINE_GX_SNAP=<copy>:<draw>,...` with `FRONTLINE_GX_SNAP_DIR` saves the EFB partway through a frame.
- `tools/build_report.py --sizes` lists the structs whose native layout differs (63, all checked for file data, fixed allocations and raw offsets).
- `tests/datacheck` checks the formats above.

## Compiles, but still wrong natively

| Where | Problem |
|---|---|
| Math functions | Apple's libm, not MSL's fdlibm: fine for single player, but lockstep multiplayer across machines may need fdlibm compiled natively (ADR 0004) |

Hazards are found by reading code, so expect more as the game starts running. `build_report.py --sizes` and the patterns in ADR 0001's revisions are the places to look first.

## Next

1. ~~Mouse look for keyboard play~~: done in Phase 2 ([phase2-status.md](phase2-status.md)).
2. Play further (whole levels, every stage) and fix what turns up; compare rendering with Dolphin.
3. Extend `tests/datacheck` to the little-endian formats above.
4. Sound polish: the mixer's 5 ms clock and the output device's drift apart slowly; the buffer covers it with a short gap or drop now and then (none seen in 75 s). The DSP's polyphase resampling is approximated as linear, and the surround and aux B buses aren't output (the game uses stereo and aux A). The mix overloads (hard clipping) when loud effects land on top of music and dialogue, on the beach in D-Day; that is the game's own mix summed as the DSP does, so Classic keeps it, and a soft limiter is planned for Rewritten mode (ROADMAP, Phase 2).
