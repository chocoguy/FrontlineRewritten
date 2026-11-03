import AppKit
import GameController
import SwiftUI

/// The Mac's screen, for the shared settings (SettingsKeys.swift), and the
/// Settings window.
extension Settings {
    /// How fast the main screen refreshes, in Hz.
    @MainActor static var screenRate: Int {
        NSScreen.main?.maximumFramesPerSecond ?? 60
    }

    /// The display's width / height, which widescreen follows.
    @MainActor static var screenAspect: CGFloat {
        guard let frame = NSScreen.main?.frame, frame.height > 0 else {
            return 16.0 / 9.0
        }
        return frame.width / frame.height
    }
}

struct SettingsView: View {
    @AppStorage(Settings.mouseSensitivity) private var sensitivity = 1.0
    @AppStorage(Settings.invertMouse) private var invert = false
    @AppStorage(Settings.pauseInBackground) private var pauseInBackground = true
    @AppStorage(Settings.fieldOfView) private var fieldOfView = 70.0
    @AppStorage(Settings.fullAutoSpringfield) private var fullAutoSpringfield = false
    @AppStorage(Settings.gameMode) private var mode = Settings.classic
    @AppStorage(Settings.widescreen) private var widescreen = false
    @State private var showingPadBindings = false

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
            Section("Weapons") {
                Toggle("Full Auto Springfield",
                       isOn: mode == Settings.rewritten ? $fullAutoSpringfield : .constant(false))
                    .disabled(mode != Settings.rewritten)
                Text(mode == Settings.rewritten
                     ? "The Springfield fires for as long as the trigger is held."
                     : "Classic keeps the original bolt action.")
                    .foregroundStyle(.secondary)
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
            Section("Game controller") {
                LabeledContent("Rebind its buttons, swap or invert its sticks") {
                    Button("Controller Buttons…") {
                        showingPadBindings = true
                    }
                }
            }
        }
        .formStyle(.grouped)
        .frame(width: 520, height: 680)
        .sheet(isPresented: $showingPadBindings) {
            VStack(spacing: 0) {
                PadBindingsView().formStyle(.grouped)
                HStack {
                    Spacer()
                    Button("Done") {
                        showingPadBindings = false
                    }
                    .keyboardShortcut(.defaultAction)
                }
                .padding()
            }
            .frame(width: 520, height: 640)
        }
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
