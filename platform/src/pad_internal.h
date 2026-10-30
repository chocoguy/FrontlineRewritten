// The controllers' layer (pad.cpp), for the rest of the platform.
#pragma once

#include <cstdint>

namespace frontline {

// Rewritten's high frame rate turns the view by the mouse movement the game
// hasn't drawn yet. LookTakes counts the game's takes of controller 1's
// input (NativeTakeInput). PendingLook is the mouse look (radians, as the app
// sends it) of takes `from` on plus what hasn't been taken; true if the mouse
// has been turning the view lately.
uint32_t LookTakes();
bool PendingLook(uint32_t from, float* lookRight, float* lookUp);
// How fast the mouse has been turning lately: the most of the last two takes
// (radians a take, each way).
void LookRate(float* right, float* up);

}  // namespace frontline
