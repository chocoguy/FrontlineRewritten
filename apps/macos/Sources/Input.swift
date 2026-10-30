import AppKit
import CoreHaptics
import GameController

/// Controller 1, as the keyboard, the mouse and any connected game controller
/// drive it.
///
/// The keyboard and mouse send game actions, not GameCube buttons, so they
/// don't depend on the controller layout picked in the game's options (see
/// `Bindings`, which players change in the Settings window). The mouse looks around while the
/// view has captured the pointer.
///
/// Controllers map by position: the left and right sticks are the main and C
/// sticks, the face buttons are A B X Y, the triggers are L and R (analog), a
/// shoulder button is Z, and Menu is START.
@MainActor
final class Input {
    /// Tells the game what the keys its prompts and menu icons name are
    /// called, from the bindings (the first input bound to each action).
    static func publishKeyLabels() {
        let labels: [(Int, Int)] = [
            (FrontlineLabelUse, FrontlineActionUse), (FrontlineLabelCrouch, FrontlineActionCrouch),
            (FrontlineLabelAim, FrontlineActionAim), (FrontlineLabelPause, FrontlineActionPause),
            (FrontlineLabelHint, FrontlineActionHint), (FrontlineLabelMenuA, FrontlineActionMenuA),
            (FrontlineLabelMenuB, FrontlineActionMenuB), (FrontlineLabelMenuX, FrontlineActionMenuX),
            (FrontlineLabelMenuY, FrontlineActionMenuY),
        ]
        for (label, action) in labels {
            // An unbound action still needs a name in its prompts.
            FrontlineSetKeyLabel(Int32(label), Bindings.label(of: UInt32(action)) ?? "Unbound")
        }
        FrontlineSetKeyLabel(Int32(FrontlineLabelScopeZoom), "Mouse Wheel")
    }

    /// Radians of look per raw mouse count at sensitivity 1.
    static let radiansPerCount: Float = 0.0012

    private var hapticEngine: CHHapticEngine?
    private var hapticPlayer: CHHapticAdvancedPatternPlayer?
    private var hapticController: GCController?
    private var rumbling = false

    // The mouse since the last send: raw movement from GCMouse, and the
    // pointer's movement from the view's events, for pointing devices GCMouse
    // doesn't report (used only when GCMouse reported nothing). Raw movement
    // goes to the game as it comes (rawMoved), so the display's frames between
    // the game's turn by the latest of it; here it only checks which way is up.
    private var rawLook = SIMD2<Float>.zero
    private var pointerLook = SIMD2<Float>.zero
    // Radians a count, right and up (the sensitivity and inverted look),
    // read from the settings each send.
    private var lookScale = SIMD2<Float>(repeating: Input.radiansPerCount)
    private var heldButtons = Set<Int>()
    private var ignoredButtons = Set<Int>()  // held when the capture began
    private var scroll: CGFloat = 0
    private var wheel: Int32 = 0
    private var captured = false

    // Which way GCMouse's vertical movement points (up positive, unlike
    // NSEvent's), checked against the pointer's while both report.
    private var rawUpSign: Float = 1
    private var signVotes: Float = 0

    init() {
        updateLookScale()
        watchMice()
        NotificationCenter.default.addObserver(forName: .GCMouseDidConnect, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.watchMice()
            }
        }
    }

    private func watchMice() {
        for mouse in GCMouse.mice() {
            mouse.handlerQueue = .main
            mouse.mouseInput?.mouseMovedHandler = { [weak self] _, deltaX, deltaY in
                MainActor.assumeIsolated {
                    self?.rawMoved(SIMD2(deltaX, deltaY))
                }
            }
        }
    }

    private func rawMoved(_ delta: SIMD2<Float>) {
        rawLook += delta
        if captured {
            let look = SIMD2(delta.x, delta.y * rawUpSign) * lookScale
            FrontlineAddMouseLook(0, look.x, look.y)
        }
    }

    // Events from the view.

    func pointerMoved(_ event: NSEvent) {
        pointerLook += SIMD2(Float(event.deltaX), Float(-event.deltaY))
    }

    func mouseButton(_ number: Int, down: Bool) {
        if down {
            heldButtons.insert(number)
        } else {
            heldButtons.remove(number)
            ignoredButtons.remove(number)
        }
    }

    func scrolled(_ event: NSEvent) {
        // A notch per line, or per 20 points of a trackpad's or smooth wheel's scrolling.
        scroll += event.hasPreciseScrollingDeltas ? event.scrollingDeltaY / 20 : event.scrollingDeltaY
        let notches = scroll.rounded(.towardZero)
        wheel += Int32(notches)
        scroll -= notches
    }

    /// The pointer was captured (the mouse looks around) or let go.
    func setCaptured(_ on: Bool) {
        guard on != captured else {
            return
        }
        captured = on
        ignoredButtons = on ? heldButtons : []
        rawLook = .zero
        pointerLook = .zero
        scroll = 0
        wheel = 0
    }

    /// Hands the game the keyboard and mouse. `focused`: the game's window
    /// has the keyboard.
    func sendKeyboardMouse(focused: Bool) {
        // Rebinding in the Settings window applies at once; the prompts and
        // icons the game makes from then on name the new keys.
        if Bindings.refresh() {
            Self.publishKeyLabels()
        }
        var actions: UInt32 = 0
        if focused, let keyboard = GCKeyboard.coalesced?.keyboardInput {
            for (key, bits) in Bindings.keys where keyboard.button(forKeyCode: key)?.isPressed ?? false {
                actions |= bits
            }
        }

        var look = SIMD2<Float>.zero
        if captured {
            for button in heldButtons.subtracting(ignoredButtons) {
                actions |= Bindings.mouseButtons[button] ?? 0
            }
            look = pointerOnlyLook()
        }
        FrontlineSetKeyboardMouse(0, actions, look.x, look.y, captured ? wheel : 0)
        rawLook = .zero
        pointerLook = .zero
        wheel = 0
        updateLookScale()
    }

    /// The pointer's movement in radians (right and up positive), for a
    /// device GCMouse didn't report this frame (its movement went already).
    /// Also checks GCMouse's vertical sign against the pointer's.
    private func pointerOnlyLook() -> SIMD2<Float> {
        if rawLook.y != 0 && abs(pointerLook.y) >= 2 {
            signVotes = max(-20, min(20, signVotes + (rawLook.y * pointerLook.y > 0 ? 1 : -1)))
            if abs(signVotes) >= 10 {
                rawUpSign = signVotes > 0 ? 1 : -1
            }
        }
        return rawLook == .zero ? pointerLook * lookScale : .zero
    }

    private func updateLookScale() {
        let defaults = UserDefaults.standard
        let sensitivity = defaults.object(forKey: Settings.mouseSensitivity) != nil
            ? Float(defaults.double(forKey: Settings.mouseSensitivity)) : 1
        let k = Self.radiansPerCount * sensitivity
        lookScale = SIMD2(k, defaults.bool(forKey: Settings.invertMouse) ? -k : k)
    }

    func read() -> FrontlinePad {
        var buttons: UInt16 = 0
        var stick = SIMD2<Float>.zero
        var substick = SIMD2<Float>.zero
        var triggerLeft: Float = 0
        var triggerRight: Float = 0

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
        // GameCube controller.
        if triggerLeft >= 0.95 { buttons |= UInt16(FrontlineButtonL) }
        if triggerRight >= 0.95 { buttons |= UInt16(FrontlineButtonR) }

        // Connected even without a controller: the keyboard and mouse drive it.
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
