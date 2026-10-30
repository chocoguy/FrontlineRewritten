// The game's messages that name controller buttons, rewritten for the
// keyboard and mouse (NativeKeyboardPrompt in native/hooks.h). The game swaps
// them in and out of its message table as the device in use changes
// (game/native/prompts.cpp).
//
// Placeholders in the rewrites are the keys' names (NativeKeyLabel): {use}
// {crouch} {aim} {pause} {hint} {zoom}, and {a} {b} for the menus' A and B.
// Keys can be rebound to buttons with long names ("Middle Mouse Button"), so
// a rewrite past the 49 characters a prompt shows is made again with short
// names ("MMB").

#include <cstdio>
#include <cstring>

#include "native/hooks.h"

namespace {

struct Rewrite {
    const char* from;
    const char* to;
};

// In-level prompts ("Press ACTION to Open Door"), wherever the phrase occurs.
const Rewrite kPhrases[] = {
    {"Press ACTION ", "Press {use} "},
    {"Press CROUCH ", "Press {crouch} "},
    {"Press START ", "Press {pause} "},
    {"Use AIM ", "Use {aim} "},
    {"Press +Control Pad Right ", "Press {hint} "},
    {"Use +Control Pad ", "Use {zoom} "},
};

// The memory card dialogs, whole lines: their sentences break across lines
// ("press the A" / "Button to continue"). These are format strings (%c), so
// a rewrite keeps every conversion and adds none.
const Rewrite kLines[] = {
    {"the A Button to continue", "{a} to continue"},
    {"and 2 files of free space, press the B", "and 2 files of free space, press {b}"},
    {"Button to manage the Memory Card, or press", "to manage the Memory Card, or press"},
    {"the A Button to continue without the", "{a} to continue without the"},
    {"Card in Slot %c, press the A Button to continue", "Card in Slot %c, press {a} to continue"},
    {"without formatting or press the B Button", "without formatting or press {b}"},
    {"Slot %c or press the A Button to continue", "Slot %c or press {a} to continue"},
    {"in Slot %c, press the A Button to continue", "in Slot %c, press {a} to continue"},
    {"to this Memory Card, or press the B Button to", "to this Memory Card, or press {b} to"},
    {"or press the A Button to continue without", "or press {a} to continue without"},
    {"for this market in Slot %c, press the A", "for this market in Slot %c, press {a}"},
    {"Button to continue without formatting or press", "to continue without formatting or press"},
    {"the B Button to format the Memory Card in Slot %c.", "{b} to format the Memory Card in Slot %c."},
    {"Press the A Button to continue without the", "Press {a} to continue without the"},
    {"Press the A Button to continue, or press the", "Press {a} to continue, or press"},
    {"B Button to delete this file.", "{b} to delete this file."},
    {"Press the A Button to continue.", "Press {a} to continue."},
    {"or press the A Button to continue without saving.", "or press {a} to continue without saving."},
};

const struct {
    const char* name;
    int label;
} kPlaceholders[] = {
    {"use", NATIVE_LABEL_USE},     {"crouch", NATIVE_LABEL_CROUCH}, {"aim", NATIVE_LABEL_AIM},
    {"pause", NATIVE_LABEL_PAUSE}, {"hint", NATIVE_LABEL_HINT},     {"zoom", NATIVE_LABEL_SCOPE_ZOOM},
    {"a", NATIVE_LABEL_MENU_A},    {"b", NATIVE_LABEL_MENU_B},
};

// Appends `text` to out[*at..], keeping room for the terminator.
void Append(char* out, int size, int* at, const char* text, int length) {
    for (int i = 0; i < length && *at < size - 1; i++) {
        out[(*at)++] = text[i];
    }
}

// Prompts at the top of the screen are cut at 49 characters, and objectives
// longer than that aren't drawn (pumhandler.cpp, Pause.cpp).
constexpr int kMaxPrompt = 49;

// Short names for the app's names longer than 7 characters (Input.swift),
// for prompts that would be too long: the longest prompt with an action in
// it ("Press F to Wear Uniform and Conceal Weapons") has room for 7. Keypad
// keys ("Keypad 5") become "KP 5".
const Rewrite kShortNames[] = {
    {"Left Mouse Button", "LMB"},   {"Right Mouse Button", "RMB"}, {"Middle Mouse Button", "MMB"},
    {"Mouse Button 4", "Mouse 4"}, {"Mouse Button 5", "Mouse 5"},  {"Mouse Wheel", "Wheel"},
    {"Right Control", "RCtrl"},     {"Right Option", "ROpt"},       {"Right Shift", "RShift"},
    {"Keypad Enter", "KPEnter"},    {"Scroll Lock", "ScrLk"},       {"Print Screen", "PrtSc"},
    {"Caps Lock", "Caps"},          {"Page Down", "PgDn"},          {"Fwd Delete", "FwdDel"},
    {"Num Lock", "NumLk"},
};

// The key name for placeholder `name` (`length` characters), or null.
const char* Label(const char* name, size_t length, bool brief) {
    for (auto& placeholder : kPlaceholders) {
        if (std::strlen(placeholder.name) == length && std::strncmp(name, placeholder.name, length) == 0) {
            const char* label = NativeKeyLabel(placeholder.label);
            if (brief) {
                for (auto& names : kShortNames) {
                    if (std::strcmp(label, names.from) == 0) {
                        return names.to;
                    }
                }
                if (std::strncmp(label, "Keypad ", 7) == 0) {
                    static thread_local char keypad[16];
                    std::snprintf(keypad, sizeof(keypad), "KP %s", label + 7);
                    return keypad;
                }
            }
            return label;
        }
    }
    return nullptr;
}

// Appends a rewrite with its placeholders filled in (`brief`: short names).
void Expand(char* out, int size, int* at, const char* pattern, bool brief) {
    for (const char* p = pattern; *p != 0;) {
        if (*p == '{') {
            const char* end = std::strchr(p, '}');
            const char* label = end != nullptr ? Label(p + 1, end - p - 1, brief) : nullptr;
            if (label != nullptr) {
                Append(out, size, at, label, (int)std::strlen(label));
                p = end + 1;
                continue;
            }
        }
        Append(out, size, at, p++, 1);
    }
}

}  // namespace

namespace {

int Rewritten(const char* text, char* out, int size, bool brief) {
    int at = 0;
    for (auto& line : kLines) {
        if (std::strcmp(text, line.from) == 0) {
            Expand(out, size, &at, line.to, brief);
            out[at] = 0;
            return 1;
        }
    }
    bool changed = false;
    for (const char* p = text; *p != 0;) {
        const Rewrite* match = nullptr;
        for (auto& phrase : kPhrases) {
            if (std::strncmp(p, phrase.from, std::strlen(phrase.from)) == 0) {
                match = &phrase;
                break;
            }
        }
        if (match != nullptr) {
            Expand(out, size, &at, match->to, brief);
            p += std::strlen(match->from);
            changed = true;
        } else {
            Append(out, size, &at, p++, 1);
        }
    }
    out[at] = 0;
    return changed ? 1 : 0;
}

}  // namespace

extern "C" int NativeKeyboardPrompt(const char* text, char* out, int size) {
    if (size <= 0) {
        return 0;
    }
    if (!Rewritten(text, out, size, false)) {
        return 0;
    }
    if ((int)std::strlen(out) > kMaxPrompt) {
        Rewritten(text, out, size, true);
    }
    return 1;
}
