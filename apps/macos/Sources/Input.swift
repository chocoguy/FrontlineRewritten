import CoreHaptics
import GameController

/// Controller 1, as the keyboard and any connected game controller drive it.
///
/// Keyboard: WASD the main stick, arrow keys the C stick, Space A, Left Shift
/// B, F X, R Y, Tab Z, Q L, E R, Return START, and I J K L the D-pad.
///
/// Controllers map by position: the left and right sticks are the main and C
/// sticks, the face buttons are A B X Y, the triggers are L and R (analog), a
/// shoulder button is Z, and Menu is START.
@MainActor
final class Input {
    private static let keys: [(GCKeyCode, UInt16)] = [
        (.spacebar, UInt16(FrontlineButtonA)),
        (.leftShift, UInt16(FrontlineButtonB)),
        (.keyF, UInt16(FrontlineButtonX)),
        (.keyR, UInt16(FrontlineButtonY)),
        (.tab, UInt16(FrontlineButtonZ)),
        (.keyQ, UInt16(FrontlineButtonL)),
        (.keyE, UInt16(FrontlineButtonR)),
        (.returnOrEnter, UInt16(FrontlineButtonStart)),
        (.keyI, UInt16(FrontlineButtonUp)),
        (.keyK, UInt16(FrontlineButtonDown)),
        (.keyJ, UInt16(FrontlineButtonLeft)),
        (.keyL, UInt16(FrontlineButtonRight)),
    ]

    private var hapticEngine: CHHapticEngine?
    private var hapticPlayer: CHHapticAdvancedPatternPlayer?
    private var hapticController: GCController?
    private var rumbling = false

    func read() -> FrontlinePad {
        var buttons: UInt16 = 0
        var stick = SIMD2<Float>.zero
        var substick = SIMD2<Float>.zero
        var triggerLeft: Float = 0
        var triggerRight: Float = 0

        if let keyboard = GCKeyboard.coalesced?.keyboardInput {
            func held(_ key: GCKeyCode) -> Bool { keyboard.button(forKeyCode: key)?.isPressed ?? false }
            func axis(_ minus: GCKeyCode, _ plus: GCKeyCode) -> Float { (held(plus) ? 1 : 0) - (held(minus) ? 1 : 0) }
            for (key, bit) in Self.keys where held(key) {
                buttons |= bit
            }
            stick = SIMD2(axis(.keyA, .keyD), axis(.keyS, .keyW))
            substick = SIMD2(axis(.leftArrow, .rightArrow), axis(.downArrow, .upArrow))
        }

        for controller in GCController.controllers() {
            guard let pad = controller.extendedGamepad else {
                continue
            }
            let bits: [(GCControllerButtonInput?, Int)] = [
                (pad.buttonA, FrontlineButtonA), (pad.buttonB, FrontlineButtonB),
                (pad.buttonX, FrontlineButtonX), (pad.buttonY, FrontlineButtonY),
                (pad.leftShoulder, FrontlineButtonZ), (pad.rightShoulder, FrontlineButtonZ),
                (pad.buttonMenu, FrontlineButtonStart),
                (pad.dpad.up, FrontlineButtonUp), (pad.dpad.down, FrontlineButtonDown),
                (pad.dpad.left, FrontlineButtonLeft), (pad.dpad.right, FrontlineButtonRight),
            ]
            for (button, bit) in bits where button?.isPressed ?? false {
                buttons |= UInt16(bit)
            }
            stick = Self.stronger(stick, SIMD2(pad.leftThumbstick.xAxis.value, pad.leftThumbstick.yAxis.value))
            substick = Self.stronger(substick, SIMD2(pad.rightThumbstick.xAxis.value, pad.rightThumbstick.yAxis.value))
            triggerLeft = max(triggerLeft, pad.leftTrigger.value)
            triggerRight = max(triggerRight, pad.rightTrigger.value)
        }

        // A trigger pressed all the way clicks its digital button, as on the
        // GameCube controller; a digital L or R is the trigger all the way down.
        if buttons & UInt16(FrontlineButtonL) != 0 { triggerLeft = 1 }
        if buttons & UInt16(FrontlineButtonR) != 0 { triggerRight = 1 }
        if triggerLeft >= 0.95 { buttons |= UInt16(FrontlineButtonL) }
        if triggerRight >= 0.95 { buttons |= UInt16(FrontlineButtonR) }

        return FrontlinePad(
            connected: true, buttons: buttons,
            stickX: Self.axis(stick.x), stickY: Self.axis(stick.y),
            substickX: Self.axis(substick.x), substickY: Self.axis(substick.y),
            triggerLeft: Self.trigger(triggerLeft), triggerRight: Self.trigger(triggerRight))
    }

    /// Runs the current controller's haptics while the game runs the rumble motor.
    func rumble(_ on: Bool) {
        let controller = GCController.current
        if controller !== hapticController {
            hapticPlayer = nil
            hapticEngine?.stop()
            hapticEngine = nil
            hapticController = controller
            rumbling = false
        }
        guard on != rumbling else {
            return
        }
        rumbling = on
        if on {
            if hapticPlayer == nil, let engine = controller?.haptics?.createEngine(withLocality: .default) {
                let event = CHHapticEvent(
                    eventType: .hapticContinuous,
                    parameters: [CHHapticEventParameter(parameterID: .hapticIntensity, value: 1)],
                    relativeTime: 0, duration: 30)
                if let pattern = try? CHHapticPattern(events: [event], parameters: []) {
                    try? engine.start()
                    hapticEngine = engine
                    hapticPlayer = try? engine.makeAdvancedPlayer(with: pattern)
                    hapticPlayer?.loopEnabled = true
                }
            }
            try? hapticPlayer?.start(atTime: CHHapticTimeImmediate)
        } else {
            try? hapticPlayer?.stop(atTime: CHHapticTimeImmediate)
        }
    }

    private static func stronger(_ a: SIMD2<Float>, _ b: SIMD2<Float>) -> SIMD2<Float> {
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
