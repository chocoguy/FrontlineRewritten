// CARD: memory cards. For now both slots are empty, which the game handles
// (it offers to continue without saving). Saves to a folder come later.

#include "dolphin/card.h"
#include "os/os_internal.h"

extern "C" {

void CARDInit(void) {}

s32 CARDProbe(s32) {
    frontline::os::SafePoint();
    return FALSE;
}

s32 CARDProbeEx(s32, s32* memSize, s32* sectorSize) {
    frontline::os::SafePoint();
    if (memSize != nullptr) {
        *memSize = 0;
    }
    if (sectorSize != nullptr) {
        *sectorSize = 0;
    }
    return CARD_RESULT_NOCARD;
}

s32 CARDMountAsync(s32, void*, CARDCallback, CARDCallback) { return CARD_RESULT_NOCARD; }
s32 CARDUnmount(s32) { return CARD_RESULT_NOCARD; }
s32 CARDGetResultCode(s32) {
    frontline::os::SafePoint();
    return CARD_RESULT_NOCARD;
}
s32 CARDCheckAsync(s32, CARDCallback) { return CARD_RESULT_NOCARD; }
s32 CARDFormatAsync(s32, CARDCallback) { return CARD_RESULT_NOCARD; }
s32 CARDFreeBlocks(s32, s32* byteNotUsed, s32* filesNotUsed) {
    if (byteNotUsed != nullptr) {
        *byteNotUsed = 0;
    }
    if (filesNotUsed != nullptr) {
        *filesNotUsed = 0;
    }
    return CARD_RESULT_NOCARD;
}
s32 CARDGetSectorSize(s32, u32* size) {
    if (size != nullptr) {
        *size = 0;
    }
    return CARD_RESULT_NOCARD;
}
s32 CARDFastOpen(s32, s32, CARDFileInfo*) { return CARD_RESULT_NOCARD; }
s32 CARDOpen(s32, const char*, CARDFileInfo*) { return CARD_RESULT_NOCARD; }
s32 CARDClose(CARDFileInfo*) { return CARD_RESULT_NOCARD; }
s32 CARDCreateAsync(s32, const char*, u32, CARDFileInfo*, CARDCallback) { return CARD_RESULT_NOCARD; }
s32 CARDDelete(s32, const char*) { return CARD_RESULT_NOCARD; }
s32 CARDReadAsync(CARDFileInfo*, void*, s32, s32, CARDCallback) { return CARD_RESULT_NOCARD; }
s32 CARDWriteAsync(CARDFileInfo*, const void*, s32, s32, CARDCallback) { return CARD_RESULT_NOCARD; }
s32 CARDWrite(CARDFileInfo*, const void*, s32, s32) { return CARD_RESULT_NOCARD; }
s32 CARDGetStatus(s32, s32, CARDStat*) { return CARD_RESULT_NOCARD; }
s32 CARDSetStatus(s32, s32, CARDStat*) { return CARD_RESULT_NOCARD; }
s32 CARDSetAttributes(s32, s32, u8) { return CARD_RESULT_NOCARD; }

}  // extern "C"
