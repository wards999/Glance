import AppKit
import UniformTypeIdentifiers

/// Makes Glance the app that opens the main picture, PDF and video types from Finder. Unlike
/// Windows, macOS lets an app do this itself, but it asks you to confirm each type, so you choose
/// which ones first and can stop part-way (other types: Get Info ▸ Open with ▸ Change All).
enum DefaultApp {
    /// PNG, JPEG (.jpg and .jpeg are one type), PDF, and MP4, MOV and MKV when libmpv is in the app.
    static func mainTypes() -> [UTType] {
        var types: [UTType] = [.png, .jpeg, .pdf]
        if MPV.available {
            types += [.mpeg4Movie, .quickTimeMovie]
            if let mkv = UTType("org.matroska.mkv") { types.append(mkv) }
        }
        return types
    }

    static func name(_ type: UTType) -> String {
        type == .jpeg ? "JPEG (.jpg, .jpeg)" : (type.preferredFilenameExtension?.uppercased() ?? type.identifier)
    }

    /// Asks which types (each one is a confirmation from macOS), then sets them one at a time.
    static func makeDefault() {
        let types = mainTypes()
        let alert = NSAlert()
        alert.messageText = "Make Glance the default app?"
        alert.informativeText = "Choose the kinds of file Glance should open from Finder. macOS asks you to "
            + "confirm each one, in turn; decline one and you can stop there.\n\n"
            + "For other kinds of file, use Get Info ▸ Open with ▸ Change All."
        let makeButton = alert.addButton(withTitle: "Make Default (\(types.count))")
        alert.addButton(withTitle: "Cancel")

        // A checkbox per type, all ticked; the button counts the confirmations to come.
        let boxes = types.map { NSButton(checkboxWithTitle: name($0), target: nil, action: nil) }
        let counter = CheckboxCounter { count in
            makeButton.title = count == 1 ? "Make Default (1)" : "Make Default (\(count))"
            makeButton.isEnabled = count > 0
        }
        for box in boxes {
            box.state = .on
            box.target = counter
            box.action = #selector(CheckboxCounter.changed(_:))
        }
        counter.boxes = boxes
        let stack = NSStackView(views: boxes)
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 6
        stack.frame = NSRect(x: 0, y: 0, width: 260, height: CGFloat(boxes.count) * 24)
        alert.accessoryView = stack

        guard alert.runModal() == .alertFirstButtonReturn else { return }
        let chosen = zip(types, boxes).filter { $0.1.state == .on }.map(\.0)
        setNext(chosen, done: [], skipped: [])
    }

    private static func setNext(_ remaining: [UTType], done: [UTType], skipped: [UTType]) {
        guard let type = remaining.first else {
            report(done: done, skipped: skipped)
            return
        }
        let rest = Array(remaining.dropFirst())
        NSWorkspace.shared.setDefaultApplication(at: Bundle.main.bundleURL, toOpen: type) { error in
            DispatchQueue.main.async {
                guard error != nil else {
                    setNext(rest, done: done + [type], skipped: skipped)
                    return
                }
                // Declined (or refused): carry on only if you want to.
                if !rest.isEmpty {
                    let ask = NSAlert()
                    ask.messageText = "\(name(type)) wasn't changed"
                    ask.informativeText = "Continue with the remaining \(rest.count) "
                        + (rest.count == 1 ? "type" : "types") + ": "
                        + rest.map(name).joined(separator: ", ") + "?"
                    ask.addButton(withTitle: "Continue")
                    ask.addButton(withTitle: "Stop Here")
                    if ask.runModal() != .alertFirstButtonReturn {
                        report(done: done, skipped: skipped + [type] + rest)
                        return
                    }
                }
                setNext(rest, done: done, skipped: skipped + [type])
            }
        }
    }

    private static func report(done: [UTType], skipped: [UTType]) {
        let alert = NSAlert()
        if done.isEmpty {
            alert.messageText = "Nothing was changed"
            alert.informativeText = "Finder opens these files as before."
        } else {
            alert.messageText = "Glance now opens " + done.map(name).joined(separator: ", ")
            alert.informativeText = skipped.isEmpty
                ? "Change any of them back with Get Info ▸ Open with ▸ Change All."
                : "Not changed: " + skipped.map(name).joined(separator: ", ") + "."
        }
        alert.runModal()
    }
}

/// Keeps the alert's button in step with the ticked checkboxes.
private final class CheckboxCounter: NSObject {
    var boxes: [NSButton] = []
    private let update: (Int) -> Void

    init(_ update: @escaping (Int) -> Void) {
        self.update = update
    }

    @objc func changed(_ sender: NSButton) {
        update(boxes.filter { $0.state == .on }.count)
    }
}
