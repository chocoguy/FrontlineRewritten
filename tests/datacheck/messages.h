#pragma once

#include <cstdint>
#include <vector>

struct MessageStats {
    long messages = 0, rewritten = 0, pressAction = 0;
};

bool CheckMessages(const std::vector<uint8_t>& text, const std::vector<uint8_t>& index, MessageStats& stats);
