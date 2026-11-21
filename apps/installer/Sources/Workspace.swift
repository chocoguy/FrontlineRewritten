import Foundation

/// Where the installer keeps everything: ~/Library/Application Support/Frontline Rewritten Installer.
struct Workspace: Sendable {
    let root: URL

    init(root: URL? = nil) {
        self.root = root ?? FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Frontline Rewritten Installer", isDirectory: true)
    }

    /// The git checkout of the release being built, with its submodule.
    var source: URL { root.appendingPathComponent("source", isDirectory: true) }
    /// tools/tvos/port's --work-dir: the prepared disc and the builds, kept across updates.
    var work: URL { root.appendingPathComponent("work", isDirectory: true) }
    /// The Mac app before the last update, for Undo.
    var previousMacApp: URL { work.appendingPathComponent("previous/Frontline Rewritten.app", isDirectory: true) }
    var tools: URL { root.appendingPathComponent("tools", isDirectory: true) }
    var logs: URL { root.appendingPathComponent("logs", isDirectory: true) }
    var stateFile: URL { root.appendingPathComponent("state.json") }
    var releasesCache: URL { root.appendingPathComponent("cache/releases.json") }
    var port: URL { source.appendingPathComponent("tools/tvos/port") }

    func load() -> InstallerState {
        guard let data = try? Data(contentsOf: stateFile),
              let state = try? JSONDecoder.installer.decode(InstallerState.self, from: data) else {
            return InstallerState()
        }
        return state
    }

    func save(_ state: InstallerState) {
        try? FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        if let data = try? JSONEncoder.installer.encode(state) {
            try? data.write(to: stateFile, options: .atomic)
        }
    }
}

enum Platform: String, Codable, CaseIterable, Identifiable, Sendable {
    case macos, ios, tvos

    var id: String { rawValue }

    var title: String {
        switch self {
        case .macos: "Mac"
        case .ios: "iPhone or iPad"
        case .tvos: "Apple TV"
        }
    }

    var symbol: String {
        switch self {
        case .macos: "desktopcomputer"
        case .ios: "iphone"
        case .tvos: "appletv"
        }
    }

    /// devicectl's name for the platform, for devices.
    var devicePlatform: String? {
        switch self {
        case .macos: nil
        case .ios: "iOS"
        case .tvos: "tvOS"
        }
    }

    /// xcodebuild's SDK, and the name -downloadPlatform takes.
    var sdk: String? {
        switch self {
        case .macos: nil
        case .ios: "iphoneos"
        case .tvos: "appletvos"
        }
    }

    var downloadName: String? {
        switch self {
        case .macos: nil
        case .ios: "iOS"
        case .tvos: "tvOS"
        }
    }

}

/// Where the game is installed: this Mac, or a device.
struct Install: Codable, Identifiable, Hashable, Sendable {
    var platform: Platform
    /// devicectl's identifier; nil for the Mac.
    var deviceId: String?
    var deviceName: String
    var version: String?
    var commit: String?
    var installedAt: Date?
    /// When the device's development profile stops the app opening.
    var expires: Date?
    /// The last update didn't reach it (it was off, or away): try again.
    var pending: Bool = false

    var id: String { platform.rawValue + ":" + (deviceId ?? "mac") }
}

/// What the installer remembers (state.json).
struct InstallerState: Codable, Sendable {
    /// The disc image or extracted folder.
    var disc: String = ""
    /// The release tag the source is at, once fetched.
    var tag: String?
    var installs: [Install] = []
    /// The old in-repository installer's settings were imported.
    var migrated = false

    mutating func record(_ install: Install) {
        if let index = installs.firstIndex(where: { $0.id == install.id }) {
            installs[index] = install
        } else {
            installs.append(install)
        }
    }
}

extension JSONEncoder {
    static var installer: JSONEncoder {
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        return encoder
    }
}

extension JSONDecoder {
    static var installer: JSONDecoder {
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        return decoder
    }
}
