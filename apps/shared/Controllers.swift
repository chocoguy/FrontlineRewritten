import CoreHaptics
import GameController

/// The connected game controllers as GameCube controllers, and their rumble.
///
/// Each gamepad's buttons do what its kind's bindings say (`PadBindings`). By
/// default they map by position: the left and right sticks are the main and C
/// sticks, the face buttons are A B X Y, the triggers are L and R (analog), a
/// shoulder button is Z, and Menu is START.
///
/// With `separatePorts` (tvOS), each gamepad is a controller of its own: ports
/// 0–3 in the order they connected, with the controller's player light to
/// match, so the original split screen can be played on the couch. Otherwise
/// (the Mac) they all drive controller 1, with the keyboard and mouse.
///
/// With `remoteMenus` (tvOS), the Siri Remote works controller 1's menus while
/// no gamepad is connected: its touch surface or arrows are the D-pad, select
/// is A, Back is B and Play/Pause is START.
///
/// A button can report a press that's over before the next read (a gamepad's
/// Menu, the remote's Back), so presses are also caught as they happen and
/// held until it.
@MainActor
final class Controllers {
    var separatePorts = false
    var remoteMenus = false

    /// One controller's input at a read.
    private struct State {
        var buttons: UInt16 = 0
        var actions: UInt32 = 0
        var stick = SIMD2<Float>.zero
        var substick = SIMD2<Float>.zero
        var triggerLeft: Float = 0
        var triggerRight: Float = 0

        mutating func merge(_ other: State) {
            buttons |= other.buttons
            actions |= other.actions
            stick = Controllers.stronger(stick, other.stick)
            substick = Controllers.stronger(substick, other.substick)
            triggerLeft = max(triggerLeft, other.triggerLeft)
            triggerRight = max(triggerRight, other.triggerRight)
        }
    }

    /// The gamepad on each port (`separatePorts`).
    private var ports: [GCController?] = Array(repeating: nil, count: 4)
    private let rumbles = (0..<4).map { _ in Rumble() }
    /// Presses since the last read: each gamepad's, and the remote's.
    private var latched: [ObjectIdentifier: Set<PadBindings.Input>] = [:]
    private var latchedRemote: UInt16 = 0

    init() {
        watchPresses()
        NotificationCenter.default.addObserver(forName: .GCControllerDidConnect, object: nil,
                                               queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.watchPresses()
            }
        }
    }

    /// Hands the game each controller's input, and runs the rumble it asks
    /// for. Call once a refresh.
    func update() {
        let gamepads = GCController.controllers().filter { $0.extendedGamepad != nil }
        if separatePorts {
            assignPorts(gamepads)
            for port in 0..<4 {
                var state = State()
                if let controller = ports[port], let pad = controller.extendedGamepad {
                    state = read(controller, pad)
                }
                if port == 0 && remoteMenus && gamepads.isEmpty {
                    state.buttons |= readRemote()
                }
                // Controller 1 is always connected: the remote (or the
                // keyboard) drives it.
                send(port, state, connected: port == 0 || ports[port] != nil)
                rumbles[port].run(FrontlineRumble(Int32(port)), on: ports[port])
            }
        } else {
            var state = State()
            for controller in gamepads {
                if let pad = controller.extendedGamepad {
                    state.merge(read(controller, pad))
                }
            }
            if remoteMenus && gamepads.isEmpty {
                state.buttons |= readRemote()
            }
            send(0, state, connected: true)
            rumbles[0].run(FrontlineRumble(0), on: GCController.current)
        }
        latched = [:]
        latchedRemote = 0
    }

    /// Keeps each gamepad on the port it got, and gives a new one the first
    /// free port.
    private func assignPorts(_ gamepads: [GCController]) {
        for port in 0..<4 {
            if let controller = ports[port], !gamepads.contains(where: { $0 === controller }) {
                ports[port] = nil
            }
        }
        for controller in gamepads where !ports.contains(where: { $0 === controller }) {
            guard let port = ports.firstIndex(where: { $0 == nil }) else {
                break
            }
            ports[port] = controller
            controller.playerIndex = GCControllerPlayerIndex(rawValue: port) ?? .indexUnset
        }
    }

    private func read(_ controller: GCController, _ pad: GCExtendedGamepad) -> State {
        let profile = PadBindings.profile(for: PadBindings.kind(of: controller))
        let caught = latched[ObjectIdentifier(controller)] ?? []
        var state = State()
        for input in PadBindings.Input.allCases {
            guard let element = input.element(pad) else {
                continue
            }
            let pressed = element.isPressed || caught.contains(input)
            let value = caught.contains(input) ? 1 : element.value
            switch profile.target(input) {
            case .nothing:
                break
            case .button(FrontlineButtonL):
                state.triggerLeft = max(state.triggerLeft, value)
            case .button(FrontlineButtonR):
                state.triggerRight = max(state.triggerRight, value)
            case .button(let bit):
                if pressed {
                    state.buttons |= UInt16(bit)
                }
            case .action(let bits):
                if pressed {
                    state.actions |= bits
                }
            }
        }
        var main = SIMD2(pad.leftThumbstick.xAxis.value, pad.leftThumbstick.yAxis.value)
        var c = SIMD2(pad.rightThumbstick.xAxis.value, pad.rightThumbstick.yAxis.value)
        if profile.swapSticks {
            swap(&main, &c)
        }
        if profile.invertMain {
            main.y = -main.y
        }
        if profile.invertC {
            c.y = -c.y
        }
        state.stick = main
        state.substick = c
        return state
    }

    private func readRemote() -> UInt16 {
        var buttons = latchedRemote
        for controller in GCController.controllers() {
            guard controller.extendedGamepad == nil, let remote = controller.microGamepad else {
                continue
            }
            let bits: [(GCControllerButtonInput, Int)] = [
                (remote.buttonA, FrontlineButtonA), (remote.buttonMenu, FrontlineButtonB),
                (remote.buttonX, FrontlineButtonStart),
                (remote.dpad.up, FrontlineButtonUp), (remote.dpad.down, FrontlineButtonDown),
                (remote.dpad.left, FrontlineButtonLeft), (remote.dpad.right, FrontlineButtonRight),
            ]
            for (button, bit) in bits where button.isPressed {
                buttons |= UInt16(bit)
            }
        }
        return buttons
    }

    private func send(_ port: Int, _ state: State, connected: Bool) {
        var buttons = state.buttons
        // A trigger pressed all the way clicks its digital button, as on the
        // GameCube controller.
        if state.triggerLeft >= 0.95 { buttons |= UInt16(FrontlineButtonL) }
        if state.triggerRight >= 0.95 { buttons |= UInt16(FrontlineButtonR) }
        FrontlineSetPad(Int32(port), FrontlinePad(
            connected: connected, buttons: buttons,
            stickX: Self.axis(state.stick.x), stickY: Self.axis(state.stick.y),
            substickX: Self.axis(state.substick.x), substickY: Self.axis(state.substick.y),
            triggerLeft: Self.trigger(state.triggerLeft), triggerRight: Self.trigger(state.triggerRight)))
        FrontlineSetPadActions(Int32(port), state.actions)
    }

    private func watchPresses() {
        for controller in GCController.controllers() {
            controller.handlerQueue = .main
            let id = ObjectIdentifier(controller)
            if let pad = controller.extendedGamepad {
                for input in PadBindings.Input.allCases {
                    input.element(pad)?.pressedChangedHandler = { [weak self] _, _, pressed in
                        MainActor.assumeIsolated {
                            if pressed {
                                self?.latched[id, default: []].insert(input)
                            }
                        }
                    }
                }
            } else if let remote = controller.microGamepad {
                for (button, bit) in [(remote.buttonMenu, FrontlineButtonB), (remote.buttonX, FrontlineButtonStart)] {
                    button.pressedChangedHandler = { [weak self] _, _, pressed in
                        MainActor.assumeIsolated {
                            if pressed {
                                self?.latchedRemote |= UInt16(bit)
                            }
                        }
                    }
                }
            }
        }
    }

    nonisolated fileprivate static func stronger(_ a: SIMD2<Float>, _ b: SIMD2<Float>) -> SIMD2<Float> {
        (b * b).sum() > (a * a).sum() ? b : a
    }

    /// -1...1 to the controller's raw range, which reaches past the clamp
    /// (PADClamp) like a real stick.
    private static func axis(_ v: Float) -> Int8 {
        Int8(max(-100, min(100, (v * 100).rounded())))
    }

    private static func trigger(_ v: Float) -> UInt8 {
        UInt8(max(0, min(255, (v * 255).rounded())))
    }
}

/// One port's rumble: the controller's haptics run while the game runs the
/// rumble motor.
@MainActor
private final class Rumble {
    private var engine: CHHapticEngine?
    private var player: CHHapticAdvancedPatternPlayer?
    private var controller: GCController?
    private var rumbling = false

    func run(_ on: Bool, on controller: GCController?) {
        if controller !== self.controller {
            player = nil
            engine?.stop()
            engine = nil
            self.controller = controller
            rumbling = false
        }
        guard on != rumbling else {
            return
        }
        rumbling = on
        if on {
            if player == nil, let engine = controller?.haptics?.createEngine(withLocality: .default) {
                let event = CHHapticEvent(
                    eventType: .hapticContinuous,
                    parameters: [CHHapticEventParameter(parameterID: .hapticIntensity, value: 1)],
                    relativeTime: 0, duration: 30)
                if let pattern = try? CHHapticPattern(events: [event], parameters: []) {
                    try? engine.start()
                    self.engine = engine
                    player = try? engine.makeAdvancedPlayer(with: pattern)
                    player?.loopEnabled = true
                }
            }
            try? player?.start(atTime: CHHapticTimeImmediate)
        } else {
            try? player?.stop(atTime: CHHapticTimeImmediate)
        }
    }
}
