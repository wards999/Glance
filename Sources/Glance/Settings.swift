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
        static let startZoom = "startZoom"
        static let enlargeSmall = "enlargeSmallImages"
        static let background = "backgroundRGB"
        static let openFullScreen = "openFullScreen"
        static let fullScreenStyle = "fullScreenStyle"
        static let showInfo = "showInfo"
        static let showControls = "showControls"
        static let hideCursor = "hideCursorInFullScreen"
        static let pixelatedZoom = "pixelatedZoom"
        static let saveQuality = "saveQuality"
        static let confirmOverwrite = "confirmOverwrite"
        static let prefetchCount = "prefetchCount"
        static let cacheGB = "cacheGB"
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
            Key.startZoom: StartZoom.fit.rawValue,
            Key.enlargeSmall: false,
            Key.background: [0.0, 0.0, 0.0],
            Key.openFullScreen: false,
            Key.fullScreenStyle: FullScreenStyle.instant.rawValue,
            Key.showInfo: false,
            Key.showControls: true,
            Key.hideCursor: true,
            Key.pixelatedZoom: true,
            Key.saveQuality: 0.9,
            Key.confirmOverwrite: true,
            Key.prefetchCount: 3,
            Key.cacheGB: Self.defaultCacheGB,
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

    var cacheGB: Double {
        get { defaults.double(forKey: Key.cacheGB) }
        set { store(newValue, Key.cacheGB) }
    }

    var cacheBytes: Int { Int(cacheGB * 1_073_741_824) }

    private func store(_ value: Any, _ key: String) {
        defaults.set(value, forKey: key)
        NotificationCenter.default.post(name: Self.didChange, object: self)
    }
}

/// Keeps a closure alive as an AppKit target/action.
final class ActionHandler: NSObject {
    private let handler: (Any?) -> Void
    init(_ handler: @escaping (Any?) -> Void) { self.handler = handler }
    @objc func fire(_ sender: Any?) { handler(sender) }
}
