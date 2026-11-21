import Foundation

/// The game's source: a git checkout of FrontlineRewritten at a release tag, with the decomp
/// submodule. Both repositories are public, so nothing asks for an account. Blobs are fetched as
/// needed (a partial clone), which keeps the first download small.
enum Source {
    static let url = "https://github.com/\(Releases.repository).git"
    static let git = "/usr/bin/git"

    static func exists(_ workspace: Workspace) -> Bool {
        FileManager.default.fileExists(atPath: workspace.source.appendingPathComponent(".git").path)
    }

    /// Brings the checkout up to date with GitHub: cloned the first time (without checking anything
    /// out), then the tags fetched.
    static func fetchCommands(_ workspace: Workspace) -> [(label: String, arguments: [String])] {
        let folder = workspace.source.path
        var steps: [(String, [String])] = []
        if !exists(workspace) {
            steps.append(("Downloading the source",
                          ["clone", "--progress", "--filter=blob:none", "--no-checkout", url, folder]))
        }
        steps.append(("Looking for releases", ["-C", folder, "fetch", "--progress", "--force", "--tags", "--prune", "origin"]))
        return steps
    }

    /// Checks `tag` out (whatever was changed there goes) and brings the submodule to the tag's commit.
    static func checkoutCommands(_ tag: String, _ workspace: Workspace) -> [(label: String, arguments: [String])] {
        let folder = workspace.source.path
        return [
            ("Switching to \(tag)", ["-C", folder, "-c", "advice.detachedHead=false", "checkout", "--force", "tags/\(tag)"]),
            ("Downloading the game code", ["-C", folder, "submodule", "sync", "--recursive"]),
            ("Downloading the game code", ["-C", folder, "submodule", "update", "--init", "--recursive", "--force",
                                           "--progress", "--filter=blob:none"]),
        ]
    }

    /// The installer version `tag` needs (its port.json's minAppVersion), read from the fetched tag
    /// before it's checked out. Nil if it doesn't say.
    static func minimumInstaller(for tag: String, in workspace: Workspace) async -> SemVer? {
        guard let text = await Shell.output(git, ["-C", workspace.source.path, "show", "tags/\(tag):port.json"]),
              let data = text.data(using: .utf8),
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let minimum = json["minAppVersion"] as? String else {
            return nil
        }
        return SemVer(minimum.split(separator: ".").count == 2 ? minimum + ".0" : minimum)
    }

    /// git's progress lines ("Receiving objects:  45% (…)") as a fraction.
    static func progress(in line: String) -> Double? {
        guard line.contains("Receiving objects") || line.contains("Resolving deltas") || line.contains("Updating files"),
              let percent = line.range(of: #"(\d+)%"#, options: .regularExpression) else {
            return nil
        }
        return Double(line[percent].dropLast()).map { $0 / 100 }
    }
}

/// Kitware's CMake, downloaded once into the workspace, so a player needs neither Homebrew nor
/// CMake.app. Pinned, and checked against its SHA-256 (Kitware's cmake-4.5.1-SHA-256.txt).
enum CMake {
    static let version = "4.5.1"
    static let archive = "cmake-\(version)-macos-universal.tar.gz"
    static let sha256 = "e1fb288543e4eb2a410e385ecebc64323bda51f0205782f2b094ccd5d572ca2d"
    static let url = URL(string: "https://github.com/Kitware/CMake/releases/download/v\(version)/\(archive)")!
    /// Inside the workspace's tools folder.
    static let binFolder = "cmake-\(version)-macos-universal/CMake.app/Contents/bin"

    static func installed(_ workspace: Workspace) -> Bool {
        FileManager.default.isExecutableFile(atPath: workspace.tools.appendingPathComponent(binFolder + "/cmake").path)
    }
}

/// A paired device devicectl can install on.
struct Device: Identifiable, Hashable, Sendable {
    let id: String
    let name: String
    let model: String
    let platform: Platform

    var title: String { model.isEmpty ? name : "\(name) (\(model))" }

    /// The paired, physical devices of `platform`.
    static func list(_ platform: Platform) async -> [Device] {
        guard let wanted = platform.devicePlatform else {
            return []
        }
        let file = FileManager.default.temporaryDirectory.appendingPathComponent("frontline-devices-\(UUID()).json")
        defer { try? FileManager.default.removeItem(at: file) }
        _ = await Shell.output("/usr/bin/xcrun", ["devicectl", "list", "devices", "--json-output", file.path])
        guard let data = try? Data(contentsOf: file),
              let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let result = root["result"] as? [String: Any],
              let entries = result["devices"] as? [[String: Any]] else {
            return []
        }
        return entries.compactMap { entry -> Device? in
            let hardware = entry["hardwareProperties"] as? [String: Any] ?? [:]
            let properties = entry["deviceProperties"] as? [String: Any] ?? [:]
            guard hardware["platform"] as? String == wanted,
                  (hardware["reality"] as? String ?? "physical") == "physical",
                  let id = entry["identifier"] as? String else {
                return nil
            }
            return Device(id: id, name: properties["name"] as? String ?? id,
                          model: hardware["marketingName"] as? String ?? "", platform: platform)
        }
        .sorted { $0.name < $1.name }
    }
}
