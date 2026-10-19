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

        for choice in EditorChoice.catalog {
            let mi = NSMenuItem(title: choice.displayName,
                                action: #selector(selectEditor(_:)), keyEquivalent: "")
            mi.target = self
            mi.representedObject = choice.id
            mi.state = (settings.selectedId == choice.id) ? .on : .off
            menu.addItem(mi)
        }

        menu.addItem(.separator())

        let isCustom = settings.selectedId == EditorChoice.customId
        let customTitle = (isCustom && !settings.customCommand.isEmpty)
            ? "Custom: \(settings.customCommand)…" : "Custom…"
        let customItem = NSMenuItem(title: customTitle,
                                    action: #selector(editCustomEditor), keyEquivalent: "")
        customItem.target = self
        customItem.state = isCustom ? .on : .off
        menu.addItem(customItem)

        // Terminal host — only meaningful when the selected editor is a
        // terminal editor, so the parent is disabled otherwise.
        menu.addItem(.separator())
        let hostParent = NSMenuItem(title: "Terminal App", action: nil, keyEquivalent: "")
        let hostMenu = NSMenu()
        hostMenu.autoenablesItems = false
        for host in TerminalHost.allCases where host.isInstalled {
            let hi = NSMenuItem(title: host.displayName,
                                action: #selector(selectTerminalHost(_:)), keyEquivalent: "")
            hi.target = self
            hi.representedObject = host.rawValue
            hi.state = (settings.terminalHost == host) ? .on : .off
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
        guard let raw = sender.representedObject as? String,
              let host = TerminalHost(rawValue: raw) else { return }
        EditorSettings.shared.terminalHost = host
    }

    // A tiny prompt for a custom command + whether it runs in a terminal.
    // Saving a non-empty command also selects "Custom" as the active editor.
    @objc private func editCustomEditor() {
        let settings = EditorSettings.shared
        let alert = NSAlert()
        alert.messageText = "Custom Editor"
        alert.informativeText =
            "Command (terminal) or application name (GUI) to open files with."
        alert.addButton(withTitle: "Save")
        alert.addButton(withTitle: "Cancel")

        let field = NSTextField(frame: NSRect(x: 0, y: 26, width: 260, height: 24))
        field.stringValue = settings.customCommand
        field.placeholderString = "e.g. vim, code, Sublime Text"

        let checkbox = NSButton(checkboxWithTitle: "Runs in a terminal",
                                target: nil, action: nil)
        checkbox.frame = NSRect(x: 0, y: 0, width: 260, height: 18)
        checkbox.state = settings.customRunsInTerminal ? .on : .off

        let accessory = NSView(frame: NSRect(x: 0, y: 0, width: 260, height: 50))
        accessory.addSubview(field)
        accessory.addSubview(checkbox)
        alert.accessoryView = accessory

        NSApp.activate(ignoringOtherApps: true)
        alert.window.initialFirstResponder = field
        guard alert.runModal() == .alertFirstButtonReturn else { return }

        let cmd = field.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
        settings.customCommand = cmd
        settings.customRunsInTerminal = checkbox.state == .on
        if !cmd.isEmpty { settings.selectedId = EditorChoice.customId }
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
