# Glance

A fast picture, PDF and video viewer for **macOS and Windows**, modeled on JPEGView: minimal UI,
and built to put pixels on screen as fast as possible, including very large
files. It opens **JPEG, PNG, GIF (animated), TIFF (multi-page), BMP, WebP, HEIC/HEIF, AVIF,
camera RAW** and more (on the Mac also JPEG XL and PSD), reads **PDFs** like a magazine (cover
first, then two-page spreads, with pages turned instantly and hold to zoom), and plays **videos**:
MP4, MKV, WebM, MOV, AVI and more, with subtitles and a timeline that shows the exact frame under
the pointer. Pictures, PDFs and videos in one folder are browsed together. Each version is native
to its platform: Swift with AppKit, ImageIO, Core Animation and Core Image on the Mac; C++ with
Win32, WIC and Direct2D on Windows. Video uses libmpv (mpv's engine, built on FFmpeg), loaded only
when a video opens.

On Windows, Glance opens every format Windows itself can decode; WebP, HEIC, AVIF and RAW need
Microsoft's free image extensions from the Microsoft Store if they aren't already installed.

## Download

Get the latest version from [Releases](https://github.com/wards999/Glance/releases/latest).

**Mac** (macOS 13 Ventura or later, Apple Silicon): download **Glance.dmg**, open it and drag
Glance to Applications. The app isn't signed with an Apple developer certificate, so the first
time, right-click Glance in Applications, choose **Open**, then **Open** again. If macOS says the
app is damaged, run `xattr -dr com.apple.quarantine /Applications/Glance.app` once in Terminal.
Video playback is built in (it makes the download about 50 MB). To open videos in Glance from
Finder, select one, choose Get Info, set "Open with" to Glance, and click "Change All…".

**Windows** (Windows 10 or 11, 64-bit): download **Glance-windows.zip** and unzip it anywhere;
there's nothing to install. Run `Glance.exe`. The app isn't code-signed, so the first time
Windows may say it protected your PC: click **More info**, then **Run anyway**. Beside
`Glance.exe`, `pdfium.dll` reads PDFs and `libmpv-2.dll` (about 120 MB) plays videos. Each is
loaded only when you first open a PDF or a video, so they don't slow Glance down; delete either
if you don't want PDFs or videos.

When Glance first runs, it registers its file types for your user (no administrator prompt), so
it's offered in Open with and in Settings ▸ Default apps. Windows doesn't let apps make themselves
the default, so to choose what Glance opens, right-click ▸ **Make Glance the default app…**: it
opens Default apps at Glance's page, where you pick Glance for each kind of file. Those files then
show Glance's icons in Explorer: the Glance lens tinted green for pictures, red for PDFs and violet
for videos (on thumbnails, as the corner badge).

## Reading PDFs

Glance reads PDFs as well as pictures, and is built for magazines: pages are sharp at any zoom,
and the next pages are rendered ahead while you read, so turning a page is instant. (The Mac uses
macOS's own PDF engine; Windows uses PDFium, Chrome's PDF engine, in `pdfium.dll`.)

**Layout.** A magazine opens as its cover on its own, then two-page spreads, as in print. Change
this under right-click ▸ PDF pages (Windows) or Settings ▸ PDF: automatic, single page, two pages,
or two pages with cover, plus **right to left** for manga and other right-to-left books.

**Turning pages.** Pages turn before Glance moves on to the next file in the folder.

| | Next page | Previous page |
|---|---|---|
| Keys | Space, Page Down | Page Up; Backspace (Windows) or Delete (Mac) |
| Arrow keys (when the whole page fits) | → or ↓ | ← or ↑ |
| Mouse wheel (when set to change images) | Down | Up |
| Windows also | `]`, or Next on the button bar | `[`, or Previous on the button bar |

To leave a PDF from any page and go straight to the next (or previous) file: **Ctrl+Page Down**
(Ctrl+Page Up) on Windows, **⌥ Page Down** or **⌥↓** (⌥ Page Up, ⌥↑) on the Mac, or right-click ▸
Next File.

**Zooming in to read.** Double-click a spot to see it at 100%; double-click again (or press 0)
to see the whole page. While zoomed in, drag or use the arrow keys to move around the page, and
turn pages with Space / Page Down (or the wheel). The zoom is kept: the next page opens at the same
size at its top-left corner, where you'd start reading (top-right for right to left), and going
back opens the previous page at its bottom corner.

**Hold to zoom.** Press and hold on a page to magnify the part under the pointer, move to look
around, and let go to return. Choose the magnification, or turn it on for pictures as well, in
Settings.

**Also:**

- Password-protected PDFs ask for their password. It's kept in memory only, until Glance closes.
- When you browse a folder of photos, PDFs in it are skipped unless you turn on "Include PDFs
  when browsing folders" in Settings. Opening a PDF directly always works.
- Glance never changes a PDF. Save As exports the page or spread on screen as an image.

**PDF covers in Explorer (Windows).** Windows doesn't show PDF pages as thumbnails on its own.
`GlanceThumbnails.dll` adds that: Explorer shows each PDF's first page, whichever app opens PDFs.
Keep it next to `Glance.exe` and turn it on with right-click ▸ PDF pages ▸ **Show PDF covers in
Explorer** (choose it again to turn it off). Windows asks for administrator permission, because
Explorer only uses thumbnail handlers registered for all users. It runs in Windows' isolated
thumbnail process, so a damaged PDF can't affect Explorer. Updating it later is just replacing the
file; turn it on again only if you move the folder. The same from a PowerShell window, if you
prefer (`'/u', ` before the path removes it):

```powershell
Start-Process regsvr32 -Verb RunAs -ArgumentList '"C:\Utilities\Glance\GlanceThumbnails.dll"'
```

## Watching videos

Glance plays videos too: MP4, MKV, WebM, MOV, AVI and more, in practically any codec (H.264,
HEVC, AV1, VP9, ProRes…), decoded on the GPU. Both versions use libmpv, mpv's player engine built
on FFmpeg, which is only loaded when you open a video: launch and photo speed are unchanged.

- **Windows:** `libmpv-2.dll` next to `Glance.exe` (about 120 MB, from
  shinchiro/mpv-winbuild-cmake). Without it Glance simply doesn't list videos.
- **Mac:** built into the app (libmpv 0.41.0 and its libraries, for Apple Silicon and Intel, as
  published by IINA), drawn through OpenGL with hardware decoding (VideoToolbox).

Videos sit in a folder alongside your pictures. Browsing doesn't slow down for them: they aren't
decoded ahead, and a video starts playing as soon as you arrive (holding a key down to skim, it
waits until you stop). Short silent clips loop like GIFs.

| | |
|---|---|
| Space, or click the picture | Play / pause |
| ← / → | Back / forward 5 seconds (Shift: 30 s, Ctrl: 1 s) |
| , / . | Previous / next frame (while paused) |
| ↑ / ↓, Ctrl+wheel | Volume |
| M | Mute |
| J / Shift+J | Next / previous subtitle track (and off) |
| V | Subtitles off / back on |
| Page Down / Page Up, wheel | Next / previous file |
| Double-click | Full screen |

Moving the mouse shows the controls, with the video's name just above them:

⏮ ▶ ⏭ · time · timeline · length · CC · 🔊 volume slider · ⛶

Previous file, play/pause and next file on the left; subtitles and audio (CC), mute, a volume
slider and full screen on the right. Click or drag along the timeline to jump. Hover over it to
see **the exact frame at that moment**, as in MPC-BE: a second, silent decoder makes rough pictures
along the whole video ahead of time, and the exact frame under the pointer replaces the nearest one
within a few tens of milliseconds. The volume, and whether it was muted, are remembered between
sessions.

**Subtitles and audio tracks.** The CC button (and the right-click menu) lists every subtitle track
by language and name, plus Off and "Load subtitle file…", and the audio tracks when there's a
choice. A subtitle file beside the video loads by itself, including names like `Movie.en.srt` or
files in a `Subs` folder. When a film opens, Glance shows that file if there is one; otherwise only
forced subtitles (the ones translating dialogue in another language), or with Settings ▸ Video ▸
Subtitles set to Always, a full track in your language (English unless you choose another).
Subtitles move up while the controls show, so they're never hidden.

Settings ▸ Video also turns autoplay, timeline pictures, looping short clips, and listing videos
when browsing on or off. Changes, including whether PDFs and videos are listed, apply at once.

The right-click menu changes with what's on screen, in one layout so things are always in the same
place: Next File and Previous File first, then what's particular to pictures (zoom, transform,
adjustments), PDFs (pages and layout) or videos (play/pause, mute, subtitles and audio), then the
file, the clipboard, display and Glance itself.

## Build the Mac version

Requires macOS 13 or later and Xcode 15 or later (or the matching Command Line Tools).

```sh
./scripts/bundle.sh            # release build -> build/Glance.app
open build/Glance.app
```

For quick testing without a bundle:

```sh
swift run -c release Glance ~/Pictures/some-photo.jpg
```

Always use a release build when judging speed; debug builds are much slower.
To make Glance the default viewer, select an image in Finder, choose Get Info, set "Open with"
to Glance, and click "Change All…".

## Why it's fast (Mac)

| Technique | Where |
|---|---|
| Decodes only **what the window shows**, rounded to the nearest 1/2, 1/4 or 1/8 of full size. JPEG and HEIF decode those sizes directly, with no full-size decode and no resampling pass. A 4000×3000 photo shown 1800 px wide decodes at 2000×1500, about a quarter of the work. A bigger window or zooming in fetches a sharper level. | `ImagePipeline.decodeLongSide` |
| Optional **quick preview** (Settings ▸ Performance): when the full size is needed, a half-size version shows first. | `ImagePipeline.decode` |
| Decodes **off the main thread and fully up front** (`ShouldCacheImmediately`), so drawing never stalls. | `ImageDecoder`, `ImagePipeline` |
| **Prefetches** the next 3 images in the direction you're moving, plus 1 behind. Prefetched images appear in the same run-loop turn as the key press. | `ViewerWindowController.prefetchNeighbors` |
| **Skips stale work**: hold an arrow key and queued decodes for images you've already passed are dropped. | `ImagePipeline.drop`, `wanted` set |
| **Embedded preview first** for large files and RAW: the thumbnail stored in the file shows instantly while the real decode runs. RAW files use their large embedded JPEG instead of a full RAW develop. | `ImagePipeline.decode` |
| **GPU display**: the bitmap is uploaded once. Zoom, pan and rotation only change a Core Animation transform. | `ImageView` |
| **Zoom levels on demand**: zooming past the screen-sized bitmap decodes the smallest 1/2ⁿ level that's sharp enough, not always the full image. | `imageView(_:needsLongSide:)` |
| **Tiling** for bitmaps over 8192 px, so huge images stay under GPU texture limits. Tiles share the decoded pixels; nothing is copied. | `ImageView.rebuildTiles` |
| **LRU memory cache** sized to ¼ of RAM (4 GB max). | `ImagePipeline` |
| Opens the file **before** listing the folder; the folder scan runs in the background. | `ViewerWindowController.open` |

## Features compared with JPEGView (Mac)

Included:

- Every format ImageIO supports: JPEG, PNG, GIF, BMP, TIFF, WebP, HEIC/HEIF, AVIF (macOS 13+), JPEG XL (macOS 14+), PSD, camera RAW, and more
- Animated GIF/WebP/APNG playback; page navigation for multi-page TIFF
- Folder navigation sorted by name (Finder order), date modified, date created, size or random, with ascending/descending and loop options
- Zoom to fit, fill or 100%; Option-drag a rectangle to zoom into it; zoom around the cursor with the mouse wheel or pinch; drag to pan; double-click to toggle fit/100%
- Rotate and flip. **Lossless** when saved: only the EXIF orientation tag is rewritten (JPEG, and other formats ImageIO can retag)
- Brightness, contrast, saturation, gamma, sharpen, and cyan/red, magenta/green, yellow/blue color balance, plus auto correction. All run on the GPU, can be kept across images, and are written out when you save
- Crop: shift-drag to select, then Return or ⌘K
- Save (asks before re-encoding the original) and Save As JPEG/PNG/TIFF/HEIC with a quality setting. EXIF, GPS and IPTC metadata are kept
- Slideshow with 1–30 s intervals
- EXIF info overlay: camera, lens, exposure, date, GPS
- Bottom control bar that fades in on hover
- Full screen with cursor auto-hide, optionally on launch. The menu bar and Dock are hidden completely; every command is on the right-click menu. "Instant" style (the default) covers the screen with no animation or Space switch; macOS full screen is available as an option
- Move to Trash, Reveal in Finder, Set as Desktop Picture, Copy, drag and drop, mouse back/forward buttons, trackpad swipe between images
- A right-click menu laid out like JPEGView's, including Reload, Print, Set Modification Date (now or date taken), Copy Original Size, Copy File Path, Paste, Show Filename, and lossless rotate/flip written straight to the file
- **PDF reading**: pages rendered sharp at any zoom, decoded ahead like photos. Magazines open as a cover followed by two-page spreads (automatic, or choose single / two pages / two pages with cover, plus right to left). Space, arrows and the wheel turn pages before moving to the next file. Turning a page while zoomed in keeps the zoom and opens the new page at its top-left corner (top-right for right to left), or at the bottom corner when going back. **Hold to zoom**: press and hold on the page to magnify it, move to look around, let go to return. Password-protected PDFs ask for their password (kept in memory only). PDFs are skipped when browsing photo folders unless you turn that on, and they can't be overwritten: Save As exports the current page or spread as an image
- Adjustments can be saved per image (JPEGView's "Save parameters to DB") and are reapplied whenever that file is opened

Not included yet:

- Side-by-side comparison of two images
- Perspective correction and JPEGView's "local density correction" (auto correction uses Core Image's auto-adjust instead)
- Histogram, lossless JPEG *crop* (crops re-encode), batch rename and copy/move to folder, "movie" folder playback

## Settings (Mac)

JPEGView keeps most of its options in an INI file. Glance has a **Settings window** instead
(⌘, or Glance ▸ Settings…). Changes apply immediately and are saved automatically (in
UserDefaults). Menu toggles such as Loop or Show Info change the same settings, so the menus
and the window always agree.

| Tab | Options |
|---|---|
| PDF | Page layout (automatic / single / two pages / two pages with cover), right to left, include PDFs when browsing folders, hold to zoom (off / PDFs / everything) and its magnification |
| General | Sort order and direction, loop at end of folder, mouse wheel zooms or flips images, slideshow interval, keep adjustments between images |
| Display | Open in full screen, full screen style (instant or macOS), initial zoom (fit / fill / 100%), enlarge small images, background color, info overlay, hover controls, hide cursor in full screen, pixel view above 400% |
| Saving | JPEG/HEIC quality, ask before re-encoding the original |
| Performance | How many images to decode ahead (0–8), memory cache size |

## Keyboard (Mac)

| Key | Action |
|---|---|
| Space, Page Down, → / ↓ | Next image (arrows pan instead when zoomed in) |
| Delete, Page Up, ← / ↑ | Previous image |
| Home / End | First / last image |
| ⌘] / ⌘[ | Next / previous image |
| ] / [  (⌥⌘] / ⌥⌘[) | Next / previous page of a multi-page TIFF |
| + / − | Zoom in / out |
| 0, 1, 2 | Fit, 100%, fill |
| Double-click | Toggle fit / 100% at the cursor |
| Mouse wheel, pinch | Zoom around the cursor |
| Drag (zoomed in), two-finger scroll | Pan |
| Right-click, Control-click | Menu with every command (the only menu in full screen) |
| Drag (image fits the window) | Draw a rectangle to zoom into |
| Option-drag (any zoom) | Draw a rectangle to zoom into |
| Shift-drag | Select a crop area. Return crops, Esc clears |
| Z, ⌘3 | Zoom to the crop selection |
| R / L, ⌘R / ⌘L | Rotate right / left |
| H / V | Flip horizontal / vertical |
| A, F5 | Auto correction on/off |
| F4 | Keep adjustments for next images on/off |
| E, ⌘E | Adjustments panel |
| I, F2, ⌘I | Info overlay |
| W | Reveal in Finder (JPEGView's "open containing folder") |
| ⌘P | Print |
| ⌘C / ⌥⌘C | Copy the displayed image / copy at original size |
| ⇧⌘C | Copy file path |
| ⌘V | Paste (opens copied files or a copied image) |
| S, ⌥⌘P | Slideshow on/off |
| F, ⌃⌘F | Full screen (instant or macOS style, per Settings). Esc leaves it, or closes Glance if Settings ▸ General ▸ Escape key says so (JPEGView style) |
| ⌘S / ⇧⌘S | Save / Save As |
| ⌘⌫ | Move to Trash |
| ⇧⌘R | Reveal in Finder |
| ⌘, | Settings |

## Windows version

`windows/` is a native C++ port: Win32, WIC decoding (with JPEG decoded directly at 1/2, 1/4 or
1/8 size), Direct2D on the GPU, the same decode-ahead pipeline, and no runtime to install (about
580 KB). It has the viewer (folder navigation, zoom, pan, drag-to-zoom, borderless full screen,
slideshow, info overlay, a button bar that appears when the mouse nears the bottom, pointer hidden
in full screen when idle), animated GIF playback, multi-page TIFF pages (`[` and `]`), a
JPEGView-style right-click menu, a Settings dialog, GPU adjustments (brightness, contrast,
saturation, gamma, sharpen, colour balance, auto correction) that can be saved per picture (Save
parameters to DB), crop (Ctrl+drag, Enter), Save processed image (Ctrl+S), lossless JPEG rotation,
Print (Ctrl+P), Paste (Ctrl+V), Copy original size (Ctrl+X), Set modification date, and PDF reading
with magazine spreads, right to left, hold to zoom, zoom kept across page turns and
password-protected files, and video playback through libmpv (mpv draws into its own GPU
window; Glance draws the controls over it). Animated WebP shows its first frame only. PDFs are rendered
by PDFium (Chrome's engine, `pdfium.dll` from bblanchon/pdfium-binaries build 8076, pinned by
hash), which is loaded only when the first PDF opens. On a 132-page magazine it renders the
heaviest pages about 3× faster than Windows' built-in PDF engine, and re-renders open pages
(zoom, hold to zoom) in well under 0.1 s.

`windows/thumbnails/` builds `GlanceThumbnails.dll`, an Explorer thumbnail handler
(IThumbnailProvider) that renders a PDF's first page with PDFium: typically about 30 ms per file
the first time, then Explorer's cache.

Settings are in a Settings dialog (right-click ▸ Settings…) and saved to
`Glance.ini` beside `Glance.exe`, so copying the folder takes your settings along; adjustments
saved per picture go to `Parameters.ini` and the page you were on in each PDF to `Pages.ini`, in
the same folder. If Glance's folder can't be written to (under Program Files, say), they go to
`%APPDATA%\Glance` instead.

| Key | Action |
|---|---|
| Space, Page Down, → / ↓ | Next image or PDF page (arrows pan instead when zoomed in) |
| Backspace, Page Up, ← / ↑ | Previous image or page |
| Home / End | First / last image |
| Mouse wheel | Next / previous image (Ctrl+wheel zooms; or set the wheel to zoom in Settings) |
| ] / [ | Next / previous page of a multi-page TIFF or PDF |
| + / − | Zoom in / out |
| 0, 1 | Fit, 100% |
| Double-click | Toggle fit / 100% at the cursor |
| Drag | Pan when zoomed in; draw a rectangle to zoom into when the image fits |
| Ctrl+drag | Select a crop area. Enter crops |
| R / L | Rotate right / left |
| E | Adjustments |
| F5 / F4 | Auto correction / keep adjustments for next images |
| F2 / Ctrl+F2 | Info overlay / file name |
| F, F11, Alt+Enter | Full screen. Esc leaves it, or closes Glance if Settings says so (JPEGView style) |
| S | Slideshow |
| Ctrl+O / Ctrl+S | Open / Save As |
| Ctrl+C / Ctrl+X / Ctrl+Shift+C | Copy image / copy at original size / copy file path |
| Ctrl+V | Paste (opens copied files or a copied picture) |
| Ctrl+P | Print |
| Ctrl+R | Reload |
| Ctrl+Page Down / Ctrl+Page Up | Next / previous file, even from the middle of a PDF |
| Ctrl+N | Button bar on/off |
| W | Show in Explorer |
| Delete | Move to Recycle Bin |
| Right-click | Menu with every command |

It builds on GitHub's Windows machines (the **Build Windows** workflow) with MSVC, or locally with
`windows/build.sh` (MinGW) or CMake. The app icon for both platforms is drawn by
`scripts/make-icons.ps1`, which writes `Resources/Glance.icns` and `windows/glance.ico`.

Measured against JPEGView 1.4 on an i5-14500T with an RTX 4070, both windowed at 1600×1000,
with 12 and 24 MP JPEGs and 12 MP PNGs (`windows/tools/ui_bench.py`, which watches the screen):

| | Glance | JPEGView |
|---|---|---|
| Launch to photo on screen | **~100 ms** | ~225 ms |
| Next image, normal browsing | **~10 ms** | ~28 ms |
| Next image, key held down | **~20 ms** | ~130 ms |

## Layout

```
Sources/Glance/
  main.swift, AppDelegate.swift   launch, Finder "Open With", command-line paths
  MainMenu.swift                   menu bar
  ViewerWindowController.swift     glue: navigation, keys, slideshow, edits, saving
  Settings.swift                   preferences (UserDefaults) with change notifications
  SettingsWindowController.swift   the Settings window
  ImageView.swift                  Core Animation display: zoom, pan, rotate, tiles, selection
  ImageDecoder.swift               ImageIO decoding and metadata
  ImagePipeline.swift              background decoding, coalescing, LRU cache, prefetch
  FolderNavigator.swift            folder listing and sorting
  Adjustments.swift                Core Image adjustment and edit rendering
  ImageSaver.swift                 lossless orientation rewrite and re-encoding
  AdjustmentsPanel.swift           slider panel
  ControlBar.swift                 hover button strip
  InfoFormatter.swift              EXIF overlay text
  MPV.swift, VideoView.swift       libmpv loading; video playback, controls, timeline pictures

windows/src/
  main.cpp, Viewer.cpp             window, drawing (Direct2D), input, menu, commands
  Decoder.cpp, Pdf.cpp             WIC decoding and metadata; PDFium pages and spreads
  Pipeline.cpp, Navigator.cpp      background decoding, cache, prefetch; folder listing
  Adjustments.cpp, Editing.cpp     adjustments, crop, saving, lossless rotation
  Settings.cpp, SettingsDialog.cpp preferences (INI) and the Settings dialog
  AdjustmentsDialog.cpp, PasswordDialog.cpp, ParameterStore.cpp
  Video.cpp                        video playback through libmpv: controls, timeline pictures
  FileTypes.cpp                    file types and their icons, for Explorer and Default apps
windows/thumbnails/                GlanceThumbnails.dll: PDF covers in Explorer
```

## Licence

Glance is free software under the [GNU General Public License, version 3](LICENSE): you can use,
share and change it, and versions you share must stay under the same licence with their source
available. The Windows download includes PDFium (BSD) and libmpv (GPL); the Mac app includes
libmpv and FFmpeg (GPL and LGPL);
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) lists them and where their source is.
