import Foundation

/// tvOS: a copy of the memory card in the user defaults.
///
/// A tvOS app can only write to its Caches folder, which the system may empty
/// when it runs low on space; the user defaults last (tvos-porting.md). So
/// after each change the game makes to the card, the whole card (its few
/// .gci files, a few KB each) is copied into the defaults, and at launch a
/// card folder that has been emptied gets them back. Both stay on the Apple
/// TV: deleting the app deletes them.
///
/// tvOS stops an app whose defaults pass about 1 MB in all, so a card that
/// outgrows `budget` isn't copied (the previous copy stays).
enum CardBackup {
    static let key = "memoryCardA"
    static let budget = 900 * 1024

    private static let queue = DispatchQueue(label: "frontline.card-backup")
    /// The latest scheduled copy; only touched on `queue`.
    nonisolated(unsafe) private static var generation = 0

    /// Puts the backed-up saves back into `folder` if it has none (Caches was
    /// emptied). Call before the game starts. The number of saves restored.
    @discardableResult
    static func restoreIfNeeded(folder: URL) -> Int {
        guard saves(in: folder).isEmpty,
              let backup = UserDefaults.standard.dictionary(forKey: key) as? [String: Data], !backup.isEmpty else {
            return 0
        }
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        var restored = 0
        for (name, data) in backup where isSaveName(name) {
            if (try? data.write(to: folder.appendingPathComponent(name), options: .atomic)) != nil {
                restored += 1
            }
        }
        print("frontline: card backup: restored \(restored) save(s) to \(folder.path)")
        return restored
    }

    /// Copies the card into the defaults shortly, once the game's burst of
    /// writes is over. Any thread (the game's, from the card's change hook).
    static func schedule(folder: URL) {
        queue.async {
            generation += 1
            let mine = generation
            queue.asyncAfter(deadline: .now() + 0.5) {
                if mine == generation {
                    copy(folder: folder)
                }
            }
        }
    }

    /// The card as it is now, deleted saves dropped too. On `queue`.
    private static func copy(folder: URL) {
        var backup: [String: Data] = [:]
        var total = 0
        for url in saves(in: folder) {
            guard let data = try? Data(contentsOf: url) else {
                continue
            }
            backup[url.lastPathComponent] = data
            total += data.count
        }
        guard total <= budget else {
            print("frontline: card backup: the card is \(total) bytes, past the \(budget) byte budget; not copied")
            return
        }
        UserDefaults.standard.set(backup, forKey: key)
    }

    private static func saves(in folder: URL) -> [URL] {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []
        return names.filter(isSaveName).map { folder.appendingPathComponent($0) }
    }

    /// A .gci file's own name, nothing that leaves the folder.
    private static func isSaveName(_ name: String) -> Bool {
        name.hasSuffix(".gci") && !name.contains("/") && !name.hasPrefix(".")
    }
}
