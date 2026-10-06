import AppKit
import Carbon.HIToolbox

// A single global hotkey via Carbon's RegisterEventHotKey. This path needs
// no Accessibility permission (unlike an event tap), which is why PLAN.md
// section 7 picks it. Default chord: Option-Space.
final class HotKey {
    private var ref: EventHotKeyRef?
    private var handler: EventHandlerRef?
    private let onFire: () -> Void

    // A process-unique id so the Carbon C callback can find us.
    private static var instances: [UInt32: HotKey] = [:]
    private static var nextID: UInt32 = 1
    private let id: UInt32

    init?(keyCode: UInt32 = UInt32(kVK_Space),
          modifiers: UInt32 = UInt32(optionKey),
          onFire: @escaping () -> Void) {
        self.onFire = onFire
        self.id = HotKey.nextID
        HotKey.nextID += 1
        HotKey.instances[id] = self

        var eventType = EventTypeSpec(eventClass: OSType(kEventClassKeyboard),
                                      eventKind: UInt32(kEventHotKeyPressed))
        let installed = InstallEventHandler(
            GetApplicationEventTarget(),
            { _, event, _ -> OSStatus in
                var hkID = EventHotKeyID()
                GetEventParameter(event, EventParamName(kEventParamDirectObject),
                                  EventParamType(typeEventHotKeyID), nil,
                                  MemoryLayout<EventHotKeyID>.size, nil, &hkID)
                HotKey.instances[hkID.id]?.onFire()
                return noErr
            },
            1, &eventType, nil, &handler)
        guard installed == noErr else { return nil }

        let hkID = EventHotKeyID(signature: OSType(0x4D41_4353 /* 'MACS' */),
                                 id: id)
        let registered = RegisterEventHotKey(keyCode, modifiers, hkID,
                                             GetApplicationEventTarget(),
                                             0, &ref)
        guard registered == noErr else { return nil }
    }

    deinit {
        if let ref { UnregisterEventHotKey(ref) }
        if let handler { RemoveEventHandler(handler) }
        HotKey.instances[id] = nil
    }
}
