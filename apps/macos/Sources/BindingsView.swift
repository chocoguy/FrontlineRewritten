import AppKit
import GameController
import SwiftUI

/// The Settings window's controls: each playing action's two slots, which
/// take the next key or mouse button pressed, and the fixed menu keys.
struct ControlsSection: View {
    @StateObject private var editor = BindingsEditor()

    var body: some View {
        Section {
            ForEach(Bindings.actions) { action in
                LabeledContent(action.name) {
                    HStack(spacing: 6) {
                        ForEach(0..<Bindings.slots, id: \.self) { slot in
                            slotButton(action, slot)
                        }
                    }
                }
            }
            if let note = editor.note {
                Text(note).foregroundStyle(.secondary)
            }
            HStack {
                Text(editor.capturing != nil
                     ? "Press a key or mouse button. Esc cancels, Delete clears."
                     : "Click a slot to change it.")
                    .foregroundStyle(.secondary)
                Spacer()
                Button("Reset to Defaults") {
                    editor.reset()
                }
            }
        } header: {
            Text("Keyboard and mouse")
        }
        Section("Menus") {
            ForEach(Bindings.menuKeyNames, id: \.0) { button, keys in
                LabeledContent(button) {
                    Text(keys).foregroundStyle(.secondary)
                }
            }
            LabeledContent("Look / scope zoom") {
                Text("Mouse / mouse wheel").foregroundStyle(.secondary)
            }
        }
        .onDisappear {
            editor.cancel()
        }
    }

    private func slotButton(_ action: Bindings.Action, _ slot: Int) -> some View {
        let capturing = editor.capturing == BindingsEditor.Slot(action: action.id, index: slot)
        let inputs = editor.bindings[action.id] ?? []
        let title = capturing ? "Press…" : slot < inputs.count ? inputs[slot].name : "—"
        return Button {
            if capturing {
                editor.cancel()
            } else {
                editor.begin(action.id, slot)
            }
        } label: {
            Text(title)
                .lineLimit(1)
                .frame(width: 126)
        }
        .buttonStyle(.bordered)
        .tint(capturing ? .accentColor : nil)
    }
}

/// Edits the saved bindings (`Bindings`): captures the next key through
/// GCKeyboard (the key codes the game's input reads) and mouse buttons
/// through the window's events.
@MainActor
final class BindingsEditor: ObservableObject {
    struct Slot: Equatable {
        let action: String
        let index: Int
    }

    @Published private(set) var bindings = Bindings.load()
    @Published private(set) var capturing: Slot?
    /// What the last change moved, if anything.
    @Published private(set) var note: String?
    private var monitor: Any?

    func begin(_ action: String, _ index: Int) {
        cancel()
        note = nil
        capturing = Slot(action: action, index: index)
        GCKeyboard.coalesced?.keyboardInput?.keyChangedHandler = { [weak self] _, _, code, pressed in
            guard pressed else {
                return
            }
            MainActor.assumeIsolated {
                self?.pressed(code)
            }
        }
        // Mouse buttons bind; key presses are swallowed so the window doesn't
        // beep (GCKeyboard reports them).
        monitor = NSEvent.addLocalMonitorForEvents(
            matching: [.leftMouseDown, .rightMouseDown, .otherMouseDown, .keyDown]
        ) { [weak self] event in
            if event.type != .keyDown, event.buttonNumber <= 4 {
                self?.assign(.mouse(event.buttonNumber))
            }
            return nil
        }
    }

    func cancel() {
        capturing = nil
        GCKeyboard.coalesced?.keyboardInput?.keyChangedHandler = nil
        if let monitor {
            NSEvent.removeMonitor(monitor)
            self.monitor = nil
        }
    }

    func reset() {
        cancel()
        Bindings.reset()
        bindings = Bindings.load()
        note = nil
    }

    private func pressed(_ code: GCKeyCode) {
        guard let slot = capturing else {
            return
        }
        if code == .escape {
            cancel()
        } else if code == .deleteOrBackspace {
            var inputs = bindings[slot.action] ?? []
            if slot.index < inputs.count {
                inputs.remove(at: slot.index)
            }
            bindings[slot.action] = inputs
            Bindings.save(bindings)
            cancel()
        } else if Bindings.bindable(code) {
            assign(.key(code))
        }
    }

    /// Binds `input` to the slot being captured, taking it from any other
    /// action that had it.
    private func assign(_ input: Bindings.Input) {
        guard let slot = capturing else {
            return
        }
        var moved: [String] = []
        for action in Bindings.actions {
            var inputs = bindings[action.id] ?? []
            if let at = inputs.firstIndex(of: input) {
                inputs.remove(at: at)
                bindings[action.id] = inputs
                if action.id != slot.action {
                    moved.append(action.name)
                }
            }
        }
        var inputs = bindings[slot.action] ?? []
        if slot.index < inputs.count {
            inputs[slot.index] = input
        } else {
            inputs.append(input)
        }
        bindings[slot.action] = inputs
        Bindings.save(bindings)
        note = moved.isEmpty ? nil : "\(input.name) moved from \(moved.joined(separator: ", "))."
        cancel()
    }
}
