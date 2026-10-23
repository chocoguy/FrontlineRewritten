#pragma once

#include <cstdint>
#include <vector>

struct MpfStats {
    long maps = 0, legacy = 0, nodes = 0, branches = 0, waves = 0, fades = 0, remaps = 0;
};

bool CheckMPF(const std::vector<uint8_t>& file, MpfStats& stats);
