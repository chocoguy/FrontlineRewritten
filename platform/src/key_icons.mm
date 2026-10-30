// Keyboard keys drawn as the menus' button icons (NativeRenderKeyIcon in
// native/hooks.h), for game/native/hint_icons.cpp. Drawn with Core Graphics
// and Core Text, which iOS has too. Runs on a game thread, so no operator new
// here (the game replaces it).

#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

#include <cstring>

#include "native/hooks.h"

namespace {

enum Arrow { kNoArrow, kUp, kDown, kLeft, kRight };

// The arrow keys' places in the D-pad picture the menus use (26 x 26, with
// lit 11 x 11 and 10 x 11 arrows laid over it), from the top left.
const CGRect kArrowKeys[] = {
    {{8, 0}, {11, 11}},   // up
    {{8, 15}, {11, 11}},  // down
    {{0, 8}, {10, 11}},   // left
    {{16, 8}, {10, 11}},  // right
};

// What a key shows: Mac keyboards' symbols for the longer names.
const char* KeyCapText(const char* label) {
    static const struct {
        const char* name;
        const char* text;
    } kSymbols[] = {
        {"Return", "↩"}, {"Esc", "esc"}, {"Delete", "⌫"}, {"Tab", "⇥"}, {"Space", "space"},
    };
    for (auto& s : kSymbols) {
        if (std::strcmp(label, s.name) == 0) {
            return s.text;
        }
    }
    return label;
}

void Fill(CGContextRef ctx, CGPathRef path, CGFloat gray, CGFloat alpha) {
    CGContextAddPath(ctx, path);
    CGContextSetGrayFillColor(ctx, gray, alpha);
    CGContextFillPath(ctx);
}

// One key filling `r` (Core Graphics coordinates, y up): a dark base with a
// lighter top face, and the label or an arrow on the face.
void DrawKey(CGContextRef ctx, CGRect r, bool lit, const char* text, Arrow arrow) {
    CGFloat size = CGRectGetWidth(r) < CGRectGetHeight(r) ? CGRectGetWidth(r) : CGRectGetHeight(r);
    CGFloat edge = size * 0.06;
    CGRect body = CGRectInset(r, edge, edge);
    CGFloat radius = size * 0.2;
    CGPathRef base = CGPathCreateWithRoundedRect(body, radius, radius, nullptr);
    Fill(ctx, base, 0.12, 0.9);
    CGRect faceRect = body;
    CGFloat depth = size * 0.1;
    faceRect.origin.y += depth;
    faceRect.size.height -= depth;
    faceRect = CGRectInset(faceRect, size * 0.05, size * 0.04);
    CGPathRef face = CGPathCreateWithRoundedRect(faceRect, radius * 0.8, radius * 0.8, nullptr);
    Fill(ctx, face, lit ? 1.0 : 0.72, 1.0);
    CGPathRelease(face);
    CGPathRelease(base);

    CGFloat ink = 0.1;
    CGPoint c = CGPointMake(CGRectGetMidX(faceRect), CGRectGetMidY(faceRect));
    if (arrow != kNoArrow) {
        CGFloat a = size * 0.22;
        CGPoint p[3];
        switch (arrow) {
        case kUp: p[0] = {c.x, c.y + a}, p[1] = {c.x - a, c.y - a * 0.7}, p[2] = {c.x + a, c.y - a * 0.7}; break;
        case kDown: p[0] = {c.x, c.y - a}, p[1] = {c.x - a, c.y + a * 0.7}, p[2] = {c.x + a, c.y + a * 0.7}; break;
        case kLeft: p[0] = {c.x - a, c.y}, p[1] = {c.x + a * 0.7, c.y + a}, p[2] = {c.x + a * 0.7, c.y - a}; break;
        default: p[0] = {c.x + a, c.y}, p[1] = {c.x - a * 0.7, c.y + a}, p[2] = {c.x - a * 0.7, c.y - a}; break;
        }
        CGContextAddLines(ctx, p, 3);
        CGContextClosePath(ctx);
        CGContextSetGrayFillColor(ctx, ink, 1.0);
        CGContextFillPath(ctx);
        return;
    }

    // The label, as big as fits the face.
    CFStringRef string = CFStringCreateWithCString(nullptr, text, kCFStringEncodingUTF8);
    if (string == nullptr) {
        return;
    }
    CGFloat fontSize = CGRectGetHeight(faceRect) * 0.62;
    for (int tries = 0; tries < 8; tries++) {
        CTFontRef font = CTFontCreateUIFontForLanguage(kCTFontUIFontEmphasizedSystem, fontSize, nullptr);
        CGColorRef color = CGColorCreateGenericGray(ink, 1.0);
        const void* keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName};
        const void* values[] = {font, color};
        CFDictionaryRef attributes = CFDictionaryCreate(nullptr, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
                                                        &kCFTypeDictionaryValueCallBacks);
        CFAttributedStringRef attributed = CFAttributedStringCreate(nullptr, string, attributes);
        CTLineRef line = CTLineCreateWithAttributedString(attributed);
        CGFloat ascent, descent;
        CGFloat width = CTLineGetTypographicBounds(line, &ascent, &descent, nullptr);
        bool fits = width <= CGRectGetWidth(faceRect) * 0.82 || tries == 7;
        if (fits) {
            CGRect glyphs = CTLineGetImageBounds(line, ctx);
            CGContextSetTextPosition(ctx, c.x - width / 2, c.y - CGRectGetMidY(glyphs));
            CTLineDraw(line, ctx);
        }
        CFRelease(line);
        CFRelease(attributed);
        CFRelease(attributes);
        CGColorRelease(color);
        CFRelease(font);
        if (fits) {
            break;
        }
        fontSize *= 0.85;
    }
    CFRelease(string);
}

// `r` in the D-pad picture's coordinates (from the top left) to the
// context's, for a picture `width` x `height`.
CGRect FromPicture(CGRect r, int width, int height) {
    CGFloat sx = width / 26.0, sy = height / 26.0;
    return CGRectMake(r.origin.x * sx, height - (r.origin.y + r.size.height) * sy, r.size.width * sx,
                      r.size.height * sy);
}

}  // namespace

extern "C" void NativeRenderKeyIcon(int icon, const char* label, int width, int height, unsigned char* rgba) {
    std::memset(rgba, 0, (size_t)width * height * 4);
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(rgba, width, height, 8, width * 4, space,
                                             (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(space);
    if (ctx == nullptr) {
        return;
    }
    CGContextSetShouldAntialias(ctx, true);
    CGRect all = CGRectMake(0, 0, width, height);
    switch (icon) {
    case NATIVE_ICON_KEY:
        DrawKey(ctx, all, false, KeyCapText(label != nullptr ? label : ""), kNoArrow);
        break;
    case NATIVE_ICON_ARROW_KEYS:
        for (int i = 0; i < 4; i++) {
            DrawKey(ctx, FromPicture(kArrowKeys[i], width, height), false, "", (Arrow)(kUp + i));
        }
        break;
    default:
        DrawKey(ctx, all, true, "", (Arrow)(kUp + (icon - NATIVE_ICON_ARROW_UP)));
        break;
    }
    CGContextRelease(ctx);

    // Core Graphics draws premultiplied; the game blends straight alpha.
    for (int i = 0; i < width * height; i++) {
        unsigned char* p = rgba + i * 4;
        if (p[3] != 0 && p[3] != 255) {
            for (int k = 0; k < 3; k++) {
                p[k] = (unsigned char)(p[k] * 255 / p[3]);
            }
        }
    }
}
