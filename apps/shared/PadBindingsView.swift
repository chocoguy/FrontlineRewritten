import GameController
import SwiftUI

/// The controller screen: what each button of a kind of controller does
/// (`PadBindings`), listed with that controller's own button names and glyphs.
/// Changes apply at once, also while the game runs. Controllers of the same
/// kind share their bindings.
///
/// Each button picks what it does from a list, rather than "press the new
/// button": on the TV, A and B also move around the screen.
struct PadBindingsView: View {
    @State private var gamepads = PadBindingsView.connected()
    @State private var kind = ""
    @State private var profile = PadBindings.Profile()

    var body: some View {
        Form {
            if gamepads.isEmpty {
                Text("Connect a game controller to set up its buttons.").foregroundStyle(.secondary)
            } else {
                if kinds.count > 1 {
                    Picker("Controller", selection: $kind) {
                        ForEach(kinds, id: \.self) { kind in
                            Text(kind).tag(kind)
                        }
                    }
                }
                Section("Buttons") {
                    ForEach(inputs) { input in
                        Picker(selection: target(input)) {
                            ForEach(PadBindings.targets, id: \.self) { target in
                                Text(target.name).tag(target)
                            }
                        } label: {
                            label(input)
                        }
                    }
                }
                Section("Sticks") {
                    Toggle("Swap the sticks", isOn: setting(\.swapSticks))
                    Toggle("Invert the main stick's up and down", isOn: setting(\.invertMain))
                    Toggle("Invert the C stick's up and down (looking)", isOn: setting(\.invertC))
                }
                Section {
                    Button("Reset to Defaults") {
                        profile = PadBindings.Profile()
                        PadBindings.save(profile, for: kind)
                    }
                    Text("By default the buttons map by position, as on the GameCube controller: the triggers are L and R, a shoulder button is Z, and Menu is START.")
                        .foregroundStyle(.secondary)
                }
            }
        }
        // Choices open over the screen (on tvOS the default pushed list came up
        // blank; on the Mac this is the default anyway).
        .pickerStyle(.menu)
        .onAppear(perform: refresh)
        .onChange(of: kind) {
            profile = PadBindings.profile(for: kind)
        }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidConnect)) { _ in
            refresh()
        }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidDisconnect)) { _ in
            refresh()
        }
    }

    private static func connected() -> [GCController] {
        GCController.controllers().filter { $0.extendedGamepad != nil }
    }

    private func refresh() {
        gamepads = Self.connected()
        if !kinds.contains(kind) {
            kind = kinds.first ?? ""
        }
        profile = PadBindings.profile(for: kind)
    }

    private var kinds: [String] {
        Array(Set(gamepads.map(PadBindings.kind(of:)))).sorted()
    }

    /// A connected controller of the kind shown, for its buttons' names.
    private var pad: GCExtendedGamepad? {
        gamepads.first { PadBindings.kind(of: $0) == kind }?.extendedGamepad
    }

    /// The buttons this kind of controller has.
    private var inputs: [PadBindings.Input] {
        guard let pad else {
            return []
        }
        return PadBindings.Input.allCases.filter { $0.element(pad) != nil }
    }

    private func label(_ input: PadBindings.Input) -> some View {
        let element = pad.flatMap(input.element)
        return Label(element?.localizedName ?? input.name, systemImage: element?.sfSymbolsName ?? "gamecontroller")
    }

    private func target(_ input: PadBindings.Input) -> Binding<PadBindings.Target> {
        Binding {
            profile.target(input)
        } set: { target in
            profile.set(input, target)
            PadBindings.save(profile, for: kind)
        }
    }

    private func setting(_ path: WritableKeyPath<PadBindings.Profile, Bool>) -> Binding<Bool> {
        Binding {
            profile[keyPath: path]
        } set: { on in
            profile[keyPath: path] = on
            PadBindings.save(profile, for: kind)
        }
    }
}
