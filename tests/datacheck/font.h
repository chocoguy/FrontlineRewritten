#pragma once

#include <cstdint>
#include <vector>

struct FontStats {
    long fonts = 0, wide = 0, chars = 0, kerns = 0;
};

bool CheckFont(uint8_t* data, FontStats& stats);
bool CheckFont(const std::vector<uint8_t>& file, FontStats& stats);
