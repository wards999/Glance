# Glance

A native macOS image viewer modeled on JPEGView: minimal UI, keyboard driven, and built to put
pixels on screen as fast as possible, including very large files. It's written in Swift with AppKit,
ImageIO, Core Animation and Core Image, and has no dependencies.

Requires macOS 13 or later and Xcode 15 or later (or the matching Command Line Tools).

## Build and run

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

## Why it's fast

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

## Features compared with JPEGView

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

## Settings

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

## Keyboard

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
540 KB). It has the viewer (folder navigation, zoom, pan, drag-to-zoom, borderless full screen,
slideshow, info overlay, a button bar that appears when the mouse nears the bottom, pointer hidden
in full screen when idle), animated GIF playback, multi-page TIFF pages (`[` and `]`), a
JPEGView-style right-click menu, a Settings dialog, GPU adjustments (brightness, contrast,
saturation, gamma, sharpen, colour balance, auto correction) that can be saved per picture (Save
parameters to DB), crop (Ctrl+drag, Enter), Save processed image (Ctrl+S), lossless JPEG rotation,
Print (Ctrl+P), Paste (Ctrl+V), Copy original size (Ctrl+X), Set modification date, and PDF reading
with magazine spreads, right to left, hold to zoom, zoom kept across page turns and
password-protected files. Animated WebP shows its first frame only. PDFs are rendered
by PDFium (Chrome's engine, `pdfium.dll` from bblanchon/pdfium-binaries build 8076, pinned by
hash), which is loaded only when the first PDF opens. On a 132-page magazine it renders the
heaviest pages about 3× faster than Windows' built-in PDF engine, and re-renders open pages
(zoom, hold to zoom) in well under 0.1 s.

It builds on GitHub's Windows machines (the **Build Windows** workflow) with MSVC, or locally with
`windows/build.sh` (MinGW) or CMake.

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
```
