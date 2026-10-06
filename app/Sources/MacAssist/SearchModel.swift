import AppKit
import Combine
import Foundation

// Drives the panel: debounces keystrokes into daemon queries, publishes
// ranked hits, and performs the per-result actions. Phase 1 wires Stage A
// ("instant") only; Stage B refinement arrives with the intelligence work
// in Phase 3.
@MainActor
final class SearchModel: ObservableObject {
    @Published var queryText = ""
    @Published var results: [FileHit] = []
    @Published var selection: Int = 0
    @Published var status: String = ""

    private let client: DaemonClient
    private var requestID = 0
    private var latestDispatched = 0
    private var debounce: AnyCancellable?

    init(client: DaemonClient) {
        self.client = client
        // Coalesce rapid typing; the daemon is fast but there's no point
        // firing a socket round-trip on every keystroke.
        debounce = $queryText
            .removeDuplicates()
            .debounce(for: .milliseconds(120), scheduler: RunLoop.main)
            .sink { [weak self] text in self?.run(text) }
    }

    // Called when the panel opens so a stale result list isn't shown.
    func reset() {
        queryText = ""
        results = []
        selection = 0
        status = ""
    }

    private func run(_ text: String) {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else {
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
                outcome = .success(try client.query(trimmed, id: id))
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

    // MARK: - actions

    func moveSelection(_ delta: Int) {
        guard !results.isEmpty else { return }
        selection = max(0, min(results.count - 1, selection + delta))
    }

    // Enter: gather every hit into a fresh folder of symlinks and open it
    // in a new Finder window, so the user can browse all relevant files at
    // once rather than committing to a single guess.
    func openResults() {
        guard !results.isEmpty else { return }
        do {
            let dir = try ResultsFolder.build(query: queryText, hits: results)
            NSWorkspace.shared.open(dir)
        } catch {
            status = "Couldn't open results (\(error))"
        }
    }

    func open(_ hit: FileHit) {
        NSWorkspace.shared.open(URL(fileURLWithPath: hit.path))
    }

    func copySelectedPath() {
        guard results.indices.contains(selection) else { return }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(results[selection].path, forType: .string)
    }
}
