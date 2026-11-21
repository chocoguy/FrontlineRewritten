import UIKit

/// iPhone and iPad: controls drawn over the game, for playing without a game
/// controller.
///
/// - The left side is the main stick: it appears where the thumb lands. In the
///   menus it's the D-pad.
/// - Dragging on the right side, off the buttons, looks (`FrontlineAddTouchLook`,
///   at once, like a mouse), at `Settings.touchLookSensitivity`.
/// - Playing, the buttons are game actions (fire, aim, reload, crouch, jump,
///   use, next weapon, melee, pause), held as the controller's bound buttons
///   are, so the prompts stay the controller's. In the menus they're A, B and
///   START.
///
/// They go into controller 1's input (`Controllers.touch`) each refresh
/// (`update`). Set to Auto, they hide while a gamepad is played and come back
/// at the next touch.
@MainActor
final class TouchControls: UIView {
    private let controllers: Controllers

    /// A button: its place from the screen's bottom right corner (or top
    /// right, `top`), in points, and what it holds.
    private struct Spec {
        let title: String
        let right: CGFloat
        let bottom: CGFloat
        let size: CGFloat
        var top = false
        var action: UInt32 = 0
        var button: UInt16 = 0
        var press: UInt32 = 0
    }

    private static let playing: [Spec] = [
        Spec(title: "FIRE", right: 110, bottom: 110, size: 96, action: UInt32(FrontlineActionFire)),
        Spec(title: "AIM", right: 215, bottom: 110, size: 60, action: UInt32(FrontlineActionAim)),
        Spec(title: "RELOAD", right: 184, bottom: 184, size: 60, action: UInt32(FrontlineActionReload)),
        Spec(title: "JUMP", right: 110, bottom: 215, size: 60, action: UInt32(FrontlineActionJump)),
        Spec(title: "CROUCH", right: 36, bottom: 184, size: 60, action: UInt32(FrontlineActionCrouch)),
        Spec(title: "USE", right: 300, bottom: 64, size: 60, action: UInt32(FrontlineActionUse)),
        Spec(title: "MELEE", right: 292, bottom: 150, size: 60, action: UInt32(FrontlineActionMelee)),
        Spec(title: "NEXT", right: 60, bottom: 290, size: 60, action: UInt32(FrontlineActionNextWeapon)),
        Spec(title: "II", right: 40, bottom: 36, size: 48, top: true, press: UInt32(FrontlineActionPause)),
    ]

    private static let menus: [Spec] = [
        Spec(title: "A", right: 110, bottom: 110, size: 88, button: UInt16(FrontlineButtonA)),
        Spec(title: "B", right: 220, bottom: 70, size: 68, button: UInt16(FrontlineButtonB)),
        Spec(title: "START", right: 40, bottom: 36, size: 56, top: true, button: UInt16(FrontlineButtonStart)),
    ]

    private final class ButtonView: UIView {
        let spec: Spec
        private let label = UILabel()
        var pressed = false {
            didSet {
                backgroundColor = UIColor(white: 1, alpha: pressed ? 0.45 : 0.18)
            }
        }

        init(_ spec: Spec) {
            self.spec = spec
            super.init(frame: .zero)
            isUserInteractionEnabled = false
            backgroundColor = UIColor(white: 1, alpha: 0.18)
            layer.borderColor = UIColor(white: 1, alpha: 0.7).cgColor
            layer.borderWidth = 2
            label.text = spec.title
            label.textColor = .white
            label.textAlignment = .center
            label.font = .systemFont(ofSize: spec.size >= 80 ? 17 : 11, weight: .bold)
            label.adjustsFontSizeToFitWidth = true
            label.minimumScaleFactor = 0.6
            addSubview(label)
        }

        @available(*, unavailable)
        required init?(coder: NSCoder) {
            fatalError("init(coder:) is not used")
        }

        override func layoutSubviews() {
            super.layoutSubviews()
            layer.cornerRadius = bounds.width / 2
            label.frame = bounds.insetBy(dx: 6, dy: 6)
        }

        func contains(_ point: CGPoint) -> Bool {
            // A little larger than drawn, for thumbs.
            let center = CGPoint(x: frame.midX, y: frame.midY)
            return hypot(point.x - center.x, point.y - center.y) <= frame.width / 2 + 8
        }
    }

    private var playingButtons: [ButtonView] = []
    private var menuButtons: [ButtonView] = []
    private var shownPlaying = true

    /// What's drawn. The view itself always takes touches, so one can bring
    /// the controls back while they're hidden.
    private let content = UIView()
    private let stickBase = UIView()
    private let stickKnob = UIView()
    private static let stickRadius: CGFloat = 64

    private var stickTouch: UITouch?
    private var stickOrigin = CGPoint.zero
    private var stick = SIMD2<Float>.zero
    private var lookTouch: UITouch?
    private var lookLast = CGPoint.zero
    private var buttonTouches: [ObjectIdentifier: ButtonView] = [:]
    private let feedback = UIImpactFeedbackGenerator(style: .light)

    init(controllers: Controllers) {
        self.controllers = controllers
        super.init(frame: .zero)
        isMultipleTouchEnabled = true
        backgroundColor = .clear
        content.isUserInteractionEnabled = false
        addSubview(content)
        for view in [stickBase, stickKnob] {
            view.isUserInteractionEnabled = false
            view.isHidden = true
            view.layer.borderColor = UIColor(white: 1, alpha: 0.7).cgColor
            view.layer.borderWidth = 2
            content.addSubview(view)
        }
        stickBase.backgroundColor = UIColor(white: 1, alpha: 0.1)
        stickKnob.backgroundColor = UIColor(white: 1, alpha: 0.35)
        playingButtons = Self.playing.map(ButtonView.init)
        menuButtons = Self.menus.map(ButtonView.init)
        for button in playingButtons + menuButtons {
            content.addSubview(button)
        }
        showSet(playing: true)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not used")
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        content.frame = bounds
        let area = bounds.inset(by: safeAreaInsets)
        for button in playingButtons + menuButtons {
            let spec = button.spec
            let x = area.maxX - spec.right
            let y = spec.top ? area.minY + spec.bottom : area.maxY - spec.bottom
            button.frame = CGRect(x: x - spec.size / 2, y: y - spec.size / 2, width: spec.size, height: spec.size)
        }
        let r = Self.stickRadius
        stickBase.frame.size = CGSize(width: r * 2, height: r * 2)
        stickBase.layer.cornerRadius = r
        stickKnob.frame.size = CGSize(width: r, height: r)
        stickKnob.layer.cornerRadius = r / 2
    }

    // MARK: Each refresh

    /// Hands `controllers` the touch input, and shows the controls the
    /// settings and the game's screen call for. Call each refresh, before
    /// `controllers.update()`.
    func update() {
        let mode = Settings.touchControls(.standard)
        let shown = mode == Settings.touchAlways || (mode == Settings.touchAuto && !controllers.gamepadUsed)
        isUserInteractionEnabled = mode != Settings.touchOff
        content.isHidden = !shown
        content.alpha = CGFloat(Settings.touchOpacity(.standard))
        let playing = FrontlineIsPlaying()
        if playing != shownPlaying {
            showSet(playing: playing)
        }
        var touch = Controllers.Touch()
        if playing {
            touch.stick = stick
        } else {
            // The menus go by the D-pad.
            if stick.x < -0.5 { touch.buttons |= UInt16(FrontlineButtonLeft) }
            if stick.x > 0.5 { touch.buttons |= UInt16(FrontlineButtonRight) }
            if stick.y > 0.5 { touch.buttons |= UInt16(FrontlineButtonUp) }
            if stick.y < -0.5 { touch.buttons |= UInt16(FrontlineButtonDown) }
        }
        for button in buttonTouches.values {
            touch.actions |= button.spec.action
            touch.buttons |= button.spec.button
        }
        controllers.touch = isUserInteractionEnabled ? touch : Controllers.Touch()
    }

    private func showSet(playing: Bool) {
        shownPlaying = playing
        for button in playingButtons {
            button.isHidden = !playing
        }
        for button in menuButtons {
            button.isHidden = playing
        }
        // A button of the set going away lets go.
        buttonTouches = buttonTouches.filter { !$0.value.isHidden }
        for button in playingButtons + menuButtons where !buttonTouches.values.contains(where: { $0 === button }) {
            button.pressed = false
        }
    }

    // MARK: Touches

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        // A touch brings the controls back after a gamepad hid them.
        controllers.gamepadUsed = false
        for touch in touches {
            let point = touch.location(in: self)
            if let button = (shownPlaying ? playingButtons : menuButtons).first(where: { $0.contains(point) }) {
                buttonTouches[ObjectIdentifier(touch)] = button
                button.pressed = true
                feedback.impactOccurred()
                if button.spec.press != 0 {
                    FrontlinePressAction(0, button.spec.press)
                }
            } else if point.x < bounds.width * 0.4 {
                if stickTouch == nil {
                    stickTouch = touch
                    stickOrigin = point
                    moveStick(to: point)
                }
            } else if lookTouch == nil && shownPlaying {
                lookTouch = touch
                lookLast = point
            }
        }
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        for touch in touches {
            let point = touch.location(in: self)
            if touch === stickTouch {
                moveStick(to: point)
            } else if touch === lookTouch {
                look(by: CGPoint(x: point.x - lookLast.x, y: point.y - lookLast.y))
                lookLast = point
            }
        }
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        release(touches)
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        release(touches)
    }

    private func release(_ touches: Set<UITouch>) {
        for touch in touches {
            if let button = buttonTouches.removeValue(forKey: ObjectIdentifier(touch)) {
                button.pressed = buttonTouches.values.contains { $0 === button }
            }
            if touch === stickTouch {
                stickTouch = nil
                stick = .zero
                stickBase.isHidden = true
                stickKnob.isHidden = true
            }
            if touch === lookTouch {
                lookTouch = nil
            }
        }
    }

    private func moveStick(to point: CGPoint) {
        let r = Self.stickRadius
        var dx = point.x - stickOrigin.x
        var dy = point.y - stickOrigin.y
        let length = hypot(dx, dy)
        if length > r {
            dx *= r / length
            dy *= r / length
        }
        // Up is positive, as on the controller.
        stick = SIMD2(Float(dx / r), Float(-dy / r))
        stickBase.center = stickOrigin
        stickKnob.center = CGPoint(x: stickOrigin.x + dx, y: stickOrigin.y + dy)
        stickBase.isHidden = false
        stickKnob.isHidden = false
    }

    /// Radians the view turns per point dragged, at sensitivity 1: about a
    /// quarter turn across an iPhone's width.
    private static let radiansPerPoint: Float = 0.0035

    private func look(by delta: CGPoint) {
        let scale = Self.radiansPerPoint * Float(Settings.touchLookSensitivity(.standard))
        let invert: Float = UserDefaults.standard.bool(forKey: Settings.touchInvertLook) ? -1 : 1
        // Dragging up looks up.
        FrontlineAddTouchLook(0, Float(delta.x) * scale, Float(-delta.y) * scale * invert)
    }
}
