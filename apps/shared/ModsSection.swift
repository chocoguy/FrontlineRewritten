import SwiftUI

/// Rewritten's mods, a toggle each (`Settings.mods`): the Mac's Settings and
/// the TV's setup screen.
struct ModsSection: View {
    /// Rewritten is on; Classic has no mods.
    let rewritten: Bool

    var body: some View {
        Section("Mods") {
            ForEach(Settings.mods) { mod in
                ModToggle(mod: mod, rewritten: rewritten)
            }
            Text(rewritten
                 ? "Single player. A mod takes effect from the next mission or stage you start."
                 : "Mods are part of Rewritten; Classic keeps the original game.")
                .foregroundStyle(.secondary)
        }
    }
}

private struct ModToggle: View {
    let mod: Settings.Mod
    let rewritten: Bool
    @AppStorage private var on: Bool

    init(mod: Settings.Mod, rewritten: Bool) {
        self.mod = mod
        self.rewritten = rewritten
        _on = AppStorage(wrappedValue: false, mod.defaultsKey)
    }

    var body: some View {
        Toggle(isOn: rewritten ? $on : .constant(false)) {
            Text(mod.title)
            Text(mod.summary)
        }
        .disabled(!rewritten)
    }
}
