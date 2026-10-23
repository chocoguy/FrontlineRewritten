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

}  // namespace frontline::os
