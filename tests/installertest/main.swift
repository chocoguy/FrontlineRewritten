// frontline_installertest: the installer's logic without its UI. Prints each failed check and exits
// non-zero if any did.

import Foundation

var failures = 0

func check(_ condition: Bool, _ what: String, line: Int = #line) {
    if !condition {
        failures += 1
        print("FAIL (line \(line)): \(what)")
    }
}

// Versions: tags with or without v, pre-releases before their release, numeric parts compared as numbers.
check(SemVer("v0.3.0") == SemVer("0.3.0"), "v prefix")
check(SemVer("0.3") == nil && SemVer("0.3.0-") == nil && SemVer("x.1.0") == nil, "bad versions rejected")
check(SemVer("0.3.0-rc1")! < SemVer("0.3.0")!, "pre-release before release")
check(SemVer("0.3.0-rc.2")! < SemVer("0.3.0-rc.10")!, "numeric pre-release parts")
check(SemVer("0.2.9")! < SemVer("0.3.0")!, "minor beats patch")
check(SemVer("0.10.0")! > SemVer("0.9.0")!, "numbers, not text")
check(SemVer("1.0.0-alpha")! < SemVer("1.0.0-beta")!, "words compared as text")
check(SemVer("1.0.0-1")! < SemVer("1.0.0-alpha")!, "numbers before words")
check(SemVer("0.3.0-rc1")!.description == "0.3.0-rc1", "description")

// Releases: drafts never, pre-releases only when asked, non-version tags skipped, newest first.
let json = """
[
  {"tag_name": "v0.2.0", "name": "Second", "body": "Notes **two**", "prerelease": false, "draft": false,
   "published_at": "2026-12-01T00:00:00Z", "html_url": "https://example.invalid/2"},
  {"tag_name": "v0.3.0-rc1", "name": "", "body": null, "prerelease": true, "draft": false},
  {"tag_name": "v0.4.0", "name": "Draft", "body": "", "prerelease": false, "draft": true},
  {"tag_name": "nightly", "name": "Nightly", "body": "", "prerelease": false, "draft": false},
  {"tag_name": "v0.1.0", "name": "First", "body": "", "prerelease": false, "draft": false}
]
"""
do {
    let all = try Releases.parse(Data(json.utf8))
    check(all.count == 5, "all parsed")
    check(Releases.installable(all, prereleases: false).map(\.tag) == ["v0.2.0", "v0.1.0"], "installable")
    check(Releases.installable(all, prereleases: true).map(\.tag) == ["v0.3.0-rc1", "v0.2.0", "v0.1.0"], "with pre-releases")
    check(all[1].title == "v0.3.0-rc1", "empty name falls back to the tag")
    check(all[0].notes == "Notes **two**", "notes")
} catch {
    check(false, "releases parse: \(error)")
}

// Saved state: round trip, and recording replaces an install with the same ID.
let folder = FileManager.default.temporaryDirectory.appendingPathComponent("installertest-\(UUID())")
let workspace = Workspace(root: folder)
var state = workspace.load()
check(state.installs.isEmpty && state.disc.isEmpty, "fresh state")
state.disc = "/Discs/Frontline.iso"
state.tag = "v0.2.0"
state.record(Install(platform: .macos, deviceId: nil, deviceName: "This Mac", version: "0.1.0"))
state.record(Install(platform: .tvos, deviceId: "ABC", deviceName: "Guest Room", version: "0.1.0",
                     expires: Date(timeIntervalSince1970: 1_900_000_000), pending: true))
state.record(Install(platform: .macos, deviceId: nil, deviceName: "This Mac", version: "0.2.0"))
workspace.save(state)
let loaded = workspace.load()
check(loaded.installs.count == 2, "record replaces by ID")
check(loaded.installs.first { $0.platform == .macos }?.version == "0.2.0", "replaced version")
check(loaded.installs.first { $0.deviceId == "ABC" }?.pending == true, "pending kept")
check(loaded.installs.first { $0.deviceId == "ABC" }?.expires == Date(timeIntervalSince1970: 1_900_000_000), "date kept")
check(loaded.tag == "v0.2.0" && loaded.disc == "/Discs/Frontline.iso", "fields kept")
try? FileManager.default.removeItem(at: folder)

// The build script's events.
check(PortEvent(line: #"{"event": "stage", "id": "compile", "label": "Compiling the game"}"#)
      == .stage(id: "compile", label: "Compiling the game"), "stage")
check(PortEvent(line: #"{"event": "progress", "fraction": 0.5}"#) == .progress(0.5), "progress")
check(PortEvent(line: #"{"event": "error", "code": "x", "message": "M", "fix": "F"}"#)
      == .error(code: "x", message: "M", fix: "F"), "error")
check(PortEvent(line: #"{"event": "installed", "present": true, "version": "0.2.0", "build": "200"}"#)
      == .installed(present: true, version: "0.2.0", build: "200"), "installed")
if case let .result(app, version, commit, expires)? = PortEvent(line:
    #"{"event": "result", "app": "/A.app", "version": "0.2.0", "commit": "abc", "expires": "2027-11-21T10:00:00+00:00"}"#) {
    check(app == "/A.app" && version == "0.2.0" && commit == "abc", "result fields")
    check(expires == ISO8601DateFormatter().date(from: "2027-11-21T10:00:00Z"), "result expiry")
} else {
    check(false, "result")
}
check(PortEvent(line: "not json") == nil, "plain lines aren't events")

print(failures == 0 ? "installertest: all passed" : "installertest: \(failures) failed")
exit(failures == 0 ? 0 : 1)
