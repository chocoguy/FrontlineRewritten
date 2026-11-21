import GameController
import SwiftUI

/// The setup screen before the game starts: Classic or Rewritten, Rewritten's
/// picture and game options, the controls, and (tvOS) the picture size. The
/// choices hold for the whole run, since the game can't change them once it's
/// running.
///
/// On the TV it's two lists side by side for the focus engine; on iPhone and
/// iPad, one list. As the Mac's, except: Rewritten's options are hidden in
/// Classic rather than greyed out; the field of view is a list (tvOS has no
/// slider); the high frame rate is only offered on a screen faster than 60 Hz
/// (ProMotion); and the disc comes with the app.
struct SetupView: View {
    /// Starts the game; the reason, if it can't.
    let play: () -> String?

    @AppStorage(Settings.gameMode) private var mode = Settings.classic
    @AppStorage(Settings.renderLines) private var lines = 480
    @AppStorage(Settings.widescreen) private var widescreen = true
    @AppStorage(Settings.msaa) private var msaa = Settings.defaultMsaa
    @AppStorage(Settings.drawDistance) private var drawDistance = Settings.defaultDrawDistance
    @AppStorage(Settings.modernRenderer) private var modern = false
    @AppStorage(Settings.correctedLighting) private var corrected = false
    @AppStorage(Settings.blobShadows) private var shadows = false
    @AppStorage(Settings.fastSmoke) private var fastSmoke = Settings.defaultFastSmoke
    @AppStorage(Settings.bicubicTextures) private var bicubic = Settings.defaultBicubicTextures
    @AppStorage(Settings.ambientOcclusion) private var occlusion = false
    @AppStorage(Settings.flashLights) private var flashLights = false
    @AppStorage(Settings.unfairAI) private var unfairAI = false
    @AppStorage(Settings.fieldOfView) private var fieldOfView = 70.0
    @AppStorage(Settings.fullAutoSpringfield) private var fullAutoSpringfield = false
    @AppStorage(Settings.pictureSize) private var pictureSize = 1.0
    @AppStorage(Settings.highFrameRate) private var highFrameRate = false
    @AppStorage(Settings.touchControls) private var touchControls = Settings.touchAuto
    @AppStorage(Settings.touchOpacity) private var touchOpacity = 0.6
    @AppStorage(Settings.touchLookSensitivity) private var touchLook = 1.0
    @AppStorage(Settings.touchInvertLook) private var touchInvert = false
    @AppStorage(Settings.pauseInBackground) private var pauseInBackground = true

    @State private var problem: String?
    @State private var gamepad = Self.hasGamepad

    private var classic: Bool { mode != Settings.rewritten }

    /// The field of views offered, degrees across at 4:3.
    private static let fieldOfViews = Array(stride(from: 70.0, through: 110, by: 5))

    var body: some View {
        NavigationStack {
            #if os(tvOS)
            VStack(spacing: 0) {
                header
                columns
            }
            #else
            list
            #endif
        }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidConnect)) { _ in
            gamepad = Self.hasGamepad
        }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidDisconnect)) { _ in
            gamepad = Self.hasGamepad
        }
    }

    /// The title, fixed above the lists on a solid bar so they don't scroll
    /// under it (a navigation title let rows show through, 2026-11-14).
    private var header: some View {
        Text("Frontline Rewritten")
            .font(.title2.bold())
            .frame(maxWidth: .infinity)
            .padding(.top, 20)
            .padding(.bottom, 24)
            .background(Color(white: 0.08).ignoresSafeArea(edges: [.top, .horizontal]))
            .zIndex(1)
    }

    #if os(iOS)
    /// One list on iPhone and iPad.
    private var list: some View {
        Form {
            playSection
            modeSection
            if !classic {
                pictureSection
                gameSection
                ModsSection(rewritten: true)
            }
            controlsSection
            touchSection
            Section("App") {
                Toggle("Pause when the app is in the background", isOn: $pauseInBackground)
            }
        }
        .navigationTitle("Frontline Rewritten")
    }
    #endif

    private var columns: some View {
        // Two lists side by side, to use the TV's width: starting the game
        // and the picture on the left, the rest on the right. The focus
        // engine moves between them, and each scrolls on its own.
        HStack(alignment: .top, spacing: 40) {
            Form {
                playSection
                modeSection
                if !classic {
                    pictureSection
                }
            }
            .frame(maxWidth: .infinity)
            Form {
                if !classic {
                    gameSection
                    ModsSection(rewritten: true)
                }
                controlsSection
                screenSection
            }
            .frame(maxWidth: .infinity)
        }
    }

    private var playSection: some View {
        Section {
            Button("Play") {
                problem = play()
            }
            if let problem {
                Text(problem).foregroundStyle(.red)
            }
            #if os(tvOS)
            if !gamepad {
                Text("No game controller is connected. The Siri Remote works the menus; playing needs a controller.")
                    .foregroundStyle(.secondary)
            }
            #endif
        }
    }

    private var modeSection: some View {
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
    }

    private var pictureSection: some View {
        Section("Picture") {
            ChoiceRow("Resolution", selection: $lines, options: Settings.resolutions.map { ($0.lines, $0.name) })
            Toggle("Widescreen", isOn: $widescreen)
            if Settings.screenRate > 60 && !modern {
                ChoiceRow("Frame rate", selection: $highFrameRate,
                          options: [(false, "60 (original)"), (true, "Up to \(min(Settings.screenRate, 120)) (experimental)")])
            }
            ChoiceRow("Anti-aliasing", selection: $msaa, options: [(1, "Off (original)"), (4, "4× MSAA")])
            ChoiceRow("Texture filtering", selection: $bicubic,
                      options: [(false, "Bilinear (original)"), (true, "Bicubic")])
            ChoiceRow("Draw distance", selection: $drawDistance,
                      options: Settings.drawDistances.map { ($0.times, $0.name) })
            ChoiceRow("Renderer", selection: $modern, options: [(false, "GX (original)"), (true, "Modern")])
            if modern {
                ChoiceRow("Lighting", selection: $corrected, options: [(false, "Original"), (true, "Corrected")])
            }
            if !(highFrameRate && !modern && Settings.screenRate > 60) {
                // Not drawn with the high frame rate.
                ChoiceRow("Ambient occlusion", selection: $occlusion,
                          options: [(false, "Off (original)"), (true, "On")])
            }
            ChoiceRow("Flash lighting", selection: $flashLights, options: [(false, "Off (original)"), (true, "On")])
            ChoiceRow("Shadows", selection: $shadows, options: [(false, "Off (original)"), (true, "Blob")])
            ChoiceRow("Smoke", selection: $fastSmoke, options: [(true, "Fast"), (false, "Full resolution")])
        }
    }

    private var gameSection: some View {
        Section("Game") {
            ChoiceRow("Field of view", selection: $fieldOfView, options: Self.fieldOfViews.map { degrees in
                (degrees, degrees == 70 ? "70° (original)" : "\(Int(degrees))°")
            })
            Toggle("Full Auto Springfield", isOn: $fullAutoSpringfield)
            Toggle(isOn: $unfairAI) {
                Text("Unfair AI")
                Text("On Hard: enemies aim perfectly, know where you are, attack relentlessly, punch rapidly up close and have three times the health.")
            }
        }
    }

    private var controlsSection: some View {
        Section("Controls") {
            NavigationLink("Controller buttons") {
                PadBindingsView().navigationTitle("Controller Buttons")
            }
        }
    }

    #if os(iOS)
    private var touchSection: some View {
        Section {
            ChoiceRow("Touch controls", selection: $touchControls, options: [
                (Settings.touchAuto, "Auto"), (Settings.touchAlways, "Always"), (Settings.touchOff, "Off"),
            ])
            ChoiceRow("Opacity", selection: $touchOpacity, options: Settings.touchOpacities.map { ($0.value, $0.name) })
            VStack(alignment: .leading) {
                Text("Look speed")
                Slider(value: $touchLook, in: 0.25...4)
            }
            Toggle("Invert look", isOn: $touchInvert)
        } header: {
            Text("Touch")
        } footer: {
            Text("Auto hides the touch controls while a game controller is played; touching the screen brings them back. Drag on the right side to look.")
        }
    }
    #endif

    private var screenSection: some View {
        Section("Screen") {
            ChoiceRow("Picture size", selection: $pictureSize,
                      options: Settings.pictureSizes.map { ($0.share, $0.name) })
            Text("Shrinks the picture for TVs that cut off its edges.").foregroundStyle(.secondary)
        }
    }

    private static var hasGamepad: Bool {
        GCController.controllers().contains { $0.extendedGamepad != nil }
    }
}
