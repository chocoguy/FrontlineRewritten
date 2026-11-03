// The shared audio session on tvOS (audio_session.mm). macOS has none: there
// the output unit (audio_out.cpp) plays through the default device directly.
#pragma once

namespace frontline::audio {

// What the output unit does when the session changes. Each runs on the thread
// that delivers the session's notifications.
struct SessionEvents {
    void (*stop)();     // an interruption began (a call, Siri, another app's audio)
    void (*restart)();  // it ended, or the output route changed (HDMI, AirPods)
    void (*reopen)();   // the media services restarted: the unit must be made anew
};

// Sets the session up for a game's own sound (the playback category),
// activates it, and from then on reports its changes through `events`.
// Returns false, with the reason printed, if it can't be activated.
bool BeginSession(const SessionEvents& events);

}  // namespace frontline::audio
