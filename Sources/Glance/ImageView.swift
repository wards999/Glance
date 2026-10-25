import AppKit
import QuartzCore

protocol ImageViewDelegate: AnyObject {
    func imageViewDidChangeZoom(_ view: ImageView)
    /// The view is magnifying its current bitmap and wants one with at least `pixels` on its long side.
    func imageView(_ view: ImageView, needsLongSide pixels: Int)
    func imageView(_ view: ImageView, handleKeyDown event: NSEvent) -> Bool
    func imageView(_ view: ImageView, navigate delta: Int)
    func imageView(_ view: ImageView, didReceiveDrop urls: [URL])
    func imageViewMouseMoved(_ view: ImageView, at point: NSPoint)
}

enum ZoomMode {
    case fit, fill, actual, custom
}

/// Shows one picture with Core Animation. The decoded bitmap goes to the GPU once; after that
/// zooming, panning and rotating only change a layer transform, so they cost nothing per frame.
///
/// Positions are in the oriented full-resolution image's pixels (y down), whatever bitmap is on
/// screen, so swapping a screen-sized decode for a sharper one doesn't move anything.
final class ImageView: NSView {
    weak var delegate: ImageViewDelegate?
    var enlargeSmallImages = false {
        didSet { if zoomMode == .fit && oldValue != enlargeSmallImages { applyZoomAndNotify() } }
    }
    /// Zoom mode each new picture opens in.
    var defaultZoomMode = ZoomMode.fit
    /// A (non-trackpad) mouse wheel flips images instead of zooming; ⌘ still zooms.
    var wheelNavigates = false
    /// Nearest-neighbour filtering when blown up past 400%, so individual pixels are visible.
    var pixelatedZoom = true {
        didSet { layoutImage() }
    }
    var backgroundColor = NSColor.black {
        didSet { hostLayer.backgroundColor = backgroundColor.cgColor }
    }

    private(set) var image: CGImage?
    /// EXIF-oriented size of the full-resolution picture.
    private(set) var fullSize = CGSize.zero
    /// User rotation/flip, as a linear transform in y-up layer space.
    private(set) var orientation = CGAffineTransform.identity
    private(set) var zoomMode = ZoomMode.fit
    /// Device pixels per full-resolution image pixel; 1 means 100%.
    private(set) var zoom: CGFloat = 1
    /// Crop selection in oriented image pixels.
    private(set) var selection: CGRect?

    /// The image point shown at the middle of the view.
    private var center = CGPoint.zero
    private let hostLayer = CALayer()
    private let container = CALayer()
    private var tiles: [CALayer] = []
    private let selectionLayer = CAShapeLayer()
    private var usesNearestFilter = false

    private var lastDragPoint: NSPoint?
    private var selectionAnchor: CGPoint?
    /// Applied by the next `setImage(…, resetView: true)` instead of the default zoom: keeps the zoom
    /// and puts that corner of the new picture in view (clamping turns it into an edge-aligned view).
    var carriedZoom: (zoom: CGFloat, fromRight: Bool, fromBottom: Bool)?

    /// Press and hold without moving to magnify; let go to return. The controller turns it on
    /// per picture (PDFs, or everything, per Settings).
    var holdToZoomEnabled = false
    var holdZoomFactor: CGFloat = 2.5
    private static let holdDelay: TimeInterval = 0.2
    private var holdWork: DispatchWorkItem?
    private var holdStart: NSPoint?
    /// The view to return to when the mouse is released.
    private var magnifyState: (zoom: CGFloat, center: CGPoint, mode: ZoomMode)?

    /// Option-drag: the rectangle to zoom into, in oriented image pixels, while it's being drawn.
    private var zoomBoxAnchor: CGPoint?
    private var zoomBox: CGRect?
    private var didDrag = false
    private var pushedCursor = false
    private var swipeDistance: CGFloat = 0
    private var swipeHandled = false

    /// Bitmaps larger than this are split into tiles. GPUs top out at 16384² textures.
    private static let tileSize = 4096
    private static let maxZoom: CGFloat = 32

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        // Layer-hosting: we own the layer tree and AppKit never redraws it.
        hostLayer.backgroundColor = NSColor.black.cgColor
        layer = hostLayer
        wantsLayer = true

        hostLayer.addSublayer(container)
        selectionLayer.fillColor = NSColor.white.withAlphaComponent(0.08).cgColor
        selectionLayer.strokeColor = NSColor.white.cgColor
        selectionLayer.lineWidth = 1
        selectionLayer.lineDashPattern = [5, 4]
        selectionLayer.isHidden = true
        hostLayer.addSublayer(selectionLayer)

        registerForDraggedTypes([.fileURL])
        addTrackingArea(NSTrackingArea(rect: .zero, options: [.mouseMoved, .activeInKeyWindow, .inVisibleRect], owner: self, userInfo: nil))
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    override var acceptsFirstResponder: Bool { true }
    override var isOpaque: Bool { true }
    override var mouseDownCanMoveWindow: Bool { false }

    var backingScale: CGFloat { window?.backingScaleFactor ?? NSScreen.main?.backingScaleFactor ?? 2 }

    var orientedSize: CGSize {
        abs(orientation.b) > 0.5 ? CGSize(width: fullSize.height, height: fullSize.width) : fullSize
    }

    var canPanHorizontally: Bool { orientedSize.width * pointsPerPixel > bounds.width + 0.5 }
    var canPanVertically: Bool { orientedSize.height * pointsPerPixel > bounds.height + 0.5 }

    private var pointsPerPixel: CGFloat { zoom / backingScale }

    // MARK: - Content

    /// Shows `image`, a rendering of the picture whose full resolution is `fullSize`.
    /// `resetView` means a different picture: forget zoom, pan, rotation and selection.
    func setImage(_ image: CGImage?, fullSize: CGSize, resetView: Bool) {
        self.image = image
        self.fullSize = fullSize
        if resetView {
            orientation = .identity
            selection = nil
            zoomMode = defaultZoomMode
            // A different picture: any hold or magnifier belonged to the old one.
            cancelHold()
            magnifyState = nil
        }
        rebuildTiles()
        if resetView || zoomMode != .custom {
            applyZoomMode()
        } else {
            clampCenter()
        }
        if resetView, let carried = carriedZoom {
            // Turning a page while zoomed in: same zoom, opened at the corner where reading continues.
            carriedZoom = nil
            if image != nil {
                zoom = carried.zoom
                zoomMode = .custom
                let size = orientedSize
                center = CGPoint(x: carried.fromRight ? size.width : 0, y: carried.fromBottom ? size.height : 0)
                clampCenter()
            }
        }
        layoutImage()
        delegate?.imageViewDidChangeZoom(self)
        requestResolutionIfNeeded()
    }

    func rotate(clockwise: Bool) {
        applyOrientation(CGAffineTransform(rotationAngle: clockwise ? -.pi / 2 : .pi / 2))
    }

    func flip(horizontal: Bool) {
        applyOrientation(horizontal ? CGAffineTransform(scaleX: -1, y: 1) : CGAffineTransform(scaleX: 1, y: -1))
    }

    func clearSelection() {
        selection = nil
        updateSelectionLayer()
    }

    // MARK: - Zoom

    func zoomToFit() { zoomMode = .fit; applyZoomAndNotify() }
    func zoomToFill() { zoomMode = .fill; applyZoomAndNotify() }
    func zoomToActualSize() { setZoom(1, around: nil) }
    func zoomIn() { step(by: 1.25) }
    func zoomOut() { step(by: 0.8) }

    /// Zooms so `rect` (oriented image pixels) fills the window.
    func zoom(to rect: CGRect) {
        guard image != nil, rect.width >= 1, rect.height >= 1, bounds.width > 0, bounds.height > 0 else { return }
        let lowest = min(fitZoom(), 1) / 8
        let target = min(bounds.width / rect.width, bounds.height / rect.height) * backingScale
        zoom = min(max(target, lowest), Self.maxZoom)
        zoomMode = .custom
        center = CGPoint(x: rect.midX, y: rect.midY)
        clampCenter()
        layoutImage()
        delegate?.imageViewDidChangeZoom(self)
        requestResolutionIfNeeded()
    }

    /// Returns false when there's no selection to zoom to.
    @discardableResult
    func zoomToSelection() -> Bool {
        guard let selection else { return false }
        zoom(to: selection)
        return true
    }

    func setZoom(_ newZoom: CGFloat, around viewPoint: NSPoint?) {
        guard image != nil else { return }
        let lowest = min(fitZoom(), 1) / 8
        let clamped = min(max(newZoom, lowest), Self.maxZoom)
        let anchor = viewPoint ?? NSPoint(x: bounds.midX, y: bounds.midY)
        let pinned = imagePoint(fromView: anchor)

        zoom = clamped
        zoomMode = .custom
        // Keep the image point under `anchor` where it is.
        center = CGPoint(
            x: pinned.x - (anchor.x - bounds.midX) / pointsPerPixel,
            y: pinned.y + (anchor.y - bounds.midY) / pointsPerPixel
        )
        clampCenter()
        layoutImage()
        delegate?.imageViewDidChangeZoom(self)
        requestResolutionIfNeeded()
    }

    /// Moves the viewport by view points; positive y shows more of the bottom of the image.
    func scroll(dx: CGFloat, dy: CGFloat) {
        guard image != nil else { return }
        // Once panned, a fill or 100% view is no longer re-centred when the window resizes.
        if zoomMode == .fill || zoomMode == .actual { zoomMode = .custom }
        center.x += dx / pointsPerPixel
        center.y += dy / pointsPerPixel
        clampCenter()
        layoutImage()
    }

    private func step(by factor: CGFloat) {
        var target = zoom * factor
        // Stop at 100% on the way through.
        if (zoom < 1 && target > 1) || (zoom > 1 && target < 1) { target = 1 }
        let fit = fitZoom()
        if factor < 1 && zoom > fit + 0.0001 && target < fit {
            zoomToFit()
            return
        }
        setZoom(target, around: nil)
    }

    private func toggleZoom(at point: NSPoint) {
        let fit = fitZoom()
        if zoomMode == .fit || abs(zoom - fit) < 0.001 {
            setZoom(abs(fit - 1) < 0.01 ? 2 : 1, around: point)
        } else {
            zoomToFit()
        }
    }

    private func fitZoom() -> CGFloat {
        let size = orientedSize
        guard size.width > 0, size.height > 0, bounds.width > 0, bounds.height > 0 else { return 1 }
        let fit = min(bounds.width / size.width, bounds.height / size.height) * backingScale
        return enlargeSmallImages ? fit : min(fit, 1)
    }

    private func fillZoom() -> CGFloat {
        let size = orientedSize
        guard size.width > 0, size.height > 0 else { return 1 }
        return max(bounds.width / size.width, bounds.height / size.height) * backingScale
    }

    private func applyZoomMode() {
        switch zoomMode {
        case .fit:
            zoom = fitZoom()
            center = CGPoint(x: orientedSize.width / 2, y: orientedSize.height / 2)
        case .fill:
            zoom = fillZoom()
            center = CGPoint(x: orientedSize.width / 2, y: orientedSize.height / 2)
        case .actual:
            zoom = 1
            center = CGPoint(x: orientedSize.width / 2, y: orientedSize.height / 2)
        case .custom:
            break
        }
        clampCenter()
    }

    private func applyZoomAndNotify() {
        applyZoomMode()
        layoutImage()
        delegate?.imageViewDidChangeZoom(self)
        requestResolutionIfNeeded()
    }

    private func applyOrientation(_ transform: CGAffineTransform) {
        guard image != nil else { return }
        orientation = orientation.concatenating(transform).roundedLinear
        selection = nil
        center = CGPoint(x: orientedSize.width / 2, y: orientedSize.height / 2)
        applyZoomMode()
        layoutImage()
        delegate?.imageViewDidChangeZoom(self)
    }

    private func clampCenter() {
        let size = orientedSize
        let scale = pointsPerPixel
        guard scale > 0 else { return }
        let halfWidth = bounds.width / 2 / scale
        let halfHeight = bounds.height / 2 / scale
        center.x = size.width <= halfWidth * 2 ? size.width / 2 : min(max(center.x, halfWidth), size.width - halfWidth)
        center.y = size.height <= halfHeight * 2 ? size.height / 2 : min(max(center.y, halfHeight), size.height - halfHeight)
    }

    /// Asks for a sharper decode once the current bitmap is being magnified.
    private func requestResolutionIfNeeded() {
        guard let image else { return }
        let fullLong = max(fullSize.width, fullSize.height)
        let have = CGFloat(max(image.width, image.height))
        guard have < fullLong - 1 else { return }
        let needed = fullLong * min(zoom, 1)
        if needed > have * 1.02 {
            delegate?.imageView(self, needsLongSide: Int(needed.rounded(.up)))
        }
    }

    // MARK: - Coordinates

    func imagePoint(fromView point: NSPoint) -> CGPoint {
        CGPoint(
            x: center.x + (point.x - bounds.midX) / pointsPerPixel,
            y: center.y - (point.y - bounds.midY) / pointsPerPixel
        )
    }

    func viewPoint(fromImage point: CGPoint) -> NSPoint {
        NSPoint(
            x: bounds.midX + (point.x - center.x) * pointsPerPixel,
            y: bounds.midY - (point.y - center.y) * pointsPerPixel
        )
    }

    private func clampToImage(_ point: CGPoint) -> CGPoint {
        let size = orientedSize
        return CGPoint(x: min(max(point.x, 0), size.width), y: min(max(point.y, 0), size.height))
    }

    // MARK: - Layers

    private func rebuildTiles() {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        defer { CATransaction.commit() }

        tiles.forEach { $0.removeFromSuperlayer() }
        tiles.removeAll()
        guard let image else {
            container.isHidden = true
            return
        }
        container.isHidden = false
        let width = image.width
        let height = image.height
        container.bounds = CGRect(x: 0, y: 0, width: width, height: height)

        // Screen-sized bitmaps are one layer; huge full-resolution ones are split.
        // Cropping a decoded CGImage shares its pixels, so tiling copies nothing.
        let tile = max(width, height) <= Self.tileSize * 2 ? max(width, height) : Self.tileSize
        for top in stride(from: 0, to: height, by: tile) {
            for left in stride(from: 0, to: width, by: tile) {
                let rect = CGRect(x: left, y: top, width: min(tile, width - left), height: min(tile, height - top))
                guard let piece = image.cropping(to: rect) else { continue }
                let layer = CALayer()
                layer.contents = piece
                layer.contentsGravity = .resize
                layer.minificationFilter = .trilinear
                layer.magnificationFilter = usesNearestFilter ? .nearest : .linear
                // CGImage rows run top-down; the container's y axis runs up.
                layer.frame = CGRect(x: rect.minX, y: CGFloat(height) - rect.maxY, width: rect.width, height: rect.height)
                container.addSublayer(layer)
                tiles.append(layer)
            }
        }
    }

    private func layoutImage() {
        guard let image, fullSize.width > 0, fullSize.height > 0 else {
            updateSelectionLayer()
            return
        }
        let scale = pointsPerPixel
        let size = orientedSize
        // The bitmap may be smaller than the full image; scale it up to full-resolution units.
        let bitmapToFull = max(fullSize.width, fullSize.height) / CGFloat(max(image.width, image.height))
        let layerScale = scale * bitmapToFull

        var position = CGPoint(
            x: bounds.midX + (size.width / 2 - center.x) * scale,
            y: bounds.midY + (center.y - size.height / 2) * scale
        )
        // Snap the image's corner to whole device pixels so 100% is pixel-exact rather than blurred.
        let device = backingScale
        let origin = CGPoint(x: position.x - size.width / 2 * scale, y: position.y - size.height / 2 * scale)
        position.x += (origin.x * device).rounded() / device - origin.x
        position.y += (origin.y * device).rounded() / device - origin.y

        // Show real pixels once they're blown up past 400%.
        let nearest = pixelatedZoom && layerScale * device >= 4
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        container.position = position
        container.setAffineTransform(CGAffineTransform(scaleX: layerScale, y: layerScale).concatenating(orientation))
        if nearest != usesNearestFilter {
            usesNearestFilter = nearest
            tiles.forEach { $0.magnificationFilter = nearest ? .nearest : .linear }
        }
        CATransaction.commit()
        updateSelectionLayer()
    }

    private func updateSelectionLayer() {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        defer { CATransaction.commit() }
        // A zoom box being drawn takes the place of the crop selection until the mouse is released.
        guard let box = zoomBox ?? selection, image != nil else {
            selectionLayer.isHidden = true
            return
        }
        selectionLayer.fillColor = zoomBox == nil ? NSColor.white.withAlphaComponent(0.08).cgColor : nil
        selectionLayer.lineDashPattern = zoomBox == nil ? [5, 4] : nil
        let a = viewPoint(fromImage: CGPoint(x: box.minX, y: box.minY))
        let b = viewPoint(fromImage: CGPoint(x: box.maxX, y: box.maxY))
        let rect = CGRect(x: min(a.x, b.x), y: min(a.y, b.y), width: abs(a.x - b.x), height: abs(a.y - b.y))
        selectionLayer.frame = hostLayer.bounds
        selectionLayer.path = CGPath(rect: rect, transform: nil)
        selectionLayer.isHidden = false
    }

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        if zoomMode != .custom {
            applyZoomMode()
            delegate?.imageViewDidChangeZoom(self)
        } else {
            clampCenter()
        }
        layoutImage()
        // Decodes are sized to the view, so a bigger window (or full screen) may need more pixels.
        requestResolutionIfNeeded()
    }

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        selectionLayer.contentsScale = backingScale
        if zoomMode != .custom { applyZoomMode() }
        layoutImage()
        requestResolutionIfNeeded()
    }

    // MARK: - Mouse and trackpad

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
        let point = convert(event.locationInWindow, from: nil)
        didDrag = false
        zoomBoxAnchor = nil
        if event.modifierFlags.contains(.control), let menu {
            // Control-click is the one-button way to get the right-click menu.
            lastDragPoint = nil
            selectionAnchor = nil
            NSMenu.popUpContextMenu(menu, with: event, for: self)
            return
        }
        lastDragPoint = point
        let canPan = canPanHorizontally || canPanVertically
        let shift = event.modifierFlags.contains(.shift)
        // Dragging draws a zoom rectangle when there's nothing to pan; once zoomed in, drag pans
        // and Option-drag draws the rectangle.
        if image != nil && (event.modifierFlags.contains(.option) || (!canPan && !shift)) {
            zoomBoxAnchor = clampToImage(imagePoint(fromView: point))
            selectionAnchor = nil
        } else if shift, image != nil {
            selectionAnchor = clampToImage(imagePoint(fromView: point))
        } else {
            selectionAnchor = nil
            if canPan {
                NSCursor.closedHand.push()
                pushedCursor = true
            }
        }

        // Hold still for a moment and the page magnifies under the mouse.
        if holdToZoomEnabled, image != nil, event.clickCount == 1,
           event.modifierFlags.intersection([.shift, .option, .command]).isEmpty {
            holdStart = point
            let work = DispatchWorkItem { [weak self] in self?.beginMagnify() }
            holdWork = work
            DispatchQueue.main.asyncAfter(deadline: .now() + Self.holdDelay, execute: work)
        }
    }

    override func mouseDragged(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        if magnifyState != nil {
            followMagnifier(to: point)
            return
        }
        if let start = holdStart {
            // Small jitters don't count; a real move cancels the hold and becomes a drag.
            if hypot(point.x - start.x, point.y - start.y) < 4 { return }
            cancelHold()
        }
        guard let last = lastDragPoint else { return }
        didDrag = true
        if let anchor = zoomBoxAnchor {
            // Crosshair only once it's really a drag, so plain clicks don't flicker the cursor.
            if !pushedCursor {
                NSCursor.crosshair.push()
                pushedCursor = true
            }
            let other = clampToImage(imagePoint(fromView: point))
            zoomBox = CGRect(
                x: min(anchor.x, other.x), y: min(anchor.y, other.y),
                width: abs(anchor.x - other.x), height: abs(anchor.y - other.y)
            )
            updateSelectionLayer()
        } else if let anchor = selectionAnchor {
            let other = clampToImage(imagePoint(fromView: point))
            selection = CGRect(
                x: min(anchor.x, other.x), y: min(anchor.y, other.y),
                width: abs(anchor.x - other.x), height: abs(anchor.y - other.y)
            )
            updateSelectionLayer()
        } else {
            // The picture follows the mouse.
            scroll(dx: last.x - point.x, dy: point.y - last.y)
            lastDragPoint = point
        }
    }

    override func mouseUp(with event: NSEvent) {
        if pushedCursor {
            NSCursor.pop()
            pushedCursor = false
        }
        cancelHold()
        if magnifyState != nil {
            endMagnify()
            lastDragPoint = nil
            zoomBoxAnchor = nil
            selectionAnchor = nil
            return
        }
        let point = convert(event.locationInWindow, from: nil)
        if zoomBoxAnchor != nil {
            let box = zoomBox
            zoomBox = nil
            zoomBoxAnchor = nil
            if didDrag {
                lastDragPoint = nil
                // Ignore an accidental click-sized box; just redraw whatever selection was there.
                if let box, box.width * pointsPerPixel >= 8, box.height * pointsPerPixel >= 8 {
                    zoom(to: box)
                } else {
                    updateSelectionLayer()
                }
                return
            }
            // A plain click: fall through to double-click zoom and click-to-clear.
        }
        if let selection, selection.width < 4 || selection.height < 4 {
            clearSelection()
        }
        if !didDrag {
            if event.clickCount == 2 {
                toggleZoom(at: point)
            } else if selectionAnchor == nil {
                clearSelection()
            }
        }
        lastDragPoint = nil
        selectionAnchor = nil
    }

    // MARK: - Hold to zoom

    private func cancelHold() {
        holdWork?.cancel()
        holdWork = nil
        holdStart = nil
    }

    private func beginMagnify() {
        holdWork = nil
        holdStart = nil
        guard image != nil, magnifyState == nil else { return }
        // The press becomes a magnifier, not a drag.
        zoomBoxAnchor = nil
        zoomBox = nil
        selectionAnchor = nil
        updateSelectionLayer()

        magnifyState = (zoom, center, zoomMode)
        zoom = min(zoom * holdZoomFactor, Self.maxZoom)
        zoomMode = .custom
        let mouse = window.map { convert($0.mouseLocationOutsideOfEventStream, from: nil) } ?? NSPoint(x: bounds.midX, y: bounds.midY)
        followMagnifier(to: mouse)
        delegate?.imageViewDidChangeZoom(self)
        // Ask for sharp pixels for the magnified view; they replace the stretched ones when ready.
        requestResolutionIfNeeded()
    }

    /// Moving across the window moves across the whole page: the point that was under the mouse
    /// before magnifying stays under it.
    private func followMagnifier(to point: NSPoint) {
        guard let saved = magnifyState else { return }
        let savedScale = saved.zoom / backingScale
        let target = CGPoint(
            x: saved.center.x + (point.x - bounds.midX) / savedScale,
            y: saved.center.y - (point.y - bounds.midY) / savedScale
        )
        center = CGPoint(
            x: target.x - (point.x - bounds.midX) / pointsPerPixel,
            y: target.y + (point.y - bounds.midY) / pointsPerPixel
        )
        clampCenter()
        layoutImage()
    }

    private func endMagnify() {
        guard let saved = magnifyState else { return }
        magnifyState = nil
        zoom = saved.zoom
        center = saved.center
        zoomMode = saved.mode
        if zoomMode != .custom { applyZoomMode() } else { clampCenter() }
        layoutImage()
        delegate?.imageViewDidChangeZoom(self)
    }

    override func scrollWheel(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        if !event.hasPreciseScrollingDeltas && wheelNavigates && !event.modifierFlags.contains(.command) {
            if event.scrollingDeltaY != 0 {
                delegate?.imageView(self, navigate: event.scrollingDeltaY > 0 ? -1 : 1)
            }
            return
        }
        if !event.hasPreciseScrollingDeltas || event.modifierFlags.contains(.command) {
            // A mouse wheel (or ⌘-scroll on a trackpad) zooms around the cursor, as in JPEGView.
            let notches = event.hasPreciseScrollingDeltas ? event.scrollingDeltaY / 20 : event.scrollingDeltaY
            guard notches != 0 else { return }
            setZoom(zoom * pow(1.2, notches), around: point)
            return
        }
        guard event.momentumPhase.isEmpty else { return }
        if canPanHorizontally || canPanVertically {
            scroll(dx: -event.scrollingDeltaX, dy: -event.scrollingDeltaY)
            return
        }
        // Nothing to pan: a horizontal two-finger swipe flips through the folder.
        if event.phase.contains(.began) {
            swipeDistance = 0
            swipeHandled = false
        }
        swipeDistance += event.scrollingDeltaX
        if !swipeHandled && abs(swipeDistance) > 60 {
            swipeHandled = true
            delegate?.imageView(self, navigate: swipeDistance < 0 ? 1 : -1)
        }
    }

    override func magnify(with event: NSEvent) {
        setZoom(zoom * (1 + event.magnification), around: convert(event.locationInWindow, from: nil))
    }

    override func smartMagnify(with event: NSEvent) {
        toggleZoom(at: convert(event.locationInWindow, from: nil))
    }

    override func otherMouseDown(with event: NSEvent) {
        // Mouse back/forward buttons.
        switch event.buttonNumber {
        case 3: delegate?.imageView(self, navigate: -1)
        case 4: delegate?.imageView(self, navigate: 1)
        default: super.otherMouseDown(with: event)
        }
    }

    override func mouseMoved(with event: NSEvent) {
        delegate?.imageViewMouseMoved(self, at: convert(event.locationInWindow, from: nil))
    }

    override func keyDown(with event: NSEvent) {
        if delegate?.imageView(self, handleKeyDown: event) != true {
            super.keyDown(with: event)
        }
    }

    // MARK: - Drag and drop

    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        fileURLs(in: sender).isEmpty ? [] : .copy
    }

    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        let urls = fileURLs(in: sender)
        guard !urls.isEmpty else { return false }
        delegate?.imageView(self, didReceiveDrop: urls)
        return true
    }

    private func fileURLs(in info: NSDraggingInfo) -> [URL] {
        info.draggingPasteboard.readObjects(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) as? [URL] ?? []
    }
}

extension CGAffineTransform {
    /// Snaps a product of quarter turns and flips back to exact 0/±1 entries.
    var roundedLinear: CGAffineTransform {
        CGAffineTransform(a: a.rounded(), b: b.rounded(), c: c.rounded(), d: d.rounded(), tx: 0, ty: 0)
    }
}
