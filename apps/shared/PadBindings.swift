import Foundation
import GameController

/// What a game controller's buttons do, per kind of controller ("Xbox One",
/// "DualSense"), as players set it: each button as a GameCube button, as a
/// game action (the keyboard's actions, such as melee, which the GameCube
/// controller has no button for), or as nothing. The sticks stay the main
/// and C sticks, with a swap and inverted vertical look.
///
/// Saved in the `padBindings` default as JSON, kind -> `Profile`; a kind
/// missing there plays with the defaults, which are the original mapping. On
/// tvOS it's mirrored to iCloud's key-value storage, so it outlives the app's
/// local data and follows the player to another Apple TV.
@MainActor
enum PadBindings {
    /// A gamepad's buttons, in the controller screen's order.
    enum Input: String, CaseIterable, Identifiable, Sendable {
        case buttonA, buttonB, buttonX, buttonY
        case leftShoulder, rightShoulder, leftTrigger, rightTrigger
        case leftStick, rightStick, menu, options
        case dpadUp, dpadDown, dpadLeft, dpadRight

        var id: String { rawValue }

        /// The button on `pad`, if it has one.
        func element(_ pad: GCExtendedGamepad) -> GCControllerButtonInput? {
            switch self {
            case .buttonA: pad.buttonA
            case .buttonB: pad.buttonB
            case .buttonX: pad.buttonX
            case .buttonY: pad.buttonY
            case .leftShoulder: pad.leftShoulder
            case .rightShoulder: pad.rightShoulder
            case .leftTrigger: pad.leftTrigger
            case .rightTrigger: pad.rightTrigger
            case .leftStick: pad.leftThumbstickButton
            case .rightStick: pad.rightThumbstickButton
            case .menu: pad.buttonMenu
            case .options: pad.buttonOptions
            case .dpadUp: pad.dpad.up
            case .dpadDown: pad.dpad.down
            case .dpadLeft: pad.dpad.left
            case .dpadRight: pad.dpad.right
            }
        }

        /// Its name when the controller doesn't give one.
        var name: String {
            switch self {
            case .buttonA: "A"
            case .buttonB: "B"
            case .buttonX: "X"
            case .buttonY: "Y"
            case .leftShoulder: "Left Shoulder"
            case .rightShoulder: "Right Shoulder"
            case .leftTrigger: "Left Trigger"
            case .rightTrigger: "Right Trigger"
            case .leftStick: "Left Stick Button"
            case .rightStick: "Right Stick Button"
            case .menu: "Menu"
            case .options: "Options"
            case .dpadUp: "D-pad Up"
            case .dpadDown: "D-pad Down"
            case .dpadLeft: "D-pad Left"
            case .dpadRight: "D-pad Right"
            }
        }

        /// What it does unless rebound: the mapping by position.
        var defaultTarget: Target {
            switch self {
            case .buttonA: .button(FrontlineButtonA)
            case .buttonB: .button(FrontlineButtonB)
            case .buttonX: .button(FrontlineButtonX)
            case .buttonY: .button(FrontlineButtonY)
            case .leftShoulder, .rightShoulder: .button(FrontlineButtonZ)
            case .leftTrigger: .button(FrontlineButtonL)
            case .rightTrigger: .button(FrontlineButtonR)
            case .menu: .button(FrontlineButtonStart)
            case .dpadUp: .button(FrontlineButtonUp)
            case .dpadDown: .button(FrontlineButtonDown)
            case .dpadLeft: .button(FrontlineButtonLeft)
            case .dpadRight: .button(FrontlineButtonRight)
            case .leftStick, .rightStick, .options: .nothing
            }
        }
    }

    /// What a button does. L and R are analog: a trigger bound to them
    /// presses as far as it's pulled, and clicks the digital button at the
    /// end, as on the GameCube controller; any other button presses them all
    /// the way.
    enum Target: Hashable, Sendable {
        case nothing
        case button(Int)     // FrontlineButton*
        case action(UInt32)  // FrontlineAction*

        var saved: String {
            switch self {
            case .nothing: "nothing"
            case .button(let bit): "button:\(bit)"
            case .action(let bits): "action:\(bits)"
            }
        }

        init?(saved: String) {
            let parts = saved.split(separator: ":")
            if parts == ["nothing"] {
                self = .nothing
                return
            }
            guard parts.count == 2, let value = UInt32(parts[1]) else {
                return nil
            }
            switch parts[0] {
            case "button" where PadBindings.buttons.contains { $0.bit == Int(value) }: self = .button(Int(value))
            case "action" where PadBindings.actions.contains { $0.bits == value }: self = .action(value)
            default: return nil
            }
        }

        var name: String {
            switch self {
            case .nothing: "Nothing"
            case .button(let bit): PadBindings.buttons.first { $0.bit == bit }?.name ?? "?"
            case .action(let bits): PadBindings.actions.first { $0.bits == bits }?.name ?? "?"
            }
        }
    }

    /// The GameCube controller's buttons, as targets.
    nonisolated static let buttons: [(bit: Int, name: String)] = [
        (FrontlineButtonA, "GameCube A"), (FrontlineButtonB, "GameCube B"),
        (FrontlineButtonX, "GameCube X"), (FrontlineButtonY, "GameCube Y"),
        (FrontlineButtonZ, "GameCube Z"), (FrontlineButtonL, "GameCube L"), (FrontlineButtonR, "GameCube R"),
        (FrontlineButtonStart, "GameCube START"),
        (FrontlineButtonUp, "D-pad Up"), (FrontlineButtonDown, "D-pad Down"),
        (FrontlineButtonLeft, "D-pad Left"), (FrontlineButtonRight, "D-pad Right"),
    ]

    /// The game actions a button can do, whatever the controller layout picked
    /// in the game's options.
    nonisolated static let actions: [(bits: UInt32, name: String)] = [
        (UInt32(FrontlineActionMelee), "Melee"),
        (UInt32(FrontlineActionReload), "Reload"),
        (UInt32(FrontlineActionUse), "Action"),
        (UInt32(FrontlineActionJump), "Jump"),
        (UInt32(FrontlineActionCrouch), "Crouch"),
        (UInt32(FrontlineActionNextWeapon), "Next weapon"),
        (UInt32(FrontlineActionPrevWeapon), "Previous weapon"),
        (UInt32(FrontlineActionCenterView), "Center view"),
        (UInt32(FrontlineActionHint), "Hint from HQ"),
        (UInt32(FrontlineActionScores), "Frag counts (hold)"),
        (UInt32(FrontlineActionPause), "Pause / continue"),
    ]

    static var targets: [Target] {
        [.nothing] + buttons.map { .button($0.bit) } + actions.map { .action($0.bits) }
    }

    /// One kind of controller's bindings.
    struct Profile: Codable, Equatable, Sendable {
        /// Input id -> `Target.saved`, for the inputs rebound.
        var targets: [String: String] = [:]
        var swapSticks = false
        var invertMain = false
        var invertC = false

        func target(_ input: Input) -> Target {
            targets[input.rawValue].flatMap(Target.init(saved:)) ?? input.defaultTarget
        }

        mutating func set(_ input: Input, _ target: Target) {
            targets[input.rawValue] = target == input.defaultTarget ? nil : target.saved
        }
    }

    /// The kind `controller` is: its profile's name.
    nonisolated static func kind(of controller: GCController) -> String {
        controller.productCategory
    }

    static let key = "padBindings"

    private static var cache: [String: Profile]?

    static func profile(for kind: String) -> Profile {
        profiles()[kind] ?? Profile()
    }

    static func save(_ profile: Profile, for kind: String) {
        var all = profiles()
        all[kind] = profile == Profile() ? nil : profile
        cache = all
        guard let data = try? JSONEncoder().encode(all) else {
            return
        }
        UserDefaults.standard.set(data, forKey: key)
        #if os(tvOS)
        NSUbiquitousKeyValueStore.default.set(data, forKey: key)
        #endif
    }

    private static func profiles() -> [String: Profile] {
        if let cache {
            return cache
        }
        let data = UserDefaults.standard.data(forKey: key)
        let all = data.flatMap { try? JSONDecoder().decode([String: Profile].self, from: $0) } ?? [:]
        cache = all
        return all
    }

    #if os(tvOS)
    /// Takes iCloud's copy, now and whenever another device changes it.
    static func syncWithICloud() {
        NSUbiquitousKeyValueStore.default.synchronize()
        adoptICloud()
        NotificationCenter.default.addObserver(forName: NSUbiquitousKeyValueStore.didChangeExternallyNotification,
                                               object: nil, queue: .main) { _ in
            MainActor.assumeIsolated {
                adoptICloud()
            }
        }
    }

    private static func adoptICloud() {
        if let data = NSUbiquitousKeyValueStore.default.data(forKey: key) {
            UserDefaults.standard.set(data, forKey: key)
            cache = nil
        }
    }
    #endif
}
