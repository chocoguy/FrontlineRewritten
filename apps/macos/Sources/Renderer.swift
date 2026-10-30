import AppKit
import QuartzCore
import os

/// Drives the game's view at the screen's refresh rate, up to 120 Hz. On the
/// main thread, each refresh hands the game the controller's and the keyboard
/// and mouse's state; a display thread of its own shows the game's latest
/// frame.
///
/// The game keeps its own 59.94 Hz clock (ROADMAP gotcha 5): a faster screen
/// only shows each frame sooner, or, with Rewritten's high frame rate, draws
/// the frames in between.
@MainActor
final class Renderer: NSObject {
    /// The fastest the game is shown.
    static let maxRate = 120

    let input = Input()
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
        FrontlineSetPad(0, input.read())
        // Rewritten's field of view follows the Settings window as it changes.
        FrontlineSetFieldOfView(Float(Settings.fieldOfView(.standard)))
        input.rumble(FrontlineRumble(0))
    }
}

/// The display thread: a CAMetalDisplayLink on a run loop of its own, which
/// presents the game's picture for each refresh, timed for when it will be
/// on screen.
final class Presenter: NSObject, CAMetalDisplayLinkDelegate, @unchecked Sendable {
    private let link: CAMetalDisplayLink
    /// A refresh rate range set from the main thread, for the display thread to apply.
    private let pendingRange = OSAllocatedUnfairLock<CAFrameRateRange?>(initialState: nil)

    init(layer: CAMetalLayer) {
        link = CAMetalDisplayLink(metalLayer: layer)
        super.init()
        link.delegate = self
        // Draw a refresh ahead, not two: the frame is little work, and at 2 the
        // 59.94 Hz frames land unevenly on a 60 Hz screen (some shown twice,
        // some never). FRONTLINE_FRAME_LATENCY=<refreshes> tries others.
        link.preferredFrameLatency = Float(ProcessInfo.processInfo.environment["FRONTLINE_FRAME_LATENCY"] ?? "") ?? 1
        let thread = Thread { [self] in
            link.add(to: .current, forMode: .default)
            RunLoop.current.run()
        }
        thread.name = "frontline display"
        thread.qualityOfService = .userInteractive
        thread.start()
    }

    func setRange(_ range: CAFrameRateRange) {
        pendingRange.withLock { $0 = range }
    }

    func metalDisplayLink(_ link: CAMetalDisplayLink, needsUpdate update: CAMetalDisplayLink.Update) {
        if let range = pendingRange.withLock({ (pending: inout CAFrameRateRange?) -> CAFrameRateRange? in
            defer { pending = nil }
            return pending
        }) {
            link.preferredFrameRateRange = range
        }
        FrontlinePresent(update.drawable, update.targetPresentationTimestamp)
    }
}
