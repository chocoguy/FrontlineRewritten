// Shared by the format checks.
#pragma once

#include <cstdint>
#include <cstdio>
#include <vector>

extern int g_failures;
extern uint8_t* g_next;

// Copies a file into the arena (where the game's heaps are), 32-byte aligned.
void* ArenaCopy(const std::vector<uint8_t>& data);

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            std::printf("    FAIL %s: ", #cond);          \
            std::printf(__VA_ARGS__);                     \
            std::printf("\n");                            \
            g_failures++;                                 \
            return false;                                 \
        }                                                 \
    } while (0)
