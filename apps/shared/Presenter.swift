import QuartzCore
import os

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
