// Exit Game on the main menu, when the app offers it (NativeExitGameOffered):
// a third button hint, "B EXIT GAME", under the menu's BROWSE and SELECT, and
// the game telling the platform each frame whether the main menu is up
// (NativeAtExitMenu), since B there asks the app to leave the game.
//
// The main menu is Master_Main (shell screen 0/0) as the active screen, with
// one of its five entries (Play, Personal Records, Credits, Options, Multiplayer) taking
// input. Its button hints are control 16: BROWSE and SELECT, the D-pad and
// the A button, laid out in the control's own frame a row apart, with the
// last row at the bottom of the screen. So while the menu is up, the control
// moves up a row (NativeUiTransformBegin) and the new row is drawn under
// SELECT in its frame (NativeUiTransformEnd): SELECT's text and A's icon,
// copied a row down, with the B button's image and the new text. The screen
// file is checked to be laid out as expected first, so another version of
// the disc just goes without the hint.

#include <string.h>

#include "game/IStudioFuncs.h"
#include "game/ShellMenu.h"
#include "game/TibPolygon.h"
#include "game/TibText.h"
#include "game/UISUtils.h"
#include "game/isexportdefs.h"
#include "game/matstack.h"
#include "game/uistypes.h"
#include "native/hooks.h"

namespace {

// Master_Main's controls.
const u32 kMenuEntries[] = {4, 7, 10, 1, 26};
const u32 kHints = 16;
const u32 kHintsList = 6;  // the root control's list holding it
const float kRow = 27.0f;  // BROWSE to SELECT

bool s_atMenu;
UISTransform_t* s_hintsTransform;
TibTextGraphicDataT* s_selectText;
TibPolyData_t* s_aIcon;
float s_mul[4], s_add[4];  // the hints' tint, as their control applied it

char s_label[] = "EXIT GAME";
UISString_t s_string;
TibTextGraphicDataT s_text;
TibPolyData_t s_icon;

bool Enabled(UISScrData_t* data, u32 control) {
    UISControlInfo_t* info = data->controls[control].info;
    return info->enabled != 0 && info->acceptsInput != 0;
}

// The B button's image in the hints' library, or -1.
int BButtonImage(int library) {
    IStudioLibrary* lib = &LibraryInfo[library];
    for (int i = 0; i < lib->numImages; i++) {
        const char* name = lib->imageNames[i];
        const char* base = strrchr(name, '\\');
        if (strcmp(base != NULL ? base + 1 : name, "Tri_button.gsh") == 0) {  // B (named for the PS2's)
            return i;
        }
    }
    return -1;
}

// Whether the main menu is up, finding its hints.
bool FindMenu() {
    UISInfo_t* ui = g_Shell.m_studio.m_ui;
    // (Screens visited stay loaded behind it, like the Mission Log.)
    if (!g_Shell.m_shellOn || ui == NULL || ui->numScreenRefs != 0 || ui->activeScreen >= ui->numScreens) {
        return false;
    }
    UISScreen_t* screen = &ui->screens[ui->activeScreen];
    UISScrData_t* data = screen->data;
    if (screen->group != 0 || screen->id != 0 || data == NULL || data->numControls <= 26) {
        return false;
    }
    UISControl_t* root = &data->controls[0];
    if (root->numLists <= kHintsList || *root->lists[kHintsList]->enable == 0) {
        return false;
    }
    bool entry = false;
    for (u32 control : kMenuEntries) {
        entry = entry || Enabled(data, control);
    }
    if (!entry) {
        return false;
    }
    // The hints: BROWSE, SELECT, the D-pad's control, the A button.
    UISControl_t* hints = &data->controls[kHints];
    if (hints->numLists < 1) {
        return false;
    }
    UISControlList_t* list = hints->lists[0];
    UISListEntry_t* e = list->entries;
    if (list->numEntries != 4 || e[0].type != 7 || e[1].type != 7 || e[2].type != UISLIST_CONTROL || e[3].type != 0) {
        return false;
    }
    TibPolyData_t* a = ADDR_TO_PTR(TibPolyData_t, e[3].data);
    if (a->library < 0 || BButtonImage(a->library) < 0) {
        return false;
    }
    s_hintsTransform = &hints->info->transform;
    s_selectText = ADDR_TO_PTR(TibTextGraphicDataT, e[1].data);
    s_aIcon = a;
    return true;
}

void DrawExitHint() {
    if (s_selectText->library < 0 || s_selectText->string == NULL) {
        return;
    }
    s_string.unk_0x0 = 0;
    s_string.maxLength = sizeof(s_label) - 1;
    s_string.text = s_label;
    s_text = *s_selectText;
    s_text.string = &s_string;
    s_text.y += kRow;
    s_icon = *s_aIcon;
    s_icon.imageIndex = (s16)BButtonImage(s_aIcon->library);
    for (TibPolyVert_t& v : s_icon.verts) {
        v.y += kRow;
    }
    // The image loads the first time (shell screens that show B free it when
    // they go, so it may load again).
    TibPolyInit(&s_icon);
    if (LibraryInfo[s_icon.library].images[s_icon.imageIndex] == NULL) {
        return;
    }
    float* mul = UISGetColorMultipler();
    float* add = UISGetColorAdditive();
    float mul0[4] = {mul[0], mul[1], mul[2], mul[3]};
    float add0[4] = {add[0], add[1], add[2], add[3]};
    UISSetColorMultipler(s_mul[0], s_mul[1], s_mul[2], s_mul[3]);
    UISSetColorAdditive(s_add[0], s_add[1], s_add[2], s_add[3]);
    TibPolyDraw(&s_icon);
    TibTextDraw(&s_text);
    UISSetColorMultipler(mul0[0], mul0[1], mul0[2], mul0[3]);
    UISSetColorAdditive(add0[0], add0[1], add0[2], add0[3]);
}

}  // namespace

void NativeExitGameUpdate() {
    s_atMenu = NativeExitGameOffered() != 0 && FindMenu();
    NativeAtExitMenu(s_atMenu);
}

void NativeUiTransformBegin(UISTransform_t* xf) {
    if (!s_atMenu || xf != s_hintsTransform) {
        return;
    }
    g_matStack->Top()->PreTranslate(CVector3(0.0f, -kRow, 0.0f));
    float* mul = UISGetColorMultipler();
    float* add = UISGetColorAdditive();
    memcpy(s_mul, mul, sizeof(s_mul));
    memcpy(s_add, add, sizeof(s_add));
}

void NativeUiTransformEnd(UISTransform_t* xf) {
    if (s_atMenu && xf == s_hintsTransform) {
        DrawExitHint();
    }
}
