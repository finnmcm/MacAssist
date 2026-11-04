import Foundation

// Installs and starts macassistd as a per-user launchd agent (PLAN.md §8).
//
// In a shipped build the daemon binary rides along inside the app bundle at
// Contents/Resources/macassistd; on launch the app writes a LaunchAgent plist
// pointing at it (RunAtLoad + KeepAlive) and bootstraps it into the user's
// gui domain, so the daemon comes up at login and stays up with the panel
// closed. When the bundled daemon can't be found — i.e. the dev workflow of
// running the SwiftPM binary directly — installation is skipped and the
// daemon is expected to be started by hand (scripts/dev-run.sh).
enum DaemonInstaller {
    // Matches the plist name in PLAN.md §8. The gui-domain service target is
    // "gui/<uid>/<label>".
    private static let label = "com.finn.macassist.daemon"

    private static var plistURL: URL {
        URL(fileURLWithPath: NSHomeDirectory())
            .appendingPathComponent("Library/LaunchAgents/\(label).plist")
    }

    private static var logDir: URL {
        URL(fileURLWithPath: NSHomeDirectory())
            .appendingPathComponent("Library/Logs/MacAssist")
    }

    // Ensure the agent is installed and running. Safe to call on every launch:
    // it only rewrites the plist and restarts the daemon when something changed
    // (first install, an app version bump, or the app moved on disk), otherwise
    // it just makes sure the existing agent is loaded. Never throws — failures
    // are logged; a missing daemon leaves the dev workflow untouched.
    static func ensureInstalled() {
        guard let daemonPath = bundledDaemonPath() else {
            NSLog("MacAssist: no bundled daemon (dev build?); skipping launchd install")
            return
        }

        try? FileManager.default.createDirectory(
            at: logDir, withIntermediateDirectories: true)
        try? FileManager.default.createDirectory(
            at: plistURL.deletingLastPathComponent(),
            withIntermediateDirectories: true)

        let desired = plistData(daemonPath: daemonPath)
        let existing = try? Data(contentsOf: plistURL)

        if existing == desired {
            // Plist is already current. Normally RunAtLoad has it going; if it
            // isn't loaded (e.g. an earlier bootout), bootstrap loads it — and
            // if it's already loaded, bootstrap's "already bootstrapped" error
            // is harmless and swallowed. No restart, so an in-flight sweep is
            // left alone.
            bootstrap()
            return
        }

        // First install, version bump, or the app moved: replace the plist and
        // restart the daemon so it runs from the current binary/args.
        NSLog("MacAssist: installing launchd agent → \(daemonPath)")
        bootout()
        do {
            try desired.write(to: plistURL, options: .atomic)
        } catch {
            NSLog("MacAssist: failed to write \(plistURL.path): \(error)")
            return
        }
        bootstrap()
    }

    // MARK: - bundled daemon

    // The daemon shipped in the app bundle, or nil when running as a bare
    // SwiftPM binary (dev). `url(forResource:)` resolves to Contents/Resources
    // in a real .app and finds nothing in a loose build directory.
    private static func bundledDaemonPath() -> String? {
        guard let url = Bundle.main.url(forResource: "macassistd", withExtension: nil),
              FileManager.default.isExecutableFile(atPath: url.path) else {
            return nil
        }
        return url.path
    }

    // MARK: - plist

    private static func plistData(daemonPath: String) -> Data {
        // The daemon takes its socket/db/roots from defaults (HOME-derived), so
        // no ProgramArguments beyond the binary itself. The version markers
        // aren't read by the daemon — they live in the plist purely so a new
        // release changes the plist bytes and triggers a reinstall/restart even
        // when the app is upgraded in place at the same path.
        let info = Bundle.main.infoDictionary
        let shortVersion = info?["CFBundleShortVersionString"] as? String ?? "0"
        let buildVersion = info?["CFBundleVersion"] as? String ?? "0"

        let dict: [String: Any] = [
            "Label": label,
            "ProgramArguments": [daemonPath],
            "RunAtLoad": true,
            "KeepAlive": true,
            // Background-index workload: let the scheduler treat it as such.
            "ProcessType": "Adaptive",
            "StandardOutPath": logDir.appendingPathComponent("daemon.out.log").path,
            "StandardErrorPath": logDir.appendingPathComponent("daemon.err.log").path,
            // HOME is already set in the gui domain, but the daemon hard-depends
            // on it; set it explicitly as insurance. The version keys are the
            // reinstall trigger described above.
            "EnvironmentVariables": [
                "HOME": NSHomeDirectory(),
                "MACASSIST_APP_SHORT_VERSION": shortVersion,
                "MACASSIST_APP_BUILD_VERSION": buildVersion,
            ],
        ]

        // PropertyListSerialization writes dictionary keys in a stable order,
        // so byte-equality against the on-disk plist is a reliable "unchanged"
        // check. Falling back to empty Data on the (practically impossible)
        // failure just forces a reinstall next launch.
        return (try? PropertyListSerialization.data(
            fromPropertyList: dict, format: .xml, options: 0)) ?? Data()
    }

    // MARK: - launchctl

    private static var domainTarget: String { "gui/\(getuid())" }
    private static var serviceTarget: String { "\(domainTarget)/\(label)" }

    // True if the agent is currently loaded in the gui domain.
    private static func isLoaded() -> Bool {
        runLaunchctl(["print", serviceTarget]).status == 0
    }

    private static func bootstrap() {
        // Retry the transient teardown race: a bootstrap issued just after a
        // bootout can hit "Input/output error" (code 5) while launchd is still
        // releasing the label. Also covers the no-change path where the agent
        // is already loaded — bootstrap fails, isLoaded() is true, we're done.
        for attempt in 0..<10 {
            let (status, out) = runLaunchctl(["bootstrap", domainTarget, plistURL.path])
            if status == 0 { return }
            if isLoaded() { return }  // already up (or a racing launch won)
            if attempt == 9 {
                NSLog("MacAssist: launchctl bootstrap → \(status) \(out)")
                return
            }
            usleep(150_000)  // 150ms; let a lingering teardown finish
        }
    }

    private static func bootout() {
        // bootout is asynchronous — it returns before the job actually exits —
        // so a bootstrap issued immediately after can fail because the old job
        // still owns the label. Kick off the unload (ignoring "wasn't loaded"),
        // then wait, bounded, for the service to actually disappear so the
        // caller can re-bootstrap cleanly within this same launch.
        _ = runLaunchctl(["bootout", serviceTarget])
        for _ in 0..<30 {  // up to ~3s
            if !isLoaded() { return }
            usleep(100_000)  // 100ms
        }
    }

    @discardableResult
    private static func runLaunchctl(_ args: [String]) -> (status: Int32, output: String) {
        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: "/bin/launchctl")
        proc.arguments = args
        let pipe = Pipe()
        proc.standardOutput = pipe
        proc.standardError = pipe
        do {
            try proc.run()
            proc.waitUntilExit()
        } catch {
            return (-1, "launch failed: \(error)")
        }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        let text = String(data: data, encoding: .utf8)?
            .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return (proc.terminationStatus, text)
    }
}
