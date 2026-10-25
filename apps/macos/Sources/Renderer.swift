import MetalKit

/// Each display frame: hands the game the controller's state, then shows the
/// game's latest frame.
@MainActor
final class Renderer: NSObject, MTKViewDelegate {
    private let input = Input()

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    func draw(in view: MTKView) {
        FrontlineSetPad(0, input.read())
        input.rumble(FrontlineRumble(0))
        FrontlinePresent(view.currentDrawable)
    }
}
