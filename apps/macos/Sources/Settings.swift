import AppKit
import GameController
import SwiftUI

/// The app's settings (user defaults keys) and its Settings window.
enum Settings {
    /// Mouse look speed, a multiple of `Input.radiansPerCount` (default 1).
    static let mouseSensitivity = "mouseSensitivity"
    /// Moving the mouse up looks down.
    static let invertMouse = "invertMouse"
    /// Rewritten's field of view: degrees across at 4:3 (70, the original, to
    /// 110). Applies while the game runs.
    static let fieldOfView = "fieldOfView"
    static let fieldOfViews: ClosedRange<Double> = 70...110

    static func fieldOfView(_ defaults: UserDefaults) -> Double {
        let degrees = defaults.object(forKey: fieldOfView) == nil ? 70 : defaults.double(forKey: fieldOfView)
        return min(max(degrees, fieldOfViews.lowerBound), fieldOfViews.upperBound)
    }

    /// Degrees across at 4:3 widened to the width a level renders at (Hor+).
    static func widenedFieldOfView(_ degrees: Double, width: Int) -> Double {
        let half = degrees / 2 * .pi / 180
        return 2 * atan(tan(half) * Double(width) / 640) * 180 / .pi
    }

    /// Pause the game when the app goes to the background (on if unset).
    static let pauseInBackground = "pauseInBackground"

    static var pausesInBackground: Bool {
        UserDefaults.standard.object(forKey: pauseInBackground) == nil
            || UserDefaults.standard.bool(forKey: pauseInBackground)
    }

    // The setup screen's, for the whole run.

    /// "classic" (the default) or "rewritten".
    static let gameMode = "gameMode"
    static let classic = "classic"
    static let rewritten = "rewritten"
    /// The render's height on a 480-line screen (Rewritten only).
    static let renderLines = "renderLines"
    /// Widescreen in the display's shape (Rewritten only).
    static let widescreen = "widescreen"
    /// Frames drawn between the game's, up to the screen's refresh rate
    /// (`Renderer.maxRate`; Rewritten only).
    static let highFrameRate = "highFrameRate"
    /// Samples a pixel: 1 (off) or 4 (Rewritten only; 4 if unset).
    static let msaa = "msaa"
    /// Times the level's own draw distance (Rewritten only).
    static let drawDistance = "drawDistance"

    /// How fast the main screen refreshes, in Hz.
    @MainActor static var screenRate: Int {
        NSScreen.main?.maximumFramesPerSecond ?? 60
    }

    /// The render resolutions offered: lines on the 480-line screen.
    static let resolutions: [(lines: Int, name: String)] = [
        (480, "Original"), (720, "720p"), (1080, "1080p"), (1440, "1440p"), (2160, "4K"),
    ]

    static func renderLines(_ defaults: UserDefaults) -> Int {
        let lines = defaults.integer(forKey: renderLines)
        return resolutions.contains { $0.lines == lines } ? lines : 480
    }

    static let defaultMsaa = 4

    static func msaa(_ defaults: UserDefaults) -> Int {
        defaults.object(forKey: msaa) == nil ? defaultMsaa : defaults.integer(forKey: msaa)
    }

    /// The draw distances offered: times the level's own.
    static let drawDistances: [(times: Double, name: String)] = [
        (1, "Original"), (2, "Far (2×)"), (4, "Farthest (4×)"),
    ]
    static let defaultDrawDistance = 2.0

    static func drawDistance(_ defaults: UserDefaults) -> Double {
        let times = defaults.object(forKey: drawDistance) == nil ? defaultDrawDistance
                                                                  : defaults.double(forKey: drawDistance)
        return drawDistances.contains { $0.times == times } ? times : 1
    }

    /// The display's width / height, which widescreen follows.
    static var screenAspect: CGFloat {
        guard let frame = NSScreen.main?.frame, frame.height > 0 else {
            return 16.0 / 9.0
        }
        return frame.width / frame.height
    }

    /// The width the game renders a level at for `aspect`, in the console's
    /// pixels (as NativeRenderWidth works it out).
    static func renderWidth(aspect: CGFloat?) -> Int {
        guard let aspect, aspect > 4.0 / 3.0 else {
            return 640
        }
        return min(1136, max(640, Int((480 * aspect / 16).rounded()) * 16))
    }
}

struct SettingsView: View {
    @AppStorage(Settings.mouseSensitivity) private var sensitivity = 1.0
    @AppStorage(Settings.invertMouse) private var invert = false
    @AppStorage(Settings.pauseInBackground) private var pauseInBackground = true
    @AppStorage(Settings.fieldOfView) private var fieldOfView = 70.0
    @AppStorage(Settings.gameMode) private var mode = Settings.classic
    @AppStorage(Settings.widescreen) private var widescreen = false

    var body: some View {
        Form {
            Section("Game") {
                Toggle("Pause when Frontline is in the background", isOn: $pauseInBackground)
            }
            Section("View") {
                Slider(value: mode == Settings.rewritten ? $fieldOfView : .constant(70),
                       in: Settings.fieldOfViews, step: 1) {
                    Text("Field of view")
                } minimumValueLabel: {
                    Text("70°")
                } maximumValueLabel: {
                    Text("110°")
                }
                .disabled(mode != Settings.rewritten)
                Text(fieldOfViewNote).foregroundStyle(.secondary)
            }
            Section("Mouse") {
                Slider(value: $sensitivity, in: 0.1...4) {
                    Text("Look speed")
                } minimumValueLabel: {
                    Text("Slow")
                } maximumValueLabel: {
                    Text("Fast")
                }
                Toggle("Invert vertical look", isOn: $invert)
            }
            ControlsSection()
        }
        .formStyle(.grouped)
        .frame(width: 520, height: 680)
    }

    private var fieldOfViewNote: String {
        if mode != Settings.rewritten {
            return "Classic keeps the original 70° (across, at 4:3)."
        }
        let degrees = Int(fieldOfView.rounded())
        let width = Settings.renderWidth(aspect: widescreen ? Settings.screenAspect : nil)
        if width > 640 {
            let wide = Int(Settings.widenedFieldOfView(fieldOfView, width: width).rounded())
            return "\(degrees)° across at 4:3, \(wide)° across in widescreen levels."
                + (degrees == 70 ? " The original." : "")
        }
        return "\(degrees)° across." + (degrees == 70 ? " The original." : "")
    }
}
