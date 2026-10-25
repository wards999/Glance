import AppKit

/// The menu bar and the right-click menu, built from the same sections so they can't drift apart.
/// Single-key shortcuts (space, arrows, R, I, …) live in
/// `ViewerWindowController.imageView(_:handleKeyDown:)`; menus only carry ⌘ shortcuts so they can't
/// swallow typing in a dialog.
enum MainMenu {
    private typealias C = ViewerWindowController
    private static let appName = ProcessInfo.processInfo.processName

    static func build() -> NSMenu {
        let main = NSMenu()
        main.addItem(submenu(appName, appItems()))
        main.addItem(submenu("File", fileItems() + [.separator(), item("Close Window", #selector(NSWindow.performClose(_:)), "w")]))
        main.addItem(submenu("Edit", [item("Copy", #selector(C.copy(_:)), "c")]))
        main.addItem(submenu("View", viewItems()))
        main.addItem(submenu("Image", imageItems()))
        main.addItem(submenu("Go", goItems()))
        let window = submenu("Window", [
            item("Minimize", #selector(NSWindow.performMiniaturize(_:)), "m"),
            item("Zoom", #selector(NSWindow.performZoom(_:)), ""),
        ])
        main.addItem(window)
        NSApp.windowsMenu = window.submenu
        return main
    }

    /// Everything, reachable without the menu bar (which is hidden in full screen).
    static func contextMenu() -> NSMenu {
        let menu = NSMenu()
        let items: [NSMenuItem] = [
            item("Next Image", #selector(C.nextImage(_:)), "]"),
            item("Previous Image", #selector(C.previousImage(_:)), "["),
            .separator(),
            item("Enter Full Screen", #selector(C.toggleFullScreenMode(_:)), "f", [.command, .control]),
            item("Show Info", #selector(C.toggleInfo(_:)), "i"),
            item("Start Slideshow", #selector(C.toggleSlideshow(_:)), "p", [.command, .option]),
            .separator(),
            submenu("Zoom", viewItems().filter { $0.action != #selector(C.toggleFullScreenMode(_:)) && $0.action != #selector(C.toggleInfo(_:)) }),
            submenu("Image", imageItems()),
            submenu("Go", goItems()),
            submenu("File", fileItems() + [.separator(), item("Copy", #selector(C.copy(_:)), "c")]),
            .separator(),
            item("Settings…", #selector(AppDelegate.showSettings(_:)), ","),
            item("Quit \(appName)", #selector(NSApplication.terminate(_:)), "q"),
        ]
        items.forEach { menu.addItem($0) }
        return menu
    }

    // MARK: - Sections (each call returns fresh items: a menu item can only live in one menu)

    private static func appItems() -> [NSMenuItem] {
        [
            item("About \(appName)", #selector(NSApplication.orderFrontStandardAboutPanel(_:)), ""),
            .separator(),
            item("Settings…", #selector(AppDelegate.showSettings(_:)), ","),
            .separator(),
            item("Hide \(appName)", #selector(NSApplication.hide(_:)), "h"),
            item("Hide Others", #selector(NSApplication.hideOtherApplications(_:)), "h", [.command, .option]),
            item("Show All", #selector(NSApplication.unhideAllApplications(_:)), ""),
            .separator(),
            item("Quit \(appName)", #selector(NSApplication.terminate(_:)), "q"),
        ]
    }

    private static func fileItems() -> [NSMenuItem] {
        [
            item("Open…", #selector(C.openDocument(_:)), "o"),
            .separator(),
            item("Save", #selector(C.saveImage(_:)), "s"),
            item("Save As…", #selector(C.saveImageAs(_:)), "s", [.command, .shift]),
            .separator(),
            item("Reveal in Finder", #selector(C.revealInFinder(_:)), "r", [.command, .shift]),
            item("Set as Desktop Picture", #selector(C.setAsDesktopPicture(_:)), ""),
            .separator(),
            item("Move to Trash", #selector(C.moveToTrash(_:)), "\u{8}"),
        ]
    }

    private static func viewItems() -> [NSMenuItem] {
        [
            item("Zoom In", #selector(C.zoomIn(_:)), "="),
            item("Zoom Out", #selector(C.zoomOut(_:)), "-"),
            item("Fit to Window", #selector(C.zoomToFit(_:)), "0"),
            item("Actual Size", #selector(C.zoomActualSize(_:)), "1"),
            item("Fill Window", #selector(C.zoomToFill(_:)), "2"),
            item("Zoom to Selection", #selector(C.zoomToSelection(_:)), "3"),
            item("Enlarge Small Images to Fit", #selector(C.toggleEnlargeSmall(_:)), ""),
            .separator(),
            item("Show Info", #selector(C.toggleInfo(_:)), "i"),
            .separator(),
            item("Enter Full Screen", #selector(C.toggleFullScreenMode(_:)), "f", [.command, .control]),
        ]
    }

    private static func imageItems() -> [NSMenuItem] {
        [
            item("Rotate Left", #selector(C.rotateLeft(_:)), "l"),
            item("Rotate Right", #selector(C.rotateRight(_:)), "r"),
            item("Flip Horizontal", #selector(C.flipHorizontal(_:)), ""),
            item("Flip Vertical", #selector(C.flipVertical(_:)), ""),
            .separator(),
            item("Crop to Selection", #selector(C.cropToSelection(_:)), "k"),
            .separator(),
            item("Auto Correction", #selector(C.toggleAutoCorrect(_:)), ""),
            item("Adjustments…", #selector(C.showAdjustments(_:)), "c", [.command, .option]),
            item("Keep Adjustments for Next Images", #selector(C.toggleKeepAdjustments(_:)), ""),
            item("Revert Edits", #selector(C.resetEdits(_:)), ""),
        ]
    }

    private static func goItems() -> [NSMenuItem] {
        let sortItems = SortOrder.allCases.map { order in
            item(order.title, #selector(C.setSortOrder(_:)), "", tag: order.rawValue)
        }
        let intervalItems = Settings.slideshowIntervals.map { seconds in
            item("\(Int(seconds)) Seconds", #selector(C.setSlideshowInterval(_:)), "", tag: Int(seconds))
        }
        return [
            item("Next Image", #selector(C.nextImage(_:)), "]"),
            item("Previous Image", #selector(C.previousImage(_:)), "["),
            item("First Image", #selector(C.firstImage(_:)), ""),
            item("Last Image", #selector(C.lastImage(_:)), ""),
            .separator(),
            item("Next Page", #selector(C.nextPage(_:)), "]", [.command, .option]),
            item("Previous Page", #selector(C.previousPage(_:)), "[", [.command, .option]),
            .separator(),
            submenu("Sort By", sortItems + [.separator(), item("Descending", #selector(C.toggleSortDescending(_:)), "")]),
            item("Loop Through Folder", #selector(C.toggleLoop(_:)), ""),
            .separator(),
            item("Start Slideshow", #selector(C.toggleSlideshow(_:)), "p", [.command, .option]),
            submenu("Slideshow Interval", intervalItems),
        ]
    }

    private static func item(_ title: String, _ action: Selector?, _ key: String, _ modifiers: NSEvent.ModifierFlags = [.command], tag: Int = 0) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: action, keyEquivalent: key)
        item.keyEquivalentModifierMask = modifiers
        item.tag = tag
        return item
    }

    private static func submenu(_ title: String, _ items: [NSMenuItem]) -> NSMenuItem {
        let parent = NSMenuItem(title: title, action: nil, keyEquivalent: "")
        let menu = NSMenu(title: title)
        items.forEach { menu.addItem($0) }
        parent.submenu = menu
        return parent
    }
}
