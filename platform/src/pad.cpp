// PAD/SI: controllers. The app hands over input snapshots (frontline::SetPadState)
// from its own thread; PADRead copies the latest. As on the console, the SI
// samples once per retrace and runs the sampling callback then (vi.cpp calls
// PadSample), where EA's pad layer reads. PADClamp is the SDK's dead zone and
// octagon clamp, so sticks behave as on the console.

#include <pthread.h>

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
