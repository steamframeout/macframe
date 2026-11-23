import AppKit

/// The Steam Frame Wireless Adapter bridge, shipped inside this app (Contents/Resources/Frame Bridge.app): a small
/// Linux VM that drives the adapter (Realtek RTL8852CU, which macOS has no driver for) through USB passthrough and
/// links the headset's dedicated 6 GHz network to this Mac. It has its own window flow (test, set up from the
/// headset, connect); the app menu opens it.
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
}
