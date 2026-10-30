// Shared by the OS layer's files (docs/architecture/0002-threads-interrupts-and-time.md).
#pragma once

#include <cstdint>

#include "dolphin/os.h"

namespace frontline::os {

// The scheduler lock. Held while scheduler or OS object state changes; never
// while game code runs.
void Lock();
void Unlock();

// An interrupt: `fn(arg)` runs on the game side, at the next safe point (or
// right away if every game thread is blocked), with interrupts disabled.
using InterruptFn = void (*)(void* arg);

// Queues an interrupt. Any host thread may call it; the lock must not be held.
void RaiseInterrupt(InterruptFn fn, void* arg);

// Delivers pending interrupts if the running context allows it. Every
// platform entry point calls it (as hardware would interrupt anywhere).
void SafePoint();

// For the other OS files, with the lock held: blocks the running thread on
// `queue` until OSWakeupThread, and wakes a queue.
void SleepLocked(OSThreadQueue* queue);
void WakeupLocked(OSThreadQueue* queue);

// The OS timer's clock: 40.5 MHz ticks since the platform started.
OSTime Now();

// The alarm list (alarm.cpp): fires due alarms, from an interrupt.
void StartAlarms();

// Prints a backtrace on a crash (crash.cpp).
void InstallCrashHandler();

// Heap memory for the game's threads' stacks, carved from the top of the arena
// before the game sees it (arena.cpp).
void* AllocThreadStack(uint32_t size);

// Host nanoseconds game threads have spent in SPIN_WAIT (waiting for the
// retrace, mostly), for FRONTLINE_PERF.
uint64_t SpinWaitNanos();

// The game image's operator delete, for game memory freed from host threads
// (NativeFreeLater): the image keeps its operator new and delete to itself.
void SetGameFree(void (*free)(void*));

// Rebooting the game in place between levels (reboot.cpp). StartGame takes
// the snapshot the first time; Reboot, on the running game thread, ends every
// game thread, puts each part back as the game's static initializers left it,
// and starts main() again. It never returns.
void TakeSnapshot(int (*gameMain)());
bool CanReboot();  // the game runs from its own image, and the snapshot is taken
[[noreturn]] void Reboot();

// Each part's share, with no game thread running:
// - thread.cpp: the running thread leaves without handing over; every other
//   game thread ends; then no threads, queues or interrupts; and the default
//   thread as the snapshot had it, started on main() again.
[[noreturn]] void LeaveForReboot();
void StopGameThreads();
void ResetThreads();
void SaveMainThread();
void RestartMainThread(int (*gameMain)());
// - memory.cpp: the OS heaps and arena bounds, and the thread stack pool.
void SaveMemoryState();
void RestoreMemoryState();
// - time.cpp: no alarms.
void ResetAlarms();
// - The other libraries (vi.cpp, pad.cpp, dvd.cpp, card.cpp, audio.cpp, and
//   the GX backend): callbacks, queues and work in flight dropped, the
//   devices as before the game's first call. Host threads keep running.
void ResetVi();
void ResetPad();
void ResetDvd();
void ResetCard();
void ResetAudio();
void ResetGx();

}  // namespace frontline::os
