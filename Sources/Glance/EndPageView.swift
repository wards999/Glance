import AppKit

/// After a PDF's last page, when the next file is a PDF too: a blank page saying the document has
/// ended, with the next one's cover, so the step from one PDF to the next is obvious.
final class EndPageView: NSView {
    private let cover = NSImageView()
    private let label = NSTextField(wrappingLabelWithString: "")
    private var coverHeight: NSLayoutConstraint?
    /// Gets the wheel and trackpad, so they still turn pages (and leave the end page).
    weak var scrollTarget: NSView?

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        cover.imageScaling = .scaleProportionallyUpOrDown
        cover.wantsLayer = true
        cover.layer?.borderColor = NSColor.white.withAlphaComponent(0.25).cgColor
        cover.layer?.borderWidth = 1
        cover.translatesAutoresizingMaskIntoConstraints = false
        label.font = .systemFont(ofSize: 13)
        label.textColor = NSColor.white.withAlphaComponent(0.85)
        label.alignment = .center
        label.isSelectable = false
        label.translatesAutoresizingMaskIntoConstraints = false
        addSubview(cover)
        addSubview(label)
        let height = cover.heightAnchor.constraint(equalToConstant: 0)
        coverHeight = height
        NSLayoutConstraint.activate([
            cover.centerXAnchor.constraint(equalTo: centerXAnchor),
            cover.centerYAnchor.constraint(equalTo: centerYAnchor, constant: -30),
            cover.heightAnchor.constraint(lessThanOrEqualTo: heightAnchor, multiplier: 0.45),
            cover.widthAnchor.constraint(lessThanOrEqualTo: widthAnchor, constant: -80),
            height,
            label.topAnchor.constraint(equalTo: cover.bottomAnchor, constant: 20),
            label.centerXAnchor.constraint(equalTo: centerXAnchor),
            label.widthAnchor.constraint(lessThanOrEqualTo: widthAnchor, constant: -80),
        ])
        height.priority = .defaultHigh
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    func show(ending current: String, next: String, background: NSColor) {
        layer?.backgroundColor = background.cgColor
        label.stringValue = "End of \(current)\n\nNext: \(next)\nSpace for the next PDF  ·  Delete to go back"
        setCover(nil)
    }

    /// The next PDF's first screen; nil until it's decoded.
    func setCover(_ image: CGImage?) {
        guard let image, image.height > 0 else {
            cover.image = nil
            cover.isHidden = true
            coverHeight?.constant = 0
            return
        }
        cover.image = NSImage(cgImage: image, size: .zero)
        cover.isHidden = false
        coverHeight?.constant = 420
        // Keep the picture's proportions as the height gives way to a small window.
        cover.constraints.filter { $0.identifier == "aspect" }.forEach { cover.removeConstraint($0) }
        let aspect = cover.widthAnchor.constraint(equalTo: cover.heightAnchor, multiplier: CGFloat(image.width) / CGFloat(image.height))
        aspect.identifier = "aspect"
        aspect.isActive = true
    }

    override func scrollWheel(with event: NSEvent) {
        if let scrollTarget { scrollTarget.scrollWheel(with: event) } else { super.scrollWheel(with: event) }
    }
}
