// CARD: memory cards. Each slot is a folder of .gci files, the format of
// Dolphin's GCI folders and GameCube save tools: the file's 64-byte directory
// entry (the SDK's CARDDir, big-endian) and then its 8 KB blocks. Saves move
// between this port, Dolphin and a console that way. A slot without a folder
// is empty (frontline::SetCardFolder, or FRONTLINE_CARD_A/B).
//
// The card acts as a 251-block (16 Mbit) one. Operations finish before they
// return: an asynchronous call leaves its result for CARDGetResultCode and
// calls its callback at once. The rules (who may open, read or change a file,
// alignment, the icon offsets in CARDStat, timestamps) follow the SDK.
//
// Each change rewrites the whole file through a temporary and a rename, so a
// crash never leaves half a save. Allocation is plain malloc: on game threads
// operator new belongs to the game.

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "dolphin/card.h"
#include "dolphin/dvd.h"
#include "dolphin/dvd_priv.h"
#include "frontline/platform.h"
#include "os/os_internal.h"

namespace {

constexpr s32 kMemSize = 16;  // Mbit
constexpr u32 kSectorSize = 0x2000;
constexpr u32 kSystemBlocks = 5;
constexpr u32 kTotalBlocks = (kMemSize << 17) / kSectorSize;
constexpr u32 kEntrySize = 0x40;
constexpr u32 kReadAlign = 0x200;     // CARD_SEG_SIZE
constexpr u32 kIconAddrLimit = 0x200;  // CARD_READ_SIZE
constexpr u32 kCommentSize = 0x40;
constexpr u32 kPathMax = 1024;

// A directory entry, as the SDK keeps it (CARDDir).
struct Dir {
    u8 gameName[4];
    u8 company[2];
    u8 bannerFormat;
    char fileName[CARD_FILENAME_MAX];
    u32 time;  // seconds since 2000
    u32 iconAddr;
    u16 iconFormat;
    u16 iconSpeed;
    u8 permission;
    u8 copyTimes;
    u16 startBlock;
    u16 length;  // blocks
    u32 commentAddr;
};

struct File {
    bool used;
    char path[kPathMax];  // the .gci
    Dir dir;
};

struct Card {
    char folder[kPathMax];  // empty: no card
    bool mounted;
    s32 result;  // the last operation's, for CARDGetResultCode
    File files[CARD_MAX_FILE];
};

Card g_cards[2];

u32 Get16(const u8* p) { return (u32)p[0] << 8 | p[1]; }
u32 Get32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
void Put16(u8* p, u32 v) {
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}
void Put32(u8* p, u32 v) {
    Put16(p, v >> 16);
    Put16(p + 2, v);
}

void LoadEntry(Dir& d, const u8* e) {
    std::memcpy(d.gameName, e, 4);
    std::memcpy(d.company, e + 0x04, 2);
    d.bannerFormat = e[0x07];
    std::memcpy(d.fileName, e + 0x08, CARD_FILENAME_MAX);
    d.time = Get32(e + 0x28);
    d.iconAddr = Get32(e + 0x2C);
    d.iconFormat = (u16)Get16(e + 0x30);
    d.iconSpeed = (u16)Get16(e + 0x32);
    d.permission = e[0x34];
    d.copyTimes = e[0x35];
    d.startBlock = (u16)Get16(e + 0x36);
    d.length = (u16)Get16(e + 0x38);
    d.commentAddr = Get32(e + 0x3C);
}

void StoreEntry(const Dir& d, u8* e) {
    std::memset(e, 0xFF, kEntrySize);
    std::memcpy(e, d.gameName, 4);
    std::memcpy(e + 0x04, d.company, 2);
    e[0x07] = d.bannerFormat;
    std::memcpy(e + 0x08, d.fileName, CARD_FILENAME_MAX);
    Put32(e + 0x28, d.time);
    Put32(e + 0x2C, d.iconAddr);
    Put16(e + 0x30, d.iconFormat);
    Put16(e + 0x32, d.iconSpeed);
    e[0x34] = d.permission;
    e[0x35] = d.copyTimes;
    Put16(e + 0x36, d.startBlock);
    Put16(e + 0x38, d.length);
    Put32(e + 0x3C, d.commentAddr);
}

// The console's clock: local time in seconds since 2000.
u32 Now() {
    time_t now = std::time(nullptr);
    struct tm local;
    localtime_r(&now, &local);
    return (u32)(now + local.tm_gmtoff - 946684800);
}

Card* GetCard(s32 chan) {
    if (chan < 0 || chan > 1) {
        return nullptr;
    }
    static bool init;
    if (!init) {
        init = true;
        const char* env[2] = {std::getenv("FRONTLINE_CARD_A"), std::getenv("FRONTLINE_CARD_B")};
        for (int i = 0; i < 2; i++) {
            if (env[i] != nullptr) {
                frontline::SetCardFolder(i, env[i]);
            }
        }
    }
    return &g_cards[chan];
}

// The card, if it is in and mounted.
Card* Mounted(s32 chan) {
    Card* card = GetCard(chan);
    return card != nullptr && card->folder[0] != '\0' && card->mounted ? card : nullptr;
}

s32 Finish(Card* card, s32 result, CARDCallback callback, s32 chan) {
    if (card != nullptr) {
        card->result = result;
    }
    if (callback != nullptr && result != CARD_RESULT_BUSY) {
        callback(chan, result);
    }
    return result;
}

// __CARDAccess: this game's file (by the disc's game and maker codes).
s32 Access(const File& f) {
    if (!f.used) {
        return CARD_RESULT_NOFILE;
    }
    const DVDDiskID* id = DVDGetCurrentDiskID();
    if (std::memcmp(f.dir.gameName, id->gameName, 4) == 0 && std::memcmp(f.dir.company, id->company, 2) == 0) {
        return CARD_RESULT_READY;
    }
    return CARD_RESULT_NOPERM;
}

// Ours, or another game's made public.
s32 Readable(const File& f) {
    s32 result = Access(f);
    if (result == CARD_RESULT_NOPERM && (f.dir.permission & CARD_ATTR_PUBLIC) != 0) {
        return CARD_RESULT_READY;
    }
    return result;
}

u32 UsedBlocks(const Card& card) {
    u32 used = 0;
    for (const File& f : card.files) {
        if (f.used) {
            used += f.dir.length;
        }
    }
    return used;
}

// First fit in the user area, for a new file's start block.
u16 AllocBlocks(const Card& card, u32 count) {
    u32 start = kSystemBlocks;
    while (start + count <= kTotalBlocks) {
        u32 next = start;
        for (const File& f : card.files) {
            if (f.used && f.dir.startBlock < start + count && start < (u32)f.dir.startBlock + f.dir.length) {
                next = f.dir.startBlock + f.dir.length;
                break;
            }
        }
        if (next == start) {
            return (u16)start;
        }
        start = next;
    }
    return 0xFFFF;
}

// The whole .gci (entry and blocks), malloc'd; null if unreadable.
u8* ReadGci(const File& f) {
    u32 size = kEntrySize + f.dir.length * kSectorSize;
    u8* data = (u8*)std::malloc(size);
    FILE* in = std::fopen(f.path, "rb");
    bool ok = in != nullptr && std::fread(data, 1, size, in) == size;
    if (in != nullptr) {
        std::fclose(in);
    }
    if (!ok) {
        std::free(data);
        return nullptr;
    }
    return data;
}

// Writes the .gci with the current entry and `blocks` (the file's length).
bool WriteGci(const File& f, const u8* blocks) {
    char temp[kPathMax + 8];
    std::snprintf(temp, sizeof(temp), "%s.tmp", f.path);
    FILE* out = std::fopen(temp, "wb");
    if (out == nullptr) {
        std::fprintf(stderr, "frontline: memory card: can't write %s\n", temp);
        return false;
    }
    u8 entry[kEntrySize];
    StoreEntry(f.dir, entry);
    u32 size = f.dir.length * kSectorSize;
    bool ok = std::fwrite(entry, 1, kEntrySize, out) == kEntrySize && std::fwrite(blocks, 1, size, out) == size;
    ok = std::fflush(out) == 0 && fsync(fileno(out)) == 0 && ok;
    std::fclose(out);
    if (!ok || std::rename(temp, f.path) != 0) {
        std::fprintf(stderr, "frontline: memory card: can't write %s\n", f.path);
        std::remove(temp);
        return false;
    }
    return true;
}

// Rewrites the file with a new entry (its blocks unchanged).
s32 UpdateEntry(File& f, const Dir& dir) {
    u8* data = ReadGci(f);
    if (data == nullptr) {
        return CARD_RESULT_IOERROR;
    }
    Dir old = f.dir;
    f.dir = dir;
    bool ok = WriteGci(f, data + kEntrySize);
    std::free(data);
    if (!ok) {
        f.dir = old;
        return CARD_RESULT_IOERROR;
    }
    return CARD_RESULT_READY;
}

int CompareNames(const void* a, const void* b) { return std::strcmp((const char*)a, (const char*)b); }

// Loads the folder's .gci files into the directory, in name order.
void Scan(Card& card) {
    for (File& f : card.files) {
        f.used = false;
    }
    DIR* dir = opendir(card.folder);
    if (dir == nullptr) {
        return;
    }
    constexpr u32 kNameMax = 256;
    char(*names)[kNameMax] = (char(*)[kNameMax])std::malloc(CARD_MAX_FILE * kNameMax);
    int count = 0;
    while (struct dirent* e = readdir(dir)) {
        size_t len = std::strlen(e->d_name);
        if (len > 4 && len < kNameMax && strcasecmp(e->d_name + len - 4, ".gci") == 0 && count < CARD_MAX_FILE) {
            std::memcpy(names[count++], e->d_name, len + 1);
        }
    }
    closedir(dir);
    std::qsort(names, count, kNameMax, CompareNames);
    int next = 0;
    for (int i = 0; i < count; i++) {
        File& f = card.files[next];
        std::snprintf(f.path, sizeof(f.path), "%s/%s", card.folder, names[i]);
        u8 entry[kEntrySize];
        FILE* in = std::fopen(f.path, "rb");
        struct stat st;
        bool ok = in != nullptr && std::fread(entry, 1, kEntrySize, in) == kEntrySize && fstat(fileno(in), &st) == 0;
        if (in != nullptr) {
            std::fclose(in);
        }
        if (!ok) {
            continue;
        }
        LoadEntry(f.dir, entry);
        if (f.dir.length == 0 || (u64)st.st_size != kEntrySize + (u64)f.dir.length * kSectorSize ||
            UsedBlocks(card) + f.dir.length > kTotalBlocks - kSystemBlocks) {
            std::fprintf(stderr, "frontline: memory card: skipping %s (not a GameCube save that fits)\n", f.path);
            continue;
        }
        // Files from elsewhere keep their start block unless it overlaps.
        u16 start = f.dir.startBlock;
        f.dir.startBlock = 0xFFFF;
        bool overlaps = start < kSystemBlocks || start + f.dir.length > kTotalBlocks;
        for (int k = 0; k < next && !overlaps; k++) {
            const Dir& o = card.files[k].dir;
            overlaps = start < o.startBlock + o.length && o.startBlock < start + f.dir.length;
        }
        f.dir.startBlock = overlaps ? AllocBlocks(card, f.dir.length) : start;
        f.used = true;
        next++;
    }
    std::free(names);
}

// Directory entry `fileNo` for an open file (FastOpen, Read, Write).
File* FileOf(CARDFileInfo* info, Card** cardOut) {
    if (info == nullptr || info->fileNo < 0 || info->fileNo >= CARD_MAX_FILE) {
        return nullptr;
    }
    Card* card = Mounted(info->chan);
    if (card == nullptr) {
        return nullptr;
    }
    *cardOut = card;
    File& f = card->files[info->fileNo];
    return f.used ? &f : nullptr;
}

s32 FindFile(Card& card, const char* name) {
    for (s32 i = 0; i < CARD_MAX_FILE; i++) {
        const File& f = card.files[i];
        if (Access(f) == CARD_RESULT_READY && std::strncmp(f.dir.fileName, name, CARD_FILENAME_MAX) == 0) {
            return i;
        }
    }
    return -1;
}

// The CARDStat offsets of the banner, icons and data (UpdateIconOffsets).
void UpdateIconOffsets(const Dir& d, CARDStat* stat) {
    u32 offset = d.iconAddr;
    if (offset == 0xFFFFFFFF) {
        stat->bannerFormat = 0;
        stat->iconFormat = 0;
        stat->iconSpeed = 0;
        offset = 0;
    }
    switch (stat->bannerFormat & 3) {
    case 1:  // C8
        stat->offsetBanner = offset;
        offset += 96 * 32;
        stat->offsetBannerTlut = offset;
        offset += 2 * 256;
        break;
    case 2:  // RGB5A3
        stat->offsetBanner = offset;
        offset += 2 * 96 * 32;
        stat->offsetBannerTlut = 0xFFFFFFFF;
        break;
    default:
        stat->offsetBanner = 0xFFFFFFFF;
        stat->offsetBannerTlut = 0xFFFFFFFF;
        break;
    }
    bool tlut = false;
    for (int i = 0; i < 8; i++) {
        switch ((stat->iconFormat >> (2 * i)) & 3) {
        case 1:  // C8
            stat->offsetIcon[i] = offset;
            offset += 32 * 32;
            tlut = true;
            break;
        case 2:  // RGB5A3
            stat->offsetIcon[i] = offset;
            offset += 2 * 32 * 32;
            break;
        default:
            stat->offsetIcon[i] = 0xFFFFFFFF;
            break;
        }
    }
    if (tlut) {
        stat->offsetIconTlut = offset;
        offset += 2 * 256;
    } else {
        stat->offsetIconTlut = 0xFFFFFFFF;
    }
    stat->offsetData = offset;
}

// A file name for the folder: "<maker>-<game>-<name>.gci", as Dolphin names them.
void HostPath(const Card& card, const Dir& d, char* path) {
    char name[CARD_FILENAME_MAX + 1];
    std::memcpy(name, d.fileName, CARD_FILENAME_MAX);
    name[CARD_FILENAME_MAX] = '\0';
    for (char* p = name; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\' || *p == ':' || (u8)*p < 0x20 || (u8)*p >= 0x7F) {
            *p = '_';
        }
    }
    std::snprintf(path, kPathMax, "%s/%.2s-%.4s-%s.gci", card.folder, (const char*)d.company,
                  (const char*)d.gameName, name);
}

}  // namespace

void frontline::SetCardFolder(int slot, const char* folder) {
    if (slot < 0 || slot > 1) {
        return;
    }
    Card& card = g_cards[slot];
    card.mounted = false;
    card.folder[0] = '\0';
    if (folder != nullptr && folder[0] != '\0') {
        mkdir(folder, 0755);
        std::snprintf(card.folder, sizeof(card.folder), "%s", folder);
    }
}

extern "C" {

void CARDInit(void) { GetCard(0); }

s32 CARDProbe(s32 chan) {
    frontline::os::SafePoint();
    Card* card = GetCard(chan);
    return card != nullptr && card->folder[0] != '\0';
}

s32 CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize) {
    frontline::os::SafePoint();
    Card* card = GetCard(chan);
    if (card == nullptr || card->folder[0] == '\0') {
        return CARD_RESULT_NOCARD;
    }
    if (memSize != nullptr) {
        *memSize = kMemSize;
    }
    if (sectorSize != nullptr) {
        *sectorSize = kSectorSize;
    }
    return CARD_RESULT_READY;
}

s32 CARDMountAsync(s32 chan, void*, CARDCallback, CARDCallback attachCallback) {
    Card* card = GetCard(chan);
    if (card == nullptr || card->folder[0] == '\0') {
        return CARD_RESULT_NOCARD;
    }
    if (!card->mounted) {
        Scan(*card);
        card->mounted = true;
    }
    return Finish(card, CARD_RESULT_READY, attachCallback, chan);
}

s32 CARDUnmount(s32 chan) {
    Card* card = GetCard(chan);
    if (card == nullptr || card->folder[0] == '\0') {
        return CARD_RESULT_NOCARD;
    }
    card->mounted = false;
    return CARD_RESULT_READY;
}

s32 CARDGetResultCode(s32 chan) {
    frontline::os::SafePoint();
    Card* card = GetCard(chan);
    if (card == nullptr || card->folder[0] == '\0') {
        return CARD_RESULT_NOCARD;
    }
    return card->result;
}

s32 CARDCheckAsync(s32 chan, CARDCallback callback) {
    Card* card = Mounted(chan);
    if (card == nullptr) {
        return CARD_RESULT_NOCARD;
    }
    return Finish(card, CARD_RESULT_READY, callback, chan);
}

// Erases the card: every file on it.
s32 CARDFormatAsync(s32 chan, CARDCallback callback) {
    Card* card = Mounted(chan);
    if (card == nullptr) {
        return CARD_RESULT_NOCARD;
    }
    s32 result = CARD_RESULT_READY;
    for (File& f : card->files) {
        if (f.used && std::remove(f.path) != 0) {
            result = CARD_RESULT_IOERROR;
        }
        f.used = false;
    }
    return Finish(card, result, callback, chan);
}

s32 CARDFreeBlocks(s32 chan, s32* byteNotUsed, s32* filesNotUsed) {
    Card* card = Mounted(chan);
    if (card == nullptr) {
        return CARD_RESULT_NOCARD;
    }
    s32 files = 0;
    for (const File& f : card->files) {
        files += !f.used;
    }
    if (byteNotUsed != nullptr) {
        *byteNotUsed = (s32)((kTotalBlocks - kSystemBlocks - UsedBlocks(*card)) * kSectorSize);
    }
    if (filesNotUsed != nullptr) {
        *filesNotUsed = files;
    }
    return CARD_RESULT_READY;
}

s32 CARDGetSectorSize(s32 chan, u32* size) {
    if (Mounted(chan) == nullptr) {
        return CARD_RESULT_NOCARD;
    }
    if (size != nullptr) {
        *size = kSectorSize;
    }
    return CARD_RESULT_READY;
}

s32 CARDFastOpen(s32 chan, s32 fileNo, CARDFileInfo* info) {
    Card* card = Mounted(chan);
    if (card == nullptr) {
        return CARD_RESULT_NOCARD;
    }
    if (fileNo < 0 || fileNo >= CARD_MAX_FILE) {
        return CARD_RESULT_FATAL_ERROR;
    }
    info->chan = -1;
    const File& f = card->files[fileNo];
    s32 result = Readable(f);
    if (result == CARD_RESULT_READY) {
        info->chan = chan;
        info->fileNo = fileNo;
        info->offset = 0;
        info->length = (s32)(f.dir.length * kSectorSize);
        info->iBlock = f.dir.startBlock;
    }
    return result;
}

s32 CARDOpen(s32 chan, const char* fileName, CARDFileInfo* info) {
    Card* card = Mounted(chan);
    if (card == nullptr) {
        return CARD_RESULT_NOCARD;
    }
    info->chan = -1;
    s32 fileNo = FindFile(*card, fileName);
    if (fileNo < 0) {
        return CARD_RESULT_NOFILE;
    }
    return CARDFastOpen(chan, fileNo, info);
}

s32 CARDClose(CARDFileInfo* info) {
    if (info == nullptr || Mounted(info->chan) == nullptr) {
        return CARD_RESULT_NOCARD;
    }
    info->chan = -1;
    return CARD_RESULT_READY;
}

s32 CARDCreateAsync(s32 chan, const char* fileName, u32 size, CARDFileInfo* info, CARDCallback callback) {
    Card* card = Mounted(chan);
    if (card == nullptr) {
        return CARD_RESULT_NOCARD;
    }
    if (size == 0 || size % kSectorSize != 0) {
        return CARD_RESULT_FATAL_ERROR;
    }
    if (std::strlen(fileName) > CARD_FILENAME_MAX) {
        return CARD_RESULT_NAMETOOLONG;
    }
    if (FindFile(*card, fileName) >= 0) {
        return CARD_RESULT_EXIST;
    }
    s32 fileNo = 0;
    while (fileNo < CARD_MAX_FILE && card->files[fileNo].used) {
        fileNo++;
    }
    if (fileNo == CARD_MAX_FILE) {
        return CARD_RESULT_NOENT;
    }
    u32 blocks = size / kSectorSize;
    u16 start = AllocBlocks(*card, blocks);
    if (UsedBlocks(*card) + blocks > kTotalBlocks - kSystemBlocks || start == 0xFFFF) {
        return CARD_RESULT_INSSPACE;
    }
    File& f = card->files[fileNo];
    std::memset(&f.dir, 0xFF, sizeof(f.dir));
    const DVDDiskID* id = DVDGetCurrentDiskID();
    std::memcpy(f.dir.gameName, id->gameName, 4);
    std::memcpy(f.dir.company, id->company, 2);
    std::memset(f.dir.fileName, 0, CARD_FILENAME_MAX);
    std::strncpy(f.dir.fileName, fileName, CARD_FILENAME_MAX);
    f.dir.permission = CARD_ATTR_PUBLIC;
    f.dir.copyTimes = 0;
    f.dir.length = (u16)blocks;
    f.dir.startBlock = start;
    f.dir.time = Now();
    HostPath(*card, f.dir, f.path);
    u8* zero = (u8*)std::calloc(blocks, kSectorSize);
    bool ok = WriteGci(f, zero);
    std::free(zero);
    if (!ok) {
        return Finish(card, CARD_RESULT_IOERROR, callback, chan);
    }
    f.used = true;
    info->chan = chan;
    info->fileNo = fileNo;
    info->offset = 0;
    info->length = (s32)size;
    info->iBlock = start;
    return Finish(card, CARD_RESULT_READY, callback, chan);
}

s32 CARDDelete(s32 chan, const char* fileName) {
    Card* card = Mounted(chan);
    if (card == nullptr) {
        return CARD_RESULT_NOCARD;
    }
    s32 fileNo = FindFile(*card, fileName);
    if (fileNo < 0) {
        return Finish(card, CARD_RESULT_NOFILE, nullptr, chan);
    }
    File& f = card->files[fileNo];
    if (std::remove(f.path) != 0) {
        return Finish(card, CARD_RESULT_IOERROR, nullptr, chan);
    }
    f.used = false;
    return Finish(card, CARD_RESULT_READY, nullptr, chan);
}

s32 CARDReadAsync(CARDFileInfo* info, void* addr, s32 length, s32 offset, CARDCallback callback) {
    Card* card = nullptr;
    File* f = FileOf(info, &card);
    if (f == nullptr) {
        return card == nullptr ? CARD_RESULT_NOCARD : CARD_RESULT_NOFILE;
    }
    if (length <= 0 || offset < 0 || length % kReadAlign != 0 || offset % kReadAlign != 0) {
        return CARD_RESULT_FATAL_ERROR;
    }
    s32 result = Readable(*f);
    if (result != CARD_RESULT_READY) {
        return result;
    }
    if ((u32)offset + (u32)length > f->dir.length * kSectorSize) {
        return CARD_RESULT_LIMIT;
    }
    u8* data = ReadGci(*f);
    if (data == nullptr) {
        return Finish(card, CARD_RESULT_IOERROR, callback, info->chan);
    }
    std::memcpy(addr, data + kEntrySize + offset, length);
    std::free(data);
    info->offset = offset + length;
    return Finish(card, CARD_RESULT_READY, callback, info->chan);
}

s32 CARDWriteAsync(CARDFileInfo* info, const void* addr, s32 length, s32 offset, CARDCallback callback) {
    Card* card = nullptr;
    File* f = FileOf(info, &card);
    if (f == nullptr) {
        return card == nullptr ? CARD_RESULT_NOCARD : CARD_RESULT_NOFILE;
    }
    if (length <= 0 || offset < 0 || length % kSectorSize != 0 || offset % kSectorSize != 0) {
        return CARD_RESULT_FATAL_ERROR;
    }
    s32 result = Access(*f);
    if (result != CARD_RESULT_READY) {
        return result;
    }
    if ((u32)offset + (u32)length > f->dir.length * kSectorSize) {
        return CARD_RESULT_LIMIT;
    }
    u8* data = ReadGci(*f);
    if (data == nullptr) {
        return Finish(card, CARD_RESULT_IOERROR, callback, info->chan);
    }
    std::memcpy(data + kEntrySize + offset, addr, length);
    Dir old = f->dir;
    f->dir.time = Now();
    bool ok = WriteGci(*f, data + kEntrySize);
    std::free(data);
    if (!ok) {
        f->dir = old;
        return Finish(card, CARD_RESULT_IOERROR, callback, info->chan);
    }
    info->offset = offset + length;
    return Finish(card, CARD_RESULT_READY, callback, info->chan);
}

s32 CARDWrite(CARDFileInfo* info, const void* addr, s32 length, s32 offset) {
    return CARDWriteAsync(info, addr, length, offset, nullptr);
}

s32 CARDGetStatus(s32 chan, s32 fileNo, CARDStat* stat) {
    Card* card = Mounted(chan);
    if (card == nullptr) {
        return CARD_RESULT_NOCARD;
    }
    if (fileNo < 0 || fileNo >= CARD_MAX_FILE) {
        return CARD_RESULT_FATAL_ERROR;
    }
    const File& f = card->files[fileNo];
    s32 result = Readable(f);
    if (result != CARD_RESULT_READY) {
        return result;
    }
    const Dir& d = f.dir;
    std::memcpy(stat->gameName, d.gameName, 4);
    std::memcpy(stat->company, d.company, 2);
    stat->length = d.length * kSectorSize;
    std::memcpy(stat->fileName, d.fileName, CARD_FILENAME_MAX);
    stat->time = d.time;
    stat->bannerFormat = d.bannerFormat;
    stat->iconAddr = d.iconAddr;
    stat->iconFormat = d.iconFormat;
    stat->iconSpeed = d.iconSpeed;
    stat->commentAddr = d.commentAddr;
    UpdateIconOffsets(d, stat);
    return CARD_RESULT_READY;
}

s32 CARDSetStatus(s32 chan, s32 fileNo, CARDStat* stat) {
    Card* card = Mounted(chan);
    if (card == nullptr) {
        return CARD_RESULT_NOCARD;
    }
    if (fileNo < 0 || fileNo >= CARD_MAX_FILE || (stat->iconAddr != 0xFFFFFFFF && stat->iconAddr >= kIconAddrLimit) ||
        (stat->commentAddr != 0xFFFFFFFF && stat->commentAddr % kSectorSize > kSectorSize - kCommentSize)) {
        return CARD_RESULT_FATAL_ERROR;
    }
    File& f = card->files[fileNo];
    s32 result = Access(f);
    if (result != CARD_RESULT_READY) {
        return Finish(card, result, nullptr, chan);
    }
    Dir d = f.dir;
    d.bannerFormat = stat->bannerFormat;
    d.iconAddr = stat->iconAddr;
    d.iconFormat = stat->iconFormat;
    d.iconSpeed = stat->iconSpeed;
    d.commentAddr = stat->commentAddr;
    UpdateIconOffsets(d, stat);
    if (d.iconAddr == 0xFFFFFFFF) {
        d.iconSpeed = (u16)((d.iconSpeed & ~3) | 1);  // CARD_STAT_SPEED_FAST
    }
    d.time = Now();
    return Finish(card, UpdateEntry(f, d), nullptr, chan);
}

s32 CARDSetAttributes(s32 chan, s32 fileNo, u8 attr) {
    Card* card = Mounted(chan);
    if (card == nullptr) {
        return CARD_RESULT_NOCARD;
    }
    if (fileNo < 0 || fileNo >= CARD_MAX_FILE ||
        (attr & ~(CARD_ATTR_PUBLIC | CARD_ATTR_NO_COPY | CARD_ATTR_NO_MOVE)) != 0) {
        return CARD_RESULT_FATAL_ERROR;
    }
    File& f = card->files[fileNo];
    s32 result = Access(f);
    if (result != CARD_RESULT_READY) {
        return Finish(card, result, nullptr, chan);
    }
    Dir d = f.dir;
    d.permission = attr;
    return Finish(card, UpdateEntry(f, d), nullptr, chan);
}

}  // extern "C"

// Rebooting (os/reboot.cpp): the cards stay in their slots, unmounted. Every
// operation finishes before it returns, so nothing is in flight.
void frontline::os::ResetCard() {
    for (Card& card : g_cards) {
        card.mounted = false;
        card.result = CARD_RESULT_READY;
    }
}
