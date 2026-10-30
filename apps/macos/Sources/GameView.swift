import AppKit
import QuartzCore
import SwiftUI

/// Hosts the Metal view the game shows in.
struct GameView: NSViewRepresentable {
    func makeCoordinator() -> Renderer {
        Renderer()
    }

    func makeNSView(context: Context) -> GameSurfaceView {
        let view = GameSurfaceView()
        view.input = context.coordinator.input
        context.coordinator.attach(view)
        return view
    }

    func updateNSView(_ view: GameSurfaceView, context: Context) {}
}

/// The game's picture: a Metal layer the display thread presents to
/// (`Renderer`), sized to the view in pixels.
///
/// It takes the keyboard focus, so key presses reach the game (read through
/// GCKeyboard) instead of beeping, and passes the mouse's buttons, wheel and
/// movement to `input`.
///
/// While the game is being played in the key window, the view captures the
/// pointer: it hides it and holds it still, so the mouse only looks around.
/// It lets go when the game pauses or a menu comes up, and when the window
/// or the app loses focus. Capturing starts with the pointer over the view,
/// so it doesn't grab a pointer that is busy elsewhere (on the title bar,
/// say).
final class GameSurfaceView: NSView {
    var input: Input?
    /// Called when the window moves to another screen (its refresh rate may differ).
    var onScreenChange: (() -> Void)?
    private(set) var captured = false
    let metalLayer = CAMetalLayer()

    override init(frame: NSRect) {
        super.init(frame: frame)
        metalLayer.device = FrontlineDevice()
        metalLayer.pixelFormat = .bgra8Unorm
        metalLayer.backgroundColor = CGColor(gray: 0, alpha: 1)
        wantsLayer = true
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not used")
    }

    override func makeBackingLayer() -> CALayer {
        metalLayer
    }

    override var acceptsFirstResponder: Bool { true }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        window?.makeFirstResponder(self)
        window?.acceptsMouseMovedEvents = true
        updateDrawableSize()
        // Let go at once on losing focus: a hidden window gets no more frames.
        let notes: [(Notification.Name, Any?)] = [
            (NSWindow.didResignKeyNotification, window), (NSApplication.didResignActiveNotification, nil),
        ]
        for (name, object) in notes {
            NotificationCenter.default.addObserver(forName: name, object: object, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated {
                    self?.updateCapture(playing: false)
                }
            }
        }
        // Pause the game when the app goes to the background or the window to
        // the Dock: it keeps running otherwise, with its keys let go. Not when
        // only the key window changes, so the Settings window can be used
        // while playing.
        let pauses: [(Notification.Name, Any?)] = [
            (NSApplication.didResignActiveNotification, nil), (NSWindow.didMiniaturizeNotification, window),
        ]
        for (name, object) in pauses {
            NotificationCenter.default.addObserver(forName: name, object: object, queue: .main) { _ in
                MainActor.assumeIsolated {
                    if Settings.pausesInBackground && FrontlineIsPlaying() {
                        FrontlinePressAction(0, UInt32(FrontlineActionPause))
                    }
                }
            }
        }
        NotificationCenter.default.addObserver(forName: NSWindow.didChangeScreenNotification, object: window,
                                               queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.updateDrawableSize()
                self?.onScreenChange?()
            }
        }
        onScreenChange?()
    }

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        updateDrawableSize()
    }

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        updateDrawableSize()
    }

    /// The layer's drawables in the screen's pixels, as MTKView sized them.
    private func updateDrawableSize() {
        let scale = window?.backingScaleFactor ?? NSScreen.main?.backingScaleFactor ?? 1
        metalLayer.contentsScale = scale
        let size = CGSize(width: (bounds.width * scale).rounded(), height: (bounds.height * scale).rounded())
        if size.width > 0 && size.height > 0 {
            metalLayer.drawableSize = size
        }
    }

    var focused: Bool {
        NSApp.isActive && (window?.isKeyWindow ?? false)
    }

    /// Each display frame: captures or lets go of the pointer.
    func updateCapture(playing: Bool) {
        let want = playing && focused && (captured || pointerInside)
        guard want != captured else {
            return
        }
        captured = want
        if want {
            if let window {
                let center = window.convertPoint(toScreen: convert(NSPoint(x: bounds.midX, y: bounds.midY), to: nil))
                // Quartz's global coordinates start at the main display's top left.
                let height = NSScreen.screens.first?.frame.height ?? 0
                CGWarpMouseCursorPosition(CGPoint(x: center.x, y: height - center.y))
            }
            CGAssociateMouseAndMouseCursorPosition(0)
            NSCursor.hide()
        } else {
            CGAssociateMouseAndMouseCursorPosition(1)
            NSCursor.unhide()
        }
        input?.setCaptured(want)
    }

    private var pointerInside: Bool {
        guard let window else {
            return false
        }
        return bounds.contains(convert(window.mouseLocationOutsideOfEventStream, from: nil))
    }

    override func keyDown(with event: NSEvent) {}
    override func keyUp(with event: NSEvent) {}
    override func flagsChanged(with event: NSEvent) {}

    override func mouseDown(with event: NSEvent) { input?.mouseButton(0, down: true) }
    override func mouseUp(with event: NSEvent) { input?.mouseButton(0, down: false) }
    override func rightMouseDown(with event: NSEvent) { input?.mouseButton(1, down: true) }
    override func rightMouseUp(with event: NSEvent) { input?.mouseButton(1, down: false) }
    override func otherMouseDown(with event: NSEvent) { input?.mouseButton(event.buttonNumber, down: true) }
    override func otherMouseUp(with event: NSEvent) { input?.mouseButton(event.buttonNumber, down: false) }

    override func mouseMoved(with event: NSEvent) { input?.pointerMoved(event) }
    override func mouseDragged(with event: NSEvent) { input?.pointerMoved(event) }
    override func rightMouseDragged(with event: NSEvent) { input?.pointerMoved(event) }
    override func otherMouseDragged(with event: NSEvent) { input?.pointerMoved(event) }

    override func scrollWheel(with event: NSEvent) { input?.scrolled(event) }
}
