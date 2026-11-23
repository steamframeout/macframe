import AppKit
import IOKit

/// The Steam Frame Wireless Adapter bridge, shipped inside this app (Contents/Resources/Frame Bridge.app): a small
/// Linux VM that takes the adapter (Realtek RTL8852CU, which macOS has no driver for) through USB passthrough. Its
/// default use passes the adapter on to SteamOS over USB/IP, where SteamOS's own driver, NetworkManager and Steam
/// drive it as on a Linux PC (guest /usr/lib/steamac/frame-adapter attaches it). It has its own window flow; the app
/// menu opens it, and while SteamOS runs a plugged-in adapter is handed over automatically ("FrameBridge share").
final class FrameBridgeLauncher: NSObject {
    static let shared = FrameBridgeLauncher()

    /// nil when this build has no bridge (built without FRAME_BRIDGE_APP)
    static var appURL: URL? {
        guard let url = Bundle.main.resourceURL?.appendingPathComponent("Frame Bridge.app"),
              FileManager.default.fileExists(atPath: url.path) else { return nil }
        return url
    }

    @objc func open(_ sender: Any?) {
        guard let url = Self.appURL else {
            NSSound.beep()
            return
        }
        let config = NSWorkspace.OpenConfiguration()
        config.activates = true
        NSWorkspace.shared.openApplication(at: url, configuration: config) { _, error in
            guard let error else { return }
            DispatchQueue.main.async { NSAlert(error: error).runModal() }
        }
    }

    // MARK: automatic hand-over to SteamOS

    static let vendorID = 0x28de
    static let productID = 0x2432
    private var notifyPort: IONotificationPortRef?
    private var arrivals: io_iterator_t = 0
    private var sharing: Process?
    private var lastAttempt = Date.distantPast

    /// Watches for the adapter for the life of this VM process (main thread); each arrival, and an adapter already
    /// plugged in at boot, runs the bridge's quiet hand-over. Off with STEAMAC_FRAME_AUTOSHARE=0.
    func startAutoShare() {
        guard Self.appURL != nil, notifyPort == nil,
              ProcessInfo.processInfo.environment["STEAMAC_FRAME_AUTOSHARE"] != "0" else { return }
        guard let port = IONotificationPortCreate(kIOMainPortDefault) else { return }
        notifyPort = port
        IONotificationPortSetDispatchQueue(port, .main)
        let match = IOServiceMatching("IOUSBHostDevice") as NSMutableDictionary
        match["idVendor"] = Self.vendorID
        match["idProduct"] = Self.productID
        let me = Unmanaged.passUnretained(self).toOpaque()
        let kr = IOServiceAddMatchingNotification(port, kIOFirstMatchNotification, match as CFDictionary, { ctx, iterator in
            guard let ctx else { return }
            Unmanaged<FrameBridgeLauncher>.fromOpaque(ctx).takeUnretainedValue().drain(iterator, share: true)
        }, me, &arrivals)
        guard kr == KERN_SUCCESS else {
            log("frame-bridge: cannot watch USB for the Steam Frame Wireless Adapter (\(kr))")
            return
        }
        // arms the notification; an adapter that is already there counts as an arrival
        drain(arrivals, share: true)
    }

    private func drain(_ iterator: io_iterator_t, share: Bool) {
        var found = false
        while case let service = IOIteratorNext(iterator), service != 0 {
            found = true
            IOObjectRelease(service)
        }
        if found && share { handOver() }
    }

    private func handOver() {
        guard let app = Self.appURL else { return }
        if let p = sharing, p.isRunning { return }
        // an adapter that re-enumerates (QEMU taking it over) arrives again: one attempt per half minute
        guard Date().timeIntervalSince(lastAttempt) > 30 else { return }
        lastAttempt = Date()
        let p = Process()
        p.executableURL = app.appendingPathComponent("Contents/MacOS/FrameBridge")
        p.arguments = ["share"]
        p.standardInput = FileHandle.nullDevice
        p.standardOutput = FileHandle.nullDevice
        p.standardError = FileHandle.nullDevice
        p.terminationHandler = { proc in
            log("frame-bridge: hand-over to SteamOS finished (status \(proc.terminationStatus))")
        }
        do {
            try p.run()
            sharing = p
            log("frame-bridge: Steam Frame Wireless Adapter plugged in: handing it to SteamOS")
        } catch {
            log("frame-bridge: cannot start Frame Bridge: \(error)")
        }
    }
}
