// GX texture formats to RGBA8. Texels are tiled in blocks and big-endian, as
// the console's texture unit reads them (they stay that way in memory).

#include "gx_texture.h"

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

void Rgb565(uint16_t v, uint8_t* out) {
    out[0] = (uint8_t)(((v >> 11) & 31) * 255 / 31);
    out[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63);
    out[2] = (uint8_t)((v & 31) * 255 / 31);
    out[3] = 255;
}

void Rgb5a3(uint16_t v, uint8_t* out) {
    if (v & 0x8000) {
        out[0] = (uint8_t)(((v >> 10) & 31) * 255 / 31);
        out[1] = (uint8_t)(((v >> 5) & 31) * 255 / 31);
        out[2] = (uint8_t)((v & 31) * 255 / 31);
        out[3] = 255;
    } else {
        out[0] = (uint8_t)(((v >> 8) & 15) * 17);
        out[1] = (uint8_t)(((v >> 4) & 15) * 17);
        out[2] = (uint8_t)((v & 15) * 17);
        out[3] = (uint8_t)(((v >> 12) & 7) * 255 / 7);
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

void Lerp(const uint8_t* a, const uint8_t* b, int wa, int wb, int div, uint8_t* out) {
    for (int i = 0; i < 4; i++) {
        out[i] = (uint8_t)((a[i] * wa + b[i] * wb) / div);
    }
}

// One 4x4 DXT1 sub-block of a CMPR block.
void Cmpr(const uint8_t* src, uint8_t* dst, uint32_t stride, uint32_t x0, uint32_t y0, uint32_t width,
          uint32_t height) {
    uint16_t c0 = Be16(src), c1 = Be16(src + 2);
    uint8_t colors[4][4];
    Rgb565(c0, colors[0]);
    Rgb565(c1, colors[1]);
    if (c0 > c1) {
        Lerp(colors[0], colors[1], 2, 1, 3, colors[2]);
        Lerp(colors[0], colors[1], 1, 2, 3, colors[3]);
    } else {
        Lerp(colors[0], colors[1], 1, 1, 2, colors[2]);
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

}  // namespace frontline::gx
