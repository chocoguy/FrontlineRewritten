// The shared audio session on tvOS, which RemoteIO plays through: its
// category, and the interruptions, route changes and media-server resets the
// output unit (audio_out.cpp) has to follow.

#import <AVFAudio/AVFAudio.h>

#include <cstdio>

#include "audio_session.h"

namespace {

frontline::audio::SessionEvents g_events;

bool Activate(AVAudioSession* session) {
    NSError* error = nil;
    if (![session setCategory:AVAudioSessionCategoryPlayback error:&error] ||
        ![session setActive:YES error:&error]) {
        std::fprintf(stderr, "frontline: audio session: %s\n", error.localizedDescription.UTF8String);
        return false;
    }
    return true;
}

}  // namespace

bool frontline::audio::BeginSession(const SessionEvents& events) {
    AVAudioSession* session = [AVAudioSession sharedInstance];
    if (!Activate(session)) {
        return false;
    }
    g_events = events;
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    [center addObserverForName:AVAudioSessionInterruptionNotification
                        object:session
                         queue:nil
                    usingBlock:^(NSNotification* note) {
                        auto type = (AVAudioSessionInterruptionType)
                            [note.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];
                        if (type == AVAudioSessionInterruptionTypeBegan) {
                            g_events.stop();
                        } else if (Activate(session)) {
                            // A game resumes its sound whatever the interruption's
                            // ShouldResume option says.
                            g_events.restart();
                        }
                    }];
    [center addObserverForName:AVAudioSessionRouteChangeNotification
                        object:session
                         queue:nil
                    usingBlock:^(NSNotification* note) {
                        auto reason = (AVAudioSessionRouteChangeReason)
                            [note.userInfo[AVAudioSessionRouteChangeReasonKey] unsignedIntegerValue];
                        switch (reason) {
                            case AVAudioSessionRouteChangeReasonNewDeviceAvailable:
                            case AVAudioSessionRouteChangeReasonOldDeviceUnavailable:
                            case AVAudioSessionRouteChangeReasonOverride:
                            case AVAudioSessionRouteChangeReasonRouteConfigurationChange:
                                g_events.restart();
                                break;
                            default:  // category changes and the like: nothing to redo
                                break;
                        }
                    }];
    [center addObserverForName:AVAudioSessionMediaServicesWereResetNotification
                        object:session
                         queue:nil
                    usingBlock:^(NSNotification*) {
                        if (Activate(session)) {
                            g_events.reopen();
                        }
                    }];
    return true;
}
