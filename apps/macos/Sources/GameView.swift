import MetalKit
import SwiftUI

/// Hosts the Metal view the game shows in.
struct GameView: NSViewRepresentable {
    func makeCoordinator() -> Renderer {
        Renderer()
    }

    func makeNSView(context: Context) -> MTKView {
        let view = KeyboardView()
        view.device = FrontlineDevice()
        view.colorPixelFormat = .bgra8Unorm
        view.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        // The game keeps its own 60 Hz clock (ROADMAP gotcha 5); the view only
        // shows its latest frame, at the display's rate.
        view.preferredFramesPerSecond = 60
        view.delegate = context.coordinator
        return view
    }

    func updateNSView(_ view: MTKView, context: Context) {}
}

/// Takes the keyboard focus, so key presses reach the game (read through
/// GCKeyboard) instead of beeping.
final class KeyboardView: MTKView {
    override var acceptsFirstResponder: Bool { true }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        window?.makeFirstResponder(self)
    }

    override func keyDown(with event: NSEvent) {}
    override func keyUp(with event: NSEvent) {}
    override func flagsChanged(with event: NSEvent) {}
}
