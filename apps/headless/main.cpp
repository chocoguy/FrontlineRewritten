// Runs the game without a window or sound: a smoke test of the platform layer
// and the game's startup on the user's disc.
//   frontline_headless <disc folder> [seconds] [dump folder|-] [buttons]
// With a dump folder, the displayed frame is written there as a PPM image
// every second. Buttons is a FRONTLINE_INPUT script for controller 1 (see
// platform/src/pad.cpp): "5:A,8:START+1,12.5:A".

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
void Dump(const char* folder, int second, const frontline::DisplayedFrame& frame) {
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
    const int width = frame.width, height = frame.height;
    std::fprintf(f, "P6\n%d %d\n255\n", width, height);
    const u32* words = ADDR_TO_PTR(const u32, frame.xfb);
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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <disc folder> [seconds] [dump folder|-] [buttons]\n", argv[0]);
        return 2;
    }
    frontline::SetDiscRoot(argv[1]);
    frontline::SetAudioOutput(false);  // FRONTLINE_AUDIO_DUMP still records it
    int seconds = argc > 2 ? std::atoi(argv[2]) : 10;
    const char* dump = argc > 3 && std::strcmp(argv[3], "-") != 0 ? argv[3] : nullptr;
    if (argc > 4) {
        setenv("FRONTLINE_INPUT", argv[4], 1);  // pad.cpp
    }
    PADStatus pad = {};  // controller 1, connected
    frontline::SetPadState(0, pad);
    frontline::StartGame(FrontlineGameMain);
    for (int i = 1; i <= seconds; i++) {
        sleep(1);
        frontline::DisplayedFrame f = frontline::GetDisplayedFrame();
        uint32_t w = 0, h = 0;
        bool rendered = frontline::ReadDisplayedFrame(nullptr, &w, &h);
        std::fprintf(stderr, "[headless] %d s: retrace %u, frame buffer %#x%s%s\n", i, f.retrace, f.xfb,
                     f.black ? " (black)" : "", rendered ? " (rendered)" : "");
        if (dump != nullptr && f.xfb != 0) {
            Dump(dump, i, f);
        }
    }
    // The game's threads are still running: leave without static destructors.
    _exit(0);
}
