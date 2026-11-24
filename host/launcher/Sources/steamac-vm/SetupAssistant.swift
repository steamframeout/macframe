import AppKit
import IOKit
import SwiftUI

/// MacFrame's setup assistant: the first start of the app (no SteamOS disk yet) walks through
///   Welcome (this Mac: chip, macOS, memory, free space, Applications folder)
///   SteamOS (Valve's license, then the official image downloaded and written: CreateDiskModel / DiskCreator;
///            or an existing disk)
///   Steam Frame (the Wireless Adapter, detected live, or Wi-Fi)
///   Ready (what happens next: SteamOS's first start, signing in to Steam, pairing the headset)
/// and returns the disk; the supervisor then boots it (FirstRun). App menu "Steam Frame Setup…" opens the last two
/// steps again at any time.
@MainActor
final class SetupAssistant: NSObject, NSWindowDelegate {
    enum Step: Int, CaseIterable, Identifiable {
        case welcome, steamos, frame, ready
        var id: Int { rawValue }
        var title: String {
            switch self {
            case .welcome: String(localized: "Welcome")
            case .steamos: String(localized: "Install SteamOS")
            case .frame: String(localized: "Connect Steam Frame")
            case .ready: String(localized: "Start Playing")
            }
        }
        var symbol: String {
            switch self {
            case .welcome: "hand.wave"
            case .steamos: "externaldrive.badge.plus"
            case .frame: "visionpro"
            case .ready: "play.circle"
            }
        }
    }

    final class Model: ObservableObject {
        @Published var step: Step
        @Published var diskPath: String?
        @Published var adapterPresent = FrameAdapter.present()
        @Published var useAdapter = true
        @Published var showOptions = false
        let disk: CreateDiskModel
        let settings: LauncherSettings
        /// The first-start assistant (all steps) or Steam Frame Setup (frame + ready, no disk step)
        let firstRun: Bool
        var finish: (Bool) -> Void = { _ in }
        private var timer: Timer?

        init(settings: LauncherSettings, firstRun: Bool) {
            self.settings = settings
            self.firstRun = firstRun
            step = firstRun ? .welcome : .frame
            disk = CreateDiskModel(settings: settings)
            disk.homeGiB = 256   // sparse: VR games are large, and only what is written takes space
            setUp()
        }
        private func setUp() {
            disk.onFinish = { [weak self] r in self?.diskPath = r.path }
            timer = Timer.scheduledTimer(withTimeInterval: 1.5, repeats: true) { [weak self] _ in
                guard let self else { return }
                let now = FrameAdapter.present()
                if now != self.adapterPresent { self.adapterPresent = now }
            }
        }
        deinit { timer?.invalidate() }

        var steps: [Step] { firstRun ? Step.allCases : [.frame, .ready] }

        var canContinue: Bool {
            switch step {
            case .welcome: true
            case .steamos: diskPath != nil
            case .frame, .ready: true
            }
        }

        func next() {
            guard let i = steps.firstIndex(of: step) else { return }
            if i + 1 < steps.count { step = steps[i + 1] } else { finish(true) }
        }

        func back() {
            guard let i = steps.firstIndex(of: step), i > 0, !disk.running else { return }
            step = steps[i - 1]
        }
    }

    private let window: NSWindow
    private let model: Model
    private static var current: SetupAssistant?

    private init(settings: LauncherSettings, firstRun: Bool) {
        model = Model(settings: settings, firstRun: firstRun)
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 820, height: 580),
                          styleMask: [.titled, .closable, .fullSizeContentView], backing: .buffered, defer: false)
        super.init()
        window.title = firstRun ? String(localized: "Set Up MacFrame") : String(localized: "Steam Frame Setup")
        window.titlebarAppearsTransparent = true
        window.isReleasedWhenClosed = false
        window.delegate = self
        window.contentView = NSHostingView(rootView: AssistantView(model: model))
    }

    /// First start: blocks until the user finishes (the disk to boot) or closes the window (nil = quit).
    static func runModal(settings: LauncherSettings) -> String? {
        let a = SetupAssistant(settings: settings, firstRun: true)
        current = a
        var done = false
        a.model.finish = { ok in
            done = ok
            NSApp.stopModal(withCode: ok ? .OK : .cancel)
        }
        a.window.center()
        NSApp.activate()
        let code = NSApp.runModal(for: a.window)
        a.window.orderOut(nil)
        current = nil
        guard code == .OK, done, let path = a.model.diskPath else { return nil }
        log("setup assistant: finished (disk \(path), \(a.model.useAdapter ? "wireless adapter" : "Wi-Fi"))")
        return path
    }

    /// App menu "Steam Frame Setup…": the headset steps again, any time.
    static func showFrameSetup() {
        if let c = current { c.window.makeKeyAndOrderFront(nil); return }
        let a = SetupAssistant(settings: LauncherSettings.shared, firstRun: false)
        a.model.finish = { [weak a] _ in a?.window.close() }
        current = a
        a.window.center()
        NSApp.activate()
        a.window.makeKeyAndOrderFront(nil)
    }

    /// --selftest-setup: every step of both forms as <dir>/setup-<n>-<step>.png, then exit.
    static func selfTest(dir: String) -> Never {
        let app = NSApplication.shared
        app.setActivationPolicy(.regular)
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        var n = 0
        for firstRun in [true, false] {
            let a = SetupAssistant(settings: LauncherSettings.shared, firstRun: firstRun)
            a.window.center()
            a.window.makeKeyAndOrderFront(nil)
            for step in a.model.steps {
                a.model.step = step
                RunLoop.main.run(until: Date().addingTimeInterval(0.8))
                n += 1
                let name = "\(dir)/setup-\(n)-\(firstRun ? "first" : "frame")-\(step).png"
                ReportControl.dump(a.window, to: name)
                print("selftest-setup: \(name)")
            }
            a.window.orderOut(nil)
        }
        print("selftest-setup: PASS")
        exit(0)
    }

    func windowShouldClose(_ sender: NSWindow) -> Bool {
        if model.disk.running {
            let alert = NSAlert()
            alert.messageText = String(localized: "Stop installing SteamOS?")
            alert.informativeText = String(localized: "What was downloaded so far is kept: installing again continues from there.")
            alert.addButton(withTitle: String(localized: "Keep Installing"))
            alert.addButton(withTitle: String(localized: "Stop and Quit"))
            guard alert.runModal() == .alertSecondButtonReturn else { return false }
            model.disk.cancel()
        }
        return true
    }

    func windowWillClose(_ notification: Notification) {
        if model.firstRun {
            NSApp.stopModal(withCode: .cancel)
        } else {
            Self.current = nil
        }
    }
}

/// Target of the app menu's "Steam Frame Setup…".
final class FrameSetupMenu: NSObject {
    static let shared = FrameSetupMenu()
    @objc func open(_ sender: Any?) { MainActor.assumeIsolated { SetupAssistant.showFrameSetup() } }
}

/// The Steam Frame Wireless Adapter (Realtek RTL8852CU in Valve's USB id 28de:2432) on this Mac's USB.
enum FrameAdapter {
    static func present() -> Bool {
        let match = IOServiceMatching("IOUSBHostDevice") as NSMutableDictionary
        match["idVendor"] = FrameBridgeLauncher.vendorID
        match["idProduct"] = FrameBridgeLauncher.productID
        var it: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, match as CFDictionary, &it) == KERN_SUCCESS else { return false }
        defer { IOObjectRelease(it) }
        var found = false
        while case let s = IOIteratorNext(it), s != 0 {
            found = true
            IOObjectRelease(s)
        }
        return found
    }
}

// MARK: - views

private struct AssistantView: View {
    @ObservedObject var model: SetupAssistant.Model

    var body: some View {
        HStack(spacing: 0) {
            sidebar
            Divider()
            VStack(spacing: 0) {
                // no ScrollView: every step fits the window (SteamOS's options are a sheet)
                Group {
                    switch model.step {
                    case .welcome: WelcomeStep(model: model)
                    case .steamos: SteamOSStep(model: model, disk: model.disk)
                    case .frame: FrameStep(model: model)
                    case .ready: ReadyStep(model: model)
                    }
                }
                .padding(.horizontal, 36)
                .padding(.top, 44)
                .padding(.bottom, 20)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                Divider()
                HStack {
                    if model.steps.first != model.step {
                        Button("Back") { model.back() }.disabled(model.disk.running)
                    }
                    Spacer()
                    Button(model.step == .ready ? (model.firstRun ? String(localized: "Start SteamOS") : String(localized: "Done"))
                           : String(localized: "Continue")) { model.next() }
                        .keyboardShortcut(.defaultAction)
                        .disabled(!model.canContinue)
                        .controlSize(.large)
                }
                .padding(16)
            }
        }
        .frame(width: 820, height: 580)
    }

    private var sidebar: some View {
        VStack(alignment: .leading, spacing: 18) {
            HStack(spacing: 10) {
                Image(nsImage: NSApp.applicationIconImage).resizable().frame(width: 40, height: 40)
                Text(verbatim: "MacFrame").font(.title3.weight(.semibold))
            }
            .padding(.bottom, 10)
            ForEach(model.steps) { s in
                let index = model.steps.firstIndex(of: s) ?? 0
                let currentIndex = model.steps.firstIndex(of: model.step) ?? 0
                HStack(spacing: 10) {
                    Image(systemName: index < currentIndex ? "checkmark.circle.fill" : s.symbol)
                        .foregroundStyle(index < currentIndex ? Color.green : s == model.step ? Color.accentColor : Color.secondary)
                        .frame(width: 20)
                    Text(verbatim: s.title)
                        .fontWeight(s == model.step ? .semibold : .regular)
                        .foregroundStyle(s == model.step ? Color.primary : Color.secondary)
                }
            }
            Spacer()
        }
        .padding(.top, 44)
        .padding(.horizontal, 20)
        .frame(width: 210, alignment: .leading)
        .frame(maxHeight: .infinity, alignment: .top)
        .background(.regularMaterial)
    }
}

private struct StepHeader: View {
    let title: String
    let text: String
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(verbatim: title).font(.largeTitle.weight(.bold))
            Text(verbatim: text).font(.title3).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
        .padding(.bottom, 18)
    }
}

private struct CheckRow: View {
    enum State { case ok, warn, fail }
    let state: State
    let title: String
    let detail: String
    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: state == .ok ? "checkmark.circle.fill" : state == .warn ? "exclamationmark.triangle.fill" : "xmark.octagon.fill")
                .foregroundStyle(state == .ok ? Color.green : state == .warn ? Color.orange : Color.red)
                .font(.title3)
            VStack(alignment: .leading, spacing: 2) {
                Text(verbatim: title).fontWeight(.medium)
                Text(verbatim: detail).font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
        }
    }
}

// MARK: welcome

private struct WelcomeStep: View {
    @ObservedObject var model: SetupAssistant.Model

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            StepHeader(title: String(localized: "Welcome to MacFrame"),
                       text: String(localized: "MacFrame runs SteamOS on this Mac, plays your Steam library, and streams SteamVR games to Steam Frame the way a gaming PC does. Setup takes a few minutes plus a 4.5 GB download."))
            ForEach(Array(checks.enumerated()), id: \.offset) { _, c in CheckRow(state: c.0, title: c.1, detail: c.2) }
        }
    }

    private var checks: [(CheckRow.State, String, String)] {
        var out: [(CheckRow.State, String, String)] = []
        let mem = Double(ProcessInfo.processInfo.physicalMemory) / 1_073_741_824
        let gib = String(format: "%.0f", mem)
        out.append(mem >= 15.5
                   ? (.ok, String(localized: "\(gib) GB of memory"), String(localized: "Enough for SteamOS and VR games."))
                   : (.warn, String(localized: "\(gib) GB of memory"), String(localized: "16 GB or more is recommended; SteamVR may run short of memory.")))
        let v = ProcessInfo.processInfo.operatingSystemVersion
        out.append((.ok, String(localized: "macOS \(v.majorVersion).\(v.minorVersion)"), String(localized: "Supported.")))
        if let free = freeGB {
            let f = String(format: "%.0f", free)
            out.append(free >= 40
                       ? (.ok, String(localized: "\(f) GB free"), String(localized: "SteamOS needs about 20 GB to start, plus room for your games."))
                       : (.warn, String(localized: "\(f) GB free"), String(localized: "SteamOS needs about 20 GB to start, plus room for your games. Free up space before installing large games.")))
        }
        let path = Bundle.main.bundlePath
        if path.contains("/AppTranslocation/") || !path.hasPrefix("/Applications/") {
            out.append((.warn, String(localized: "Not in the Applications folder"),
                        String(localized: "Quit, drag MacFrame into Applications and open it from there: the Steam Frame Wireless Adapter only works from there.")))
        } else {
            out.append((.ok, String(localized: "Installed in Applications"), String(localized: "Ready for the Steam Frame Wireless Adapter.")))
        }
        return out
    }

    private var freeGB: Double? {
        let url = URL(fileURLWithPath: NSHomeDirectory())
        guard let v = try? url.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]),
              let bytes = v.volumeAvailableCapacityForImportantUsage else { return nil }
        return Double(bytes) / 1e9
    }
}

// MARK: SteamOS

private struct SteamOSStep: View {
    @ObservedObject var model: SetupAssistant.Model
    @ObservedObject var disk: CreateDiskModel

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            StepHeader(title: String(localized: "Install SteamOS"),
                       text: String(localized: "MacFrame downloads the official SteamOS from Valve, checks Valve's signature and builds the SteamOS disk on this Mac."))
            if let path = model.diskPath {
                CheckRow(state: .ok, title: String(localized: "SteamOS is installed"),
                         detail: (path as NSString).abbreviatingWithTildeInPath)
            } else {
                license
                progress
                HStack(spacing: 12) {
                    if disk.running {
                        Button("Stop") { disk.cancel() }
                    } else {
                        Button(disk.interrupted ? String(localized: "Resume Installing") : String(localized: "Install SteamOS")) { disk.start() }
                            .controlSize(.large)
                            .buttonStyle(.borderedProminent)
                            .disabled(!disk.licenseAccepted || disk.pathExists)
                        Button("Use an Existing SteamOS Disk…") { chooseExisting() }
                    }
                }
                Button("Options…") { model.showOptions = true }
                    .buttonStyle(.link)
                    .disabled(disk.running)
                    .sheet(isPresented: $model.showOptions) {
                        VStack(alignment: .leading, spacing: 0) {
                            Text("SteamOS Options").font(.title2.weight(.semibold)).padding([.horizontal, .top], 20)
                            options
                            HStack {
                                Spacer()
                                Button("Done") { model.showOptions = false }.keyboardShortcut(.defaultAction)
                            }
                            .padding(16)
                        }
                        .frame(width: 540)
                    }
            }
        }
    }

    private var license: some View {
        Toggle(isOn: $disk.licenseAccepted) {
            VStack(alignment: .leading, spacing: 3) {
                Text("I accept Valve's SteamOS license and the Steam Subscriber Agreement")
                Text(verbatim: SteamOSLicense.localizedSummary)
                    .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                HStack(spacing: 12) {
                    Link("SteamOS End User License Agreement", destination: SteamOSLicense.eulaURL)
                    Link("Steam Subscriber Agreement", destination: SteamOSLicense.ssaURL)
                }
                .font(.caption)
            }
        }
        .toggleStyle(.checkbox)
        .disabled(disk.running)
    }

    @ViewBuilder private var progress: some View {
        if disk.running || disk.status != nil || disk.error != nil {
            VStack(alignment: .leading, spacing: 6) {
                Text(verbatim: disk.error != nil ? String(localized: "Installing failed")
                     : disk.interrupted ? String(localized: "Paused — installing again continues where it stopped")
                     : disk.status?.localizedTitle ?? String(localized: "Starting…"))
                    .font(.headline)
                ProgressView(value: disk.status?.fraction ?? 0)
                Text(verbatim: disk.error ?? disk.status?.localizedDetail ?? "")
                    .font(.caption).foregroundStyle(disk.error != nil ? Color.red : Color.secondary)
                    .fixedSize(horizontal: false, vertical: true).textSelection(.enabled)
            }
            .padding(14)
            .background(RoundedRectangle(cornerRadius: 10).fill(Color.secondary.opacity(0.08)))
        }
    }

    private var options: some View {
        Form {
            Stepper(value: $disk.homeGiB, in: 32...4096, step: 32) {
                HStack {
                    Text("Space for games (grows as needed)")
                    Spacer()
                    Text("\(disk.homeGiB) GB").monospacedDigit()
                }
            }
            HStack {
                Text("Location")
                Spacer()
                Text(verbatim: (disk.path as NSString).abbreviatingWithTildeInPath)
                    .lineLimit(1).truncationMode(.middle).foregroundStyle(disk.pathExists ? Color.red : Color.secondary)
                Button("Choose…") { chooseLocation() }
            }
            Text(verbatim: disk.freeSpace).font(.caption).foregroundStyle(.secondary)
            Picker("SteamOS branch", selection: $disk.branch) {
                ForEach(DiskCreator.branches, id: \.self) { Text(verbatim: $0).tag($0) }
            }
            SteamClientPicker(settings: model.settings)
        }
        .formStyle(.grouped)
        .frame(height: 300)
    }

    private func chooseLocation() {
        let panel = NSSavePanel()
        panel.title = String(localized: "Location of the new SteamOS disk")
        panel.nameFieldStringValue = (disk.path as NSString).lastPathComponent
        panel.directoryURL = URL(fileURLWithPath: (disk.path as NSString).deletingLastPathComponent)
        panel.canCreateDirectories = true
        if panel.runModal() == .OK, let url = panel.url { disk.path = CreateDiskModel.freePath(url.path) }
    }

    private func chooseExisting() {
        let panel = NSOpenPanel()
        AppBundle.configureDiskPanel(panel)
        panel.message = String(localized: "Raw GPT disk image (e.g. steamos.img). It stays where it is.")
        if panel.runModal() == .OK, let url = panel.url, AppBundle.isDiskImage(url.path) { model.diskPath = url.path }
    }
}

// MARK: Steam Frame

private struct FrameStep: View {
    @ObservedObject var model: SetupAssistant.Model

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            StepHeader(title: String(localized: "Connect Steam Frame"),
                       text: String(localized: "Choose how the headset connects to this Mac. You can switch at any time."))
            choice(adapter: true, symbol: "cable.connector",
                   title: String(localized: "Steam Frame Wireless Adapter (recommended)"),
                   text: String(localized: "The adapter gives the headset its own fast link. Plug it into this Mac: MacFrame hands it to SteamOS whenever SteamOS runs, and Steam uses it exactly as on a PC."))
            if model.useAdapter {
                CheckRow(state: model.adapterPresent ? .ok : .warn,
                         title: model.adapterPresent ? String(localized: "Adapter connected") : String(localized: "Adapter not plugged in"),
                         detail: model.adapterPresent ? String(localized: "It goes to SteamOS when SteamOS starts. macOS may ask for your password the first time.")
                                                      : String(localized: "Plug it into a USB-C or USB 3 port on this Mac (not a hub, if you can). You can also do this later."))
                    .padding(.leading, 34)
            }
            choice(adapter: false, symbol: "wifi",
                   title: String(localized: "Wi-Fi"),
                   text: String(localized: "No adapter: the headset streams over your home network. Best with this Mac on Ethernet and the headset near a Wi-Fi 6 or 6E router."))
            Text("When macOS asks whether MacFrame may find devices on your local network, choose Allow: that is how the headset finds Steam.")
                .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
    }

    private func choice(adapter: Bool, symbol: String, title: String, text: String) -> some View {
        Button { model.useAdapter = adapter } label: {
            HStack(alignment: .top, spacing: 14) {
                Image(systemName: model.useAdapter == adapter ? "largecircle.fill.circle" : "circle")
                    .foregroundStyle(model.useAdapter == adapter ? Color.accentColor : Color.secondary)
                    .font(.title3)
                Image(systemName: symbol).font(.title2).frame(width: 30)
                VStack(alignment: .leading, spacing: 3) {
                    Text(verbatim: title).fontWeight(.semibold)
                    Text(verbatim: text).font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                }
                Spacer(minLength: 0)
            }
            .padding(14)
            .contentShape(Rectangle())
            .background(RoundedRectangle(cornerRadius: 10)
                .strokeBorder(model.useAdapter == adapter ? Color.accentColor : Color.secondary.opacity(0.3), lineWidth: 1.5))
        }
        .buttonStyle(.plain)
    }
}

// MARK: ready

private struct ReadyStep: View {
    @ObservedObject var model: SetupAssistant.Model

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            StepHeader(title: model.firstRun ? String(localized: "You're all set") : String(localized: "Pair your Steam Frame"),
                       text: model.firstRun ? String(localized: "Here is what happens when SteamOS starts.")
                                            : String(localized: "With SteamOS running in MacFrame:"))
            if model.firstRun {
                item(1, String(localized: "SteamOS finishes setting itself up"),
                     String(localized: "The first start takes a few minutes. MacFrame shows the progress."))
                item(2, String(localized: "Sign in to Steam"),
                     String(localized: "Use your Steam account, the same one as on the headset."))
            }
            item(model.firstRun ? 3 : 1, String(localized: "Pair the headset"),
                 model.useAdapter
                 ? String(localized: "With the adapter plugged in, open Steam's Steam Frame settings in SteamOS and pair, just like on a PC. Put the headset on and confirm there.")
                 : String(localized: "Put the headset on, on the same network as this Mac: it lists this Mac's Steam as a PC to stream from. Choose it and confirm the code."))
            item(model.firstRun ? 4 : 2, String(localized: "Play"),
                 String(localized: "Start SteamVR games from your library in the headset; they run in SteamOS on this Mac and stream to Steam Frame."))
        }
    }

    private func item(_ n: Int, _ title: String, _ text: String) -> some View {
        HStack(alignment: .top, spacing: 14) {
            Text(verbatim: "\(n)")
                .font(.headline).foregroundStyle(.white)
                .frame(width: 28, height: 28)
                .background(Circle().fill(Color.accentColor))
            VStack(alignment: .leading, spacing: 3) {
                Text(verbatim: title).fontWeight(.semibold)
                Text(verbatim: text).font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
        }
    }
}
