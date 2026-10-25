// UI screen files (.scr): the swap in game/uisswap.h, then the structure
// PatchScrData relocates and the plugin objects the screens hold.

#include "scr.h"

#include <cmath>
#include <cstring>

#include "check.h"
#include "types.h"
#include "game/uisswap.h"

namespace {

bool Finite(f32 f, f32 limit) { return std::isfinite(f) && std::fabs(f) <= limit; }

}  // namespace

bool CheckSCR(const std::vector<uint8_t>& file, ScrStats& stats) {
    auto* data = static_cast<UISScrData_t*>(ArenaCopy(file));
    u8* base = (u8*)data;
    u32 size = file.size();
    auto inFile = [&](u32 off, u32 n) { return off + n <= size; };
    CHECK(ScrDataFromBigEndian(data), "numControls %#x", data->numControls);
    CHECK(!ScrDataFromBigEndian(data), "swapped twice");
    CHECK(data->numControls > 0 && inFile(data->controls.addr(), data->numControls * sizeof(UISControl_t)),
          "%u controls", data->numControls);
    CHECK(inFile(data->strings.addr(), data->numStrings * sizeof(UISString_t)), "%u strings", data->numStrings);
    CHECK(inFile(data->stringRefs.addr(), data->numStringRefs * 4), "%u string refs", data->numStringRefs);
    CHECK(inFile(data->objects.addr(), data->numObjects * sizeof(UISListEntry_t)), "%u objects", data->numObjects);

    auto checkObject = [&](u16 type, u32 off) {
        stats.objects[type]++;
        if (type == 0) {
            CHECK(inFile(off, sizeof(TibPolyData_t)), "polygon at %#x", off);
            auto* poly = (TibPolyData_t*)(base + off);
            CHECK((poly->flags & ~3) == 0 && poly->library < 64 && poly->imageIndex < 1024,
                  "polygon flags %#x library %d image %d", poly->flags, poly->library, poly->imageIndex);
            for (auto& v : poly->verts) {
                CHECK(Finite(v.u, 16) && Finite(v.v, 16) && Finite(v.x, 1e4f) && Finite(v.y, 1e4f) &&
                          Finite(v.z, 1e4f),
                      "polygon vertex %f %f %f %f %f", v.u, v.v, v.x, v.y, v.z);
            }
        } else if (type == 7) {
            CHECK(inFile(off, sizeof(TibTextGraphicDataT)), "text at %#x", off);
            auto* text = (TibTextGraphicDataT*)(base + off);
            CHECK(text->library < 64 && text->fontIndex >= 0 && text->fontIndex < 1024 && (text->height > 0 || text->height == -1) &&
                      text->height <= 256,
                  "text library %d font %d height %d", text->library, text->fontIndex, text->height);
            for (f32 c : text->color) {
                CHECK(c >= 0 && c <= 255, "text color %f", c);
            }
            CHECK(Finite(text->x, 1e4f) && Finite(text->y, 1e4f), "text at %f, %f", text->x, text->y);
            // Relative to the object until TibTextProcessFnc loads it.
            u32 str = off + text->string.addr();
            CHECK(str >= data->strings.addr() && str < data->strings.addr() + data->numStrings * sizeof(UISString_t) &&
                      (str - data->strings.addr()) % sizeof(UISString_t) == 0,
                  "text string at %#x", str);
        }
        return true;
    };

    u32 codeStart = 0xFFFFFFFF;
    for (u32 i = 0; i < data->numControls; i++) {
        UISControl_t* c = (UISControl_t*)(base + data->controls.addr()) + i;
        CHECK(inFile(c->info.addr(), sizeof(UISControlInfo_t)), "control %u info", i);
        auto* info = (UISControlInfo_t*)(base + c->info.addr());
        CHECK(info->visible <= 1 && info->enabled <= 1 && info->acceptsInput <= 1, "control %u flags %u %u %u", i,
              info->visible, info->enabled, info->acceptsInput);
        for (f32 f : info->transform.pos) {
            CHECK(Finite(f, 1e4f), "control %u position %f", i, f);
        }
        for (f32 f : info->transform.scale) {
            CHECK(Finite(f, 100), "control %u scale %f", i, f);
        }
        for (f32 f : info->colorMul) {
            CHECK(Finite(f, 1000), "control %u color %f", i, f);
        }
        CHECK(inFile(c->events.addr(), c->numEvents * sizeof(UISEventEntry_t)), "control %u events", i);
        for (u32 k = 0; k < c->numEvents; k++) {
            UISEventEntry_t* e = (UISEventEntry_t*)(base + c->events.addr()) + k;
            if (e->id == UISEVENT_CHILD) {
                CHECK(e->pc < data->numControls, "control %u child %u", i, e->pc);
            } else {
                CHECK(e->pc < size, "control %u handler at %#x", i, e->pc);
                codeStart = std::min(codeStart, e->pc);
                stats.handlers++;
            }
        }
        CHECK(inFile(c->lists.addr(), c->numLists * 4), "control %u lists", i);
        for (u32 j = 0; j < c->numLists; j++) {
            u32 list = ((PTR32(UISControlList_t)*)(base + c->lists.addr()))[j].addr();
            CHECK(inFile(list, sizeof(UISControlList_t)), "control %u list %u", i, j);
            auto* l = (UISControlList_t*)(base + list);
            CHECK(inFile(l->enable.addr(), 4) && inFile(l->entries.addr(), l->numEntries * sizeof(UISListEntry_t)),
                  "control %u list %u", i, j);
            for (u32 k = 0; k < l->numEntries; k++) {
                UISListEntry_t* e = (UISListEntry_t*)(base + l->entries.addr()) + k;
                if (e->type == UISLIST_CONTROL) {
                    CHECK(e->data < data->numControls, "control %u list %u child %u", i, j, e->data);
                } else {
                    CHECK(e->type <= 7 && e->state == 0, "control %u list %u object type %u state %d", i, j, e->type,
                          e->state);
                    if (!checkObject(e->type, e->data)) {
                        return false;
                    }
                }
            }
        }
        stats.controls++;
    }
    CHECK(codeStart != 0xFFFFFFFF, "no script code");
    for (u32 i = 0; i < data->numObjects; i++) {
        UISListEntry_t* e = (UISListEntry_t*)(base + data->objects.addr()) + i;
        CHECK(e->type <= 7, "screen object type %u", e->type);
        if (e->data != 0 && !checkObject(e->type, e->data)) {
            return false;
        }
    }
    for (u32 i = 0; i < data->numStrings; i++) {
        UISString_t* s = (UISString_t*)(base + data->strings.addr()) + i;
        CHECK(s->maxLength < 0x10000 && inFile(s->text.addr(), s->maxLength + 1) && s->text.addr() < codeStart,
              "string %u: %u at %#x", i, s->maxLength, s->text.addr());
        // The buffer holds maxLength characters and a terminator, which the swap must leave in place.
        CHECK(std::memchr(base + s->text.addr(), 0, s->maxLength + 1) != nullptr, "string %u: \"%.*s\" unterminated",
              i, (int)s->maxLength, (const char*)base + s->text.addr());
        stats.strings++;
    }
    for (u32 i = 0; i < data->numStringRefs; i++) {
        u32 ref = ((u32*)(base + data->stringRefs.addr()))[i];
        CHECK(ref + 4 <= codeStart, "string ref at %#x", ref);
        u32 index = *(u32*)(base + ref);
        CHECK(index < data->numStrings, "string ref %u -> %u of %u", i, index, data->numStrings);
        stats.stringRefs++;
    }
    stats.screens++;
    return true;
}
