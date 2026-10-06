import AppKit
import Combine
import SwiftUI

// A non-activating floating panel (PLAN.md section 7): centered, blurred,
// escape to dismiss, focus returns to the previous app on close. Hosts the
// SwiftUI SearchView, owns the keyboard navigation monitor, and grows the
// window to fit the results (anchored at its top edge).
@MainActor
final class PanelController {
    private let panel: KeyablePanel
    private let model: SearchModel
    private var keyMonitor: Any?
    private var cancellables = Set<AnyCancellable>()

    // Screen position of the panel's top-left while shown. Fixed on show so
    // the window grows downward as results arrive instead of drifting.
    private var anchorX: CGFloat = 0
    private var anchorTop: CGFloat = 0

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
        panel.isMovableByWindowBackground = false  // keep the top-edge anchor stable
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

        // Actions that open something close the panel.
        model.onDismiss = { [weak self] in self?.hide() }

        // Grow/shrink the window to fit the results as they change, keeping
        // the top edge anchored. Deferred to the next run loop so the model's
        // @Published values have settled.
        Publishers.CombineLatest(model.$results, model.$status)
            .receive(on: RunLoop.main)
            .sink { [weak self] results, status in
                guard let self, self.panel.isVisible else { return }
                self.applyHeight(PanelMetrics.totalHeight(
                    rowCount: results.count, hasStatus: !status.isEmpty))
            }
            .store(in: &cancellables)
    }

    var isVisible: Bool { panel.isVisible }

    func toggle() {
        isVisible ? hide() : show()
    }

    func show() {
        model.reset()
        if let screen = NSScreen.main {
            let visible = screen.visibleFrame
            anchorX = visible.midX - PanelMetrics.width / 2
            // Top edge a little above center, Spotlight-style.
            anchorTop = visible.midY + visible.height * 0.18
        }
        applyHeight(PanelMetrics.totalHeight(rowCount: 0, hasStatus: false))
        installKeyMonitor()
        NSApp.activate(ignoringOtherApps: true)
        panel.makeKeyAndOrderFront(nil)
    }

    func hide() {
        removeKeyMonitor()
        panel.orderOut(nil)
    }

    // Resize to `height`, keeping the top-left corner pinned at the anchor.
    private func applyHeight(_ height: CGFloat) {
        let frame = NSRect(x: anchorX, y: anchorTop - height,
                           width: PanelMetrics.width, height: height)
        panel.setFrame(frame, display: true, animate: false)
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
        case 36, 76:  // return / keypad enter
            if cmd {
                model.openResults()     // ⌘Enter: show all hits in Finder
            } else {
                model.revealSelected()  // Enter: reveal the selected file in place
            }
            return true  // the model dismisses on success
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
