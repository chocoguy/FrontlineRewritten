import MetalKit

/// Placeholder renderer: clears the view every frame. Phase 1 replaces the body
/// of `draw(in:)` with the GX → Metal backend.
@MainActor
final class Renderer: NSObject, MTKViewDelegate {
    let device: MTLDevice
    private let queue: MTLCommandQueue

    init?(device: MTLDevice? = MTLCreateSystemDefaultDevice()) {
        guard let device, let queue = device.makeCommandQueue() else {
            return nil
        }
        self.device = device
        self.queue = queue
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    func draw(in view: MTKView) {
        guard let pass = view.currentRenderPassDescriptor,
              let drawable = view.currentDrawable,
              let commands = queue.makeCommandBuffer(),
              let encoder = commands.makeRenderCommandEncoder(descriptor: pass)
        else {
            return
        }
        encoder.endEncoding()
        commands.present(drawable)
        commands.commit()
    }
}
