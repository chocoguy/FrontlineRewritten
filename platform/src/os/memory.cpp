// The OS arena and heaps.
//
// The arena (platform/src/address.cpp) is what OSGetArenaLo/Hi hand the game,
// minus a pool at the top for game threads' host stacks: those stay inside
// the 32-bit address map, so pointers to locals can be stored where the game
// keeps 32-bit pointers. The OS heap is a first-fit free list like the
// console's (the game makes one heap and takes nearly all of it for EA's MEM).

#include <sys/mman.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "frontline/platform.h"
#include "os_internal.h"

namespace frontline::os {
namespace {

constexpr uint32_t kStackPool = 32u << 20;
constexpr uint32_t kPage = 0x4000;

uint8_t* g_arenaLo;
uint8_t* g_arenaHi;
uint8_t* g_stackTop;  // the pool grows down from the arena's end
uint8_t* g_stackLimit;

void InitArena() {
    if (g_arenaLo != nullptr) {
        return;
    }
    g_arenaLo = static_cast<uint8_t*>(ArenaInit());
    g_stackTop = static_cast<uint8_t*>(ArenaHi());
    g_stackLimit = g_stackTop - kStackPool;
    g_arenaHi = g_stackLimit;
}

// The console's heap cells: a 32-byte header before each block.
struct Cell {
    Cell* prev;
    Cell* next;
    uint64_t size;  // with the header
    uint64_t pad;
};
static_assert(sizeof(Cell) == 32);

struct Heap {
    Cell* free;
    Cell* used;
    bool live;
};
constexpr int kMaxHeaps = 8;
Heap g_heaps[kMaxHeaps];
int g_numHeaps;

Cell* Unlink(Cell* list, Cell* cell) {
    if (cell->next != nullptr) {
        cell->next->prev = cell->prev;
    }
    if (cell->prev != nullptr) {
        cell->prev->next = cell->next;
    } else {
        list = cell->next;
    }
    return list;
}

Cell* Push(Cell* list, Cell* cell) {
    cell->prev = nullptr;
    cell->next = list;
    if (list != nullptr) {
        list->prev = cell;
    }
    return cell;
}

// Into the address-ordered free list, merging with its neighbours.
Cell* AddFree(Cell* list, Cell* cell) {
    Cell* prev = nullptr;
    Cell* next = list;
    while (next != nullptr && next < cell) {
        prev = next;
        next = next->next;
    }
    cell->prev = prev;
    cell->next = next;
    if (next != nullptr) {
        next->prev = cell;
        if ((uint8_t*)cell + cell->size == (uint8_t*)next) {
            cell->size += next->size;
            cell->next = next->next;
            if (cell->next != nullptr) {
                cell->next->prev = cell;
            }
        }
    }
    if (prev == nullptr) {
        return cell;
    }
    prev->next = cell;
    if ((uint8_t*)prev + prev->size == (uint8_t*)cell) {
        prev->size += cell->size;
        prev->next = cell->next;
        if (prev->next != nullptr) {
            prev->next->prev = prev;
        }
    }
    return list;
}

}  // namespace

void* AllocThreadStack(uint32_t size) {
    InitArena();
    if (g_stackTop - size < g_stackLimit) {
        std::fprintf(stderr, "frontline: out of game thread stacks\n");
        std::abort();
    }
    g_stackTop -= size;
    mprotect(g_stackTop, kPage, PROT_NONE);  // a guard page below each stack
    return g_stackTop;
}

}  // namespace frontline::os

using namespace frontline::os;

extern "C" {

volatile OSHeapHandle __OSCurrHeap = -1;

void* OSGetArenaLo(void) {
    InitArena();
    return g_arenaLo;
}

void* OSGetArenaHi(void) {
    InitArena();
    return g_arenaHi;
}

void OSSetArenaLo(void* addr) {
    InitArena();
    g_arenaLo = static_cast<uint8_t*>(addr);
}

void OSSetArenaHi(void* addr) {
    InitArena();
    g_arenaHi = static_cast<uint8_t*>(addr);
}

// The heap descriptors live here natively, so the arena is only aligned.
void* OSInitAlloc(void* arenaStart, void* arenaEnd, int maxHeaps) {
    if (maxHeaps > kMaxHeaps) {
        std::fprintf(stderr, "frontline: OSInitAlloc(%d heaps)\n", maxHeaps);
        std::abort();
    }
    std::memset(g_heaps, 0, sizeof(g_heaps));
    g_numHeaps = maxHeaps;
    __OSCurrHeap = -1;
    return reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(arenaStart) + 0x1F) & ~uintptr_t(0x1F));
}

OSHeapHandle OSCreateHeap(void* start, void* end) {
    uintptr_t lo = (reinterpret_cast<uintptr_t>(start) + 0x1F) & ~uintptr_t(0x1F);
    uintptr_t hi = reinterpret_cast<uintptr_t>(end) & ~uintptr_t(0x1F);
    for (int i = 0; i < g_numHeaps; i++) {
        if (!g_heaps[i].live) {
            Cell* cell = reinterpret_cast<Cell*>(lo);
            cell->prev = cell->next = nullptr;
            cell->size = hi - lo;
            g_heaps[i] = {cell, nullptr, true};
            return i;
        }
    }
    return -1;
}

OSHeapHandle OSSetCurrentHeap(OSHeapHandle heap) {
    OSHeapHandle prev = __OSCurrHeap;
    __OSCurrHeap = heap;
    return prev;
}

void* OSAllocFromHeap(OSHeapHandle handle, u32 size) {
    SafePoint();
    if (handle < 0 || handle >= g_numHeaps || !g_heaps[handle].live) {
        return nullptr;
    }
    Heap& heap = g_heaps[handle];
    uint64_t need = ((uint64_t)size + sizeof(Cell) + 0x1F) & ~uint64_t(0x1F);
    Cell* cell = heap.free;
    while (cell != nullptr && cell->size < need) {
        cell = cell->next;
    }
    if (cell == nullptr) {
        return nullptr;
    }
    heap.free = Unlink(heap.free, cell);
    if (cell->size - need >= 0x40) {  // split off the rest
        Cell* rest = reinterpret_cast<Cell*>(reinterpret_cast<uint8_t*>(cell) + need);
        rest->size = cell->size - need;
        cell->size = need;
        heap.free = AddFree(heap.free, rest);
    }
    heap.used = Push(heap.used, cell);
    return cell + 1;
}

void OSFreeToHeap(OSHeapHandle handle, void* ptr) {
    SafePoint();
    if (handle < 0 || handle >= g_numHeaps || !g_heaps[handle].live || ptr == nullptr) {
        return;
    }
    Heap& heap = g_heaps[handle];
    Cell* cell = static_cast<Cell*>(ptr) - 1;
    heap.used = Unlink(heap.used, cell);
    heap.free = AddFree(heap.free, cell);
}

}  // extern "C"
