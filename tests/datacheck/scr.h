#pragma once

#include <cstdint>
#include <map>
#include <vector>

struct ScrStats {
    long screens = 0, controls = 0, handlers = 0, strings = 0, stringRefs = 0;
    std::map<int, long> objects;  // by plugin type
};

bool CheckSCR(const std::vector<uint8_t>& file, ScrStats& stats);
