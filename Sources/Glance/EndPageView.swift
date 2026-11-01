import AppKit

/// Between two PDFs, after the last page going forward or before the first going back: a blank
/// page with both covers in reading order, so the step from one document to the next is obvious.
/// The one Space or Delete goes to is large; the one you're leaving is half the size and dimmed.
final class EndPageView: NSView {
    private let destination = NSImageView()
    private let origin = NSImageView()
    private let covers = NSStackView()
    private let label = NSTextField(wrappingLabelWithString: "")
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
        label.translatesAutoresizingMaskIntoConstraints = false
        addSubview(covers)
        addSubview(label)
        let height = destination.heightAnchor.constraint(equalToConstant: 420)
        height.priority = .defaultHigh
        NSLayoutConstraint.activate([
            covers.centerXAnchor.constraint(equalTo: centerXAnchor),
            covers.centerYAnchor.constraint(equalTo: centerYAnchor, constant: -30),
            covers.widthAnchor.constraint(lessThanOrEqualTo: widthAnchor, constant: -80),
            height,
            destination.heightAnchor.constraint(lessThanOrEqualTo: heightAnchor, multiplier: 0.45),
            origin.heightAnchor.constraint(equalTo: destination.heightAnchor, multiplier: 0.5),
            label.topAnchor.constraint(equalTo: covers.bottomAnchor, constant: 20),
            label.centerXAnchor.constraint(equalTo: centerXAnchor),
            label.widthAnchor.constraint(lessThanOrEqualTo: widthAnchor, constant: -80),
        ])
        setAspect(of: destination, 0.7)
        setAspect(of: origin, 0.7)
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    /// `forward`: past this PDF's last page, heading for the next one; otherwise before its first,
    /// heading for the previous one. Covers arrive afterwards, as they're decoded.
    func show(forward: Bool, rightToLeft: Bool, current: String, other: String, background: NSColor) {
        layer?.backgroundColor = background.cgColor
        label.stringValue = forward
            ? "End of \(current)\n\nNext: \(other)\nSpace for the next PDF  ·  Delete to go back"
            : "Start of \(current)\n\nPrevious: \(other)\nDelete for the previous PDF  ·  Space to go back"
        // Reading order: going forward, this PDF comes first; going back, the previous one does.
        let originFirst = forward != rightToLeft
        covers.arrangedSubviews.forEach { covers.removeArrangedSubview($0) }
        (originFirst ? [origin, destination] : [destination, origin]).forEach { covers.addArrangedSubview($0) }
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

    override func scrollWheel(with event: NSEvent) {
        if let scrollTarget { scrollTarget.scrollWheel(with: event) } else { super.scrollWheel(with: event) }
    }
}
