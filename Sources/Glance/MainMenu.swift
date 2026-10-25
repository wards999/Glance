import AppKit

/// The menu bar. Single-key shortcuts (space, arrows, R, I, …) live in
/// `ViewerWindowController.imageView(_:handleKeyDown:)`; menus only carry ⌘ shortcuts so they can't
/// swallow typing in a dialog.
enum MainMenu {
    static func build() -> NSMenu {
        typealias C = ViewerWindowController
        let appName = ProcessInfo.processInfo.processName
        let main = NSMenu()

        main.addItem(submenu(appName, [
            item("About \(appName)", #selector(NSApplication.orderFrontStandardAboutPanel(_:)), ""),
            .separator(),
            item("Settings…", #selector(AppDelegate.showSettings(_:)), ","),
            .separator(),
            item("Hide \(appName)", #selector(NSApplication.hide(_:)), "h"),
            item("Hide Others", #selector(NSApplication.hideOtherApplications(_:)), "h", [.command, .option]),
            item("Show All", #selector(NSApplication.unhideAllApplications(_:)), ""),
            .separator(),
            item("Quit \(appName)", #selector(NSApplication.terminate(_:)), "q"),
        ]))

        main.addItem(submenu("File", [
            item("Open…", #selector(C.openDocument(_:)), "o"),
            .separator(),
            item("Save", #selector(C.saveImage(_:)), "s"),
            item("Save As…", #selector(C.saveImageAs(_:)), "s", [.command, .shift]),
            .separator(),
            item("Reveal in Finder", #selector(C.revealInFinder(_:)), "r", [.command, .shift]),
            item("Set as Desktop Picture", #selector(C.setAsDesktopPicture(_:)), ""),
            .separator(),
            item("Move to Trash", #selector(C.moveToTrash(_:)), "\u{8}"),
            .separator(),
            item("Close Window", #selector(NSWindow.performClose(_:)), "w"),
        ]))

        main.addItem(submenu("Edit", [
            item("Copy", #selector(C.copy(_:)), "c"),
        ]))

        main.addItem(submenu("View", [
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
            item("Enter Full Screen", #selector(NSWindow.toggleFullScreen(_:)), "f", [.command, .control]),
        ]))

        main.addItem(submenu("Image", [
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
        ]))

        let sortItems = SortOrder.allCases.map { order in
            item(order.title, #selector(C.setSortOrder(_:)), "", tag: order.rawValue)
        }
        let intervalItems = [1, 2, 3, 5, 10, 30].map { seconds in
            item("\(seconds) Seconds", #selector(C.setSlideshowInterval(_:)), "", tag: seconds)
        }
        main.addItem(submenu("Go", [
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
        ]))
        let window = submenu("Window", [
            item("Minimize", #selector(NSWindow.performMiniaturize(_:)), "m"),
            item("Zoom", #selector(NSWindow.performZoom(_:)), ""),
        ])
        main.addItem(window)
        NSApp.windowsMenu = window.submenu

        return main
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
