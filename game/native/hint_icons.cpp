// The menus' button icons (the A and B circles, the D-pad) for the device in
// use: while the keyboard and mouse were used last, keyboard keys drawn by
// the platform (NativeRenderKeyIcon) stand in for them. The UI's image slots
// are shared by every screen, so each controller icon a screen loads gets a
// keyboard twin, and TibPolyDraw draws whichever matches the device.

#include <string.h>

#include "ea/real.h"
#include "game/TibPolygon.h"
#include "game/memory.h"
#include "game/shape.h"
#include "game/sprite.h"
#include "native/hooks.h"

namespace {

// The icons (the shell's and the pause menu's copies have the same names;
// they're named after the PS2's buttons) and the texture drawn for each, at
// about four times its size on screen.
const struct {
    const char* file;
    int icon;
    int label;
    int width, height;
} kIcons[] = {
    {"X_button.gsh", NATIVE_ICON_KEY, NATIVE_LABEL_MENU_A, 96, 96},    // A
    {"Tri_button.gsh", NATIVE_ICON_KEY, NATIVE_LABEL_MENU_B, 96, 96},  // B
    {"Cir_button.gsh", NATIVE_ICON_KEY, NATIVE_LABEL_MENU_X, 96, 96},  // X
    {"Squ_button.gsh", NATIVE_ICON_KEY, NATIVE_LABEL_MENU_Y, 96, 96},  // Y
    {"DPad_blank.gsh", NATIVE_ICON_ARROW_KEYS, -1, 104, 104},
    {"DPad_UpHL.gsh", NATIVE_ICON_ARROW_UP, -1, 44, 44},
    {"DPad_DownHL.gsh", NATIVE_ICON_ARROW_DOWN, -1, 44, 44},
    {"DPad_LeftHL.gsh", NATIVE_ICON_ARROW_LEFT, -1, 40, 44},
    {"DPad_RightHL.gsh", NATIVE_ICON_ARROW_RIGHT, -1, 40, 44},
};

const u32 kShapeFormatRGBA8 = 0x16;  // CTexture::Set

struct Twin {
    CSprite* original;
    CSprite* keyboard;
};
Twin s_twins[32];

// The file name, without the library's directories.
const char* BaseName(const char* path) {
    const char* name = path;
    for (const char* p = path; *p != 0; p++) {
        if (*p == '\\' || *p == '/') {
            name = p + 1;
        }
    }
    return name;
}

// A shape file holding `rgba` as a GX RGBA8 texture: 4x4 texel blocks, each
// 16 alpha-red pairs then 16 green-blue pairs. The header is written already
// swapped ("SHPN"), as ShapeFileFromBigEndian leaves it.
ShapeFile* MakeShapeFile(const u8* rgba, int width, int height) {
    const u32 shapeAt = 0x20;
    u32 size = shapeAt + 0x10 + width * height * 4;
    ShapeFile* file = (ShapeFile*)DWI_allocalign("Native key icon", size, 32, 0x400);
    memcpy(file->magic, "SHPN", 4);
    file->firstShape = shapeAt;
    Shape* shape = (Shape*)((u8*)file + shapeAt);
    shape->format = kShapeFormatRGBA8;
    shape->palette = 0;
    shape->width = width;
    shape->height = height;
    shape->mipLevels = 0;
    u8* out = shape->data;
    for (int by = 0; by < height; by += 4) {
        for (int bx = 0; bx < width; bx += 4) {
            for (int i = 0; i < 16; i++) {
                const u8* p = rgba + ((by + i / 4) * width + bx + i % 4) * 4;
                out[i * 2] = p[3];
                out[i * 2 + 1] = p[0];
                out[32 + i * 2] = p[1];
                out[32 + i * 2 + 1] = p[2];
            }
            out += 64;
        }
    }
    return file;
}

}  // namespace

void NativeHintIconLoaded(const char* name, CSprite* sprite) {
    if (sprite == NULL || sprite->m_file == NULL) {
        return;
    }
    const char* base = BaseName(name);
    for (auto& icon : kIcons) {
        if (strcasecmp(base, icon.file) != 0) {
            continue;
        }
        for (Twin& twin : s_twins) {
            if (twin.original != NULL) {
                continue;
            }
            u8* rgba = (u8*)DWI_alloc("Native key icon pixels", icon.width * icon.height * 4, 0x400);
            NativeRenderKeyIcon(icon.icon, icon.label >= 0 ? NativeKeyLabel(icon.label) : "", icon.width,
                                icon.height, rgba);
            ShapeFile* file = MakeShapeFile(rgba, icon.width, icon.height);
            MEM_free(rgba);
            twin.original = sprite;
            twin.keyboard = new CSprite(file, (ERenderPriority)sprite->m_type);
            return;
        }
        return;
    }
}

void NativeHintIconUnloaded(CSprite* sprite) {
    for (Twin& twin : s_twins) {
        if (twin.original == sprite) {
            delete twin.keyboard;
            twin = Twin();
        }
    }
}

CSprite* NativeHintIcon(CSprite* sprite) {
    if (NativePromptDevice() != NATIVE_DEVICE_KEYBOARD) {
        return sprite;
    }
    for (Twin& twin : s_twins) {
        if (twin.original == sprite && sprite != NULL) {
            return twin.keyboard;
        }
    }
    return sprite;
}
