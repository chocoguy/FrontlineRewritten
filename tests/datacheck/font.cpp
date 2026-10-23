// Fonts (.gfn, and the one compiled into the game): ea/sys/font.h's swap.

#include "font.h"

#include <cmath>

#include "check.h"
#include "types.h"
#include "ea/sys/font.h"

bool CheckFont(uint8_t* data, FontStats& stats) {
    FONT* font = (FONT*)data;
    CHECK(FontFromBigEndian(font), "magic %.4s", (char*)data);
    CHECK(!FontFromBigEndian(font), "swapped twice");
    bool wide = (font->flags & FONT_FLAG_WIDE) != 0;
    CHECK(font->numChars > 0 && font->charOffset >= 0x74 && font->shapeOffset > font->charOffset,
          "%u characters at %#x, texture at %#x", font->numChars, font->charOffset, font->shapeOffset);
    CHECK(std::isfinite(font->scaleX) && std::isfinite(font->scaleY) && font->scaleX > 0 && font->scaleY > 0,
          "scale %f %f", font->scaleX, font->scaleY);
    Shape* shape = (Shape*)(data + font->shapeOffset);
    CHECK(shape->width > 0 && shape->width <= 1024 && shape->height > 0 && shape->height <= 1024,
          "texture %d x %d", shape->width, shape->height);
    int prev = -1;
    for (int i = 0; i < font->numChars; i++) {
        FONTCHAR* ch = (FONTCHAR*)(data + font->charOffset + i * (wide ? 0x10 : 0xC));
        CHECK(ch->code > prev, "character %d code %#x after %#x (FONT_bsearch needs them sorted)", i, ch->code, prev);
        CHECK(ch->texX + ch->width <= shape->width && ch->texY + ch->height <= shape->height,
              "character %#x at %u,%u size %u x %u", ch->code, ch->texX, ch->texY, ch->width, ch->height);
        prev = ch->code;
        stats.chars++;
    }
    if (font->kernOffset != 0) {
        s32 count = *(s32*)(data + font->kernOffset);
        CHECK(count >= 0 && count < 0x10000 && font->kernOffset + 4 + count * 4 <= font->shapeOffset, "%d kerns",
              count);
        FONTKERN* kern = (FONTKERN*)(data + font->kernOffset + 4);
        for (int i = 0; i < count; i++) {
            CHECK(kern[i].prev <= prev && kern[i].amount > -64 && kern[i].amount < 64, "kern %d: %#x %d", i,
                  kern[i].prev, kern[i].amount);
        }
        stats.kerns += count;
    }
    stats.fonts++;
    stats.wide += wide;
    return true;
}

bool CheckFont(const std::vector<uint8_t>& file, FontStats& stats) {
    return CheckFont((uint8_t*)ArenaCopy(file), stats);
}
