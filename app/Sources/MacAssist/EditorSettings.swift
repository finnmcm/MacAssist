import AppKit

// The user's "open in editor" preference (EDITOR_PLAN.md, Brick 2). Supports
// both terminal editors (vim/nvim/... launched inside a terminal host) and GUI
// editors (VS Code/Xcode/... launched directly). Persisted in UserDefaults;
// read by the menu-bar "Default Editor" submenu (main.swift) and by the launch
// engine (Brick 3). Default is vim in Terminal.app.
//
// A single shared instance keeps the preference reachable from both the menu
// (AppController) and the action path (SearchModel) without threading an
// object through — the app is tiny and entirely main-actor.

enum EditorKind: String, Sendable {
    case terminal  // launched inside a TerminalHost (vim, nvim, nano, emacs)
    case gui       // launched directly via `open -a` / NSWorkspace
}

// One selectable editor. For `.terminal`, `launchTarget` is the command to run
// (resolved on the shell's PATH inside the terminal). For `.gui`, it is the
// application name passed to `open -a` / NSWorkspace (not a CLI, to avoid the
// PATH an LSUIElement app lacks). `bundleIds` are used only to detect whether a
// GUI editor is installed (any one resolving ⇒ installed); several are listed
// where the id varies by version/distribution.
struct EditorChoice: Identifiable, Equatable, Sendable {
    let id: String
    let displayName: String
    let kind: EditorKind
    let launchTarget: String
    var bundleIds: [String] = []

    // Whether to offer this editor in the menu. Terminal editors are always
    // offered: their command lives on the terminal's shell PATH, which an
    // LSUIElement app can't reliably probe. GUI editors are "installed" when any
    // of their bundle ids resolves via LaunchServices (so uninstalled apps are
    // hidden). A GUI entry with no bundle ids is left visible rather than hidden.
    var isInstalled: Bool {
        if kind == .terminal || bundleIds.isEmpty { return true }
        return bundleIds.contains {
            NSWorkspace.shared.urlForApplication(withBundleIdentifier: $0) != nil
        }
    }
}

extension EditorChoice {
    // Curated list shown in the menu. Order is the menu order.
    static let catalog: [EditorChoice] = [
        .init(id: "vim",      displayName: "vim",          kind: .terminal, launchTarget: "vim"),
        .init(id: "nvim",     displayName: "Neovim",       kind: .terminal, launchTarget: "nvim"),
        .init(id: "nano",     displayName: "nano",         kind: .terminal, launchTarget: "nano"),
        .init(id: "emacs",    displayName: "Emacs",        kind: .terminal, launchTarget: "emacs"),
        .init(id: "vscode",   displayName: "VS Code",      kind: .gui,      launchTarget: "Visual Studio Code",
              bundleIds: ["com.microsoft.VSCode"]),
        .init(id: "cursor",   displayName: "Cursor",       kind: .gui,      launchTarget: "Cursor",
              bundleIds: ["com.todesktop.230313mzl4w4u92"]),
        .init(id: "zed",      displayName: "Zed",          kind: .gui,      launchTarget: "Zed",
              bundleIds: ["dev.zed.Zed"]),
        .init(id: "sublime",  displayName: "Sublime Text", kind: .gui,      launchTarget: "Sublime Text",
              bundleIds: ["com.sublimetext.4", "com.sublimetext.3"]),
        .init(id: "xcode",    displayName: "Xcode",        kind: .gui,      launchTarget: "Xcode",
              bundleIds: ["com.apple.dt.Xcode"]),
        .init(id: "textedit", displayName: "TextEdit",     kind: .gui,      launchTarget: "TextEdit",
              bundleIds: ["com.apple.TextEdit"]),
    ]

    static let defaultChoice = catalog[0]  // vim
}

// The terminal application a terminal editor is launched inside. Terminal.app
// is always present on macOS; iTerm is offered only when installed.
enum TerminalHost: String, CaseIterable, Sendable {
    case terminal
    case iterm

    var displayName: String {
        switch self {
        case .terminal: return "Terminal"
        case .iterm:    return "iTerm"
        }
    }

    var bundleId: String {
        switch self {
        case .terminal: return "com.apple.Terminal"
        case .iterm:    return "com.googlecode.iterm2"
        }
    }

    var isInstalled: Bool {
        NSWorkspace.shared.urlForApplication(withBundleIdentifier: bundleId) != nil
    }
}

@MainActor
final class EditorSettings {
    static let shared = EditorSettings()

    private let defaults: UserDefaults
    private enum Key {
        static let selectedId = "editor.selectedId"
        static let terminalHost = "editor.terminalHost"
    }

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    // MARK: - stored preferences

    // The id of the chosen catalog entry. An unknown stored id (e.g. a stale
    // "custom" from the removed custom-editor entry) resolves to the default.
    var selectedId: String {
        get {
            let id = defaults.string(forKey: Key.selectedId) ?? EditorChoice.defaultChoice.id
            return EditorChoice.catalog.contains { $0.id == id }
                ? id : EditorChoice.defaultChoice.id
        }
        set { defaults.set(newValue, forKey: Key.selectedId) }
    }

    var terminalHost: TerminalHost {
        get { TerminalHost(rawValue: defaults.string(forKey: Key.terminalHost) ?? "") ?? .terminal }
        set { defaults.set(newValue.rawValue, forKey: Key.terminalHost) }
    }

    // MARK: - resolved choice

    // The editor to actually launch. Falls back to vim if the stored id is
    // unknown (e.g. a stale value from a removed catalog entry).
    var current: EditorChoice {
        EditorChoice.catalog.first { $0.id == selectedId } ?? EditorChoice.defaultChoice
    }
}
