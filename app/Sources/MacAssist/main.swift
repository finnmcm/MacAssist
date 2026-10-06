import AppKit

// Entry point. MacAssist is a menu-bar-resident agent (no Dock icon): the
// global hotkey summons the search panel; the menu bar item carries status
// and Quit. See PLAN.md section 7.
@MainActor
final class AppController: NSObject, NSApplicationDelegate {
    private var statusItem: NSStatusItem?
    private var hotKey: HotKey?
    private var panel: PanelController?
    private let model = SearchModel(client: DaemonClient(socketPath: Proto.socketPath))

    func applicationDidFinishLaunching(_ notification: Notification) {
        panel = PanelController(model: model)

        hotKey = HotKey { [weak self] in self?.panel?.toggle() }
        if hotKey == nil {
            NSLog("MacAssist: failed to register global hotkey (Option-Space)")
        }

        setUpMenuBar()
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
