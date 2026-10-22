# 0002: Threads, interrupts and time

**Status:** accepted (Phase 0, 2026-10-21) · **Covers:** ROADMAP gotchas 4–5

## Problem

The GameCube has one CPU core and a strict priority scheduler. The game code relies on that:

- **Threads.** Besides the main thread, EA's code starts three: the timer thread ([timerthread.cpp](../../FrontlineDecompiled/src/ea/sys/timerthread.cpp)), the file-read thread ([filedev.cpp](../../FrontlineDecompiled/src/ea/sys/filedev.cpp)) and the sound driver ([snddrv.c](../../FrontlineDecompiled/src/ea/snd/snddrv.c)). They talk through message queues and `OSLockMutex`.
- **Interrupt callbacks.** These run in between the game's own code: the video retrace (`VISetPreRetraceCallback`), alarms, the audio DSP's frame callback (where EA's mixer runs), and disc-read completion.
- **"Disable interrupts" is the lock.** `OSDisableInterrupts`/`OSRestoreInterrupts` guard shared state in 6 places.
- **Spin-waits.** [Screen.cpp:172](../../FrontlineDecompiled/src/game/Screen.cpp:172) spins on `g_bSwap` until the retrace callback clears it. That only works because an interrupt can preempt the loop.
- **Game time.** It's counted in whole retraces, and the simulation scales by the count `CScreen::Wait()` returns.

On a multi-core Mac, real threads running freely would turn all of this into data races.

## Decision

### Only one game thread runs at a time, and the platform layer picks it

**Scheduling**
- Each `OSThread` is a real host thread (a pthread). That gives it a real stack, so it shows up properly in Xcode, Instruments and the Thread Sanitizer.
- A thread only runs while it holds the **baton**.
- The scheduler follows the GameCube's rules. The highest-priority ready thread runs. It switches only when the running thread:
  - blocks (sleep, message queue, mutex, semaphore, `VIWaitForRetrace`), yields or exits; or
  - is handed a callback at a safe point that wakes a higher-priority thread.
- A switch is a condition-variable handoff, a few microseconds, at most a few dozen times per frame.

**Interrupts**
- Host threads (the clock, the file reader, the audio clock) don't call into the game. They queue "interrupts".
- The running game thread delivers queued interrupts at **safe points**:
  - every call into the platform layer (any `OS*`, `VI*`, `GX*`, `DVD*` or `AX*` function);
  - an explicit `OSSafePoint()`.
- `OSDisableInterrupts` becomes a counter that delays delivery. It's cheap and exactly as strong as on hardware.
- Spin-waits get an explicit safe point. This is a shared-source edit, and the macro is a no-op on GameCube. `Screen.cpp:172` is the known case; any others show up as hangs.

**Who does what on the host**
- **Main thread:** owns windows and input events. The game never runs there. It reads input snapshots that are handed over without locks (`GCController` can be polled from any thread).
- **File reads:** run on host worker threads and come back as disc-completion interrupts. The waiting game thread gives up the baton, so other game threads keep going, as on hardware.
- **Audio:** the Core Audio render callback never takes the baton. It only reads from a lock-free single-producer/single-consumer ring buffer. The audio DSP frame callback runs on the game side, at safe points. It's paced by how full that ring is, aiming to keep 2–3 audio frames buffered.

### Time comes from one 59.94 Hz clock

- The emulated video retrace ticks at NTSC's 60000/1001 Hz. It's derived from `mach_absolute_time`, never from the display's refresh. The game counts whole retraces, so a 120 Hz ProMotion screen can't speed it up (gotcha 5).
- `OSGetTime`/`OSGetTick` count at the GameCube timer's 40.5 MHz (bus clock ÷ 4), from the same clock. The 44 places that convert ticks keep working unchanged.
- Rendering at higher frame rates (Phase 3) means drawing interpolated frames, not running the simulation faster.

## Rejected

- **Free-running threads behind one big mutex.** The host OS would decide who runs next, which breaks the priority assumptions and makes runs unrepeatable. It would still need safe points for interrupts anyway.
- **Hand-rolled fibers on a single host thread (`ucontext`).** Just as deterministic, but `makecontext` is deprecated on macOS, debuggers and sanitizers can't see the fiber stacks, and a blocking host call freezes everything.
- **Making EA's systems thread-safe.** A huge rewrite that drifts away from the shared source.

## Consequences

- Scheduling is deterministic: the same inputs give the same thread interleaving. Input replays and lockstep (Phase 5) depend on that.
- Ordinary tools (Xcode's thread view, TSan, Instruments) work on game threads.
- Code that waits by spinning needs an edit. Hangs make these easy to find, and the macro keeps the GameCube build unchanged.
