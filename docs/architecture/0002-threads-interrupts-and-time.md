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
- Rendering at higher frame rates (Phase 2) means drawing interpolated frames, not running the simulation faster.

## Rejected

- **Free-running threads behind one big mutex.** The host OS would decide who runs next, which breaks the priority assumptions and makes runs unrepeatable. It would still need safe points for interrupts anyway.
- **Hand-rolled fibers on a single host thread (`ucontext`).** Just as deterministic, but `makecontext` is deprecated on macOS, debuggers and sanitizers can't see the fiber stacks, and a blocking host call freezes everything.
- **Making EA's systems thread-safe.** A huge rewrite that drifts away from the shared source.

## Revisions from Phase 1

Implemented in `platform/src/os/` as planned, with these details:
- **Native thread state lives in the `OSThread`'s register context**, which nothing uses natively. The scheduler never allocates: on game threads, `operator new` belongs to the game.
- **Game threads run on host stacks carved from the arena** (2 MB each, a 32 MB pool at its top). Pointers to locals then have 32-bit addresses, which the game stores (event data, message queues). The game's own 4–8 KB stack buffers go unused.
- **The OS structs keep their console sizes.** EA's mutex is an `OSMutex` in a 28-byte buffer, so the pointers in `OSThread`, `OSMutex`, `OSMessageQueue` and the thread queues are `PTR32` natively.
- **Static initializers run on the default thread.** The game's run on the host's main thread before `main()`, and they allocate. The first OS call adopts that thread as the console's default thread, and `frontline::StartGame` then moves it to a host thread of its own.
- **When every thread waits, the last one to block idles.** It waits for an interrupt and delivers it (handlers run with no current thread, as on the console), until a thread is ready.
- **Spin-waits use `SPIN_WAIT()`** (`types.h`): it sleeps until an interrupt is raised and delivers it, keeping the baton as the spinning CPU would. Found so far: `CScreen::Wait`, `FILESYS_waitop`, the one-shot sound bank wait, the animation DMA wait.
- **Retraces, alarms, disc reads and audio frames each have a host thread** that only raises interrupts.
- `tests/ostest` checks the scheduler: priorities, preemption, blocking queues, mutex priority inheritance, alarms while idle, and delivery held back while interrupts are disabled. It's clean under the Thread Sanitizer.

## Revisions from Phase 2

- **The display runs on its own clock (2026-10-27).** The app presents from a `CAMetalDisplayLink` on a thread of its own, at the screen's refresh rate up to 120 Hz, in both modes. Each present shows the latest latched frame, so a new frame reaches a fast screen sooner; in Classic nothing else changes.
- **Retrace times are exact.** Retrace n is due n × 1001/60 ms after the clock started. The displayed frame carries that time (`DisplayedFrame::latchTime`), read together with its frame buffer.
- **Frames between the game's are replays, as planned (Rewritten's high frame rate).** The GX layer records each frame's draws instead of drawing them. The display draws the record for every refresh, with the camera moved on to that refresh's time. The game's own drawing never runs twice: it moves the game on (bones, random numbers, timers). See [High frame rate](../port/phase2-status.md#high-frame-rate-rewritten).
- **The game reboots in place between levels (2026-10-30).** The console boots a stub that boots the game again (`LoadElf::RunModule`); this used to re-exec the process, which lost the window, fullscreen, focus and the compiled pipelines. Now (`platform/src/os/reboot.cpp`):
  - `StartGame` takes a snapshot first: the game image's writable segments, the arena's non-zero pages (about 2 MB and 1.4 MB), the OS heaps and arena bounds, and the default thread's `OSThread`.
  - `RunModule`'s thread leaves without handing the baton on and a host thread does the rest: it cancels and joins every game thread (each leaves at its next turn), drops the work host threads have in flight (DVD reads finish first; ARAM, alarms, VI and PAD callbacks, AX voices go), flushes the interrupts, maps fresh arena pages and copies the snapshot back, and starts `main()` on a new default thread.
  - The retrace, alarm, audio and disc host threads keep running; the display shows black meanwhile (the game set `VISetBlack` before leaving).
  - `FRONTLINE_RESTART_EXEC=1` re-execs as before; `FRONTLINE_REBOOT_TRACE=1` times each reboot.

## Consequences

- Scheduling is deterministic: the same inputs give the same thread interleaving. Input replays and lockstep (Phase 5) depend on that.
- Ordinary tools (Xcode's thread view, TSan, Instruments) work on game threads.
- Code that waits by spinning needs an edit. Hangs make these easy to find, and the macro keeps the GameCube build unchanged.
