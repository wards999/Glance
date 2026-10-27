import AppKit

enum WheelAction: Int {
    case zoom = 0, navigate
}

enum StartZoom: Int {
    case fit = 0, fill, actual

    var zoomMode: ZoomMode {
        switch self {
        case .fit: return .fit
        case .fill: return .fill
        case .actual: return .actual
        }
    }
}

enum FullScreenStyle: Int {
    /// A borderless window covering the screen: appears immediately, no Space switch.
    case instant = 0
    /// macOS full screen: its own Space, with the slide animation.
    case native
}

enum HoldToZoom: Int {
    case off = 0, pdfsOnly, everything
}

/// User preferences, kept in UserDefaults (what JPEGView puts in its INI file).
/// Setting any value posts `Settings.didChange`; the viewer and the Settings window both follow it.
final class Settings {
    static let shared = Settings()
    static let didChange = Notification.Name("GlanceSettingsDidChange")

    static let slideshowIntervals: [TimeInterval] = [1, 2, 3, 5, 10, 30]

    private enum Key {
        static let sortOrder = "sortOrder"
        static let descending = "sortDescending"
        static let loop = "loopFolder"
        static let wheel = "mouseWheel"
        static let slideshowInterval = "slideshowInterval"
        static let keepAdjustments = "keepAdjustments"
        static let escapeQuits = "escapeQuits"
        static let startZoom = "startZoom"
        static let enlargeSmall = "enlargeSmallImages"
        static let background = "backgroundRGB"
        static let openFullScreen = "openFullScreen"
        static let fullScreenStyle = "fullScreenStyle"
        static let showInfo = "showInfo"
        static let showFilename = "showFilename"
        static let showControls = "showControls"
        static let hideCursor = "hideCursorInFullScreen"
        static let pixelatedZoom = "pixelatedZoom"
        static let saveQuality = "saveQuality"
        static let confirmOverwrite = "confirmOverwrite"
        static let prefetchCount = "prefetchCount"
        static let quickPreview = "quickPreview"
        static let pdfPageMode = "pdfPageMode"
        static let pdfRightToLeft = "pdfRightToLeft"
        static let includePDFs = "includePDFsInFolders"
        static let holdToZoom = "holdToZoom"
        static let holdZoomFactor = "holdZoomFactor"
        static let cacheGB = "cacheGB"
        static let includeVideos = "includeVideosInFolders"
        static let videoAutoplay = "videoAutoplay"
        static let videoPreviews = "videoTimelinePictures"
        static let videoLoopShort = "videoLoopShortClips"
        static let alwaysSubtitles = "alwaysSubtitles"
        static let subtitleLanguage = "subtitleLanguage"
        static let videoVolume = "videoVolume"
        static let videoMuted = "videoMuted"
    }

    private let defaults = UserDefaults.standard

    /// A quarter of RAM, capped at 4 GB.
    static let defaultCacheGB = min(Double(ProcessInfo.processInfo.physicalMemory) / 4 / 1_073_741_824, 4)
    static let maxCacheGB = max(Double(ProcessInfo.processInfo.physicalMemory) / 2 / 1_073_741_824, 1)

    private init() {
        defaults.register(defaults: [
            Key.sortOrder: SortOrder.name.rawValue,
            Key.descending: false,
            Key.loop: true,
            Key.wheel: WheelAction.zoom.rawValue,
            Key.slideshowInterval: 3.0,
            Key.keepAdjustments: false,
            Key.escapeQuits: false,
            Key.startZoom: StartZoom.fit.rawValue,
            Key.enlargeSmall: false,
            Key.background: [0.0, 0.0, 0.0],
            Key.openFullScreen: false,
            Key.fullScreenStyle: FullScreenStyle.instant.rawValue,
            Key.showInfo: false,
            Key.showFilename: false,
            Key.showControls: true,
            Key.hideCursor: true,
            Key.pixelatedZoom: true,
            Key.saveQuality: 0.9,
            Key.confirmOverwrite: true,
            Key.prefetchCount: 3,
            Key.quickPreview: false,
            Key.pdfPageMode: PDFPageMode.automatic.rawValue,
            Key.pdfRightToLeft: false,
            Key.includePDFs: false,
            Key.holdToZoom: HoldToZoom.pdfsOnly.rawValue,
            Key.holdZoomFactor: 2.5,
            Key.cacheGB: Self.defaultCacheGB,
            Key.includeVideos: true,
            Key.videoAutoplay: true,
            Key.videoPreviews: true,
            Key.videoLoopShort: true,
            Key.alwaysSubtitles: false,
            Key.subtitleLanguage: 0,
            Key.videoVolume: 100,
            Key.videoMuted: false,
        ])
    }

    // MARK: General

    var sortOrder: SortOrder {
        get { SortOrder(rawValue: defaults.integer(forKey: Key.sortOrder)) ?? .name }
        set { store(newValue.rawValue, Key.sortOrder) }
    }

    var descending: Bool {
        get { defaults.bool(forKey: Key.descending) }
        set { store(newValue, Key.descending) }
    }

    var loop: Bool {
        get { defaults.bool(forKey: Key.loop) }
        set { store(newValue, Key.loop) }
    }

    var wheelAction: WheelAction {
        get { WheelAction(rawValue: defaults.integer(forKey: Key.wheel)) ?? .zoom }
        set { store(newValue.rawValue, Key.wheel) }
    }

    var slideshowInterval: TimeInterval {
        get { defaults.double(forKey: Key.slideshowInterval) }
        set { store(newValue, Key.slideshowInterval) }
    }

    var keepAdjustments: Bool {
        get { defaults.bool(forKey: Key.keepAdjustments) }
        set { store(newValue, Key.keepAdjustments) }
    }

    /// JPEGView-style: Escape closes the app instead of only leaving full screen.
    var escapeQuits: Bool {
        get { defaults.bool(forKey: Key.escapeQuits) }
        set { store(newValue, Key.escapeQuits) }
    }

    // MARK: Display

    var startZoom: StartZoom {
        get { StartZoom(rawValue: defaults.integer(forKey: Key.startZoom)) ?? .fit }
        set { store(newValue.rawValue, Key.startZoom) }
    }

    var enlargeSmallImages: Bool {
        get { defaults.bool(forKey: Key.enlargeSmall) }
        set { store(newValue, Key.enlargeSmall) }
    }

    var backgroundColor: NSColor {
        get {
            let rgb = defaults.array(forKey: Key.background) as? [Double] ?? [0, 0, 0]
            guard rgb.count == 3 else { return .black }
            return NSColor(srgbRed: rgb[0], green: rgb[1], blue: rgb[2], alpha: 1)
        }
        set {
            let color = newValue.usingColorSpace(.sRGB) ?? .black
            store([Double(color.redComponent), Double(color.greenComponent), Double(color.blueComponent)], Key.background)
        }
    }

    var openFullScreen: Bool {
        get { defaults.bool(forKey: Key.openFullScreen) }
        set { store(newValue, Key.openFullScreen) }
    }

    var fullScreenStyle: FullScreenStyle {
        get { FullScreenStyle(rawValue: defaults.integer(forKey: Key.fullScreenStyle)) ?? .instant }
        set { store(newValue.rawValue, Key.fullScreenStyle) }
    }

    var showInfo: Bool {
        get { defaults.bool(forKey: Key.showInfo) }
        set { store(newValue, Key.showInfo) }
    }

    var showFilename: Bool {
        get { defaults.bool(forKey: Key.showFilename) }
        set { store(newValue, Key.showFilename) }
    }

    var showControls: Bool {
        get { defaults.bool(forKey: Key.showControls) }
        set { store(newValue, Key.showControls) }
    }

    var hideCursorInFullScreen: Bool {
        get { defaults.bool(forKey: Key.hideCursor) }
        set { store(newValue, Key.hideCursor) }
    }

    var pixelatedZoom: Bool {
        get { defaults.bool(forKey: Key.pixelatedZoom) }
        set { store(newValue, Key.pixelatedZoom) }
    }

    // MARK: Saving

    var saveQuality: Double {
        get { defaults.double(forKey: Key.saveQuality) }
        set { store(newValue, Key.saveQuality) }
    }

    var confirmOverwrite: Bool {
        get { defaults.bool(forKey: Key.confirmOverwrite) }
        set { store(newValue, Key.confirmOverwrite) }
    }

    // MARK: Performance

    /// How many images ahead (in the direction of travel) to decode in advance.
    var prefetchCount: Int {
        get { defaults.integer(forKey: Key.prefetchCount) }
        set { store(newValue, Key.prefetchCount) }
    }

    /// Show a half-size decode while a full-size one loads: faster first look, brief softness.
    var quickPreview: Bool {
        get { defaults.bool(forKey: Key.quickPreview) }
        set { store(newValue, Key.quickPreview) }
    }

    var cacheGB: Double {
        get { defaults.double(forKey: Key.cacheGB) }
        set { store(newValue, Key.cacheGB) }
    }

    var cacheBytes: Int { Int(cacheGB * 1_073_741_824) }

    // MARK: PDF

    static let holdZoomFactors: [Double] = [1.5, 2, 2.5, 3, 4]

    var pdfPageMode: PDFPageMode {
        get { PDFPageMode(rawValue: defaults.integer(forKey: Key.pdfPageMode)) ?? .automatic }
        set { store(newValue.rawValue, Key.pdfPageMode) }
    }

    var pdfRightToLeft: Bool {
        get { defaults.bool(forKey: Key.pdfRightToLeft) }
        set { store(newValue, Key.pdfRightToLeft) }
    }

    var pdfLayout: PDFLayout { PDFLayout(mode: pdfPageMode, rightToLeft: pdfRightToLeft) }

    /// Off by default so flipping through a photo folder never stops to render a PDF.
    var includePDFsInFolders: Bool {
        get { defaults.bool(forKey: Key.includePDFs) }
        set { store(newValue, Key.includePDFs) }
    }

    var holdToZoom: HoldToZoom {
        get { HoldToZoom(rawValue: defaults.integer(forKey: Key.holdToZoom)) ?? .pdfsOnly }
        set { store(newValue.rawValue, Key.holdToZoom) }
    }

    var holdZoomFactor: Double {
        get { defaults.double(forKey: Key.holdZoomFactor) }
        set { store(newValue, Key.holdZoomFactor) }
    }

    // MARK: Video

    /// Listed alongside pictures when browsing a folder (opening a video always lists its neighbours).
    var includeVideosInFolders: Bool {
        get { defaults.bool(forKey: Key.includeVideos) }
        set { store(newValue, Key.includeVideos) }
    }

    var videoAutoplay: Bool {
        get { defaults.bool(forKey: Key.videoAutoplay) }
        set { store(newValue, Key.videoAutoplay) }
    }

    var videoTimelinePictures: Bool {
        get { defaults.bool(forKey: Key.videoPreviews) }
        set { store(newValue, Key.videoPreviews) }
    }

    var videoLoopShortClips: Bool {
        get { defaults.bool(forKey: Key.videoLoopShort) }
        set { store(newValue, Key.videoLoopShort) }
    }

    /// Otherwise only forced subtitles (the ones translating dialogue in another language).
    var alwaysSubtitles: Bool {
        get { defaults.bool(forKey: Key.alwaysSubtitles) }
        set { store(newValue, Key.alwaysSubtitles) }
    }

    /// Index into VideoLanguages.all; 0 is English.
    var subtitleLanguage: Int {
        get { defaults.integer(forKey: Key.subtitleLanguage) }
        set { store(newValue, Key.subtitleLanguage) }
    }

    /// Percent, as last left. Saved quietly as it changes (no settings-changed notification).
    var videoVolume: Int {
        get { defaults.integer(forKey: Key.videoVolume) }
        set { defaults.set(min(max(newValue, 0), 100), forKey: Key.videoVolume) }
    }

    /// Whether video was left muted. Saved quietly as it changes.
    var videoMuted: Bool {
        get { defaults.bool(forKey: Key.videoMuted) }
        set { defaults.set(newValue, forKey: Key.videoMuted) }
    }

    private func store(_ value: Any, _ key: String) {
        defaults.set(value, forKey: key)
        NotificationCenter.default.post(name: Self.didChange, object: self)
    }
}

/// Adjustments remembered per file (JPEGView's "Save parameters to DB"), reapplied whenever
/// that file is opened again.
enum ParameterStore {
    private static let key = "savedParameters"

    static func adjustments(for url: URL) -> Adjustments? {
        guard let data = UserDefaults.standard.dictionary(forKey: key)?[url.path] as? Data else { return nil }
        return try? JSONDecoder().decode(Adjustments.self, from: data)
    }

    static func save(_ adjustments: Adjustments, for url: URL) {
        var all = UserDefaults.standard.dictionary(forKey: key) ?? [:]
        all[url.path] = try? JSONEncoder().encode(adjustments)
        UserDefaults.standard.set(all, forKey: key)
    }

    static func clear(for url: URL) {
        var all = UserDefaults.standard.dictionary(forKey: key) ?? [:]
        all[url.path] = nil
        UserDefaults.standard.set(all, forKey: key)
    }
}

/// Where you were in each PDF, so it opens there next time.
enum PDFPlaces {
    struct Place {
        var page = 0      // the first page on screen
        var screen = 0    // which screen (page or spread) that was, in `layout`
        var layout = ""   // the page layout then: if it's unchanged, `screen` is used directly
    }

    private static let key = "pdfPlaces"

    static func place(for url: URL) -> Place? {
        guard let saved = UserDefaults.standard.dictionary(forKey: key)?[url.path] as? [String: Any],
              let page = saved["page"] as? Int, let screen = saved["screen"] as? Int,
              let layout = saved["layout"] as? String else { return nil }
        return Place(page: page, screen: screen, layout: layout)
    }

    /// The first screen forgets the file (it opens there anyway).
    static func save(_ place: Place, for url: URL) {
        var all = UserDefaults.standard.dictionary(forKey: key) ?? [:]
        all[url.path] = place.screen > 0 ? ["page": place.page, "screen": place.screen, "layout": place.layout] : nil
        UserDefaults.standard.set(all, forKey: key)
    }
}

/// Keeps a closure alive as an AppKit target/action.
final class ActionHandler: NSObject {
    private let handler: (Any?) -> Void
    init(_ handler: @escaping (Any?) -> Void) { self.handler = handler }
    @objc func fire(_ sender: Any?) { handler(sender) }
}
