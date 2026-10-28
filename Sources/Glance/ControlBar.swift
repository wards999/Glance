import AppKit

/// The button strip that fades in when the mouse nears the bottom of the window.
final class ControlBar: NSVisualEffectView {
    /// Previous and next file look the same everywhere (|< >|, as on the video bar); in a PDF,
    /// page arrows show between them.
    var showsPageButtons = false {
        didSet { pageButtons.forEach { $0.isHidden = !showsPageButtons } }
    }
    private var pageButtons: [NSButton] = []

    init(target: AnyObject) {
        super.init(frame: .zero)
        material = .hudWindow
        blendingMode = .withinWindow
        state = .active
        wantsLayer = true
        layer?.cornerRadius = 10
        layer?.masksToBounds = true

        typealias C = ViewerWindowController
        let items: [(symbol: String, title: String, action: Selector)] = [
            ("backward.end.fill", "Previous File", #selector(C.previousDocument(_:))),
            ("chevron.left", "Previous Page", #selector(C.previousImage(_:))),
            ("chevron.right", "Next Page", #selector(C.nextImage(_:))),
            ("forward.end.fill", "Next File", #selector(C.nextDocument(_:))),
            ("rotate.left", "Rotate Left", #selector(C.rotateLeft(_:))),
            ("rotate.right", "Rotate Right", #selector(C.rotateRight(_:))),
            ("minus.magnifyingglass", "Zoom Out", #selector(C.zoomOut(_:))),
            ("plus.magnifyingglass", "Zoom In", #selector(C.zoomIn(_:))),
            ("aspectratio", "Fit to Window", #selector(C.zoomToFit(_:))),
            ("1.magnifyingglass", "Actual Size", #selector(C.zoomActualSize(_:))),
            ("info.circle", "Info", #selector(C.toggleInfo(_:))),
            ("slider.horizontal.3", "Adjustments", #selector(C.showAdjustments(_:))),
            ("play.fill", "Slideshow", #selector(C.toggleSlideshow(_:))),
            ("arrow.up.left.and.arrow.down.right", "Full Screen", #selector(C.toggleFullScreenMode(_:))),
        ]

        let buttons: [NSButton] = items.map { item in
            let button: NSButton
            if let image = NSImage(systemSymbolName: item.symbol, accessibilityDescription: item.title) {
                button = NSButton(image: image, target: target, action: item.action)
            } else {
                button = NSButton(title: item.title, target: target, action: item.action)
            }
            button.isBordered = false
            button.contentTintColor = .white
            button.toolTip = item.title
            button.widthAnchor.constraint(greaterThanOrEqualToConstant: 30).isActive = true
            button.heightAnchor.constraint(equalToConstant: 28).isActive = true
            return button
        }

        pageButtons = [buttons[1], buttons[2]]
        pageButtons.forEach { $0.isHidden = true }

        let stack = NSStackView(views: buttons)  // hidden buttons take no room
        stack.orientation = .horizontal
        stack.spacing = 4
        stack.edgeInsets = NSEdgeInsets(top: 4, left: 10, bottom: 4, right: 10)
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor),
            stack.topAnchor.constraint(equalTo: topAnchor),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor),
        ])
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not supported")
    }
}
