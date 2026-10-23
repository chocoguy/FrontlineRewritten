# 0001: Pointers and memory

**Status:** accepted (Phase 0, 2026-10-21); revised in Phase 1 (see the end) · **Covers:** ROADMAP gotcha 1

## Problem

The game was written for a 32-bit machine, and it shows in three places:

- **Pointers inside loaded files.** Loaders read a file into a heap buffer and turn 4-byte offsets into pointers in place: `RELOCATE` in [animemot.cpp](../../FrontlineDecompiled/src/game/animemot.cpp), `offsetPtr` in [offsetptr.h](../../FrontlineDecompiled/include/game/offsetptr.h) (casts through `int`), plus the skeleton, sound-bank and script loaders. A native pointer is 8 bytes and doesn't fit.
- **Pointers in integers.** There are about 330 `(u32)` casts and a script VM whose stack slots are 32 bits wide.
- **Layout checks.** 268 `SIZE_ASSERT`/`OFFSET_ASSERT`s pin struct layouts to the GameCube's sizes.

The first native build shows this directly: 267 layout-assert errors and 135 "pointer truncated" errors ([build-errors.md](../port/build-errors.md)).

Apple has no 32-bit ABI on macOS, iOS or tvOS (`arm64_32` is watchOS only). The low 4 GB of address space is `__PAGEZERO`, and there is no `MAP_32BIT`, so the game's memory can't be parked where pointers fit in 32 bits.

## Decision

### 1. One arena holds all game memory

- At startup the platform layer reserves a single contiguous region: **256 MB by default**, configurable. It's reserved with `mmap`, and pages are only committed when touched.
- `OSGetArenaLo`/`OSGetArenaHi` return its bounds. EA's allocator (`initosalloc.cpp`) and `MallocInit.cpp` already take all their heaps from there, so every game heap ends up inside the arena without changing the game code.
- The first 64 KB of the arena stays inaccessible (`PROT_NONE`). Offset 0 is therefore never a valid object, and null encodes as 0.
- The arena never moves during a run. Offsets into it are stable, and Phase 5 can snapshot it whole for rollback.
- **ARAM** (the 16 MB of audio RAM) is a separate host buffer. Its addresses are already plain integers on the GameCube, so nothing changes.
- The arena is about 10× the GameCube's 24 MB of main RAM, because native structs are bigger. Fixed memory budgets in the game code get scaled as Phase 1 finds them.

### 2. Two kinds of pointer

**File-layout pointers** are fields of structs that get laid over loaded file data. They're declared `PTR32(T)`:

| Build | `PTR32(T)` is | Size |
|---|---|---|
| GameCube | `T*` (the macro expands to the raw pointer, so codegen is identical) | 4 |
| Native | `ptr32<T>`: a 32-bit offset from the arena base, which converts to and from `T*` | 4 |

- These structs keep exactly the same layout on both builds. They use a `FILE_SIZE_ASSERT`, which is checked on both, so a mistake fails to compile.
- `RELOCATE` and `offsetPtr` become "file offset + the buffer's arena offset" natively.
- Encoding a pointer that lies outside the arena traps in debug builds.

**Every other pointer** is a normal native pointer. Structs created only at runtime (never overlaid on a file) are free to grow. Their `SIZE_ASSERT`/`OFFSET_ASSERT` checks stay active only in the GameCube build.

### 3. Pointers in integers, case by case

| What the code does | Native fix |
|---|---|
| Pointer arithmetic or alignment through an integer | `uintptr_t` |
| Pointer stored in a 32-bit slot that must stay 32 bits (script stack, file fields) | Arena-offset encoding via `PtrToSlot`/`SlotToPtr` (plain casts on GameCube) |
| A pointer to non-arena memory (a global, a string literal, a function) in a 32-bit slot | Widen the slot, or register it in a small handle table. The debug trap finds these. |

### 4. Fixed addresses

A few reads go straight to the GameCube's low-memory OS globals: `OS_BUS_CLOCK`/`OS_CORE_CLOCK` (`*(u32*)0x800000F8`), and an inline copy of it in `animemot.cpp`. Natively those macros return constants (162 MHz bus, 486 MHz core).

## Rejected

- **Allocating in the low 4 GB.** Not possible on Apple platforms (see above).
- **Widening every file struct and converting at load.** Every loader would need a second "native" struct definition and a conversion pass. We'd also lose the check that both builds agree on file layouts.
- **Compressing every pointer.** Far too invasive. Only the file-layout pointers need it.

## Consequences

- Phase 1's portability pass has a mechanical rule for each error bucket.
- `ptr32<T>` costs an add on every dereference. That only applies to data read from files, and it's cheap.
- An arena snapshot is a single `memcpy`. That makes rollback (Phase 5) and save states practical.

## Revisions from Phase 1

The plan above held up. Implementing it changed four details:

- **32-bit values are GameCube-style addresses, not plain arena offsets.**
  - `[0x10000, arena size)` is the arena.
  - `[0x80000000, …)` is the executable image (statics, string literals, functions), like the real console's address map.
  - So `(u32)ptr` round-trips for heap and static data alike, and alignment tests still work.
  - The helpers are `PTR_TO_ADDR(p)` and `ADDR_TO_PTR(T, a)` (plain casts on GameCube), plus `PTR32_RAW(f)` for a field's stored value. They're in `types.h` and `native/address.h`, and implemented in `platform/src/address.cpp`.
- **Sometimes the slot is widened instead.** A runtime struct's 32-bit slot that only ever holds a pointer becomes `uptr` (still `unsigned long` on GameCube). Example: the draw context's fog pointer, which points at a stack object.
- **Objects built inside file data get native side objects.** Some loaders construct C++ objects with vtables over file records: compartment and static-mesh materials, and skinned-mesh parts. Natively those objects are bigger than the records, so the native loader builds them separately from the record fields and points the file's references at them.
- **The game's `operator new`/`delete` stay inside the game.** They're unexported at link time, so Apple's frameworks keep the system allocator. Non-game threads get `calloc`/`free`.
- **Every 32-bit value round-trips, not just addresses.** The game also passes plain numbers where a pointer goes: a float's bits as script event data, an ARAM address as a `void*`. So the arena sits at the bottom of a reserved 4 GB window, and any value outside the image maps into the rest of the window. `ADDR_TO_PTR` then `PTR_TO_ADDR` gives back any value; dereferencing one outside the arena faults. A real pointer outside the window and the image (a stack address, a host allocation) still traps when converted. The other direction holds too: a pointer-typed value below 4 GB (an integer the game put in a `void*`, such as event data 1) converts to its own value, since macOS maps nothing there.
- **Runtime structs with GameCube-sized allocations get one of two fixes.** Some code allocates or clears a struct with a literal size (`SNDMEMI_allocz(0x3C)`), or embeds one in a fixed-size header.
  - If the struct is only used at run time, the literal becomes `GC_SIZE(0x3C, sizeof(RSFSTATE))` from `types.h`: the literal on GameCube, the native expression natively.
  - If the layout itself is fixed (sizes come from tables, or headers sit at fixed offsets: the AEMS sound runtime, EA's pool allocators, the sound memory header), its pointer fields become `PTR32` so the GameCube layout holds natively.
  - `tools/build_report.py --sizes` lists every struct whose `SIZE_ASSERT` fails natively, as the list to check.
- **Raw offsets into objects become field accesses.** For example `*(BSObject**)((u8*)this + 0x250)` becomes `m_pScriptObject`. The GameCube code is unchanged (`regress.py`), and natively the offsets follow the 8-byte vtable pointers and pointer fields.
- **Data viewed with a GameCube object layout is copied.** Location collision boxes and a static mesh's root node are `CVolBox` images in file data, with a 4-byte vtable slot. Natively, `GCVolBox()` in `box.h` builds a real `CVolBox` from those bytes.
- **Console-sized space that holds native objects is sized natively.** Objects are placement-built into byte arrays sized for the console (a mesh in a 16-byte template slot), at fixed offsets inside other objects (a soldier's AI filter at `+0x44B0`), or in pools whose element sizes are literals. Natively the buffer, offset or pool size becomes `sizeof` the real object (`GC_SIZE`, or an `#ifdef` with the size), and the slot that holds both grows to fit.
- **MWCC can put a class's vtable pointer after data members.** `BSGO_Basic`'s is at `0xC`, after its three members; clang always puts it first. Code that reads "the first word of the object" (`BSGO_Basic`'s script object) gets a native accessor.
