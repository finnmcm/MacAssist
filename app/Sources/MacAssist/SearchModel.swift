import AppKit
import Combine
import Foundation

// One active filter-toolbar selection. `.kind` carries the canonical daemon
// kind (image|audio|video|document), never the UI label — the toolbar maps
// labels (Photos/Videos/Music/Documents) to these in SearchView.
enum Filter: Equatable, Hashable, Identifiable {
    case recentlyDownloaded
    case kind(String)
    case tag(color: Int)

    var id: String {
        switch self {
        case .recentlyDownloaded: return "downloaded"
        case .kind(let k): return "kind:\(k)"
        case .tag(let c): return "tag:\(c)"
        }
    }
}

// Drives the panel: debounces keystrokes into daemon queries, publishes
// ranked hits, and performs the per-result actions. Phase 1 wires Stage A
// ("instant") only; Stage B refinement arrives with the intelligence work
// in Phase 3.
@MainActor
final class SearchModel: ObservableObject {
    @Published var queryText = ""
    @Published var activeFilters: [Filter] = []
    @Published var results: [FileHit] = []
    @Published var selection: Int = 0
    @Published var status: String = ""

    // Called after an action that should close the panel (open file / show
    // all). Wired by PanelController; keeps the view free of window logic.
    var onDismiss: (() -> Void)?

    private let client: DaemonClient
    private var requestID = 0
    private var latestDispatched = 0
    private var debounce: AnyCancellable?

    init(client: DaemonClient) {
        self.client = client
        // Coalesce rapid typing; the daemon is fast but there's no point
        // firing a socket round-trip on every keystroke. A filter toggle fires
        // the same path, so the two sources are combined.
        debounce = Publishers.CombineLatest($queryText, $activeFilters)
            .removeDuplicates { $0.0 == $1.0 && $0.1 == $1.1 }
            .debounce(for: .milliseconds(120), scheduler: RunLoop.main)
            .sink { [weak self] text, filters in self?.run(text, filters) }
    }

    // Called when the panel opens so a stale result list isn't shown.
    // Persistence = clear everything on open, so filters reset too.
    func reset() {
        queryText = ""
        activeFilters = []
        results = []
        selection = 0
        status = ""
    }

    // Translate the active filters into the wire shape. `.kind` values are
    // already canonical daemon kinds (set by the toolbar), so no mapping here.
    private func wireFilters(_ filters: [Filter]) -> QueryFilters {
        var qf = QueryFilters()
        for f in filters {
            switch f {
            case .recentlyDownloaded: qf.downloaded = true
            case .kind(let k): qf.kinds.append(k)
            case .tag(let c): qf.tagColors.append(c)
            }
        }
        return qf
    }

    private func run(_ text: String, _ filters: [Filter]) {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        let qf = wireFilters(filters)
        // Empty text is now a valid query when filters are active; only bail
        // when there is nothing at all to search on.
        guard !trimmed.isEmpty || !qf.isEmpty else {
            results = []
            status = ""
            return
        }
        requestID += 1
        let id = requestID
        latestDispatched = id

        Task.detached(priority: .userInitiated) { [client] in
            let outcome: Result<[FileHit], Error>
            do {
                outcome = .success(try client.query(trimmed, id: id, filters: qf))
            } catch {
                outcome = .failure(error)
            }
            await MainActor.run { [weak self] in
                guard let self, id == self.latestDispatched else { return }
                switch outcome {
                case .success(let hits):
                    self.results = hits
                    self.selection = 0
                    self.status = hits.isEmpty ? "No matches" : ""
                case .failure(let err):
                    self.results = []
                    self.status = "Daemon unavailable (\(err))"
                }
            }
        }
    }

    // MARK: - filters

    // Toolbar button: add the filter if absent, remove it if already active.
    func toggle(_ filter: Filter) {
        if let i = activeFilters.firstIndex(of: filter) {
            activeFilters.remove(at: i)
        } else {
            activeFilters.append(filter)
        }
    }

    // Chip ×.
    func remove(_ filter: Filter) {
        activeFilters.removeAll { $0 == filter }
    }

    // Backspace on an empty field removes the most recently added chip.
    func removeLast() {
        if !activeFilters.isEmpty { activeFilters.removeLast() }
    }

    var hasFilters: Bool { !activeFilters.isEmpty }

    func isActive(_ filter: Filter) -> Bool { activeFilters.contains(filter) }

    // MARK: - actions

    func moveSelection(_ delta: Int) {
        guard !results.isEmpty else { return }
        selection = max(0, min(results.count - 1, selection + delta))
    }

    // Enter: reveal the selected (default top) hit in Finder — jump to its
    // real location and highlight it there, rather than opening it.
    func revealSelected() {
        guard results.indices.contains(selection) else { return }
        reveal(results[selection])
    }

    // ⌘Enter / "Show all" footer: gather every hit into a fresh folder of
    // links and open it in a new Finder window, so the user can browse all
    // relevant files at once rather than committing to a single guess.
    func openResults() {
        guard !results.isEmpty else { return }
        do {
            let dir = try ResultsFolder.build(query: queryText, hits: results)
            NSWorkspace.shared.open(dir)
        } catch {
            status = "Couldn't open results (\(error))"
            return  // leave the panel up so the message is visible
        }
        onDismiss?()
    }

    func reveal(_ hit: FileHit) {
        NSWorkspace.shared.activateFileViewerSelecting(
            [URL(fileURLWithPath: hit.path)])
        onDismiss?()
    }

    // ⌘C: copy the selected path. Deliberately does not dismiss — copying is
    // often a prelude to another action in the same session.
    func copySelectedPath() {
        guard results.indices.contains(selection) else { return }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(results[selection].path, forType: .string)
    }
}
