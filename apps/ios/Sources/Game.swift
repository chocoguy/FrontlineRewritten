import Foundation

/// Starting the game with the setup screen's settings (`Settings`).
@MainActor
enum Game {
    /// The game is running (not before Play, nor after Exit Game).
    private(set) static var started = false

    /// Starts the game; the reason, if it can't. After Exit Game, it starts
    /// again from a cold boot with the setup screen's settings as they are
    /// now.
    static func start() -> String? {
        guard !started else {
            return nil
        }
        guard let disc = Disc.locate() else {
            return Disc.missing
        }
        guard let card = MemoryCard.folder() else {
            return "The app has nowhere to keep its memory card."
        }
        #if os(tvOS)
        CardBackup.restoreIfNeeded(folder: card)
        FrontlineSetCardChangeCallback(cardChanged)
        #endif
        Settings.apply(.standard, aspect: Settings.screenAspect)
        FrontlineSetExitGameOffered(true)
        guard FrontlineStart(disc, card.path) else {
            return "The game image (FrontlineGame.so) is missing or didn't load. Rebuild the app."
        }
        started = true
        return nil
    }

    /// Exit Game: stops the game in place, for the setup screen. False if it
    /// didn't stop within a few seconds (it's still running).
    static func stop() -> Bool {
        guard started, FrontlineStopGame() else {
            return false
        }
        started = false
        return true
    }
}

#if os(tvOS)
/// The TV's screen, for the shared settings (SettingsKeys.swift).
extension Settings {
    /// TVs show the game at 60 Hz.
    static let screenRate = 60

    /// A TV is 16:9.
    static let screenAspect: CGFloat = 16.0 / 9.0
}

/// The card's change hook (FrontlineSetCardChangeCallback): backs the card up
/// (CardBackup). It runs on the game's thread, so it's a top-level function
/// with no actor of its own: a closure written in a main-actor function
/// would be the main actor's, and Swift's isolation check stops the app when
/// another thread calls it.
private func cardChanged(_ path: UnsafePointer<CChar>?, _ removed: Bool) {
    CardBackup.schedule(folder: MemoryCard.location)
}
#endif

/// The app's Caches folder: the only place a tvOS app writes (the memory
/// card is backed up in the user defaults, CardBackup), and where a disc is
/// pushed during development.
enum Caches {
    static func folder(_ name: String) -> URL? {
        guard let caches = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first else {
            return nil
        }
        return caches.appendingPathComponent(name, isDirectory: true)
    }
}

/// The memory card in slot A, a folder of .gci saves: in Application Support
/// on iPhone and iPad, which lasts; in Caches on the TV, the only place it can
/// write.
enum MemoryCard {
    static var location: URL {
        #if os(tvOS)
        let base = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)
        #else
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)
        #endif
        let root = base.first ?? URL(fileURLWithPath: NSTemporaryDirectory(), isDirectory: true)
        return root.appendingPathComponent("Memory Card A", isDirectory: true)
    }

    /// The folder, made if it isn't there; nil if it can't be.
    static func folder() -> URL? {
        let folder = location
        do {
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        } catch {
            return nil
        }
        return folder
    }
}

/// Finds the extracted disc (the folder holding sys/ and files/): the
/// FRONTLINE_DISC environment variable, a Disc folder in Caches (pushed there
/// during development), or the Disc folder in the app.
enum Disc {
    static let missing = """
        No disc found. Build the app with the disc in it, or launch it with FRONTLINE_DISC set to \
        an extracted disc folder.
        """

    static func locate() -> String? {
        let candidates = [
            ProcessInfo.processInfo.environment["FRONTLINE_DISC"],
            Caches.folder("Disc")?.path,
            Bundle.main.url(forResource: "Disc", withExtension: nil)?.path,
        ]
        return candidates.compactMap { $0 }.first { FrontlineIsDisc($0) }
    }
}
