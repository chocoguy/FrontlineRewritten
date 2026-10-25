// PAD/SI: controllers. The app hands over input snapshots (frontline::SetPadState)
// from its own thread; PADRead copies the latest. As on the console, the SI
// samples once per retrace and runs the sampling callback then (vi.cpp calls
// PadSample), where EA's pad layer reads. PADClamp is the SDK's dead zone and
// octagon clamp, so sticks behave as on the console.
//
// FRONTLINE_INPUT scripts controller 1 for testing: presses at given seconds
// after start, for a moment each or held for "+seconds", on top of the app's
// input: "5:A,8:START+1,12.5:A". Buttons: A B X Y Z L R START UP DOWN LEFT
// RIGHT, and SUP SDOWN SLEFT SRIGHT push the main stick.

#include <pthread.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dolphin/pad.h"
#include "frontline/platform.h"
#include "os/os_internal.h"

namespace {

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
PADStatus g_state[4] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, PAD_ERR_NO_CONTROLLER},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, PAD_ERR_NO_CONTROLLER},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, PAD_ERR_NO_CONTROLLER},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, PAD_ERR_NO_CONTROLLER},
};
u32 g_motor[4];
PADSamplingCallback g_samplingCallback;

void ClampStick(s8* px, s8* py, s8 max, s8 xy, s8 min) {
    int x = *px;
    int y = *py;
    int signX = x < 0 ? -1 : 1;
    int signY = y < 0 ? -1 : 1;
    x *= signX;
    y *= signY;
    x = x <= min ? 0 : x - min;
    y = y <= min ? 0 : y - min;
    if (x == 0 && y == 0) {
        *px = *py = 0;
        return;
    }
    int d = xy * y <= xy * x ? xy * x + (max - xy) * y : xy * y + (max - xy) * x;
    if (xy * max < d) {
        x = (s8)(xy * max * x / d);
        y = (s8)(xy * max * y / d);
    }
    *px = (s8)(signX * x);
    *py = (s8)(signY * y);
}

// FRONTLINE_INPUT. Buttons in the low 16 bits; stick directions above them.
struct Press {
    float time;
    float duration;
    u32 bits;
};
Press g_script[256];
int g_scriptLength = -1;

u32 ScriptBit(const char* name) {
    static const struct {
        const char* name;
        u32 bit;
    } kNames[] = {{"LEFT", 0x0001},   {"RIGHT", 0x0002}, {"DOWN", 0x0004},    {"UP", 0x0008},     {"Z", 0x0010},
                  {"R", 0x0020},      {"L", 0x0040},     {"A", 0x0100},       {"B", 0x0200},      {"X", 0x0400},
                  {"Y", 0x0800},      {"START", 0x1000}, {"SUP", 0x10000},    {"SDOWN", 0x20000}, {"SLEFT", 0x40000},
                  {"SRIGHT", 0x80000}};
    for (auto& n : kNames) {
        if (std::strcmp(name, n.name) == 0) {
            return n.bit;
        }
    }
    std::fprintf(stderr, "frontline: FRONTLINE_INPUT: no button %s\n", name);
    return 0;
}

void LoadScript() {
    g_scriptLength = 0;
    const char* env = std::getenv("FRONTLINE_INPUT");
    if (env == nullptr) {
        return;
    }
    static char script[8192];
    std::snprintf(script, sizeof(script), "%s", env);
    char* save;
    for (char* item = strtok_r(script, ",", &save); item != nullptr && g_scriptLength < 256;
         item = strtok_r(nullptr, ",", &save)) {
        char* colon = std::strchr(item, ':');
        if (colon == nullptr) {
            continue;
        }
        *colon = 0;
        float duration = 0.2f;
        if (char* plus = std::strchr(colon + 1, '+')) {
            *plus = 0;
            duration = (float)std::atof(plus + 1);
        }
        g_script[g_scriptLength++] = {(float)std::atof(item), duration, ScriptBit(colon + 1)};
    }
}

// Controller 1 with the script's presses at this moment.
void ApplyScript(PADStatus* status) {
    if (g_scriptLength < 0) {
        LoadScript();
    }
    if (g_scriptLength == 0) {
        return;
    }
    float t = (float)frontline::os::Now() / OS_TIMER_CLOCK;
    u32 held = 0;
    for (int i = 0; i < g_scriptLength; i++) {
        if (t >= g_script[i].time && t < g_script[i].time + g_script[i].duration) {
            held |= g_script[i].bits;
        }
    }
    status->err = PAD_ERR_NONE;
    status->button |= (u16)held;
    if (held & 0x30000) {
        status->stickY = (held & 0x10000) ? 100 : -100;
    }
    if (held & 0xC0000) {
        status->stickX = (held & 0x80000) ? 100 : -100;
    }
}

void ClampTrigger(u8* trigger, u8 min, u8 max) {
    if (*trigger <= min) {
        *trigger = 0;
        return;
    }
    if (*trigger > max) {
        *trigger = max;
    }
    *trigger -= min;
}

}  // namespace

void PadSample() {
    if (g_samplingCallback != nullptr) {
        g_samplingCallback();
    }
}

void frontline::SetPadState(int port, const PADStatus& status) {
    pthread_mutex_lock(&g_lock);
    g_state[port] = status;
    pthread_mutex_unlock(&g_lock);
}

uint32_t frontline::GetPadMotor(int port) { return g_motor[port]; }

extern "C" {

u32 PADRead(PADStatus* status) {
    frontline::os::SafePoint();
    pthread_mutex_lock(&g_lock);
    std::memcpy(status, g_state, sizeof(g_state));
    pthread_mutex_unlock(&g_lock);
    ApplyScript(&status[0]);
    return 0;
}

void PADClamp(PADStatus* status) {
    for (int i = 0; i < 4; i++, status++) {
        if (status->err != PAD_ERR_NONE) {
            continue;
        }
        ClampStick(&status->stickX, &status->stickY, 72, 40, 15);
        ClampStick(&status->substickX, &status->substickY, 59, 31, 15);
        ClampTrigger(&status->triggerLeft, 30, 180);
        ClampTrigger(&status->triggerRight, 30, 180);
    }
}

BOOL PADReset(u32) { return TRUE; }
BOOL PADRecalibrate(u32) { return TRUE; }

void PADControlMotor(s32 chan, u32 command) {
    if (chan >= 0 && chan < 4) {
        g_motor[chan] = command;
    }
}

PADSamplingCallback PADSetSamplingCallback(PADSamplingCallback callback) {
    PADSamplingCallback prev = g_samplingCallback;
    g_samplingCallback = callback;
    return prev;
}

BOOL __PADDisableRecalibration(BOOL) { return FALSE; }

u32 SIProbe(s32 chan) {
    pthread_mutex_lock(&g_lock);
    bool connected = g_state[chan].err == PAD_ERR_NONE;
    pthread_mutex_unlock(&g_lock);
    return connected ? 0x09000000 : 0x00000008;  // SI_GC_CONTROLLER : SI_ERROR_NO_RESPONSE
}

void SISetSamplingRate(u32) {}

}  // extern "C"
