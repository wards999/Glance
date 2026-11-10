import AppKit

/// After a PDF's last page (or before its first): a blank page with both covers in reading order,
/// and the next (or previous) document is a button, not another page turn, so nobody lands in it
/// without noticing. The one the button goes to is large; the one you're leaving is half the size
/// and dimmed.
final class EndPageView: NSView {
    private let destination = NSImageView()
    private let origin = NSImageView()
    private let covers = NSStackView()
    private let label = NSTextField(wrappingLabelWithString: "")
    private let button = NSButton(title: "", target: nil, action: nil)
    private let keys = NSTextField(wrappingLabelWithString: "")
    private let texts = NSStackView()
    /// The button (or Return): on to the other document.
    var onButton: (() -> Void)?
    /// Gets the wheel and trackpad, so they still turn pages (and leave this page).
    weak var scrollTarget: NSView?

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        for cover in [destination, origin] {
            cover.imageScaling = .scaleProportionallyUpOrDown
            cover.wantsLayer = true
            cover.layer?.borderColor = NSColor.white.withAlphaComponent(0.25).cgColor
            cover.translatesAutoresizingMaskIntoConstraints = false
            // The layout below decides the size, not the pictures' pixels.
            for orientation in [NSLayoutConstraint.Orientation.horizontal, .vertical] {
                cover.setContentHuggingPriority(.defaultLow, for: orientation)
                cover.setContentCompressionResistancePriority(.defaultLow, for: orientation)
            }
        }
        origin.alphaValue = 0.5
        covers.orientation = .horizontal
        covers.alignment = .bottom
        covers.spacing = 32
        covers.translatesAutoresizingMaskIntoConstraints = false
        label.font = .systemFont(ofSize: 13)
        label.textColor = NSColor.white.withAlphaComponent(0.85)
        label.alignment = .center
        label.isSelectable = false
        button.bezelStyle = .rounded
        button.controlSize = .large
        button.target = self
        button.action = #selector(pressed)
        keys.font = .systemFont(ofSize: 12)
        keys.textColor = NSColor.white.withAlphaComponent(0.6)
        keys.alignment = .center
        keys.isSelectable = false
        texts.orientation = .vertical
        texts.alignment = .centerX
        texts.spacing = 14
        texts.translatesAutoresizingMaskIntoConstraints = false
        texts.addArrangedSubview(label)
        texts.addArrangedSubview(button)
        texts.addArrangedSubview(keys)
        addSubview(covers)
        addSubview(texts)
        // In the view tree before any constraint ties them to this view: AppKit refuses (with an
        // exception) a constraint between views with no common ancestor. show() sets the order.
        covers.addArrangedSubview(origin)
        covers.addArrangedSubview(destination)
        let height = destination.heightAnchor.constraint(equalToConstant: 420)
        height.priority = .defaultHigh
        NSLayoutConstraint.activate([
            covers.centerXAnchor.constraint(equalTo: centerXAnchor),
            covers.centerYAnchor.constraint(equalTo: centerYAnchor, constant: -30),
            covers.widthAnchor.constraint(lessThanOrEqualTo: widthAnchor, constant: -80),
            height,
            destination.heightAnchor.constraint(lessThanOrEqualTo: heightAnchor, multiplier: 0.45),
            origin.heightAnchor.constraint(equalTo: destination.heightAnchor, multiplier: 0.5),
            texts.topAnchor.constraint(equalTo: covers.bottomAnchor, constant: 20),
            texts.centerXAnchor.constraint(equalTo: centerXAnchor),
            texts.widthAnchor.constraint(lessThanOrEqualTo: widthAnchor, constant: -80),
        ])
        setAspect(of: destination, 0.7)
        setAspect(of: origin, 0.7)
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    /// `forward`: past this PDF's last page, heading for the next one; otherwise before its first,
    /// heading for the previous one. `other`: the file the button goes to (nil: none that way, no
    /// button). Covers arrive afterwards, as they're decoded.
    /// `currentIsPDF`: leaving a PDF (its end or start); else a photo or movie with a PDF beside it.
    func show(forward: Bool, rightToLeft: Bool, current: String, currentIsPDF: Bool, other: String?, otherIsPDF: Bool,
              noneText: String, background: NSColor) {
        layer?.backgroundColor = background.cgColor
        let heading = currentIsPDF ? (forward ? "End of \(current)" : "Start of \(current)") : current
        let back = currentIsPDF ? (forward ? "Delete back to the last page" : "Space back to the first page")
                                : (forward ? "Delete back to \(current)" : "Space back to \(current)")
        if let other {
            label.stringValue = heading + (forward ? "\n\nNext: \(other)" : "\n\nPrevious: \(other)")
            let kind = otherIsPDF ? "document" : "file"
            button.title = forward ? "Next \(kind)  ›" : "‹  Previous \(kind)"
            keys.stringValue = (forward ? "⌥↓  ·  " : "⌥↑  ·  ") + back
        } else {
            label.stringValue = heading + "\n\n" + noneText
            keys.stringValue = back
        }
        button.isHidden = other == nil
        // Reading order: going forward, this PDF comes first; going back, the previous one does.
        let originFirst = forward != rightToLeft
        // Moving one to the front reorders them; both stay in the stack, and their constraints with them.
        covers.insertArrangedSubview(originFirst ? origin : destination, at: 0)
        setCover(nil, of: destination)
        setCover(nil, of: origin)
    }

    /// The first screen of the PDF that Space or Delete goes to; shown large.
    func setDestinationCover(_ image: CGImage?) {
        setCover(image, of: destination)
        // Until this PDF's own cover arrives, its place has the same proportions, so nothing jumps.
        if origin.image == nil, let image, image.height > 0 { setAspect(of: origin, CGFloat(image.width) / CGFloat(image.height)) }
    }

    /// The first screen of the PDF you're leaving; shown small and dimmed.
    func setOriginCover(_ image: CGImage?) {
        setCover(image, of: origin)
    }

    private func setCover(_ image: CGImage?, of view: NSImageView) {
        view.image = image.map { NSImage(cgImage: $0, size: .zero) }
        view.layer?.borderWidth = image == nil ? 0 : 1
        if let image, image.height > 0 { setAspect(of: view, CGFloat(image.width) / CGFloat(image.height)) }
    }

    private func setAspect(of view: NSImageView, _ ratio: CGFloat) {
        view.constraints.filter { $0.identifier == "aspect" }.forEach { view.removeConstraint($0) }
        let aspect = view.widthAnchor.constraint(equalTo: view.heightAnchor, multiplier: ratio)
        aspect.identifier = "aspect"
        aspect.isActive = true
    }

    @objc private func pressed() {
        onButton?()
    }

    override func scrollWheel(with event: NSEvent) {
        if let scrollTarget { scrollTarget.scrollWheel(with: event) } else { super.scrollWheel(with: event) }
    }
}
