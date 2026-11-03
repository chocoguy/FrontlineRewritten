import GameController
import SwiftUI

/// The setup screen before the game starts, for the TV's focus engine: Classic
/// or Rewritten, Rewritten's picture and game options, and the picture size.
/// The choices hold for the whole run, since the game can't change them once
/// it's running.
///
/// As the Mac's, except: Rewritten's options are hidden in Classic rather than
/// greyed out; the field of view is a list (tvOS has no slider); there's no
/// high frame rate (a TV runs at 60 Hz); and the disc comes with the app.
struct SetupView: View {
    /// Starts the game; the reason, if it can't.
    let play: () async -> String?

    @AppStorage(Settings.gameMode) private var mode = Settings.classic
    @AppStorage(Settings.renderLines) private var lines = 480
    @AppStorage(Settings.widescreen) private var widescreen = true
    @AppStorage(Settings.msaa) private var msaa = Settings.defaultMsaa
    @AppStorage(Settings.drawDistance) private var drawDistance = Settings.defaultDrawDistance
    @AppStorage(Settings.modernRenderer) private var modern = false
    @AppStorage(Settings.correctedLighting) private var corrected = false
    @AppStorage(Settings.blobShadows) private var shadows = false
    @AppStorage(Settings.unfairAI) private var unfairAI = false
    @AppStorage(Settings.fieldOfView) private var fieldOfView = 70.0
    @AppStorage(Settings.fullAutoSpringfield) private var fullAutoSpringfield = false
    @AppStorage(Settings.pictureSize) private var pictureSize = 1.0

    @State private var problem: String?
    @State private var starting = false
    @State private var gamepad = Self.hasGamepad

    private var classic: Bool { mode != Settings.rewritten }

    /// The field of views offered, degrees across at 4:3.
    private static let fieldOfViews = Array(stride(from: 70.0, through: 110, by: 5))

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Button(starting ? "Getting your saves from iCloud…" : "Play") {
                        starting = true
                        Task {
                            problem = await play()
                            starting = false
                        }
                    }
                    .disabled(starting)
                    if let problem {
                        Text(problem).foregroundStyle(.red)
                    }
                    if !gamepad {
                        Text("No game controller is connected. The Siri Remote works the menus; playing needs a controller.")
                            .foregroundStyle(.secondary)
                    }
                }

                Section {
                    Picker("Mode", selection: $mode) {
                        Text("Classic").tag(Settings.classic)
                        Text("Rewritten").tag(Settings.rewritten)
                    }
                    .pickerStyle(.segmented)
                    Text(classic
                        ? "The original game, as close to the GameCube version as possible: 640 × 448 at 4:3."
                        : "The game with deliberate changes, like higher resolutions, widescreen and smoother textures.")
                        .foregroundStyle(.secondary)
                }

                if !classic {
                    Section("Picture") {
                        Picker("Resolution", selection: $lines) {
                            ForEach(Settings.resolutions, id: \.lines) { resolution in
                                Text(resolution.name).tag(resolution.lines)
                            }
                        }
                        Toggle("Widescreen", isOn: $widescreen)
                        Picker("Anti-aliasing", selection: $msaa) {
                            Text("Off (original)").tag(1)
                            Text("4× MSAA").tag(4)
                        }
                        Picker("Draw distance", selection: $drawDistance) {
                            ForEach(Settings.drawDistances, id: \.times) { distance in
                                Text(distance.name).tag(distance.times)
                            }
                        }
                        Picker("Renderer", selection: $modern) {
                            Text("GX (original)").tag(false)
                            Text("Modern (in progress)").tag(true)
                        }
                        if modern {
                            Picker("Lighting", selection: $corrected) {
                                Text("Original").tag(false)
                                Text("Corrected").tag(true)
                            }
                        }
                        Picker("Shadows", selection: $shadows) {
                            Text("Off (original)").tag(false)
                            Text("Blob").tag(true)
                        }
                    }

                    Section("Game") {
                        Picker("Field of view", selection: $fieldOfView) {
                            ForEach(Self.fieldOfViews, id: \.self) { degrees in
                                Text(degrees == 70 ? "70° (original)" : "\(Int(degrees))°").tag(degrees)
                            }
                        }
                        Toggle("Full Auto Springfield", isOn: $fullAutoSpringfield)
                        Toggle("Unfair AI", isOn: $unfairAI)
                        Text("Unfair AI, on Hard: enemies aim perfectly, know where you are, attack relentlessly and have three times the health.")
                            .foregroundStyle(.secondary)
                    }
                }

                Section("Controls") {
                    NavigationLink("Controller buttons") {
                        PadBindingsView().navigationTitle("Controller Buttons")
                    }
                }

                Section("Screen") {
                    Picker("Picture size", selection: $pictureSize) {
                        ForEach(Settings.pictureSizes, id: \.share) { size in
                            Text(size.name).tag(size.share)
                        }
                    }
                    Text("Shrinks the picture for TVs that cut off its edges.").foregroundStyle(.secondary)
                }
            }
            // Pickers open their choices over the screen. tvOS's default, a
            // pushed list, came up blank here (2026-11-02).
            .pickerStyle(.menu)
            .navigationTitle("Frontline Rewritten")
        }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidConnect)) { _ in
            gamepad = Self.hasGamepad
        }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidDisconnect)) { _ in
            gamepad = Self.hasGamepad
        }
    }

    private static var hasGamepad: Bool {
        GCController.controllers().contains { $0.extendedGamepad != nil }
    }
}
