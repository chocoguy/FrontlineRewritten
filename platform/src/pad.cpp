// PAD/SI: controllers. The app hands over input snapshots (frontline::SetPadState)
// from its own thread; PADRead copies the latest. As on the console, the SI
// samples once per retrace and runs the sampling callback then (vi.cpp calls
// PadSample), where EA's pad layer reads. PADClamp is the SDK's dead zone and
// octagon clamp, so sticks behave as on the console.
//
// The keyboard and mouse reach the game as game actions (native/hooks.h), not
// as controller buttons: the app sets what's held and how far the mouse moved
// (SetKeyboardMouse), and CInputManager takes it once a frame (NativeTakeInput).
// Whichever of them or a controller was used last picks what the on-screen
// controls show (NativePromptDevice), with the keys' names from the app
// (SetKeyLabel).
//
// FRONTLINE_INPUT scripts controller 1 for testing: presses at given seconds
// after start, for a moment each or held for "+seconds", on top of the app's
// input: "5:A,8:START+1,12.5:A". Buttons: A B X Y Z L R START UP DOWN LEFT
// RIGHT, and SUP SDOWN SLEFT SRIGHT push the main stick. Names starting with @
// are keyboard and mouse actions: @FIRE, @JUMP, @MENU_A, ... (NATIVE_ACTION_*),
// @LOOKLEFT @LOOKRIGHT @LOOKUP @LOOKDOWN turn the mouse at a radian a second,
// and @WHEELUP @WHEELDOWN scroll a notch.
//
// The high frame rate turns the view at the screen's rate by the mouse
// movement the game hasn't drawn yet (PendingLook), so the takes are kept.

#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <cmath>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dolphin/pad.h"
#include "frontline/platform.h"
#include "native/hooks.h"
#include "os/os_internal.h"
#include "pad_internal.h"

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

// The keyboard and mouse, as the app left them since the game's last take.
NativeInput g_keyboardMouse[4];
bool g_playing;

// The device the on-screen controls show, and how often it changed.
int g_promptDevice = NATIVE_DEVICE_KEYBOARD;
unsigned g_promptDeviceChanges;

// What the keys are called, as the app binds them (Input.swift) until it says.
char g_keyLabels[NATIVE_NUM_LABELS][32] = {
    "F", "C", "Right Mouse Button", "Esc", "H", "Mouse Wheel", "Return", "Esc", "X", "Y",
};

void UsedDevice(int device) {
    if (__atomic_load_n(&g_promptDevice, __ATOMIC_RELAXED) != device) {
        __atomic_store_n(&g_promptDevice, device, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_promptDeviceChanges, 1, __ATOMIC_RELEASE);
    }
}

// Pressed or pushed well past the dead zone (a resting stick doesn't count).
bool PadInUse(const PADStatus& s) {
    auto far = [](int v, int limit) { return v > limit || v < -limit; };
    return s.err == PAD_ERR_NONE &&
           (s.button != 0 || far(s.stickX, 30) || far(s.stickY, 30) || far(s.substickX, 30) ||
            far(s.substickY, 30) || s.triggerLeft > 76 || s.triggerRight > 76);
}

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
// Keyboard and mouse items have `native` set, with action bits or a kScript*.
struct Press {
    float time;
    float duration;
    u32 bits;
    bool native;
};
Press g_script[256];
int g_scriptLength = -1;
std::atomic<float> g_scriptLastTake{-1};  // when the game last took the script's mouse

enum : u32 {
    kScriptLookLeft = 1,
    kScriptLookRight,
    kScriptLookUp,
    kScriptLookDown,
    kScriptWheelUp,
    kScriptWheelDown,
};

u32 ScriptNativeBit(const char* name) {
    static const struct {
        const char* name;
        u32 bit;
    } kNames[] = {
        {"FORWARD", NATIVE_ACTION_FORWARD},
        {"BACK", NATIVE_ACTION_BACK},
        {"LEFT", NATIVE_ACTION_LEFT},
        {"RIGHT", NATIVE_ACTION_RIGHT},
        {"WALK", NATIVE_ACTION_WALK},
        {"JUMP", NATIVE_ACTION_JUMP},
        {"CROUCH", NATIVE_ACTION_CROUCH},
        {"AIM", NATIVE_ACTION_AIM},
        {"FIRE", NATIVE_ACTION_FIRE},
        {"RELOAD", NATIVE_ACTION_RELOAD},
        {"NEXT_WEAPON", NATIVE_ACTION_NEXT_WEAPON},
        {"PREV_WEAPON", NATIVE_ACTION_PREV_WEAPON},
        {"MELEE", NATIVE_ACTION_MELEE},
        {"USE", NATIVE_ACTION_USE},
        {"CENTER_VIEW", NATIVE_ACTION_CENTER_VIEW},
        {"HINT", NATIVE_ACTION_HINT},
        {"SCORES", NATIVE_ACTION_SCORES},
        {"PAUSE", NATIVE_ACTION_PAUSE},
        {"MENU_UP", NATIVE_ACTION_MENU_UP},
        {"MENU_DOWN", NATIVE_ACTION_MENU_DOWN},
        {"MENU_LEFT", NATIVE_ACTION_MENU_LEFT},
        {"MENU_RIGHT", NATIVE_ACTION_MENU_RIGHT},
        {"MENU_A", NATIVE_ACTION_MENU_A},
        {"MENU_B", NATIVE_ACTION_MENU_B},
        {"MENU_X", NATIVE_ACTION_MENU_X},
        {"MENU_Y", NATIVE_ACTION_MENU_Y},
        {"MENU_Z", NATIVE_ACTION_MENU_Z},
        {"MENU_L", NATIVE_ACTION_MENU_L},
        {"MENU_R", NATIVE_ACTION_MENU_R},
        {"MENU_START", NATIVE_ACTION_MENU_START},
        {"LOOKLEFT", kScriptLookLeft},
        {"LOOKRIGHT", kScriptLookRight},
        {"LOOKUP", kScriptLookUp},
        {"LOOKDOWN", kScriptLookDown},
        {"WHEELUP", kScriptWheelUp},
        {"WHEELDOWN", kScriptWheelDown},
    };
    for (auto& n : kNames) {
        if (std::strcmp(name, n.name) == 0) {
            return n.bit;
        }
    }
    std::fprintf(stderr, "frontline: FRONTLINE_INPUT: no action @%s\n", name);
    return 0;
}

bool IsScriptMouse(u32 bits) { return bits >= kScriptLookLeft && bits <= kScriptWheelDown; }

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
        const char* name = colon + 1;
        bool native = name[0] == '@';
        g_script[g_scriptLength++] = {(float)std::atof(item), duration,
                                      native ? ScriptNativeBit(name + 1) : ScriptBit(name), native};
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
        if (!g_script[i].native && t >= g_script[i].time && t < g_script[i].time + g_script[i].duration) {
            held |= g_script[i].bits;
        }
    }
    status->err = PAD_ERR_NONE;
    status->button |= (u16)held;
    if (held != 0) {
        UsedDevice(NATIVE_DEVICE_CONTROLLER);
    }
    if (held & 0x30000) {
        status->stickY = (held & 0x10000) ? 100 : -100;
    }
    if (held & 0xC0000) {
        status->stickX = (held & 0x80000) ? 100 : -100;
    }
}

// The script's mouse look from `last` to `t` (seconds), added to `input`.
void ScriptLook(float last, float t, NativeInput* input) {
    for (int i = 0; i < g_scriptLength; i++) {
        const Press& p = g_script[i];
        if (!p.native || !IsScriptMouse(p.bits) || p.bits == kScriptWheelUp || p.bits == kScriptWheelDown) {
            continue;
        }
        float from = last > p.time ? last : p.time;
        float to = t < p.time + p.duration ? t : p.time + p.duration;
        float turn = to > from ? to - from : 0;
        switch (p.bits) {
        case kScriptLookLeft: input->lookRight -= turn; break;
        case kScriptLookRight: input->lookRight += turn; break;
        case kScriptLookUp: input->lookUp += turn; break;
        case kScriptLookDown: input->lookUp -= turn; break;
        }
    }
}

// The script's keyboard and mouse at this moment, added to `input`.
void ApplyNativeScript(NativeInput* input) {
    if (g_scriptLength < 0) {
        LoadScript();
    }
    float t = (float)frontline::os::Now() / OS_TIMER_CLOCK;
    float last = g_scriptLastTake < 0 ? t : g_scriptLastTake.load();
    g_scriptLastTake = t;
    for (int i = 0; i < g_scriptLength; i++) {
        const Press& p = g_script[i];
        if (!p.native) {
            continue;
        }
        if (!IsScriptMouse(p.bits)) {
            if (t >= p.time && t < p.time + p.duration) {
                input->held |= p.bits;
            }
            continue;
        }
        if (p.bits == kScriptWheelUp || p.bits == kScriptWheelDown) {
            if (p.time > last && p.time <= t) {
                input->wheel += p.bits == kScriptWheelUp ? 1 : -1;
            }
        }
    }
    // Radians a second over the part of each hold since the last take.
    ScriptLook(last, t, input);
}

// The mouse look of controller 1's recent takes (the high frame rate's).
struct Take {
    uint32_t number;
    float lookRight, lookUp;
};
constexpr uint32_t kTakes = 8;
Take g_takes[kTakes];
uint32_t g_numTakes;  // (under g_lock)

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
    if (PadInUse(status)) {
        UsedDevice(NATIVE_DEVICE_CONTROLLER);
    }
}

uint32_t frontline::GetPadMotor(int port) { return g_motor[port]; }

void frontline::SetKeyboardMouse(int port, uint32_t held, float lookRight, float lookUp, int wheel) {
    pthread_mutex_lock(&g_lock);
    NativeInput& in = g_keyboardMouse[port];
    in.pressed |= held & ~in.held;
    in.held = held;
    in.lookRight += lookRight;
    in.lookUp += lookUp;
    in.wheel += wheel;
    pthread_mutex_unlock(&g_lock);
    if (held != 0 || lookRight != 0 || lookUp != 0 || wheel != 0) {
        UsedDevice(NATIVE_DEVICE_KEYBOARD);
    }
}

void frontline::PressAction(int port, uint32_t action) {
    pthread_mutex_lock(&g_lock);
    g_keyboardMouse[port].pressed |= action;
    pthread_mutex_unlock(&g_lock);
}

void frontline::AddMouseLook(int port, float lookRight, float lookUp) {
    pthread_mutex_lock(&g_lock);
    g_keyboardMouse[port].lookRight += lookRight;
    g_keyboardMouse[port].lookUp += lookUp;
    pthread_mutex_unlock(&g_lock);
    if (lookRight != 0 || lookUp != 0) {
        UsedDevice(NATIVE_DEVICE_KEYBOARD);
    }
}

void frontline::SetKeyLabel(int label, const char* text) {
    if (label >= 0 && label < NATIVE_NUM_LABELS) {
        pthread_mutex_lock(&g_lock);
        std::snprintf(g_keyLabels[label], sizeof(g_keyLabels[label]), "%s", text);
        pthread_mutex_unlock(&g_lock);
    }
}

extern "C" int NativePromptDevice(void) { return __atomic_load_n(&g_promptDevice, __ATOMIC_RELAXED); }

extern "C" unsigned int NativePromptDeviceChanges(void) {
    return __atomic_load_n(&g_promptDeviceChanges, __ATOMIC_ACQUIRE);
}

// The app sets labels at startup and again when keys are rebound, so the
// game reads a copy of its own (prompts and icons are made as screens load).
extern "C" const char* NativeKeyLabel(int label) {
    if (label < 0 || label >= NATIVE_NUM_LABELS) {
        return "";
    }
    static thread_local char copies[NATIVE_NUM_LABELS][sizeof(g_keyLabels[0])];
    pthread_mutex_lock(&g_lock);
    std::memcpy(copies[label], g_keyLabels[label], sizeof(copies[label]));
    pthread_mutex_unlock(&g_lock);
    return copies[label];
}

bool frontline::IsPlaying() { return __atomic_load_n(&g_playing, __ATOMIC_RELAXED); }

extern "C" void NativeTakeInput(int port, int playing, NativeInput* input) {
    pthread_mutex_lock(&g_lock);
    *input = g_keyboardMouse[port];
    g_keyboardMouse[port].pressed = 0;
    g_keyboardMouse[port].lookRight = 0;
    g_keyboardMouse[port].lookUp = 0;
    g_keyboardMouse[port].wheel = 0;
    pthread_mutex_unlock(&g_lock);
    if (port == 0) {
        __atomic_store_n(&g_playing, playing != 0, __ATOMIC_RELAXED);
        NativeInput script = {};
        ApplyNativeScript(&script);
        if (script.held != 0 || script.lookRight != 0 || script.lookUp != 0 || script.wheel != 0) {
            UsedDevice(NATIVE_DEVICE_KEYBOARD);
        }
        input->held |= script.held;
        input->lookRight += script.lookRight;
        input->lookUp += script.lookUp;
        input->wheel += script.wheel;
        pthread_mutex_lock(&g_lock);
        g_takes[g_numTakes % kTakes] = {g_numTakes, input->lookRight, input->lookUp};
        g_numTakes++;
        pthread_mutex_unlock(&g_lock);
    }
}

uint32_t frontline::LookTakes() {
    pthread_mutex_lock(&g_lock);
    uint32_t n = g_numTakes;
    pthread_mutex_unlock(&g_lock);
    return n;
}

void frontline::LookRate(float* right, float* up) {
    float r = 0, u = 0;
    pthread_mutex_lock(&g_lock);
    for (uint32_t n = g_numTakes > 2 ? g_numTakes - 2 : 0; n < g_numTakes; n++) {
        r = std::max(r, std::fabs(g_takes[n % kTakes].lookRight));
        u = std::max(u, std::fabs(g_takes[n % kTakes].lookUp));
    }
    pthread_mutex_unlock(&g_lock);
    *right = r;
    *up = u;
}

bool frontline::PendingLook(uint32_t from, float* lookRight, float* lookUp) {
    NativeInput look = {};
    bool recent = false;
    pthread_mutex_lock(&g_lock);
    for (uint32_t n = from; n < g_numTakes; n++) {
        if (g_numTakes - n <= kTakes) {
            look.lookRight += g_takes[n % kTakes].lookRight;
            look.lookUp += g_takes[n % kTakes].lookUp;
        }
    }
    // Mouse movement in the last few takes says the mouse is what turns the view.
    for (uint32_t n = g_numTakes > 3 ? g_numTakes - 3 : 0; n < g_numTakes; n++) {
        recent = recent || g_takes[n % kTakes].lookRight != 0 || g_takes[n % kTakes].lookUp != 0;
    }
    look.lookRight += g_keyboardMouse[0].lookRight;
    look.lookUp += g_keyboardMouse[0].lookUp;
    pthread_mutex_unlock(&g_lock);
    const float last = g_scriptLastTake.load();
    if (g_scriptLength > 0 && last >= 0) {
        ScriptLook(last, (float)frontline::os::Now() / OS_TIMER_CLOCK, &look);
    }
    *lookRight = look.lookRight;
    *lookUp = look.lookUp;
    return recent || look.lookRight != 0 || look.lookUp != 0;
}

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

// Rebooting (os/reboot.cpp): the game's callback and rumble go; the app's
// input stays.
void frontline::os::ResetPad() {
    g_samplingCallback = nullptr;
    for (u32& motor : g_motor) {
        motor = PAD_MOTOR_STOP;
    }
    __atomic_store_n(&g_playing, false, __ATOMIC_RELAXED);
}
