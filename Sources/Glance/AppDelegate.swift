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
