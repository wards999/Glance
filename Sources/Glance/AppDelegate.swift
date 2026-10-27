import AppKit

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var viewer: ViewerWindowController?
    private var pendingURLs: [URL] = []
    private var didLaunch = false

    func applicationWillFinishLaunching(_ notification: Notification) {
        // Needed when run unbundled (`swift run`); harmless inside Glance.app.
        NSApp.setActivationPolicy(.regular)
        NSApp.mainMenu = MainMenu.build()
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        didLaunch = true
        // Paths given on the command line, e.g. `swift run -c release Glance ~/Pictures/big.tif`.
        let fileManager = FileManager.default
        let arguments = CommandLine.arguments.dropFirst().filter { fileManager.fileExists(atPath: $0) }
        pendingURLs += arguments.map { URL(fileURLWithPath: $0) }

        let viewer = showViewer()
        if pendingURLs.isEmpty {
            viewer.openAtLaunch()
        } else {
            viewer.open(pendingURLs)
            pendingURLs = []
        }
        NSApp.activate(ignoringOtherApps: true)
    }

    // Finder "Open With", drag onto the Dock icon, `open -a Glance file.jpg`.
    func application(_ application: NSApplication, open urls: [URL]) {
        if didLaunch {
            showViewer().open(urls)
        } else {
            pendingURLs += urls
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }

    @objc func showSettings(_ sender: Any?) {
        SettingsWindowController.shared.showWindow(nil)
    }

    /// The standard About panel (name, version and icon from Info.plist), with what Glance is.
    @objc func showAbout(_ sender: Any?) {
        let credits = NSMutableAttributedString(
            string: "Fast picture, PDF and video viewer, modeled on JPEGView.\n\n"
                + (MPV.available ? "Videos are played by libmpv and FFmpeg (GPL and LGPL).\n" : ""),
            attributes: [.font: NSFont.systemFont(ofSize: NSFont.smallSystemFontSize), .foregroundColor: NSColor.labelColor])
        credits.append(NSAttributedString(string: "github.com/wards999/Glance", attributes: [
            .font: NSFont.systemFont(ofSize: NSFont.smallSystemFontSize),
            .link: URL(string: "https://github.com/wards999/Glance")!,
        ]))
        let centred = NSMutableParagraphStyle()
        centred.alignment = .center
        credits.addAttribute(.paragraphStyle, value: centred, range: NSRange(location: 0, length: credits.length))
        NSApp.orderFrontStandardAboutPanel(options: [.credits: credits])
        NSApp.activate(ignoringOtherApps: true)
    }

    @objc func makeGlanceDefault(_ sender: Any?) {
        DefaultApp.makeDefault()
    }

    @discardableResult
    private func showViewer() -> ViewerWindowController {
        if let viewer { return viewer }
        let controller = ViewerWindowController()
        controller.showAtLaunch()
        viewer = controller
        return controller
    }
}
