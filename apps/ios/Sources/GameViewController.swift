#if os(tvOS)
import AVKit
import CoreMedia
#endif
import GameController
import QuartzCore
import UIKit

#if os(tvOS)
/// tvOS: a GCEventViewController with UIKit's controller handling off, so
/// Menu and the remote's Back reach the game instead of leaving the app.
typealias GameViewControllerBase = GCEventViewController
#else
typealias GameViewControllerBase = UIViewController
#endif

/// The game: its picture, and the controllers, each on a port of its own (on
/// tvOS the Siri Remote works controller 1's menus; on iPhone and iPad the
/// touch controls add to controller 1).
///
/// Each refresh on the main thread hands the game the controllers' state; a
/// display thread of its own (`Presenter`) shows the game's latest frame.
///
/// Exit Game: B on the game's main menu asks to leave the game. While the
/// dialog is up, UIKit gets the controllers and the game gets none of their
/// input; leaving stops the game and calls `exited`.
final class GameViewController: GameViewControllerBase {
    /// Called once the game has stopped for Exit Game.
    var exited: (() -> Void)?

    private let controllers = Controllers()
    private var inputLink: CADisplayLink?
    private var presenter: Presenter?
    private var askingToExit = false
    #if os(iOS)
    private lazy var touchControls = TouchControls(controllers: controllers)
    #endif

    override func loadView() {
        view = GameSurfaceView()
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        setControllerInteraction(false)
        controllers.separatePorts = true
        #if os(tvOS)
        controllers.remoteMenus = true
        #else
        touchControls.frame = view.bounds
        touchControls.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        view.addSubview(touchControls)
        #endif
        if let surface = view as? GameSurfaceView {
            presenter = Presenter(layer: surface.metalLayer)
        }
        let link = CADisplayLink(target: self, selector: #selector(step))
        link.add(to: .main, forMode: .common)
        inputLink = link
        setRate(Self.frameRange(screenRate: Settings.screenRate))
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        UIApplication.shared.isIdleTimerDisabled = true
        #if os(tvOS)
        matchFrameRate()
        #else
        setRate(Self.frameRange(screenRate: view.window?.screen.maximumFramesPerSecond ?? 60))
        #endif
    }

    /// The refresh range: 60 Hz (the game keeps its own 59.94 Hz clock), or
    /// up to the screen's rate (ProMotion's 120) with the high frame rate.
    private static func frameRange(screenRate: Int) -> CAFrameRateRange {
        let defaults = UserDefaults.standard
        let high = defaults.string(forKey: Settings.gameMode) == Settings.rewritten &&
            !defaults.bool(forKey: Settings.modernRenderer) && defaults.bool(forKey: Settings.highFrameRate)
        let rate = Float(high ? min(max(screenRate, 60), 120) : 60)
        return CAFrameRateRange(minimum: 60, maximum: rate, preferred: rate)
    }

    private func setRate(_ range: CAFrameRateRange) {
        inputLink?.preferredFrameRateRange = range
        presenter?.setRange(range)
    }

    private func setControllerInteraction(_ on: Bool) {
        #if os(tvOS)
        controllerUserInteractionEnabled = on
        #endif
    }

    #if os(tvOS)
    /// Asks a TV with Match Frame Rate on to run at the game's 59.94 Hz, in
    /// SDR, so its frames don't judder against a 60 Hz screen.
    private func matchFrameRate() {
        guard let window = view.window else {
            return
        }
        var format: CMVideoFormatDescription?
        CMVideoFormatDescriptionCreate(allocator: nil, codecType: kCMVideoCodecType_H264, width: 1920, height: 1080,
                                       extensions: nil, formatDescriptionOut: &format)
        if let format {
            window.avDisplayManager.preferredDisplayCriteria = AVDisplayCriteria(refreshRate: 59.94,
                                                                                 formatDescription: format)
        }
    }
    #else
    override var prefersStatusBarHidden: Bool { true }
    override var prefersHomeIndicatorAutoHidden: Bool { true }
    // The game's controls reach the screen's edges: a swipe from one first
    // goes to the game, and a second swipe to the system.
    override var preferredScreenEdgesDeferringSystemGestures: UIRectEdge { .all }
    override var supportedInterfaceOrientations: UIInterfaceOrientationMask { .landscape }
    #endif

    @objc private func step(_ link: CADisplayLink) {
        if !askingToExit && FrontlineTakeExitGameRequest() {
            askToExit()
        }
        #if os(iOS)
        touchControls.update()
        #endif
        controllers.update()
        Settings.applyLive(.standard)
    }

    private func askToExit() {
        askingToExit = true
        controllers.holdInput = true
        setControllerInteraction(true)
        let alert = UIAlertController(title: "Exit Game?",
                                      message: "You'll go back to the setup screen. Progress you haven't saved will be lost.",
                                      preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "Exit Game", style: .destructive) { [weak self] _ in
            self?.exitGame()
        })
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel) { [weak self] _ in
            self?.resume()
        })
        present(alert, animated: true)
    }

    private func resume() {
        askingToExit = false
        setControllerInteraction(false)
        // The press that answered the dialog stays the dialog's.
        controllers.holdInput = false
    }

    private func exitGame() {
        guard Game.stop() else {
            resume()  // the game didn't stop: carry on
            return
        }
        inputLink?.invalidate()
        inputLink = nil
        presenter?.stop()
        presenter = nil
        controllers.stopRumble()
        UIApplication.shared.isIdleTimerDisabled = false
        exited?()
    }
}

/// The game's picture: a Metal layer the display thread presents to, sized to
/// the view in the screen's pixels (3840 × 2160 on a 4K TV), edge to edge.
final class GameSurfaceView: UIView {
    override class var layerClass: AnyClass { CAMetalLayer.self }

    var metalLayer: CAMetalLayer {
        // The layer is a CAMetalLayer (layerClass).
        layer as! CAMetalLayer
    }

    override init(frame: CGRect) {
        super.init(frame: frame)
        metalLayer.device = FrontlineDevice()
        metalLayer.pixelFormat = .bgra8Unorm
        backgroundColor = .black
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not used")
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        let scale = window?.windowScene?.screen.nativeScale ?? traitCollection.displayScale
        metalLayer.contentsScale = scale
        let size = CGSize(width: (bounds.width * scale).rounded(), height: (bounds.height * scale).rounded())
        if size.width > 0 && size.height > 0 {
            metalLayer.drawableSize = size
        }
    }
}

#if os(iOS)
/// The phone's or tablet's screen, for the shared settings (SettingsKeys.swift).
extension Settings {
    @MainActor private static var screen: UIScreen? {
        UIApplication.shared.connectedScenes.compactMap { ($0 as? UIWindowScene)?.screen }.first
    }

    /// How fast the screen refreshes, in Hz (120 with ProMotion).
    @MainActor static var screenRate: Int {
        screen?.maximumFramesPerSecond ?? 60
    }

    /// The screen's width / height in landscape, which widescreen follows.
    @MainActor static var screenAspect: CGFloat {
        guard let size = screen?.bounds.size, min(size.width, size.height) > 0 else {
            return 16.0 / 9.0
        }
        return max(size.width, size.height) / min(size.width, size.height)
    }
}
#endif
