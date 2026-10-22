# Architecture decisions

Phase 0 made these calls before any porting starts. Each one records the problem, the decision, what was rejected and why.

| # | Decision | In one line |
|---|---|---|
| [0001](0001-pointers-and-memory.md) | Pointers and memory | All game memory lives in one arena. Pointers inside file structs are 32-bit arena offsets (`PTR32(T)`); everything else is a normal pointer. |
| [0002](0002-threads-interrupts-and-time.md) | Threads, interrupts and time | Real threads, but only one runs at a time, picked by a GameCube-style scheduler. Interrupts arrive at safe points. A 59.94 Hz clock drives game time. |
| [0003](0003-byte-order.md) | Byte order | CPU-read files are little-endian (the swaps turn off). GPU memory stays big-endian, as on hardware, and the Metal layer decodes it. |
| [0004](0004-compiler-rules.md) | Compiler, types and shared-source rules | `FRONTLINE_NATIVE`, `u32` → `uint32_t`, the compiler flags, and the rule that the GameCube build must stay unchanged. |

## Repo shape

```
FrontlineRewritten/
├── FrontlineDecompiled/   submodule: the decomp, the only copy of the game code
├── game/                  CMake target that compiles the decomp's game units with clang
├── apps/macos/            SwiftUI + Metal app shell
├── tools/                 gen_sources.py (unit list), build_report.py (error work list)
└── docs/
    ├── architecture/      these decisions
    └── port/              generated reports (build-errors.md)
```

Phase 1 adds `platform/` (the replacements for the GameCube system libraries) and `renderer/` (GX → Metal).
