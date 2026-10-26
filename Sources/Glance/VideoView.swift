import AppKit
import CMpv
import OpenGL.GL3
import QuartzCore

protocol VideoViewDelegate: AnyObject {
    var videoViewIsFullScreen: Bool { get }
    func videoViewToggleFullScreen(_ view: VideoView)
    func videoViewDidLoadFile(_ view: VideoView)
    func videoView(_ view: VideoView, scrollWheel event: NSEvent)
}

/// Plays videos with libmpv (the same engine as Glance for Windows). mpv draws through OpenGL into
/// a layer; Glance draws the controls on top (play/pause, time, timeline with pictures from the
/// moment under the pointer, subtitles and audio, mute) and keeps every key and click.
final class VideoView: NSView {
    struct Options: Equatable {
        var autoplay = true
        var previews = true
        var loopShort = true
        var alwaysSubtitles = false
        var subtitleLanguage = 0  // index into VideoLanguages.all
    }

    weak var delegate: VideoViewDelegate?
    var options = Options()

    private let mpv: MPV
    private var handle: MPV.Handle?
    fileprivate var renderContext: OpaquePointer?
    private let surface = MPVSurfaceView()
    private let controls = VideoControlsView()
    private var wakePending = false

    private(set) var url: URL?
    private(set) var isOpen = false
    private var ended = false
    private(set) var duration = 0.0
    private(set) var videoSize = CGSize.zero
    private var lastSubtitle = 0
    private var pressedOnPicture = false
    private var scrubbing = false
    private var draggingVolume = false
    private var hideTimer: Timer?
    private var tickTimer: Timer?
    private var toastTimer: Timer?

    // Timeline pictures: a second, silent mpv on its own queue; the newest request wins.
    private let previews = PreviewMaker()
    private var pictures: [Int: (time: Double, image: CGImage)] = [:]  // made ahead: bucket -> keyframe picture
    private var exactPicture: (time: Double, image: CGImage)?          // the exact frame for the latest hover
    private var generation = 0

    init?(delegate: VideoViewDelegate) {
        guard let mpv = MPV.shared else { return nil }
        self.mpv = mpv
        self.delegate = delegate
        super.init(frame: .zero)
        wantsLayer = true
        layer?.backgroundColor = NSColor.black.cgColor
        surface.owner = self
        surface.frame = bounds
        surface.autoresizingMask = [.width, .height]
        addSubview(surface)
        controls.frame = bounds
        controls.autoresizingMask = [.width, .height]
        controls.isHidden = true
        addSubview(controls)
        previews.onPicture = { [weak self] generation, exact, time, image in
            guard let self, generation == self.generation else { return }
            if exact {
                self.exactPicture = (time, image)
            } else {
                self.pictures[self.bucket(for: time)] = (time, image)
            }
            self.updateControls()
        }
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    deinit {
        if let renderContext { mpv.renderFree(renderContext) }
        if let handle { mpv.terminateDestroy(handle) }
    }

    // MARK: - Player

    /// Starts mpv without a file, so the first video opens without waiting for it.
    @discardableResult
    func warm() -> Bool {
        if handle != nil { return true }
        guard let created = mpv.create() else { return false }
        for (name, value) in [
            ("vo", "libmpv"),
            ("hwdec", "auto-safe"),       // VideoToolbox decodes when it can
            ("idle", "yes"),
            ("keep-open", "yes"),         // stay on the last frame at the end
            ("pause", "yes"),
            ("input-default-bindings", "no"),
            ("input-vo-keyboard", "no"),
            ("osc", "no"),
            ("osd-level", "0"),
            ("load-scripts", "no"),
            ("ytdl", "no"),
            ("terminal", "no"),
            ("config", "no"),
            ("audio-display", "no"),
            // Subtitle files beside the video load even when named "Movie.en.srt" or kept in a Subs
            // folder; which one shows is Glance's choice (chooseSubtitles).
            ("sub-auto", "fuzzy"),
            ("sub-file-paths", "subs:Subs:subtitles:Subtitles"),
        ] {
            mpv.set(created, option: name, value)
        }
        if let caches = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first {
            let folder = caches.appendingPathComponent("Glance/mpv-shaders")
            try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
            mpv.set(created, option: "gpu-shader-cache-dir", folder.path)
        }
        guard mpv.initialize(created) >= 0 else {
            mpv.terminateDestroy(created)
            return false
        }
        handle = created
        mpv.setWakeupCallback(created, { context in
            guard let context else { return }
            let view = Unmanaged<VideoView>.fromOpaque(context).takeUnretainedValue()
            DispatchQueue.main.async { view.handleEvents() }
        }, Unmanaged.passUnretained(self).toOpaque())
        return true
    }

    /// Shows `url`, playing at once if `play`; otherwise paused on the first frame until play().
    func open(_ url: URL, play: Bool) {
        guard warm() else { return }
        self.url = url
        isOpen = true
        ended = false
        duration = 0
        videoSize = .zero
        pictures = [:]
        exactPicture = nil
        generation += 1
        mpv.set(handle, "pause", play ? "no" : "yes")
        mpv.set(handle, "loop-file", "no")
        mpv.run(handle, ["loadfile", url.path, "replace"])
        previews.load(options.previews ? url : nil, generation: generation)
        surface.layer?.setNeedsDisplay()
        updateControls()
        startTicking()
    }

    func play() {
        if isOpen { mpv.set(handle, "pause", "no") }
    }

    /// Stops playback and lets go of the file (so it can be moved to the Trash).
    func close() {
        guard isOpen else { return }
        isOpen = false
        scrubbing = false
        controls.hovering = false
        mpv.run(handle, ["stop"])
        generation += 1
        pictures = [:]
        previews.load(nil, generation: generation)
        setControlsVisible(false)
        tickTimer?.invalidate()
        tickTimer = nil
    }

    var isPaused: Bool { mpv.flag(handle, "pause") ?? true }
    var isPlaying: Bool { isOpen && !isPaused && !ended }

    /// "1920 × 1080 · 1:32:10" for the window's subtitle.
    var details: String? {
        guard isOpen, videoSize.width > 0 else { return nil }
        return "\(Int(videoSize.width)) × \(Int(videoSize.height))  ·  \(Self.clock(duration))"
    }

    private func handleEvents() {
        guard let handle else { return }
        while let event = mpv.waitEvent(handle, 0)?.pointee, event.event_id != MPV_EVENT_NONE {
            guard isOpen else { continue }
            switch event.event_id {
            case MPV_EVENT_FILE_LOADED:
                duration = mpv.double(handle, "duration") ?? 0
                videoSize = CGSize(width: mpv.double(handle, "width") ?? 0, height: mpv.double(handle, "height") ?? 0)
                // Short silent clips (phone "live" videos and the like) loop, like GIFs.
                let silent = (mpv.string(handle, "current-tracks/audio/id") ?? "").isEmpty
                if options.loopShort && silent && duration > 0 && duration < 30 {
                    mpv.set(handle, "loop-file", "inf")
                }
                if options.previews && duration > 0 { requestPicture(at: duration / 2) }  // warms the decoder
                chooseSubtitles()
                placeSubtitles()
                delegate?.videoViewDidLoadFile(self)
                updateControls()
            case MPV_EVENT_PLAYBACK_RESTART:
                updateControls()
            default:
                break
            }
        }
    }

    private func startTicking() {
        tickTimer?.invalidate()
        tickTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            guard let self, self.isOpen else { return }
            self.ended = self.mpv.flag(self.handle, "eof-reached") ?? false
            if !self.controls.isHidden { self.updateControls() }
        }
    }

    // MARK: - Playback commands

    func togglePause() {
        guard isOpen else { return }
        let position = mpv.double(handle, "time-pos") ?? 0
        if ended || (duration > 0 && position >= duration - 0.05) {
            mpv.run(handle, ["seek", "0", "absolute"])  // at the end: play again from the start
            ended = false
            play()
        } else {
            mpv.run(handle, ["cycle", "pause"])
        }
        updateControls()
        if isPaused { setControlsVisible(true) }
    }

    func seek(by seconds: Double) {
        guard isOpen else { return }
        mpv.run(handle, ["seek", String(format: "%.3f", seconds), "relative"])
        updateControls()
    }

    func frameStep(forward: Bool) {
        guard isOpen else { return }
        mpv.run(handle, [forward ? "frame-step" : "frame-back-step"])
        updateControls()
    }

    func changeVolume(by delta: Int) {
        guard isOpen else { return }
        mpv.run(handle, ["add", "volume", "\(delta)"])
        mpv.set(handle, "mute", "no")
        flash("Volume \(Int(mpv.double(handle, "volume") ?? 100))%")
    }

    /// Clicking or dragging along the volume slider: left is silent, right is full volume.
    private func setVolume(atX x: CGFloat) {
        let slider = controls.volumeRect
        guard slider.width > 0 else { return }
        let volume = Int((min(max((x - slider.minX) / slider.width, 0), 1) * 100).rounded())
        mpv.set(handle, "volume", "\(volume)")
        mpv.set(handle, "mute", "no")
        flash("Volume \(volume)%")
    }

    func toggleMute() {
        guard isOpen else { return }
        mpv.run(handle, ["cycle", "mute"])
        flash(mpv.flag(handle, "mute") == true ? "Muted" : "Sound on")
    }

    // MARK: - Drawing (called by the layer)

    fileprivate func glContextReady(_ context: CGLContextObj) {
        guard renderContext == nil, warm(), let handle else { return }
        CGLSetCurrentContext(context)
        var initParams = mpv_opengl_init_params(get_proc_address: { _, name in
            guard let name else { return nil }
            let symbol = CFStringCreateWithCString(kCFAllocatorDefault, name, CFStringBuiltInEncodings.ASCII.rawValue)
            guard let bundle = CFBundleGetBundleWithIdentifier("com.apple.opengl" as CFString) else { return nil }
            return CFBundleGetFunctionPointerForName(bundle, symbol)
        }, get_proc_address_ctx: nil)
        let apiType = strdup(MPV_RENDER_API_TYPE_OPENGL)
        defer { free(apiType) }
        var created: OpaquePointer?
        withUnsafeMutablePointer(to: &initParams) { initPointer in
            var params = [
                mpv_render_param(type: MPV_RENDER_PARAM_API_TYPE, data: UnsafeMutableRawPointer(apiType)),
                mpv_render_param(type: MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, data: UnsafeMutableRawPointer(initPointer)),
                mpv_render_param(type: MPV_RENDER_PARAM_INVALID, data: nil),
            ]
            if mpv.renderCreate(&created, handle, &params) < 0 { created = nil }
        }
        guard let created else { return }
        renderContext = created
        mpv.renderSetUpdateCallback(created, { context in
            guard let context else { return }
            let view = Unmanaged<VideoView>.fromOpaque(context).takeUnretainedValue()
            DispatchQueue.main.async { view.surface.layer?.setNeedsDisplay() }
        }, Unmanaged.passUnretained(self).toOpaque())
    }

    fileprivate func drawFrame() {
        guard let renderContext else { return }
        var framebuffer: GLint = 0
        glGetIntegerv(GLenum(GL_DRAW_FRAMEBUFFER_BINDING), &framebuffer)
        var viewport = [GLint](repeating: 0, count: 4)
        glGetIntegerv(GLenum(GL_VIEWPORT), &viewport)
        var fbo = mpv_opengl_fbo(fbo: Int32(framebuffer), w: viewport[2], h: viewport[3], internal_format: 0)
        var flip: Int32 = 1
        withUnsafeMutablePointer(to: &fbo) { fboPointer in
            withUnsafeMutablePointer(to: &flip) { flipPointer in
                var params = [
                    mpv_render_param(type: MPV_RENDER_PARAM_OPENGL_FBO, data: UnsafeMutableRawPointer(fboPointer)),
                    mpv_render_param(type: MPV_RENDER_PARAM_FLIP_Y, data: UnsafeMutableRawPointer(flipPointer)),
                    mpv_render_param(type: MPV_RENDER_PARAM_INVALID, data: nil),
                ]
                _ = mpv.renderRender(renderContext, &params)
            }
        }
    }

    fileprivate func didPresent() {
        if let renderContext { mpv.renderReportSwap(renderContext) }
    }

    // MARK: - Controls

    func setControlsVisible(_ visible: Bool) {
        let show = visible && isOpen && Settings.shared.showControls
        guard show == controls.isHidden else { return }
        controls.isHidden = !show
        controls.computeLayout()
        if !show { controls.hovering = false }
        placeSubtitles()
        updateControls()
    }

    /// Mouse moved: show the controls, then hide them (and in full screen the pointer) after a rest.
    private func noteActivity() {
        setControlsVisible(true)
        hideTimer?.invalidate()
        hideTimer = Timer.scheduledTimer(withTimeInterval: 2, repeats: false) { [weak self] _ in
            guard let self, self.isOpen else { return }
            let mouse = self.convert(self.window?.mouseLocationOutsideOfEventStream ?? .zero, from: nil)
            if self.scrubbing || self.draggingVolume || self.isPaused || self.controls.barRect.insetBy(dx: 0, dy: -8).contains(mouse) { return }
            self.setControlsVisible(false)
            if self.delegate?.videoViewIsFullScreen == true && Settings.shared.hideCursorInFullScreen {
                NSCursor.setHiddenUntilMouseMoves(true)
            }
        }
    }

    private func updateControls() {
        guard !controls.isHidden || controls.toast != nil else {
            controls.needsDisplay = true
            return
        }
        let position = min(max(mpv.double(handle, "time-pos") ?? 0, 0), max(duration, 0))
        controls.name = url.map { $0.deletingPathExtension().lastPathComponent } ?? ""
        controls.duration = duration
        controls.position = scrubbing ? time(atX: controls.hoverPoint.x) : position
        controls.isPaused = isPaused || ended
        controls.isMuted = mpv.flag(handle, "mute") ?? false
        controls.volume = min(max(mpv.double(handle, "volume") ?? 100, 0), 100)
        controls.showBar = !controls.isHidden
        if (controls.hovering || scrubbing) && duration > 0 {
            let at = time(atX: controls.hoverPoint.x)
            controls.hoverTime = at
            controls.picture = options.previews ? nearestPicture(to: at) : nil
        } else {
            controls.hoverTime = nil
            controls.picture = nil
        }
        controls.needsDisplay = true
    }

    private func time(atX x: CGFloat) -> Double {
        let track = controls.trackRect
        guard track.width > 0, duration > 0 else { return 0 }
        return Double(min(max((x - track.minX) / track.width, 0), 1)) * duration
    }

    private func flash(_ text: String) {
        controls.toast = text
        controls.isHidden = false
        updateControls()
        toastTimer?.invalidate()
        toastTimer = Timer.scheduledTimer(withTimeInterval: 1.8, repeats: false) { [weak self] _ in
            guard let self else { return }
            self.controls.toast = nil
            if !self.controls.showBar { self.controls.isHidden = true }
            self.controls.needsDisplay = true
        }
    }

    // MARK: - Mouse (the controls draw; this view handles the input)

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { false }  // keys stay with the window's picture view

    override func layout() {
        super.layout()
        controls.computeLayout()
        placeSubtitles()
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        trackingAreas.forEach(removeTrackingArea)
        addTrackingArea(NSTrackingArea(rect: .zero, options: [.mouseMoved, .mouseEnteredAndExited, .activeInKeyWindow, .inVisibleRect],
                                       owner: self, userInfo: nil))
    }

    override func mouseMoved(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        noteActivity()
        let over = !controls.isHidden && controls.timelineHitRect.contains(point)
        if over { requestPicture(at: time(atX: point.x)) }
        if over || over != controls.hovering {
            controls.hovering = over
            controls.hoverPoint = point
            updateControls()
        }
    }

    override func mouseExited(with event: NSEvent) {
        controls.hovering = false
        updateControls()
    }

    override func mouseDown(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        if event.clickCount == 2 {
            // The first click already played or paused; undo that, and switch full screen.
            if !controls.barRect.contains(point) {
                togglePause()
                delegate?.videoViewToggleFullScreen(self)
            }
            pressedOnPicture = false
            return
        }
        pressedOnPicture = false
        if !controls.isHidden {
            if controls.playRect.contains(point) { togglePause(); return }
            if controls.muteRect.contains(point) { toggleMute(); return }
            if controls.ccRect.contains(point) { showTrackMenu(); return }
            if controls.volumeRect.contains(point) {
                draggingVolume = true
                setVolume(atX: point.x)
                return
            }
            if controls.timelineHitRect.contains(point) {
                scrubbing = true
                controls.hoverPoint = point
                mpv.run(handle, ["seek", String(format: "%.3f", time(atX: point.x)), "absolute+keyframes"])
                updateControls()
                return
            }
            if controls.barRect.contains(point) { return }
        }
        pressedOnPicture = true
    }

    override func mouseDragged(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        if draggingVolume {
            setVolume(atX: point.x)
            return
        }
        guard scrubbing else { return }
        controls.hoverPoint = point
        let at = time(atX: point.x)
        mpv.run(handle, ["seek", String(format: "%.3f", at), "absolute+keyframes"])  // keyframes while dragging: instant
        requestPicture(at: at)
        updateControls()
    }

    override func mouseUp(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        if draggingVolume {
            draggingVolume = false
        } else if scrubbing {
            scrubbing = false
            mpv.run(handle, ["seek", String(format: "%.3f", time(atX: point.x)), "absolute+exact"])
            ended = false
            updateControls()
        } else if pressedOnPicture && event.clickCount == 1 {
            togglePause()
        }
        pressedOnPicture = false
    }

    override func scrollWheel(with event: NSEvent) {
        delegate?.videoView(self, scrollWheel: event)
    }

    // MARK: - Timeline pictures

    private func bucket(for time: Double) -> Int {
        Int(time / max(1, duration / 300))
    }

    /// About a pixel of timeline, in seconds.
    private var exactTolerance: Double { max(0.04, duration / Double(max(controls.trackRect.width, 1))) }

    /// Asks for the exact frame at `time` (the newest request replaces any waiting one).
    private func requestPicture(at time: Double) {
        guard options.previews, duration > 0, let url else { return }
        if let exactPicture, abs(exactPicture.time - time) <= exactTolerance { return }  // already have it
        let width = Int(220 * (window?.backingScaleFactor ?? 2))
        previews.request(url: url, time: min(max(time, 0), duration), width: width, generation: generation)
    }

    private func nearestPicture(to time: Double) -> CGImage? {
        let wanted = bucket(for: time)
        // The nearest made ahead (until the exact one arrives), but not a far-away one.
        var best: (time: Double, image: CGImage)?
        if let nearest = pictures.keys.min(by: { abs($0 - wanted) < abs($1 - wanted) }), abs(nearest - wanted) <= 8 {
            best = pictures[nearest]
        }
        // The exact frame wins whenever it's nearer the pointer.
        if let exactPicture, abs(bucket(for: exactPicture.time) - wanted) <= 8,
           best.map({ abs(exactPicture.time - time) < abs($0.time - time) }) ?? true {
            best = exactPicture
        }
        return best?.image
    }

    // MARK: - Subtitles and audio tracks

    struct Track {
        var isSubtitle = false
        var id = 0
        var lang = "", title = "", file = ""
        var external = false, forced = false, hearingImpaired = false, selected = false
    }

    func tracks() -> [Track] {
        guard isOpen else { return [] }
        let count = Int(mpv.string(handle, "track-list/count") ?? "") ?? 0
        var list: [Track] = []
        for i in 0..<count {
            func field(_ name: String) -> String { mpv.string(handle, "track-list/\(i)/\(name)") ?? "" }
            let type = field("type")
            guard type == "sub" || type == "audio" else { continue }
            var track = Track()
            track.isSubtitle = type == "sub"
            track.id = Int(field("id")) ?? 0
            track.lang = field("lang").lowercased()
            track.title = field("title")
            track.file = field("external-filename")
            track.external = field("external") == "yes"
            track.forced = field("forced") == "yes"
            track.hearingImpaired = field("hearing-impaired") == "yes" || Self.saysHearingImpaired(track.title)
            track.selected = field("selected") == "yes"
            list.append(track)
        }
        return list
    }

    private static func saysHearingImpaired(_ title: String) -> Bool {
        let text = title.lowercased()
        return text.contains("sdh") || text.contains("hearing") || text.contains("(cc)") || text == "cc"
    }

    private func matchesLanguage(_ track: Track) -> Bool {
        guard !track.lang.isEmpty else { return false }
        let languages = VideoLanguages.all
        return languages[min(max(options.subtitleLanguage, 0), languages.count - 1)].codes.contains(track.lang)
    }

    /// "English - British | SDH", "French (forced)", or the file name of an .srt.
    func label(_ track: Track) -> String {
        var name = VideoLanguages.all.first { $0.codes.contains(track.lang) }?.name ?? track.lang
        var detail = track.title
        if track.external && !track.file.isEmpty { detail = (track.file as NSString).lastPathComponent }
        if !detail.isEmpty && detail.lowercased() != name.lowercased() { name += name.isEmpty ? detail : " - " + detail }
        if name.isEmpty { name = (track.isSubtitle ? "Subtitles " : "Audio ") + "\(track.id)" }
        if track.forced && !track.title.lowercased().contains("forced") { name += " (forced)" }
        if track.hearingImpaired && !Self.saysHearingImpaired(track.title) { name += " (SDH)" }
        return name
    }

    var currentSubtitle: Int { Int(mpv.string(handle, "sid") ?? "") ?? 0 }

    private func chooseSubtitles() {
        let subtitles = tracks().filter(\.isSubtitle)
        // A subtitle file put beside the film is there to be read, unless it is in another language.
        var pick = subtitles.first { $0.external && ($0.lang.isEmpty || matchesLanguage($0)) }
        if pick == nil && options.alwaysSubtitles {
            pick = subtitles.first { matchesLanguage($0) && !$0.forced && !$0.hearingImpaired }
                ?? subtitles.first { matchesLanguage($0) && !$0.forced }
        }
        // Forced subtitles translate dialogue in other languages; they belong on either way.
        if pick == nil { pick = subtitles.first { $0.forced && (matchesLanguage($0) || $0.lang.isEmpty) } }
        selectSubtitle(pick?.id ?? 0)
        lastSubtitle = pick?.id ?? 0
    }

    func selectSubtitle(_ id: Int, announce: Bool = false) {
        mpv.set(handle, "sid", id == 0 ? "no" : "\(id)")
        mpv.set(handle, "sub-visibility", "yes")
        if id != 0 { lastSubtitle = id }
        if announce {
            if id == 0 { flash("Subtitles off") }
            else if let track = tracks().first(where: { $0.isSubtitle && $0.id == id }) { flash("Subtitles: " + label(track)) }
        }
    }

    func selectAudio(_ id: Int) {
        mpv.set(handle, "aid", "\(id)")
        if let track = tracks().first(where: { !$0.isSubtitle && $0.id == id }) { flash("Audio: " + label(track)) }
    }

    /// J / Shift+J: Off, then each track in turn.
    func cycleSubtitles(forward: Bool) {
        guard isOpen else { return }
        let subtitles = tracks().filter(\.isSubtitle)
        guard !subtitles.isEmpty else {
            flash("No subtitles for this video")
            return
        }
        let ids = [0] + subtitles.map(\.id)
        let index = ids.firstIndex(of: currentSubtitle) ?? 0
        selectSubtitle(ids[(index + (forward ? 1 : ids.count - 1)) % ids.count], announce: true)
    }

    /// V: off, and back on (to the last one, or the best for your language).
    func toggleSubtitles() {
        guard isOpen else { return }
        if currentSubtitle != 0 {
            lastSubtitle = currentSubtitle
            selectSubtitle(0, announce: true)
            return
        }
        let subtitles = tracks().filter(\.isSubtitle)
        let pick = subtitles.first { $0.id == lastSubtitle }
            ?? subtitles.first { matchesLanguage($0) && !$0.forced && !$0.hearingImpaired }
            ?? subtitles.first { matchesLanguage($0) }
            ?? subtitles.first
        guard let pick else {
            flash("No subtitles for this video")
            return
        }
        selectSubtitle(pick.id, announce: true)
    }

    func loadSubtitleFile() {
        guard isOpen, let window else { return }
        let panel = NSOpenPanel()
        panel.directoryURL = url?.deletingLastPathComponent()
        panel.allowedFileTypes = ["srt", "ass", "ssa", "vtt", "sub", "idx", "sup"]
        panel.beginSheetModal(for: window) { [weak self] response in
            guard let self, response == .OK, let file = panel.url else { return }
            self.mpv.run(self.handle, ["sub-add", file.path, "select"])
            self.lastSubtitle = self.currentSubtitle
            self.flash("Subtitles: " + file.lastPathComponent)
        }
    }

    /// The menu behind the CC button (and the right-click menu's Subtitles and Audio items).
    func trackMenuItems(target: AnyObject, subtitleAction: Selector, audioAction: Selector, loadAction: Selector) -> (subtitles: [NSMenuItem], audio: [NSMenuItem]) {
        let list = tracks()
        let current = currentSubtitle
        var subtitles: [NSMenuItem] = []
        let off = NSMenuItem(title: "Off", action: subtitleAction, keyEquivalent: "")
        off.target = target
        off.tag = 0
        off.state = current == 0 ? .on : .off
        subtitles.append(off)
        for track in list where track.isSubtitle {
            let item = NSMenuItem(title: label(track), action: subtitleAction, keyEquivalent: "")
            item.target = target
            item.tag = track.id
            item.state = track.id == current ? .on : .off
            subtitles.append(item)
        }
        subtitles.append(.separator())
        let load = NSMenuItem(title: "Load Subtitle File…", action: loadAction, keyEquivalent: "")
        load.target = target
        subtitles.append(load)
        var audio: [NSMenuItem] = []
        let audioTracks = list.filter { !$0.isSubtitle }
        if audioTracks.count > 1 {
            for track in audioTracks {
                let item = NSMenuItem(title: label(track), action: audioAction, keyEquivalent: "")
                item.target = target
                item.tag = track.id
                item.state = track.selected ? .on : .off
                audio.append(item)
            }
        }
        return (subtitles, audio)
    }

    private func showTrackMenu() {
        let menu = NSMenu()
        let items = trackMenuItems(target: self, subtitleAction: #selector(pickSubtitle(_:)),
                                   audioAction: #selector(pickAudio(_:)), loadAction: #selector(pickLoad(_:)))
        items.subtitles.forEach { menu.addItem($0) }
        if !items.audio.isEmpty {
            menu.addItem(.separator())
            let parent = NSMenuItem(title: "Audio Track", action: nil, keyEquivalent: "")
            parent.submenu = NSMenu(title: "Audio Track")
            items.audio.forEach { parent.submenu?.addItem($0) }
            menu.addItem(parent)
        }
        menu.popUp(positioning: nil, at: NSPoint(x: controls.ccRect.minX, y: controls.ccRect.minY), in: self)
    }

    @objc private func pickSubtitle(_ sender: NSMenuItem) { selectSubtitle(sender.tag, announce: true) }
    @objc private func pickAudio(_ sender: NSMenuItem) { selectAudio(sender.tag) }
    @objc private func pickLoad(_ sender: NSMenuItem) { loadSubtitleFile() }

    /// Keeps subtitles above the controls while they show.
    private func placeSubtitles() {
        guard isOpen else { return }
        // mpv measures the margin in pixels of a 720-pixel-high window; 22 is its own default.
        var margin = 22
        if !controls.isHidden && bounds.height > 0 {
            margin = max(22, Int((16 + 44 + 14) / bounds.height * 720))
        }
        mpv.set(handle, "sub-margin-y", "\(margin)")
    }

    static func clock(_ seconds: Double) -> String {
        let total = max(Int(seconds.isFinite ? seconds : 0), 0)
        if total >= 3600 { return String(format: "%d:%02d:%02d", total / 3600, total / 60 % 60, total % 60) }
        return String(format: "%d:%02d", total / 60, total % 60)
    }
}

// MARK: - Subtitle languages

enum VideoLanguages {
    struct Language {
        let name: String
        let codes: [String]
    }

    static let all: [Language] = [
        Language(name: "English", codes: ["en", "eng", "english"]),
        Language(name: "French", codes: ["fr", "fre", "fra", "french"]),
        Language(name: "German", codes: ["de", "ger", "deu", "german"]),
        Language(name: "Spanish", codes: ["es", "spa", "spanish"]),
        Language(name: "Italian", codes: ["it", "ita", "italian"]),
        Language(name: "Dutch", codes: ["nl", "dut", "nld", "dutch"]),
        Language(name: "Portuguese", codes: ["pt", "por", "portuguese"]),
        Language(name: "Swedish", codes: ["sv", "swe", "swedish"]),
        Language(name: "Danish", codes: ["da", "dan", "danish"]),
        Language(name: "Norwegian", codes: ["no", "nor", "nob", "norwegian"]),
        Language(name: "Finnish", codes: ["fi", "fin", "finnish"]),
        Language(name: "Polish", codes: ["pl", "pol", "polish"]),
        Language(name: "Greek", codes: ["el", "gre", "ell", "greek"]),
        Language(name: "Russian", codes: ["ru", "rus", "russian"]),
        Language(name: "Japanese", codes: ["ja", "jpn", "japanese"]),
        Language(name: "Korean", codes: ["ko", "kor", "korean"]),
        Language(name: "Chinese", codes: ["zh", "chi", "zho", "chinese"]),
        Language(name: "Arabic", codes: ["ar", "ara", "arabic"]),
    ]
}

// MARK: - The OpenGL surface mpv draws into

final class MPVSurfaceView: NSView {
    weak var owner: VideoView? {
        didSet { (layer as? MPVLayer)?.owner = owner }
    }

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    override func makeBackingLayer() -> CALayer {
        let layer = MPVLayer()
        layer.owner = owner
        return layer
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        (layer as? MPVLayer)?.owner = owner
        layer?.contentsScale = window?.backingScaleFactor ?? 2
    }

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        layer?.contentsScale = window?.backingScaleFactor ?? 2
    }

    override func hitTest(_ point: NSPoint) -> NSView? { nil }  // VideoView takes the mouse
}

final class MPVLayer: CAOpenGLLayer {
    weak var owner: VideoView?

    override init() {
        super.init()
        isAsynchronous = false
        needsDisplayOnBoundsChange = true
        backgroundColor = NSColor.black.cgColor
    }

    override init(layer: Any) {
        super.init(layer: layer)
        if let other = layer as? MPVLayer { owner = other.owner }
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    override func copyCGLPixelFormat(forDisplayMask mask: UInt32) -> CGLPixelFormatObj {
        let attributes: [CGLPixelFormatAttribute] = [
            kCGLPFAOpenGLProfile, CGLPixelFormatAttribute(UInt32(kCGLOGLPVersion_3_2_Core.rawValue)),
            kCGLPFADoubleBuffer, kCGLPFAAccelerated, kCGLPFAAllowOfflineRenderers,
            _CGLPixelFormatAttribute(rawValue: 0),
        ]
        var format: CGLPixelFormatObj?
        var count: GLint = 0
        CGLChoosePixelFormat(attributes, &format, &count)
        return format ?? super.copyCGLPixelFormat(forDisplayMask: mask)
    }

    override func copyCGLContext(forPixelFormat format: CGLPixelFormatObj) -> CGLContextObj {
        let context = super.copyCGLContext(forPixelFormat: format)
        owner?.glContextReady(context)
        return context
    }

    override func canDraw(inCGLContext context: CGLContextObj, pixelFormat: CGLPixelFormatObj,
                          forLayerTime time: CFTimeInterval, displayTime: UnsafePointer<CVTimeStamp>?) -> Bool {
        owner?.renderContext != nil
    }

    override func draw(inCGLContext context: CGLContextObj, pixelFormat: CGLPixelFormatObj,
                       forLayerTime time: CFTimeInterval, displayTime: UnsafePointer<CVTimeStamp>?) {
        owner?.drawFrame()
        glFlush()
        owner?.didPresent()
    }
}

// MARK: - The controls, drawn over the video

final class VideoControlsView: NSView {
    var name = ""
    var duration = 0.0
    var position = 0.0
    var isPaused = true
    var isMuted = false
    var volume = 100.0
    var showBar = false
    var hovering = false
    var hoverPoint = NSPoint.zero
    var hoverTime: Double?
    var picture: CGImage?
    var toast: String?

    private(set) var barRect = NSRect.zero
    private(set) var playRect = NSRect.zero
    private(set) var muteRect = NSRect.zero
    private(set) var ccRect = NSRect.zero
    private(set) var volumeRect = NSRect.zero
    private(set) var trackRect = NSRect.zero
    var timelineHitRect: NSRect { NSRect(x: trackRect.minX, y: barRect.minY, width: trackRect.width, height: barRect.height) }

    override var isFlipped: Bool { true }
    override func hitTest(_ point: NSPoint) -> NSView? { nil }  // VideoView takes the mouse

    override func layout() {
        super.layout()
        computeLayout()
    }

    func computeLayout() {
        let margin: CGFloat = 16, barHeight: CGFloat = 44, button: CGFloat = 36
        let timeWidth: CGFloat = duration >= 3600 ? 64 : 48
        barRect = NSRect(x: margin, y: bounds.height - margin - barHeight, width: bounds.width - 2 * margin, height: barHeight)
        let middle = barRect.midY
        playRect = NSRect(x: barRect.minX + 4, y: middle - button / 2, width: button, height: button)
        // Volume: a short slider at the right-hand end, the speaker (mute) just before it.
        volumeRect = NSRect(x: barRect.maxX - 14 - 80, y: barRect.minY, width: 80, height: barRect.height)
        muteRect = NSRect(x: volumeRect.minX - 6 - button, y: middle - button / 2, width: button, height: button)
        ccRect = muteRect.offsetBy(dx: -button, dy: 0)
        let nowRight = playRect.maxX + timeWidth
        let totalLeft = ccRect.minX - timeWidth
        trackRect = NSRect(x: nowRight + 8, y: middle - 2, width: max(totalLeft - 8 - (nowRight + 8), 0), height: 4)
    }

    override func draw(_ dirtyRect: NSRect) {
        computeLayout()
        let panel = NSColor(calibratedWhite: 0.07, alpha: 0.78)
        let font = NSFont.systemFont(ofSize: 13)
        let white: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: NSColor.white]
        let soft: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: NSColor.white.withAlphaComponent(0.75)]

        func centred(_ text: String, in rect: NSRect, _ attributes: [NSAttributedString.Key: Any]) {
            let size = (text as NSString).size(withAttributes: attributes)
            (text as NSString).draw(at: NSPoint(x: rect.midX - size.width / 2, y: rect.midY - size.height / 2), withAttributes: attributes)
        }
        func symbol(_ name: String, in rect: NSRect) {
            guard let image = NSImage(systemSymbolName: name, accessibilityDescription: nil)?
                .withSymbolConfiguration(.init(pointSize: 15, weight: .regular)) else { return }
            let tinted = NSImage(size: image.size, flipped: false) { bounds in
                image.draw(in: bounds)
                NSColor.white.set()
                bounds.fill(using: .sourceAtop)
                return true
            }
            let size = tinted.size
            tinted.draw(in: NSRect(x: rect.midX - size.width / 2, y: rect.midY - size.height / 2, width: size.width, height: size.height),
                        from: .zero, operation: .sourceOver, fraction: 1, respectFlipped: true, hints: nil)
        }

        if showBar {
            panel.setFill()
            NSBezierPath(roundedRect: barRect, xRadius: 10, yRadius: 10).fill()
            symbol(isPaused ? "play.fill" : "pause.fill", in: playRect)
            symbol("captions.bubble", in: ccRect)
            // The speaker shows the level: muted, then one to three waves.
            let speaker = isMuted || volume < 1 ? "speaker.slash.fill"
                : volume < 34 ? "speaker.wave.1.fill" : volume < 67 ? "speaker.wave.2.fill" : "speaker.wave.3.fill"
            symbol(speaker, in: muteRect)
            // The volume slider: a thin track, filled to the level (dimmed while muted), with a knob.
            let slider = NSRect(x: volumeRect.minX, y: volumeRect.midY - 2, width: volumeRect.width, height: 4)
            NSColor.white.withAlphaComponent(0.3).setFill()
            NSBezierPath(roundedRect: slider, xRadius: 2, yRadius: 2).fill()
            var level = slider
            level.size.width = slider.width * CGFloat(volume / 100)
            (isMuted ? NSColor.white.withAlphaComponent(0.3) : NSColor.white).setFill()
            NSBezierPath(roundedRect: level, xRadius: 2, yRadius: 2).fill()
            NSBezierPath(ovalIn: NSRect(x: level.maxX - 5, y: slider.midY - 5, width: 10, height: 10)).fill()
            let timeWidth: CGFloat = duration >= 3600 ? 64 : 48
            centred(VideoView.clock(position), in: NSRect(x: playRect.maxX, y: barRect.minY, width: timeWidth, height: barRect.height), white)
            centred(VideoView.clock(duration), in: NSRect(x: ccRect.minX - timeWidth, y: barRect.minY, width: timeWidth, height: barRect.height), soft)

            // Timeline: thicker under the pointer, played part solid, a knob at the playhead.
            var track = trackRect
            if hovering { track = track.insetBy(dx: 0, dy: -1.5) }
            NSColor.white.withAlphaComponent(0.3).setFill()
            NSBezierPath(roundedRect: track, xRadius: track.height / 2, yRadius: track.height / 2).fill()
            if duration > 0 {
                var played = track
                played.size.width = track.width * CGFloat(min(max(position / duration, 0), 1))
                NSColor.white.setFill()
                NSBezierPath(roundedRect: played, xRadius: track.height / 2, yRadius: track.height / 2).fill()
                NSBezierPath(ovalIn: NSRect(x: played.maxX - 6, y: track.midY - 6, width: 12, height: 12)).fill()
            }

            if let hoverTime {
                // The picture for the moment under the pointer, with its time.
                let boxWidth: CGFloat = picture.map { CGFloat($0.width) / (window?.backingScaleFactor ?? 2) } ?? 70
                let boxHeight: CGFloat = picture.map { CGFloat($0.height) / (window?.backingScaleFactor ?? 2) } ?? 0
                let labelHeight: CGFloat = 22
                let x = min(max(hoverPoint.x - boxWidth / 2, barRect.minX), barRect.maxX - boxWidth)
                let bottom = barRect.minY - 10
                let frame = NSRect(x: x - 3, y: bottom - boxHeight - labelHeight - 3, width: boxWidth + 6, height: boxHeight + labelHeight + 3)
                panel.setFill()
                NSBezierPath(roundedRect: frame, xRadius: 6, yRadius: 6).fill()
                if let picture {
                    NSImage(cgImage: picture, size: NSSize(width: boxWidth, height: boxHeight))
                        .draw(in: NSRect(x: x, y: frame.minY + 3, width: boxWidth, height: boxHeight),
                              from: .zero, operation: .sourceOver, fraction: 1, respectFlipped: true, hints: nil)
                }
                centred(VideoView.clock(hoverTime), in: NSRect(x: frame.minX, y: frame.maxY - labelHeight, width: frame.width, height: labelHeight), white)
            } else if !name.isEmpty {
                // The file's name, just above the bar (there's no title bar in full screen).
                let size = (name as NSString).size(withAttributes: white)
                let width = min(size.width, barRect.width * 0.6)
                let box = NSRect(x: barRect.minX, y: barRect.minY - 8 - 30, width: width + 24, height: 30)
                panel.setFill()
                NSBezierPath(roundedRect: box, xRadius: 8, yRadius: 8).fill()
                let style = NSMutableParagraphStyle()
                style.lineBreakMode = .byTruncatingMiddle
                var attributes = white
                attributes[.paragraphStyle] = style
                (name as NSString).draw(in: NSRect(x: box.minX + 12, y: box.midY - size.height / 2, width: width, height: size.height),
                                        withAttributes: attributes)
            }
        }

        if let toast {
            // A moment's message ("Subtitles: English") above the bar.
            let size = (toast as NSString).size(withAttributes: white)
            let width = min(max(size.width + 40, 200), bounds.width - 32)
            let bottom = barRect.minY - 18 - (hoverTime != nil ? 170 : 0)
            let box = NSRect(x: bounds.midX - width / 2, y: bottom - 34, width: width, height: 34)
            panel.setFill()
            NSBezierPath(roundedRect: box, xRadius: 8, yRadius: 8).fill()
            centred(toast, in: box, white)
        }
    }
}

// MARK: - Timeline pictures

/// A second, silent mpv: no audio, software decoding of the nearest keyframe only, drawn small by
/// mpv's software renderer. It runs below normal priority so playback never waits for it.
final class PreviewMaker {
    var onPicture: ((Int, Bool, Double, CGImage) -> Void)?  // generation, exact, time shown, picture

    private let queue = DispatchQueue(label: "glance.video-previews", qos: .utility)
    private let lock = NSLock()
    private var wantedURL: URL?
    private var wantedTime: Double?
    private var wantedWidth = 440
    private var wantedGeneration = 0
    private var working = false

    // Owned by `queue`.
    private var mpv: MPV?
    private var handle: MPV.Handle?
    private var context: OpaquePointer?
    private var loaded: URL?

    func load(_ url: URL?, generation: Int) {
        lock.lock()
        wantedURL = url
        wantedTime = nil
        wantedGeneration = generation
        lock.unlock()
        kick()
    }

    func request(url: URL, time: Double, width: Int, generation: Int) {
        lock.lock()
        wantedURL = url
        wantedTime = time
        wantedWidth = width
        wantedGeneration = generation
        lock.unlock()
        kick()
    }

    private func kick() {
        lock.lock()
        let start = !working
        working = true
        lock.unlock()
        if start { queue.async { self.work() } }
    }

    // Pictures made ahead across the whole timeline once a video opens, coarse to fine (8 evenly
    // spaced, then 16, 32, ... up to 128), so hovering finds one nearby straight away. What you
    // hover over is always made first. Owned by `queue`.
    private var ahead: [Double] = []
    private var nextAhead = 0

    private static func aheadTimes(duration: Double) -> [Double] {
        // About one every 2 seconds for short clips, at most 128.
        let count = min(max(Int(duration / 2), 0), 128)
        guard count > 0 else { return [] }
        var taken = [Bool](repeating: false, count: count)
        var times: [Double] = []
        var stride = 16
        while stride >= 1 {
            var i = 0
            while i < count {
                if !taken[i] {
                    taken[i] = true
                    times.append((Double(i) + 0.5) * duration / Double(count))
                }
                i += max(1, count / (128 / stride))
            }
            stride /= 2
        }
        for i in 0..<count where !taken[i] { times.append((Double(i) + 0.5) * duration / Double(count)) }
        return times
    }

    private func work() {
        while true {
            lock.lock()
            let url = wantedURL, width = wantedWidth, generation = wantedGeneration
            var time = wantedTime
            wantedTime = nil
            let moreAhead = url != nil && url == loaded && nextAhead < ahead.count
            if url == loaded && time == nil && !moreAhead {
                working = false
                lock.unlock()
                return
            }
            lock.unlock()
            guard start() else {
                lock.lock()
                working = false
                lock.unlock()
                return
            }
            if url != loaded {
                ahead = []
                nextAhead = 0
                if let url {
                    mpv?.run(handle, ["loadfile", url.path, "replace"])
                    loaded = (mpv?.wait(handle, for: MPV_EVENT_FILE_LOADED, timeout: 10) ?? false) ? url : nil
                    if loaded != nil, let duration = mpv?.double(handle, "duration") {
                        ahead = Self.aheadTimes(duration: duration)
                    }
                } else {
                    mpv?.run(handle, ["stop"])
                    loaded = nil
                }
            }
            var exact = true
            if time == nil, url != nil, url == loaded, nextAhead < ahead.count {
                time = ahead[nextAhead]
                nextAhead += 1
                exact = false  // made ahead: the keyframe is enough, and quick
            }
            guard let time, let url, url == loaded, let (shown, image) = picture(at: time, width: width, exact: exact) else { continue }
            DispatchQueue.main.async { self.onPicture?(generation, exact, shown, image) }
        }
    }

    private func start() -> Bool {
        if handle != nil { return true }
        guard let mpv = MPV.shared, let created = mpv.create() else { return false }
        // Exact frames mean decoding on from a keyframe: the GPU (VideoToolbox) does that fastest
        // for HEVC, AV1 and VP9, the CPU for H.264.
        for (name, value) in [("vo", "libmpv"), ("aid", "no"), ("sid", "no"), ("hr-seek", "yes"), ("pause", "yes"),
                              ("hwdec", "auto-copy"), ("hwdec-codecs", "hevc,av1,vp9"), ("idle", "yes"), ("keep-open", "yes"),
                              ("vd-lavc-fast", "yes"), ("cache", "no"), ("demuxer-max-bytes", "8MiB"),
                              ("load-scripts", "no"), ("ytdl", "no"), ("terminal", "no"), ("config", "no")] {
            mpv.set(created, option: name, value)
        }
        guard mpv.initialize(created) >= 0 else {
            mpv.terminateDestroy(created)
            return false
        }
        let apiType = strdup(MPV_RENDER_API_TYPE_SW)
        defer { free(apiType) }
        var params = [mpv_render_param(type: MPV_RENDER_PARAM_API_TYPE, data: UnsafeMutableRawPointer(apiType)),
                      mpv_render_param(type: MPV_RENDER_PARAM_INVALID, data: nil)]
        var created2: OpaquePointer?
        guard mpv.renderCreate(&created2, created, &params) >= 0, let renderContext = created2 else {
            mpv.terminateDestroy(created)
            return false
        }
        self.mpv = mpv
        handle = created
        context = renderContext
        return true
    }

    private func render(into buffer: inout [UInt8], width: Int, height: Int) {
        guard let mpv, let context else { return }
        var size: [Int32] = [Int32(width), Int32(height)]
        var stride = width * 4
        let format = strdup("bgr0")
        defer { free(format) }
        buffer = [UInt8](repeating: 0, count: stride * height)
        size.withUnsafeMutableBufferPointer { sizePointer in
            withUnsafeMutablePointer(to: &stride) { stridePointer in
                buffer.withUnsafeMutableBytes { pixels in
                    var params = [
                        mpv_render_param(type: MPV_RENDER_PARAM_SW_SIZE, data: UnsafeMutableRawPointer(sizePointer.baseAddress)),
                        mpv_render_param(type: MPV_RENDER_PARAM_SW_FORMAT, data: UnsafeMutableRawPointer(format)),
                        mpv_render_param(type: MPV_RENDER_PARAM_SW_STRIDE, data: UnsafeMutableRawPointer(stridePointer)),
                        mpv_render_param(type: MPV_RENDER_PARAM_SW_POINTER, data: pixels.baseAddress),
                        mpv_render_param(type: MPV_RENDER_PARAM_INVALID, data: nil),
                    ]
                    _ = mpv.renderRender(context, &params)
                }
            }
        }
    }

    private func picture(at time: Double, width: Int, exact: Bool) -> (Double, CGImage)? {
        guard let mpv, let context else { return nil }
        let frameFlag = UInt64(MPV_RENDER_UPDATE_FRAME.rawValue)
        // The picture's size, from the video's shape: asked before seeking, because once a frame
        // is waiting mpv answers nothing until it has been drawn (a ~200 ms stall).
        let w = mpv.double(handle, "dwidth") ?? 16, h = mpv.double(handle, "dheight") ?? 9
        let height = max(2, Int((Double(width) * (w > 0 && h > 0 ? h / w : 9.0 / 16)).rounded()))
        // A frame mpv drew before this seek mustn't be taken for the new one.
        var scratch: [UInt8] = []
        if mpv.renderUpdate(context) & frameFlag != 0 { render(into: &scratch, width: 16, height: 16) }
        while let event = mpv.waitEvent(handle, 0)?.pointee, event.event_id != MPV_EVENT_NONE {}  // old seeks and restarts
        mpv.run(handle, ["seek", String(format: "%.3f", time), exact ? "absolute+exact" : "absolute+keyframes"])
        // Take the frame the moment it's decoded. (Not "playback restarted": with this renderer mpv
        // only reports that once the frame has been drawn, so waiting for it costs ~200 ms each.)
        let deadline = Date().addingTimeInterval(1.5)
        while mpv.renderUpdate(context) & frameFlag == 0 && Date() < deadline { usleep(1000) }
        // Draw it straight away, then ask where a keyframe seek landed.
        var pixels: [UInt8] = []
        render(into: &pixels, width: width, height: height)
        let shown = mpv.double(handle, "time-pos") ?? time
        let data = Data(pixels) as CFData
        guard let provider = CGDataProvider(data: data) else { return nil }
        guard let image = CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: width * 4,
                                  space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGBitmapInfo(rawValue: CGBitmapInfo.byteOrder32Little.rawValue | CGImageAlphaInfo.noneSkipFirst.rawValue),
                                  provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent) else { return nil }
        return (shown, image)
    }
}
