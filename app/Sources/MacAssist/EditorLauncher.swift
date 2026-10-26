import Foundation

// Opens a file in the user's chosen editor (EDITOR_PLAN.md, Brick 3). Two
// mechanisms behind one entry point:
//   • GUI editors  → `/usr/bin/open -a "<App>" <path>` (LaunchServices resolves
//     the app by name; no dependence on a PATH an LSUIElement app lacks).
//   • Terminal editors → an AppleScript that tells the chosen terminal host to
//     `cd` into the file's folder and run `<command> <path>`.
//
// Pure/stateless (takes the already-resolved choice + host), so the caller can
// run it off the main thread — the first terminal launch blocks on the system
// Automation (Apple Events) permission prompt. Failures are returned, never
// swallowed, so the caller can surface a message and keep the panel up.
// Which results ⌘E will open. `code` files are always eligible; plain-text
// documents (txt/md/csv, which the daemon classifies `document`, not `code`)
// are too. Binary "documents" (pdf/docx) and media are not — opening them in a
// text editor is garbage, so ⌘E is a no-op there (SearchModel shows a message).
enum EditorEligibility {
    private static let textExtensions: Set<String> = [
        "txt", "md", "markdown", "text", "log", "csv", "tsv",
    ]

    static func isEditable(kind: String, path: String) -> Bool {
        if kind == "code" { return true }
        return textExtensions.contains((path as NSString).pathExtension.lowercased())
    }
}

enum EditorLauncher {
    enum LaunchError: Error, Sendable {
        case appNotFound(editor: String)       // GUI app couldn't be resolved
        case automationDenied(host: String)    // TCC Apple Events prompt refused
        case launchFailed(detail: String)      // anything else

        // Short, user-facing line for the panel's status row.
        var userMessage: String {
            switch self {
            case .appNotFound(let e):
                return "Couldn't launch \(e) — is it installed?"
            case .automationDenied(let h):
                return "Allow MacAssist to control \(h) in System Settings › Privacy › Automation"
            case .launchFailed:
                return "Couldn't launch editor"
            }
        }
    }

    static func launch(path: String, choice: EditorChoice,
                       terminalHost: TerminalHost) -> Result<Void, LaunchError> {
        switch choice.kind {
        case .gui:
            return launchGUI(path: path, appName: choice.launchTarget)
        case .terminal:
            return launchTerminal(path: path, command: choice.launchTarget,
                                  host: terminalHost)
        }
    }

    // MARK: - GUI

    private static func launchGUI(path: String, appName: String)
        -> Result<Void, LaunchError> {
        let r = run("/usr/bin/open", ["-a", appName, path])
        if r.status == 0 { return .success(()) }
        // `open` prints "Unable to find application named '…'" for a bad -a.
        if r.stderr.localizedCaseInsensitiveContains("unable to find") {
            return .failure(.appNotFound(editor: appName))
        }
        return .failure(.launchFailed(detail: r.stderr))
    }

    // MARK: - terminal

    private static func launchTerminal(path: String, command: String,
                                       host: TerminalHost) -> Result<Void, LaunchError> {
        let dir = (path as NSString).deletingLastPathComponent
        // One shell string reused by every host; only the cd target and the
        // path are shell-quoted (the editor command is a bare word, e.g. "vim").
        let shellCmd = "cd \(shellQuote(dir)) && \(command) \(shellQuote(path))"
        switch host.strategy {
        case .appleScript(let flavor):
            return launchAppleScript(shellCmd: shellCmd, flavor: flavor, host: host)
        case .cli(let prefix):
            return launchCLI(shellCmd: shellCmd, prefix: prefix, host: host)
        }
    }

    // Terminal.app / iTerm: drive via osascript. These are the only hosts that
    // trip the TCC Automation (Apple Events) permission prompt.
    private static func launchAppleScript(shellCmd: String,
                                          flavor: TerminalHost.AppleScriptFlavor,
                                          host: TerminalHost) -> Result<Void, LaunchError> {
        let script = (flavor == .iterm) ? itermScript(shellCmd) : terminalScript(shellCmd)
        let r = run("/usr/bin/osascript", ["-e", script])
        if r.status == 0 { return .success(()) }
        // errAEEventNotPermitted (-1743) / "not authorized" ⇒ the user denied
        // (or hasn't granted) Automation control of the terminal app.
        let e = r.stderr
        if e.contains("-1743")
            || e.localizedCaseInsensitiveContains("not authori")
            || e.localizedCaseInsensitiveContains("not allowed") {
            return .failure(.automationDenied(host: host.displayName))
        }
        return .failure(.launchFailed(detail: e))
    }

    // Ghostty / WezTerm / kitty: `open -n <app> --args <recipe>`. The app is
    // resolved by bundle id (not PATH — an LSUIElement app has a restricted one)
    // and the nil check is the install guard: `open`'s exit status only reflects
    // the hand-off, so a missing terminal can't be detected from it afterward.
    // No AppleScript ⇒ no Automation prompt.
    private static func launchCLI(shellCmd: String, prefix: [String],
                                  host: TerminalHost) -> Result<Void, LaunchError> {
        guard let appURL = host.appURL else {
            return .failure(.appNotFound(editor: host.displayName))
        }
        let args = ["-n", appURL.path, "--args"] + cliArgs(prefix: prefix, shellCmd: shellCmd)
        let r = run("/usr/bin/open", args)
        if r.status == 0 { return .success(()) }
        return .failure(.launchFailed(detail: r.stderr))
    }

    // The `open --args` vector for a CLI terminal: the host's run-command prefix
    // followed by `sh -lc <shellCmd>`, so one shell string works everywhere.
    // Pure (no I/O) so it is checkable against the expected arrays:
    //   Ghostty → ["-e", "sh", "-lc", shellCmd]
    //   kitty   → ["sh", "-lc", shellCmd]
    //   WezTerm → ["start", "--", "sh", "-lc", shellCmd]
    static func cliArgs(prefix: [String], shellCmd: String) -> [String] {
        prefix + ["sh", "-lc", shellCmd]
    }

    private static func terminalScript(_ shellCmd: String) -> String {
        """
        tell application "Terminal"
            activate
            do script "\(appleScriptQuote(shellCmd))"
        end tell
        """
    }

    private static func itermScript(_ shellCmd: String) -> String {
        """
        tell application "iTerm"
            activate
            set w to (create window with default profile)
            tell current session of w
                write text "\(appleScriptQuote(shellCmd))"
            end tell
        end tell
        """
    }

    // MARK: - quoting

    // POSIX single-quote escaping: wrap in '…', and render any embedded
    // single quote as the '\'' idiom. Safe for arbitrary paths in a shell word.
    private static func shellQuote(_ s: String) -> String {
        "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }

    // Escape for embedding inside an AppleScript "…" string literal: backslash
    // first, then the double quote.
    private static func appleScriptQuote(_ s: String) -> String {
        s.replacingOccurrences(of: "\\", with: "\\\\")
         .replacingOccurrences(of: "\"", with: "\\\"")
    }

    // MARK: - process

    private static func run(_ launchPath: String, _ args: [String])
        -> (status: Int32, stderr: String) {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: launchPath)
        p.arguments = args
        let errPipe = Pipe()
        p.standardError = errPipe
        p.standardOutput = FileHandle.nullDevice
        do {
            try p.run()
        } catch {
            return (-1, "\(error)")
        }
        p.waitUntilExit()
        let data = errPipe.fileHandleForReading.readDataToEndOfFile()
        return (p.terminationStatus, String(data: data, encoding: .utf8) ?? "")
    }
}
