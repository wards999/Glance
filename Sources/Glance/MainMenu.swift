import AppKit

/// The menu bar and the right-click menu, built from the same pieces so they can't drift apart.
/// Single-key shortcuts (space, arrows, R, I, F2, …) live in
/// `ViewerWindowController.imageView(_:handleKeyDown:)`; menus only carry ⌘ shortcuts so they can't
/// swallow typing in a dialog.
enum MainMenu {
    private typealias C = ViewerWindowController
    private static let appName = ProcessInfo.processInfo.processName

    static func build() -> NSMenu {
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
        main.addItem(submenu("File", sections(
            fileItems(),
            [item("Move to Trash", #selector(C.moveToTrash(_:)), "\u{8}")],
            [item("Close Window", #selector(NSWindow.performClose(_:)), "w")]
        )))
        main.addItem(submenu("Edit", clipboardItems()))
        main.addItem(submenu("View", sections(
            zoomItems(),
            overlayItems(),
            [submenu("Auto Zoom Mode", autoZoomItems())],
            [item("Enter Full Screen", #selector(C.toggleFullScreenMode(_:)), "f", [.command, .control])]
        )))
        main.addItem(submenu("Image", sections(
            transformItems(),
            [submenu("Lossless Transformations", losslessItems())],
            adjustmentItems(),
            parameterItems()
        )))
        main.addItem(submenu("Go", sections(
            navigationItems(),
            [submenu("Display Order", displayOrderItems()), submenu("Slideshow", slideshowItems())]
        )))
        let window = submenu("Window", [
            item("Minimize", #selector(NSWindow.performMiniaturize(_:)), "m"),
            item("Zoom", #selector(NSWindow.performZoom(_:)), ""),
        ])
        main.addItem(window)
        NSApp.windowsMenu = window.submenu
        return main
    }

    /// The right-click menu, laid out like JPEGView's. It's the only menu in full screen,
    /// so everything is here.
    static func contextMenu() -> NSMenu {
        let navigation = navigationItems().filter {
            $0.action != #selector(C.nextImage(_:)) && $0.action != #selector(C.previousImage(_:))
        }
        let menu = NSMenu()
        let items = sections(
            fileItems(),
            clipboardItems(),
            overlayItems(),
            [
                item("Next Image", #selector(C.nextImage(_:)), "]"),
                item("Previous Image", #selector(C.previousImage(_:)), "["),
                submenu("Navigation", navigation),
                submenu("Display Order", displayOrderItems()),
                submenu("Slideshow", slideshowItems()),
            ],
            [
                submenu("Transform Image", transformItems()),
                submenu("Lossless Transformations", losslessItems()),
            ] + adjustmentItems(),
            parameterItems(),
            [
                submenu("Zoom", zoomItems()),
                submenu("Auto Zoom Mode", autoZoomItems()),
                item("Enter Full Screen", #selector(C.toggleFullScreenMode(_:)), "f", [.command, .control]),
            ],
            [
                item("Settings…", #selector(AppDelegate.showSettings(_:)), ","),
                item("About \(appName)", #selector(NSApplication.orderFrontStandardAboutPanel(_:)), ""),
            ],
            [
                item("Move to Trash", #selector(C.moveToTrash(_:)), "\u{8}"),
                item("Quit \(appName)", #selector(NSApplication.terminate(_:)), "q"),
            ]
        )
        items.forEach { menu.addItem($0) }
        return menu
    }

    /// Joins groups of items with separators between them.
    private static func sections(_ groups: [NSMenuItem]...) -> [NSMenuItem] {
        var items: [NSMenuItem] = []
        for group in groups where !group.isEmpty {
            if !items.isEmpty { items.append(.separator()) }
            items.append(contentsOf: group)
        }
        return items
    }

    // MARK: - Pieces (each call returns fresh items: a menu item can only live in one menu)

    private static func fileItems() -> [NSMenuItem] {
        [
            item("Open Image…", #selector(C.openDocument(_:)), "o"),
            item("Save", #selector(C.saveImage(_:)), "s"),
            item("Save Processed Image…", #selector(C.saveImageAs(_:)), "s", [.command, .shift]),
            item("Reload Image", #selector(C.reloadImage(_:)), ""),
            item("Reveal in Finder", #selector(C.revealInFinder(_:)), "r", [.command, .shift]),
            item("Print Image…", #selector(C.printImage(_:)), "p"),
            submenu("Set Modification Date", [
                item("To Now", #selector(C.setModificationDateToNow(_:)), ""),
                item("To Date Taken", #selector(C.setModificationDateToDateTaken(_:)), ""),
            ]),
            item("Set as Desktop Picture", #selector(C.setAsDesktopPicture(_:)), ""),
        ]
    }

    private static func clipboardItems() -> [NSMenuItem] {
        [
            item("Copy", #selector(C.copy(_:)), "c"),
            item("Copy Original Size Image", #selector(C.copyOriginalSize(_:)), "c", [.command, .option]),
            item("Copy File Path", #selector(C.copyFilePath(_:)), "c", [.command, .shift]),
            item("Paste", #selector(C.paste(_:)), "v"),
        ]
    }

    private static func overlayItems() -> [NSMenuItem] {
        [
            item("Show Picture Info (EXIF)", #selector(C.toggleInfo(_:)), "i"),
            item("Show Filename", #selector(C.toggleFilename(_:)), ""),
            item("Show Navigation Controls", #selector(C.toggleNavigationControls(_:)), ""),
        ]
    }

    private static func navigationItems() -> [NSMenuItem] {
        [
            item("Next Image", #selector(C.nextImage(_:)), "]"),
            item("Previous Image", #selector(C.previousImage(_:)), "["),
            item("First Image", #selector(C.firstImage(_:)), ""),
            item("Last Image", #selector(C.lastImage(_:)), ""),
            .separator(),
            item("Next Page", #selector(C.nextPage(_:)), "]", [.command, .option]),
            item("Previous Page", #selector(C.previousPage(_:)), "[", [.command, .option]),
            .separator(),
            item("Loop Through Folder", #selector(C.toggleLoop(_:)), ""),
        ]
    }

    private static func displayOrderItems() -> [NSMenuItem] {
        let orders = SortOrder.allCases.map { order in
            item(order.title, #selector(C.setSortOrder(_:)), "", tag: order.rawValue)
        }
        return sections(orders, [item("Descending", #selector(C.toggleSortDescending(_:)), "")])
    }

    private static func slideshowItems() -> [NSMenuItem] {
        let intervals = Settings.slideshowIntervals.map { seconds in
            item("\(Int(seconds)) Seconds", #selector(C.setSlideshowInterval(_:)), "", tag: Int(seconds))
        }
        return sections([item("Start Slideshow", #selector(C.toggleSlideshow(_:)), "p", [.command, .option])], intervals)
    }

    private static func transformItems() -> [NSMenuItem] {
        [
            item("Rotate Left", #selector(C.rotateLeft(_:)), "l"),
            item("Rotate Right", #selector(C.rotateRight(_:)), "r"),
            item("Flip Horizontal", #selector(C.flipHorizontal(_:)), ""),
            item("Flip Vertical", #selector(C.flipVertical(_:)), ""),
            .separator(),
            item("Crop to Selection", #selector(C.cropToSelection(_:)), "k"),
            item("Revert Edits", #selector(C.resetEdits(_:)), ""),
        ]
    }

    /// Applied straight to the file by rewriting its orientation tag.
    private static func losslessItems() -> [NSMenuItem] {
        [
            item("Rotate 90° Clockwise", #selector(C.losslessTransform(_:)), "", tag: 0),
            item("Rotate 90° Counterclockwise", #selector(C.losslessTransform(_:)), "", tag: 1),
            item("Rotate 180°", #selector(C.losslessTransform(_:)), "", tag: 2),
            item("Flip Horizontally", #selector(C.losslessTransform(_:)), "", tag: 3),
            item("Flip Vertically", #selector(C.losslessTransform(_:)), "", tag: 4),
        ]
    }

    private static func adjustmentItems() -> [NSMenuItem] {
        [
            item("Auto Correction", #selector(C.toggleAutoCorrect(_:)), ""),
            item("Adjustments…", #selector(C.showAdjustments(_:)), "e"),
            item("Keep Adjustments", #selector(C.toggleKeepAdjustments(_:)), ""),
        ]
    }

    private static func parameterItems() -> [NSMenuItem] {
        [
            item("Save Adjustments for This Image", #selector(C.saveParameters(_:)), ""),
            item("Clear Saved Adjustments", #selector(C.clearParameters(_:)), ""),
        ]
    }

    private static func zoomItems() -> [NSMenuItem] {
        [
            item("Zoom In", #selector(C.zoomIn(_:)), "="),
            item("Zoom Out", #selector(C.zoomOut(_:)), "-"),
            item("Fit to Window", #selector(C.zoomToFit(_:)), "0"),
            item("Actual Size", #selector(C.zoomActualSize(_:)), "1"),
            item("Fill Window", #selector(C.zoomToFill(_:)), "2"),
            item("Zoom to Selection", #selector(C.zoomToSelection(_:)), "3"),
        ]
    }

    private static func autoZoomItems() -> [NSMenuItem] {
        [
            item("Fit to Window", #selector(C.setStartZoom(_:)), "", tag: StartZoom.fit.rawValue),
            item("Fill Window", #selector(C.setStartZoom(_:)), "", tag: StartZoom.fill.rawValue),
            item("Actual Size", #selector(C.setStartZoom(_:)), "", tag: StartZoom.actual.rawValue),
            .separator(),
            item("Enlarge Small Images to Fit", #selector(C.toggleEnlargeSmall(_:)), ""),
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
