// Minimal reader for EA's .viv archives (BIGF and the compact C0FB index).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct VivEntry {
    std::string name;
    std::vector<uint8_t> data;
};

std::vector<VivEntry> ReadViv(const std::filesystem::path& path);
