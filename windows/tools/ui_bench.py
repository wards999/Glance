"""Times image viewers the way you experience them: by watching the screen.

For each viewer: launch it on the first image and time until the photo is visible; then put
both windows at the same size and time "next image" key presses until the picture changes.
Keys are posted to the viewer's own window, so nothing goes to other apps.

    python ui_bench.py <folder> <viewer.exe> [<viewer.exe> ...]
"""

import ctypes
import ctypes.wintypes as wt
import os
import statistics
import subprocess
import sys
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32")
# Physical pixels, so window rectangles match what's captured from the screen.
user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))

WM_KEYDOWN, WM_KEYUP, WM_CLOSE = 0x0100, 0x0101, 0x0010
VK_RIGHT = 0x27
SRCCOPY = 0x00CC0020
SAMPLE = 240  # side of the square captured from the middle of the window
WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG), ("biPlanes", wt.WORD),
                ("biBitCount", wt.WORD), ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD),
                ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                ("biClrImportant", wt.DWORD)]


class Screen:
    """Grabs a small square of the screen quickly, reusing the same GDI objects."""

    def __init__(self):
        self.screen = user32.GetDC(0)
        self.dc = gdi32.CreateCompatibleDC(self.screen)
        self.bitmap = gdi32.CreateCompatibleBitmap(self.screen, SAMPLE, SAMPLE)
        gdi32.SelectObject(self.dc, self.bitmap)
        self.info = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), SAMPLE, -SAMPLE, 1, 32, 0, 0, 0, 0, 0, 0)
        self.buffer = ctypes.create_string_buffer(SAMPLE * SAMPLE * 4)

    def grab(self, x, y):
        gdi32.BitBlt(self.dc, 0, 0, SAMPLE, SAMPLE, self.screen, x, y, SRCCOPY)
        gdi32.GetDIBits(self.dc, self.bitmap, 0, SAMPLE, self.buffer, ctypes.byref(self.info), 0)
        return self.buffer.raw


def looks_like_photo(pixels):
    # A blank or single-colour window has few distinct values; a photo has many.
    samples = pixels[::397]
    return len(set(samples)) > 40 and statistics.pstdev(samples) > 12


def window_of(pid, timeout=10):
    deadline = time.perf_counter() + timeout
    while time.perf_counter() < deadline:
        found = []

        @WNDENUMPROC
        def visit(hwnd, _):
            owner = wt.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            if owner.value == pid and user32.IsWindowVisible(hwnd):
                r = wt.RECT()
                user32.GetWindowRect(hwnd, ctypes.byref(r))
                if r.right - r.left > 200 and r.bottom - r.top > 200:
                    found.append(hwnd)
            return True

        user32.EnumWindows(visit, 0)
        if found:
            return found[0]
        time.sleep(0.001)
    return None


def sample_origin(hwnd):
    r = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    p = wt.POINT(r.right // 2 - SAMPLE // 2, r.bottom // 2 - SAMPLE // 2)
    user32.ClientToScreen(hwnd, ctypes.byref(p))
    return p.x, p.y


def wait_until(predicate, timeout=5):
    deadline = time.perf_counter() + timeout
    while time.perf_counter() < deadline:
        if predicate():
            return True
    return False


def bench(viewer, first_image, screen):
    name = os.path.basename(viewer)
    start = time.perf_counter()
    process = subprocess.Popen([viewer, first_image])
    hwnd = window_of(process.pid)
    if not hwnd:
        process.kill()
        return name, None
    user32.SetForegroundWindow(hwnd)
    shown = wait_until(lambda: looks_like_photo(screen.grab(*sample_origin(hwnd))), timeout=10)
    launch_ms = (time.perf_counter() - start) * 1000 if shown else None

    # Same window size for every viewer, then let decode-ahead settle.
    user32.ShowWindow(hwnd, 1)
    user32.MoveWindow(hwnd, 100, 100, 1600, 1000, True)
    time.sleep(1.5)

    def flip():
        x, y = sample_origin(hwnd)
        before = screen.grab(x, y)
        t0 = time.perf_counter()
        user32.PostMessageW(hwnd, WM_KEYDOWN, VK_RIGHT, 0x00000001)
        user32.PostMessageW(hwnd, WM_KEYUP, VK_RIGHT, 0xC0000001)
        changed = wait_until(lambda: screen.grab(x, y) != before, timeout=3)
        return (time.perf_counter() - t0) * 1000 if changed else None

    browsing = []
    for _ in range(10):
        browsing.append(flip())
        time.sleep(0.4)
    held = []
    for _ in range(12):
        held.append(flip())  # next press as soon as the picture changes

    user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
    return name, (launch_ms, browsing, held)


def summary(times):
    good = [t for t in times if t is not None]
    missed = len(times) - len(good)
    if not good:
        return "no change seen"
    text = f"median {statistics.median(good):6.1f} ms, worst {max(good):6.1f} ms"
    return text + (f", {missed} missed" if missed else "")


def launches(viewer, first_image, screen, count):
    """Launch only, several times: the first run can include Windows scanning a new exe."""
    times = []
    for _ in range(count):
        start = time.perf_counter()
        process = subprocess.Popen([viewer, first_image])
        hwnd = window_of(process.pid)
        shown = hwnd and wait_until(lambda: looks_like_photo(screen.grab(*sample_origin(hwnd))), timeout=10)
        times.append((time.perf_counter() - start) * 1000 if shown else None)
        if hwnd:
            user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
        time.sleep(0.5)
    return times


def main():
    if sys.argv[1].startswith("--launches="):
        count = int(sys.argv[1].split("=")[1])
        folder, viewers = sys.argv[2], sys.argv[3:]
        images = sorted(f for f in os.listdir(folder) if f.lower().endswith((".jpg", ".jpeg", ".png")))
        screen = Screen()
        for viewer in viewers:
            times = launches(viewer, os.path.join(folder, images[0]), screen, count)
            shown = ", ".join(f"{t:.0f}" if t else "-" for t in times)
            print(f"{os.path.basename(viewer)} launch to photo (ms): {shown}")
        return
    folder, viewers = sys.argv[1], sys.argv[2:]
    images = sorted(f for f in os.listdir(folder) if f.lower().endswith((".jpg", ".jpeg", ".png")))
    first = os.path.join(folder, images[0])
    screen = Screen()
    print(f"{len(images)} images in {folder}; first: {images[0]}\n")
    for viewer in viewers:
        name, result = bench(viewer, first, screen)
        if result is None:
            print(f"{name}: no window appeared")
            continue
        launch_ms, browsing, held = result
        print(name)
        print(f"  launch to photo on screen: {launch_ms:.0f} ms" if launch_ms else "  launch: photo never detected")
        print(f"  next image, browsing:      {summary(browsing)}")
        print(f"  next image, key held:      {summary(held)}")
        print()
        time.sleep(1)


if __name__ == "__main__":
    main()
