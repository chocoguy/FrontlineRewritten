import SwiftUI

/// A setting picked from a list: its name on the left, the current choice on
/// the right, and the choices in a menu over the screen.
///
/// On tvOS a menu-style Picker in a Form shows only the current choice, so the
/// setting's name went missing (2026-11-14); there this is a Menu whose label
/// is the whole row. Elsewhere it's a plain menu Picker, which already shows
/// both.
struct ChoiceRow<Value: Hashable, Label: View>: View {
    @Binding var selection: Value
    let options: [(value: Value, name: String)]
    let label: Label

    init(selection: Binding<Value>, options: [(value: Value, name: String)], @ViewBuilder label: () -> Label) {
        _selection = selection
        self.options = options
        self.label = label()
    }

    var body: some View {
        #if os(tvOS)
        Menu {
            picker.pickerStyle(.inline)
        } label: {
            HStack {
                label
                Spacer()
                Text(current).foregroundStyle(.secondary)
            }
        }
        #else
        picker.pickerStyle(.menu)
        #endif
    }

    private var picker: some View {
        Picker(selection: $selection) {
            ForEach(options.indices, id: \.self) { i in
                Text(options[i].name).tag(options[i].value)
            }
        } label: {
            label
        }
    }

    private var current: String {
        options.first { $0.value == selection }?.name ?? ""
    }
}

extension ChoiceRow where Label == Text {
    init(_ title: String, selection: Binding<Value>, options: [(value: Value, name: String)]) {
        self.init(selection: selection, options: options) { Text(title) }
    }
}
