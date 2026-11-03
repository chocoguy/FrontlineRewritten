import Foundation

/// Starting the game with the setup screen's settings (`Settings`).
@MainActor
enum Game {
    private(set) static var started = false

    /// Starts the game, once the saves are in from iCloud (or a few seconds
    /// have passed); the reason, if it can't.
    static func start() async -> String? {
        guard !started else {
            return nil
        }
        guard let disc = Disc.locate() else {
            return Disc.missing
        }
        await Saves.handToGame(within: 5)
        Settings.apply(.standard, aspect: Settings.screenAspect)
        guard FrontlineStart(disc, MemoryCard.folder()) else {
            return "The game image (FrontlineGame.so) is missing or didn't load. Rebuild the app."
        }
        started = true
        return nil
    }
}

/// The TV's screen, for the shared settings (SettingsKeys.swift).
extension Settings {
    /// TVs show the game at 60 Hz.
    static let screenRate = 60

    /// A TV is 16:9.
    static let screenAspect: CGFloat = 16.0 / 9.0
}

/// The app's Caches folder: the only place a tvOS app writes (its saves are
/// also kept in iCloud from T6 on).
enum Caches {
    static func folder(_ name: String) -> URL? {
        guard let caches = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first else {
            return nil
        }
        return caches.appendingPathComponent(name, isDirectory: true)
    }
}

/// The memory card's saves in iCloud (SaveSync), when the app is signed for
/// it: Info.plist's FrontlineICloudContainer, which only builds with a team
/// have. Nil otherwise, and the saves stay in Caches.
enum Saves {
    static let sync: SaveSync? = {
        guard let container = Bundle.main.object(forInfoDictionaryKey: "FrontlineICloudContainer") as? String,
              !container.isEmpty, let folder = MemoryCard.folder() else {
            return nil
        }
        return SaveSync(container: container, folder: URL(fileURLWithPath: folder, isDirectory: true))
    }()

    @MainActor private static var preparation: Task<Void, Never>?

    /// Brings the card and iCloud up to date with each other: at launch, so
    /// it's usually done by the time the player presses Play.
    @MainActor static func prepare() {
        guard let sync else {
            return
        }
        preparation = Task {
            await sync.prepare()
        }
    }

    /// Waits for `prepare`, `seconds` at most (offline, iCloud can take a
    /// while to give up), then hands the card to the game: from then on the
    /// game's saves go up to iCloud as it writes them.
    @MainActor static func handToGame(within seconds: Double) async {
        guard let sync else {
            return
        }
        if let preparation {
            await withTaskGroup(of: Void.self) { group in
                group.addTask {
                    await preparation.value
                }
                group.addTask {
                    try? await Task.sleep(for: .seconds(seconds))
                }
                await group.next()
                group.cancelAll()
            }
        }
        await sync.beginGame()
        FrontlineSetCardChangeCallback(cardChanged)
    }
}

/// The card's change callback (FrontlineSetCardChangeCallback). It runs on the
/// game's thread, so it's a top-level function with no actor of its own: a
/// closure written in `handToGame` would be the main actor's, and Swift's
/// isolation check stops the app when another thread calls it.
private func cardChanged(_ path: UnsafePointer<CChar>?, _ removed: Bool) {
    guard let path else {
        return
    }
    let file = String(cString: path)
    Task {
        await Saves.sync?.changed(path: file)
    }
}

/// The memory card in slot A: a folder of .gci saves in Caches.
enum MemoryCard {
    static func folder() -> String? {
        guard let folder = Caches.folder("Memory Card A") else {
            return nil
        }
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        return folder.path
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
