import Foundation

enum SortOrder: Int, CaseIterable {
    case name = 0, dateModified, dateCreated, size, random

    var title: String {
        switch self {
        case .name: return "Name"
        case .dateModified: return "Date Modified"
        case .dateCreated: return "Date Created"
        case .size: return "Size"
        case .random: return "Random"
        }
    }
}

/// The list of images in the current folder and where we are in it.
final class FolderNavigator {
    private(set) var files: [URL] = []
    private(set) var index = 0
    var sortOrder = SortOrder.name
    var descending = false
    var wraps = true

    var current: URL? { files.indices.contains(index) ? files[index] : nil }
    var count: Int { files.count }

    /// Lists every decodable image in `folder`. Call off the main thread: big folders on network volumes are slow.
    static func scan(folder: URL, order: SortOrder, descending: Bool, includePDFs: Bool) -> [URL] {
        let keys: [URLResourceKey] = [.isRegularFileKey, .contentModificationDateKey, .creationDateKey, .fileSizeKey]
        guard let items = try? FileManager.default.contentsOfDirectory(
            at: folder, includingPropertiesForKeys: keys, options: [.skipsHiddenFiles]
        ) else { return [] }
        let images = items.filter { ImageDecoder.isSupported($0) || (includePDFs && PDFRenderer.isPDF($0)) }
        return sort(images, by: order, descending: descending)
    }

    static func sort(_ urls: [URL], by order: SortOrder, descending: Bool) -> [URL] {
        if order == .random { return urls.shuffled() }

        func sorted<T: Comparable>(by value: (URL) -> T) -> [URL] {
            // Read each value once rather than once per comparison.
            let keyed = urls.map { ($0, value($0)) }
            return keyed.sorted { descending ? $0.1 > $1.1 : $0.1 < $1.1 }.map(\.0)
        }

        switch order {
        case .name:
            // Finder order: "img2" before "img10".
            let ascending = urls.sorted { $0.lastPathComponent.localizedStandardCompare($1.lastPathComponent) == .orderedAscending }
            return descending ? ascending.reversed() : ascending
        case .dateModified:
            return sorted { (try? $0.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast }
        case .dateCreated:
            return sorted { (try? $0.resourceValues(forKeys: [.creationDateKey]).creationDate) ?? .distantPast }
        case .size:
            return sorted { (try? $0.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0 }
        case .random:
            return urls
        }
    }

    /// Replaces the list, keeping `current` selected (it's added if the scan didn't include it).
    func setFiles(_ urls: [URL], current: URL?) {
        files = urls
        index = 0
        guard let current else { return }
        if let found = position(of: current) {
            index = found
        } else {
            files.insert(current, at: 0)
        }
    }

    func resort() {
        let selected = current
        files = Self.sort(files, by: sortOrder, descending: descending)
        index = selected.flatMap(position(of:)) ?? 0
    }

    @discardableResult
    func move(by delta: Int) -> Bool {
        guard files.count > 1 else { return false }
        var target = index + delta
        if !files.indices.contains(target) {
            guard wraps else { return false }
            target = ((target % files.count) + files.count) % files.count
        }
        index = target
        return true
    }

    func moveToFirst() { index = 0 }
    func moveToLast() { index = max(files.count - 1, 0) }

    /// The file `offset` places away from the current one, following the wrap setting.
    func url(at offset: Int) -> URL? {
        guard !files.isEmpty else { return nil }
        let target = index + offset
        if files.indices.contains(target) { return files[target] }
        guard wraps else { return nil }
        return files[((target % files.count) + files.count) % files.count]
    }

    func removeCurrent() {
        guard files.indices.contains(index) else { return }
        files.remove(at: index)
        if index >= files.count { index = max(files.count - 1, 0) }
    }

    private func position(of url: URL) -> Int? {
        let target = url.standardizedFileURL
        return files.firstIndex { $0.standardizedFileURL == target }
    }
}
