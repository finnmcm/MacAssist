import AppKit

// Entry point. MacAssist is a menu-bar-resident agent (no Dock icon): the
// global hotkey summons the search panel; the menu bar item carries status
// and Quit. See PLAN.md section 7.
@MainActor
final class AppController: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private var statusItem: NSStatusItem?
    private var hotKey: HotKey?
    private var panel: PanelController?
    private let model = SearchModel(client: DaemonClient(socketPath: Proto.socketPath))

    // The "Default Editor ▸" submenu. Rebuilt on each open (menuNeedsUpdate)
    // so checkmarks, the custom label, and the terminal-host state stay live.
    private let editorSubmenu = NSMenu()

    func applicationDidFinishLaunching(_ notification: Notification) {
        panel = PanelController(model: model)

        hotKey = HotKey { [weak self] in self?.panel?.toggle() }
        if hotKey == nil {
            NSLog("MacAssist: failed to register global hotkey (Option-Space)")
        }

        setUpMenuBar()
    }

    func applicationWillTerminate(_ notification: Notification) {
        // Result folders are disposable per-search outputs; drop them on quit
        // so no hard links (or .DS_Store) linger between sessions.
        ResultsFolder.removeAll()
    }

    private func setUpMenuBar() {
        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        // Fall back through a couple of symbols, then to a text glyph, so
        // the item is never a zero-width (invisible) button.
        if let image = NSImage(systemSymbolName: "magnifyingglass",
                               accessibilityDescription: "MacAssist") {
            item.button?.image = image
        } else {
            item.button?.title = "⌕"
        }

        let menu = NSMenu()
        menu.addItem(withTitle: "Search…  (⌥Space)",
                     action: #selector(openPanel), keyEquivalent: "")
            .target = self
        menu.addItem(.separator())

        // Default Editor ▸ — the open-in-editor preference (EDITOR_PLAN.md).
        editorSubmenu.delegate = self          // rebuilt lazily on open
        editorSubmenu.autoenablesItems = false  // we manage enabled/checkmarks
        let editorItem = NSMenuItem(title: "Default Editor", action: nil, keyEquivalent: "")
        editorItem.submenu = editorSubmenu
        menu.addItem(editorItem)

        menu.addItem(.separator())
        menu.addItem(withTitle: "Quit MacAssist",
                     action: #selector(NSApplication.terminate(_:)),
                     keyEquivalent: "q")
        item.menu = menu
        item.isVisible = true
        statusItem = item
    }

    @objc private func openPanel() {
        panel?.show()
    }

    // MARK: - Default Editor submenu

    // Rebuild the submenu just before it opens, so checkmarks / the custom
    // label / the terminal-host enabled state reflect the current preference
    // (and a newly-installed iTerm appears).
    func menuNeedsUpdate(_ menu: NSMenu) {
        guard menu === editorSubmenu else { return }
        let settings = EditorSettings.shared
        let current = settings.current
        menu.removeAllItems()

        // Only offer installed editors (GUI editors resolve via LaunchServices;
        // terminal editors are always installed). The active selection is kept
        // visible even if it's an uninstalled GUI editor, so its checkmark and
        // name don't silently vanish.
        for choice in EditorChoice.catalog
        where choice.isInstalled || choice.id == settings.selectedId {
            let mi = NSMenuItem(title: choice.displayName,
                                action: #selector(selectEditor(_:)), keyEquivalent: "")
            mi.target = self
            mi.representedObject = choice.id
            mi.state = (settings.selectedId == choice.id) ? .on : .off
            menu.addItem(mi)
        }

        // Terminal host — only meaningful when the selected editor is a
        // terminal editor, so the parent is disabled otherwise.
        menu.addItem(.separator())
        let hostParent = NSMenuItem(title: "Terminal App", action: nil, keyEquivalent: "")
        let hostMenu = NSMenu()
        hostMenu.autoenablesItems = false
        // Only installed terminals (Terminal.app always resolves); the active
        // selection stays visible even if its app was removed, mirroring the
        // editor list above.
        for host in TerminalHost.catalog
        where host.isInstalled || host.id == settings.terminalHost.id {
            let hi = NSMenuItem(title: host.displayName,
                                action: #selector(selectTerminalHost(_:)), keyEquivalent: "")
            hi.target = self
            hi.representedObject = host.id
            hi.state = (settings.terminalHost.id == host.id) ? .on : .off
            hostMenu.addItem(hi)
        }
        hostParent.submenu = hostMenu
        hostParent.isEnabled = current.kind == .terminal
        menu.addItem(hostParent)
    }

    @objc private func selectEditor(_ sender: NSMenuItem) {
        guard let id = sender.representedObject as? String else { return }
        EditorSettings.shared.selectedId = id
    }

    @objc private func selectTerminalHost(_ sender: NSMenuItem) {
        guard let id = sender.representedObject as? String,
              let host = TerminalHost.catalog.first(where: { $0.id == id }) else { return }
        EditorSettings.shared.terminalHost = host
    }
}

// Headless connectivity check: `MacAssist query <text...>` runs one query
// through the same DaemonClient the panel uses and prints the hits, then
// exits. Handy for confirming the app can reach the daemon without the GUI.
let args = CommandLine.arguments
if args.count >= 3, args[1] == "query" {
    let text = args[2...].joined(separator: " ")
    let client = DaemonClient(socketPath: Proto.socketPath)
    do {
        let hits = try client.query(text, id: 1)
        if hits.isEmpty { print("(no matches)") }
        for h in hits {
            print("\(String(format: "%.3f", h.score))  [\(h.kind)]  \(h.name)")
            print("        \(h.path)")
        }
    } catch {
        FileHandle.standardError.write(Data("query failed: \(error)\n".utf8))
        exit(1)
    }
    exit(0)
}

// Top-level code runs on the main thread; assert that so the main-actor
// controller can be constructed here.
MainActor.assumeIsolated {
    let app = NSApplication.shared
    let controller = AppController()
    app.delegate = controller
    // .accessory: resident in the menu bar, no Dock icon, never steals
    // focus as the active app on launch.
    app.setActivationPolicy(.accessory)
    app.run()
}
