# screenshot — an MCP server that lets an agent see the app

An agent can read a stack trace but not a rendered frame, which makes "the gears
look wrong" impossible to act on. This is a small local
[MCP](https://modelcontextprotocol.io) server that closes that gap: its tools
return screenshots as MCP image content, so the picture arrives as part of the
tool result and does not need to be shuttled around as a file.

It is how `--headless` output and live windows get looked at during development.

## Tools

| Tool | What it does |
| --- | --- |
| `list_windows` | Lists visible desktop windows: hwnd, pid, process, client size, title. Call this first. |
| `capture_window` | Screenshots a window (by `hwnd`, `process` or `title`) or the whole screen, and returns the image. |
| `show_image` | Returns an existing image file. Converts `.ppm` to PNG on the way. |

`list_windows` exists to make targeting exact. Matching on a title fragment will
happily pick the wrong window — searching for `vulkangears` also matches the
editor that has the folder open — so the reliable route is to read an `hwnd`
from `list_windows` and pass that to `capture_window`.

`show_image` understands `.ppm` because that is what
`vulkangears --headless --out shot.ppm` writes, and almost nothing on Windows
will open one.

## Setup

Once per clone. The server runs in a private virtualenv, which is not committed:

```powershell
powershell -ExecutionPolicy Bypass -File .mcp/screenshot/setup.ps1
```

Re-running is cheap; `-Force` rebuilds the venv from scratch.

## How it is registered

`.vscode/mcp.json`, at workspace scope, so it is only active while this folder is
open:

```json
{
  "servers": {
    "screenshot": {
      "type": "stdio",
      "command": "${workspaceFolder}/.mcp/screenshot/.venv/Scripts/python.exe",
      "args": ["${workspaceFolder}/.mcp/screenshot/server.py"]
    }
  }
}
```

Every path is written in terms of `${workspaceFolder}`, so the configuration has
no absolute paths in it and works on any clone. That is also the reason the
server lives *inside* the repository rather than in a global tools directory: a
user-profile registration would have to hardcode an absolute path into this
project, and would break the moment the project moved.

After `setup.ps1`, VS Code asks you to trust the server the first time it starts.
Manage it with **MCP: List Servers**.

## Testing it without VS Code

The capture layer is exercisable straight from a shell, with no MCP client
involved:

```sh
python .mcp/screenshot/server.py --selftest        # capture layer health check
python .mcp/screenshot/server.py --list            # visible windows
python .mcp/screenshot/server.py --capture-process vulkangears --out shot.png
python .mcp/screenshot/server.py --capture-screen  --out screen.png
python .mcp/screenshot/server.py --convert-ppm shot.ppm --out shot.png
```

And end to end over the real protocol, in a subprocess, the way a host does it:

```sh
.mcp/screenshot/.venv/Scripts/python.exe .mcp/screenshot/smoke_test.py
```

There is also `run.cmd`, which builds the virtualenv on first use so it works
from a fresh clone with no setup step at all:

```sh
.mcp/screenshot/run.cmd --selftest
.mcp/screenshot/run.cmd --capture-process vulkangears --out shot.png
```

## Why the MCP `command` is `python.exe` and not `run.cmd`

`run.cmd` is the friendlier thing to point the configuration at — it bootstraps
the venv, so a fresh clone would need no setup step. It does not work, and the
failure is invisible rather than loud.

Node refuses to spawn a `.cmd` unless `shell: true` is set (the hardening for
CVE-2024-27980), and it throws `spawn EINVAL` *synchronously* rather than
emitting an `error` event, so a host that handles spawn failures gracefully still
crashes out. VS Code is Electron, so it inherits this. An `.exe` is unaffected:

```sh
# throws spawn EINVAL
node -e "require('child_process').spawn('C:/.../.mcp/screenshot/run.cmd')"

# fine
node -e "require('child_process').spawn('C:/.../.mcp/screenshot/.venv/Scripts/python.exe')"
```

So `mcp.json` names `python.exe` directly, and `setup.ps1` stays the required
once-per-clone step for the VS Code path. `run.cmd` remains useful by hand, and as
the launcher for anyone who would rather not think about the venv at all.

## How a window is captured

Two routes, tried in order:

1. **The window renders itself** — `PrintWindow` with `PW_RENDERFULLCONTENT`.
   This is the normal path. It needs no focus and works while the window sits
   behind others, so nothing is raised and nothing steals focus. It works for
   GPU-composited surfaces too, which was the thing worth checking: verified
   against a running `vulkangears` while occluded, reporting a live frame
   counter, so the Vulkan swapchain comes through.
2. **Read the screen** at the window's rectangle, raising it first. Only used if
   the window refuses to render. A uniformly black result counts as a refusal,
   because that is `PrintWindow`'s failure mode rather than an error it reports.

The route used is named in the text next to the image. That is not decoration:
route 2 captures whatever is physically on the monitor, so it can return a
picture of a *different window* that happens to be on top. Knowing which route
ran is the only way to tell.

That failure is not hypothetical — it is why route 1 exists. The first version
of this server only read the screen and raised the target. Called through the
agent it returned a screenshot of the code editor instead of the application,
with nothing to indicate anything was wrong. `SetForegroundWindow` from a
background process is at the mercy of the Windows foreground lock, and when it
fails silently the capture is simply of whatever was on top.

There is a third case, sitting between the two and easy to miss. A DPI-**unaware**
window renders itself at its own logical size. Ask for a bitmap the size of the
physical window and the content comes back shrunk into a corner with the rest
left black — not a refusal, just a plausible-looking image with most of the
window missing. Measured on the SDL2 + OpenGL demo, launched at 800x600 on a 150%
scaled display: the window is 1224x959 physical, and `PrintWindow` delivered
816x631 of content, which is 1224 divided by 1.5. Since a real window does not
have a wholly black bottom edge *and* right edge at the same time, that pattern
is treated as a refusal too and the screen route takes over.

## Relationship to `scripts/capture_window.ps1`

They are two independent implementations of the same small idea, and neither
depends on the other. The PowerShell script stays so that someone who clones the
project can take a screenshot without installing an MCP server at all; this
server reimplements it in-process so it needs no external tooling and can hand
back bytes instead of a file.

Ported from that script deliberately, because its details were arrived at by
testing rather than by reading documentation:

- **DPI awareness** is set once, at import. Without it Windows reports logical
  pixels while the screen holds physical ones, and an 800x600 window at 150%
  scaling comes back as a cropped 533x400. The first version called this per
  entry point, and one of them quietly forgot.
- **The client area** is captured rather than the whole window, so window chrome
  is not in the picture. Route 1 renders the whole window and then crops, using
  the offset between the window rectangle and the client rectangle.
- **Reading the screen is the fallback, not the main event.** A window DC returns
  black for a GPU-composited surface, while reading the screen sees whatever is
  on top — which is how a request for one window came back as a picture of
  another. Asking the window to render itself avoids both problems at once.

## Notes and limits

- **Windows only**, by nature.
- `capture_window` normally does **not** disturb focus, because the window
  renders itself. The exceptions are a minimised target (it has to be restored
  to have anything to render) and the rare fallback to reading the screen, which
  requires the window to be visible.
- **Full-screen capture means the whole virtual desktop**, so on a multi-monitor
  setup it spans every monitor.
- **Mixed-DPI multi-monitor** is the one place geometry can still surprise:
  per-monitor-v2 awareness makes each window's own size correct, but a
  full-screen grab across monitors at different scale factors is taken at face
  value.
- **Any window can be captured**, including password managers and private
  conversations, and this tool is invocable by an agent. Keep that in mind before
  pointing it at a screen that should not be photographed.
- The `.mcp/` directory (this tool's code) and `.vscode/mcp.json` (VS Code's
  configuration) are unrelated things that happen to share a name — that
  collision is VS Code's naming, not ours.

## Troubleshooting

Run `--selftest` first; it reports the DPI mode, the virtual screen rectangle and
the windows it can see.

If the server does not appear in VS Code, the usual cause is a missing venv —
run `setup.ps1`. To see why a server is failing to start, use **MCP: List
Servers** and choose **Show Output**.
