import AppKit

// No storyboard or nib: the window and menus are built in code so launch does the minimum work.
let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.run()
