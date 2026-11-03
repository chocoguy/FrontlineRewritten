import AppKit
import QuartzCore

/// Drives the game's view at the screen's refresh rate, up to 120 Hz. On the
/// main thread, each refresh hands the game the controller's and the keyboard
/// and mouse's state; a display thread of its own (`Presenter`) shows the
/// game's latest frame.
///
/// The game keeps its own 59.94 Hz clock (ROADMAP gotcha 5): a faster screen
/// only shows each frame sooner, or, with Rewritten's high frame rate, draws
/// the frames in between.
@MainActor
final class Renderer: NSObject {
    /// The fastest the game is shown.
    static let maxRate = 120

    let input = Input()
    private let controllers = Controllers()
    private weak var view: GameSurfaceView?
    private var inputLink: CADisplayLink?
    private var presenter: Presenter?

    func attach(_ view: GameSurfaceView) {
        self.view = view
        let link = view.displayLink(target: self, selector: #selector(step))
        link.add(to: .main, forMode: .common)
        inputLink = link
        presenter = Presenter(layer: view.metalLayer)
        view.onScreenChange = { [weak self] in
            self?.updateRate()
        }
        updateRate()
    }

    /// As fast as the window's screen refreshes, from 60 to `maxRate`.
    private func updateRate() {
        let screen = view?.window?.screen?.maximumFramesPerSecond ?? 60
        let rate = Float(min(max(screen, 60), Self.maxRate))
        let range = CAFrameRateRange(minimum: 60, maximum: rate, preferred: rate)
        inputLink?.preferredFrameRateRange = range
        presenter?.setRange(range)
    }

    @objc private func step(_ link: CADisplayLink) {
        if let view {
            view.updateCapture(playing: FrontlineIsPlaying())
            input.sendKeyboardMouse(focused: view.focused)
        }
        controllers.update()
        // Rewritten's field of view follows the Settings window as it changes.
        Settings.applyLive(.standard)
    }
}
