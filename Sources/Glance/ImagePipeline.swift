import Foundation
import ImageIO

/// Decodes images off the main thread, keeps recent results in memory and prefetches neighbours.
///
/// Requests for the same key are coalesced, so if you navigate onto an image that is still
/// being prefetched you wait for that decode instead of starting a second one.
final class ImagePipeline {
    /// How big a decode to make. The image's own size isn't known until its header is read,
    /// so the opening sizes describe the view and the pipeline works out the pixels.
    enum DecodeSize: Hashable {
        /// Enough to show the whole picture in a view this many device pixels across.
        case fit(width: Int, height: Int)
        /// Enough to fill a view this size, cropping the overflow.
        case fill(width: Int, height: Int)
        /// A fixed long side (a zoom level); more than the full size means full size.
        case longSide(Int)
    }

    struct Key: Hashable {
        let url: URL
        let page: Int
        /// The opening decode and each zoom level are separate entries.
        let size: DecodeSize
        /// PDFs only: `page` counts screens under this layout. Images leave the default.
        var layout = PDFLayout()
    }

    private struct InFlight {
        var callbacks: [(DecodedImage?) -> Void] = []
        var foreground = false
        var started = false
    }

    private let lock = NSLock()
    private var cache: [Key: DecodedImage] = [:]
    private var recency: [Key] = []
    private var totalCost = 0
    /// Bytes of decoded pixels to keep. Lowering it evicts immediately.
    var costLimit: Int {
        didSet {
            lock.lock()
            evictIfNeeded()
            lock.unlock()
        }
    }
    private var inFlight: [Key: InFlight] = [:]
    /// Keys the latest `prefetch` call asked for. Queued prefetches that fall out of this set are skipped.
    private var wanted: Set<Key> = []

    private let foregroundQueue: OperationQueue = {
        let queue = OperationQueue()
        queue.name = "glance.decode.foreground"
        queue.qualityOfService = .userInteractive
        queue.maxConcurrentOperationCount = 2
        return queue
    }()

    private let backgroundQueue: OperationQueue = {
        let queue = OperationQueue()
        queue.name = "glance.decode.prefetch"
        queue.qualityOfService = .utility
        queue.maxConcurrentOperationCount = 2
        return queue
    }()

    /// Files above this size get their embedded thumbnail shown while the real decode runs.
    private let previewThreshold: Int64 = 12_000_000
    private var pdfPasswords: [URL: String] = [:]

    /// Remembered for the session only, never written to disk.
    func setPassword(_ password: String, for url: URL) {
        lock.lock()
        pdfPasswords[url] = password
        lock.unlock()
    }

    func password(for url: URL) -> String? {
        lock.lock()
        defer { lock.unlock() }
        return pdfPasswords[url]
    }

    private var showsQuickPreview = false
    /// When a full-size decode is needed, show a half-size one first (Settings ▸ Performance).
    var quickPreview: Bool {
        get {
            lock.lock()
            defer { lock.unlock() }
            return showsQuickPreview
        }
        set {
            lock.lock()
            showsQuickPreview = newValue
            lock.unlock()
        }
    }

    init(costLimit: Int) {
        self.costLimit = costLimit
    }

    func cached(_ key: Key) -> DecodedImage? {
        lock.lock()
        defer { lock.unlock() }
        guard let hit = cache[key] else { return nil }
        touch(key)
        return hit
    }

    /// Decodes `key` at high priority. `preview` may be called first with the file's embedded
    /// thumbnail; `completion` always runs on the main queue.
    func load(_ key: Key, preview: ((CGImage, ImageInfo) -> Void)?, completion: @escaping (DecodedImage?) -> Void) {
        lock.lock()
        if let hit = cache[key] {
            touch(key)
            lock.unlock()
            DispatchQueue.main.async { completion(hit) }
            return
        }
        var entry = inFlight[key] ?? InFlight()
        entry.callbacks.append(completion)
        entry.foreground = true
        let alreadyStarted = entry.started
        inFlight[key] = entry
        lock.unlock()

        // Even if a prefetch for this key is queued, start a foreground decode now;
        // whichever operation runs first does the work and the other one exits.
        if !alreadyStarted {
            foregroundQueue.addOperation { [weak self] in self?.run(key, preview: preview) }
        }
    }

    /// Replaces the set of images to decode ahead of time. Earlier prefetches that haven't
    /// started yet are dropped.
    func prefetch(_ keys: [Key]) {
        lock.lock()
        wanted = Set(keys)
        var toStart: [Key] = []
        for key in keys where cache[key] == nil && inFlight[key] == nil {
            inFlight[key] = InFlight()
            toStart.append(key)
        }
        lock.unlock()

        for key in toStart {
            backgroundQueue.addOperation { [weak self] in self?.run(key, preview: nil) }
        }
    }

    /// The caller no longer needs `key` (the user moved on). A decode that hasn't started yet is skipped.
    func drop(_ key: Key) {
        lock.lock()
        if var entry = inFlight[key], !entry.started {
            entry.callbacks.removeAll()
            entry.foreground = false
            inFlight[key] = entry
        }
        lock.unlock()
    }

    /// Forget everything decoded from `url`, e.g. after it was overwritten.
    func invalidate(_ url: URL) {
        lock.lock()
        for key in cache.keys where key.url == url {
            remove(key)
        }
        lock.unlock()
    }

    // MARK: - Private

    private func run(_ key: Key, preview: ((CGImage, ImageInfo) -> Void)?) {
        lock.lock()
        guard var entry = inFlight[key], !entry.started else {
            lock.unlock()
            return
        }
        if !entry.foreground && !wanted.contains(key) {
            // Stale prefetch, or a foreground request that was dropped.
            inFlight[key] = nil
            lock.unlock()
            return
        }
        entry.started = true
        inFlight[key] = entry
        lock.unlock()

        let result = decode(key, preview: preview)

        lock.lock()
        let callbacks = inFlight.removeValue(forKey: key)?.callbacks ?? []
        if let result {
            insert(result, for: key)
        }
        lock.unlock()

        if !callbacks.isEmpty {
            DispatchQueue.main.async {
                callbacks.forEach { $0(result) }
            }
        }
    }

    private func decode(_ key: Key, preview: ((CGImage, ImageInfo) -> Void)?) -> DecodedImage? {
        // The only cost PDF support adds to images: this extension check.
        if PDFRenderer.isPDF(key.url) {
            return PDFRenderer.decode(key.url, index: key.page, size: key.size, layout: key.layout, password: password(for: key.url))
        }
        guard let source = ImageDecoder.makeSource(key.url),
              let info = ImageDecoder.info(for: key.url, source: source, page: key.page) else {
            return nil
        }

        if let preview, info.fileSize > previewThreshold || info.isRaw,
           let thumbnail = ImageDecoder.embeddedPreview(source: source, page: key.page),
           max(thumbnail.width, thumbnail.height) >= 480 {
            DispatchQueue.main.async { preview(thumbnail, info) }
        }

        let full = info.fullLongSide
        let maxPixel = Self.decodeLongSide(for: key.size, info: info)
        let preferEmbedded = info.isRaw && maxPixel < full

        // Quick preview: a half-size decode costs a fraction of a full one and is nearly sharp,
        // so show it while the full decode runs. Only when full size is genuinely needed.
        if let preview, quickPreview, maxPixel == full, full / 2 >= 800, !info.isRaw,
           let half = ImageDecoder.decode(source: source, page: key.page, maxPixelSize: full / 2) {
            DispatchQueue.main.async { preview(half, info) }
        }

        guard let image = ImageDecoder.decode(source: source, page: key.page, maxPixelSize: maxPixel, preferEmbedded: preferEmbedded) else {
            return nil
        }
        return DecodedImage(image: image, info: info, page: key.page)
    }

    /// Picks the decode's long side: the smallest 1/2ⁿ of full size that still covers what the view
    /// needs. JPEG and HEIF decode those sizes directly, so there's no full-size decode and no
    /// resampling pass. A 4000×3000 photo shown 1800 px wide decodes at 2000×1500, a quarter of the work.
    static func decodeLongSide(for size: DecodeSize, info: ImageInfo) -> Int {
        if case .longSide(let pixels) = size {
            return min(pixels, info.fullLongSide)
        }
        return halvingLevel(full: info.fullLongSide, atLeast: neededLongSide(for: size, info: info))
    }

    /// The long side the view actually needs, never more than full size.
    static func neededLongSide(for size: DecodeSize, info: ImageInfo) -> Int {
        let full = info.fullLongSide
        let fullSize = info.fullSize
        switch size {
        case .longSide(let pixels):
            return min(pixels, full)
        case .fit(let width, let height):
            let scale = min(CGFloat(width) / fullSize.width, CGFloat(height) / fullSize.height)
            return Int((CGFloat(full) * min(scale, 1)).rounded(.up))
        case .fill(let width, let height):
            let scale = max(CGFloat(width) / fullSize.width, CGFloat(height) / fullSize.height)
            return Int((CGFloat(full) * min(scale, 1)).rounded(.up))
        }
    }

    /// The smallest of full, full/2, full/4, … that is at least `needed`.
    static func halvingLevel(full: Int, atLeast needed: Int) -> Int {
        var level = full
        while level / 2 >= max(needed, 1) { level /= 2 }
        return level
    }

    // The helpers below expect `lock` to be held.

    private func insert(_ image: DecodedImage, for key: Key) {
        if cache[key] != nil { remove(key) }
        cache[key] = image
        recency.append(key)
        totalCost += image.cost
        evictIfNeeded()
    }

    /// Drops least recently used images, but always keeps the newest one.
    private func evictIfNeeded() {
        while totalCost > costLimit, recency.count > 1 {
            remove(recency[0])
        }
    }

    private func remove(_ key: Key) {
        if let old = cache.removeValue(forKey: key) {
            totalCost -= old.cost
        }
        recency.removeAll { $0 == key }
    }

    private func touch(_ key: Key) {
        if let index = recency.firstIndex(of: key) {
            recency.remove(at: index)
            recency.append(key)
        }
    }
}
