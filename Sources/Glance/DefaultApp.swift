import AppKit
import ImageIO
import UniformTypeIdentifiers

/// Makes Glance the app that opens pictures, PDFs and videos from Finder, in one go. Unlike
/// Windows, macOS lets an app do this itself (and may ask you to confirm some types). You can
/// change any type back later with Get Info ▸ Open with.
enum DefaultApp {
    /// Every concrete type Glance opens: each picture format ImageIO decodes, PDF, and the
    /// videos libmpv plays when it's in the app.
    static func supportedTypes() -> [UTType] {
        var types: [UTType] = []
        func add(_ type: UTType?) {
            guard let type, !type.isDynamic, !types.contains(type) else { return }
            types.append(type)
        }
        for identifier in (CGImageSourceCopyTypeIdentifiers() as? [String]) ?? [] {
            if let type = UTType(identifier), type != .pdf { add(type) }
        }
        add(.pdf)
        if MPV.available {
            for ext in VideoFiles.extensions.sorted() { add(UTType(filenameExtension: ext, conformingTo: .movie)) }
        }
        return types
    }

    /// Asks first, then sets Glance as the default for every type and says how it went.
    static func makeDefault() {
        let alert = NSAlert()
        alert.messageText = "Make Glance the default app?"
        alert.informativeText = "Pictures, PDFs and videos will open in Glance when you double-click them in Finder. "
            + "You can change any kind of file back later with Get Info ▸ Open with."
        alert.addButton(withTitle: "Make Default")
        alert.addButton(withTitle: "Cancel")
        guard alert.runModal() == .alertFirstButtonReturn else { return }

        let types = supportedTypes()
        let app = Bundle.main.bundleURL
        let group = DispatchGroup()
        var failed: [String] = []
        let lock = NSLock()
        for type in types {
            group.enter()
            NSWorkspace.shared.setDefaultApplication(at: app, toOpen: type) { error in
                if error != nil {
                    lock.lock()
                    failed.append(type.preferredFilenameExtension ?? type.identifier)
                    lock.unlock()
                }
                group.leave()
            }
        }
        group.notify(queue: .main) {
            let done = NSAlert()
            if failed.isEmpty {
                done.messageText = "Glance is now the default app"
                done.informativeText = "It opens \(types.count) kinds of file: pictures, PDFs and videos."
            } else {
                done.messageText = "Glance is the default for \(types.count - failed.count) of \(types.count) kinds of file"
                done.informativeText = "macOS didn't change: " + failed.sorted().joined(separator: ", ")
                    + ". You can set those with Get Info ▸ Open with ▸ Change All."
            }
            done.runModal()
        }
    }
}
