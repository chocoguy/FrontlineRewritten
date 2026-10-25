import AppKit
import SwiftUI

@main
struct FrontlineApp: App {
    @NSApplicationDelegateAdaptor private var delegate: AppDelegate

    var body: some Scene {
        // One window: the game is one console.
        Window("Frontline Rewritten", id: "game") {
            GameView()
                .frame(minWidth: 640, minHeight: 480)
                .background(.black)
        }
        .defaultSize(width: 960, height: 720)
        .windowResizability(.contentMinSize)
    }
}

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        guard let disc = Disc.locate() else {
            NSApp.terminate(nil)
            return
        }
        FrontlineStart(disc, MemoryCard.folder())
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }

    func applicationWillTerminate(_ notification: Notification) {
        // The game's threads are still running: leave without static destructors.
        _exit(0)
    }
}

/// The memory card in slot A: a folder of .gci saves (Dolphin's GCI folder
/// format) in Application Support, or the `memoryCard` default.
enum MemoryCard {
    static func folder() -> String? {
        if let folder = UserDefaults.standard.string(forKey: "memoryCard") {
            return folder
        }
        guard let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first else {
            return nil
        }
        let folder = support.appendingPathComponent("Frontline Rewritten/Memory Card A", isDirectory: true)
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        return folder.path
    }
}

/// Finds the extracted disc (the folder holding sys/ and files/): the `disc`
/// default (also `-disc <folder>` on the command line), the FRONTLINE_DISC
/// environment variable, or else asks for it and remembers the choice.
@MainActor
enum Disc {
    private static let key = "disc"

    static func locate() -> String? {
        let candidates = [
            UserDefaults.standard.string(forKey: key),
            ProcessInfo.processInfo.environment["FRONTLINE_DISC"],
        ]
        for case let folder? in candidates where FrontlineIsDisc(folder) {
            return folder
        }
        while let folder = ask() {
            if FrontlineIsDisc(folder) {
                UserDefaults.standard.set(folder, forKey: key)
                return folder
            }
            let alert = NSAlert()
            alert.messageText = "That folder isn't an extracted disc"
            alert.informativeText = "Choose the folder that holds the disc's sys and files folders."
            alert.runModal()
        }
        return nil
    }

    private static func ask() -> String? {
        let panel = NSOpenPanel()
        panel.message = "Choose your extracted Medal of Honor: Frontline (GMFE69) disc: the folder that holds sys and files."
        panel.prompt = "Play"
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        return panel.runModal() == .OK ? panel.url?.path : nil
    }
}
