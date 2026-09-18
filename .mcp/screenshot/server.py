#!/usr/bin/env python3
"""screenshot - a local MCP server that lets an agent SEE windows.

An agent can read a stack trace but not a rendered frame, which makes "the app
looks wrong" impossible to act on.  This server closes that gap: its tools
return PNG bytes as MCP `ImageContent` blocks, so a screenshot arrives as part
of the tool result and needs no file round-trip.

Three tools:

    list_windows()                       visible windows: hwnd, pid, process,
                                         title, size
    capture_window(...)                   -> Image, by hwnd / process / title,
                                         or the whole virtual screen
    show_image(path)                      -> Image, any existing image file
                                         (a .ppm is converted on the way)

Design notes
------------
Standalone on purpose.  Nothing here imports from the rest of the repository,
so the file can be copied anywhere and run directly.  That is why it carries
its own PNG encoder and PPM reader instead of reaching into scripts/.

Capture is done in-process with ctypes against Win32, with no PowerShell and no
temporary file: the pixels go screen -> memory -> PNG -> base64 -> MCP.

There is a deliberate sibling in the repository, scripts/capture_window.ps1. It
exists so that someone who clones the project can take a screenshot without
installing an MCP server at all. The two are independent implementations of the
same small idea; neither depends on the other.

The behaviour deliberately mirrors that script, because the script's details
were arrived at by testing rather than by reading documentation:

  * DPI awareness must be set before any rectangle is read, or Windows reports
    logical pixels while the screen holds physical ones and the capture comes
    back cropped (800x600 asked for, 533x400 delivered, at 150% scaling).
  * The client area is cropped to, so the render surface is not framed by
    window chrome.
  * Pixels are blitted from the *screen* device context, not the window's. A
    GPU-composited surface (Vulkan, D3D, a browser's video path) is not
    readable through a window DC and comes back black; the screen DC holds what
    is actually on the monitor, which is also what a human sees.

Windows only, by nature.

Command line (for testing the capture layer without an MCP client):

    python server.py --list
    python server.py --capture-process vulkangears --out shot.png
    python server.py --capture-screen --out screen.png
    python server.py --convert-ppm shot.ppm --out shot.png
    python server.py --selftest
"""

# NOTE: there is deliberately no `from __future__ import annotations` here.
# Postponed annotations turn the tool return types into the *strings* "Image",
# and the SDK recognises its image helper by identity against the real class.
# With a string annotation that check misses, pydantic is asked to build a
# schema for it instead, and registration dies with
# PydanticSchemaGenerationError. The target is 3.10+ (the mcp package requires
# it), where `int | None` and `list[str]` work natively, so nothing here needs
# the future import.

import argparse
import ctypes
import logging
import os
import struct
import sys
import time
import zlib
from ctypes import wintypes
from dataclasses import dataclass
from pathlib import Path

# Imported lazily-by-try so the capture layer stays runnable (and testable) with
# a plain interpreter, before setup.ps1 has built the venv.
try:
    from mcp.server.mcpserver.exceptions import ToolError
except ImportError:  # pragma: no cover - depends on the environment
    # Failures below raise ToolError so that their message reaches the caller.
    # The SDK's tool runner replaces the text of any *other* exception with a
    # generic "Error executing tool X" and keeps the real message server-side,
    # which would discard exactly the diagnostic that makes these tools usable
    # ("no window owned by 'foo'; running: ..."). Without the SDK -- running
    # --selftest on a bare interpreter -- RuntimeError is the honest equivalent:
    # a message and a stack.
    ToolError = RuntimeError  # type: ignore[assignment,misc]


# An MCP stdio server must never write to stdout: that stream carries the
# JSON-RPC protocol, and a stray print() corrupts it and kills the connection.
# Every diagnostic therefore goes to stderr.
log = logging.getLogger("screenshot")


# ---------------------------------------------------------------------------
# Win32 bindings
# ---------------------------------------------------------------------------
# HDC/HBITMAP/HGDIOBJ are all pointer-sized handles. Declaring the restypes
# matters: ctypes defaults to C int, which silently truncates a 64-bit handle to
# its low 32 bits, and the process then crashes somewhere far away when that
# handle is used.

HDC = wintypes.HANDLE
HBITMAP = wintypes.HANDLE
HGDIOBJ = wintypes.HANDLE

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)


class RECT(ctypes.Structure):
    _fields_ = [
        ("left", wintypes.LONG),
        ("top", wintypes.LONG),
        ("right", wintypes.LONG),
        ("bottom", wintypes.LONG),
    ]


class POINT(ctypes.Structure):
    _fields_ = [("x", wintypes.LONG), ("y", wintypes.LONG)]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wintypes.DWORD),
        ("biWidth", wintypes.LONG),
        ("biHeight", wintypes.LONG),
        ("biPlanes", wintypes.WORD),
        ("biBitCount", wintypes.WORD),
        ("biCompression", wintypes.DWORD),
        ("biSizeImage", wintypes.DWORD),
        ("biXPelsPerMeter", wintypes.LONG),
        ("biYPelsPerMeter", wintypes.LONG),
        ("biClrUsed", wintypes.DWORD),
        ("biClrImportant", wintypes.DWORD),
    ]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wintypes.DWORD * 3)]


user32.EnumWindows.argtypes = [WNDENUMPROC, wintypes.LPARAM]
user32.EnumWindows.restype = wintypes.BOOL
user32.IsWindowVisible.argtypes = [wintypes.HWND]
user32.IsWindowVisible.restype = wintypes.BOOL
user32.IsIconic.argtypes = [wintypes.HWND]
user32.IsIconic.restype = wintypes.BOOL
user32.GetWindowTextLengthW.argtypes = [wintypes.HWND]
user32.GetWindowTextLengthW.restype = ctypes.c_int
user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.GetWindowTextW.restype = ctypes.c_int
user32.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(RECT)]
user32.GetClientRect.restype = wintypes.BOOL
user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(RECT)]
user32.GetWindowRect.restype = wintypes.BOOL
user32.ClientToScreen.argtypes = [wintypes.HWND, ctypes.POINTER(POINT)]
user32.ClientToScreen.restype = wintypes.BOOL
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.GetWindowThreadProcessId.restype = wintypes.DWORD
user32.SetForegroundWindow.argtypes = [wintypes.HWND]
user32.SetForegroundWindow.restype = wintypes.BOOL
user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
user32.ShowWindow.restype = wintypes.BOOL
user32.IsWindow.argtypes = [wintypes.HWND]
user32.IsWindow.restype = wintypes.BOOL
user32.GetDC.argtypes = [wintypes.HWND]
user32.GetDC.restype = HDC
user32.ReleaseDC.argtypes = [wintypes.HWND, HDC]
user32.ReleaseDC.restype = ctypes.c_int
user32.GetSystemMetrics.argtypes = [ctypes.c_int]
user32.GetSystemMetrics.restype = ctypes.c_int
user32.PrintWindow.argtypes = [wintypes.HWND, HDC, wintypes.UINT]
user32.PrintWindow.restype = wintypes.BOOL

gdi32.CreateCompatibleDC.argtypes = [HDC]
gdi32.CreateCompatibleDC.restype = HDC
gdi32.CreateCompatibleBitmap.argtypes = [HDC, ctypes.c_int, ctypes.c_int]
gdi32.CreateCompatibleBitmap.restype = HBITMAP
gdi32.SelectObject.argtypes = [HDC, HGDIOBJ]
gdi32.SelectObject.restype = HGDIOBJ
gdi32.DeleteObject.argtypes = [HGDIOBJ]
gdi32.DeleteObject.restype = wintypes.BOOL
gdi32.DeleteDC.argtypes = [HDC]
gdi32.DeleteDC.restype = wintypes.BOOL
gdi32.BitBlt.argtypes = [
    HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
    HDC, ctypes.c_int, ctypes.c_int, wintypes.DWORD,
]
gdi32.BitBlt.restype = wintypes.BOOL
gdi32.GetDIBits.argtypes = [
    HDC, HBITMAP, wintypes.UINT, wintypes.UINT,
    ctypes.c_void_p, ctypes.POINTER(BITMAPINFO), wintypes.UINT,
]
gdi32.GetDIBits.restype = ctypes.c_int

kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel32.OpenProcess.restype = wintypes.HANDLE
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.CloseHandle.restype = wintypes.BOOL
kernel32.QueryFullProcessImageNameW.argtypes = [
    wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)
]
kernel32.QueryFullProcessImageNameW.restype = wintypes.BOOL

SRCCOPY = 0x00CC0020
DIB_RGB_COLORS = 0
BI_RGB = 0
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
SW_RESTORE = 9

# PW_RENDERFULLCONTENT (Windows 8.1+): render the window's own composed
# contents, including surfaces that were handed to the compositor, rather than
# the GDI backing store. Without the flag a GPU-composited window renders blank.
PW_RENDERFULLCONTENT = 0x00000002

SM_XVIRTUALSCREEN = 76
SM_YVIRTUALSCREEN = 77
SM_CXVIRTUALSCREEN = 78
SM_CYVIRTUALSCREEN = 79

# DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 is the pseudo-handle -4.  Wrapped
# as an unsigned value because ctypes has no signed representation for it.
_PER_MONITOR_AWARE_V2 = ctypes.c_void_p(-4 & (2**64 - 1))


def enable_dpi_awareness() -> str:
    """Tell Windows we want real pixels, and report which mode we got.

    Must run before any geometry is read.  Without it GetClientRect reports
    logical pixels while the screen holds physical ones, so a capture of an
    800x600 window at 150% scaling comes back as a cropped 533x400.

    Per-monitor-v2 needs Windows 10 1703+, per-monitor needs 8.1+; the
    system-aware fallback is there so an old machine degrades rather than fails.
    """
    try:
        user32.SetProcessDpiAwarenessContext.argtypes = [wintypes.HANDLE]
        user32.SetProcessDpiAwarenessContext.restype = wintypes.BOOL
        if user32.SetProcessDpiAwarenessContext(_PER_MONITOR_AWARE_V2):
            return "per-monitor-v2"
    except (AttributeError, OSError):
        pass

    try:
        shcore = ctypes.WinDLL("shcore", use_last_error=True)
        shcore.SetProcessDpiAwareness.argtypes = [ctypes.c_int]
        shcore.SetProcessDpiAwareness.restype = ctypes.c_long
        # 2 == PROCESS_PER_MONITOR_DPI_AWARE; returns E_ACCESSDENIED if the
        # awareness was already set, which is not a problem for us.
        if shcore.SetProcessDpiAwareness(2) == 0:
            return "per-monitor"
    except (AttributeError, OSError):
        pass

    try:
        user32.SetProcessDPIAware.argtypes = []
        user32.SetProcessDPIAware.restype = wintypes.BOOL
        if user32.SetProcessDPIAware():
            return "system"
    except (AttributeError, OSError):
        pass

    return "unaware"


# Applied once, at import, before any rectangle can possibly be read. Doing this
# per capture instead invites exactly the bug it prevents: one code path that
# forgets, and quietly returns geometry in logical pixels while the screen holds
# physical ones.  (The first draft of this file had that bug: --capture-screen
# returned 1280x720 for a 1920x1080 screen because only some entry points called
# this.)  Harmless if the process already has an awareness mode set.
_DPI_MODE = enable_dpi_awareness()


# ---------------------------------------------------------------------------
# Windows
# ---------------------------------------------------------------------------


@dataclass
class WindowInfo:
    hwnd: int
    pid: int
    process: str
    title: str
    width: int
    height: int
    minimized: bool

    @property
    def area(self) -> int:
        return self.width * self.height


def process_name(pid: int) -> str:
    """Full image path for a pid, or "" if it cannot be opened.

    A pid can vanish between enumeration and this call, and system processes
    refuse the query; both are normal and simply produce no name.
    """
    handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return ""
    try:
        size = wintypes.DWORD(32768)
        buffer = ctypes.create_unicode_buffer(size.value)
        if kernel32.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)):
            return os.path.basename(buffer.value)
        return ""
    finally:
        kernel32.CloseHandle(handle)


def window_title(hwnd: int) -> str:
    length = user32.GetWindowTextLengthW(hwnd)
    if length <= 0:
        return ""
    buffer = ctypes.create_unicode_buffer(length + 1)
    user32.GetWindowTextW(hwnd, buffer, length + 1)
    return buffer.value


def client_size(hwnd: int) -> tuple[int, int]:
    """Client-area size in physical pixels, falling back to the whole window."""
    rect = RECT()
    if user32.GetClientRect(hwnd, ctypes.byref(rect)):
        width = rect.right - rect.left
        height = rect.bottom - rect.top
        if width > 0 and height > 0:
            return width, height
    if user32.GetWindowRect(hwnd, ctypes.byref(rect)):
        return max(0, rect.right - rect.left), max(0, rect.bottom - rect.top)
    return 0, 0


def client_rect_on_screen(hwnd: int) -> tuple[int, int, int, int]:
    """(x, y, width, height) of the client area, in screen coordinates."""
    rect = RECT()
    origin = POINT()
    if user32.GetClientRect(hwnd, ctypes.byref(rect)) and user32.ClientToScreen(
        hwnd, ctypes.byref(origin)
    ):
        width = rect.right - rect.left
        height = rect.bottom - rect.top
        if width > 0 and height > 0:
            return origin.x, origin.y, width, height

    if not user32.GetWindowRect(hwnd, ctypes.byref(rect)):
        raise ToolError("the window disappeared before it could be measured")
    return rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top


def virtual_screen() -> tuple[int, int, int, int]:
    """(x, y, width, height) covering every monitor."""
    return (
        user32.GetSystemMetrics(SM_XVIRTUALSCREEN),
        user32.GetSystemMetrics(SM_YVIRTUALSCREEN),
        user32.GetSystemMetrics(SM_CXVIRTUALSCREEN),
        user32.GetSystemMetrics(SM_CYVIRTUALSCREEN),
    )


def enumerate_windows() -> list[WindowInfo]:
    """Every visible top-level window that has a title.

    Titles are the filter because a window without one is a hidden helper
    surface that a user would not recognise in a list.
    """
    found: list[WindowInfo] = []

    def visit(hwnd: int, _param: int) -> bool:
        if not user32.IsWindowVisible(hwnd):
            return True
        title = window_title(hwnd)
        if not title:
            return True

        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        width, height = client_size(hwnd)
        found.append(
            WindowInfo(
                hwnd=int(hwnd),
                pid=int(pid.value),
                process=process_name(int(pid.value)),
                title=title,
                width=width,
                height=height,
                minimized=bool(user32.IsIconic(hwnd)),
            )
        )
        return True

    user32.EnumWindows(WNDENUMPROC(visit), 0)
    return found


def find_window(process: str = "", title: str = "") -> WindowInfo:
    """Locate a single window by process name or a fragment of its title.

    Process matching is the reliable one: it keys off the executable, whereas a
    title search hits any window that merely mentions the string -- searching
    for a project called "vulkangears" also matches the editor that has that
    folder open.  Prefer passing an hwnd from list_windows() and skip both.

    When a process owns several windows the largest is chosen, since the
    interesting one is the one with a rendering surface in it.
    """
    windows = enumerate_windows()

    if process:
        wanted = process.lower()
        if not wanted.endswith(".exe"):
            wanted += ".exe"
        matches = [w for w in windows if w.process.lower() == wanted]
        if not matches:
            # Accept a bare stem too, so "chrome" finds "chrome.exe".
            stem = wanted[:-4]
            matches = [w for w in windows if w.process.lower().removesuffix(".exe") == stem]
        if not matches:
            known = sorted({w.process for w in windows if w.process})
            raise ToolError(
                f"no visible window owned by {process!r}. "
                f"Running processes with windows: {', '.join(known) or 'none'}"
            )
        return max(matches, key=lambda w: w.area)

    if title:
        wanted = title.lower()
        matches = [w for w in windows if wanted in w.title.lower()]
        if not matches:
            available = sorted({w.title for w in windows})[:15]
            raise ToolError(
                f"no visible window whose title contains {title!r}. "
                f"Titles: {' | '.join(available) or 'none'}"
            )
        return max(matches, key=lambda w: w.area)

    raise ToolError("find_window needs a process or a title")


def window_by_hwnd(hwnd: int) -> WindowInfo:
    """Validate a caller-supplied hwnd and describe it."""
    if not user32.IsWindow(hwnd):
        raise ToolError(f"hwnd {hwnd} is not a window (it may have been closed)")
    pid = wintypes.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    width, height = client_size(hwnd)
    return WindowInfo(
        hwnd=hwnd,
        pid=int(pid.value),
        process=process_name(int(pid.value)),
        title=window_title(hwnd),
        width=width,
        height=height,
        minimized=bool(user32.IsIconic(hwnd)),
    )


def bring_to_front(hwnd: int, settle: float = 0.25) -> bool:
    """Un-minimise and raise a window so it is not captured occluded.

    Only needed on the fallback path now: the primary capture asks the window to
    render itself, which works while something else is on top of it. Reaching
    this function at all means the window refused to render, and we are about to
    read the screen instead -- which does require the window to be visible.

    Returns False (rather than raising) when the window cannot be raised, which
    happens legitimately for another user's window or under the foreground lock.
    The capture still proceeds; it just may contain whatever is on top.

    Note the side effect: raising someone else's window changes what the user is
    looking at.
    """
    if user32.IsIconic(hwnd):
        user32.ShowWindow(hwnd, SW_RESTORE)
    raised = bool(user32.SetForegroundWindow(hwnd))
    time.sleep(settle)
    return raised


# ---------------------------------------------------------------------------
# Pixels
# ---------------------------------------------------------------------------


def _dib_to_rgb(memory_dc: int, bitmap: int, width: int, height: int) -> bytes:
    """Read a selected bitmap out of a memory DC as packed RGB."""
    info = BITMAPINFO()
    info.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    info.bmiHeader.biWidth = width
    # Negative height asks for top-down rows, matching PNG's order; a positive
    # height would hand back a vertically mirrored image.
    info.bmiHeader.biHeight = -height
    info.bmiHeader.biPlanes = 1
    info.bmiHeader.biBitCount = 32
    info.bmiHeader.biCompression = BI_RGB

    buffer = ctypes.create_string_buffer(width * height * 4)
    if not gdi32.GetDIBits(
        memory_dc, bitmap, 0, height,
        ctypes.cast(buffer, ctypes.c_void_p), ctypes.byref(info), DIB_RGB_COLORS,
    ):
        raise ToolError("GetDIBits failed")

    # BGRA in, RGB out. Slicing with a stride keeps this in C rather than a
    # per-pixel Python loop.
    bgra = buffer.raw
    rgb = bytearray(width * height * 3)
    rgb[0::3] = bgra[2::4]
    rgb[1::3] = bgra[1::4]
    rgb[2::3] = bgra[0::4]
    return bytes(rgb)


def grab_rgb(x: int, y: int, width: int, height: int) -> bytes:
    """Copy a screen rectangle into packed RGB bytes.

    Reads from the screen device context rather than the window's, so a
    GPU-composited window is captured as the compositor presented it. This sees
    whatever is physically on the monitor though, so it requires the target to
    be visible -- see render_window_rgb for the occlusion-proof route.
    """
    if width <= 0 or height <= 0:
        raise ToolError(f"refusing to capture a {width}x{height} region")

    screen_dc = user32.GetDC(None)
    if not screen_dc:
        raise ToolError("GetDC(NULL) failed: no screen device context")
    memory_dc = None
    bitmap = None
    try:
        memory_dc = gdi32.CreateCompatibleDC(screen_dc)
        if not memory_dc:
            raise ToolError("CreateCompatibleDC failed")
        bitmap = gdi32.CreateCompatibleBitmap(screen_dc, width, height)
        if not bitmap:
            raise ToolError(f"CreateCompatibleBitmap failed for {width}x{height}")

        previous = gdi32.SelectObject(memory_dc, bitmap)
        try:
            if not gdi32.BitBlt(
                memory_dc, 0, 0, width, height, screen_dc, x, y, SRCCOPY
            ):
                raise ToolError(f"BitBlt failed for the region at {x},{y}")
        finally:
            gdi32.SelectObject(memory_dc, previous)

        return _dib_to_rgb(memory_dc, bitmap, width, height)
    finally:
        if bitmap:
            gdi32.DeleteObject(bitmap)
        if memory_dc:
            gdi32.DeleteDC(memory_dc)
        user32.ReleaseDC(None, screen_dc)


def print_window_rgb(hwnd: int, width: int, height: int) -> bytes | None:
    """Have a window draw itself into a bitmap. None if it refuses.

    This is the occlusion-proof route, and the reason it is preferred: asking
    the window to render needs no focus, so nothing has to be raised. It returns
    None (rather than raising) when PrintWindow itself fails, so the caller can
    fall back.
    """
    screen_dc = user32.GetDC(None)
    if not screen_dc:
        return None
    memory_dc = None
    bitmap = None
    try:
        memory_dc = gdi32.CreateCompatibleDC(screen_dc)
        if not memory_dc:
            return None
        bitmap = gdi32.CreateCompatibleBitmap(screen_dc, width, height)
        if not bitmap:
            return None

        previous = gdi32.SelectObject(memory_dc, bitmap)
        try:
            if not user32.PrintWindow(hwnd, memory_dc, PW_RENDERFULLCONTENT):
                return None
        finally:
            gdi32.SelectObject(memory_dc, previous)

        return _dib_to_rgb(memory_dc, bitmap, width, height)
    finally:
        if bitmap:
            gdi32.DeleteObject(bitmap)
        if memory_dc:
            gdi32.DeleteDC(memory_dc)
        user32.ReleaseDC(None, screen_dc)


def is_uniform(rgb: bytes, tolerance: int = 8) -> bool:
    """True when every sampled pixel is (near enough) the same colour.

    PrintWindow's failure mode on a window it cannot render is a uniformly
    black image rather than an error, so this is what distinguishes "the window
    had nothing to say" from "the window is genuinely dark". Sampling rather
    than scanning keeps it cheap; a real window varies immediately.
    """
    if len(rgb) < 6:
        return True
    first = rgb[0], rgb[1], rgb[2]
    stride = max(3, (len(rgb) // 3 // 4096) * 3)
    for i in range(0, len(rgb) - 2, stride):
        if (
            abs(rgb[i] - first[0]) > tolerance
            or abs(rgb[i + 1] - first[1]) > tolerance
            or abs(rgb[i + 2] - first[2]) > tolerance
        ):
            return False
    return True


def crop_rgb(rgb: bytes, source_width: int, x: int, y: int, width: int, height: int) -> bytes:
    """Cut a rectangle out of packed RGB rows."""
    out = bytearray(width * height * 3)
    for row in range(height):
        start = ((y + row) * source_width + x) * 3
        out[row * width * 3:(row + 1) * width * 3] = rgb[start:start + width * 3]
    return bytes(out)


def _row_is_blank(rgb: bytes, width: int, row: int) -> bool:
    """True when an entire row of packed RGB is black."""
    start = row * width * 3
    return not any(rgb[start:start + width * 3])


def _column_is_blank(rgb: bytes, width: int, column: int) -> bool:
    """True when an entire column of packed RGB is black.

    Strided slices rather than an index loop, so the work happens in C. All
    three channels are checked: a lone green column has a red byte of 0.
    """
    offset = column * 3
    stride = width * 3
    return not (
        any(rgb[offset::stride])
        or any(rgb[offset + 1::stride])
        or any(rgb[offset + 2::stride])
    )


def encode_png(width: int, height: int, rgb: bytes) -> bytes:
    """Encode packed RGB bytes as a PNG.

    A minimal writer on purpose: PNG's core format is a signature plus
    length-prefixed CRC'd chunks, and zlib and crc32 are both in the standard
    library. That keeps this file free of third-party imaging dependencies.
    """
    if len(rgb) != width * height * 3:
        raise ValueError(
            f"pixel buffer is {len(rgb)} bytes, expected {width * height * 3}"
        )

    # Each scanline carries a filter-type byte. Filter 0 (None) costs a little
    # size but keeps the encoder honest and predictable.
    stride = width * 3
    raw = bytearray()
    for row in range(height):
        raw.append(0)
        raw += rgb[row * stride:(row + 1) * stride]

    def chunk(tag: bytes, payload: bytes) -> bytes:
        body = tag + payload
        return (
            struct.pack(">I", len(payload))
            + body
            + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
        )

    # 8-bit depth, colour type 2 (truecolour RGB), no interlace.
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(raw), 6))
        + chunk(b"IEND", b"")
    )


def render_window_rgb(hwnd: int) -> tuple[int, int, int, int, bytes] | None:
    """Ask a window to draw itself.

    Returns (left, top, width, height, rgb) covering the WHOLE window -- the
    caller crops, because only it knows where the client area sits inside it.
    None when the window will not render.
    """
    rect = RECT()
    if not user32.GetWindowRect(hwnd, ctypes.byref(rect)):
        return None
    width = rect.right - rect.left
    height = rect.bottom - rect.top
    if width <= 0 or height <= 0:
        return None

    rgb = print_window_rgb(hwnd, width, height)
    if rgb is None or is_uniform(rgb):
        return None
    return rect.left, rect.top, width, height, rgb


def capture_window_client(
    hwnd: int,
    client_x: int,
    client_y: int,
    client_w: int,
    client_h: int,
    allow_raise: bool = True,
) -> tuple[bytes, str]:
    """Capture a window's client area, and say which route produced it.

    The window is asked to render itself first. That works while the window is
    occluded -- which matters more than it sounds: capturing from the screen and
    hoping the target is on top is how a request for one window silently returns
    a picture of a different one. Ask for a terminal that is behind the editor
    and you get a screenshot of the editor, with nothing to indicate it.

    Only if the window refuses to render do we read the screen, and that route
    has to raise the window to be meaningful.

    "Refuses" covers two cases. One is a window that will not render at all,
    which shows up as a uniform image. The other is a DPI-*unaware* window, which
    renders at its own logical size: ask for a bitmap the size of the physical
    window and the content arrives shrunk into a corner, the rest black. That is
    not a refusal, it is a plausible-looking image with most of the window
    missing, so it is worth catching explicitly.
    """
    rendered = render_window_rgb(hwnd)
    if rendered is not None:
        left, top, full_w, full_h, rgb = rendered
        # The client area's offset inside the rendered window, which is what
        # strips the title bar and border off the result.
        offset_x = client_x - left
        offset_y = client_y - top
        if (
            offset_x >= 0
            and offset_y >= 0
            and offset_x + client_w <= full_w
            and offset_y + client_h <= full_h
        ):
            cropped = crop_rgb(rgb, full_w, offset_x, offset_y, client_w, client_h)
            # A real window does not have a totally black bottom edge and right
            # edge at the same time, so both being blank means the render did
            # not reach the size we are cropping at.
            if not (
                _row_is_blank(cropped, client_w, client_h - 1)
                and _column_is_blank(cropped, client_w, client_w - 1)
            ):
                return (
                    encode_png(client_w, client_h, cropped),
                    "rendered by the window (works even while occluded)",
                )
            log.debug(
                "the window rendered only part of its frame (%dx%d asked for); "
                "falling back to reading the screen",
                client_w,
                client_h,
            )

    if allow_raise:
        bring_to_front(hwnd)
    return (
        capture_region(client_x, client_y, client_w, client_h),
        "read from the screen (the window would not render itself)",
    )


def capture_region(x: int, y: int, width: int, height: int) -> bytes:
    """Screen rectangle -> PNG bytes."""
    return encode_png(width, height, grab_rgb(x, y, width, height))


# ---------------------------------------------------------------------------
# PPM input
# ---------------------------------------------------------------------------
# vulkangears --headless writes a PPM, which nothing else on a stock Windows box
# will open, so being able to read one is what makes that render viewable.


def _ppm_token(data: bytes, pos: int) -> tuple[bytes, int]:
    """Next whitespace-delimited token, skipping '#' comments."""
    while pos < len(data):
        char = data[pos:pos + 1]
        if char in b" \t\r\n":
            pos += 1
        elif char == b"#":
            while pos < len(data) and data[pos:pos + 1] not in b"\r\n":
                pos += 1
        else:
            break
    start = pos
    while pos < len(data) and data[pos:pos + 1] not in b" \t\r\n":
        pos += 1
    return data[start:pos], pos


def decode_ppm(data: bytes) -> tuple[int, int, bytes]:
    """Parse a binary (P6) or ASCII (P3) PPM into (width, height, RGB)."""
    magic, pos = _ppm_token(data, 0)
    if magic not in (b"P6", b"P3"):
        raise ToolError(f"not a PPM file (expected P6 or P3, found {magic!r})")

    raw_width, pos = _ppm_token(data, pos)
    raw_height, pos = _ppm_token(data, pos)
    raw_max, pos = _ppm_token(data, pos)
    width, height, maxval = int(raw_width), int(raw_height), int(raw_max)
    if maxval <= 0 or maxval > 255:
        raise ToolError(f"only 8-bit PPM is supported (maxval={maxval})")

    if magic == b"P6":
        pos += 1  # exactly one whitespace byte separates header from pixels
        expected = width * height * 3
        pixels = data[pos:pos + expected]
        if len(pixels) != expected:
            raise ToolError(
                f"truncated PPM: {len(pixels)} bytes of pixels, expected {expected}"
            )
    else:
        values = []
        for _ in range(width * height * 3):
            token, pos = _ppm_token(data, pos)
            values.append(int(token))
        pixels = bytes(min(255, max(0, v * 255 // maxval)) for v in values)

    if maxval != 255 and magic == b"P6":
        pixels = bytes(min(255, v * 255 // maxval) for v in pixels)

    return width, height, pixels


# ---------------------------------------------------------------------------
# Shared helpers
# ---------------------------------------------------------------------------

# The formats the SDK's Image helper can take straight from a path.
_PASSTHROUGH_SUFFIXES = {".png", ".jpg", ".jpeg", ".gif", ".webp"}


def capture_to_png(
    hwnd: int | None = None,
    process: str = "",
    title: str = "",
    raise_window: bool = True,
) -> tuple[bytes, str]:
    """Capture a target and return (png_bytes, description).

    Precedence is hwnd, then process, then title, then the whole virtual screen.
    """
    if hwnd is None and not process and not title:
        x, y, width, height = virtual_screen()
        return (
            capture_region(x, y, width, height),
            f"{width}x{height} from the whole virtual screen",
        )

    if hwnd is None:
        hwnd = find_window(process=process, title=title).hwnd
    info = window_by_hwnd(hwnd)

    # A minimised window has nothing to render and reports a useless rectangle,
    # so it has to come back first. This is the one unavoidable focus change.
    if info.minimized and raise_window:
        user32.ShowWindow(hwnd, SW_RESTORE)
        time.sleep(0.25)

    client_x, client_y, width, height = client_rect_on_screen(hwnd)
    png, method = capture_window_client(
        hwnd, client_x, client_y, width, height, allow_raise=raise_window
    )
    source = (
        f"{info.process or 'unknown process'} "
        f"(hwnd {hwnd} - {info.title or 'untitled'})"
    )
    return png, f"{width}x{height} from {source}, {method}"


def is_capture_available() -> bool:
    return sys.platform == "win32"


# ---------------------------------------------------------------------------
# MCP server
# ---------------------------------------------------------------------------


def build_server():
    """Construct the MCP server and register the tools.

    The tools, and the SDK types their signatures need, are brought into scope
    here rather than at module level so that this file still imports cleanly
    without `mcp` present -- which keeps the capture layer above testable from a
    bare interpreter. It also means the `Image` in the return annotations is the
    real class rather than a module-level stand-in, which both the SDK and the
    type checker need it to be.
    """
    try:
        from mcp.server import MCPServer
        from mcp.server.mcpserver import Image
        from mcp.types import TextContent
    except ImportError as exc:
        raise RuntimeError(
            "the 'mcp' package is not available in this interpreter.\n"
            "Run .mcp/screenshot/setup.ps1 to build the venv, or point the MCP "
            "server configuration at .mcp/screenshot/.venv/Scripts/python.exe"
        ) from exc

    mcp = MCPServer("screenshot")

    @mcp.tool()
    def list_windows() -> str:
        """List the visible desktop windows, so a capture target can be chosen exactly.

        Returns one line per window: hwnd, pid, process, client size and title.
        Call this first and then pass the `hwnd` to capture_window -- it is
        unambiguous, whereas matching on a title fragment can pick the wrong
        window (an editor whose title mentions the name you searched for, for
        example).
        """
        if not is_capture_available():
            raise ToolError("list_windows is Windows-only")

        # Note the distinct internal name: this tool is called list_windows, so
        # a bare list_windows() here would be a recursive call to itself.
        windows = sorted(
            enumerate_windows(), key=lambda w: (w.process.lower(), w.title.lower())
        )
        if not windows:
            return "no visible windows with titles were found"

        lines = [f"{len(windows)} visible window(s):", ""]
        lines.append(f"{'hwnd':>10}  {'pid':>7}  {'size':>11}  process / title")
        lines.append(f"{'-' * 10}  {'-' * 7}  {'-' * 11}  {'-' * 40}")
        for window in windows:
            size = f"{window.width}x{window.height}"
            flag = " [minimized]" if window.minimized else ""
            lines.append(
                f"{window.hwnd:>10}  {window.pid:>7}  {size:>11}  "
                f"{window.process or '?'}{flag}"
            )
            lines.append(f"{'':>10}  {'':>7}  {'':>11}  {window.title}")
        return "\n".join(lines)

    @mcp.tool()
    def capture_window(
        hwnd: int | None = None,
        process: str = "",
        title: str = "",
        save_to: str = "",
    ) -> list[TextContent | Image]:
        """Screenshot a window and return the image itself.

        Pick the target with exactly one of:
          * hwnd    - from list_windows(); exact, and the recommended way
          * process - executable name, e.g. "vulkangears" or "vulkangears.exe"
          * title   - a fragment of the window title (least precise)
        With none of them the entire virtual screen is captured, which is the
        right choice when no single window is wanted: showing overall desktop
        layout, or a window that spans monitors.

        The window's client area is captured, so no title bar or border is
        included. The window is asked to render itself, which works even when it
        is behind other windows, and normally does not disturb focus.

        Returns the image plus a line of text naming the source and the route
        used. The route matters: if it says the pixels were read from the screen,
        the window would not render itself and the result may show whatever was
        covering it, so the image is worth a second look.

        Args:
            hwnd: window handle from list_windows(); the exact way to target
            process: executable name, matched case-insensitively
            title: fragment of the window title, matched case-insensitively
            save_to: optional path to also write the PNG to
        """
        if not is_capture_available():
            raise ToolError("capture_window is Windows-only")

        png, description = capture_to_png(hwnd=hwnd, process=process, title=title)

        if save_to:
            destination = Path(save_to).expanduser()
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(png)
            log.info("wrote %s (%s)", destination, description)
        else:
            log.info("captured %s", description)

        return [
            TextContent(type="text", text=f"Captured {description}."),
            Image(data=png, format="png"),
        ]

    @mcp.tool()
    def show_image(path: str) -> Image:
        """Return an existing image file, so it can be looked at.

        Any of .png, .jpg, .jpeg, .gif or .webp is passed to the client as-is.
        A .ppm is converted to PNG on the way, which is what makes the output of
        `vulkangears --headless --out shot.ppm` visible: PPM is a format almost
        nothing on Windows will open.

        Args:
            path: path to the image file
        """
        source = Path(path).expanduser()
        if not source.is_file():
            raise ToolError(f"no such file: {source}")

        suffix = source.suffix.lower()
        if suffix in _PASSTHROUGH_SUFFIXES:
            return Image(path=source)

        if suffix == ".ppm":
            width, height, rgb = decode_ppm(source.read_bytes())
            log.info("converted %s (%dx%d) from PPM", source, width, height)
            return Image(data=encode_png(width, height, rgb), format="png")

        raise ToolError(
            f"unsupported image type {suffix!r}: expected one of "
            f"{', '.join(sorted(_PASSTHROUGH_SUFFIXES | {'.ppm'}))}"
        )

    return mcp


# ---------------------------------------------------------------------------
# Command line
# ---------------------------------------------------------------------------


def _describe(window: WindowInfo) -> str:
    flag = " [minimized]" if window.minimized else ""
    return (
        f"{window.hwnd:>10}  {window.pid:>7}  {window.width}x{window.height:<7}  "
        f"{window.process or '?'}{flag}  {window.title}"
    )


def _run_selftest() -> int:
    """Exercise the capture layer directly, with no MCP client involved."""
    print(f"python      : {sys.executable}")
    print(f"dpi mode    : {_DPI_MODE}")
    print(f"virtual scr : {virtual_screen()}")
    print()

    windows = enumerate_windows()
    print(f"{len(windows)} visible window(s) with titles:")
    for window in sorted(windows, key=lambda w: (w.process.lower(), w.title.lower())):
        print("  " + _describe(window))

    if not windows:
        print("\nno windows to capture; the screen grab below is still valid")

    print()
    png, description = capture_to_png()
    print(f"full screen : {description}, {len(png)} bytes of PNG")
    if png[:8] != b"\x89PNG\r\n\x1a\n":
        print("FAIL: the captured bytes are not a PNG")
        return 1
    print("OK: capture layer is working")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="server.py",
        description="MCP server that lets an agent see windows. With no arguments "
                    "it serves MCP over stdio.",
    )
    parser.add_argument("--list", action="store_true", help="list visible windows and exit")
    parser.add_argument("--selftest", action="store_true", help="check the capture layer and exit")
    parser.add_argument("--capture-process", metavar="NAME", default="",
                        help="capture the window owned by NAME")
    parser.add_argument("--capture-hwnd", type=int, default=None, metavar="HWND",
                        help="capture a specific window handle")
    parser.add_argument("--capture-screen", action="store_true",
                        help="capture the whole virtual screen")
    parser.add_argument("--convert-ppm", metavar="FILE", default="",
                        help="convert a PPM to PNG")
    parser.add_argument("--out", metavar="FILE", default="", help="output path for --out")
    parser.add_argument("--verbose", action="store_true", help="log to stderr")
    args = parser.parse_args(argv)

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        stream=sys.stderr,
        format="[screenshot] %(levelname)s %(message)s",
    )

    if args.selftest:
        return _run_selftest()

    if args.list:
        for window in sorted(enumerate_windows(), key=lambda w: (w.process.lower(), w.title.lower())):
            print(_describe(window))
        return 0

    if args.convert_ppm:
        source = Path(args.convert_ppm)
        width, height, rgb = decode_ppm(source.read_bytes())
        payload = encode_png(width, height, rgb)
        destination = Path(args.out) if args.out else source.with_suffix(".png")
        destination.write_bytes(payload)
        print(f"{source} -> {destination} ({width}x{height})")
        return 0

    if args.capture_process or args.capture_screen or args.capture_hwnd is not None:
        png, description = capture_to_png(
            hwnd=args.capture_hwnd,
            process=args.capture_process,
            raise_window=args.capture_hwnd is not None or bool(args.capture_process),
        )
        destination = Path(args.out) if args.out else Path("capture.png")
        destination.write_bytes(png)
        print(f"captured {description} -> {destination}")
        return 0

    # Default: serve MCP over stdio. Nothing may be written to stdout from here on.
    # DPI awareness was already applied at import time.
    mcp = build_server()
    log.info("screenshot MCP server starting on stdio")
    mcp.run(transport="stdio")
    return 0


if __name__ == "__main__":
    sys.exit(main())
