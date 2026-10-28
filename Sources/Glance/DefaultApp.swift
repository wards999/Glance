import AppKit
import UniformTypeIdentifiers

/// Makes Glance the app that opens the main picture, PDF and video types from Finder. Unlike
/// Windows, macOS lets an app do this itself, but it asks you to confirm each type, so this sticks
/// to the formats people actually use (other types: Get Info ▸ Open with ▸ Change All).
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

    /// Asks first, then sets Glance as the default type by type (one confirmation from macOS at a
    /// time, never a stack of them), and says how it went.
    static func makeDefault() {
        let types = mainTypes()
        let names = types.compactMap { $0.preferredFilenameExtension?.uppercased() }.joined(separator: ", ")
        let alert = NSAlert()
        alert.messageText = "Make Glance the default app?"
        alert.informativeText = "Glance will open \(names) files when you double-click them in Finder. "
            + "macOS may ask you to confirm each one.\n\n"
            + "For other kinds of file, use Get Info ▸ Open with ▸ Change All."
        alert.addButton(withTitle: "Make Default")
        alert.addButton(withTitle: "Cancel")
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        setNext(types, done: [], failed: [])
    }

    private static func setNext(_ remaining: [UTType], done: [UTType], failed: [UTType]) {
        guard let type = remaining.first else {
            report(done: done, failed: failed)
            return
        }
        NSWorkspace.shared.setDefaultApplication(at: Bundle.main.bundleURL, toOpen: type) { error in
            DispatchQueue.main.async {
                setNext(Array(remaining.dropFirst()), done: error == nil ? done + [type] : done,
                        failed: error == nil ? failed : failed + [type])
            }
        }
    }

    private static func report(done: [UTType], failed: [UTType]) {
        func list(_ types: [UTType]) -> String {
            types.compactMap { $0.preferredFilenameExtension?.uppercased() }.joined(separator: ", ")
        }
        let alert = NSAlert()
        if failed.isEmpty {
            alert.messageText = "Glance is now the default app"
            alert.informativeText = "It opens \(list(done)) files."
        } else {
            alert.messageText = done.isEmpty ? "Glance wasn't made the default" : "Glance opens \(list(done)) files"
            alert.informativeText = "Not changed: \(list(failed)) (declined, or macOS refused). "
                + "You can set those with Get Info ▸ Open with ▸ Change All."
        }
        alert.runModal()
    }
}
