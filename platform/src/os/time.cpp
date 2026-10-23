// The OS timer and alarms (docs/architecture/0002-threads-interrupts-and-time.md).
//
// OSGetTime/OSGetTick count at the console's timer rate (the 162 MHz bus
// clock / 4 = 40.5 MHz) from the host's monotonic clock. Alarms sit in a list
// sorted by fire time; a host thread sleeps until the first one is due and
// raises an interrupt, which fires every due alarm on the game side.

#include <mach/mach_time.h>
#include <pthread.h>

#include <cerrno>
#include <cstdio>
#include <ctime>

#include "os_internal.h"

namespace frontline::os {
namespace {

constexpr uint64_t kTimerHz = OS_BUS_CLOCK / 4;

uint64_t g_start;
mach_timebase_info_data_t g_timebase;

OSAlarm* g_alarmHead;  // sorted by fire time; under the scheduler lock
pthread_cond_t g_alarmChanged = PTHREAD_COND_INITIALIZER;
pthread_mutex_t g_alarmWait = PTHREAD_MUTEX_INITIALIZER;
bool g_alarmInterruptQueued;

uint64_t HostNanoseconds() {
    if (g_timebase.denom == 0) {
        mach_timebase_info(&g_timebase);
        g_start = mach_absolute_time();
    }
    return (mach_absolute_time() - g_start) * g_timebase.numer / g_timebase.denom;
}

void Insert(OSAlarm* alarm, OSTime fire, OSAlarmHandler handler) {
    alarm->handler = handler;
    alarm->fire = fire;
    OSAlarm* prev = nullptr;
    OSAlarm* next = g_alarmHead;
    while (next != nullptr && next->fire <= fire) {
        prev = next;
        next = next->next;
    }
    alarm->prev = prev;
    alarm->next = next;
    if (next != nullptr) {
        next->prev = alarm;
    }
    if (prev != nullptr) {
        prev->next = alarm;
    } else {
        g_alarmHead = alarm;
    }
}

void Unlink(OSAlarm* alarm) {
    if (alarm->next != nullptr) {
        alarm->next->prev = alarm->prev;
    }
    if (alarm->prev != nullptr) {
        alarm->prev->next = alarm->next;
    } else {
        g_alarmHead = alarm->next;
    }
    alarm->prev = alarm->next = nullptr;
}

// The first fire time of a periodic alarm after `now`.
OSTime NextPeriod(OSAlarm* alarm, OSTime now) {
    if (alarm->start > now) {
        return alarm->start;
    }
    return alarm->start + ((now - alarm->start) / alarm->period + 1) * alarm->period;
}

void WakeClock() {
    pthread_mutex_lock(&g_alarmWait);
    pthread_cond_signal(&g_alarmChanged);
    pthread_mutex_unlock(&g_alarmWait);
}

// On the game side: fire every due alarm. Periodic ones go back in first.
void FireAlarms(void*) {
    for (;;) {
        Lock();
        g_alarmInterruptQueued = false;
        OSAlarm* alarm = g_alarmHead;
        OSTime now = Now();
        if (alarm == nullptr || alarm->fire > now) {
            Unlock();
            WakeClock();
            return;
        }
        Unlink(alarm);
        OSAlarmHandler handler = alarm->handler;
        if (alarm->period != 0) {
            Insert(alarm, NextPeriod(alarm, now), handler);
        } else {
            alarm->handler = nullptr;
        }
        Unlock();
        handler(alarm, nullptr);
    }
}

void* ClockThread(void*) {
    pthread_setname_np("frontline alarm clock");
    pthread_mutex_lock(&g_alarmWait);
    for (;;) {
        Lock();
        OSTime next = g_alarmHead != nullptr ? g_alarmHead->fire : -1;
        OSTime now = Now();
        bool queued = g_alarmInterruptQueued;
        bool due = next >= 0 && next <= now && !queued;
        if (due) {
            g_alarmInterruptQueued = true;
        }
        Unlock();
        if (due) {
            RaiseInterrupt(FireAlarms, nullptr);
            continue;
        }
        if (next < 0 || queued) {
            pthread_cond_wait(&g_alarmChanged, &g_alarmWait);
        } else {
            uint64_t ns = (uint64_t)(next - now) * 1000000000ull / kTimerHz;
            timespec wait = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
            pthread_cond_timedwait_relative_np(&g_alarmChanged, &g_alarmWait, &wait);
        }
    }
    return nullptr;
}

}  // namespace

OSTime Now() {
    uint64_t ns = HostNanoseconds();
    return (OSTime)((__uint128_t)ns * kTimerHz / 1000000000u);
}

void StartAlarms() {
    static bool started;
    if (started) {
        return;
    }
    started = true;
    HostNanoseconds();
    pthread_t thread;
    pthread_create(&thread, nullptr, ClockThread, nullptr);
    pthread_detach(thread);
}

}  // namespace frontline::os

using namespace frontline::os;

extern "C" {

OSTime OSGetTime(void) {
    SafePoint();
    return Now();
}

OSTime __OSGetSystemTime(void) { return Now(); }

u32 OSGetTick(void) {
    SafePoint();
    return (u32)Now();
}

void OSInitAlarm(void) { StartAlarms(); }

void OSCreateAlarm(OSAlarm* alarm) {
    alarm->handler = nullptr;
    alarm->tag = 0;
}

void OSSetAlarm(OSAlarm* alarm, OSTime tick, OSAlarmHandler handler) {
    SafePoint();
    Lock();
    alarm->period = 0;
    Insert(alarm, Now() + tick, handler);
    Unlock();
    WakeClock();
}

void OSSetPeriodicAlarm(OSAlarm* alarm, OSTime start, OSTime period, OSAlarmHandler handler) {
    SafePoint();
    Lock();
    alarm->period = period;
    alarm->start = start;
    Insert(alarm, NextPeriod(alarm, Now()), handler);
    Unlock();
    WakeClock();
}

void OSCancelAlarm(OSAlarm* alarm) {
    Lock();
    if (alarm->handler != nullptr) {
        Unlink(alarm);
        alarm->handler = nullptr;
    }
    Unlock();
}

}  // extern "C"
