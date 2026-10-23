// Game threads, run one at a time (docs/architecture/0002-threads-interrupts-and-time.md).
//
// Every OSThread runs on its own host thread, but only the one holding the
// baton (g_running) runs game code; the rest wait on their condition variable.
// The scheduler follows the console's OS: the highest-priority ready thread
// runs, switching only when the running thread blocks, yields, exits, or makes
// a higher-priority thread ready. Interrupts from host threads (the clock, file
// reads, audio) are queued and delivered at safe points on the running thread,
// or by the last thread to block when every thread is waiting.
//
// Nothing here allocates: the game's operator new owns the heap on game
// threads. A thread's native state lives in its OSThread's register context,
// which nothing else uses natively.

#include <pthread.h>
#include <sys/mman.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "frontline/platform.h"
#include "native/hooks.h"
#include "os_internal.h"

namespace frontline::os {
namespace {

constexpr int kNumPriorities = OS_PRIORITY_MAX + 1;
constexpr uint32_t kStackSize = 2u << 20;  // console stacks are 4-8 KB; native frames are bigger
constexpr int kDefaultPriority = 16;       // the console's default (main) thread

struct NativeThread {
    pthread_t host;
    pthread_cond_t turn;  // signalled when the thread gets the baton
    void* (*func)(void*);
    void* param;
    bool interrupts;  // the interrupt-enable state (per context, as MSR[EE] on the console)
    bool cancelled;   // cancelled from another thread: exit when woken
};
static_assert(sizeof(NativeThread) <= sizeof(OSContext), "NativeThread lives in OSThread::context");

NativeThread* Native(OSThread* t) { return reinterpret_cast<NativeThread*>(&t->context); }

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_idle = PTHREAD_COND_INITIALIZER;  // an interrupt arrived while every thread waits

OSThread* g_running;  // holds the baton; null while idle
OSThreadQueue g_runQueue[kNumPriorities];
uint32_t g_runMask;  // bit p: g_runQueue[p] isn't empty
OSThreadQueue g_active;  // every live thread, through linkActive
bool g_idleInterrupts = true;
int g_interruptDepth;  // > 0 while handlers run: a wakeup switches threads only after them
bool g_rescheduleLater;

struct Interrupt {
    InterruptFn fn;
    void* arg;
};
constexpr int kMaxInterrupts = 256;
Interrupt g_interrupts[kMaxInterrupts];
int g_interruptHead;
int g_interruptCount;
std::atomic<int> g_pending;

// Host stacks for game threads, from the arena, reused when an OSThread is
// created again.
struct StackSlot {
    OSThread* owner;
    void* stack;
    pthread_t host;
    bool joinable;
};
StackSlot g_stacks[32];

// Arena memory freed from host threads (NativeFreeLater), handed to the
// game's operator delete at a safe point.
constexpr int kMaxFrees = 4096;
void* g_frees[kMaxFrees];
int g_numFrees;
std::atomic<int> g_freesPending;

OSThread g_mainThread;

thread_local OSThread* t_self;   // the game thread this host thread runs, if any
thread_local bool t_gameCode;    // holds the baton, or delivers interrupts
bool g_booted;

// Thread queues, through OSThread::link (as the console's OS keeps them).
void AddTail(OSThreadQueue* q, OSThread* t) {
    t->link.next = nullptr;
    t->link.prev = q->tail;
    if (q->tail != nullptr) {
        q->tail->link.next = t;
    } else {
        q->head = t;
    }
    q->tail = t;
}

void Remove(OSThreadQueue* q, OSThread* t) {
    if (t->link.next != nullptr) {
        t->link.next->link.prev = t->link.prev;
    } else {
        q->tail = t->link.prev;
    }
    if (t->link.prev != nullptr) {
        t->link.prev->link.next = t->link.next;
    } else {
        q->head = t->link.next;
    }
}

// In priority order, after the threads of the same priority.
void AddPrio(OSThreadQueue* q, OSThread* t) {
    OSThread* next = q->head;
    while (next != nullptr && next->priority <= t->priority) {
        next = next->link.next;
    }
    if (next == nullptr) {
        AddTail(q, t);
        return;
    }
    t->link.next = next;
    t->link.prev = next->link.prev;
    next->link.prev = t;
    if (t->link.prev != nullptr) {
        t->link.prev->link.next = t;
    } else {
        q->head = t;
    }
}

void AddActive(OSThread* t) {
    t->linkActive.next = nullptr;
    t->linkActive.prev = g_active.tail;
    if (g_active.tail != nullptr) {
        g_active.tail->linkActive.next = t;
    } else {
        g_active.head = t;
    }
    g_active.tail = t;
}

void RemoveActive(OSThread* t) {
    if (t->linkActive.next != nullptr) {
        t->linkActive.next->linkActive.prev = t->linkActive.prev;
    } else {
        g_active.tail = t->linkActive.prev;
    }
    if (t->linkActive.prev != nullptr) {
        t->linkActive.prev->linkActive.next = t->linkActive.next;
    } else {
        g_active.head = t->linkActive.next;
    }
}

void SetRun(OSThread* t) {
    t->queue = &g_runQueue[t->priority];
    AddTail(t->queue, t);
    g_runMask |= 1u << t->priority;
}

void UnsetRun(OSThread* t) {
    OSThreadQueue* q = t->queue;
    Remove(q, t);
    if (q->head == nullptr) {
        g_runMask &= ~(1u << (q - g_runQueue));
    }
    t->queue = nullptr;
}

int HighestReady() { return g_runMask != 0 ? __builtin_ctz(g_runMask) : kNumPriorities; }

bool* InterruptFlag() { return g_running != nullptr ? &Native(g_running)->interrupts : &g_idleInterrupts; }

// Runs the queued interrupts on this host thread, with interrupts disabled.
void DeliverLocked() {
    g_interruptDepth++;
    bool* flag = InterruptFlag();
    bool enabled = *flag;
    *flag = false;
    bool gameCode = t_gameCode;
    t_gameCode = true;
    while (g_interruptCount > 0) {
        Interrupt i = g_interrupts[g_interruptHead];
        g_interruptHead = (g_interruptHead + 1) % kMaxInterrupts;
        g_interruptCount--;
        g_pending.fetch_sub(1, std::memory_order_relaxed);
        pthread_mutex_unlock(&g_lock);
        i.fn(i.arg);
        pthread_mutex_lock(&g_lock);
    }
    t_gameCode = gameCode;
    *flag = enabled;
    g_interruptDepth--;
}

// Every thread waits: deliver interrupts here until one makes a thread ready.
void IdleLocked() {
    g_running = nullptr;
    while (g_runMask == 0) {
        if (g_interruptCount > 0 && g_idleInterrupts) {
            DeliverLocked();
            g_rescheduleLater = false;
        } else {
            pthread_cond_wait(&g_idle, &g_lock);
        }
    }
}

// Waits for `self`'s turn; a cancelled thread exits here.
void WaitTurnLocked(OSThread* self) {
    t_gameCode = false;
    while (g_running != self && !Native(self)->cancelled) {
        pthread_cond_wait(&Native(self)->turn, &g_lock);
    }
    if (Native(self)->cancelled) {
        pthread_mutex_unlock(&g_lock);
        pthread_exit(nullptr);
    }
    t_gameCode = true;
}

// Picks the thread to run, with the lock held, on the running thread's host
// thread. `yield`: the running thread goes behind ready threads of its
// priority. `exiting`: the running thread is gone; don't wait for a turn.
void Reschedule(bool yield = false, bool exiting = false) {
    if (g_interruptDepth > 0) {
        g_rescheduleLater = true;
        return;
    }
    OSThread* self = t_self;
    OSThread* cur = g_running;
    if (cur != nullptr && cur->state == OS_THREAD_STATE_RUNNING) {
        int best = HighestReady();
        if (yield ? best > cur->priority : best >= cur->priority) {
            return;
        }
        cur->state = OS_THREAD_STATE_READY;
        SetRun(cur);
    }
    if (g_runMask == 0) {
        IdleLocked();
    }
    OSThread* next = g_runQueue[HighestReady()].head;
    UnsetRun(next);
    next->state = OS_THREAD_STATE_RUNNING;
    g_running = next;
    if (next == self) {
        return;
    }
    pthread_cond_signal(&Native(next)->turn);
    if (!exiting) {
        WaitTurnLocked(self);
    }
}

// Makes a queue's threads ready, without switching.
void ReadyAllLocked(OSThreadQueue* q) {
    while (OSThread* t = q->head) {
        Remove(q, t);
        t->state = OS_THREAD_STATE_READY;
        t->queue = nullptr;
        if (t->suspend <= 0) {
            SetRun(t);
        }
    }
}

// Mutexes: priority inheritance, as the console's OS does it.
s32 EffectivePriority(OSThread* t) {
    s32 priority = t->base;
    for (OSMutex* m = t->queueMutex.head; m != nullptr; m = m->link.next) {
        OSThread* waiter = m->queue.head;
        if (waiter != nullptr && waiter->priority < priority) {
            priority = waiter->priority;
        }
    }
    return priority;
}

void SetPriority(OSThread* t, s32 priority) {
    if (t->priority == priority) {
        return;
    }
    if (t->state == OS_THREAD_STATE_READY && t->queue != nullptr && t->suspend <= 0) {
        UnsetRun(t);
        t->priority = priority;
        SetRun(t);
    } else if (t->state == OS_THREAD_STATE_WAITING && t->queue != nullptr) {
        OSThreadQueue* q = t->queue;
        Remove(q, t);
        t->priority = priority;
        AddPrio(q, t);
    } else {
        t->priority = priority;
    }
}

void Promote(OSThread* owner, s32 priority) {
    while (owner != nullptr && priority < owner->priority) {
        SetPriority(owner, priority);
        owner = owner->mutex != nullptr ? owner->mutex->thread : nullptr;
    }
}

void PushMutex(OSThread* t, OSMutex* m) {
    m->link.next = nullptr;
    m->link.prev = t->queueMutex.tail;
    if (t->queueMutex.tail != nullptr) {
        t->queueMutex.tail->link.next = m;
    } else {
        t->queueMutex.head = m;
    }
    t->queueMutex.tail = m;
}

void PopMutex(OSThread* t, OSMutex* m) {
    if (m->link.next != nullptr) {
        m->link.next->link.prev = m->link.prev;
    } else {
        t->queueMutex.tail = m->link.prev;
    }
    if (m->link.prev != nullptr) {
        m->link.prev->link.next = m->link.next;
    } else {
        t->queueMutex.head = m->link.next;
    }
}

void UnlockAllMutexes(OSThread* t) {
    while (OSMutex* m = t->queueMutex.head) {
        PopMutex(t, m);
        m->count = 0;
        m->thread = nullptr;
        ReadyAllLocked(&m->queue);
    }
}

void FlushFrees() {
    if (g_freesPending.load(std::memory_order_acquire) == 0) {
        return;
    }
    void* batch[64];
    int n;
    do {
        pthread_mutex_lock(&g_lock);
        n = g_numFrees < 64 ? g_numFrees : 64;
        g_numFrees -= n;
        std::memcpy(batch, &g_frees[g_numFrees], n * sizeof(void*));
        g_freesPending.store(g_numFrees, std::memory_order_release);
        pthread_mutex_unlock(&g_lock);
        for (int i = 0; i < n; i++) {
            ::operator delete(batch[i]);
        }
    } while (n == 64);
}

void* HostEntry(void* arg) {
    OSThread* t = static_cast<OSThread*>(arg);
    t_self = t;
    char name[32];
    std::snprintf(name, sizeof(name), "game thread %p", static_cast<void*>(t));
    pthread_setname_np(t == &g_mainThread ? "game main" : name);
    pthread_mutex_lock(&g_lock);
    WaitTurnLocked(t);
    pthread_mutex_unlock(&g_lock);
    void* result = Native(t)->func(Native(t)->param);
    OSExitThread(result);
    return nullptr;
}

void StartHost(OSThread* t) {
    StackSlot* slot = nullptr;
    for (StackSlot& s : g_stacks) {
        if (s.owner == t) {
            slot = &s;
            break;
        }
    }
    if (slot == nullptr) {
        for (StackSlot& s : g_stacks) {
            if (s.owner == nullptr) {
                slot = &s;
                slot->owner = t;
                slot->stack = AllocThreadStack(kStackSize);
                break;
            }
        }
    }
    if (slot == nullptr) {
        std::fprintf(stderr, "frontline: too many game threads\n");
        std::abort();
    }
    if (slot->joinable) {
        pthread_join(slot->host, nullptr);  // the OSThread's last host thread has exited
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstack(&attr, slot->stack, kStackSize);
    if (pthread_create(&Native(t)->host, &attr, HostEntry, t) != 0) {
        std::perror("frontline: pthread_create");
        std::abort();
    }
    pthread_attr_destroy(&attr);
    slot->host = Native(t)->host;
    slot->joinable = true;
}

void InitThread(OSThread* t, void* (*func)(void*), void* param, s32 priority, u16 attr) {
    std::memset(&t->context, 0, sizeof(t->context));
    NativeThread* n = new (Native(t)) NativeThread{};
    pthread_cond_init(&n->turn, nullptr);
    n->func = func;
    n->param = param;
    n->interrupts = true;
    t->state = OS_THREAD_STATE_READY;
    t->attr = attr & OS_THREAD_ATTR_DETACH;
    t->base = t->priority = priority;
    t->suspend = 1;
    t->val.setAddr(0xFFFFFFFF);
    t->queue = nullptr;
    t->mutex = nullptr;
    OSInitThreadQueue(&t->queueJoin);
    t->queueMutex.head = t->queueMutex.tail = nullptr;
}

// The game's static initializers run on the host's main thread before main();
// on the console they run on the default thread, with the OS up. So the first
// OS call adopts the calling thread as the default thread, until StartGame
// moves that to a host thread of its own.
void Boot() {
    g_booted = true;
    InitThread(&g_mainThread, nullptr, nullptr, kDefaultPriority, 0);
    g_mainThread.suspend = 0;
    g_mainThread.state = OS_THREAD_STATE_RUNNING;
    AddActive(&g_mainThread);
    g_running = &g_mainThread;
    t_self = &g_mainThread;
    t_gameCode = true;
}

inline void EnsureBoot() {
    if (__builtin_expect(!g_booted, 0)) {
        Boot();
    }
}

}  // namespace

void Lock() { pthread_mutex_lock(&g_lock); }
void Unlock() { pthread_mutex_unlock(&g_lock); }

void RaiseInterrupt(InterruptFn fn, void* arg) {
    pthread_mutex_lock(&g_lock);
    if (g_interruptCount == kMaxInterrupts) {
        std::fprintf(stderr, "frontline: interrupt queue overflow (the game isn't reaching safe points)\n");
        std::abort();
    }
    g_interrupts[(g_interruptHead + g_interruptCount) % kMaxInterrupts] = {fn, arg};
    g_interruptCount++;
    g_pending.fetch_add(1, std::memory_order_release);
    pthread_cond_broadcast(&g_idle);
    pthread_mutex_unlock(&g_lock);
}

void SafePoint() {
    EnsureBoot();
    if (t_self == nullptr || g_running != t_self) {
        return;  // host threads, and interrupt handlers run while idle
    }
    FlushFrees();
    if (g_pending.load(std::memory_order_acquire) == 0 || !Native(t_self)->interrupts) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    if (g_interruptCount > 0) {
        DeliverLocked();
        if (g_rescheduleLater) {
            g_rescheduleLater = false;
            Reschedule();
        }
    }
    pthread_mutex_unlock(&g_lock);
}

void SleepLocked(OSThreadQueue* queue) {
    OSThread* cur = g_running;
    cur->state = OS_THREAD_STATE_WAITING;
    cur->queue = queue;
    AddPrio(queue, cur);
    Reschedule();
}

void WakeupLocked(OSThreadQueue* queue) {
    ReadyAllLocked(queue);
    Reschedule();
}

}  // namespace frontline::os

using namespace frontline::os;

void frontline::StartGame(int (*gameMain)()) {
    InstallCrashHandler();
    ArenaInit();
    StartAlarms();
    EnsureBoot();
    if (t_self != &g_mainThread || g_running != &g_mainThread) {
        std::fprintf(stderr, "frontline: StartGame must run on the thread that ran the static initializers\n");
        std::abort();
    }
    pthread_mutex_lock(&g_lock);
    Native(&g_mainThread)->func = [](void* main) -> void* {
        reinterpret_cast<int (*)()>(main)();
        return nullptr;
    };
    Native(&g_mainThread)->param = reinterpret_cast<void*>(gameMain);
    t_self = nullptr;  // this thread goes back to the app
    t_gameCode = false;
    pthread_mutex_unlock(&g_lock);
    StartHost(&g_mainThread);
}

extern "C" {

int NativeOnGameThread(void) {
    EnsureBoot();
    return t_gameCode;
}

void NativeSpinWait(void) {
    if (t_self == nullptr || g_running != t_self) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    if (g_interruptCount == 0) {
        pthread_cond_wait(&g_idle, &g_lock);  // the caller's loop checks again
    }
    pthread_mutex_unlock(&g_lock);
    SafePoint();
}

void NativeFreeLater(void* p) {
    pthread_mutex_lock(&g_lock);
    if (g_numFrees < kMaxFrees) {
        g_frees[g_numFrees++] = p;
        g_freesPending.store(g_numFrees, std::memory_order_release);
    }  // else: leak it rather than touch the game heap from here
    pthread_mutex_unlock(&g_lock);
}

BOOL OSCreateThread(OSThread* thread, void* (*func)(void*), void* param, void* stack, u32 stackSize, s32 priority,
                    u16 attr) {
    EnsureBoot();
    if (priority < OS_PRIORITY_MIN || priority > OS_PRIORITY_MAX) {
        return FALSE;
    }
    InitThread(thread, func, param, priority, attr);
    // The game's own stack goes unused natively, but keeps its overflow marker.
    thread->stackBase = static_cast<u8*>(stack);
    thread->stackEnd = reinterpret_cast<u32*>(static_cast<u8*>(stack) - stackSize);
    *thread->stackEnd = OS_THREAD_STACK_MAGIC;
    pthread_mutex_lock(&g_lock);
    AddActive(thread);
    pthread_mutex_unlock(&g_lock);
    StartHost(thread);
    return TRUE;
}

s32 OSResumeThread(OSThread* thread) {
    SafePoint();
    pthread_mutex_lock(&g_lock);
    s32 prev = thread->suspend--;
    if (thread->suspend < 0) {
        thread->suspend = 0;
    } else if (thread->suspend == 0) {
        if (thread->state == OS_THREAD_STATE_READY) {
            thread->priority = EffectivePriority(thread);
            SetRun(thread);
        } else if (thread->state == OS_THREAD_STATE_WAITING && thread->queue != nullptr) {
            OSThreadQueue* q = thread->queue;
            Remove(q, thread);
            thread->priority = EffectivePriority(thread);
            AddPrio(q, thread);
        }
        Reschedule();
    }
    pthread_mutex_unlock(&g_lock);
    return prev;
}

s32 OSSuspendThread(OSThread* thread) {
    SafePoint();
    pthread_mutex_lock(&g_lock);
    s32 prev = thread->suspend++;
    if (prev == 0) {
        if (thread->state == OS_THREAD_STATE_RUNNING) {
            thread->state = OS_THREAD_STATE_READY;
            Reschedule();  // back when resumed
        } else if (thread->state == OS_THREAD_STATE_READY && thread->queue != nullptr) {
            UnsetRun(thread);
        }
    }
    pthread_mutex_unlock(&g_lock);
    return prev;
}

void OSCancelThread(OSThread* thread) {
    pthread_mutex_lock(&g_lock);
    switch (thread->state) {
    case OS_THREAD_STATE_READY:
        if (thread->queue != nullptr) {
            UnsetRun(thread);
        }
        break;
    case OS_THREAD_STATE_RUNNING:
        break;
    case OS_THREAD_STATE_WAITING:
        if (thread->queue != nullptr) {
            Remove(thread->queue, thread);
            thread->queue = nullptr;
        }
        if (thread->mutex != nullptr && thread->mutex->thread != nullptr) {
            OSThread* owner = thread->mutex->thread;
            SetPriority(owner, EffectivePriority(owner));
        }
        break;
    default:
        pthread_mutex_unlock(&g_lock);
        return;
    }
    thread->state = 0;
    UnlockAllMutexes(thread);
    ReadyAllLocked(&thread->queueJoin);
    RemoveActive(thread);
    if (thread == g_running) {
        Reschedule(false, true);
        pthread_mutex_unlock(&g_lock);
        pthread_exit(nullptr);
    }
    Native(thread)->cancelled = true;
    pthread_cond_signal(&Native(thread)->turn);
    Reschedule();
    pthread_mutex_unlock(&g_lock);
}

void OSExitThread(void* val) {
    pthread_mutex_lock(&g_lock);
    OSThread* cur = g_running;
    if (cur->attr & OS_THREAD_ATTR_DETACH) {
        cur->state = 0;
    } else {
        cur->state = OS_THREAD_STATE_MORIBUND;
        cur->val.setAddr(PTR_TO_ADDR(val));
    }
    UnlockAllMutexes(cur);
    ReadyAllLocked(&cur->queueJoin);
    RemoveActive(cur);
    Reschedule(false, true);
    pthread_mutex_unlock(&g_lock);
    pthread_exit(nullptr);
}

OSThread* OSGetCurrentThread(void) {
    EnsureBoot();
    return g_running;
}

void OSYieldThread(void) {
    SafePoint();
    pthread_mutex_lock(&g_lock);
    Reschedule(true);
    pthread_mutex_unlock(&g_lock);
}

void OSInitThreadQueue(OSThreadQueue* queue) { queue->head = queue->tail = nullptr; }

void OSSleepThread(OSThreadQueue* queue) {
    SafePoint();
    pthread_mutex_lock(&g_lock);
    SleepLocked(queue);
    pthread_mutex_unlock(&g_lock);
}

void OSWakeupThread(OSThreadQueue* queue) {
    EnsureBoot();
    pthread_mutex_lock(&g_lock);
    WakeupLocked(queue);
    pthread_mutex_unlock(&g_lock);
}

BOOL OSDisableInterrupts(void) {
    EnsureBoot();
    bool* flag = InterruptFlag();
    BOOL prev = *flag;
    *flag = false;
    return prev;
}

BOOL OSEnableInterrupts(void) {
    bool* flag = InterruptFlag();
    BOOL prev = *flag;
    *flag = true;
    SafePoint();
    return prev;
}

BOOL OSRestoreInterrupts(BOOL level) {
    bool* flag = InterruptFlag();
    BOOL prev = *flag;
    *flag = level != FALSE;
    if (level) {
        SafePoint();
    }
    return prev;
}

void OSInitMutex(OSMutex* mutex) {
    OSInitThreadQueue(&mutex->queue);
    mutex->thread = nullptr;
    mutex->count = 0;
}

void OSLockMutex(OSMutex* mutex) {
    SafePoint();
    pthread_mutex_lock(&g_lock);
    OSThread* cur = g_running;
    for (;;) {
        if (mutex->thread == nullptr) {
            mutex->thread = cur;
            mutex->count++;
            PushMutex(cur, mutex);
            break;
        }
        if (mutex->thread == cur) {
            mutex->count++;
            break;
        }
        cur->mutex = mutex;
        Promote(mutex->thread, cur->priority);
        SleepLocked(&mutex->queue);
        cur->mutex = nullptr;
    }
    pthread_mutex_unlock(&g_lock);
}

void OSUnlockMutex(OSMutex* mutex) {
    EnsureBoot();
    pthread_mutex_lock(&g_lock);
    OSThread* cur = g_running;
    if (mutex->thread == cur && --mutex->count == 0) {
        PopMutex(cur, mutex);
        mutex->thread = nullptr;
        if (cur->priority < cur->base) {
            cur->priority = EffectivePriority(cur);
        }
        WakeupLocked(&mutex->queue);
    }
    pthread_mutex_unlock(&g_lock);
}

void OSInitMessageQueue(OSMessageQueue* mq, OSMessage* msgArray, s32 msgCount) {
    OSInitThreadQueue(&mq->queueSend);
    OSInitThreadQueue(&mq->queueReceive);
    mq->msgArray = msgArray;
    mq->msgCount = msgCount;
    mq->firstIndex = 0;
    mq->usedCount = 0;
}

BOOL OSSendMessage(OSMessageQueue* mq, OSMessage msg, s32 flags) {
    SafePoint();
    pthread_mutex_lock(&g_lock);
    while (mq->msgCount <= mq->usedCount) {
        if (!(flags & OS_MESSAGE_BLOCK)) {
            pthread_mutex_unlock(&g_lock);
            return FALSE;
        }
        SleepLocked(&mq->queueSend);
    }
    mq->msgArray[(mq->firstIndex + mq->usedCount) % mq->msgCount] = msg;
    mq->usedCount++;
    WakeupLocked(&mq->queueReceive);
    pthread_mutex_unlock(&g_lock);
    return TRUE;
}

BOOL OSReceiveMessage(OSMessageQueue* mq, OSMessage* msg, s32 flags) {
    SafePoint();
    pthread_mutex_lock(&g_lock);
    while (mq->usedCount == 0) {
        if (!(flags & OS_MESSAGE_BLOCK)) {
            pthread_mutex_unlock(&g_lock);
            return FALSE;
        }
        SleepLocked(&mq->queueReceive);
    }
    if (msg != nullptr) {
        *msg = mq->msgArray[mq->firstIndex];
    }
    mq->firstIndex = (mq->firstIndex + 1) % mq->msgCount;
    mq->usedCount--;
    WakeupLocked(&mq->queueSend);
    pthread_mutex_unlock(&g_lock);
    return TRUE;
}

}  // extern "C"
