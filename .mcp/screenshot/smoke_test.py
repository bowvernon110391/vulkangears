#!/usr/bin/env python3
"""End-to-end smoke test for the screenshot MCP server.

Launches the server the same way a host does -- a subprocess, over stdio -- and
exercises every tool, so this covers the protocol path and not merely the
capture functions.  Run it with the venv interpreter:

    .mcp/screenshot/.venv/bin/python .mcp/screenshot/smoke_test.py

Windows uses the same command with the other interpreter path:

    .mcp/screenshot/.venv/Scripts/python.exe .mcp/screenshot/smoke_test.py

Exits non-zero if anything fails.  Needs a desktop session, since the capture
tools talk to the real screen.
"""

from __future__ import annotations

import asyncio
import base64
import os
import re
import struct
import sys
import tempfile
from pathlib import Path

from mcp import Client
from mcp.client.stdio import StdioServerParameters

HERE = Path(__file__).resolve().parent
SERVER = HERE / "server.py"
PNG_MAGIC = b"\x89PNG\r\n\x1a\n"

# The expected wording of the two capture routes is the server's own, not a copy
# of it, so the checks here follow the platform instead of pinning one of them:
# importing the module only runs its setup, never main().
sys.path.insert(0, str(HERE))
import server  # noqa: E402  (the path has to be set up first)

# hwnd  pid  size       process / title
WINDOW_ROW = re.compile(r"^\s*(\d+)\s+(\d+)\s+(\d+)x(\d+)\s+(\S+)", re.MULTILINE)

_failures: list[str] = []


def check(label: str, condition: bool, detail: str = "") -> None:
    if condition:
        print(f"  ok    {label}")
    else:
        print(f"  FAIL  {label}" + (f" -- {detail}" if detail else ""))
        _failures.append(label)


def image_blocks(content) -> list:
    return [block for block in content if getattr(block, "type", "") == "image"]


def assert_png_block(label: str, content, expect_size: tuple[int, int] | None = None) -> None:
    blocks = image_blocks(content)
    check(f"{label}: returns an image block", len(blocks) == 1, f"got {len(blocks)}")
    if not blocks:
        return
    block = blocks[0]
    check(f"{label}: mime type is image/png", block.mime_type == "image/png",
          f"got {block.mime_type}")
    try:
        raw = base64.b64decode(block.data)
    except Exception as exc:  # noqa: BLE001 - report whatever went wrong
        check(f"{label}: payload is base64", False, str(exc))
        return
    check(f"{label}: payload is a real PNG", raw[:8] == PNG_MAGIC,
          f"first bytes {raw[:8]!r}")
    # 8 byte signature + IHDR + IDAT + IEND chunks is the floor for a valid file.
    check(f"{label}: payload is not truncated", len(raw) > 60, f"{len(raw)} bytes")
    if expect_size:
        width, height = struct.unpack(">II", raw[16:24])
        check(f"{label}: size is {expect_size[0]}x{expect_size[1]}",
              (width, height) == expect_size, f"got {width}x{height}")


async def run() -> int:
    print(f"server : {SERVER}")
    print(f"python : {sys.executable}")
    print()

    # The child is given the whole environment on purpose.  The SDK's own idea of
    # a clean environment carries only HOME, LOGNAME, PATH, SHELL, TERM and USER,
    # which is enough on Windows but leaves out DISPLAY and XAUTHORITY -- and on
    # Linux those are the entire configuration of the connection to the screen,
    # so the server would come up believing there is no X server at all.
    params = StdioServerParameters(
        command=sys.executable, args=[str(SERVER)], env=dict(os.environ)
    )

    # A successful handshake is itself the check that the server keeps stdout
    # clean: anything printed there would corrupt the JSON-RPC framing and the
    # connection would fail before we got this far.
    async with Client(params) as client:
        print("tools")
        listing = await client.list_tools()
        names = sorted(tool.name for tool in listing.tools)
        check("exactly three tools are exposed", len(names) == 3, f"got {names}")
        check("tool names are as expected",
              names == ["capture_window", "list_windows", "show_image"],
              f"got {names}")
        for tool in listing.tools:
            summary = (tool.description or "").strip().splitlines()
            print(f"        {tool.name}: {summary[0] if summary else '(no description)'}")

        print("\nlist_windows")
        result = await client.call_tool("list_windows", {})
        check("list_windows succeeds", not result.is_error)
        text = "".join(
            getattr(block, "text", "") for block in result.content
        )
        rows = WINDOW_ROW.findall(text)
        check("list_windows finds at least one window", bool(rows), text[:200])
        for hwnd, pid, width, height, process in rows[:6]:
            print(f"        hwnd {hwnd:>10}  pid {pid:>7}  {width}x{height}  {process}")

        print("\ncapture_window")
        if rows:
            hwnd, _pid, width, height, process = rows[0]
            result = await client.call_tool("capture_window", {"hwnd": int(hwnd)})
            check("capture_window by hwnd succeeds", not result.is_error,
                  _error_text(result))
            assert_png_block("capture_window by hwnd", result.content)
            # The image alone would leave the caller unable to tell which route
            # produced it -- and a fallback to reading the screen can return the
            # wrong window, so the route is the difference between trusting the
            # picture and not.
            note = text_of(result)
            check("capture_window explains what it captured",
                  "Captured" in note and process in note, f"got {note[:120]!r}")
            check("capture_window names the route it used",
                  server.RENDER_ROUTE in note or server.SCREEN_ROUTE in note,
                  f"got {note[:120]!r}")
            print(f"        -> {note.strip().splitlines()[0][:110]}")

        # A window that cannot exist must fail loudly rather than hand back a
        # plausible-looking blank image, and the reason must survive the trip:
        # the SDK strips the text of unrecognised exceptions.
        result = await client.call_tool("capture_window", {"process": "no-such-program-xyz"})
        check("capture_window reports an unknown process", result.is_error,
              "expected an error result")
        detail = _error_text(result)
        check("the diagnostic reaches the caller", "no-such-program-xyz" in detail,
              f"message was: {detail[:120]!r}")
        print(f"        -> {detail.splitlines()[0][:110]}")

        # No target at all means the whole virtual screen, by design.
        result = await client.call_tool("capture_window", {})
        check("capture_window with no target grabs the screen", not result.is_error,
              _error_text(result))
        assert_png_block("capture_window (full screen)", result.content)
        check("the full-screen route is described",
              "virtual screen" in text_of(result), text_of(result)[:120])

        print("\nshow_image")
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)

            # A PPM is what `vulkangears --headless --out shot.ppm` produces and
            # nothing on a stock Windows box will open it, so this path matters.
            ppm = tmpdir / "tiny.ppm"
            ppm.write_bytes(b"P6\n2 2\n255\n" + bytes([
                255, 0, 0, 0, 255, 0,
                0, 0, 255, 255, 255, 255,
            ]))
            result = await client.call_tool("show_image", {"path": str(ppm)})
            check("show_image converts a PPM", not result.is_error, _error_text(result))
            assert_png_block("show_image (ppm)", result.content, expect_size=(2, 2))

            png = tmpdir / "tiny.png"
            png.write_bytes(PNG_MAGIC)  # signature only; the pass-through path does not decode
            result = await client.call_tool("show_image", {"path": str(png)})
            check("show_image accepts a PNG", not result.is_error, _error_text(result))
            blocks = image_blocks(result.content)
            check("show_image passes a PNG through", len(blocks) == 1)

            result = await client.call_tool("show_image", {"path": str(tmpdir / "nope.png")})
            check("show_image reports a missing file", result.is_error)

            junk = tmpdir / "nope.txt"
            junk.write_text("not an image")
            result = await client.call_tool("show_image", {"path": str(junk)})
            check("show_image rejects an unsupported type", result.is_error)

    print()
    if _failures:
        print(f"{len(_failures)} check(s) failed:")
        for name in _failures:
            print(f"  - {name}")
        return 1
    print("all checks passed")
    return 0


def text_of(result) -> str:
    """Every text block in a result, successful or not."""
    return "\n".join(
        text for block in result.content if (text := getattr(block, "text", ""))
    )


def _error_text(result) -> str:
    return text_of(result) if result.is_error else ""


if __name__ == "__main__":
    sys.exit(asyncio.run(run()))
