// Runs the game without a window or sound: a smoke test of the platform layer
// and the game's startup on the user's disc.
//   frontline_headless <disc folder> [seconds] [dump folder|-] [buttons]
// With a dump folder, the displayed frame is written there as a PPM image
// every second. Buttons press controller 1's buttons at given times, for a
// moment each, or held for "+seconds": "5:A,8:START+1,12.5:A" (A B X Y Z L R
// START UP DOWN LEFT RIGHT, and SUP SDOWN SLEFT SRIGHT for the main stick).

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "frontline/platform.h"
#include "types.h"

extern int FrontlineGameMain();

namespace {

// The frame as GX rendered it, or else the frame buffer's memory: YUV 4:2:2,
// two pixels per word (Y0 U Y1 V), as the movie player writes it.
void Dump(const char* folder, int second, uint32_t xfb) {
    char path[1024];
    std::snprintf(path, sizeof(path), "%s/frame%03d.ppm", folder, second);
    FILE* f = std::fopen(path, "wb");
    if (f == nullptr) {
        return;
    }
    uint32_t w, h;
    if (frontline::ReadDisplayedFrame(nullptr, &w, &h)) {
        uint8_t* rgba = static_cast<uint8_t*>(std::malloc(w * h * 4));
        frontline::ReadDisplayedFrame(rgba, &w, &h);
        std::fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (uint32_t i = 0; i < w * h; i++) {
            std::fwrite(rgba + i * 4, 1, 3, f);
        }
        std::free(rgba);
        std::fclose(f);
        return;
    }
    const int width = 640, height = 448;
    std::fprintf(f, "P6\n%d %d\n255\n", width, height);
    const u32* words = ADDR_TO_PTR(const u32, xfb);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width / 2; x++) {
            u32 w = words[y * (width / 2) + x];
            int y0 = w >> 24, u = (w >> 16) & 0xFF, y1 = (w >> 8) & 0xFF, v = w & 0xFF;
            for (int yy : {y0, y1}) {
                float c = 1.164f * (yy - 16);
                int rgb[3] = {(int)(c + 1.596f * (v - 128)), (int)(c - 0.392f * (u - 128) - 0.813f * (v - 128)),
                              (int)(c + 2.017f * (u - 128))};
                for (int& k : rgb) {
                    k = k < 0 ? 0 : k > 255 ? 255 : k;
                    std::fputc(k, f);
                }
            }
        }
    }
    std::fclose(f);
}

// Buttons in the low 16 bits; stick directions above them.
u32 Button(const char* name) {
    static const struct {
        const char* name;
        u32 bit;
    } kButtons[] = {{"LEFT", 0x0001}, {"RIGHT", 0x0002}, {"DOWN", 0x0004}, {"UP", 0x0008}, {"Z", 0x0010},
                    {"R", 0x0020},    {"L", 0x0040},     {"A", 0x0100},    {"B", 0x0200},  {"X", 0x0400},
                    {"Y", 0x0800},    {"START", 0x1000}, {"SUP", 0x10000}, {"SDOWN", 0x20000},
                    {"SLEFT", 0x40000}, {"SRIGHT", 0x80000}};
    for (auto& b : kButtons) {
        if (std::strcmp(name, b.name) == 0) {
            return b.bit;
        }
    }
    return 0;
}

struct Press {
    float time;
    float duration;
    u32 button;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <disc folder> [seconds] [dump folder]\n", argv[0]);
        return 2;
    }
    frontline::SetDiscRoot(argv[1]);
    int seconds = argc > 2 ? std::atoi(argv[2]) : 10;
    const char* dump = argc > 3 && std::strcmp(argv[3], "-") != 0 ? argv[3] : nullptr;
    Press presses[64];
    int numPresses = 0;
    if (argc > 4) {
        char script[1024];
        std::snprintf(script, sizeof(script), "%s", argv[4]);
        for (char* item = std::strtok(script, ","); item != nullptr && numPresses < 64; item = std::strtok(nullptr, ",")) {
            char* colon = std::strchr(item, ':');
            if (colon != nullptr) {
                *colon = 0;
                float duration = 0.2f;
                if (char* plus = std::strchr(colon + 1, '+')) {
                    *plus = 0;
                    duration = (float)std::atof(plus + 1);
                }
                presses[numPresses++] = {(float)std::atof(item), duration, Button(colon + 1)};
            }
        }
    }
    PADStatus pad = {};  // controller 1, connected
    frontline::SetPadState(0, pad);
    frontline::StartGame(FrontlineGameMain);
    for (int i = 1; i <= seconds; i++) {
        for (int tick = 0; tick < 20; tick++) {  // 50 ms steps
            float t = (i - 1) + tick * 0.05f;
            u32 held = 0;
            for (int p = 0; p < numPresses; p++) {
                if (t >= presses[p].time && t < presses[p].time + presses[p].duration) {
                    held |= presses[p].button;
                }
            }
            pad.button = (u16)held;
            pad.stickY = (s8)((held & 0x10000) ? 100 : (held & 0x20000) ? -100 : 0);
            pad.stickX = (s8)((held & 0x80000) ? 100 : (held & 0x40000) ? -100 : 0);
            frontline::SetPadState(0, pad);
            usleep(50000);
        }
        frontline::DisplayedFrame f = frontline::GetDisplayedFrame();
        uint32_t w = 0, h = 0;
        bool rendered = frontline::ReadDisplayedFrame(nullptr, &w, &h);
        std::fprintf(stderr, "[headless] %d s: retrace %u, frame buffer %#x%s%s\n", i, f.retrace, f.xfb,
                     f.black ? " (black)" : "", rendered ? " (rendered)" : "");
        if (dump != nullptr && f.xfb != 0) {
            Dump(dump, i, f.xfb);
        }
    }
    // The game's threads are still running: leave without static destructors.
    _exit(0);
}
