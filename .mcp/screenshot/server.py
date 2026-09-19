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

Capture is done in-process with ctypes: against Win32 (user32/gdi32) on
Windows, and against Xlib plus the Composite extension on Linux. No PowerShell,
no external screenshot tool, and no temporary file: the pixels go
screen -> memory -> PNG -> base64 -> MCP.

There is a deliberate sibling in the repository, scripts/capture_window.ps1. It
exists so that someone who clones the project can take a screenshot without
installing an MCP server at all, and it is Windows-only; this file is the way to
do it on X11. The two are independent implementations of the same small idea;
neither depends on the other.

The behaviour deliberately mirrors that script, because the script's details
were arrived at by testing rather than by reading documentation:

  * Ask the window to render itself before reaching for the screen. On Windows
    that is PrintWindow with PW_RENDERFULLCONTENT; on X11 it is the off-screen
    pixmap a compositor keeps for every redirected window. Both need no focus,
    work while the window is occluded, and see GPU-composited surfaces, which
    reading the screen does not.
  * Windows: DPI awareness must be set before any rectangle is read, or Windows
    reports logical pixels while the screen holds physical ones and the capture
    comes back cropped (800x600 asked for, 533x400 delivered, at 150% scaling).
  * Windows: the client area is cropped to, so the render surface is not framed
    by window chrome. X11 needs no equivalent -- there a window *is* its client
    area, and the decorations belong to the window manager.
  * Reading the screen is the fallback on both platforms: pixels are blitted
    from the *screen* device context rather than a window's on Windows, because
    a GPU-composited surface (Vulkan, D3D, a browser's video path) is not
    readable through a window DC; on X11 the root window is read instead. Either
    way the fallback sees whatever is physically on top, so the route that
    produced an image is reported next to it.

X11 reaches only what the X server can see, so a Wayland-only session has no
root window to read: run the app under XWayland, or use the compositor's own
screenshot tool.

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

# `ctypes.wintypes` is a pure-Python table of Win32 type aliases: it imports on
# any platform, but the sizes it reports are only correct on Windows (c_long is
# 64 bits elsewhere).  That is harmless, because the structures built from it
# are only ever *read* on Windows, and the X11 backend at the end of this file
# has its own.  The fallback keeps this from being a hard requirement on an
# interpreter build that omits the table altogether.
try:
    from ctypes import wintypes
except ImportError:  # pragma: no cover - depends on the interpreter build
    class _Win32TypeAliases:
        def __getattr__(self, _name):
            return ctypes.c_void_p

    wintypes = _Win32TypeAliases()

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
#
# The handles, the three DLLs and the callback type exist only on Windows, so
# they are bound only there.  Off Windows a stand-in absorbs the declarations
# that follow, which keeps one table of prototypes, in one style, on every
# platform.  Nothing outside Windows ever calls through them: the X11
# implementations at the end of this file are bound in their place.

if sys.platform == "win32":
    HDC = wintypes.HANDLE
    HBITMAP = wintypes.HANDLE
    HGDIOBJ = wintypes.HANDLE

    user32 = ctypes.WinDLL("user32", use_last_error=True)
    gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

    WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
else:
    HDC = HBITMAP = HGDIOBJ = ctypes.c_void_p
    WNDENUMPROC = None

    class _InertLibrary:
        """Swallows Win32 prototype declarations (.argtypes / .restype) off Windows.

        The declarations below are plain attribute assignments, and running them
        against this object is cheaper and clearer than keeping the whole table
        inside an `if`.  Calling through one would be a bug, so it raises rather
        than returning something plausible.
        """

        def __getattr__(self, _name):
            return self

        def __setattr__(self, _name, _value):
            pass

        def __call__(self, *_args, **_kwargs):
            raise ToolError("Win32 is not available on this platform")

    user32 = gdi32 = kernel32 = _InertLibrary()


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
#
# There is no X11 equivalent to set: an X11 window is already told its size in
# pixels, so the backend below has nothing to declare.
_DPI_MODE = enable_dpi_awareness() if sys.platform == "win32" else "n/a (x11)"

# How the two capture routes describe themselves in what the tools return. A
# constant rather than a literal at the one place that produces it, because the
# same two routes exist on both platforms and only the wording of the reason
# differs -- the Windows wording ("would not render itself") is false on X11,
# where the pixels were the compositor's all along.
RENDER_ROUTE = "rendered by the window (works even while occluded)"
SCREEN_ROUTE = "read from the screen (the window would not render itself)"

# Whether a minimised window must be brought back before measuring it. True on
# Windows, where a minimised window has no bitmap to ask for and is parked at
# -32000,-32000 so its rectangle is meaningless. False on X11, where the
# compositor's copy of a hidden window is still readable: restoring first would
# rearrange the user's desktop to capture something that was already available.
RESTORE_MINIMIZED = True


def platform_description() -> str:
    """One line naming the active backend and what it can do, for --selftest."""
    return f"win32 (dpi awareness: {_DPI_MODE})"


def process_key(name: str) -> str:
    """Normalise a process name for comparison.

    Windows compares executable names, so the key is lower-cased and given the
    ".exe" it is always displayed with: "vulkangears" and "Vulkangears.exe" have
    to land on the same window.  X11 rebinds this to the bare name, since there
    is no suffix to add there.
    """
    key = name.lower()
    return key if key.endswith(".exe") else key + ".exe"


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
        wanted = process_key(process)
        matches = [w for w in windows if w.process.lower() == wanted]
        if not matches:
            # Accept a bare stem too, so "chrome" finds "chrome.exe".
            stem = wanted.removesuffix(".exe")
            matches = [w for w in windows if w.process.lower().removesuffix(".exe") == stem]
        if not matches:
            # Last resort, and the reason this is not the first pass: on X11 the
            # executable is often named for its packaging rather than its
            # application -- the browser is firefox-bin, the terminal is
            # gnome-terminal-server -- so "firefox" has to be allowed to find
            # "firefox-bin". Loose enough to be ambiguous, which is why the
            # largest match wins and why an exact key is preferred over this.
            matches = [w for w in windows if stem in w.process.lower()]
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


def restore_window(hwnd: int) -> None:
    """Bring a minimised window back, so it has something to render.

    Split out of capture_to_png so that restoring is one platform decision
    rather than an inline Win32 call in shared code.  The short settle is here
    rather than at the call site because both callers want it, and a restore
    that is not waited for produces a stale rectangle.
    """
    user32.ShowWindow(hwnd, SW_RESTORE)
    time.sleep(0.25)


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

    The same edge check catches a third case, which is the X11 one: a window that
    does not paint its own edges -- transparent, or drawing over something it
    does not own -- comes back with black there. Reading the screen is the better
    answer for those, since what is behind them is what a person would see.
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
                    RENDER_ROUTE,
                )
            log.debug(
                "the bottom row and right column of the window's own pixels are "
                "black (%dx%d asked for); either the render did not reach that "
                "size or the window is transparent there -- falling back to "
                "reading the screen",
                client_w,
                client_h,
            )

    if allow_raise:
        bring_to_front(hwnd)
    # Measured again because the raise may have moved the window: on X11 the one
    # that needed raising is the hidden one, and a hidden window's geometry is
    # where its manager parked it, not where it appears once it is back.
    client_x, client_y, client_w, client_h = client_rect_on_screen(hwnd)
    return (
        capture_region(client_x, client_y, client_w, client_h),
        SCREEN_ROUTE,
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
# X11
# ---------------------------------------------------------------------------
# The same job on the other platform, and deliberately the same two routes: ask
# the window for its own pixels, and read the screen only if it refuses.
#
# This is built on Xlib plus Xcomposite rather than on a subprocess -- xwd,
# gnome-screenshot, import -- because a subprocess cannot do the first route at
# all. Those tools read the screen, so they return whatever is on top, which is
# the failure this design exists to avoid. Asking the compositor for a window's
# off-screen pixmap is the X11 counterpart of PrintWindow, and it has the same
# property of working while the window is occluded.
#
# Everything is behind a library probe, so a machine with no X server gets a
# clear refusal from capture_unavailable_reason() instead of an ImportError at
# start-up.

# The sonames, with the distribution package that provides each, so a missing
# one can be reported as something actionable rather than as a raw OSError.
_X11_LIBRARIES = {
    "libX11.so.6": "libx11-6",
    "libXcomposite.so.1": "libxcomposite1",
}
_X11_REQUIRED = "libX11.so.6"

# Constants the X headers would otherwise supply. The calls that take them are
# positional, so they are named here to keep the call sites readable.
_ALL_PLANES = ctypes.c_ulong(-1).value
_Z_PIXMAP = 2
_ANY_PROPERTY_TYPE = 0
_CLIENT_MESSAGE = 33
_SUBSTRUCTURE_NOTIFY = 1 << 19
_SUBSTRUCTURE_REDIRECT = 1 << 20
_IS_UNMAPPED = 0
_LSB_FIRST = 0
_X_SUCCESS = 0
_PAGER_SOURCE_INDICATION = 2
_CURRENT_TIME = 0


class _XImage(ctypes.Structure):
    """XImage, as far as the fields that are read.

    The masks and bytes_per_line are why this is here at all: they are what turn
    the server's buffer into RGB without assuming anything about the visual.

    Unlike XWindowAttributes below, this one may legitimately stop early: the
    image is allocated by libX11 and only ever read here, never written through a
    pointer of this type, so a declaration shorter than the real structure cannot
    overflow anything.
    """

    _fields_ = [
        ("width", ctypes.c_int),
        ("height", ctypes.c_int),
        ("xoffset", ctypes.c_int),
        ("format", ctypes.c_int),
        ("data", ctypes.c_void_p),
        ("byte_order", ctypes.c_int),
        ("bitmap_unit", ctypes.c_int),
        ("bitmap_bit_order", ctypes.c_int),
        ("bitmap_pad", ctypes.c_int),
        ("depth", ctypes.c_int),
        ("bytes_per_line", ctypes.c_int),
        ("bits_per_pixel", ctypes.c_int),
        ("red_mask", ctypes.c_ulong),
        ("green_mask", ctypes.c_ulong),
        ("blue_mask", ctypes.c_ulong),
        ("obdata", ctypes.c_void_p),
        ("funcs", ctypes.c_void_p),
    ]


class _XWindowAttributes(ctypes.Structure):
    """XWindowAttributes, in full.

    Complete rather than truncated at map_state, which is the only field that is
    read. XGetWindowAttributes is an out-parameter call: the server writes the
    whole structure through the pointer it is given, so a struct that stops early
    is not a saving, it is a buffer overflow. Getting this wrong does not fail
    loudly -- it corrupts whatever the allocator put next, and the first symptom
    is a nonsensical answer from a later, unrelated call.

    The two trailing pointer-sized members are the padding that brings the
    structure back to 8-byte alignment on LP64; without it the size is 4 bytes
    short of what the server writes.
    """

    _fields_ = [
        ("x", ctypes.c_int),
        ("y", ctypes.c_int),
        ("width", ctypes.c_int),
        ("height", ctypes.c_int),
        ("border_width", ctypes.c_int),
        ("depth", ctypes.c_int),
        ("visual", ctypes.c_void_p),
        ("root", ctypes.c_ulong),
        ("klass", ctypes.c_int),
        ("bit_gravity", ctypes.c_int),
        ("win_gravity", ctypes.c_int),
        ("backing_store", ctypes.c_int),
        ("backing_planes", ctypes.c_ulong),
        ("backing_pixel", ctypes.c_ulong),
        ("save_under", ctypes.c_int),
        ("colormap", ctypes.c_ulong),
        ("map_installed", ctypes.c_int),
        ("map_state", ctypes.c_int),
        ("all_event_masks", ctypes.c_long),
        ("your_event_mask", ctypes.c_long),
        ("do_not_propagate_mask", ctypes.c_long),
        ("override_redirect", ctypes.c_int),
        ("screen", ctypes.c_void_p),
    ]


class _XVisual(ctypes.Structure):
    """Visual, as far as the colour masks.

    Read through the pointer in XWindowAttributes' `visual` field, which is the
    only way to learn how a pixel maps to a colour when XGetImage did not say.
    """

    _fields_ = [
        ("ext_data", ctypes.c_void_p),
        ("visualid", ctypes.c_ulong),
        ("klass", ctypes.c_int),
        ("red_mask", ctypes.c_ulong),
        ("green_mask", ctypes.c_ulong),
        ("blue_mask", ctypes.c_ulong),
        ("bits_per_rgb", ctypes.c_int),
        ("map_entries", ctypes.c_int),
    ]


class _XClientMessageEvent(ctypes.Structure):
    """XClientMessageEvent, padded to the size of an XEvent union.

    Field order and widths matter more than usual here: XSendEvent hands this
    structure to the X server as-is, so a missing int shifts every field after it
    and the request silently stops meaning anything. The trailing pad is what
    makes the structure the size of the union the protocol expects (24 longs on
    LP64); a short structure is delivered as a truncated event.
    """

    _fields_ = [
        ("type", ctypes.c_int),
        ("serial", ctypes.c_ulong),
        ("send_event", ctypes.c_int),
        ("display", ctypes.c_void_p),
        ("window", ctypes.c_ulong),
        ("message_type", ctypes.c_ulong),
        ("format", ctypes.c_int),
        ("data", ctypes.c_long * 5),
        ("pad", ctypes.c_long * 24),
    ]


class _XErrorEvent(ctypes.Structure):
    """XErrorEvent, as far as error_code."""

    _fields_ = [
        ("type", ctypes.c_int),
        ("display", ctypes.c_void_p),
        ("resourceid", ctypes.c_ulong),
        ("serial", ctypes.c_ulong),
        ("error_code", ctypes.c_ubyte),
        ("request_code", ctypes.c_ubyte),
        ("minor_code", ctypes.c_ubyte),
    ]


# Set by the error handler below. A one-element list rather than a bare global
# because the handler is called from C, where a `global` statement would read
# oddly; a list is mutable and needs none.
_probe_failed = [False]


def _probe_error_handler(_display, _event) -> int:
    """Record an X protocol error and carry on.

    The default handler prints a message and calls exit(). An X error is not
    exceptional here: this file deliberately asks for the pixels of windows that
    may not be viewable and reads properties that may not exist, so an error is
    the ordinary way for such a request to fail -- and it must not take the MCP
    server down with it.
    """
    _probe_failed[0] = True
    return 0


_X_ERROR_HANDLER = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.POINTER(_XErrorEvent)
)
# Kept referenced at module level: ctypes does not hold a reference to a
# callback, and a collected one leaves the C side jumping into freed memory.
_ERROR_HANDLER_INSTANCE = _X_ERROR_HANDLER(_probe_error_handler)


_x11 = None
_xcomposite = None
_display = None
_x11_problem = "libX11 is not present, so there is no X11 capture backend"

if sys.platform != "win32":
    # libX11 is the requirement. Xcomposite only upgrades the primary route from
    # "unavailable" to "occlusion-proof", so its absence is not fatal.
    try:
        _x11 = ctypes.CDLL(_X11_REQUIRED)
    except OSError:
        _x11 = None
    else:
        try:
            _x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
            _x11.XOpenDisplay.restype = ctypes.c_void_p
            _x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
            _x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
            _x11.XDefaultRootWindow.restype = ctypes.c_ulong
            _x11.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
            _x11.XInternAtom.restype = ctypes.c_ulong
            _x11.XGetWindowProperty.argtypes = [
                ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_long,
                ctypes.c_long, ctypes.c_int, ctypes.c_ulong,
                ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int),
                ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
                ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte)),
            ]
            _x11.XGetWindowProperty.restype = ctypes.c_int
            _x11.XGetWindowAttributes.argtypes = [
                ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(_XWindowAttributes)
            ]
            _x11.XGetWindowAttributes.restype = ctypes.c_int
            _x11.XGetGeometry.argtypes = [
                ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
                ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
                ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint),
                ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint),
            ]
            _x11.XGetGeometry.restype = ctypes.c_int
            _x11.XTranslateCoordinates.argtypes = [
                ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong,
                ctypes.c_int, ctypes.c_int,
                ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
                ctypes.POINTER(ctypes.c_ulong),
            ]
            _x11.XTranslateCoordinates.restype = ctypes.c_int
            _x11.XQueryTree.argtypes = [
                ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
                ctypes.POINTER(ctypes.c_ulong),
                ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)),
                ctypes.POINTER(ctypes.c_uint),
            ]
            _x11.XQueryTree.restype = ctypes.c_int
            _x11.XGetImage.argtypes = [
                ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int,
                ctypes.c_uint, ctypes.c_uint, ctypes.c_ulong, ctypes.c_int,
            ]
            _x11.XGetImage.restype = ctypes.POINTER(_XImage)
            _x11.XDestroyImage.argtypes = [ctypes.POINTER(_XImage)]
            _x11.XDestroyImage.restype = ctypes.c_int
            _x11.XSetErrorHandler.argtypes = [ctypes.c_void_p]
            _x11.XSetErrorHandler.restype = ctypes.c_void_p
            _x11.XSendEvent.argtypes = [
                ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_long,
                ctypes.POINTER(_XClientMessageEvent),
            ]
            _x11.XSendEvent.restype = ctypes.c_int
            _x11.XGetSelectionOwner.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
            _x11.XGetSelectionOwner.restype = ctypes.c_ulong
            _x11.XFreePixmap.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
            _x11.XFree.argtypes = [ctypes.c_void_p]
            _x11.XFlush.argtypes = [ctypes.c_void_p]
        except AttributeError as exc:  # pragma: no cover - unusual libX11 build
            _x11 = None
            _x11_problem = f"libX11 is present but not usable through ctypes: {exc}"
        else:
            _display = _x11.XOpenDisplay(None)
            if not _display:
                _x11 = None
                # XOpenDisplay reads DISPLAY, and only DISPLAY, so an unset one
                # is the usual cause and deserves to be named rather than being
                # reported as a dead server.  A host that starts this process
                # with a curated environment can drop it: the reference stdio
                # client keeps only a handful of variables, and on Linux that
                # leaves the display out.
                _x11_problem = (
                    "no X server is reachable "
                    f"(DISPLAY={os.environ.get('DISPLAY')!r}, "
                    f"XAUTHORITY={'set' if os.environ.get('XAUTHORITY') else 'unset'}). "
                    "If DISPLAY is empty, this process was started without the "
                    "session's environment; run it from the desktop session, or "
                    "set DISPLAY in the server's env."
                )
            else:
                try:
                    _xcomposite = ctypes.CDLL("libXcomposite.so.1")
                    _xcomposite.XCompositeQueryExtension.argtypes = [
                        ctypes.c_void_p, ctypes.POINTER(ctypes.c_int),
                        ctypes.POINTER(ctypes.c_int),
                    ]
                    _xcomposite.XCompositeQueryExtension.restype = ctypes.c_int
                    _xcomposite.XCompositeNameWindowPixmap.argtypes = [
                        ctypes.c_void_p, ctypes.c_ulong
                    ]
                    _xcomposite.XCompositeNameWindowPixmap.restype = ctypes.c_ulong
                    # The library being installed is not the same as the server
                    # offering the extension, so it is asked rather than assumed.
                    # A server without it predates 1.7; capture then falls back to
                    # reading the screen and still works.
                    _event_base = ctypes.c_int()
                    _error_base = ctypes.c_int()
                    if not _xcomposite.XCompositeQueryExtension(
                        _display, ctypes.byref(_event_base), ctypes.byref(_error_base)
                    ):
                        _xcomposite = None
                        log.debug("the X server has no Composite extension")
                except OSError:
                    _xcomposite = None
                    log.debug("libXcomposite.so.1 is not installed")


# Interning an atom is a round trip to the server, and the same dozen names are
# asked for on every enumeration, so they are remembered.
_atom_cache: dict[str, int] = {}


def _atom(name: str) -> int:
    atom = _atom_cache.get(name)
    if atom is None:
        atom = int(_x11.XInternAtom(_display, name.encode("ascii"), 0))
        _atom_cache[name] = atom
    return atom


def _root() -> int:
    return int(_x11.XDefaultRootWindow(_display))


def _property_bytes(
    xid: int, name: str, expected_type: int = _ANY_PROPERTY_TYPE
) -> tuple[int, bytes] | None:
    """(format, value) for a property, or None when it is absent.

    XGetWindowProperty is the most awkward call in Xlib: five out parameters, one
    of which is an allocation the caller must free, and two that exist only to say
    whether the value was truncated. This keeps the parts that are used here -- a
    32-bit cardinal list or an 8-bit string -- and frees the buffer on every path
    out, including the early ones.
    """
    actual_type = ctypes.c_ulong()
    actual_format = ctypes.c_int()
    count = ctypes.c_ulong()
    after = ctypes.c_ulong()
    data = ctypes.POINTER(ctypes.c_ubyte)()
    status = _x11.XGetWindowProperty(
        _display,
        ctypes.c_ulong(xid),
        ctypes.c_ulong(_atom(name)),
        0,  # offset
        4096,  # long_length; anything beyond this is truncated by the server
        False,  # delete: these properties are read, never consumed
        ctypes.c_ulong(expected_type),
        ctypes.byref(actual_type),
        ctypes.byref(actual_format),
        ctypes.byref(count),
        ctypes.byref(after),
        ctypes.byref(data),
    )
    if status != _X_SUCCESS or not data:
        return None
    try:
        if actual_format.value == 8:
            return 8, ctypes.string_at(data, count.value)
        if actual_format.value == 32:
            # Format 32 data arrives as an array of long, not of int: on LP64
            # each item occupies 8 bytes with the value in the low half.
            return 32, ctypes.string_at(data, count.value * ctypes.sizeof(ctypes.c_ulong))
        return None
    finally:
        # Leaking this would be invisible and unbounded: every capture reads
        # several properties per window.
        _x11.XFree(data)


def _cardinals(xid: int, name: str) -> list[int]:
    """A 32-bit cardinal-list property, e.g. _NET_CLIENT_LIST."""
    found = _property_bytes(xid, name)
    if found is None or found[0] != 32:
        return []
    raw = found[1]
    step = ctypes.sizeof(ctypes.c_ulong)
    return [int.from_bytes(raw[i:i + step], sys.byteorder) for i in range(0, len(raw), step)]


def _text(xid: int, name: str) -> str:
    """An 8-bit text property, e.g. _NET_WM_NAME, decoded as UTF-8."""
    found = _property_bytes(xid, name, _atom("UTF8_STRING"))
    if found is None or found[0] != 8:
        return ""
    return found[1].decode("utf-8", errors="replace")


def _map_state(xid: int) -> int:
    attributes = _XWindowAttributes()
    if not _x11.XGetWindowAttributes(_display, ctypes.c_ulong(xid), ctypes.byref(attributes)):
        return _IS_UNMAPPED
    return int(attributes.map_state)


def _is_minimized(xid: int) -> bool:
    """Whether the window manager is currently hiding this window.

    "Minimised" is a window-manager state, not a server one, so it is read from
    _NET_WM_STATE rather than from the window's map state: an iconified window is
    usually still mapped, merely moved out of the way, and map state alone would
    call it visible. Where the manager publishes no state the answer is "not
    minimised", because guessing would mean restoring a window that was never
    hidden.
    """
    return _atom("_NET_WM_STATE_HIDDEN") in _cardinals(xid, "_NET_WM_STATE")


def _tree_parent_and_children(xid: int) -> tuple[int, list[int]]:
    """(parent, children) for a window, or (root, []) if it is gone."""
    root_return = ctypes.c_ulong()
    parent_return = ctypes.c_ulong()
    children = ctypes.POINTER(ctypes.c_ulong)()
    count = ctypes.c_uint()
    if not _x11.XQueryTree(
        _display,
        ctypes.c_ulong(xid),
        ctypes.byref(root_return),
        ctypes.byref(parent_return),
        ctypes.byref(children),
        ctypes.byref(count),
    ):
        return _root(), []
    try:
        return int(parent_return.value) or _root(), [
            int(children[i]) for i in range(count.value)
        ]
    finally:
        if children:
            _x11.XFree(children)


def _parent_of(xid: int) -> int:
    """The window this one sits inside, or the root when it is top-level."""
    return _tree_parent_and_children(xid)[0]


def _query_tree(xid: int) -> list[int]:
    """The window's immediate children."""
    return _tree_parent_and_children(xid)[1]


def _process_name(pid: int) -> str:
    """Executable name for a pid, or "" if it cannot be read.

    /proc/<pid>/exe is a symlink to the binary and is the same thing the Win32
    side reports, so a process name means the same on both platforms. A pid that
    has already exited is normal -- enumeration and this call are not atomic --
    and simply produces no name, matching the tolerance on the Windows side.
    """
    if pid <= 0:
        return ""
    try:
        return os.path.basename(os.readlink(f"/proc/{pid}/exe"))
    except OSError:
        pass
    try:
        # Only the first 15 characters, and empty for kernel threads, so this is
        # the fallback rather than the first choice.
        return Path(f"/proc/{pid}/comm").read_text(errors="replace").strip()
    except OSError:
        return ""


def _x11_geometry(xid: int) -> tuple[int, int, int, int, int] | None:
    """(x, y, width, height, border_width) of a window, relative to its parent.

    Every out-parameter is backed by a real variable rather than by NULL. Xlib's
    documentation says NULL is allowed for a value the caller does not want, but
    this call writes through its pointers unconditionally, so NULL segfaults
    instead of returning -- which is worth the five unused locals.
    """
    root_return = ctypes.c_ulong()
    x = ctypes.c_int()
    y = ctypes.c_int()
    width = ctypes.c_uint()
    height = ctypes.c_uint()
    border = ctypes.c_uint()
    depth = ctypes.c_uint()
    if not _x11.XGetGeometry(
        _display,
        ctypes.c_ulong(xid),
        ctypes.byref(root_return),
        ctypes.byref(x),
        ctypes.byref(y),
        ctypes.byref(width),
        ctypes.byref(height),
        ctypes.byref(border),
        ctypes.byref(depth),
    ):
        return None
    return (
        int(x.value),
        int(y.value),
        int(width.value),
        int(height.value),
        int(border.value),
    )


def _x11_size(xid: int) -> tuple[int, int]:
    """The window's drawable size in pixels.

    X has no client/non-client split to measure, so this is the whole window: the
    decorations around a managed window belong to a separate window the manager
    owns, so they are not part of this geometry in the first place.
    """
    geometry = _x11_geometry(xid)
    if geometry is None:
        return 0, 0
    return geometry[2], geometry[3]


def _x11_window_info(xid: int) -> WindowInfo | None:
    """Describe a window, or None when it is not one a user would recognise.

    The filter is the same intent as the Windows side's: mapped, and carrying a
    title. An unmapped window is not on screen to look at; a window without a
    title is a helper surface -- a filter, a drag proxy, an offscreen render
    target -- which only adds noise to a list someone is choosing from.
    """
    if _map_state(xid) == _IS_UNMAPPED:
        return None
    title = _text(xid, "_NET_WM_NAME")
    if not title:
        return None
    pid_values = _cardinals(xid, "_NET_WM_PID")
    pid = pid_values[0] if pid_values else 0
    width, height = _x11_size(xid)
    return WindowInfo(
        hwnd=int(xid),
        pid=pid,
        process=_process_name(pid),
        title=title,
        width=width,
        height=height,
        minimized=_is_minimized(xid),
    )


def _x11_enumerate_windows() -> list[WindowInfo]:
    """Every window a user could point at.

    _NET_CLIENT_LIST is what a task bar reads: the window manager's own list of
    managed clients. It is preferred over walking the tree because how deep a
    client sits depends on the manager -- one that reparents for decorations puts
    the client one level below a frame it created, and a naive walk then reports
    the frame, which has no title, and misses the window entirely.
    """
    xids = _cardinals(_root(), "_NET_CLIENT_LIST")
    if not xids:
        # A manager that predates EWMH, or a bare X server with no manager at
        # all. Two levels of tree cover both cases: the client is either a child
        # of the root, or a child of a decoration frame that is.
        xids = _query_tree(_root())
        for parent in list(xids):
            xids.extend(_query_tree(parent))

    infos: list[WindowInfo] = []
    seen: set[int] = set()
    for xid in xids:
        if xid in seen:
            continue
        seen.add(xid)
        info = _x11_window_info(xid)
        if info is not None:
            infos.append(info)
    return infos


def _x11_window_by_hwnd(hwnd: int) -> WindowInfo:
    """Validate a caller-supplied window id and describe it."""
    info = _x11_window_info(hwnd)
    if info is None:
        raise ToolError(
            f"window 0x{hwnd:x} is not a window this can capture (it may have been "
            "closed, or it may have no title); list_windows() shows the ones there are"
        )
    return info


def _x11_client_rect_on_screen(hwnd: int) -> tuple[int, int, int, int]:
    """(x, y, width, height) of a window's own pixels, in root coordinates.

    XGetGeometry reports the position relative to the parent, and
    XTranslateCoordinates converts that into the coordinates captures are read
    at. The border is added back on because X measures a window from the outside
    of its border while the pixels start inside it -- on a window the manager has
    decorated, border_width is normally 0 and this changes nothing.
    """
    geometry = _x11_geometry(hwnd)
    if geometry is None:
        raise ToolError(f"window 0x{hwnd:x} disappeared before it could be measured")
    x, y, width, height, border = geometry

    dest_x = ctypes.c_int()
    dest_y = ctypes.c_int()
    child = ctypes.c_ulong()
    if not _x11.XTranslateCoordinates(
        _display,
        ctypes.c_ulong(_parent_of(hwnd)),
        ctypes.c_ulong(_root()),
        x,
        y,
        ctypes.byref(dest_x),
        ctypes.byref(dest_y),
        ctypes.byref(child),
    ):
        # Both windows have to be viewable on the same screen for this to work,
        # so it fails on an unmapped one.
        raise ToolError(
            f"cannot locate window 0x{hwnd:x} on the screen (it is not viewable)"
        )
    return dest_x.value + border, dest_y.value + border, width, height


def _x11_virtual_screen() -> tuple[int, int, int, int]:
    """(0, 0, width, height) of the root window.

    On X11 the root window *is* the screen: one drawable spanning every monitor,
    so the origin is always (0, 0) and there is no equivalent of the Windows
    virtual-screen metrics to query. Areas a multi-monitor layout leaves without
    a monitor are still part of the root; reading them costs nothing and the
    result is trimmed to its content afterwards, exactly as it is on Windows,
    where the virtual desktop has the same property.
    """
    width, height = _x11_size(_root())
    return 0, 0, width, height


def _x11_call(action):
    """Run one Xlib call with the non-fatal error handler installed.

    Returns (result, failed). Asking the X server for something it will not give
    is an ordinary occurrence here -- the pixels of a window that is not
    viewable, the compositor pixmap of a window nobody redirected, the screen
    rectangle under a cursor sitting on the edge -- and Xlib's default handler
    answers such a refusal by calling exit(). The handler is swapped for the
    duration of the call so that a refusal comes back as False instead of taking
    the MCP server down.

    The XSync is load bearing, not tidiness. Xlib hands back a reply for a
    request before it hands the matching error to the error handler, so straight
    after the call a refused request is indistinguishable from a successful one
    -- and the difference here is between a real pixmap id and a number that only
    looks like one. The round trip drains whatever the server had to say before
    the flag is read.
    """
    _probe_failed[0] = False
    previous = _x11.XSetErrorHandler(
        ctypes.cast(_ERROR_HANDLER_INSTANCE, ctypes.c_void_p)
    )
    try:
        result = action()
        _x11.XSync(_display, False)
        failed = _probe_failed[0]
    finally:
        _x11.XSetErrorHandler(previous)
    return result, failed


def _free_pixmap(pixmap: int) -> None:
    """XFreePixmap, tolerating a pixmap the server has already forgotten.

    Freed through the guarded path because this is the one place where a stale
    id is likely: the id is what it is whether or not the compositor agreed to
    make the pixmap, and an unguarded free of a refused one ends the process.
    """
    _, failed = _x11_call(lambda: _x11.XFreePixmap(_display, ctypes.c_ulong(pixmap)))
    if failed:
        log.debug("the X server had no pixmap 0x%x to free", pixmap)


def _get_image(drawable: int, x: int, y: int, width: int, height: int):
    """XGetImage, or None if the server refuses."""
    if width <= 0 or height <= 0:
        return None
    image, failed = _x11_call(
        lambda: _x11.XGetImage(
            _display,
            ctypes.c_ulong(drawable),
            x,
            y,
            ctypes.c_uint(width),
            ctypes.c_uint(height),
            ctypes.c_ulong(_ALL_PLANES),
            ctypes.c_int(_Z_PIXMAP),
        )
    )
    if not image:
        return None
    if failed:
        # A refusal of the read itself. XDestroyImage is still the right call:
        # XGetImage either hands back an image or NULL, so a non-NULL result is
        # one libX11 allocated and something has to give it back.
        _x11.XDestroyImage(image)
        return None
    return image


def _channel(value: int, mask: int) -> int:
    """One masked channel from a pixel, scaled to 0..255."""
    if not mask:
        return 0
    shift = (mask & -mask).bit_length() - 1
    bits = (mask >> shift).bit_length()
    return ((value & mask) >> shift) * 255 // ((1 << bits) - 1)


_visual_masks_cache: dict[int, tuple[int, int, int]] = {}


def _visual_masks(depth: int) -> tuple[int, int, int]:
    """Colour masks for an image the server sent no masks for.

    XGetImage takes the masks from the drawable's visual. A window has one; a
    pixmap does not, so the server reports zero for all three -- and zero masks
    convert every pixel to black, which is a picture of nothing rather than an
    error. The composite route reads exactly such a pixmap, so this is what keeps
    that route from silently degrading to a black rectangle.

    The pixmap was created by this server for a window of the same depth on the
    same screen, so the root window's visual is the one that describes it. Where
    the depths differ -- a 32-bit compositor pixmap against a 24-bit root -- the
    standard layout for that depth is used instead, which is what the common
    depths (16, 24 and 32) mean everywhere in practice.
    """
    cached = _visual_masks_cache.get(depth)
    if cached is not None:
        return cached

    masks: tuple[int, int, int] | None = None
    attributes = _XWindowAttributes()
    if _x11.XGetWindowAttributes(_display, ctypes.c_ulong(_root()), ctypes.byref(attributes)):
        if attributes.depth == depth and attributes.visual:
            visual = ctypes.cast(
                attributes.visual, ctypes.POINTER(_XVisual)
            ).contents
            if visual.red_mask or visual.green_mask or visual.blue_mask:
                masks = (visual.red_mask, visual.green_mask, visual.blue_mask)
    if masks is None:
        masks = {
            16: (0xF800, 0x07E0, 0x001F),
            24: (0x00FF0000, 0x0000FF00, 0x000000FF),
            32: (0x00FF0000, 0x0000FF00, 0x000000FF),
        }.get(depth)
        if masks is None:
            raise ToolError(
                f"the X server sent an image with no colour masks and this "
                f"display's {depth}-bit format is not a known one, so its pixels "
                f"cannot be read"
            )
        log.debug("no visual for a %d-bit image; assuming masks %#x/%#x/%#x", depth, *masks)
    _visual_masks_cache[depth] = masks
    return masks


def _ximage_to_rgb(image, width: int, height: int) -> bytes:
    """XImage -> tightly packed RGB bytes.

    Colour is taken from the image's own masks rather than assumed, so a visual
    that is not the usual 32-bit BGRA is converted correctly instead of coming
    out with channels swapped. That generality is exactly why the fast path below
    has to check the masks before taking itself.

    An image with no masks at all came from a pixmap rather than a window (see
    _visual_masks); those masks are substituted before anything reads a pixel,
    because a zero mask converts to black rather than failing.
    """
    info = image.contents
    step = info.bits_per_pixel // 8
    stride = info.bytes_per_line
    if step <= 0 or stride < width * step:
        raise ToolError(
            f"the X server described its pixels impossibly "
            f"({info.bits_per_pixel} bits per pixel, {stride} bytes per line, "
            f"{width} pixels wide)"
        )
    if info.red_mask or info.green_mask or info.blue_mask:
        masks = (info.red_mask, info.green_mask, info.blue_mask)
    else:
        masks = _visual_masks(info.depth)
        log.debug("image carried no colour masks; using %#x/%#x/%#x from the visual", *masks)
    red_mask, green_mask, blue_mask = masks
    raw = ctypes.string_at(info.data, stride * height)

    standard = (
        step == 4
        and info.byte_order == _LSB_FIRST
        and stride == width * 4
        and masks == (0x00FF0000, 0x0000FF00, 0x000000FF)
    )
    if standard:
        # The overwhelmingly common case: 32-bit little-endian BGRA. Done with
        # strided slices rather than a per-pixel loop, because the loop costs
        # seconds on a full screen -- the difference between a capture that feels
        # instant and one that feels broken.
        rgb = bytearray(width * height * 3)
        rgb[0::3] = raw[2::4]
        rgb[1::3] = raw[1::4]
        rgb[2::3] = raw[0::4]
        return bytes(rgb)

    log.debug(
        "non-standard XImage (%d bpp, masks %#x/%#x/%#x, %d bytes per line); "
        "converting pixel by pixel",
        info.bits_per_pixel,
        red_mask,
        green_mask,
        blue_mask,
        stride,
    )
    order = "little" if info.byte_order == _LSB_FIRST else "big"
    rgb = bytearray(width * height * 3)
    offset = 0
    for row in range(height):
        base = row * stride
        for column in range(width):
            start = base + column * step
            pixel = int.from_bytes(raw[start:start + step], order)
            rgb[offset] = _channel(pixel, red_mask)
            rgb[offset + 1] = _channel(pixel, green_mask)
            rgb[offset + 2] = _channel(pixel, blue_mask)
            offset += 3
    return bytes(rgb)


def _x11_grab_rgb(x: int, y: int, width: int, height: int) -> bytes:
    """Copy a screen rectangle into packed RGB bytes.

    Reads the root window, so this sees whatever is physically on top -- which is
    what makes it the fallback. See _x11_render_window_rgb for the route that
    does not care what is on top.
    """
    if width <= 0 or height <= 0:
        raise ToolError(f"refusing to capture a {width}x{height} region")
    image = _get_image(_root(), x, y, width, height)
    if image is None:
        raise ToolError(
            f"the X server refused to read a {width}x{height} region at {x},{y} "
            "(is the display still connected?)"
        )
    try:
        return _ximage_to_rgb(image, width, height)
    finally:
        _x11.XDestroyImage(image)


def _x11_render_window_rgb(hwnd: int) -> tuple[int, int, int, int, bytes] | None:
    """Ask the compositor for a window's own pixels.

    This is the X11 counterpart of PrintWindow. A compositing manager redirects
    managed windows into off-screen pixmaps so that it can composite them, and
    XCompositeNameWindowPixmap names that pixmap. Reading it gives the window's
    contents with no dependence on what is covering it, on whether it has focus,
    or on whether it is on screen at all.

    What it holds is the window's OWN pixels, which is not the same as what the
    screen shows. A window that does not paint every pixel it owns -- a
    transparent terminal, or the desktop window that draws icons over a wallpaper
    it does not own -- has nothing in those pixels, and a pixel with nothing in
    it reads as black. That is why the caller still checks the result, and why
    blank edges send it to the screen instead.

    Some windows have no pixmap to name at all: a compositor keeps one for the
    windows it composites, and which those are is its own business (on Cinnamon a
    minimised window has one while a plainly visible one answers BadMatch). That
    is a refusal rather than a failure, and it comes back as None so the caller
    reads the screen.

    Returns the geometry of what it read, or None for either of the above.
    """
    if _xcomposite is None:
        return None
    x, y, width, height = _x11_client_rect_on_screen(hwnd)
    if width <= 0 or height <= 0:
        return None

    pixmap, failed = _x11_call(
        lambda: _xcomposite.XCompositeNameWindowPixmap(_display, ctypes.c_ulong(hwnd))
    )
    if not pixmap or failed:
        # Ordinary rather than exceptional, and common: a compositor redirects
        # the windows it composites, and this one keeps a pixmap only for some of
        # them (on Cinnamon, a minimized window still has one while a plain
        # visible one answers BadMatch). Either way the answer is "ask the
        # screen", so this is not logged as a problem.
        log.debug("window 0x%x has no compositor pixmap; reading the screen", hwnd)
        return None

    try:
        image = _get_image(pixmap, 0, 0, width, height)
        if image is None:
            log.debug(
                "window 0x%x: no usable pixmap at its %dx%d geometry", hwnd, width, height
            )
            return None
        try:
            rgb = _ximage_to_rgb(image, width, height)
        finally:
            _x11.XDestroyImage(image)
    finally:
        _free_pixmap(pixmap)

    if is_uniform(rgb):
        # A perfectly uniform read means there is nothing to show -- the window
        # has not drawn yet, or the pixmap came back empty. Not worth returning
        # as an image, and the screen may well do better.
        return None
    return x, y, width, height, rgb


def _x11_bring_to_front(hwnd: int, settle: float = 0.25) -> bool:
    """Ask the window manager to activate a window, and say whether it did.

    Raising is the window manager's business: with a reparenting or
    click-to-focus manager a client cannot reliably raise itself, and the EWMH
    way to ask is a _NET_ACTIVE_WINDOW client message sent to the root. Source
    indication 2 marks the request as coming from a pager-like tool, which is
    what this is; it tells the manager the request is not from the application
    itself, and so takes the same path a task bar click does.

    Returns whether the window ended up active, mirroring the Windows route
    testing SetForegroundWindow's return value. The capture proceeds regardless;
    it may simply contain whatever was on top.
    """
    if not _cardinals(_root(), "_NET_ACTIVE_WINDOW"):
        # No EWMH manager: there is no request to send, and nothing to verify
        # against afterwards.
        log.debug("no _NET_ACTIVE_WINDOW support; cannot ask for a raise")
        return False

    event = _XClientMessageEvent()
    ctypes.memset(ctypes.byref(event), 0, ctypes.sizeof(event))
    event.type = _CLIENT_MESSAGE
    event.display = _display
    event.window = hwnd
    event.message_type = _atom("_NET_ACTIVE_WINDOW")
    event.format = 32
    event.data[0] = _PAGER_SOURCE_INDICATION
    event.data[1] = _CURRENT_TIME
    event.data[2] = 0

    _x11.XSendEvent(
        _display,
        ctypes.c_ulong(_root()),
        False,  # sent to the root specifically, so it is not propagated further
        _SUBSTRUCTURE_NOTIFY | _SUBSTRUCTURE_REDIRECT,
        ctypes.byref(event),
    )
    _x11.XFlush(_display)
    time.sleep(settle)

    active = _cardinals(_root(), "_NET_ACTIVE_WINDOW")
    return bool(active) and active[0] == hwnd


def _x11_restore_window(hwnd: int) -> None:
    """Un-hide a window so it has something to render.

    Xlib has no "restore", because iconifying is not something the server does:
    the manager moves the window out of the way and remembers that it did. Under
    EWMH the request that un-hides a window is the same one that activates it --
    what a task bar click sends -- so this is that, with the settle the caller
    needs before measuring.

    Kept bound as restore_window for the shape of the interface rather than
    because the capture path calls it: on X11 RESTORE_MINIMIZED is False, since
    a hidden window's pixels are already available from the compositor and
    raising it would disturb the desktop for nothing. The fallback path still
    calls bring_to_front when it has to read the screen, which is the case where
    a hidden window really does have to appear.
    """
    _x11_bring_to_front(hwnd, settle=0.25)


def _x11_process_key(name: str) -> str:
    """Process names on X11 are executable names, with no suffix to add."""
    return name.lower()


# The X11 route descriptions. "Would not render itself" is a Windows way of
# putting it: on X11 the fallback happens because the compositor has no
# off-screen copy of the window, not because the window refused anything. The
# Windows constants of the same name live at the top of the file, and the
# rebinding block at the end swaps these in.
_X11_RENDER_ROUTE = "taken from the compositor's own copy of the window"
_X11_SCREEN_ROUTE = "read from the screen (the compositor had no copy to read)"


def _x11_platform_description() -> str:
    """What the backends on this machine can actually do."""
    if _x11 is None:
        return f"x11 unavailable ({_x11_problem})"
    _, _, screen_width, screen_height = _x11_virtual_screen()
    has_compositor = _xcomposite is not None and bool(
        _x11.XGetSelectionOwner(_display, ctypes.c_ulong(_atom("_NET_WM_CM_S0")))
    )
    if _xcomposite is None:
        route = "libXcomposite absent, so captures must read the screen"
    elif has_compositor:
        route = "compositor present, so occluded windows can still be read"
    else:
        route = "no compositor, so captures must read the screen"
    return (
        f"x11 (DISPLAY={os.environ.get('DISPLAY')!r}, screen "
        f"{screen_width}x{screen_height}, {route})"
    )


def capture_unavailable_reason() -> str:
    """Why capture cannot work here, or "" when it can.

    One place for the answer, because more than one thing can make it impossible
    and whoever asked needs to be told which.
    """
    if sys.platform == "win32":
        return ""
    return _x11_problem if _x11 is None else ""


# Windows uses the section at the top of this file; every other platform uses the
# one above, which raises a clear error of its own if the X server turns out to be
# unusable at the moment of a call.
if sys.platform != "win32":
    # Rebind the primitives the shared code in between calls. Doing it once, here,
    # keeps the two backends side by side rather than interleaving
    # `if sys.platform` through every function -- and it means the traversal,
    # cropping, PNG encoding and MCP plumbing are written once. find_window and
    # capture_to_png resolve these names at call time, so they follow along.
    enumerate_windows = _x11_enumerate_windows
    window_by_hwnd = _x11_window_by_hwnd
    client_rect_on_screen = _x11_client_rect_on_screen
    virtual_screen = _x11_virtual_screen
    grab_rgb = _x11_grab_rgb
    render_window_rgb = _x11_render_window_rgb
    bring_to_front = _x11_bring_to_front
    restore_window = _x11_restore_window
    process_key = _x11_process_key
    platform_description = _x11_platform_description
    RENDER_ROUTE = _X11_RENDER_ROUTE
    SCREEN_ROUTE = _X11_SCREEN_ROUTE
    RESTORE_MINIMIZED = False


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
    # so it has to come back first. This is the one unavoidable focus change --
    # and on X11 it is avoidable, hence the flag: the compositor keeps an
    # off-screen copy of a hidden window, so the capture can be taken from it
    # without touching the user's desktop at all.
    if info.minimized and raise_window and RESTORE_MINIMIZED:
        restore_window(hwnd)

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
    return not capture_unavailable_reason()


def _require_capture() -> None:
    """Refuse a capture request with an explanation rather than a traceback."""
    reason = capture_unavailable_reason()
    if reason:
        raise ToolError(f"screen capture is not available here: {reason}")


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
            "Run .mcp/screenshot/setup.sh (Windows: setup.ps1) to build the "
            "venv, or point the MCP server configuration at "
            ".mcp/screenshot/.venv/bin/python "
            "(Windows: .venv/Scripts/python.exe)"
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
            raise ToolError(f"list_windows needs a window system: {capture_unavailable_reason()}")

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
        included. Where the platform can produce the window's own pixels --
        on X11, the compositor's off-screen copy -- they are used, which works
        even when the window is behind other windows and does not disturb focus.

        Returns the image plus a line of text naming the source and the route
        used. The route matters: if it says the pixels were read from the screen,
        nothing off-screen was available for that window and the result may show
        whatever was covering it, so the image is worth a second look.

        Args:
            hwnd: window handle from list_windows(); the exact way to target
            process: executable name, matched case-insensitively
            title: fragment of the window title, matched case-insensitively
            save_to: optional path to also write the PNG to
        """
        if not is_capture_available():
            raise ToolError(f"capture_window needs a window system: {capture_unavailable_reason()}")

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
        no image viewer will open.

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
    print(f"backend     : {platform_description()}")
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

    # Everything below except --convert-ppm needs a window system. Without this
    # check each one dies on its first call, inside the ctypes bindings, as a
    # NoneType AttributeError that says nothing about the real problem -- which on
    # Linux is usually a missing DISPLAY rather than a broken desktop.
    wants_capture = (
        args.selftest or args.list or args.capture_screen
        or args.capture_process or args.capture_hwnd is not None
    )
    if wants_capture:
        reason = capture_unavailable_reason()
        if reason:
            print(f"cannot capture here: {reason}", file=sys.stderr)
            return 1

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
