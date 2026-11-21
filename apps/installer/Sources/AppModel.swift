import AppKit
import CryptoKit
import Foundation

/// A failed step: what went wrong, and what to do about it.
struct Problem: Error, Sendable {
    let message: String
    let fix: String
}

/// The job running now (or the last one): what the Activity screen shows.
struct Activity {
    var title = ""
    var running = false
    var stage = ""
    var progress: Double?
    var log: [String] = []
    var notes: [String] = []
    var problem: Problem?
    var finished: String?
    var cancelled = false
}

/// The installer: its saved state, the releases on GitHub, the setup checks, and every job (fetching
/// the source, building, installing, updating), run one at a time through tools/tvos/port from the
/// release being built.
@MainActor
final class AppModel: ObservableObject {
    enum Screen: Hashable {
        case setup, game, updates, activity
    }

    let workspace = Workspace()
    @Published var screen: Screen = .game
    @Published var state: InstallerState {
        didSet { workspace.save(state) }
    }
    @Published var releases: [Release] = []
    @Published var releaseError: String?
    @Published var doctor: [DoctorItem] = []
    @Published var checkingSetup = false
    @Published var activity = Activity()
    /// What each install reports now (port installed), by install ID: its version, "Not installed", or
    /// "Not reachable".
    @Published var reported: [String: String] = [:]
    /// A release needs this installer version or later (its port.json's minAppVersion).
    @Published var needsInstaller: SemVer?

    private let shell = Shell()
    private var log: FileHandle?

    init() {
        state = workspace.load()
    }

    // MARK: What the screens ask

    var installerVersion: SemVer {
        SemVer(Bundle.main.object(forInfoDictionaryKey: "FrontlineVersion") as? String ?? "") ?? SemVer("0.0.1")!
    }

    /// Release candidates too (the hidden includePrereleases default, for testing them).
    var prereleases: Bool { UserDefaults.standard.bool(forKey: "includePrereleases") }

    var installable: [Release] { Releases.installable(releases, prereleases: prereleases) }
    var latest: Release? { installable.first }

    /// A newer release than the one installed.
    var updateAvailable: Bool {
        guard let latest = latest?.version, !state.installs.isEmpty else {
            return false
        }
        return state.tag.flatMap(SemVer.init).map { latest > $0 } ?? true
    }

    var setupReady: Bool { !doctor.isEmpty && !doctor.contains { $0.status == .missing } }
    var setupIssues: Int { doctor.filter { $0.status == .missing }.count }
    var pending: [Install] { state.installs.filter(\.pending) }
    var canRun: Bool { !activity.running && setupReady && !state.disc.isEmpty && (state.tag ?? latest?.tag) != nil }

    /// Why nothing can be installed yet, if so.
    var blocker: String? {
        if activity.running { return "Something is already running." }
        if !setupReady { return "Finish Setup first." }
        if state.disc.isEmpty { return "Choose your disc first." }
        if state.tag == nil && latest == nil { return releaseError ?? "No release found on GitHub yet." }
        return nil
    }

    // MARK: Start

    func start() {
        migrate()
        Task {
            await refresh()
            if !setupReady {
                screen = .setup
            } else if updateAvailable {
                screen = .updates
            }
        }
    }

    func refresh() async {
        await checkSetup()
        await loadReleases()
        await checkInstalled()
    }

    func checkSetup() async {
        checkingSetup = true
        doctor = await Doctor.check(workspace)
        checkingSetup = false
    }

    func loadReleases() async {
        do {
            releases = try await Releases.fetch(cacheFile: workspace.releasesCache)
            releaseError = nil
        } catch {
            releaseError = "Couldn't reach GitHub: \(error.localizedDescription)"
        }
    }

    /// Asks each install what's there now (tools/tvos/port installed), so the list stays honest when
    /// an app was deleted or installed some other way. Quietly: devices that are off just say so.
    func checkInstalled() async {
        guard Source.exists(workspace) else {
            return
        }
        for install in state.installs {
            var arguments = ["-I", workspace.port.path, "installed", "--platform", install.platform.rawValue,
                             "--work-dir", workspace.work.path, "--json"]
            if let device = install.deviceId {
                arguments += ["--device", device]
            }
            let output = await Shell.output("/usr/bin/python3", arguments, path: Shell.path(tools: workspace.tools))
            let event = output?.split(separator: "\n").compactMap { PortEvent(line: String($0)) }.last
            switch event {
            case .installed(true, let version, _):
                reported[install.id] = version ?? "Installed"
            case .installed(false, _, _):
                reported[install.id] = "Not installed"
            default:
                reported[install.id] = "Not reachable"
            }
        }
    }

    /// Imports the disc from the installer that used to live in the repository, and removes the copy
    /// it built of itself.
    private func migrate() {
        guard !state.migrated else {
            return
        }
        if state.disc.isEmpty, let disc = UserDefaults(suiteName: "io.github.chocoguy.FrontlineInstaller")?.string(forKey: "disc") {
            state.disc = disc
        }
        let old = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Frontline Installer", isDirectory: true)
        try? FileManager.default.removeItem(at: old)
        state.migrated = true
    }

    // MARK: Jobs

    /// Installs the game on a new target, at the release already built (or the latest).
    func add(_ platform: Platform, device: Device?) {
        guard let tag = state.tag ?? latest?.tag else {
            return
        }
        let install = Install(platform: platform, deviceId: device?.id, deviceName: device?.name ?? "This Mac")
        run("Installing on \(install.deviceName)") {
            try await self.bringSource(to: tag)
            try await self.build(tag: tag, targets: [install], launch: true)
        }
    }

    func reinstall(_ install: Install) {
        guard let tag = state.tag ?? latest?.tag else {
            return
        }
        run("Reinstalling on \(install.deviceName)") {
            try await self.bringSource(to: tag)
            try await self.build(tag: tag, targets: [install], launch: true)
        }
    }

    /// The latest release, everywhere the game is installed.
    func updateAll() {
        guard let latest else {
            return
        }
        install(latest, title: "Updating to \(latest.tag)")
    }

    /// `release` on every install: an update, or going back to an earlier version.
    func install(_ release: Release, title: String? = nil) {
        let targets = state.installs
        run(title ?? "Installing \(release.tag)") {
            try await self.bringSource(to: release.tag)
            try await self.build(tag: release.tag, targets: targets, launch: false)
        }
    }

    func retryPending() {
        guard let tag = state.tag else {
            return
        }
        let targets = pending
        run("Finishing the update") {
            try await self.bringSource(to: tag)
            try await self.build(tag: tag, targets: targets, launch: false)
        }
    }

    func forget(_ install: Install) {
        Reminders.cancel(for: install)
        state.installs.removeAll { $0.id == install.id }
        reported[install.id] = nil
    }

    var canUndoMac: Bool { FileManager.default.fileExists(atPath: workspace.previousMacApp.path) && !activity.running }

    /// Puts back the Mac app from before the last update.
    func undoMacUpdate() {
        run("Undoing the Mac update") {
            self.step("Putting the previous version back")
            let installed = try self.installedMacApp()
            try await self.tool("/usr/bin/osascript", ["-e", "if application \"Frontline Rewritten\" is running then tell application \"Frontline Rewritten\" to quit"])
            try? FileManager.default.removeItem(at: installed)
            try await self.tool("/usr/bin/ditto", [self.workspace.previousMacApp.path, installed.path])
            try? FileManager.default.removeItem(at: self.workspace.previousMacApp)
            let info = NSDictionary(contentsOf: installed.appendingPathComponent("Contents/Info.plist"))
            if var mac = self.state.installs.first(where: { $0.platform == .macos }) {
                mac.version = info?["FrontlineVersion"] as? String ?? info?["CFBundleShortVersionString"] as? String
                mac.commit = info?["FrontlineCommit"] as? String
                self.state.record(mac)
            }
            self.activity.finished = "The previous Mac version is back."
        }
    }

    func downloadCMake() {
        run("Downloading CMake \(CMake.version)") {
            self.step("Downloading CMake from Kitware (about 80 MB)")
            let (file, response) = try await URLSession.shared.download(from: CMake.url)
            guard (response as? HTTPURLResponse)?.statusCode == 200 else {
                throw Problem(message: "Couldn't download CMake.", fix: "Check the internet connection and try again.")
            }
            self.step("Checking it")
            let digest = SHA256.hash(data: try Data(contentsOf: file)).map { String(format: "%02x", $0) }.joined()
            guard digest == CMake.sha256 else {
                throw Problem(message: "The CMake download didn't match its checksum.", fix: "Try again; if it keeps failing, report it.")
            }
            self.step("Unpacking it")
            try FileManager.default.createDirectory(at: self.workspace.tools, withIntermediateDirectories: true)
            try await self.tool("/usr/bin/tar", ["-xzf", file.path, "-C", self.workspace.tools.path])
            try? FileManager.default.removeItem(at: file)
            self.activity.finished = "CMake \(CMake.version) is ready."
            await self.checkSetup()
        }
    }

    func downloadPlatform(_ platform: Platform) {
        guard let name = platform.downloadName else {
            return
        }
        run("Installing \(name) support in Xcode") {
            self.step("Downloading \(name) support (a few GB)")
            try await self.tool("/usr/bin/xcodebuild", ["-downloadPlatform", name]) { line in
                if let percent = line.range(of: #"(\d+(\.\d+)?)%"#, options: .regularExpression) {
                    self.activity.progress = Double(line[percent].dropLast()).map { $0 / 100 }
                }
            }
            self.activity.finished = "\(name) support is installed."
            await self.checkSetup()
        }
    }

    func perform(_ action: DoctorItem.Action) {
        switch action {
        case .installXcode:
            NSWorkspace.shared.open(URL(string: "macappstore://apps.apple.com/app/id497799835")!)
        case .openXcode, .openAccounts:
            if let xcode = NSWorkspace.shared.urlForApplication(withBundleIdentifier: "com.apple.dt.Xcode") {
                NSWorkspace.shared.openApplication(at: xcode, configuration: NSWorkspace.OpenConfiguration())
            }
        case .installCMake:
            downloadCMake()
        case .downloadPlatform(let platform):
            downloadPlatform(platform)
        }
    }

    func cancel() {
        guard activity.running else {
            return
        }
        activity.cancelled = true
        activity.stage = "Cancelling…"
        shell.cancel()
    }

    /// Everything a bug report needs, on the clipboard.
    func copyDiagnostics() {
        var lines = ["Frontline Installer \(installerVersion)", "Source: \(state.tag ?? "none")", "Disc: \(state.disc)", ""]
        lines += doctor.map { "\($0.status == .ok ? "ok" : "--") \($0.title): \($0.detail)" }
        lines += [""] + state.installs.map { "\($0.deviceName) (\($0.platform.rawValue)): \($0.version ?? "?") \($0.pending ? "pending" : "")" }
        lines += ["", "Last job: \(activity.title)"] + activity.log.suffix(300)
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(lines.joined(separator: "\n"), forType: .string)
    }

    // MARK: Running a job

    private func run(_ title: String, _ body: @escaping @MainActor () async throws -> Void) {
        guard !activity.running else {
            return
        }
        activity = Activity(title: title, running: true)
        screen = .activity
        openLog(title)
        Task {
            do {
                try await body()
                if activity.finished == nil {
                    activity.finished = "Done."
                }
            } catch let problem as Problem {
                activity.problem = activity.cancelled ? nil : problem
            } catch {
                activity.problem = activity.cancelled ? nil : Problem(message: error.localizedDescription, fix: "")
            }
            activity.running = false
            activity.stage = ""
            activity.progress = nil
            try? log?.close()
            log = nil
            if activity.problem != nil || activity.finished != nil {
                NSApp.requestUserAttention(.informationalRequest)
            }
        }
    }

    private func openLog(_ title: String) {
        try? FileManager.default.createDirectory(at: workspace.logs, withIntermediateDirectories: true)
        let stamp = ISO8601DateFormatter().string(from: .now).replacingOccurrences(of: ":", with: ".")
        let file = workspace.logs.appendingPathComponent("\(stamp) \(title).log")
        FileManager.default.createFile(atPath: file.path, contents: nil)
        log = try? FileHandle(forWritingTo: file)
    }

    private func append(_ line: String) {
        activity.log.append(line)
        if activity.log.count > 4000 {
            activity.log.removeFirst(1000)
        }
        log?.write(Data((line + "\n").utf8))
    }

    private func step(_ label: String) {
        activity.stage = label
        activity.progress = nil
        append("==> " + label)
    }

    /// Runs a tool as part of the job; throws if it fails (or the job was cancelled).
    private func tool(_ executable: String, _ arguments: [String], in folder: URL? = nil,
                      onLine: (@MainActor (String) -> Void)? = nil) async throws {
        let status = await shell.run(executable, arguments, in: folder, path: Shell.path(tools: workspace.tools)) { line, _ in
            self.append(line)
            onLine?(line)
        }
        if activity.cancelled {
            throw CancellationError()
        }
        if status != 0 {
            throw Problem(message: "\(URL(fileURLWithPath: executable).lastPathComponent) stopped (status \(status)).",
                          fix: "The Details below say why. Copy Diagnostics to report it.")
        }
    }

    /// Runs tools/tvos/port from the checked-out release, showing its events. Its result, if it made one.
    @discardableResult
    private func port(_ arguments: [String]) async throws -> PortEvent? {
        var result: PortEvent?
        var failure: Problem?
        let all = ["-I", workspace.port.path] + arguments + ["--work-dir", workspace.work.path, "--json"]
        let status = await shell.run("/usr/bin/python3", all, in: workspace.source, path: Shell.path(tools: workspace.tools)) { line, isError in
            guard !isError, let event = PortEvent(line: line) else {
                self.append(line)
                return
            }
            switch event {
            case .stage(_, let label):
                self.step(label)
            case .progress(let fraction):
                self.activity.progress = fraction
            case .error(_, let message, let fix):
                failure = Problem(message: message, fix: fix)
                self.append("error: " + message)
            case .warning(let message), .note(let message):
                self.activity.notes.append(message)
                self.append(message)
            case .result:
                result = event
            default:
                break
            }
        }
        if activity.cancelled {
            throw CancellationError()
        }
        if status != 0 {
            throw failure ?? Problem(message: "The build script stopped (status \(status)).",
                                     fix: "The Details below say why. Copy Diagnostics to report it.")
        }
        return result
    }

    /// Brings the source to `tag`: fetches it, checks the installer is new enough for it (its
    /// port.json), then checks it and its submodule out. On a failure it goes back to the tag it was at.
    private func bringSource(to tag: String) async throws {
        let previous = state.tag
        for (label, arguments) in Source.fetchCommands(workspace) {
            step(label)
            try await tool(Source.git, arguments) { line in
                if let fraction = Source.progress(in: line) {
                    self.activity.progress = fraction
                }
            }
        }
        if let minimum = await Source.minimumInstaller(for: tag, in: workspace), minimum > installerVersion {
            needsInstaller = minimum
            throw Problem(message: "\(tag) needs Frontline Installer \(minimum) or later (this is \(installerVersion)).",
                          fix: "Choose Frontline Installer → Check for Updates…, then try again.")
        }
        needsInstaller = nil
        guard previous != tag || !FileManager.default.fileExists(atPath: workspace.port.path) else {
            return
        }
        do {
            for (label, arguments) in Source.checkoutCommands(tag, workspace) {
                step(label)
                try await tool(Source.git, arguments) { line in
                    if let fraction = Source.progress(in: line) {
                        self.activity.progress = fraction
                    }
                }
            }
            state.tag = tag
        } catch {
            if let previous, !activity.cancelled {
                append("Going back to \(previous)")
                for (_, arguments) in Source.checkoutCommands(previous, workspace) {
                    _ = await shell.run(Source.git, arguments, path: nil) { line, _ in self.append(line) }
                }
            }
            throw error
        }
    }

    /// Builds `tag` once per platform, then installs it on each target. A device that can't be
    /// reached is marked pending, and the others go on.
    private func build(tag: String, targets: [Install], launch: Bool) async throws {
        var missed: [String] = []
        for platform in Platform.allCases {
            let mine = targets.filter { $0.platform == platform }
            guard !mine.isEmpty else {
                continue
            }
            var buildArguments = ["build", "--platform", platform.rawValue, "--disc", state.disc]
            if let device = mine.first?.deviceId {
                buildArguments += ["--device", device]
            }
            let built = try await port(buildArguments)
            var version: String?
            var commit: String?
            var expires: Date?
            if case let .result(_, v, c, e) = built {
                version = v ?? tag.trimmingCharacters(in: CharacterSet(charactersIn: "v"))
                commit = c
                expires = e
            }
            for target in mine {
                var install = target
                if platform == .macos {
                    try await keepPreviousMacApp()
                }
                var arguments = ["install", "--platform", platform.rawValue]
                if let device = target.deviceId {
                    arguments += ["--device", device]
                }
                if launch {
                    arguments.append("--launch")
                }
                do {
                    try await port(arguments)
                } catch let problem as Problem where platform != .macos && !activity.cancelled {
                    install.pending = true
                    state.record(install)
                    missed.append(target.deviceName)
                    activity.notes.append("\(target.deviceName): \(problem.message) It's marked to finish later.")
                    continue
                }
                install.version = version
                install.commit = commit
                install.installedAt = .now
                install.expires = platform == .macos ? nil : expires
                install.pending = false
                state.record(install)
                reported[install.id] = version
                await Reminders.schedule(for: install)
            }
        }
        let names = targets.map(\.deviceName).filter { !missed.contains($0) }
        if names.isEmpty && !missed.isEmpty {
            throw Problem(message: "Couldn't reach \(missed.joined(separator: ", ")).",
                          fix: "Wake it up, check it's paired in Xcode's Devices window, then press Finish the Update.")
        }
        activity.finished = "Frontline \(tag) is on " + names.joined(separator: ", ") + "." +
            (missed.isEmpty ? "" : " \(missed.joined(separator: ", ")) will be finished later.")
    }

    /// Where the Mac app is installed (Applications, or the user's own Applications).
    private func installedMacApp() throws -> URL {
        for folder in [URL(fileURLWithPath: "/Applications"), FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Applications")] {
            let app = folder.appendingPathComponent("Frontline Rewritten.app")
            if FileManager.default.fileExists(atPath: app.path) {
                return app
            }
        }
        throw Problem(message: "Frontline Rewritten isn't in Applications.", fix: "Install it on this Mac again.")
    }

    /// Copies the Mac app as it is into work/previous before an install replaces it (Undo).
    private func keepPreviousMacApp() async throws {
        guard let installed = try? installedMacApp() else {
            return
        }
        step("Keeping the current Mac version, for Undo")
        try? FileManager.default.removeItem(at: workspace.previousMacApp)
        try FileManager.default.createDirectory(at: workspace.previousMacApp.deletingLastPathComponent(),
                                                withIntermediateDirectories: true)
        try await tool("/usr/bin/ditto", [installed.path, workspace.previousMacApp.path])
    }
}
