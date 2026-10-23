// GX texture formats (gx_texture.cpp).
#pragma once

#include <cstdint>

namespace frontline::gx {

enum TexFormat : uint32_t {
    kTexI4 = 0,
    kTexI8 = 1,
    kTexIA4 = 2,
    kTexIA8 = 3,
    kTexRGB565 = 4,
    kTexRGB5A3 = 5,
    kTexRGBA8 = 6,
    kTexC4 = 8,
    kTexC8 = 9,
    kTexC14X2 = 10,
    kTexCMPR = 14,
};

// Bytes of one mip level.
uint32_t TextureLevelSize(uint32_t format, uint32_t width, uint32_t height);

// Decodes one level to RGBA8 (width x height). `tlut` is the palette (big-endian
// entries) for the color-index formats.
void DecodeTexture(const uint8_t* src, uint32_t format, uint32_t width, uint32_t height, const uint8_t* tlut,
                   uint32_t tlutFormat, uint8_t* out);

}  // namespace frontline::gx
