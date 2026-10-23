// DVD: disc reads from the user's extracted disc folder (the one holding
// sys/ and files/; see frontline::SetDiscRoot).
//
// DVDInit indexes files/ once into a table sorted by lower-case path, which
// gives files stable entry numbers and the disc's case-insensitive lookups.
// Reads queue for one host worker thread (one drive, in priority order); each
// completion comes back as an interrupt that calls the game's callback.
// Allocation is plain malloc: on game threads operator new belongs to the game.

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dolphin/dvd.h"
#include "frontline/platform.h"
#include "os/os_internal.h"

namespace {

struct Entry {
    char* path;  // lower case, '/' separated, relative to files/
    char* host;
    u32 size;
    int fd;
};

char* g_root;
Entry* g_entries;
int g_numEntries;
int g_capacity;

struct Read {
    DVDFileInfo* info;
    void* addr;
    s32 length;
    s32 offset;
    s32 prio;
    s32 result;
    u32 seq;
    bool used;
};
constexpr int kMaxReads = 64;
Read g_reads[kMaxReads];
u32 g_seq;
int g_inFlight;
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_work = PTHREAD_COND_INITIALIZER;

void Add(const char* path, const char* host, u32 size) {
    if (g_numEntries == g_capacity) {
        g_capacity = g_capacity ? g_capacity * 2 : 1024;
        g_entries = static_cast<Entry*>(std::realloc(g_entries, g_capacity * sizeof(Entry)));
    }
    Entry& e = g_entries[g_numEntries++];
    e.path = strdup(path);
    for (char* c = e.path; *c != 0; c++) {
        *c = (char)std::tolower((unsigned char)*c);
    }
    e.host = strdup(host);
    e.size = size;
    e.fd = -1;
}

void Scan(const char* host, const char* rel) {
    DIR* dir = opendir(host);
    if (dir == nullptr) {
        return;
    }
    while (dirent* d = readdir(dir)) {
        if (d->d_name[0] == '.') {
            continue;
        }
        char childHost[1024];
        char childRel[1024];
        std::snprintf(childHost, sizeof(childHost), "%s/%s", host, d->d_name);
        std::snprintf(childRel, sizeof(childRel), "%s%s%s", rel, *rel ? "/" : "", d->d_name);
        struct stat st;
        if (stat(childHost, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            Scan(childHost, childRel);
        } else if (S_ISREG(st.st_mode)) {
            Add(childRel, childHost, (u32)st.st_size);
        }
    }
    closedir(dir);
}

int Compare(const void* a, const void* b) {
    return std::strcmp(static_cast<const Entry*>(a)->path, static_cast<const Entry*>(b)->path);
}

// Delivered on the game side: the read's done.
void Complete(void* arg) {
    Read* r = static_cast<Read*>(arg);
    DVDFileInfo* info = r->info;
    s32 result = r->result;
    pthread_mutex_lock(&g_lock);
    r->used = false;
    g_inFlight--;
    pthread_mutex_unlock(&g_lock);
    info->cb.state = 0;
    info->cb.transferredSize = result > 0 ? (u32)result : 0;
    if (info->callback != nullptr) {
        info->callback(result, info);
    }
}

void* Worker(void*) {
    pthread_setname_np("frontline dvd");
    pthread_mutex_lock(&g_lock);
    for (;;) {
        Read* next = nullptr;
        for (Read& r : g_reads) {
            if (r.used && r.result == INT32_MIN &&
                (next == nullptr || r.prio < next->prio || (r.prio == next->prio && r.seq < next->seq))) {
                next = &r;
            }
        }
        if (next == nullptr) {
            pthread_cond_wait(&g_work, &g_lock);
            continue;
        }
        Entry& e = g_entries[next->info->startAddr];
        if (e.fd < 0) {
            e.fd = open(e.host, O_RDONLY);
        }
        void* addr = next->addr;
        s32 length = next->length;
        s32 offset = next->offset;
        pthread_mutex_unlock(&g_lock);
        s32 result = -1;
        if (e.fd >= 0) {
            ssize_t got = pread(e.fd, addr, (size_t)length, offset);
            if (got >= 0) {
                // The console reads in 32-byte units past the file's end; the
                // bytes there are whatever follows on the disc. Zeros here.
                std::memset(static_cast<u8*>(addr) + got, 0, (size_t)(length - got));
                result = length;
            }
        }
        pthread_mutex_lock(&g_lock);
        next->result = result;
        pthread_mutex_unlock(&g_lock);
        frontline::os::RaiseInterrupt(Complete, next);
        pthread_mutex_lock(&g_lock);
    }
    return nullptr;
}

const char* Root() {
    if (g_root == nullptr) {
        const char* env = std::getenv("FRONTLINE_DISC");
        if (env != nullptr) {
            frontline::SetDiscRoot(env);
        }
    }
    return g_root;
}

}  // namespace

void frontline::SetDiscRoot(const char* folder) {
    std::free(g_root);
    g_root = strdup(folder);
}

extern "C" {

void DVDInit(void) {
    frontline::os::SafePoint();
    if (g_entries != nullptr) {
        return;
    }
    const char* root = Root();
    if (root == nullptr) {
        std::fprintf(stderr, "frontline: no disc folder (set FRONTLINE_DISC to the folder with sys/ and files/)\n");
        std::abort();
    }
    char files[1024];
    std::snprintf(files, sizeof(files), "%s/files", root);
    Scan(files, "");
    if (g_numEntries == 0) {
        std::fprintf(stderr, "frontline: no files in %s\n", files);
        std::abort();
    }
    std::qsort(g_entries, g_numEntries, sizeof(Entry), Compare);
    pthread_t thread;
    pthread_create(&thread, nullptr, Worker, nullptr);
    pthread_detach(thread);
}

s32 DVDConvertPathToEntrynum(const char* pathPtr) {
    frontline::os::SafePoint();
    char path[1024];
    int n = 0;
    for (const char* c = pathPtr; *c != 0 && n < (int)sizeof(path) - 1; c++) {
        char ch = *c == '\\' ? '/' : (char)std::tolower((unsigned char)*c);
        if (ch == '/' && (n == 0 || path[n - 1] == '/')) {
            continue;  // leading and doubled separators
        }
        path[n++] = ch;
    }
    path[n] = 0;
    int lo = 0, hi = g_numEntries - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int cmp = std::strcmp(path, g_entries[mid].path);
        if (cmp == 0) {
            return mid;
        }
        if (cmp < 0) {
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    return -1;
}

BOOL DVDFastOpen(s32 entrynum, DVDFileInfo* fileInfo) {
    if (entrynum < 0 || entrynum >= g_numEntries) {
        return FALSE;
    }
    std::memset(fileInfo, 0, sizeof(*fileInfo));
    fileInfo->startAddr = (u32)entrynum;  // natively the entry, not a disc offset
    fileInfo->length = g_entries[entrynum].size;
    return TRUE;
}

BOOL DVDOpen(const char* fileName, DVDFileInfo* fileInfo) {
    return DVDFastOpen(DVDConvertPathToEntrynum(fileName), fileInfo);
}

BOOL DVDClose(DVDFileInfo*) { return TRUE; }

BOOL DVDReadAsyncPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset, DVDCallback callback, s32 prio) {
    frontline::os::SafePoint();
    pthread_mutex_lock(&g_lock);
    Read* slot = nullptr;
    for (Read& r : g_reads) {
        if (!r.used) {
            slot = &r;
            break;
        }
    }
    if (slot == nullptr) {
        std::fprintf(stderr, "frontline: too many disc reads queued\n");
        std::abort();
    }
    *slot = {fileInfo, addr, length, offset, prio, INT32_MIN, g_seq++, true};
    fileInfo->callback = callback;
    fileInfo->cb.state = 1;  // busy
    fileInfo->cb.addr = addr;
    fileInfo->cb.length = (u32)length;
    fileInfo->cb.offset = (u32)offset;
    g_inFlight++;
    pthread_cond_signal(&g_work);
    pthread_mutex_unlock(&g_lock);
    return TRUE;
}

s32 DVDGetCommandBlockStatus(const DVDCommandBlock* block) { return block->state; }

// 0 ready, 1 busy: never an error (cover open, wrong disc, ...).
s32 DVDGetDriveStatus(void) {
    frontline::os::SafePoint();
    pthread_mutex_lock(&g_lock);
    s32 status = g_inFlight > 0 ? 1 : 0;
    pthread_mutex_unlock(&g_lock);
    return status;
}

}  // extern "C"
