// The user messages (chars.bum and its index binum.bum), as
// CUserMessageTable::Load splits them, and their keyboard rewrites
// (NativeKeyboardPrompt; the game swaps them in in game/native/prompts.cpp).

#include "messages.h"

#include <cstring>
#include <string>

#include "check.h"
#include "frontline/platform.h"
#include "native/hooks.h"

namespace {

int Conversions(const std::string& s) {
    int n = 0;
    for (size_t at = s.find('%'); at != std::string::npos; at = s.find('%', at + 1)) {
        n++;
    }
    return n;
}

bool Has(const std::string& s, const char* word) { return s.find(word) != std::string::npos; }

// The labels of the actions the app lets players rebind, as the app names
// them (Input.swift's names): every name longer than 7 characters, which
// have short forms (prompts.cpp), and the longest without one.
const int kRebindable[] = {NATIVE_LABEL_USE, NATIVE_LABEL_CROUCH, NATIVE_LABEL_AIM, NATIVE_LABEL_PAUSE,
                           NATIVE_LABEL_HINT};
const char* const kLongestLabels[] = {
    "Caps Lock",         "Print Screen",       "Scroll Lock",         "Page Down",      "Fwd Delete",
    "Num Lock",          "Keypad Enter",       "Keypad 5",            "Right Control",  "Right Shift",
    "Right Option",      "Left Mouse Button",  "Right Mouse Button",  "Middle Mouse Button",
    "Mouse Button 4",    "Mouse Button 5",     "Control",
};
// pad.cpp's defaults, which the app's default bindings match.
const char* const kDefaultLabels[NATIVE_NUM_LABELS] = {
    "F", "C", "Right Mouse Button", "Esc", "H", "Mouse Wheel", "Return", "Esc", "X", "Y",
};

}  // namespace

bool CheckMessages(const std::vector<uint8_t>& text, const std::vector<uint8_t>& index, MessageStats& stats) {
    CHECK(index.size() >= 2, "index of %zu bytes", index.size());
    int count = index[0] | index[1] << 8;
    CHECK(index.size() >= 2 + 2 * (size_t)count, "%d messages in an index of %zu bytes", count, index.size());
    size_t at = 0;
    for (int i = 0; i < count; i++) {
        int gap = index[2 + 2 * i], length = index[3 + 2 * i];
        CHECK(at + length < text.size(), "message %#x past the end", i);
        std::string message((const char*)text.data() + at, length);
        at += length + gap + 1;
        stats.messages++;

        char out[256];
        // The controller screen's labels (ConfigGC.cpp), which the game copies.
        bool config = (i >= 0x2E5 && i <= 0x2F3) || i == 0x2F7 || i == 0x2F8 || i == 0x2FA;
        if (!NativeKeyboardPrompt(message.c_str(), out, sizeof(out))) {
            // Anything still naming a button would show on the keyboard.
            CHECK(config || (!Has(message, "Press ACTION") && !Has(message, "Press START") &&
                             !Has(message, "Press CROUCH") && !Has(message, "Use AIM") &&
                             !Has(message, "Control Pad") && !Has(message, "A Button") &&
                             !Has(message, "B Button") && !Has(message, "press the A") &&
                             !Has(message, "press the B")),
                  "message %#x not rewritten: %s", i, message.c_str());
            continue;
        }
        CHECK(!config, "controller screen label %#x rewritten: %s", i, message.c_str());
        std::string keyboard(out);
        stats.rewritten++;
        stats.pressAction += Has(message, "Press ACTION");
        // Prompts at the top of the screen are cut at 49 characters, and
        // objectives longer than 49 aren't drawn (pumhandler.cpp, Pause.cpp).
        CHECK(keyboard.size() <= 49, "message %#x is %zu characters: %s", i, keyboard.size(), keyboard.c_str());
        // Memory card messages are format strings.
        CHECK(Conversions(keyboard) == Conversions(message), "message %#x changed its conversions: %s", i,
              keyboard.c_str());
        CHECK(!Has(keyboard, "ACTION") && !Has(keyboard, "A Button") && !Has(keyboard, "B Button") &&
                  !Has(keyboard, "Control Pad") && !Has(keyboard, "{"),
              "message %#x still names a button: %s", i, keyboard.c_str());

        // The playing actions can be rebound to keys or buttons with longer
        // names; with the longest of each kind, the prompt still fits.
        for (const char* longest : kLongestLabels) {
            for (int label : kRebindable) {
                frontline::SetKeyLabel(label, longest);
            }
            char rebound[256];
            NativeKeyboardPrompt(message.c_str(), rebound, sizeof(rebound));
            CHECK(std::strlen(rebound) <= 49, "message %#x with \"%s\" is %zu characters: %s", i, longest,
                  std::strlen(rebound), rebound);
        }
        for (int label : kRebindable) {
            frontline::SetKeyLabel(label, kDefaultLabels[label]);
        }
    }
    return true;
}
