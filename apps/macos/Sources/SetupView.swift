import SwiftUI

/// The setup screen before the game starts: Classic or Rewritten, and
/// Rewritten's picture (render resolution, widescreen, frame rate,
/// anti-aliasing, draw distance). The
/// choices hold for the whole run, since the game can't change them once it's
/// running.
struct SetupView: View {
    let play: () -> Void

    @AppStorage(Settings.gameMode) private var mode = Settings.classic
    @AppStorage(Settings.renderLines) private var lines = 480
    @AppStorage(Settings.widescreen) private var widescreen = false
    @AppStorage(Settings.highFrameRate) private var highFrameRate = false
    @AppStorage(Settings.msaa) private var msaa = Settings.defaultMsaa
    @AppStorage(Settings.drawDistance) private var drawDistance = Settings.defaultDrawDistance
    @AppStorage(Disc.key) private var discFolder = ""

    private var classic: Bool { mode != Settings.rewritten }

    var body: some View {
        VStack(spacing: 20) {
            Text("Frontline Rewritten")
                .font(.largeTitle.weight(.bold))

            Form {
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

                Section("Picture") {
                    Picker("Resolution", selection: classic ? .constant(480) : $lines) {
                        ForEach(Settings.resolutions, id: \.lines) { resolution in
                            Text(label(resolution.name, lines: resolution.lines)).tag(resolution.lines)
                        }
                    }
                    .disabled(classic)
                    Toggle("Widescreen", isOn: classic ? .constant(false) : $widescreen)
                        .disabled(classic)
                    Picker("Frame rate", selection: classic ? .constant(false) : $highFrameRate) {
                        Text("60 (original)").tag(false)
                        Text("Up to \(Renderer.maxRate) (experimental)").tag(true)
                    }
                    .disabled(classic)
                    Picker("Anti-aliasing", selection: classic ? .constant(1) : $msaa) {
                        Text("Off (original)").tag(1)
                        Text("4× MSAA").tag(4)
                    }
                    .disabled(classic)
                    Picker("Draw distance", selection: classic ? .constant(1.0) : $drawDistance) {
                        ForEach(Settings.drawDistances, id: \.times) { distance in
                            Text(distance.name).tag(distance.times)
                        }
                    }
                    .disabled(classic)
                    if classic {
                        Text("Classic keeps the original picture.").foregroundStyle(.secondary)
                    } else if highFrameRate && Settings.screenRate <= 60 {
                        Text("This screen refreshes at 60 Hz, so it shows 60 frames a second.")
                            .foregroundStyle(.secondary)
                    } else if highFrameRate && lines >= 2160 {
                        Text("Each frame shown is drawn again: at 4K, slower Macs may not keep up with the screen.")
                            .foregroundStyle(.secondary)
                    } else if lines >= 2160 && msaa > 1 {
                        Text("4K with 4× MSAA is the most demanding picture; slower Macs may not keep 60 fps.")
                            .foregroundStyle(.secondary)
                    } else if lines >= 2160 {
                        Text("4K renders about 20 times the original's pixels; slower Macs may not keep 60 fps.")
                            .foregroundStyle(.secondary)
                    } else if highFrameRate {
                        Text("Experimental: camera movement is drawn at the screen's rate, with some stutter and edge pop-in on fast turns.")
                            .foregroundStyle(.secondary)
                    } else if widescreen {
                        Text("Levels fill the screen's shape; menus and movies stay 4:3.")
                            .foregroundStyle(.secondary)
                    }
                }

                Section("Disc") {
                    LabeledContent("Extracted disc") {
                        HStack {
                            Text(discName).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                            Button("Change…") {
                                Disc.choose()
                            }
                        }
                    }
                }
            }
            .formStyle(.grouped)
            .scrollDisabled(true)
            .frame(width: 480, height: 520)

            Button(action: play) {
                Text("Play").frame(width: 120)
            }
            .keyboardShortcut(.defaultAction)
            .controlSize(.large)
        }
        .padding(24)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color(nsColor: .windowBackgroundColor))
    }

    private var discName: String {
        if let folder = Disc.current() {
            return (folder as NSString).abbreviatingWithTildeInPath
        }
        return discFolder.isEmpty ? "Not chosen" : "Not found"
    }

    /// "1080p · 1904 × 1008 (2.25×)": the render size at this resolution.
    private func label(_ name: String, lines: Int) -> String {
        let scale = Double(lines) / 480
        let width = Settings.renderWidth(aspect: !classic && widescreen ? Settings.screenAspect : nil)
        let size = "\(Int((Double(width) * scale).rounded())) × \(Int((448 * scale).rounded()))"
        if lines == 480 {
            return "\(name) · \(size)"
        }
        let factor = scale == scale.rounded() ? String(format: "%.0f×", scale) : String(format: "%g×", scale)
        return "\(name) · \(size) (\(factor))"
    }
}
