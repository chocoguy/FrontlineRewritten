import AppKit
import SwiftUI

@main
struct FrontlineApp: App {
    @NSApplicationDelegateAdaptor private var delegate: AppDelegate

    var body: some Scene {
        // One window: the game is one console. It shows the setup screen
        // until Play.
        Window("Frontline Rewritten", id: "game") {
            RootView()
                .frame(minWidth: 640, minHeight: 480)
                .background(.black)
        }
        .defaultSize(width: 960, height: 720)
        .windowResizability(.contentMinSize)

        SwiftUI.Settings {
            SettingsView()
        }
    }
}

/// The setup screen, then the game.
struct RootView: View {
    @State private var playing = Game.isRelaunch

    var body: some View {
        if playing {
            GameView()
        } else {
            SetupView {
                playing = Game.start()
            }
        }
    }
}

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        // With FRONTLINE_RESTART_EXEC=1 the game relaunches the app between
        // levels (it reboots in place otherwise): straight back in, with the
        // settings it ran with.
        if Game.isRelaunch && !Game.start() {
            NSApp.terminate(nil)
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }

    func applicationWillTerminate(_ notification: Notification) {
        // The game's threads are still running: leave without static destructors.
        _exit(0)
    }
}

/// Starting the game with the setup screen's settings (`Settings`).
@MainActor
enum Game {
    /// The game restarted the app to change levels (LoadElf, with
    /// FRONTLINE_RESTART_EXEC=1; the shell's state rides along in
    /// FRONTLINE_RESTART).
    static let isRelaunch = ProcessInfo.processInfo.environment["FRONTLINE_RESTART"] != nil

    /// Starts the game; false if there's no disc to play.
    static func start() -> Bool {
        guard let disc = Disc.locate() else {
            return false
        }
        let defaults = UserDefaults.standard
        let rewritten = defaults.string(forKey: Settings.gameMode) == Settings.rewritten
        FrontlineSetGameMode(Int32(rewritten ? FrontlineModeRewritten : FrontlineModeClassic))
        FrontlineSetRenderLines(Int32(Settings.renderLines(defaults)))
        FrontlineSetWidescreen(defaults.bool(forKey: Settings.widescreen) ? Float(Settings.screenAspect) : 0)
        FrontlineSetHighFrameRate(defaults.bool(forKey: Settings.highFrameRate))
        FrontlineSetMsaa(Int32(Settings.msaa(defaults)))
        FrontlineSetDrawDistance(Float(Settings.drawDistance(defaults)))
        Input.publishKeyLabels()
        guard FrontlineStart(disc, MemoryCard.folder()) else {
            let alert = NSAlert()
            alert.messageText = "The game couldn't start"
            alert.informativeText = "Its game image (FrontlineGame.so) is missing or didn't load. Rebuild the app."
            alert.runModal()
            return false
        }
        DisplayAwake.start()
        return true
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
    static let key = "disc"

    /// The disc found without asking, if any.
    static func current() -> String? {
        let candidates = [
            UserDefaults.standard.string(forKey: key),
            ProcessInfo.processInfo.environment["FRONTLINE_DISC"],
        ]
        return candidates.compactMap { $0 }.first { FrontlineIsDisc($0) }
    }

    static func locate() -> String? {
        current() ?? choose()
    }

    /// Asks for the disc until a folder is one (or the panel is cancelled),
    /// and remembers it.
    @discardableResult
    static func choose() -> String? {
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
        panel.prompt = "Choose"
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        return panel.runModal() == .OK ? panel.url?.path : nil
    }
}
