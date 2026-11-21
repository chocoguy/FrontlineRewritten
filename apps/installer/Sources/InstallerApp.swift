// Frontline Installer: builds Frontline Rewritten from the player's own disc, at a GitHub release
// tag, and installs it on this Mac, iPhones, iPads and Apple TVs; updates them all when a new release
// comes out. Distributed signed and notarized (tools/release.sh), and updated by Sparkle.

import AppKit
import SwiftUI
#if canImport(Sparkle)
import Sparkle
#endif

@main
struct InstallerApp: App {
    @StateObject private var model = AppModel()
    @StateObject private var updater = InstallerUpdater()

    var body: some Scene {
        Window("Frontline Installer", id: "main") {
            ContentView()
                .environmentObject(model)
                .environmentObject(updater)
                .task { model.start() }
        }
        .commands {
            CommandGroup(after: .appInfo) {
                Button("Check for Updates…", action: updater.check).disabled(!updater.available)
            }
        }
    }
}

/// The installer's own updates: Sparkle, reading the appcast attached to the latest GitHub release.
/// Off in builds without a Sparkle key (local builds), so they never offer to replace themselves.
@MainActor
final class InstallerUpdater: ObservableObject {
    #if canImport(Sparkle)
    private let controller: SPUStandardUpdaterController?
    #endif

    var available: Bool {
        #if canImport(Sparkle)
        controller != nil
        #else
        false
        #endif
    }

    init() {
        #if canImport(Sparkle)
        let key = Bundle.main.object(forInfoDictionaryKey: "SUPublicEDKey") as? String ?? ""
        controller = key.isEmpty ? nil
            : SPUStandardUpdaterController(startingUpdater: true, updaterDelegate: nil, userDriverDelegate: nil)
        #endif
    }

    func check() {
        #if canImport(Sparkle)
        controller?.checkForUpdates(nil)
        #endif
    }
}
