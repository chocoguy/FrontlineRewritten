import CloudKit
import CryptoKit
import Foundation

/// The memory card's saves in iCloud (CloudKit's private database), so they
/// outlive the app's local data, which tvOS may clear, and follow the player
/// to another Apple TV.
///
/// One `MemoryCardFile` record per .gci file, named after it, in a zone of its
/// own ("MemoryCard"): the file (an asset), when it was written, and its
/// SHA-256. Before the game starts, `prepare` brings the card folder and
/// iCloud up to date with each other. While the game runs, each save it
/// writes or deletes goes up (`changed`); the folder isn't touched then, since
/// the game owns it.
///
/// A manifest next to the folder remembers each file's SHA-256 at the last
/// sync, which tells a change here from one made elsewhere:
/// - changed on one side: that side's version wins;
/// - changed on both: the newer wins, and the other is kept in the
///   "<folder> Conflicts" folder, which the card never reads;
/// - deleted on one side: deleted on the other, except that a save deleted
///   elsewhere is moved to the conflicts folder here, rather than lost.
/// Whatever fails (no network, no iCloud account) is put right at the next
/// launch's `prepare`.
actor SaveSync {
    private static let recordType = "MemoryCardFile"

    private let folder: URL
    private let conflicts: URL
    private let manifestURL: URL
    private let container: CKContainer
    private let zone = CKRecordZone.ID(zoneName: "MemoryCard", ownerName: CKCurrentUserDefaultName)

    /// File name -> SHA-256 at the last sync.
    private var manifest: [String: String]
    /// The game has the folder: only uploads from here on.
    private var gameStarted = false
    private var available = false
    /// Files changed while the game runs, to send up in turn.
    private var pending: [String] = []
    private var sending = false

    /// `identifier`: the iCloud container, as in the app's entitlements.
    init(container identifier: String, folder: URL) {
        self.folder = folder
        conflicts = folder.deletingLastPathComponent().appendingPathComponent(folder.lastPathComponent + " Conflicts")
        manifestURL = folder.deletingLastPathComponent().appendingPathComponent(folder.lastPathComponent + " Sync.json")
        container = CKContainer(identifier: identifier)
        manifest = (try? Data(contentsOf: manifestURL))
            .flatMap { try? JSONDecoder().decode([String: String].self, from: $0) } ?? [:]
    }

    /// Brings the folder and iCloud up to date with each other.
    func prepare() async {
        do {
            let status = try await container.accountStatus()
            guard status == .available else {
                Self.log("no iCloud account (status \(status.rawValue)); the saves stay on this Apple TV")
                return
            }
            _ = try await database.modifyRecordZones(saving: [CKRecordZone(zoneID: zone)], deleting: [])
            available = true
            let remote = try await fetchAll()
            try await reconcile(remote)
            Self.log("up to date (\(manifest.count) saves)")
        } catch {
            Self.log("sync failed: \(error.localizedDescription)")
        }
        saveManifest()
    }

    /// From here on the game owns the folder: nothing more is written to it.
    func beginGame() {
        gameStarted = true
    }

    /// The game wrote or deleted the .gci at `path`: sends that up.
    func changed(path: String) async {
        let url = URL(fileURLWithPath: path)
        guard url.deletingLastPathComponent().standardizedFileURL == folder.standardizedFileURL else {
            return
        }
        let name = url.lastPathComponent
        if !pending.contains(name) {
            pending.append(name)
        }
        guard !sending else {
            return
        }
        sending = true
        while !pending.isEmpty {
            let next = pending.removeFirst()
            await send(next)
        }
        sending = false
        saveManifest()
    }

    // MARK: Syncing

    private var database: CKDatabase { container.privateCloudDatabase }

    private func fetchAll() async throws -> [String: CKRecord] {
        var records: [String: CKRecord] = [:]
        var token: CKServerChangeToken?
        var more = true
        while more {
            let changes = try await database.recordZoneChanges(inZoneWith: zone, since: token)
            for (id, result) in changes.modificationResultsByID {
                if case .success(let modification) = result {
                    records[id.recordName] = modification.record
                }
            }
            for deletion in changes.deletions {
                records[deletion.recordID.recordName] = nil
            }
            token = changes.changeToken
            more = changes.moreComing
        }
        return records
    }

    private func reconcile(_ remote: [String: CKRecord]) async throws {
        let local = localFiles()
        for name in Set(remote.keys).union(local.keys).sorted() {
            let record = remote[name]
            let remoteHash = record?["sha256"] as? String
            let localHash = local[name]
            let base = manifest[name]
            switch (localHash, remoteHash) {
            case let (here?, there?) where here == there:
                manifest[name] = here
            case let (here?, there?):
                if base == here {
                    try download(record!, as: name)  // changed only elsewhere
                } else if base == there {
                    try await upload(name)  // changed only here
                } else if let record, (record["modified"] as? Date ?? .distantPast) > modified(name) {
                    moveToConflicts(name)
                    try download(record, as: name)
                    Self.log("\(name) changed here and elsewhere; kept the newer, from iCloud")
                } else {
                    if let record {
                        try download(record, as: name, into: conflicts, suffix: true)
                    }
                    try await upload(name)
                    Self.log("\(name) changed here and elsewhere; kept the newer, from here")
                }
            case let (nil, there?):
                if base == there {
                    try await deleteRemote(name)  // deleted here since the last sync
                } else {
                    try download(record!, as: name)
                }
            case let (here?, nil):
                if base == here {
                    moveToConflicts(name)  // deleted elsewhere
                    manifest[name] = nil
                    Self.log("\(name) was deleted on another device; moved it to the conflicts folder")
                } else {
                    try await upload(name)
                }
            case (nil, nil):
                manifest[name] = nil
            }
        }
    }

    /// A save the game changed while running: up to iCloud, or deleted there.
    private func send(_ name: String) async {
        guard available else {
            return
        }
        do {
            if FileManager.default.fileExists(atPath: folder.appendingPathComponent(name).path) {
                if let hash = hash(name), manifest[name] != hash {
                    try await upload(name)
                }
            } else if manifest[name] != nil {
                try await deleteRemote(name)
            }
        } catch {
            Self.log("couldn't sync \(name): \(error.localizedDescription); it goes up at the next launch")
        }
    }

    private func upload(_ name: String) async throws {
        let file = folder.appendingPathComponent(name)
        let data = try Data(contentsOf: file)
        // The asset is read as it uploads, and the game may write the file
        // again meanwhile: upload a copy.
        let copy = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString + ".gci")
        try data.write(to: copy)
        defer { try? FileManager.default.removeItem(at: copy) }
        let record = CKRecord(recordType: Self.recordType, recordID: CKRecord.ID(recordName: name, zoneID: zone))
        record["file"] = CKAsset(fileURL: copy)
        record["modified"] = modified(name)
        record["sha256"] = Self.hash(data)
        let (saved, _) = try await database.modifyRecords(saving: [record], deleting: [], savePolicy: .allKeys)
        if case .failure(let error)? = saved[record.recordID] {
            throw error
        }
        manifest[name] = Self.hash(data)
        Self.log("uploaded \(name)")
    }

    private func deleteRemote(_ name: String) async throws {
        let id = CKRecord.ID(recordName: name, zoneID: zone)
        let (_, deleted) = try await database.modifyRecords(saving: [], deleting: [id])
        if case .failure(let error)? = deleted[id], (error as? CKError)?.code != .unknownItem {
            throw error
        }
        manifest[name] = nil
        Self.log("deleted \(name) from iCloud")
    }

    /// Writes `record`'s file into `folder` (the card's, unless the game has
    /// it), through a temporary file and a rename as the card does.
    private func download(_ record: CKRecord, as name: String, into target: URL? = nil, suffix: Bool = false) throws {
        guard let asset = record["file"] as? CKAsset, let source = asset.fileURL else {
            throw CKError(.assetFileNotFound)
        }
        let toCard = target == nil
        if toCard && gameStarted {
            return
        }
        let directory = target ?? folder
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let file = directory.appendingPathComponent(suffix ? Self.conflictName(name) : name)
        let temp = file.appendingPathExtension("tmp")
        try? FileManager.default.removeItem(at: temp)
        try FileManager.default.copyItem(at: source, to: temp)
        if let date = record["modified"] as? Date {
            try? FileManager.default.setAttributes([.modificationDate: date], ofItemAtPath: temp.path)
        }
        guard rename(temp.path, file.path) == 0 else {
            throw CocoaError(.fileWriteUnknown, userInfo: [NSFilePathErrorKey: file.path])
        }
        if toCard {
            manifest[name] = record["sha256"] as? String
            Self.log("downloaded \(name)")
        }
    }

    /// Moves the card's `name` to the conflicts folder (not while the game has the card).
    private func moveToConflicts(_ name: String) {
        guard !gameStarted else {
            return
        }
        try? FileManager.default.createDirectory(at: conflicts, withIntermediateDirectories: true)
        try? FileManager.default.moveItem(at: folder.appendingPathComponent(name),
                                          to: conflicts.appendingPathComponent(Self.conflictName(name)))
    }

    // MARK: Files

    /// The card's .gci files and their SHA-256.
    private func localFiles() -> [String: String] {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []
        var files: [String: String] = [:]
        for name in names where name.lowercased().hasSuffix(".gci") {
            files[name] = hash(name)
        }
        return files
    }

    private func hash(_ name: String) -> String? {
        (try? Data(contentsOf: folder.appendingPathComponent(name))).map(Self.hash)
    }

    private func modified(_ name: String) -> Date {
        let attributes = try? FileManager.default.attributesOfItem(atPath: folder.appendingPathComponent(name).path)
        return attributes?[.modificationDate] as? Date ?? .distantPast
    }

    private func saveManifest() {
        if let data = try? JSONEncoder().encode(manifest) {
            try? data.write(to: manifestURL, options: .atomic)
        }
    }

    private static func hash(_ data: Data) -> String {
        SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    }

    /// "<name> (conflict 2026-11-02 18.04.31).gci"
    private static func conflictName(_ name: String) -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyy-MM-dd HH.mm.ss"
        let base = (name as NSString).deletingPathExtension
        return "\(base) (conflict \(formatter.string(from: Date()))).gci"
    }

    private static func log(_ message: String) {
        FileHandle.standardError.write(Data("frontline: saves: \(message)\n".utf8))
    }
}
