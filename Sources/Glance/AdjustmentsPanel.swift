import AppKit

/// Floating sliders for brightness, contrast, color balance and so on.
/// It never takes keyboard focus, so arrow keys keep flipping images while it's open.
final class AdjustmentsPanel: NSPanel {
    var onChange: ((Adjustments) -> Void)?

    private var adjustments = Adjustments()
    private var sliders: [(WritableKeyPath<Adjustments, Double>, NSSlider)] = []
    private let autoCheckbox = NSButton(checkboxWithTitle: "Auto correction", target: nil, action: nil)

    init() {
        super.init(
            contentRect: NSRect(x: 0, y: 0, width: 320, height: 340),
            styleMask: [.titled, .closable, .utilityWindow, .hudWindow],
            backing: .buffered,
            defer: true
        )
        title = "Adjustments"
        isFloatingPanel = true
        becomesKeyOnlyIfNeeded = true
        hidesOnDeactivate = true
        isReleasedWhenClosed = false

        let rows: [(String, WritableKeyPath<Adjustments, Double>, ClosedRange<Double>)] = [
            ("Brightness", \.brightness, -1...1),
            ("Contrast", \.contrast, -1...1),
            ("Saturation", \.saturation, -1...1),
            ("Gamma", \.gamma, -1...1),
            ("Sharpen", \.sharpen, 0...1),
            ("Cyan ↔ Red", \.cyanRed, -1...1),
            ("Magenta ↔ Green", \.magentaGreen, -1...1),
            ("Yellow ↔ Blue", \.yellowBlue, -1...1),
        ]
        let grid = NSGridView(numberOfColumns: 2, rows: 0)
        grid.rowSpacing = 8
        grid.columnSpacing = 10
        for (name, keyPath, range) in rows {
            let slider = NSSlider(value: 0, minValue: range.lowerBound, maxValue: range.upperBound, target: self, action: #selector(sliderMoved(_:)))
            slider.isContinuous = true
            slider.widthAnchor.constraint(equalToConstant: 180).isActive = true
            grid.addRow(with: [NSTextField(labelWithString: name), slider])
            sliders.append((keyPath, slider))
        }
        grid.column(at: 0).xPlacement = .trailing

        autoCheckbox.target = self
        autoCheckbox.action = #selector(autoToggled(_:))
        let reset = NSButton(title: "Reset", target: self, action: #selector(resetAll(_:)))

        let buttons = NSStackView(views: [autoCheckbox, reset])
        buttons.spacing = 16
        let stack = NSStackView(views: [grid, buttons])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 14
        stack.edgeInsets = NSEdgeInsets(top: 14, left: 16, bottom: 14, right: 16)
        contentView = stack
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    /// Shows `adjustments` without reporting a change.
    func update(_ adjustments: Adjustments) {
        self.adjustments = adjustments
        for (keyPath, slider) in sliders {
            slider.doubleValue = adjustments[keyPath: keyPath]
        }
        autoCheckbox.state = adjustments.autoCorrect ? .on : .off
    }

    @objc private func sliderMoved(_ sender: NSSlider) {
        guard let keyPath = sliders.first(where: { $0.1 === sender })?.0 else { return }
        // Double-click-free reset: snap to zero near the middle.
        let value = abs(sender.doubleValue) < 0.03 ? 0 : sender.doubleValue
        adjustments[keyPath: keyPath] = value
        onChange?(adjustments)
    }

    @objc private func autoToggled(_ sender: NSButton) {
        adjustments.autoCorrect = sender.state == .on
        onChange?(adjustments)
    }

    @objc private func resetAll(_ sender: Any?) {
        update(Adjustments())
        onChange?(adjustments)
    }
}
