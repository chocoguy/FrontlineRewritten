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

**The game runs natively through the first mission's opening** (`apps/headless`, no window yet): intro movies, shell menus, a new game, the level load, the D-Day landing cutscenes and first-person play on Omaha Beach, for 5 minutes without a crash.
- Platform layer: OS (threads, interrupts, alarms, mutexes, messages, heaps, time; checked by `tests/ostest`), DVD (from the disc folder), VI, PAD, AR/ARQ, MTX, and the restart between levels (`LoadElf`, which re-execs the process).
- GX → Metal renderer: CPU vertex stage (matrices, lighting, texgen), TEV ubershader with indirect texturing, GX texture formats and TLUTs, the EFB (color and depth), frame copies to per-frame-buffer textures. Copy clears honor the color and Z update masks (the game clears Z alone before drawing the weapon).
- Stubs: AX runs the mixer and voice callbacks but sends nothing to Core Audio; CARD reports no card.

**Hazards fixed while running the game** (each a pattern worth grepping for in code that hasn't run yet):

| Pattern | Examples |
|---|---|
| Console-sized buffers holding native objects | Mesh objects built into `u8 m_mesh[0x10]`; AI filters placed at a fixed offset in soldier and tank slots; pool element sizes (`0x50`, `0x7C0`, decoder states) |
| Overlays of runtime structs on byte regions | The render and cluster objects inside `CAnimated`; scene-node views (`SSceneNodeView`, the static and anim list links) |
| Structs read by offset from files or by scripts | Trigger message lists, decal types, weapon properties, proximity data, event memory blocks (`AISoundHeardData`, hit info) |
| MWCC layout | `BSGO_Basic`'s vtable pointer comes after its members, so "the first word is the script object" reads need a native accessor |
| Little-endian data swapped by the console code | Weapon tables, poly path points: the swap must be a no-op natively |
| Big-endian fields read before their swap | The `.dmf` format check, `.cpt`/`.cdb` sizes while streaming |
| Integers carried in pointers | Script branch offsets (signed), event data, animation state arguments. Pointers below 4 GB pass through `PTR_TO_ADDR` unchanged, since macOS maps nothing there |
| Tags with default addresses | `NULL + value` in the sound patch parser |

**Tools:**
- `apps/headless`: runs the game with a button and stick script and dumps the displayed frame each second. A crash prints the fault address (and its game address) and a backtrace.
- GX debugging: `FRONTLINE_GX_TRACE=1` prints a line per frame copy (draws, triangles, projection, viewport); `=2` adds full-screen draws. `FRONTLINE_GX_SNAP=<copy>:<draw>,...` with `FRONTLINE_GX_SNAP_DIR` saves the EFB partway through a frame.
- `tools/build_report.py --sizes` lists the structs whose native layout differs (63, all checked for file data, fixed allocations and raw offsets).
- `tests/datacheck` checks the formats above.

## Compiles, but still wrong natively

| Where | Problem |
|---|---|
| Math functions | Apple's libm, not MSL's fdlibm: fine for single player, but lockstep multiplayer across machines may need fdlibm compiled natively (ADR 0004) |

Hazards are found by reading code, so expect more as the game starts running. `build_report.py --sizes` and the patterns in ADR 0001's revisions are the places to look first.

## Next

1. Run the game in the macOS app: present the displayed frame buffer, feed controller input (keyboard/mouse and game controllers).
2. Audio: mix AX voices (DSP ADPCM) to Core Audio. Then CARD saves to a folder.
3. Play further (the rest of the first mission, then the others) and fix what turns up; compare rendering with Dolphin.
4. Extend `tests/datacheck` to the little-endian formats above.
