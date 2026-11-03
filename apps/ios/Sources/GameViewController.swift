import AVKit
import CoreMedia
import GameController
import QuartzCore
import UIKit

/// The game on the TV: its picture, and the controllers, each on a port of its
/// own (the Siri Remote works controller 1's menus).
///
/// A GCEventViewController with UIKit's controller handling off, so Menu and
/// the remote's Back reach the game instead of leaving the app. Each refresh
/// on the main thread hands the game the controllers' state; a display thread
/// of its own (`Presenter`) shows the game's latest frame.
final class GameViewController: GCEventViewController {
    private let controllers = Controllers()
    private var inputLink: CADisplayLink?
    private var presenter: Presenter?

    override func loadView() {
        view = GameSurfaceView()
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        controllerUserInteractionEnabled = false
        controllers.separatePorts = true
        controllers.remoteMenus = true
        // The game keeps its own 59.94 Hz clock; the TV shows it at 60.
        let range = CAFrameRateRange(minimum: 60, maximum: 60, preferred: 60)
        if let surface = view as? GameSurfaceView {
            presenter = Presenter(layer: surface.metalLayer)
            presenter?.setRange(range)
        }
        let link = CADisplayLink(target: self, selector: #selector(step))
        link.preferredFrameRateRange = range
        link.add(to: .main, forMode: .common)
        inputLink = link
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        UIApplication.shared.isIdleTimerDisabled = true
        matchFrameRate()
    }

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

    @objc private func step(_ link: CADisplayLink) {
        controllers.update()
        Settings.applyLive(.standard)
    }
}

/// The game's picture: a Metal layer the display thread presents to, sized to
/// the view in the screen's pixels (3840 × 2160 on a 4K TV).
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
