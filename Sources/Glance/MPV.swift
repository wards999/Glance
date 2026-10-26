import CMpv
import Foundation

/// The libmpv functions Glance uses, looked up at run time. libmpv (mpv's player engine, with
/// FFmpeg) lives in the app's Frameworks folder and is loaded the first time a video is opened,
/// so it costs nothing at launch or when viewing pictures; without it, videos aren't listed.
struct MPV {
    typealias Handle = OpaquePointer

    let create: @convention(c) () -> Handle?
    let initialize: @convention(c) (Handle?) -> Int32
    let terminateDestroy: @convention(c) (Handle?) -> Void
    let setOptionString: @convention(c) (Handle?, UnsafePointer<CChar>?, UnsafePointer<CChar>?) -> Int32
    let setPropertyString: @convention(c) (Handle?, UnsafePointer<CChar>?, UnsafePointer<CChar>?) -> Int32
    let getProperty: @convention(c) (Handle?, UnsafePointer<CChar>?, mpv_format, UnsafeMutableRawPointer?) -> Int32
    let getPropertyString: @convention(c) (Handle?, UnsafePointer<CChar>?) -> UnsafeMutablePointer<CChar>?
    let free: @convention(c) (UnsafeMutableRawPointer?) -> Void
    let command: @convention(c) (Handle?, UnsafeMutablePointer<UnsafePointer<CChar>?>?) -> Int32
    let waitEvent: @convention(c) (Handle?, Double) -> UnsafeMutablePointer<mpv_event>?
    let setWakeupCallback: @convention(c) (Handle?, (@convention(c) (UnsafeMutableRawPointer?) -> Void)?, UnsafeMutableRawPointer?) -> Void
    let renderCreate: @convention(c) (UnsafeMutablePointer<OpaquePointer?>?, Handle?, UnsafeMutablePointer<mpv_render_param>?) -> Int32
    let renderSetUpdateCallback: @convention(c) (OpaquePointer?, (@convention(c) (UnsafeMutableRawPointer?) -> Void)?, UnsafeMutableRawPointer?) -> Void
    let renderUpdate: @convention(c) (OpaquePointer?) -> UInt64
    let renderRender: @convention(c) (OpaquePointer?, UnsafeMutablePointer<mpv_render_param>?) -> Int32
    let renderReportSwap: @convention(c) (OpaquePointer?) -> Void
    let renderFree: @convention(c) (OpaquePointer?) -> Void

    static var frameworks: URL? { Bundle.main.privateFrameworksURL }

    /// Whether libmpv is in the app (checked without loading it).
    static let available: Bool = {
        guard let folder = frameworks else { return false }
        return FileManager.default.fileExists(atPath: folder.appendingPathComponent("libmpv.2.dylib").path)
    }()

    /// Loads libmpv on first use; nil if it's missing or unusable.
    static let shared: MPV? = {
        guard available, let folder = frameworks else { return nil }
        let path = folder.appendingPathComponent("libmpv.2.dylib").path
        guard let library = dlopen(path, RTLD_NOW | RTLD_LOCAL) else {
            NSLog("Glance: can't load libmpv: %@", String(cString: dlerror()))
            return nil
        }
        func find<T>(_ name: String, as type: T.Type) -> T? {
            guard let symbol = dlsym(library, name) else { return nil }
            return unsafeBitCast(symbol, to: type)
        }
        guard
            let create = find("mpv_create", as: (@convention(c) () -> Handle?).self),
            let initialize = find("mpv_initialize", as: (@convention(c) (Handle?) -> Int32).self),
            let terminateDestroy = find("mpv_terminate_destroy", as: (@convention(c) (Handle?) -> Void).self),
            let setOptionString = find("mpv_set_option_string", as: (@convention(c) (Handle?, UnsafePointer<CChar>?, UnsafePointer<CChar>?) -> Int32).self),
            let setPropertyString = find("mpv_set_property_string", as: (@convention(c) (Handle?, UnsafePointer<CChar>?, UnsafePointer<CChar>?) -> Int32).self),
            let getProperty = find("mpv_get_property", as: (@convention(c) (Handle?, UnsafePointer<CChar>?, mpv_format, UnsafeMutableRawPointer?) -> Int32).self),
            let getPropertyString = find("mpv_get_property_string", as: (@convention(c) (Handle?, UnsafePointer<CChar>?) -> UnsafeMutablePointer<CChar>?).self),
            let free = find("mpv_free", as: (@convention(c) (UnsafeMutableRawPointer?) -> Void).self),
            let command = find("mpv_command", as: (@convention(c) (Handle?, UnsafeMutablePointer<UnsafePointer<CChar>?>?) -> Int32).self),
            let waitEvent = find("mpv_wait_event", as: (@convention(c) (Handle?, Double) -> UnsafeMutablePointer<mpv_event>?).self),
            let setWakeupCallback = find("mpv_set_wakeup_callback", as: (@convention(c) (Handle?, (@convention(c) (UnsafeMutableRawPointer?) -> Void)?, UnsafeMutableRawPointer?) -> Void).self),
            let renderCreate = find("mpv_render_context_create", as: (@convention(c) (UnsafeMutablePointer<OpaquePointer?>?, Handle?, UnsafeMutablePointer<mpv_render_param>?) -> Int32).self),
            let renderSetUpdateCallback = find("mpv_render_context_set_update_callback", as: (@convention(c) (OpaquePointer?, (@convention(c) (UnsafeMutableRawPointer?) -> Void)?, UnsafeMutableRawPointer?) -> Void).self),
            let renderUpdate = find("mpv_render_context_update", as: (@convention(c) (OpaquePointer?) -> UInt64).self),
            let renderRender = find("mpv_render_context_render", as: (@convention(c) (OpaquePointer?, UnsafeMutablePointer<mpv_render_param>?) -> Int32).self),
            let renderReportSwap = find("mpv_render_context_report_swap", as: (@convention(c) (OpaquePointer?) -> Void).self),
            let renderFree = find("mpv_render_context_free", as: (@convention(c) (OpaquePointer?) -> Void).self)
        else {
            NSLog("Glance: libmpv is missing functions")
            return nil
        }
        return MPV(create: create, initialize: initialize, terminateDestroy: terminateDestroy,
                   setOptionString: setOptionString, setPropertyString: setPropertyString, getProperty: getProperty,
                   getPropertyString: getPropertyString, free: free, command: command, waitEvent: waitEvent,
                   setWakeupCallback: setWakeupCallback, renderCreate: renderCreate,
                   renderSetUpdateCallback: renderSetUpdateCallback, renderUpdate: renderUpdate,
                   renderRender: renderRender, renderReportSwap: renderReportSwap, renderFree: renderFree)
    }()

    // MARK: Conveniences

    func set(_ handle: Handle?, option name: String, _ value: String) {
        if setOptionString(handle, name, value) < 0 { NSLog("Glance: mpv option %@=%@ not accepted", name, value) }
    }

    func set(_ handle: Handle?, _ name: String, _ value: String) {
        _ = setPropertyString(handle, name, value)
    }

    func run(_ handle: Handle?, _ arguments: [String]) {
        var cStrings = arguments.map { strdup($0) }
        defer { cStrings.forEach { Foundation.free($0) } }
        var pointers: [UnsafePointer<CChar>?] = cStrings.map { UnsafePointer($0) }
        pointers.append(nil)
        _ = pointers.withUnsafeMutableBufferPointer { command(handle, $0.baseAddress) }
    }

    func string(_ handle: Handle?, _ name: String) -> String? {
        guard let value = getPropertyString(handle, name) else { return nil }
        defer { free(value) }
        return String(cString: value)
    }

    func double(_ handle: Handle?, _ name: String) -> Double? {
        var value = 0.0
        return getProperty(handle, name, MPV_FORMAT_DOUBLE, &value) >= 0 ? value : nil
    }

    func flag(_ handle: Handle?, _ name: String) -> Bool? {
        var value: Int32 = 0
        return getProperty(handle, name, MPV_FORMAT_FLAG, &value) >= 0 ? value != 0 : nil
    }

    /// Waits up to `timeout` seconds for `wanted`; false on timeout or if the file failed to load.
    func wait(_ handle: Handle?, for wanted: mpv_event_id, timeout: Double) -> Bool {
        let end = Date().addingTimeInterval(timeout)
        while true {
            let left = end.timeIntervalSinceNow
            if left <= 0 { return false }
            guard let event = waitEvent(handle, left)?.pointee else { return false }
            if event.event_id == wanted { return true }
            if event.event_id == MPV_EVENT_END_FILE || event.event_id == MPV_EVENT_SHUTDOWN { return false }
        }
    }
}

enum VideoFiles {
    static let extensions: Set<String> = ["mp4", "m4v", "mov", "mkv", "webm", "avi", "wmv", "flv", "mpg", "mpeg",
                                          "m2ts", "mts", "ts", "3gp", "ogv", "vob", "asf", "f4v", "rm", "rmvb", "divx"]

    static func isVideo(_ url: URL) -> Bool { extensions.contains(url.pathExtension.lowercased()) }

    /// Video files Glance can play: needs libmpv in the app.
    static func isPlayable(_ url: URL) -> Bool { MPV.available && isVideo(url) }
}
