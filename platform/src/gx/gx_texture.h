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

// Rows of texels instead of blocks, for Rewritten's movies (gx.cpp
// NativeLinearTexture): 8-bit formats only (I8, C8).
uint32_t LinearLevelSize(uint32_t format, uint32_t width, uint32_t height);
void DecodeLinearTexture(const uint8_t* src, uint32_t format, uint32_t width, uint32_t height, const uint8_t* tlut,
                         uint32_t tlutFormat, uint8_t* out);

// Rewritten's mipmaps, for textures the game gave none (gx_metal.mm Texture).
// - The next level down from an RGBA8 level, `width` x `height` (half each
//   way, at least 1). Colors are averaged by their alpha, so the colors of
//   see-through texels don't bleed into the edges.
void Downsample(const uint8_t* src, uint32_t width, uint32_t height, uint8_t* dst);
// - An alpha-tested texture (alpha only near 0 or 255) keeps the share of its
//   texels that pass the game's alpha test at every level, or fences and
//   leaves would thin out and vanish in the distance. The share at level 0,
//   or -1 if the texture isn't one.
float AlphaTestCoverage(const uint8_t* rgba, uint32_t texels);
// - Scales a smaller level's alpha so that share passes.
void KeepCoverage(uint8_t* rgba, uint32_t texels, float coverage);

}  // namespace frontline::gx
