import Foundation

/// What the Setup screen checks before anything is built, and what each missing piece's button does.
struct DoctorItem: Identifiable, Sendable {
    enum Status: Sendable {
        case ok, missing, optional, checking
    }

    enum Action: Sendable, Hashable {
        case installXcode, openXcode, installCMake, downloadPlatform(Platform), openAccounts
    }

    let id: String
    let title: String
    var status: Status
    var detail: String
    var action: Action?
    var actionTitle: String?
}

enum Doctor {
    static func check(_ workspace: Workspace) async -> [DoctorItem] {
        var items: [DoctorItem] = []

        // Xcode, selected and set up (its first launch installs the parts builds need).
        let selected = await Shell.output("/usr/bin/xcode-select", ["-p"])?.trimmingCharacters(in: .whitespacesAndNewlines)
        let xcodeVersion = await Shell.output("/usr/bin/xcodebuild", ["-version"])?
            .split(separator: "\n").first.map(String.init)
        let major = xcodeVersion.flatMap { $0.split(separator: " ").last }.flatMap { Int($0.split(separator: ".")[0]) } ?? 0
        if selected == nil || selected!.contains("CommandLineTools") || xcodeVersion == nil {
            items.append(DoctorItem(id: "xcode", title: "Xcode 27", status: .missing,
                                    detail: "Install Xcode from the App Store. It's free, and about 10 GB.",
                                    action: .installXcode, actionTitle: "Open the App Store"))
        } else if major < 27 {
            items.append(DoctorItem(id: "xcode", title: "Xcode 27", status: .missing,
                                    detail: "\(xcodeVersion!) is installed; the game needs Xcode 27 or later. Update it in the App Store.",
                                    action: .installXcode, actionTitle: "Open the App Store"))
        } else if await Shell.output("/usr/bin/xcodebuild", ["-checkFirstLaunchStatus"]) == nil {
            items.append(DoctorItem(id: "xcode", title: "Xcode 27", status: .missing,
                                    detail: "Open Xcode once and let it install its components (it asks for your password).",
                                    action: .openXcode, actionTitle: "Open Xcode"))
        } else {
            items.append(DoctorItem(id: "xcode", title: "Xcode 27", status: .ok, detail: xcodeVersion!))
        }
        let xcodeReady = items.last?.status == .ok

        // Python 3 comes with Xcode; the build script runs on it.
        let python = await Shell.output("/usr/bin/python3", ["--version"])?.trimmingCharacters(in: .whitespacesAndNewlines)
        let pythonMinor = python.flatMap { $0.split(separator: " ").last?.split(separator: ".").dropFirst().first }
            .flatMap { Int($0) } ?? 0
        items.append(DoctorItem(id: "python", title: "Python 3", status: python != nil && pythonMinor >= 9 ? .ok : .missing,
                                detail: python ?? "Comes with Xcode: install Xcode first."))

        // CMake: the installer's own copy, or one already on this Mac.
        let path = Shell.path(tools: workspace.tools)
        let cmake = await Shell.output("/usr/bin/env", ["cmake", "--version"], path: path)?
            .split(separator: "\n").first.map(String.init)
        let cmakeVersion = cmake.flatMap { $0.split(separator: " ").last }.flatMap { SemVer(String($0)) }
        if let cmakeVersion, cmakeVersion >= SemVer("3.31.0")! {
            items.append(DoctorItem(id: "cmake", title: "CMake", status: .ok,
                                    detail: CMake.installed(workspace) ? "CMake \(CMake.version), downloaded by the installer" : cmake!))
        } else {
            items.append(DoctorItem(id: "cmake", title: "CMake", status: .missing,
                                    detail: "The build tool the game uses. The installer downloads it from Kitware (about 80 MB).",
                                    action: .installCMake, actionTitle: "Download"))
        }

        // The iPhone and Apple TV platforms, for devices only.
        let sdks = xcodeReady ? await Shell.output("/usr/bin/xcodebuild", ["-showsdks", "-json"]) ?? "" : ""
        for platform in [Platform.ios, .tvos] {
            let has = sdks.contains("\"\(platform.sdk!)\"")
            items.append(DoctorItem(id: "platform-\(platform.rawValue)", title: "\(platform.downloadName!) support in Xcode",
                                    status: has ? .ok : .optional,
                                    detail: has ? "Installed" : "Needed only to install on an \(platform == .ios ? "iPhone or iPad" : "Apple TV").",
                                    action: has || !xcodeReady ? nil : .downloadPlatform(platform),
                                    actionTitle: has ? nil : "Install"))
        }

        // A paid developer team signed in to Xcode, for devices only.
        let teams = await xcodeTeams()
        let paid = teams.filter { !$0.free }
        items.append(DoctorItem(
            id: "account", title: "Apple Developer team", status: paid.isEmpty ? .optional : .ok,
            detail: paid.first.map { "\($0.name) (\($0.id))" }
                ?? (teams.isEmpty ? "Needed only for an iPhone, iPad or Apple TV: sign in to Xcode with a paid Apple Developer account."
                                  : "Only a free team is signed in. Devices need a paid Apple Developer membership: a free team's installs stop after 7 days."),
            action: paid.isEmpty && xcodeReady ? .openAccounts : nil, actionTitle: paid.isEmpty ? "Open Xcode Settings" : nil))
        return items
    }

    struct Team: Sendable {
        let id: String
        let name: String
        let free: Bool
    }

    /// The teams of the Apple IDs signed in to Xcode (as tools/tvos/port's xcode_teams reads them).
    static func xcodeTeams() async -> [Team] {
        guard let text = await Shell.output("/usr/bin/defaults", ["export", "com.apple.dt.Xcode", "-"]),
              let plist = try? PropertyListSerialization.propertyList(from: Data(text.utf8), format: nil) as? [String: Any],
              let byAccount = plist["IDEProvisioningTeamByIdentifier"] as? [String: Any] else {
            return []
        }
        var teams: [Team] = []
        for case let account as [[String: Any]] in byAccount.values {
            for team in account {
                guard let id = team["teamID"] as? String, !teams.contains(where: { $0.id == id }) else {
                    continue
                }
                teams.append(Team(id: id, name: team["teamName"] as? String ?? "",
                                  free: (team["isFreeProvisioningTeam"] as? Bool) ?? false))
            }
        }
        return teams
    }
}
