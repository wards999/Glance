import AppKit
import ImageIO
import UniformTypeIdentifiers

/// Cancels a running GIF/WebP animation. The animation callback may arrive on any thread.
private final class AnimationToken {
    private let lock = NSLock()
    private var cancelled = false

    var isCancelled: Bool {
        lock.lock()
        defer { lock.unlock() }
        return cancelled
    }

    func cancel() {
        lock.lock()
        cancelled = true
        lock.unlock()
    }
}

/// A borderless window normally refuses to become key, which would stop keyboard input
/// in instant full screen.
private final class ViewerWindow: NSWindow {
    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { true }
}

final class ViewerWindowController: NSWindowController, NSWindowDelegate, NSMenuItemValidation, NSMenuDelegate, ImageViewDelegate, VideoViewDelegate {
    private let imageView = ImageView(frame: .zero)
    private let infoBox = NSView()
    private let infoLabel = NSTextField(wrappingLabelWithString: "")
    private let messageLabel = NSTextField(labelWithString: "")
    private lazy var controlBar = ControlBar(target: self)
    private var adjustmentsPanel: AdjustmentsPanel?

    private let navigator = FolderNavigator()
    private let pipeline = ImagePipeline(costLimit: Settings.shared.cacheBytes)
    private let settings = Settings.shared
    private var settingsObserver: NSObjectProtocol?
    private var activationObserver: NSObjectProtocol?
    private var appliedPDFLayout = PDFLayout()
    private var appliedIncludes: (pdfs: Bool, videos: Bool)?
    /// Core Image work (adjustments, crop, save) runs here, one job at a time.
    private let renderQueue = DispatchQueue(label: "glance.render", qos: .userInitiated)

    /// Best decode so far of the picture on screen, before adjustments.
    private var current: DecodedImage?
    /// Set after a crop: the picture no longer matches the file, and this is its full-resolution pixels.
    private var editedImage: CGImage?
    private var page = 0
    /// Bumped whenever the picture changes, so late results for an old picture are ignored.
    private var generation = 0
    private var direction = 1
    private var requestedLongSide = 0
    private var foregroundKey: ImagePipeline.Key?
    private var animation: AnimationToken?

    private var adjustments = Adjustments()
    private var keepAdjustments = false
    private var renderInFlight = false
    private var renderAgain = false
    private var pendingViewReset = false

    private var showInfo = false
    private var slideshowTimer: Timer?
    private var slideshowInterval: TimeInterval = 3
    private var saveQuality = 0.9
    private var controlsHideTimer: Timer?
    private var messageHideTimer: Timer?
    /// The message stays until the next picture appears ("Loading…", "Can't open …").
    private var messageIsSticky = false

    /// Window state to return to when leaving instant full screen.
    private var windowedFrame: NSRect?
    private var windowedStyle: NSWindow.StyleMask = []

    // Videos (libmpv, loaded only when the first one is shown)
    private var videoView: VideoView?
    private var showingVideo = false
    private var keyRepeating = false  // while a held key repeats: videos wait until you stop
    private var videoPlayTimer: Timer?
    private let contextMenu = NSMenu()
    private static let frameAutosaveName = "GlanceViewer"

    init() {
        let visible = NSScreen.main?.visibleFrame ?? NSRect(x: 0, y: 0, width: 1440, height: 900)
        let frame = visible.insetBy(dx: visible.width * 0.1, dy: visible.height * 0.1)
        let window = ViewerWindow(
            contentRect: frame,
            styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView],
            backing: .buffered,
            defer: false
        )
        window.titlebarAppearsTransparent = true
        window.backgroundColor = .black
        window.appearance = NSAppearance(named: .darkAqua)
        window.collectionBehavior.insert(.fullScreenPrimary)
        window.minSize = NSSize(width: 320, height: 240)
        window.isReleasedWhenClosed = false
        // No "growing" animation when the window first appears (or closes): the picture just shows.
        window.animationBehavior = .none
        super.init(window: window)
        window.delegate = self
        window.setFrameAutosaveName(Self.frameAutosaveName)
        buildViews(in: window)
        applySettings()
        settingsObserver = NotificationCenter.default.addObserver(forName: Settings.didChange, object: nil, queue: nil) { [weak self] _ in
            self?.applySettings()
        }
        activationObserver = NotificationCenter.default.addObserver(forName: NSApplication.didBecomeActiveNotification, object: nil, queue: nil) { [weak self] _ in
            guard let self, self.isInstantFullScreen else { return }
            self.applyFullScreenPresentation()
        }
        updateTitle()
    }

    /// Picks up every preference. Menu toggles write to `Settings` too and arrive here,
    /// so the menus, the Settings window and the viewer never disagree.
    private func applySettings() {
        if navigator.sortOrder != settings.sortOrder || navigator.descending != settings.descending {
            navigator.sortOrder = settings.sortOrder
            navigator.descending = settings.descending
            navigator.resort()
            updateTitle()
        }
        let includes = (settings.includePDFsInFolders, settings.includeVideosInFolders)
        if let applied = appliedIncludes, applied != includes, let current = navigator.current {
            // Which files the folder lists changed: list it again, staying on the file you're viewing.
            scanFolder(current.deletingLastPathComponent(), keeping: current)
        }
        appliedIncludes = includes
        navigator.wraps = settings.loop
        if slideshowInterval != settings.slideshowInterval {
            slideshowInterval = settings.slideshowInterval
            if slideshowTimer != nil { startSlideshow() }
        }
        keepAdjustments = settings.keepAdjustments
        saveQuality = settings.saveQuality
        showInfo = settings.showInfo
        updateInfo()
        if !settings.showControls { setControlsVisible(false) }
        videoView?.options = videoOptions

        imageView.defaultZoomMode = settings.startZoom.zoomMode
        imageView.enlargeSmallImages = settings.enlargeSmallImages
        imageView.wheelNavigates = settings.wheelAction == .navigate
        imageView.pixelatedZoom = settings.pixelatedZoom
        imageView.backgroundColor = settings.backgroundColor
        window?.backgroundColor = settings.backgroundColor

        pipeline.costLimit = settings.cacheBytes
        pipeline.quickPreview = settings.quickPreview
        imageView.holdZoomFactor = CGFloat(settings.holdZoomFactor)
        updateHoldToZoom()

        let layout = settings.pdfLayout
        if layout != appliedPDFLayout {
            appliedPDFLayout = layout
            // Page indexes count spreads, so find the screen that holds the page you were on.
            if let pdf = currentPDF, let firstPage = pdf.info.pdf?.pages.first {
                let url = pdf.info.url
                let spreads = PDFRenderer.spreads(for: url, layout: layout, password: pipeline.password(for: url))
                page = spreads.firstIndex { $0.contains(firstPage) } ?? 0
                showCurrent(keepPage: true)
                return
            }
        }
        if navigator.current != nil { prefetchNeighbors() }
    }

    private func updateHoldToZoom() {
        let isPDF = current?.info.pdf != nil
        switch settings.holdToZoom {
        case .off: imageView.holdToZoomEnabled = false
        case .pdfsOnly: imageView.holdToZoomEnabled = isPDF
        case .everything: imageView.holdToZoomEnabled = true
        }
    }

    @objc func setPDFPageMode(_ sender: NSMenuItem) {
        settings.pdfPageMode = PDFPageMode(rawValue: sender.tag) ?? .automatic
    }

    @objc func togglePDFRightToLeft(_ sender: Any?) {
        settings.pdfRightToLeft.toggle()
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    /// Shows the window at launch, already full screen if that's the setting. Instant full screen
    /// is applied before the window first appears, so there's no windowed frame on the way.
    func showAtLaunch() {
        let fullScreen = settings.openFullScreen
        if fullScreen && settings.fullScreenStyle == .instant {
            enterInstantFullScreen()
        }
        showWindow(nil)
        if fullScreen && settings.fullScreenStyle == .native {
            window?.toggleFullScreen(nil)
        }
    }

    // MARK: - Full screen

    private var isInstantFullScreen: Bool { windowedFrame != nil }

    private var isFullScreen: Bool {
        isInstantFullScreen || window?.styleMask.contains(.fullScreen) == true
    }

    @objc func toggleFullScreenMode(_ sender: Any?) {
        guard let window else { return }
        if window.styleMask.contains(.fullScreen) {
            window.toggleFullScreen(nil)
        } else if isInstantFullScreen {
            exitInstantFullScreen()
        } else if settings.fullScreenStyle == .native {
            window.toggleFullScreen(nil)
        } else {
            enterInstantFullScreen()
        }
    }

    private func enterInstantFullScreen() {
        guard let window, !isInstantFullScreen, let screen = window.screen ?? NSScreen.main else { return }
        windowedFrame = window.frame
        windowedStyle = window.styleMask
        // Don't let the screen-sized frame overwrite the saved window position.
        window.setFrameAutosaveName("")
        applyFullScreenPresentation()
        window.styleMask = [.borderless]
        window.setFrame(screen.frame, display: true)
        window.makeFirstResponder(imageView)
    }

    /// Menu bar and Dock fully hidden: nothing but the picture. Everything is on the right-click menu.
    /// Re-applied whenever we come back to the front, since macOS only honours it for the active app
    /// (at launch we set it before the app has finished activating).
    private func applyFullScreenPresentation() {
        NSApp.presentationOptions = [.hideDock, .hideMenuBar]
    }

    func windowDidBecomeKey(_ notification: Notification) {
        if isInstantFullScreen { applyFullScreenPresentation() }
    }

    /// macOS full screen would otherwise drop the menu bar down whenever the mouse touches the top.
    func window(_ window: NSWindow, willUseFullScreenPresentationOptions proposedOptions: NSApplication.PresentationOptions) -> NSApplication.PresentationOptions {
        [.fullScreen, .hideDock, .hideMenuBar]
    }

    private func exitInstantFullScreen() {
        guard let window, let frame = windowedFrame else { return }
        windowedFrame = nil
        NSApp.presentationOptions = []
        window.styleMask = windowedStyle
        window.titlebarAppearsTransparent = true
        window.setFrame(frame, display: true)
        window.setFrameAutosaveName(Self.frameAutosaveName)
        window.makeFirstResponder(imageView)
    }

    private func buildViews(in window: NSWindow) {
        let content = NSView()
        window.contentView = content

        imageView.frame = content.bounds
        imageView.autoresizingMask = [.width, .height]
        imageView.delegate = self
        contextMenu.delegate = self  // rebuilt for pictures, PDFs or videos each time it opens
        imageView.menu = contextMenu
        content.addSubview(imageView)

        infoBox.wantsLayer = true
        infoBox.layer?.backgroundColor = NSColor.black.withAlphaComponent(0.6).cgColor
        infoBox.layer?.cornerRadius = 8
        infoBox.isHidden = true
        infoBox.translatesAutoresizingMaskIntoConstraints = false
        infoLabel.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        infoLabel.textColor = .white
        infoLabel.isSelectable = false
        infoLabel.preferredMaxLayoutWidth = 420
        infoLabel.translatesAutoresizingMaskIntoConstraints = false
        infoBox.addSubview(infoLabel)
        content.addSubview(infoBox)

        messageLabel.font = .systemFont(ofSize: 15, weight: .semibold)
        messageLabel.textColor = .white
        messageLabel.alignment = .center
        messageLabel.drawsBackground = true
        messageLabel.backgroundColor = NSColor.black.withAlphaComponent(0.6)
        messageLabel.isHidden = true
        messageLabel.translatesAutoresizingMaskIntoConstraints = false
        content.addSubview(messageLabel)

        controlBar.alphaValue = 0
        controlBar.isHidden = true
        controlBar.translatesAutoresizingMaskIntoConstraints = false
        content.addSubview(controlBar)

        NSLayoutConstraint.activate([
            infoLabel.leadingAnchor.constraint(equalTo: infoBox.leadingAnchor, constant: 14),
            infoLabel.trailingAnchor.constraint(equalTo: infoBox.trailingAnchor, constant: -14),
            infoLabel.topAnchor.constraint(equalTo: infoBox.topAnchor, constant: 10),
            infoLabel.bottomAnchor.constraint(equalTo: infoBox.bottomAnchor, constant: -10),
            // The safe area keeps clear of the title bar in a window and of the camera
            // housing and rounded corners in full screen.
            infoBox.leadingAnchor.constraint(equalTo: content.safeAreaLayoutGuide.leadingAnchor, constant: 20),
            infoBox.topAnchor.constraint(equalTo: content.safeAreaLayoutGuide.topAnchor, constant: 16),

            messageLabel.centerXAnchor.constraint(equalTo: content.centerXAnchor),
            messageLabel.centerYAnchor.constraint(equalTo: content.centerYAnchor),

            controlBar.centerXAnchor.constraint(equalTo: content.centerXAnchor),
            controlBar.bottomAnchor.constraint(equalTo: content.bottomAnchor, constant: -16),
        ])

        window.makeFirstResponder(imageView)
    }

    // MARK: - Opening

    func open(_ urls: [URL]) {
        guard let first = urls.first else { return }
        window?.makeKeyAndOrderFront(nil)
        NSDocumentController.shared.noteNewRecentDocumentURL(first)

        var isDirectory: ObjCBool = false
        let exists = FileManager.default.fileExists(atPath: first.path, isDirectory: &isDirectory)
        if urls.count == 1, exists, isDirectory.boolValue {
            showMessage("Loading folder…", duration: nil)
            scanFolder(first, keeping: nil)
        } else if urls.count == 1 {
            // Show the file first; list its folder in the background.
            navigator.setFiles([first], current: first)
            showCurrent()
            scanFolder(first.deletingLastPathComponent(), keeping: first)
        } else {
            let openable = urls.filter { ImageDecoder.isSupported($0) || PDFRenderer.isPDF($0) || VideoFiles.isPlayable($0) }
            let images = FolderNavigator.sort(openable, by: navigator.sortOrder, descending: navigator.descending)
            navigator.setFiles(images, current: images.first)
            showCurrent()
        }
    }

    private func scanFolder(_ folder: URL, keeping selected: URL?) {
        let order = navigator.sortOrder
        let descending = navigator.descending
        // Opening a PDF directly means you're reading PDFs, so its neighbours count too.
        let includePDFs = settings.includePDFsInFolders || (selected.map(PDFRenderer.isPDF) ?? false)
        let includeVideos = settings.includeVideosInFolders || (selected.map(VideoFiles.isPlayable) ?? false)
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let files = FolderNavigator.scan(folder: folder, order: order, descending: descending,
                                             includePDFs: includePDFs, includeVideos: includeVideos)
            DispatchQueue.main.async {
                guard let self else { return }
                if let selected {
                    // Only adopt the listing if the user hasn't opened something else meanwhile.
                    guard self.navigator.current == selected else { return }
                    self.navigator.setFiles(files, current: selected)
                    self.updateTitle()
                    self.prefetchNeighbors()
                } else {
                    self.navigator.setFiles(files, current: nil)
                    if files.isEmpty {
                        self.showMessage("No images in “\(folder.lastPathComponent)”", duration: nil)
                    }
                    self.showCurrent()
                }
            }
        }
    }

    @objc func openDocument(_ sender: Any?) {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = true
        panel.canChooseDirectories = true
        panel.allowedContentTypes = MPV.available ? [.image, .pdf, .movie, .video] : [.image, .pdf]
        let completion: (NSApplication.ModalResponse) -> Void = { [weak self] response in
            guard response == .OK else {
                if self?.navigator.current == nil {
                    self?.showMessage("Open an image with ⌘O, or drop one here", duration: nil)
                }
                return
            }
            self?.open(panel.urls)
        }
        if let window, window.isVisible {
            panel.beginSheetModal(for: window, completionHandler: completion)
        } else {
            panel.begin(completionHandler: completion)
        }
    }

    // MARK: - Showing the current image

    /// What to decode for a newly opened picture: just enough pixels for the view at the opening
    /// zoom. Rounded up to 256 px so small window resizes still hit prefetched images. If the window
    /// later grows or you zoom in, the view asks for a sharper level.
    private var openingDecodeSize: ImagePipeline.DecodeSize {
        let scale = imageView.backingScale
        func bucket(_ points: CGFloat) -> Int { max(Int((points * scale / 256).rounded(.up)), 1) * 256 }
        let width = bucket(imageView.bounds.width)
        let height = bucket(imageView.bounds.height)
        switch settings.startZoom {
        case .fit: return .fit(width: width, height: height)
        case .fill: return .fill(width: width, height: height)
        case .actual: return .longSide(Int.max)
        }
    }

    /// PDF keys carry the page layout, since `page` counts spreads under it.
    private func pipelineKey(_ url: URL, page: Int, size: ImagePipeline.DecodeSize) -> ImagePipeline.Key {
        var key = ImagePipeline.Key(url: url, page: page, size: size)
        if PDFRenderer.isPDF(url) { key.layout = settings.pdfLayout }
        return key
    }

    /// The PDF currently on screen, if it's the file the navigator is on.
    private var currentPDF: DecodedImage? {
        guard let current, current.info.pdf != nil, current.info.url == navigator.current else { return nil }
        return current
    }

    private var displayFullSize: CGSize {
        if let editedImage { return CGSize(width: editedImage.width, height: editedImage.height) }
        return current?.info.fullSize ?? .zero
    }

    private func showCurrent(keepPage: Bool = false) {
        animation?.cancel()
        animation = nil
        if let foregroundKey { pipeline.drop(foregroundKey) }
        foregroundKey = nil

        guard let url = navigator.current else {
            if showingVideo { leaveVideo() }
            current = nil
            imageView.setImage(nil, fullSize: .zero, resetView: true)
            updateTitle()
            updateInfo()
            return
        }
        if VideoFiles.isPlayable(url) {
            showVideo(url)
            return
        }
        if showingVideo { leaveVideo() }
        if !keepPage { page = 0 }
        generation += 1
        let gen = generation
        editedImage = nil
        requestedLongSide = 0
        if let saved = ParameterStore.adjustments(for: url) {
            // Adjustments saved for this file win over carried-over ones.
            adjustments = saved
            adjustmentsPanel?.update(adjustments)
        } else if !keepAdjustments && !adjustments.isIdentity {
            adjustments = Adjustments()
            adjustmentsPanel?.update(adjustments)
        }
        updateTitle()

        let key = pipelineKey(url, page: page, size: openingDecodeSize)
        if let hit = pipeline.cached(key) {
            // Prefetched: on screen in the same run-loop turn as the key press.
            present(hit, resetView: true)
            prefetchNeighbors()
            return
        }

        // Not ready yet: keep the previous picture up until the new one is decoded.
        foregroundKey = key
        var previewShown = false
        pipeline.load(key, preview: { [weak self] thumbnail, info in
            // Embedded thumbnail and/or half-size quick preview; only the first resets the view.
            guard let self, gen == self.generation else { return }
            self.imageView.setImage(thumbnail, fullSize: info.fullSize, resetView: !previewShown)
            previewShown = true
        }, completion: { [weak self] decoded in
            guard let self, gen == self.generation else { return }
            self.foregroundKey = nil
            if let decoded {
                self.present(decoded, resetView: !previewShown)
            } else if PDFRenderer.isPDF(url), PDFRenderer.needsPassword(url, password: self.pipeline.password(for: url)) {
                self.current = nil
                self.imageView.setImage(nil, fullSize: .zero, resetView: true)
                self.askForPassword(url)
            } else {
                self.current = nil
                self.imageView.setImage(nil, fullSize: .zero, resetView: true)
                self.showMessage("Can't open “\(url.lastPathComponent)”", duration: nil)
            }
            self.prefetchNeighbors()
        })
    }

    private func askForPassword(_ url: URL) {
        guard let window else { return }
        let alert = NSAlert()
        alert.messageText = "“\(url.lastPathComponent)” is password protected"
        alert.informativeText = "Enter its password to open it. It's remembered until Glance quits."
        let field = NSSecureTextField(frame: NSRect(x: 0, y: 0, width: 260, height: 24))
        alert.accessoryView = field
        alert.addButton(withTitle: "Open")
        alert.addButton(withTitle: "Cancel")
        alert.window.initialFirstResponder = field
        alert.beginSheetModal(for: window) { [weak self] response in
            guard let self else { return }
            guard response == .alertFirstButtonReturn else {
                self.showMessage("“\(url.lastPathComponent)” is locked", duration: nil)
                return
            }
            self.pipeline.setPassword(field.stringValue, for: url)
            if self.navigator.current == url { self.showCurrent(keepPage: true) }
        }
    }

    private func present(_ decoded: DecodedImage, resetView: Bool) {
        current = decoded
        updateHoldToZoom()
        if messageIsSticky { hideMessage() }
        display(decoded.image, resetView: resetView)
        if decoded.info.isAnimated && decoded.page == 0 {
            startAnimation(decoded.info)
        }
        updateTitle()
        updateInfo()
    }

    /// Puts `base` on screen, through Core Image when adjustments are active.
    private func display(_ base: CGImage, resetView: Bool) {
        pendingViewReset = pendingViewReset || resetView
        if adjustments.isIdentity && !renderInFlight {
            imageView.setImage(base, fullSize: displayFullSize, resetView: pendingViewReset)
            pendingViewReset = false
        } else {
            scheduleRender()
        }
    }

    /// Renders the adjusted image off the main thread. While a render is running, further slider
    /// moves only mark it dirty, so dragging a slider never queues up stale work.
    private func scheduleRender() {
        guard let base = editedImage ?? current?.image else { return }
        if renderInFlight {
            renderAgain = true
            return
        }
        renderInFlight = true
        let adjustments = self.adjustments
        let gen = generation
        let autoKey = current?.info.url.path ?? ""
        let size = displayFullSize
        renderQueue.async { [weak self] in
            let output = adjustments.isIdentity ? base : (ImageRenderer.shared.apply(adjustments, to: base, autoKey: autoKey) ?? base)
            DispatchQueue.main.async {
                guard let self else { return }
                self.renderInFlight = false
                if gen == self.generation {
                    self.imageView.setImage(output, fullSize: size, resetView: self.pendingViewReset)
                    self.pendingViewReset = false
                }
                if self.renderAgain {
                    self.renderAgain = false
                    self.scheduleRender()
                }
            }
        }
    }

    private func prefetchNeighbors() {
        let size = openingDecodeSize
        let ahead = settings.prefetchCount
        guard ahead > 0 else {
            pipeline.prefetch([])
            return
        }
        var keys: [ImagePipeline.Key] = []
        // Mostly in the direction of travel, plus one behind for "oops, go back".
        var offsets = (1...ahead).map { $0 * direction }
        offsets.insert(-direction, at: min(2, offsets.count))
        for offset in offsets {
            // Inside a PDF the neighbours are its other pages; past either end, the next files.
            if let pdf = currentPDF, (0..<pdf.info.pageCount).contains(page + offset) {
                let key = pipelineKey(pdf.info.url, page: page + offset, size: size)
                if !keys.contains(key) { keys.append(key) }
                continue
            }
            guard let url = navigator.url(at: offset), url != navigator.current else { continue }
            if VideoFiles.isPlayable(url) {
                // Videos aren't decoded ahead (that's what keeps browsing past them instant), but
                // one right next to you gets the player ready, so it opens without a pause.
                if abs(offset) == 1 { makeVideoView()?.warm() }
                continue
            }
            let key = pipelineKey(url, page: 0, size: size)
            if !keys.contains(key) { keys.append(key) }
        }
        pipeline.prefetch(keys)
    }

    private func startAnimation(_ info: ImageInfo) {
        let token = AnimationToken()
        animation = token
        _ = CGAnimateImageAtURLWithBlock(info.url as CFURL, nil) { [weak self] _, frame, stop in
            if token.isCancelled {
                stop.pointee = true
                return
            }
            DispatchQueue.main.async {
                guard let self, !token.isCancelled else { return }
                self.imageView.setImage(frame, fullSize: CGSize(width: frame.width, height: frame.height), resetView: false)
            }
        }
    }

    // MARK: - ImageViewDelegate

    func imageViewDidChangeZoom(_ view: ImageView) {
        updateTitle()
        if showInfo { updateInfo() }
    }

    func imageView(_ view: ImageView, needsLongSide pixels: Int) {
        // While a preview of a new picture is up, `current` is still the previous picture; its
        // decode is already on the way at the right size, so there's nothing to ask for.
        guard let current, current.info.url == navigator.current, current.page == page,
              editedImage == nil, !current.info.isAnimated else { return }
        // JPEG and HEIF decode fastest at 1/2, 1/4, 1/8 scale, so ask for the smallest such level that's sharp enough.
        let level = ImagePipeline.halvingLevel(full: current.info.fullLongSide, atLeast: pixels)
        guard level > requestedLongSide, level > current.longSide else { return }
        requestedLongSide = level

        let key = pipelineKey(current.info.url, page: current.page, size: .longSide(level))
        let gen = generation
        let upgrade: (DecodedImage) -> Void = { [weak self] decoded in
            guard let self, gen == self.generation, decoded.longSide > (self.current?.longSide ?? 0) else { return }
            self.current = decoded
            self.display(decoded.image, resetView: false)
        }
        if let hit = pipeline.cached(key) {
            upgrade(hit)
        } else {
            pipeline.load(key, preview: nil) { decoded in
                if let decoded { upgrade(decoded) }
            }
        }
    }

    func imageView(_ view: ImageView, navigate delta: Int) {
        go(delta)
    }

    func imageView(_ view: ImageView, didReceiveDrop urls: [URL]) {
        open(urls)
    }

    func imageViewMouseMoved(_ view: ImageView, at point: NSPoint) {
        setControlsVisible(settings.showControls && point.y < 110)
        controlsHideTimer?.invalidate()
        controlsHideTimer = Timer.scheduledTimer(withTimeInterval: 2, repeats: false) { [weak self] _ in
            guard let self, let window = self.window else { return }
            let mouse = self.controlBar.convert(window.mouseLocationOutsideOfEventStream, from: nil)
            if self.controlBar.bounds.contains(mouse) { return }
            self.setControlsVisible(false)
            if self.isFullScreen && self.settings.hideCursorInFullScreen {
                NSCursor.setHiddenUntilMouseMoves(true)
            }
        }
    }

    func imageView(_ view: ImageView, handleKeyDown event: NSEvent) -> Bool {
        let modifiers = event.modifierFlags.intersection([.command, .control, .option])
        if modifiers == .option, let key = event.charactersIgnoringModifiers?.unicodeScalars.first.map({ Int($0.value) }) {
            // ⌥ Page Down / ⌥↓: straight to the next file, even from the middle of a PDF.
            switch key {
            case NSPageDownFunctionKey, NSDownArrowFunctionKey:
                goToFile(1)
                return true
            case NSPageUpFunctionKey, NSUpArrowFunctionKey:
                goToFile(-1)
                return true
            default:
                break
            }
        }
        guard modifiers.isEmpty, let scalar = event.charactersIgnoringModifiers?.unicodeScalars.first else { return false }
        keyRepeating = event.isARepeat
        defer { keyRepeating = false }
        if showingVideo, let videoView, handleVideoKey(scalar, shift: event.modifierFlags.contains(.shift), video: videoView) {
            return true
        }
        let step = max(view.bounds.width, view.bounds.height) * 0.15

        switch Int(scalar.value) {
        // Arrows pan a zoomed-in image and flip through the folder otherwise, as in JPEGView.
        case NSRightArrowFunctionKey:
            if view.canPanHorizontally { view.scroll(dx: step, dy: 0) } else { go(1) }
        case NSLeftArrowFunctionKey:
            if view.canPanHorizontally { view.scroll(dx: -step, dy: 0) } else { go(-1) }
        case NSDownArrowFunctionKey:
            if view.canPanVertically { view.scroll(dx: 0, dy: step) } else { go(1) }
        case NSUpArrowFunctionKey:
            if view.canPanVertically { view.scroll(dx: 0, dy: -step) } else { go(-1) }
        case NSPageDownFunctionKey, 0x20:
            go(1)
        case NSPageUpFunctionKey, 0x7F:
            go(-1)
        // JPEGView's function keys.
        case NSF2FunctionKey:
            toggleInfo(nil)
        case NSF4FunctionKey:
            toggleKeepAdjustments(nil)
        case NSF5FunctionKey:
            toggleAutoCorrect(nil)
        case NSHomeFunctionKey:
            firstImage(nil)
        case NSEndFunctionKey:
            lastImage(nil)
        case 0x1B:
            if view.selection != nil {
                view.clearSelection()
            } else if slideshowTimer != nil {
                stopSlideshow()
            } else if settings.escapeQuits {
                NSApp.terminate(nil)
            } else if isFullScreen {
                toggleFullScreenMode(nil)
            }
        case 0x0D, 0x03:
            if view.selection != nil { cropToSelection(nil) }
        default:
            switch Character(scalar) {
            case "+", "=": view.zoomIn()
            case "-", "_": view.zoomOut()
            case "0": view.zoomToFit()
            case "1": view.zoomToActualSize()
            case "2": view.zoomToFill()
            case "z": zoomToSelection(nil)
            case "f": toggleFullScreenMode(nil)
            case "i": toggleInfo(nil)
            case "w": revealInFinder(nil)
            case "a": toggleAutoCorrect(nil)
            case "e": showAdjustments(nil)
            case "s": toggleSlideshow(nil)
            case "r": rotateRight(nil)
            case "l": rotateLeft(nil)
            case "h": flipHorizontal(nil)
            case "v": flipVertical(nil)
            case "[": previousPage(nil)
            case "]": nextPage(nil)
            default: return false
            }
        }
        return true
    }

    // MARK: - Navigation

    private func go(_ delta: Int) {
        direction = delta < 0 ? -1 : 1
        // In a PDF, Space, arrows and the wheel turn its pages before moving to the next file.
        if let pdf = currentPDF, (0..<pdf.info.pageCount).contains(page + delta) {
            carryZoomForPageTurn(forward: delta > 0)
            page += delta
            showCurrent(keepPage: true)
            return
        }
        guard navigator.move(by: delta) else {
            NSSound.beep()
            return
        }
        showCurrent()
    }

    @objc func nextImage(_ sender: Any?) { go(1) }
    @objc func previousImage(_ sender: Any?) { go(-1) }

    @objc func firstImage(_ sender: Any?) {
        direction = 1
        navigator.moveToFirst()
        showCurrent()
    }

    @objc func lastImage(_ sender: Any?) {
        direction = -1
        navigator.moveToLast()
        showCurrent()
    }

    @objc func nextPage(_ sender: Any?) { changePage(by: 1) }
    @objc func previousPage(_ sender: Any?) { changePage(by: -1) }

    private func changePage(by delta: Int) {
        guard let info = current?.info, info.pageCount > 1, !info.isAnimated,
              (0..<info.pageCount).contains(page + delta) else {
            NSSound.beep()
            return
        }
        if currentPDF != nil { carryZoomForPageTurn(forward: delta > 0) }
        page += delta
        showCurrent(keepPage: true)
    }

    /// Reading a PDF zoomed in: the next page keeps the zoom and opens at its top-left corner
    /// (top-right for right-to-left); going back opens at the opposite, bottom corner, where
    /// you left off reading that page.
    private func carryZoomForPageTurn(forward: Bool) {
        guard imageView.zoomMode != .fit,
              imageView.canPanHorizontally || imageView.canPanVertically else { return }
        let rightToLeft = settings.pdfRightToLeft
        imageView.carriedZoom = (imageView.zoom, fromRight: forward == rightToLeft, fromBottom: !forward)
    }

    @objc func setSortOrder(_ sender: NSMenuItem) {
        settings.sortOrder = SortOrder(rawValue: sender.tag) ?? .name
    }

    @objc func toggleSortDescending(_ sender: Any?) {
        settings.descending.toggle()
    }

    @objc func toggleLoop(_ sender: Any?) {
        settings.loop.toggle()
    }

    // MARK: - Slideshow

    @objc func toggleSlideshow(_ sender: Any?) {
        if slideshowTimer != nil {
            stopSlideshow()
        } else {
            startSlideshow()
        }
    }

    @objc func setSlideshowInterval(_ sender: NSMenuItem) {
        settings.slideshowInterval = TimeInterval(sender.tag)
    }

    private func startSlideshow() {
        slideshowTimer?.invalidate()
        direction = 1
        slideshowTimer = Timer.scheduledTimer(withTimeInterval: slideshowInterval, repeats: true) { [weak self] _ in
            guard let self else { return }
            if self.showingVideo && self.videoView?.isPlaying == true { return }  // a video plays to its end first
            if !self.navigator.move(by: 1) {
                self.stopSlideshow()
                return
            }
            self.showCurrent()
        }
        showMessage("Slideshow · \(Int(slideshowInterval)) s")
    }

    private func stopSlideshow() {
        slideshowTimer?.invalidate()
        slideshowTimer = nil
        showMessage("Slideshow stopped")
    }

    // MARK: - View

    @objc func zoomIn(_ sender: Any?) { imageView.zoomIn() }
    @objc func zoomOut(_ sender: Any?) { imageView.zoomOut() }
    @objc func zoomToFit(_ sender: Any?) { imageView.zoomToFit() }
    @objc func zoomActualSize(_ sender: Any?) { imageView.zoomToActualSize() }
    @objc func zoomToFill(_ sender: Any?) { imageView.zoomToFill() }

    @objc func zoomToSelection(_ sender: Any?) {
        if !imageView.zoomToSelection() { NSSound.beep() }
    }

    @objc func toggleEnlargeSmall(_ sender: Any?) {
        settings.enlargeSmallImages.toggle()
    }

    @objc func toggleInfo(_ sender: Any?) {
        settings.showInfo.toggle()
    }

    // MARK: - Editing

    @objc func rotateLeft(_ sender: Any?) { imageView.rotate(clockwise: false) }
    @objc func rotateRight(_ sender: Any?) { imageView.rotate(clockwise: true) }
    @objc func flipHorizontal(_ sender: Any?) { imageView.flip(horizontal: true) }
    @objc func flipVertical(_ sender: Any?) { imageView.flip(horizontal: false) }

    @objc func toggleAutoCorrect(_ sender: Any?) {
        guard current?.info.isAnimated == false else { return }
        adjustments.autoCorrect.toggle()
        adjustmentsPanel?.update(adjustments)
        scheduleRender()
        showMessage(adjustments.autoCorrect ? "Auto correction on" : "Auto correction off")
    }

    @objc func toggleKeepAdjustments(_ sender: Any?) {
        settings.keepAdjustments.toggle()
    }

    @objc func showAdjustments(_ sender: Any?) {
        let panel: AdjustmentsPanel
        if let existing = adjustmentsPanel {
            panel = existing
        } else {
            panel = AdjustmentsPanel()
            panel.onChange = { [weak self] adjustments in
                guard let self, self.current?.info.isAnimated == false else { return }
                self.adjustments = adjustments
                self.scheduleRender()
            }
            if let frame = window?.frame {
                panel.setFrameTopLeftPoint(NSPoint(x: frame.maxX - 340, y: frame.maxY - 60))
            }
            adjustmentsPanel = panel
        }
        panel.update(adjustments)
        panel.orderFront(nil)
    }

    @objc func resetEdits(_ sender: Any?) {
        adjustments = Adjustments()
        adjustmentsPanel?.update(adjustments)
        editedImage = nil
        if let current {
            display(current.image, resetView: true)
        }
        updateInfo()
    }

    @objc func cropToSelection(_ sender: Any?) {
        guard let selection = imageView.selection, let current else {
            NSSound.beep()
            return
        }
        let url = current.info.url
        let page = current.page
        let orientation = imageView.orientation
        let adjustments = self.adjustments
        let base = editedImage
        let gen = generation
        let layout = settings.pdfLayout
        let password = pipeline.password(for: url)
        showMessage("Cropping…", duration: nil)

        renderQueue.async { [weak self] in
            // Crops always work on full-resolution pixels, whatever is on screen.
            let full = base ?? ImageDecoder.decodeFull(url, page: page, layout: layout, password: password)
            let output = full.flatMap {
                ImageRenderer.shared.renderEdited($0, adjustments: adjustments, orientation: orientation, crop: selection, autoKey: url.path)
            }
            DispatchQueue.main.async {
                guard let self, gen == self.generation else { return }
                guard let output else {
                    self.showMessage("Crop failed")
                    return
                }
                // Rotation and adjustments are now baked into the pixels.
                self.editedImage = output
                self.adjustments = Adjustments()
                self.adjustmentsPanel?.update(self.adjustments)
                self.hideMessage()
                self.imageView.setImage(output, fullSize: CGSize(width: output.width, height: output.height), resetView: true)
                self.updateInfo()
            }
        }
    }

    private var hasEdits: Bool {
        editedImage != nil || !adjustments.isIdentity || imageView.orientation != .identity
    }

    // MARK: - Saving

    @objc func saveImage(_ sender: Any?) {
        guard let current, let window else { return }
        guard hasEdits else {
            showMessage("No changes to save")
            return
        }
        let url = current.info.url
        // Never write over a PDF: it would become a single flattened page. Export instead.
        guard current.info.pdf == nil,
              let type = current.info.typeIdentifier.flatMap(UTType.init), ImageSaver.canWrite(type) else {
            saveImageAs(sender)
            return
        }
        if (editedImage == nil && adjustments.isIdentity) || !settings.confirmOverwrite {
            // Rotation only is saved by rewriting the orientation tag where the format allows it.
            save(to: url, type: type, quality: saveQuality)
            return
        }
        let alert = NSAlert()
        alert.messageText = "Replace “\(url.lastPathComponent)”?"
        alert.informativeText = "The original file will be re-encoded with your changes."
        alert.addButton(withTitle: "Replace")
        alert.addButton(withTitle: "Cancel")
        alert.beginSheetModal(for: window) { [weak self] response in
            guard response == .alertFirstButtonReturn, let self else { return }
            self.save(to: url, type: type, quality: self.saveQuality)
        }
    }

    @objc func saveImageAs(_ sender: Any?) {
        guard let current, let window else { return }
        let url = current.info.url
        let formats: [(name: String, type: UTType)] = [("JPEG", .jpeg), ("PNG", .png), ("TIFF", .tiff), ("HEIC", .heic)]
            .filter { ImageSaver.canWrite($0.type) }

        let panel = NSSavePanel()
        let popup = NSPopUpButton()
        formats.forEach { popup.addItem(withTitle: $0.name) }
        let quality = NSSlider(value: saveQuality, minValue: 0.3, maxValue: 1, target: nil, action: nil)
        quality.widthAnchor.constraint(equalToConstant: 140).isActive = true

        let sourceType = current.info.typeIdentifier.flatMap(UTType.init)
        let startIndex = formats.firstIndex { sourceType?.conforms(to: $0.type) == true } ?? 0
        popup.selectItem(at: startIndex)
        panel.allowedContentTypes = [formats[startIndex].type]

        let formatChanged = ActionHandler { _ in
            panel.allowedContentTypes = [formats[popup.indexOfSelectedItem].type]
        }
        popup.target = formatChanged
        popup.action = #selector(ActionHandler.fire(_:))

        let accessory = NSStackView(views: [
            NSTextField(labelWithString: "Format:"), popup,
            NSTextField(labelWithString: "Quality:"), quality,
        ])
        accessory.edgeInsets = NSEdgeInsets(top: 10, left: 16, bottom: 10, right: 16)
        panel.accessoryView = accessory
        panel.directoryURL = url.deletingLastPathComponent()
        panel.nameFieldStringValue = url.deletingPathExtension().lastPathComponent

        panel.beginSheetModal(for: window) { [weak self] response in
            withExtendedLifetime(formatChanged) {}
            guard response == .OK, let destination = panel.url, let self else { return }
            self.settings.saveQuality = quality.doubleValue
            self.save(to: destination, type: formats[popup.indexOfSelectedItem].type, quality: quality.doubleValue)
        }
    }

    private func save(to destination: URL, type: UTType, quality: Double) {
        guard let current else { return }
        let source = current.info.url
        let page = current.page
        let orientation = imageView.orientation
        let adjustments = self.adjustments
        let edited = editedImage
        let sourceType = current.info.typeIdentifier.flatMap(UTType.init)
        let canRewriteTag = edited == nil && adjustments.isIdentity && sourceType == type && page == 0
        let layout = settings.pdfLayout
        let password = pipeline.password(for: source)
        showMessage("Saving…", duration: nil)

        renderQueue.async { [weak self] in
            let result = Result<Void, Error> {
                if canRewriteTag, try ImageSaver.rewriteOrientation(of: source, to: destination, applying: orientation) {
                    return
                }
                guard let base = edited ?? ImageDecoder.decodeFull(source, page: page, layout: layout, password: password) else {
                    throw ImageSaver.SaveError.cannotRead
                }
                let needsRender = !adjustments.isIdentity || orientation != .identity
                let output = needsRender
                    ? ImageRenderer.shared.renderEdited(base, adjustments: adjustments, orientation: orientation, crop: nil, autoKey: source.path)
                    : base
                guard let output else { throw ImageSaver.SaveError.cannotWrite }
                try ImageSaver.write(output, to: destination, type: type, quality: quality, metadataSource: source, page: page)
            }
            DispatchQueue.main.async {
                self?.didSave(to: destination, result: result)
            }
        }
    }

    private func didSave(to destination: URL, result: Result<Void, Error>) {
        switch result {
        case .success:
            pipeline.invalidate(destination)
            showMessage("Saved “\(destination.lastPathComponent)”")
            if destination.standardizedFileURL == navigator.current?.standardizedFileURL {
                // The edits are in the file now; reload it clean.
                adjustments = Adjustments()
                adjustmentsPanel?.update(adjustments)
                showCurrent(keepPage: true)
            }
        case .failure(let error):
            hideMessage()
            presentErrorSheet(error)
        }
    }

    // MARK: - File actions

    @objc func copy(_ sender: Any?) {
        guard let url = navigator.current else { return }
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        var objects: [NSPasteboardWriting] = []
        if !hasEdits { objects.append(url as NSURL) }
        if let image = imageView.image {
            objects.append(NSImage(cgImage: image, size: .zero))
        }
        pasteboard.writeObjects(objects)
    }

    @objc func revealInFinder(_ sender: Any?) {
        guard let url = navigator.current else { return }
        NSWorkspace.shared.activateFileViewerSelecting([url])
    }

    @objc func setAsDesktopPicture(_ sender: Any?) {
        guard let url = navigator.current, let screen = window?.screen ?? NSScreen.main else { return }
        do {
            try NSWorkspace.shared.setDesktopImageURL(url, for: screen, options: [:])
            showMessage("Set as desktop picture")
        } catch {
            presentErrorSheet(error)
        }
    }

    @objc func moveToTrash(_ sender: Any?) {
        guard let url = navigator.current else { return }
        if showingVideo { videoView?.close() }  // mpv lets go of the file first
        do {
            try FileManager.default.trashItem(at: url, resultingItemURL: nil)
        } catch {
            presentErrorSheet(error)
            return
        }
        pipeline.invalidate(url)
        navigator.removeCurrent()
        showMessage("Moved “\(url.lastPathComponent)” to the Trash")
        showCurrent()
    }

    // MARK: - More JPEGView commands

    @objc func reloadImage(_ sender: Any?) {
        guard let url = navigator.current else { return }
        pipeline.invalidate(url)
        showCurrent(keepPage: true)
    }

    /// Renders the current picture at full resolution with every edit applied, then hands it to
    /// `body` on the main thread (if the user is still on the same picture).
    private func withEditedFullImage(_ message: String, _ body: @escaping (CGImage) -> Void) {
        guard let current else { return }
        let source = current.info.url
        let page = current.page
        let edited = editedImage
        let adjustments = self.adjustments
        let orientation = imageView.orientation
        let gen = generation
        let layout = settings.pdfLayout
        let password = pipeline.password(for: source)
        showMessage(message, duration: nil)
        renderQueue.async { [weak self] in
            var image = edited ?? ImageDecoder.decodeFull(source, page: page, layout: layout, password: password)
            if let base = image, !adjustments.isIdentity || orientation != .identity {
                image = ImageRenderer.shared.renderEdited(base, adjustments: adjustments, orientation: orientation, crop: nil, autoKey: source.path)
            }
            DispatchQueue.main.async {
                guard let self, gen == self.generation else { return }
                self.hideMessage()
                guard let image else {
                    self.showMessage("Couldn't read the image")
                    return
                }
                body(image)
            }
        }
    }

    @objc func printImage(_ sender: Any?) {
        withEditedFullImage("Preparing to print…") { [weak self] image in
            guard let window = self?.window else { return }
            let printInfo = NSPrintInfo.shared
            printInfo.orientation = image.width > image.height ? .landscape : .portrait
            printInfo.horizontalPagination = .fit
            printInfo.verticalPagination = .fit
            printInfo.isHorizontallyCentered = true
            printInfo.isVerticallyCentered = true
            let page = printInfo.imageablePageBounds
            let view = NSImageView(frame: NSRect(origin: .zero, size: page.size))
            view.image = NSImage(cgImage: image, size: .zero)
            view.imageScaling = .scaleProportionallyUpOrDown
            NSPrintOperation(view: view, printInfo: printInfo)
                .runModal(for: window, delegate: nil, didRun: nil, contextInfo: nil)
        }
    }

    @objc func setModificationDateToNow(_ sender: Any?) {
        setModificationDate(Date())
    }

    @objc func setModificationDateToDateTaken(_ sender: Any?) {
        let exif = current?.info.properties[kCGImagePropertyExifDictionary] as? [CFString: Any]
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyy:MM:dd HH:mm:ss"
        guard let text = exif?[kCGImagePropertyExifDateTimeOriginal] as? String, let date = formatter.date(from: text) else {
            showMessage("This image has no date taken")
            return
        }
        setModificationDate(date)
    }

    private func setModificationDate(_ date: Date) {
        guard let url = navigator.current else { return }
        do {
            try FileManager.default.setAttributes([.modificationDate: date], ofItemAtPath: url.path)
            showMessage("Modification date set")
        } catch {
            presentErrorSheet(error)
        }
    }

    @objc func copyOriginalSize(_ sender: Any?) {
        withEditedFullImage("Copying…") { [weak self] image in
            let pasteboard = NSPasteboard.general
            pasteboard.clearContents()
            pasteboard.writeObjects([NSImage(cgImage: image, size: .zero)])
            self?.showMessage("Copied \(image.width) × \(image.height) image")
        }
    }

    @objc func copyFilePath(_ sender: Any?) {
        guard let url = navigator.current else { return }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(url.path, forType: .string)
        showMessage("Copied file path")
    }

    /// Opens copied files, or a copied image (saved to a temporary file first, since everything
    /// here works on files).
    @objc func paste(_ sender: Any?) {
        let pasteboard = NSPasteboard.general
        if let urls = pasteboard.readObjects(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) as? [URL], !urls.isEmpty {
            open(urls)
            return
        }
        guard let image = NSImage(pasteboard: pasteboard),
              let tiff = image.tiffRepresentation,
              let png = NSBitmapImageRep(data: tiff)?.representation(using: .png, properties: [:]) else {
            NSSound.beep()
            return
        }
        // A folder of its own, so navigation doesn't wander into other temporary files.
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent("Glance Pasted \(UUID().uuidString)")
        let url = folder.appendingPathComponent("Pasted Image.png")
        do {
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
            try png.write(to: url)
            open([url])
        } catch {
            presentErrorSheet(error)
        }
    }

    @objc func toggleFilename(_ sender: Any?) {
        settings.showFilename.toggle()
    }

    @objc func toggleNavigationControls(_ sender: Any?) {
        settings.showControls.toggle()
    }

    @objc func setStartZoom(_ sender: NSMenuItem) {
        settings.startZoom = StartZoom(rawValue: sender.tag) ?? .fit
        imageView.defaultZoomMode = settings.startZoom.zoomMode
        switch settings.startZoom {
        case .fit: imageView.zoomToFit()
        case .fill: imageView.zoomToFill()
        case .actual: imageView.zoomToActualSize()
        }
    }

    /// Rotates or flips the file itself by rewriting its orientation tag (JPEGView's lossless
    /// JPEG transformations). Tags: 0 right, 1 left, 2 180°, 3 flip horizontal, 4 flip vertical.
    @objc func losslessTransform(_ sender: NSMenuItem) {
        guard let current else { return }
        guard editedImage == nil, adjustments.isIdentity, imageView.orientation == .identity else {
            showMessage("Revert other edits first")
            return
        }
        let transform: CGAffineTransform
        switch sender.tag {
        case 0: transform = CGAffineTransform(rotationAngle: -.pi / 2)
        case 1: transform = CGAffineTransform(rotationAngle: .pi / 2)
        case 2: transform = CGAffineTransform(rotationAngle: .pi)
        case 3: transform = CGAffineTransform(scaleX: -1, y: 1)
        default: transform = CGAffineTransform(scaleX: 1, y: -1)
        }
        let url = current.info.url
        renderQueue.async { [weak self] in
            let result = Result { try ImageSaver.rewriteOrientation(of: url, to: url, applying: transform.roundedLinear) }
            DispatchQueue.main.async {
                guard let self else { return }
                switch result {
                case .success(true):
                    self.pipeline.invalidate(url)
                    if self.navigator.current == url { self.showCurrent(keepPage: true) }
                    self.showMessage("Saved losslessly")
                case .success(false):
                    self.showMessage("This format can't be transformed losslessly")
                case .failure(let error):
                    self.presentErrorSheet(error)
                }
            }
        }
    }

    @objc func saveParameters(_ sender: Any?) {
        guard let url = navigator.current else { return }
        if adjustments.isIdentity {
            ParameterStore.clear(for: url)
        } else {
            ParameterStore.save(adjustments, for: url)
        }
        showMessage("Adjustments saved for this image")
    }

    @objc func clearParameters(_ sender: Any?) {
        guard let url = navigator.current else { return }
        ParameterStore.clear(for: url)
        showMessage("Saved adjustments cleared")
    }

    // MARK: - Videos

    private var videoOptions: VideoView.Options {
        VideoView.Options(autoplay: settings.videoAutoplay, previews: settings.videoTimelinePictures,
                          loopShort: settings.videoLoopShortClips, alwaysSubtitles: settings.alwaysSubtitles,
                          subtitleLanguage: settings.subtitleLanguage, volume: settings.videoVolume,
                          muted: settings.videoMuted)
    }

    /// The player view, made the first time it's needed; nil without libmpv.
    private func makeVideoView() -> VideoView? {
        if let videoView { return videoView }
        guard MPV.available, let content = window?.contentView, let view = VideoView(delegate: self) else { return nil }
        view.frame = content.bounds
        view.autoresizingMask = [.width, .height]
        view.isHidden = true
        view.menu = contextMenu
        view.options = videoOptions
        content.addSubview(view, positioned: .above, relativeTo: imageView)
        videoView = view
        return view
    }

    private func showVideo(_ url: URL) {
        // Nothing of the picture machinery applies: no decoding, zoom or adjustments.
        current = nil
        editedImage = nil
        imageView.setImage(nil, fullSize: .zero, resetView: true)
        setControlsVisible(false)
        if messageIsSticky { hideMessage() }
        guard let view = makeVideoView() else {
            showMessage("Can't play “\(url.lastPathComponent)”", duration: nil)
            return
        }
        showingVideo = true
        view.isHidden = false
        // Plays at once, except while a key is held down to skim: then it waits until you stop.
        let playNow = settings.videoAutoplay && !keyRepeating
        view.open(url, play: playNow)
        videoPlayTimer?.invalidate()
        if settings.videoAutoplay && !playNow {
            videoPlayTimer = Timer.scheduledTimer(withTimeInterval: 0.3, repeats: false) { [weak self] _ in
                guard let self, self.showingVideo, self.navigator.current == url else { return }
                self.videoView?.play()
            }
        }
        window?.makeFirstResponder(imageView)  // keys stay with Glance
        updateTitle()
        updateInfo()
        prefetchNeighbors()
    }

    private func leaveVideo() {
        videoPlayTimer?.invalidate()
        showingVideo = false
        videoView?.close()
        videoView?.isHidden = true
    }

    private func handleVideoKey(_ scalar: Unicode.Scalar, shift: Bool, video: VideoView) -> Bool {
        switch Int(scalar.value) {
        case 0x20: video.togglePause()
        // Arrows move through the video; Page Up/Down and the wheel still change file.
        case NSLeftArrowFunctionKey: video.seek(by: shift ? -30 : -5)
        case NSRightArrowFunctionKey: video.seek(by: shift ? 30 : 5)
        case NSUpArrowFunctionKey: video.changeVolume(by: 5)
        case NSDownArrowFunctionKey: video.changeVolume(by: -5)
        default:
            switch Character(scalar) {
            case "m", "M": video.toggleMute()
            case "j": video.cycleSubtitles(forward: true)  // as in mpv
            case "J": video.cycleSubtitles(forward: false)
            case "v", "V": video.toggleSubtitles()
            case ",": video.frameStep(forward: false)
            case ".": video.frameStep(forward: true)
            default: return false
            }
        }
        return true
    }

    @objc func togglePlayPause(_ sender: Any?) { videoView?.togglePause() }
    @objc func toggleVideoMute(_ sender: Any?) { videoView?.toggleMute() }
    @objc func chooseSubtitle(_ sender: NSMenuItem) { videoView?.selectSubtitle(sender.tag, announce: true) }
    @objc func chooseAudioTrack(_ sender: NSMenuItem) { videoView?.selectAudio(sender.tag) }
    @objc func loadSubtitleFile(_ sender: Any?) { videoView?.loadSubtitleFile() }

    @objc func nextDocument(_ sender: Any?) { goToFile(1) }
    @objc func previousDocument(_ sender: Any?) { goToFile(-1) }

    private func goToFile(_ delta: Int) {
        direction = delta < 0 ? -1 : 1
        guard navigator.move(by: delta) else {
            NSSound.beep()
            return
        }
        showCurrent()
    }

    // MARK: VideoViewDelegate

    var videoViewIsFullScreen: Bool { isFullScreen }

    func videoViewToggleFullScreen(_ view: VideoView) { toggleFullScreenMode(nil) }

    func videoViewDidLoadFile(_ view: VideoView) { updateTitle() }

    func videoView(_ view: VideoView, scrollWheel event: NSEvent) {
        // Mouse wheels only: trackpad scrolling does nothing on a video.
        guard !event.hasPreciseScrollingDeltas, event.scrollingDeltaY != 0 else { return }
        if settings.wheelAction == .navigate && !event.modifierFlags.contains(.command) {
            go(event.scrollingDeltaY > 0 ? -1 : 1)
        } else {
            view.changeVolume(by: event.scrollingDeltaY > 0 ? 5 : -5)  // where pictures would zoom, videos get louder
        }
    }

    // MARK: NSMenuDelegate

    func menuNeedsUpdate(_ menu: NSMenu) {
        guard menu === contextMenu else { return }
        menu.removeAllItems()
        let kind: MainMenu.ContextKind = showingVideo ? .video : currentPDF != nil ? .pdf : .picture
        var tracks: (subtitles: [NSMenuItem], audio: [NSMenuItem])?
        if kind == .video, let videoView, videoView.isOpen {
            tracks = videoView.trackMenuItems(target: self, subtitleAction: #selector(chooseSubtitle(_:)),
                                              audioAction: #selector(chooseAudioTrack(_:)), loadAction: #selector(loadSubtitleFile(_:)))
        }
        let hasPages = (current?.info.pageCount ?? 0) > 1 && current?.info.isAnimated == false && current?.info.pdf == nil
        MainMenu.contextItems(for: kind, hasPages: hasPages, tracks: tracks).forEach { menu.addItem($0) }
    }

    // MARK: - Menu validation

    func validateMenuItem(_ item: NSMenuItem) -> Bool {
        let hasImage = current != nil
        switch item.action {
        case #selector(togglePlayPause(_:)):
            item.title = videoView?.isPlaying == true ? "Pause" : "Play"
            return showingVideo
        case #selector(toggleVideoMute(_:)), #selector(chooseSubtitle(_:)), #selector(chooseAudioTrack(_:)), #selector(loadSubtitleFile(_:)):
            return showingVideo
        case #selector(nextDocument(_:)), #selector(previousDocument(_:)):
            return navigator.count > 1
        case #selector(revealInFinder(_:)), #selector(copyFilePath(_:)), #selector(setModificationDateToNow(_:)), #selector(moveToTrash(_:)):
            return navigator.current != nil
        case #selector(openDocument(_:)):
            return true
        case #selector(toggleFullScreenMode(_:)):
            item.title = isFullScreen ? "Exit Full Screen" : "Enter Full Screen"
            return true
        case #selector(toggleInfo(_:)):
            item.state = showInfo ? .on : .off
            return true
        case #selector(toggleFilename(_:)):
            item.state = settings.showFilename ? .on : .off
            return true
        case #selector(toggleNavigationControls(_:)):
            item.state = settings.showControls ? .on : .off
            return true
        case #selector(setStartZoom(_:)):
            item.state = item.tag == settings.startZoom.rawValue ? .on : .off
            return true
        case #selector(paste(_:)):
            return NSPasteboard.general.canReadObject(forClasses: [NSURL.self, NSImage.self], options: nil)
        case #selector(clearParameters(_:)):
            return navigator.current.map { ParameterStore.adjustments(for: $0) != nil } ?? false
        case #selector(losslessTransform(_:)), #selector(setAsDesktopPicture(_:)):
            return hasImage && current?.info.pdf == nil && (item.action != #selector(losslessTransform(_:)) || !hasEdits)
        case #selector(setPDFPageMode(_:)):
            item.state = item.tag == settings.pdfPageMode.rawValue ? .on : .off
            return true
        case #selector(togglePDFRightToLeft(_:)):
            item.state = settings.pdfRightToLeft ? .on : .off
            return true
        case #selector(toggleAutoCorrect(_:)):
            item.state = adjustments.autoCorrect ? .on : .off
            return hasImage
        case #selector(toggleKeepAdjustments(_:)):
            item.state = keepAdjustments ? .on : .off
            return true
        case #selector(toggleEnlargeSmall(_:)):
            item.state = imageView.enlargeSmallImages ? .on : .off
            return true
        case #selector(toggleLoop(_:)):
            item.state = navigator.wraps ? .on : .off
            return true
        case #selector(toggleSortDescending(_:)):
            item.state = navigator.descending ? .on : .off
            return true
        case #selector(setSortOrder(_:)):
            item.state = item.tag == navigator.sortOrder.rawValue ? .on : .off
            return true
        case #selector(setSlideshowInterval(_:)):
            item.state = item.tag == Int(slideshowInterval) ? .on : .off
            return true
        case #selector(toggleSlideshow(_:)):
            item.title = slideshowTimer == nil ? "Start Slideshow" : "Stop Slideshow"
            return navigator.count > 1
        case #selector(cropToSelection(_:)), #selector(zoomToSelection(_:)):
            return imageView.selection != nil
        case #selector(saveImageAs(_:)):
            return hasImage
        case #selector(nextPage(_:)), #selector(previousPage(_:)):
            return (current?.info.pageCount ?? 0) > 1 && current?.info.isAnimated == false
        case #selector(saveImage(_:)), #selector(resetEdits(_:)):
            return hasImage && hasEdits
        case #selector(nextImage(_:)), #selector(previousImage(_:)):
            return navigator.count > 1 || (currentPDF?.info.pageCount ?? 0) > 1  // in a PDF they turn pages
        case #selector(firstImage(_:)), #selector(lastImage(_:)):
            return navigator.count > 1
        default:
            return hasImage
        }
    }

    // MARK: - Overlays

    private func updateTitle() {
        guard let window else { return }
        guard let url = navigator.current else {
            window.title = "Glance"
            window.subtitle = ""
            window.representedURL = nil
            return
        }
        window.title = url.lastPathComponent
        window.representedURL = url
        var parts = ["\(navigator.index + 1) / \(navigator.count)"]
        if let info = current?.info, info.url == url {
            let size = displayFullSize
            parts.append("\(Int(size.width)) × \(Int(size.height))")
            if let pdf = info.pdf {
                parts.append(pdf.pagesText.lowercased())
            } else if info.pageCount > 1 && !info.isAnimated {
                parts.append("page \(page + 1)/\(info.pageCount)")
            }
            parts.append("\(Int((imageView.zoom * 100).rounded()))%")
        } else if showingVideo, let details = videoView?.details {
            parts.append(details)
        }
        window.subtitle = parts.joined(separator: "  ·  ")
        if settings.showFilename && !showInfo { updateInfo() }
    }

    private func updateInfo() {
        let position = "\(navigator.index + 1) / \(navigator.count)"
        if !showInfo, settings.showFilename, let url = navigator.current {
            // File name only (JPEGView's "Show filename").
            infoBox.isHidden = false
            infoLabel.stringValue = "\(url.lastPathComponent)   \(position)"
            return
        }
        guard showInfo, let current else {
            infoBox.isHidden = true
            return
        }
        infoBox.isHidden = false
        infoLabel.stringValue = InfoFormatter.text(
            for: current.info,
            position: position,
            size: displayFullSize,
            zoom: imageView.zoom,
            page: page,
            edited: hasEdits
        )
    }

    private func showMessage(_ text: String, duration: TimeInterval? = 1.2) {
        messageLabel.stringValue = "   \(text)   "
        messageLabel.isHidden = false
        messageHideTimer?.invalidate()
        messageIsSticky = duration == nil
        if let duration {
            messageHideTimer = Timer.scheduledTimer(withTimeInterval: duration, repeats: false) { [weak self] _ in
                self?.hideMessage()
            }
        }
    }

    private func hideMessage() {
        messageHideTimer?.invalidate()
        messageIsSticky = false
        messageLabel.isHidden = true
    }

    private func setControlsVisible(_ visible: Bool) {
        if visible {
            guard controlBar.isHidden else { return }
            controlBar.isHidden = false
            NSAnimationContext.runAnimationGroup { context in
                context.duration = 0.15
                self.controlBar.animator().alphaValue = 1
            }
        } else {
            guard !controlBar.isHidden else { return }
            NSAnimationContext.runAnimationGroup({ context in
                context.duration = 0.25
                self.controlBar.animator().alphaValue = 0
            }, completionHandler: { [weak self] in
                // A transparent view still takes clicks, so hide it for real.
                if self?.controlBar.alphaValue == 0 { self?.controlBar.isHidden = true }
            })
        }
    }

    private func presentErrorSheet(_ error: Error) {
        let alert = NSAlert(error: error)
        if let window {
            alert.beginSheetModal(for: window)
        } else {
            alert.runModal()
        }
    }
}
