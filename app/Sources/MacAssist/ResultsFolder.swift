import Foundation

// Materializes a set of search hits as a folder of symlinks, so the user
// can browse every relevant file at once in a real Finder window. Each
// search gets its own fresh folder (hence a new window); previous folders
// are cleared on each new search — they're disposable outputs the app never
// reads back, so retaining history would be clutter, not a feature.
enum ResultsFolder {
    enum FolderError: Error, CustomStringConvertible {
        case noHits
        var description: String { "no results to show" }
    }

    // Root for per-search folders, alongside the daemon's socket/index.
    static var root: URL {
        let base = FileManager.default.urls(
            for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return base.appendingPathComponent("MacAssist/Results", isDirectory: true)
    }

    // Builds a fresh folder of symlinks to `hits` and returns its URL.
    @discardableResult
    static func build(query: String, hits: [FileHit]) throws -> URL {
        guard !hits.isEmpty else { throw FolderError.noHits }
        let fm = FileManager.default
        try fm.createDirectory(at: root, withIntermediateDirectories: true)
        clearPrevious()  // only the current search's folder is kept

        let dir = root.appendingPathComponent(folderName(for: query), isDirectory: true)
        // Start from a clean, empty folder even if the name repeats.
        try? fm.removeItem(at: dir)
        try fm.createDirectory(at: dir, withIntermediateDirectories: true)

        var used = Set<String>()
        for hit in hits {
            let target = URL(fileURLWithPath: hit.path)
            let linkName = uniqueName(hit.name, used: &used)
            let link = dir.appendingPathComponent(linkName)
            // Best-effort per link: a missing/renamed target shouldn't sink
            // the whole folder.
            try? fm.createSymbolicLink(at: link, withDestinationURL: target)
        }
        return dir
    }

    // MARK: - naming

    // "drake mp4s" -> "drake mp4s · 14.23.05". Keeps it readable in Finder
    // while staying filesystem-safe and collision-resistant across searches.
    private static func folderName(for query: String) -> String {
        let cleaned = query
            .components(separatedBy: CharacterSet(charactersIn: "/:\n\t"))
            .joined(separator: " ")
            .trimmingCharacters(in: .whitespacesAndNewlines)
        let label = cleaned.isEmpty ? "results" : String(cleaned.prefix(40))

        let f = DateFormatter()
        f.dateFormat = "HH.mm.ss"
        return "\(label) · \(f.string(from: Date()))"
    }

    // Ensures each symlink name is unique within the folder, suffixing
    // " 2", " 3", … before the extension on collisions.
    private static func uniqueName(_ name: String, used: inout Set<String>) -> String {
        if used.insert(name).inserted { return name }
        let ext = (name as NSString).pathExtension
        let stem = (name as NSString).deletingPathExtension
        var n = 2
        while true {
            let candidate = ext.isEmpty ? "\(stem) \(n)" : "\(stem) \(n).\(ext)"
            if used.insert(candidate).inserted { return candidate }
            n += 1
        }
    }

    // MARK: - cleanup

    // Removes every existing result folder. Runs before each search builds
    // its own folder, so only the current search's output survives. If a
    // "recent searches" feature is ever added, a bounded retention policy
    // would replace this.
    private static func clearPrevious() {
        let fm = FileManager.default
        guard let entries = try? fm.contentsOfDirectory(
            at: root, includingPropertiesForKeys: nil,
            options: [.skipsHiddenFiles]) else { return }
        for entry in entries {
            try? fm.removeItem(at: entry)
        }
    }
}
