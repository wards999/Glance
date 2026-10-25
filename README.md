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
| Decodes at **screen size**, not full size. ImageIO's thumbnail path lets JPEG decode directly at 1/2, 1/4 or 1/8 scale, so a 100 MP photo costs about as much as a 10 MP one. | `ImageDecoder.decode` |
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
- Bottom control bar that fades in on hover; full screen with cursor auto-hide
- Move to Trash, Reveal in Finder, Set as Desktop Picture, Copy, drag and drop, mouse back/forward buttons, trackpad swipe between images

Not included yet:

- Side-by-side comparison of two images
- Perspective correction and JPEGView's "local density correction" (auto correction uses Core Image's auto-adjust instead)
- Histogram, lossless JPEG *crop* (crops re-encode), batch rename and copy/move to folder

## Settings

JPEGView keeps most of its options in an INI file. Glance has a **Settings window** instead
(⌘, or Glance ▸ Settings…). Changes apply immediately and are saved automatically (in
UserDefaults). Menu toggles such as Loop or Show Info change the same settings, so the menus
and the window always agree.

| Tab | Options |
|---|---|
| General | Sort order and direction, loop at end of folder, mouse wheel zooms or flips images, slideshow interval, keep adjustments between images |
| Display | Initial zoom (fit / fill / 100%), enlarge small images, background color, info overlay, hover controls, hide cursor in full screen, pixel view above 400% |
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
| Drag, two-finger scroll | Pan |
| Option-drag | Draw a rectangle to zoom into |
| Shift-drag | Select a crop area. Return crops, Esc clears |
| Z, ⌘3 | Zoom to the crop selection |
| R / L, ⌘R / ⌘L | Rotate right / left |
| H / V | Flip horizontal / vertical |
| A | Auto correction on/off |
| E, ⌥⌘C | Adjustments panel |
| I, ⌘I | Info overlay |
| S, ⌥⌘P | Slideshow on/off |
| F, ⌃⌘F | Full screen. Esc leaves it |
| ⌘S / ⇧⌘S | Save / Save As |
| ⌘⌫ | Move to Trash |
| ⇧⌘R | Reveal in Finder |
| ⌘, | Settings |

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
