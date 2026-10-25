import AppKit

/// The Settings window (⌘,). Every control writes straight to `Settings`, so changes apply
/// immediately and there's no Save or Apply button.
final class SettingsWindowController: NSWindowController {
    static let shared = SettingsWindowController()

    private var handlers: [ActionHandler] = []
    /// Re-read each control from `Settings`, e.g. after a menu command changed one.
    private var refreshers: [() -> Void] = []
    private var observer: NSObjectProtocol?

    private init() {
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 580, height: 470),
            styleMask: [.titled, .closable],
            backing: .buffered,
            defer: true
        )
        window.title = "Settings"
        window.isReleasedWhenClosed = false
        super.init(window: window)

        let tabs = NSTabView(frame: NSRect(x: 0, y: 0, width: 580, height: 470))
        tabs.addTabViewItem(tab("General", generalRows()))
        tabs.addTabViewItem(tab("Display", displayRows()))
        tabs.addTabViewItem(tab("PDF", pdfRows()))
        tabs.addTabViewItem(tab("Saving", savingRows()))
        tabs.addTabViewItem(tab("Performance", performanceRows()))
        window.contentView = tabs
        window.center()

        refresh()
        observer = NotificationCenter.default.addObserver(forName: Settings.didChange, object: nil, queue: nil) { [weak self] _ in
            self?.refresh()
        }
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    private func refresh() {
        refreshers.forEach { $0() }
    }

    // MARK: - Tabs

    private func generalRows() -> [[NSView]] {
        let s = Settings.shared
        return [
            [label("Sort images by:"), popup(SortOrder.allCases.map(\.title), { s.sortOrder.rawValue }, { s.sortOrder = SortOrder(rawValue: $0) ?? .name })],
            [empty, checkbox("Descending order", { s.descending }, { s.descending = $0 })],
            [empty, checkbox("Loop back to the first image at the end of a folder", { s.loop }, { s.loop = $0 })],
            [label("Mouse wheel:"), popup(["Zooms in and out", "Goes to the next or previous image"], { s.wheelAction.rawValue }, { s.wheelAction = WheelAction(rawValue: $0) ?? .zoom })],
            [empty, note("Hold ⌘ while scrolling to zoom either way. Trackpads always pan.")],
            [label("Slideshow interval:"), popup(
                Settings.slideshowIntervals.map { "\(Int($0)) seconds" },
                { Settings.slideshowIntervals.firstIndex(of: s.slideshowInterval) ?? 2 },
                { s.slideshowInterval = Settings.slideshowIntervals[$0] }
            )],
            [empty, checkbox("Keep adjustments when changing images", { s.keepAdjustments }, { s.keepAdjustments = $0 })],
        ]
    }

    private func displayRows() -> [[NSView]] {
        let s = Settings.shared
        return [
            [label("Open images at:"), popup(["Fit to window", "Fill window", "Actual size (100%)"], { s.startZoom.rawValue }, { s.startZoom = StartZoom(rawValue: $0) ?? .fit })],
            [empty, checkbox("Enlarge small images to fit the window", { s.enlargeSmallImages }, { s.enlargeSmallImages = $0 })],
            [empty, checkbox("Open in full screen", { s.openFullScreen }, { s.openFullScreen = $0 })],
            [label("Full screen style:"), popup(
                ["Instant (covers the screen, no animation)", "macOS full screen (own Space, animated)"],
                { s.fullScreenStyle.rawValue },
                { s.fullScreenStyle = FullScreenStyle(rawValue: $0) ?? .instant }
            )],
            [label("Background:"), colorWell({ s.backgroundColor }, { s.backgroundColor = $0 })],
            [empty, checkbox("Show the info overlay", { s.showInfo }, { s.showInfo = $0 })],
            [empty, checkbox("Show the file name", { s.showFilename }, { s.showFilename = $0 })],
            [empty, checkbox("Show controls when the mouse nears the bottom", { s.showControls }, { s.showControls = $0 })],
            [empty, checkbox("Hide the cursor in full screen", { s.hideCursorInFullScreen }, { s.hideCursorInFullScreen = $0 })],
            [empty, checkbox("Show individual pixels above 400% zoom", { s.pixelatedZoom }, { s.pixelatedZoom = $0 })],
        ]
    }

    private func pdfRows() -> [[NSView]] {
        let s = Settings.shared
        let factors = Settings.holdZoomFactors
        return [
            [label("Page layout:"), popup(PDFPageMode.allCases.map(\.title), { s.pdfPageMode.rawValue }, { s.pdfPageMode = PDFPageMode(rawValue: $0) ?? .automatic })],
            [empty, note("Automatic shows magazine-like PDFs as a cover followed by two-page spreads, and other PDFs one page at a time.")],
            [empty, checkbox("Right to left (manga)", { s.pdfRightToLeft }, { s.pdfRightToLeft = $0 })],
            [empty, checkbox("Include PDFs when browsing folders", { s.includePDFsInFolders }, { s.includePDFsInFolders = $0 })],
            [label("Hold to zoom:"), popup(["Off", "PDFs only", "PDFs and images"], { s.holdToZoom.rawValue }, { s.holdToZoom = HoldToZoom(rawValue: $0) ?? .pdfsOnly })],
            [label("Magnification:"), popup(
                factors.map { String(format: "%g×", $0) },
                { factors.firstIndex(of: s.holdZoomFactor) ?? 2 },
                { s.holdZoomFactor = factors[$0] }
            )],
            [empty, note("Press and hold on the page to magnify it; move the mouse to look around, let go to return.")],
        ]
    }

    private func savingRows() -> [[NSView]] {
        let s = Settings.shared
        return [
            [label("JPEG / HEIC quality:"), slider(0.3...1, { "\(Int(($0 * 100).rounded()))%" }, { s.saveQuality }, { s.saveQuality = $0 })],
            [empty, checkbox("Ask before re-encoding the original file", { s.confirmOverwrite }, { s.confirmOverwrite = $0 })],
            [empty, note("Rotating or flipping a JPEG and saving is always lossless.")],
        ]
    }

    private func performanceRows() -> [[NSView]] {
        let s = Settings.shared
        return [
            [label("Decode ahead:"), stepper(0...8, { "\($0) images" }, { s.prefetchCount }, { s.prefetchCount = $0 })],
            [label("Memory cache:"), slider(0.5...Settings.maxCacheGB, { String(format: "%.1f GB", $0) }, { s.cacheGB }, { s.cacheGB = $0 })],
            [empty, note("More of both makes flipping through a folder instant, at the cost of RAM.")],
            [empty, checkbox("Show a quick preview while large images load", { s.quickPreview }, { s.quickPreview = $0 })],
            [empty, note("Shows a half-size version first when the full size is needed (e.g. full screen on a Retina display). Faster to appear, but slightly soft for a moment.")],
        ]
    }

    private func tab(_ title: String, _ rows: [[NSView]]) -> NSTabViewItem {
        let grid = NSGridView(views: rows)
        grid.rowSpacing = 10
        grid.columnSpacing = 10
        grid.rowAlignment = .firstBaseline
        grid.column(at: 0).xPlacement = .trailing
        grid.translatesAutoresizingMaskIntoConstraints = false

        let container = NSView()
        container.addSubview(grid)
        NSLayoutConstraint.activate([
            grid.topAnchor.constraint(equalTo: container.topAnchor, constant: 24),
            grid.centerXAnchor.constraint(equalTo: container.centerXAnchor),
        ])
        let item = NSTabViewItem(identifier: title)
        item.label = title
        item.view = container
        return item
    }

    // MARK: - Controls

    private var empty: NSView { NSGridCell.emptyContentView }

    private func label(_ text: String) -> NSTextField {
        NSTextField(labelWithString: text)
    }

    private func note(_ text: String) -> NSTextField {
        let field = NSTextField(wrappingLabelWithString: text)
        field.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        field.textColor = .secondaryLabelColor
        field.preferredMaxLayoutWidth = 320
        return field
    }

    private func bind(_ control: NSControl, _ action: @escaping () -> Void) {
        let handler = ActionHandler { _ in action() }
        control.target = handler
        control.action = #selector(ActionHandler.fire(_:))
        handlers.append(handler)
    }

    private func checkbox(_ title: String, _ get: @escaping () -> Bool, _ set: @escaping (Bool) -> Void) -> NSButton {
        let button = NSButton(checkboxWithTitle: title, target: nil, action: nil)
        bind(button) { set(button.state == .on) }
        refreshers.append { button.state = get() ? .on : .off }
        return button
    }

    private func popup(_ titles: [String], _ get: @escaping () -> Int, _ set: @escaping (Int) -> Void) -> NSPopUpButton {
        let popup = NSPopUpButton()
        popup.addItems(withTitles: titles)
        bind(popup) { set(popup.indexOfSelectedItem) }
        refreshers.append { popup.selectItem(at: get()) }
        return popup
    }

    private func slider(_ range: ClosedRange<Double>, _ format: @escaping (Double) -> String,
                        _ get: @escaping () -> Double, _ set: @escaping (Double) -> Void) -> NSView {
        let slider = NSSlider(value: range.lowerBound, minValue: range.lowerBound, maxValue: range.upperBound, target: nil, action: nil)
        slider.widthAnchor.constraint(equalToConstant: 200).isActive = true
        let value = NSTextField(labelWithString: "")
        value.font = .monospacedDigitSystemFont(ofSize: NSFont.systemFontSize, weight: .regular)
        bind(slider) {
            value.stringValue = format(slider.doubleValue)
            set(slider.doubleValue)
        }
        refreshers.append {
            slider.doubleValue = get()
            value.stringValue = format(slider.doubleValue)
        }
        return NSStackView(views: [slider, value])
    }

    private func stepper(_ range: ClosedRange<Int>, _ format: @escaping (Int) -> String,
                         _ get: @escaping () -> Int, _ set: @escaping (Int) -> Void) -> NSView {
        let stepper = NSStepper()
        stepper.minValue = Double(range.lowerBound)
        stepper.maxValue = Double(range.upperBound)
        stepper.increment = 1
        let value = NSTextField(labelWithString: "")
        bind(stepper) { set(stepper.integerValue) }
        refreshers.append {
            stepper.integerValue = get()
            value.stringValue = format(stepper.integerValue)
        }
        return NSStackView(views: [value, stepper])
    }

    private func colorWell(_ get: @escaping () -> NSColor, _ set: @escaping (NSColor) -> Void) -> NSColorWell {
        let well = NSColorWell()
        well.widthAnchor.constraint(equalToConstant: 44).isActive = true
        well.heightAnchor.constraint(equalToConstant: 24).isActive = true
        bind(well) { set(well.color) }
        refreshers.append { well.color = get() }
        return well
    }
}
