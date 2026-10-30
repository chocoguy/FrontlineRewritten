// GX texture formats to RGBA8. Texels are tiled in blocks and big-endian, as
// the console's texture unit reads them (they stay that way in memory).

#include "gx_texture.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace frontline::gx {
namespace {

uint16_t Be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

struct BlockSize {
    uint32_t w, h, bytes;
};

BlockSize Block(uint32_t format) {
    switch (format) {
    case kTexI4:
    case kTexC4:
    case kTexCMPR:
        return {8, 8, 32};
    case kTexI8:
    case kTexIA4:
    case kTexC8:
        return {8, 4, 32};
    case kTexRGBA8:
        return {4, 4, 64};
    default:  // IA8, RGB565, RGB5A3, C14X2
        return {4, 4, 32};
    }
}

// Fewer bits to 8, as the texture unit widens them: the bits repeated.
uint8_t Expand5(uint32_t v) { return (uint8_t)(v << 3 | v >> 2); }
uint8_t Expand6(uint32_t v) { return (uint8_t)(v << 2 | v >> 4); }
uint8_t Expand3(uint32_t v) { return (uint8_t)(v << 5 | v << 2 | v >> 1); }

void Rgb565(uint16_t v, uint8_t* out) {
    out[0] = Expand5((v >> 11) & 31);
    out[1] = Expand6((v >> 5) & 63);
    out[2] = Expand5(v & 31);
    out[3] = 255;
}

void Rgb5a3(uint16_t v, uint8_t* out) {
    if (v & 0x8000) {
        out[0] = Expand5((v >> 10) & 31);
        out[1] = Expand5((v >> 5) & 31);
        out[2] = Expand5(v & 31);
        out[3] = 255;
    } else {
        out[0] = (uint8_t)(((v >> 8) & 15) * 17);
        out[1] = (uint8_t)(((v >> 4) & 15) * 17);
        out[2] = (uint8_t)((v & 15) * 17);
        out[3] = Expand3((v >> 12) & 7);
    }
}

void Palette(const uint8_t* tlut, uint32_t tlutFormat, uint32_t index, uint8_t* out) {
    uint16_t v = Be16(tlut + index * 2);
    switch (tlutFormat) {
    case 0:  // IA8
        out[0] = out[1] = out[2] = (uint8_t)v;
        out[3] = (uint8_t)(v >> 8);
        break;
    case 1:
        Rgb565(v, out);
        break;
    default:
        Rgb5a3(v, out);
        break;
    }
}

// (a * wa + b * wb) >> shift, each channel.
void Lerp(const uint8_t* a, const uint8_t* b, int wa, int wb, int shift, uint8_t* out) {
    for (int i = 0; i < 4; i++) {
        out[i] = (uint8_t)((a[i] * wa + b[i] * wb) >> shift);
    }
}

// One 4x4 DXT1 sub-block of a CMPR block. The console puts the colors between
// the two ends at 3/8 and 5/8, not DXT1's thirds (Dolphin's DXTBlend).
void Cmpr(const uint8_t* src, uint8_t* dst, uint32_t stride, uint32_t x0, uint32_t y0, uint32_t width,
          uint32_t height) {
    uint16_t c0 = Be16(src), c1 = Be16(src + 2);
    uint8_t colors[4][4];
    Rgb565(c0, colors[0]);
    Rgb565(c1, colors[1]);
    if (c0 > c1) {
        Lerp(colors[0], colors[1], 5, 3, 3, colors[2]);
        Lerp(colors[0], colors[1], 3, 5, 3, colors[3]);
    } else {
        Lerp(colors[0], colors[1], 1, 1, 1, colors[2]);
        colors[3][0] = colors[2][0], colors[3][1] = colors[2][1], colors[3][2] = colors[2][2];
        colors[3][3] = 0;
    }
    for (uint32_t y = 0; y < 4; y++) {
        uint8_t bits = src[4 + y];
        for (uint32_t x = 0; x < 4; x++) {
            uint32_t px = x0 + x, py = y0 + y;
            if (px < width && py < height) {
                std::memcpy(dst + (py * stride + px) * 4, colors[(bits >> (6 - x * 2)) & 3], 4);
            }
        }
    }
}

// The alpha the game's alpha-tested materials must pass (a > 160:
// GXSetAlphaCompare in compartment.cpp and staticmesh.cpp).
constexpr uint32_t kAlphaTestRef = 160;

}  // namespace

uint32_t TextureLevelSize(uint32_t format, uint32_t width, uint32_t height) {
    BlockSize b = Block(format);
    uint32_t bw = (width + b.w - 1) / b.w;
    uint32_t bh = (height + b.h - 1) / b.h;
    return bw * bh * b.bytes;
}

void DecodeTexture(const uint8_t* src, uint32_t format, uint32_t width, uint32_t height, const uint8_t* tlut,
                   uint32_t tlutFormat, uint8_t* out) {
    BlockSize b = Block(format);
    uint32_t blocksX = (width + b.w - 1) / b.w;
    uint32_t blocksY = (height + b.h - 1) / b.h;
    for (uint32_t by = 0; by < blocksY; by++) {
        for (uint32_t bx = 0; bx < blocksX; bx++, src += b.bytes) {
            if (format == kTexCMPR) {
                for (uint32_t s = 0; s < 4; s++) {
                    Cmpr(src + s * 8, out, width, bx * 8 + (s & 1) * 4, by * 8 + (s >> 1) * 4, width, height);
                }
                continue;
            }
            for (uint32_t y = 0; y < b.h; y++) {
                for (uint32_t x = 0; x < b.w; x++) {
                    uint32_t px = bx * b.w + x, py = by * b.h + y;
                    uint32_t i = y * b.w + x;
                    uint8_t texel[4];
                    switch (format) {
                    case kTexI4: {
                        uint8_t v = (src[i / 2] >> ((i & 1) ? 0 : 4)) & 15;
                        texel[0] = texel[1] = texel[2] = texel[3] = (uint8_t)(v * 17);
                        break;
                    }
                    case kTexI8:
                        texel[0] = texel[1] = texel[2] = texel[3] = src[i];
                        break;
                    case kTexIA4:
                        texel[0] = texel[1] = texel[2] = (uint8_t)((src[i] & 15) * 17);
                        texel[3] = (uint8_t)((src[i] >> 4) * 17);
                        break;
                    case kTexIA8:
                        texel[3] = src[i * 2];
                        texel[0] = texel[1] = texel[2] = src[i * 2 + 1];
                        break;
                    case kTexRGB565:
                        Rgb565(Be16(src + i * 2), texel);
                        break;
                    case kTexRGB5A3:
                        Rgb5a3(Be16(src + i * 2), texel);
                        break;
                    case kTexRGBA8:
                        texel[3] = src[i * 2];
                        texel[0] = src[i * 2 + 1];
                        texel[1] = src[32 + i * 2];
                        texel[2] = src[32 + i * 2 + 1];
                        break;
                    case kTexC4:
                        Palette(tlut, tlutFormat, (src[i / 2] >> ((i & 1) ? 0 : 4)) & 15, texel);
                        break;
                    case kTexC8:
                        Palette(tlut, tlutFormat, src[i], texel);
                        break;
                    case kTexC14X2:
                        Palette(tlut, tlutFormat, Be16(src + i * 2) & 0x3FFF, texel);
                        break;
                    default:
                        texel[0] = 255, texel[1] = 0, texel[2] = 255, texel[3] = 255;
                        break;
                    }
                    if (px < width && py < height) {
                        std::memcpy(out + (py * width + px) * 4, texel, 4);
                    }
                }
            }
        }
    }
}

uint32_t LinearLevelSize(uint32_t, uint32_t width, uint32_t height) { return width * height; }

void DecodeLinearTexture(const uint8_t* src, uint32_t format, uint32_t width, uint32_t height, const uint8_t* tlut,
                         uint32_t tlutFormat, uint8_t* out) {
    for (uint32_t i = 0; i < width * height; i++, out += 4) {
        if (format == kTexC8) {
            Palette(tlut, tlutFormat, src[i], out);
        } else {  // I8
            out[0] = out[1] = out[2] = out[3] = src[i];
        }
    }
}

void Downsample(const uint8_t* src, uint32_t width, uint32_t height, uint8_t* dst) {
    const uint32_t w = width > 1 ? width / 2 : 1, h = height > 1 ? height / 2 : 1;
    for (uint32_t y = 0; y < h; y++) {
        const uint32_t y0 = std::min(y * 2, height - 1), y1 = std::min(y * 2 + 1, height - 1);
        for (uint32_t x = 0; x < w; x++, dst += 4) {
            const uint32_t x0 = std::min(x * 2, width - 1), x1 = std::min(x * 2 + 1, width - 1);
            const uint8_t* p[4] = {src + (y0 * width + x0) * 4, src + (y0 * width + x1) * 4,
                                   src + (y1 * width + x0) * 4, src + (y1 * width + x1) * 4};
            const uint32_t alpha = p[0][3] + p[1][3] + p[2][3] + p[3][3];
            for (int c = 0; c < 3; c++) {
                dst[c] = alpha != 0 ? (uint8_t)((p[0][c] * p[0][3] + p[1][c] * p[1][3] + p[2][c] * p[2][3] +
                                                 p[3][c] * p[3][3] + alpha / 2) /
                                                alpha)
                                    : (uint8_t)((p[0][c] + p[1][c] + p[2][c] + p[3][c] + 2) / 4);
            }
            dst[3] = (uint8_t)((alpha + 2) / 4);
        }
    }
}

float AlphaTestCoverage(const uint8_t* rgba, uint32_t texels) {
    uint32_t passing = 0;
    for (uint32_t i = 0; i < texels; i++) {
        const uint8_t a = rgba[i * 4 + 3];
        if (a > 8 && a < 247) {
            return -1.0f;  // see-through in between: blended, not tested
        }
        passing += a > kAlphaTestRef;
    }
    return passing != 0 && passing != texels ? (float)passing / texels : -1.0f;
}

void KeepCoverage(uint8_t* rgba, uint32_t texels, float coverage) {
    uint32_t histogram[256] = {};
    for (uint32_t i = 0; i < texels; i++) {
        histogram[rgba[i * 4 + 3]]++;
    }
    // The alpha at which the share passing matches: the texels at or above
    // it are scaled to just pass.
    const uint32_t want = std::max(1u, (uint32_t)std::lround(coverage * texels));
    uint32_t above = 0, q = 255;
    for (; q > 0; q--) {
        above += histogram[q];
        if (above >= want) {
            break;
        }
    }
    if (q == 0) {
        return;
    }
    for (uint32_t i = 0; i < texels; i++) {
        uint8_t& a = rgba[i * 4 + 3];
        a = (uint8_t)std::min(255u, a * (kAlphaTestRef + 1) / q);
    }
}

}  // namespace frontline::gx
