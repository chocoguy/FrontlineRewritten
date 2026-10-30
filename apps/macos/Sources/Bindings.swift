import AppKit
import GameController

/// What the keyboard and mouse bind to the playing actions, as players set
/// it in the Settings window (up to two keys or mouse buttons each), and the
/// fixed keys for the menus.
///
/// Saved in the `keyBindings` default as action id -> ["key:<HID usage>" or
/// "mouse:<button>"]; an action missing there keeps its defaults.
@MainActor
enum Bindings {
    /// A key, or a mouse button by NSEvent's number (0 left, 1 right, 2
    /// middle, 3 and 4 the side buttons).
    enum Input: Hashable {
        case key(GCKeyCode)
        case mouse(Int)

        var saved: String {
            switch self {
            case .key(let code): "key:\(code.rawValue)"
            case .mouse(let button): "mouse:\(button)"
            }
        }

        init?(saved: String) {
            let parts = saved.split(separator: ":")
            guard parts.count == 2, let value = Int(parts[1]) else {
                return nil
            }
            switch parts[0] {
            case "key" where value > 0: self = .key(GCKeyCode(rawValue: value))
            case "mouse" where (0...4).contains(value): self = .mouse(value)
            default: return nil
            }
        }

        /// As players read it ("F", "Right Mouse Button").
        var name: String {
            switch self {
            case .key(let code): Bindings.name(of: code)
            case .mouse(let button): Bindings.mouseNames[button]
            }
        }
    }

    struct Action: Identifiable {
        let id: String
        let name: String
        let bits: UInt32
        let defaults: [Input]
    }

    static let slots = 2

    /// The actions players can rebind, in the Settings window's order.
    static let actions: [Action] = [
        Action(id: "forward", name: "Forward", bits: UInt32(FrontlineActionForward), defaults: [.key(.keyW)]),
        Action(id: "back", name: "Back", bits: UInt32(FrontlineActionBack), defaults: [.key(.keyS)]),
        Action(id: "left", name: "Left", bits: UInt32(FrontlineActionLeft), defaults: [.key(.keyA)]),
        Action(id: "right", name: "Right", bits: UInt32(FrontlineActionRight), defaults: [.key(.keyD)]),
        Action(id: "walk", name: "Walk (hold)", bits: UInt32(FrontlineActionWalk), defaults: [.key(.leftShift)]),
        Action(id: "jump", name: "Jump", bits: UInt32(FrontlineActionJump), defaults: [.key(.spacebar)]),
        Action(id: "crouch", name: "Crouch", bits: UInt32(FrontlineActionCrouch),
               defaults: [.key(.keyC), .key(.leftControl)]),
        Action(id: "use", name: "Action", bits: UInt32(FrontlineActionUse), defaults: [.key(.keyF)]),
        Action(id: "fire", name: "Fire", bits: UInt32(FrontlineActionFire), defaults: [.mouse(0)]),
        Action(id: "aim", name: "Aim (hold)", bits: UInt32(FrontlineActionAim), defaults: [.mouse(1)]),
        Action(id: "reload", name: "Reload", bits: UInt32(FrontlineActionReload), defaults: [.key(.keyR)]),
        Action(id: "melee", name: "Melee", bits: UInt32(FrontlineActionMelee), defaults: [.key(.keyV), .mouse(2)]),
        Action(id: "nextWeapon", name: "Next weapon", bits: UInt32(FrontlineActionNextWeapon),
               defaults: [.key(.keyQ)]),
        Action(id: "prevWeapon", name: "Previous weapon", bits: UInt32(FrontlineActionPrevWeapon),
               defaults: [.key(.keyE)]),
        Action(id: "centerView", name: "Center view", bits: UInt32(FrontlineActionCenterView), defaults: []),
        Action(id: "hint", name: "Hint from HQ", bits: UInt32(FrontlineActionHint), defaults: [.key(.keyH)]),
        Action(id: "scores", name: "Frag counts (hold)", bits: UInt32(FrontlineActionScores),
               defaults: [.key(.tab)]),
        Action(id: "pause", name: "Pause / continue", bits: UInt32(FrontlineActionPause), defaults: [.key(.escape)]),
    ]

    /// The menus' keys (the console's buttons), which don't change. A key can
    /// carry a playing action too: the game reads one set or the other.
    static let menuKeys: [(GCKeyCode, UInt32)] = [
        (.spacebar, UInt32(FrontlineActionMenuStart)),
        (.escape, UInt32(FrontlineActionMenuB)),
        (.deleteOrBackspace, UInt32(FrontlineActionMenuB)),
        (.returnOrEnter, UInt32(FrontlineActionMenuA)),
        (.keypadEnter, UInt32(FrontlineActionMenuA)),
        (.upArrow, UInt32(FrontlineActionMenuUp)),
        (.downArrow, UInt32(FrontlineActionMenuDown)),
        (.leftArrow, UInt32(FrontlineActionMenuLeft)),
        (.rightArrow, UInt32(FrontlineActionMenuRight)),
        (.keyX, UInt32(FrontlineActionMenuX)),
        (.keyY, UInt32(FrontlineActionMenuY)),
        (.keyZ, UInt32(FrontlineActionMenuZ)),
        (.keyL, UInt32(FrontlineActionMenuL)),
        (.keyR, UInt32(FrontlineActionMenuR)),
    ]

    /// How the menu keys read in the Settings window.
    static let menuKeyNames: [(String, String)] = [
        ("Move", "Arrow keys"),
        ("A / B", "Return / Esc or Delete"),
        ("START", "Space"),
        ("X Y Z L R", "X Y Z L R"),
    ]

    static let key = "keyBindings"

    /// Each action's inputs, as saved (or the defaults).
    static func load(_ defaults: UserDefaults = .standard) -> [String: [Input]] {
        let saved = defaults.dictionary(forKey: key) as? [String: [String]] ?? [:]
        var bindings: [String: [Input]] = [:]
        for action in actions {
            if let inputs = saved[action.id] {
                bindings[action.id] = Array(inputs.compactMap(Input.init(saved:)).prefix(slots))
            } else {
                bindings[action.id] = action.defaults
            }
        }
        return bindings
    }

    static func save(_ bindings: [String: [Input]], _ defaults: UserDefaults = .standard) {
        var saved: [String: [String]] = [:]
        for (id, inputs) in bindings {
            saved[id] = inputs.map(\.saved)
        }
        defaults.set(saved, forKey: key)
    }

    static func reset(_ defaults: UserDefaults = .standard) {
        defaults.removeObject(forKey: key)
    }

    // The tables Input reads each frame, rebuilt when the saved bindings change.

    private(set) static var keys: [(GCKeyCode, UInt32)] = []
    private(set) static var mouseButtons: [Int: UInt32] = [:]
    private static var loaded: [String: [String]]?

    /// Rebuilds the tables if the saved bindings changed (or were never
    /// read); true if they did.
    @discardableResult
    static func refresh() -> Bool {
        let saved = UserDefaults.standard.dictionary(forKey: key) as? [String: [String]] ?? [:]
        if let loaded, loaded == saved {
            return false
        }
        loaded = saved
        var keyBits: [GCKeyCode: UInt32] = [:]
        var buttons: [Int: UInt32] = [:]
        for (code, bits) in menuKeys {
            keyBits[code, default: 0] |= bits
        }
        let bindings = load()
        for action in actions {
            for input in bindings[action.id] ?? [] {
                switch input {
                case .key(let code): keyBits[code, default: 0] |= action.bits
                case .mouse(let button): buttons[button, default: 0] |= action.bits
                }
            }
        }
        keys = keyBits.map { ($0.key, $0.value) }.sorted { $0.0.rawValue < $1.0.rawValue }
        mouseButtons = buttons
        return true
    }

    /// The first input bound to `bits`' action, as players read it.
    static func label(of bits: UInt32) -> String? {
        let bindings = load()
        if let action = actions.first(where: { $0.bits & bits != 0 }) {
            return bindings[action.id]?.first?.name
        }
        if let (code, _) = menuKeys.first(where: { $0.1 & bits != 0 }) {
            return name(of: code)
        }
        return nil
    }

    // Names. Anything longer than 7 characters needs a short form in
    // platform/src/prompts.cpp (and a line in tests/datacheck/messages.cpp),
    // so prompts still fit their 49 characters.

    nonisolated static let mouseNames = [
        "Left Mouse Button", "Right Mouse Button", "Middle Mouse Button", "Mouse Button 4", "Mouse Button 5",
    ]

    /// Keys that can't be bound: Command (the system's shortcuts), and Delete,
    /// which clears a binding in the Settings window.
    nonisolated static func bindable(_ code: GCKeyCode) -> Bool {
        code != .leftGUI && code != .rightGUI && code != .deleteOrBackspace
    }

    nonisolated static func name(of code: GCKeyCode) -> String {
        let usage = code.rawValue
        switch usage {
        case 4...29: return String(UnicodeScalar(UInt8(65 + usage - 4)))
        case 30...38: return String(usage - 29)
        case 39: return "0"
        case 58...69: return "F\(usage - 57)"
        case 104...115: return "F\(usage - 91)"
        case 89...97: return "Keypad \(usage - 88)"
        default: break
        }
        let names: [Int: String] = [
            40: "Return", 41: "Esc", 42: "Delete", 43: "Tab", 44: "Space", 45: "-", 46: "=", 47: "[", 48: "]",
            49: "\\", 50: "#", 51: ";", 52: "'", 53: "`", 54: ",", 55: ".", 56: "/", 57: "Caps Lock",
            70: "Print Screen", 71: "Scroll Lock", 72: "Pause", 73: "Insert", 74: "Home", 75: "Page Up",
            76: "Fwd Delete", 77: "End", 78: "Page Down", 79: "Right", 80: "Left", 81: "Down", 82: "Up",
            83: "Num Lock", 84: "Keypad /", 85: "Keypad *", 86: "Keypad -", 87: "Keypad +", 88: "Keypad Enter",
            98: "Keypad 0", 99: "Keypad .", 100: "§", 103: "Keypad =", 224: "Control", 225: "Shift",
            226: "Option", 228: "Right Control", 229: "Right Shift", 230: "Right Option",
        ]
        return names[usage] ?? "Key \(usage)"
    }
}
