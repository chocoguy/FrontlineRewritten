import MetalKit
import SwiftUI

/// Hosts the Metal view the game draws into.
struct GameView: NSViewRepresentable {
    func makeCoordinator() -> Renderer? {
        Renderer()
    }

    func makeNSView(context: Context) -> MTKView {
        let view = MTKView()
        guard let renderer = context.coordinator else {
            return view
        }
        view.device = renderer.device
        view.colorPixelFormat = .bgra8Unorm
        view.clearColor = MTLClearColor(red: 0.10, green: 0.12, blue: 0.10, alpha: 1)
        // The game's simulation is paced by its own 60 Hz clock (ROADMAP gotcha 5);
        // the view just presents at the display's rate.
        view.preferredFramesPerSecond = 60
        view.delegate = renderer
        return view
    }

    func updateNSView(_ view: MTKView, context: Context) {}
}
