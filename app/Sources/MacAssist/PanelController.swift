import AppKit
import SwiftUI

// A non-activating floating panel (PLAN.md section 7): centered, blurred,
// escape to dismiss, focus returns to the previous app on close. Hosts the
// SwiftUI SearchView and owns the keyboard navigation monitor.
@MainActor
final class PanelController {
    private let panel: KeyablePanel
    private let model: SearchModel
    private var keyMonitor: Any?

    init(model: SearchModel) {
        self.model = model

        panel = KeyablePanel(
            contentRect: NSRect(x: 0, y: 0, width: 620, height: 56),
            styleMask: [.nonactivatingPanel, .borderless, .fullSizeContentView],
            backing: .buffered,
            defer: false)
        panel.level = .floating
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = true
        panel.isMovableByWindowBackground = true
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        panel.hidesOnDeactivate = false
        panel.animationBehavior = .utilityWindow

        // Blur material behind rounded-corner SwiftUI content.
        let blur = NSVisualEffectView()
        blur.material = .hudWindow
        blur.state = .active
        blur.blendingMode = .behindWindow
        blur.wantsLayer = true
        blur.layer?.cornerRadius = 14
        blur.layer?.masksToBounds = true

        let host = NSHostingView(rootView: SearchView(model: model))
        host.translatesAutoresizingMaskIntoConstraints = false
        blur.addSubview(host)
        NSLayoutConstraint.activate([
            host.leadingAnchor.constraint(equalTo: blur.leadingAnchor),
            host.trailingAnchor.constraint(equalTo: blur.trailingAnchor),
            host.topAnchor.constraint(equalTo: blur.topAnchor),
            host.bottomAnchor.constraint(equalTo: blur.bottomAnchor),
        ])
        panel.contentView = blur
    }

    var isVisible: Bool { panel.isVisible }

    func toggle() {
        isVisible ? hide() : show()
    }

    func show() {
        model.reset()
        panel.layoutIfNeeded()
        center()
        installKeyMonitor()
        NSApp.activate(ignoringOtherApps: true)
        panel.makeKeyAndOrderFront(nil)
    }

    func hide() {
        removeKeyMonitor()
        panel.orderOut(nil)
    }

    private func center() {
        guard let screen = NSScreen.main else { return }
        let frame = panel.frame
        let visible = screen.visibleFrame
        let x = visible.midX - frame.width / 2
        // Sit a little above vertical center, Spotlight-style.
        let y = visible.midY + visible.height * 0.12 - frame.height / 2
        panel.setFrameOrigin(NSPoint(x: x, y: y))
    }

    // MARK: - keyboard

    private func installKeyMonitor() {
        guard keyMonitor == nil else { return }
        keyMonitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) {
            [weak self] event in
            guard let self else { return event }
            return self.handle(event) ? nil : event
        }
    }

    private func removeKeyMonitor() {
        if let keyMonitor {
            NSEvent.removeMonitor(keyMonitor)
            self.keyMonitor = nil
        }
    }

    // Returns true if the event was consumed.
    private func handle(_ event: NSEvent) -> Bool {
        let cmd = event.modifierFlags.contains(.command)
        switch Int(event.keyCode) {
        case 53:  // escape
            hide()
            return true
        case 125:  // down arrow
            model.moveSelection(1)
            return true
        case 126:  // up arrow
            model.moveSelection(-1)
            return true
        case 36, 76:  // return / keypad enter — open all hits in a Finder window
            model.openResults()
            hide()
            return true
        case 8 where cmd:  // ⌘C — copy selected path
            model.copySelectedPath()
            return true
        default:
            return false
        }
    }
}

// An NSPanel that can become key despite being borderless/non-activating,
// so the search field receives typed input.
private final class KeyablePanel: NSPanel {
    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { false }
}
