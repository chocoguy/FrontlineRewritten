import AppKit
import IOKit.pwr_mgt

/// Keeps the display awake while the game runs in the active app. A game
/// controller's input doesn't count as activity for the idle timer, and long
/// stretches play without any (D-Day's landing), so the screen would dim.
/// Let go while the app is in the background.
@MainActor
enum DisplayAwake {
    private static var assertion: IOPMAssertionID = 0
    private static var started = false

    /// From now on, while the app is active.
    static func start() {
        guard !started else {
            return
        }
        started = true
        let notes: [(Notification.Name, Bool)] = [
            (NSApplication.didBecomeActiveNotification, true), (NSApplication.didResignActiveNotification, false),
        ]
        for (name, on) in notes {
            NotificationCenter.default.addObserver(forName: name, object: nil, queue: .main) { _ in
                MainActor.assumeIsolated {
                    hold(on)
                }
            }
        }
        hold(NSApp.isActive)
    }

    private static func hold(_ on: Bool) {
        if on && assertion == 0 {
            let reason = "Playing Frontline Rewritten" as CFString
            if IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep as CFString,
                                           IOPMAssertionLevel(kIOPMAssertionLevelOn), reason,
                                           &assertion) != kIOReturnSuccess {
                assertion = 0
            }
        } else if !on && assertion != 0 {
            IOPMAssertionRelease(assertion)
            assertion = 0
        }
    }
}
