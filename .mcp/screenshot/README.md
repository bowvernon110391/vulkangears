# screenshot — an MCP server that lets an agent see the app

An agent can read a stack trace but not a rendered frame, which makes "the gears
look wrong" impossible to act on. This is a small local
[MCP](https://modelcontextprotocol.io) server that closes that gap: its tools
return screenshots as MCP image content, so the picture arrives as part of the
tool result and does not need to be shuttled around as a file.

It is how `--headless` output and live windows get looked at during development.

Two backends, chosen automatically: Win32 (`PrintWindow` and GDI) on Windows, X11
(xlib, with Xcomposite when available) everywhere else. The MCP surface is
identical on both, and so is everything it returns.

```
server.py              the MCP server, both backends
write_mcp_config.py    writes .vscode/mcp.json for this platform (run by setup)
setup.sh / setup.ps1   build the venv, then register it
run.sh / run.cmd       build the venv on first use, then run the server
smoke_test.py          end-to-end check over the real stdio protocol
requirements.txt       the MCP SDK, and nothing else
```

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
`vulkangears --headless --out shot.ppm` writes, and a stock file manager will not
open one on either platform.

Throughout this file `hwnd` is the name of the field and the argument, following
the original Windows version. On X11 it holds a window id (an XID); nothing else
changes, and `list_windows` prints whichever one this machine uses.

## Setup

Once per clone. The server runs in a private virtualenv, which is not committed:

```sh
bash .mcp/screenshot/setup.sh
```

```powershell
powershell -ExecutionPolicy Bypass -File .mcp/screenshot/setup.ps1
```

Re-running is cheap and always safe. `--force` (`-Force` on Windows) throws the
venv away and rebuilds it from scratch, which is the fix for one that is corrupt
or half-installed.

Setup does two things: it builds the venv, and it writes `.vscode/mcp.json` so
VS Code starts the server with the interpreter it just built. Reload the window
afterwards to pick that up. A venv built by the other OS's Python is discarded and
rebuilt without being asked, because a hybrid one — both `bin/` and `Scripts/`, a
`pyvenv.cfg` pointing at the other system — cannot be run by either. That makes
re-running the setup script the fix for moving between platforms, as well.

`setup.sh` installs nothing outside the clone and never asks for root. It has a
fallback for a Debian box where `python3-venv` and `python3-pip` are missing,
which is common on a fresh install: `python3 -m venv --without-pip` plus the
official `get-pip.py` bootstrap, cached in `.mcp/screenshot/.pip-bootstrap/` so a
rebuild does not need the network twice. A real `pip` is used when one is present.
If you would rather install the packages properly:

```sh
sudo apt install python3-venv python3-pip
```

## How it is registered

`.vscode/mcp.json`, at workspace scope, so it is only active while this folder is
open. It is **generated, not committed**: each setup script writes it from the
interpreter it just built, and the file is gitignored.

```json
{
    "servers": {
        "screenshot": {
            "type": "stdio",
            "command": "${workspaceFolder}/.mcp/screenshot/.venv/bin/python",
            "args": ["${workspaceFolder}/.mcp/screenshot/server.py"]
        }
    },
    "inputs": []
}
```

That `command` is the Linux value; on Windows it is
`${workspaceFolder}/.mcp/screenshot/.venv/Scripts/python.exe`.

It has to be written rather than committed because a venv keeps its interpreter
in `bin/` on Linux and `Scripts/` on Windows, and the MCP schema has no
per-platform conditional — the stdio fields are only `type`, `command`, `args`,
`cwd`, `env`, `envFile`, `dev` and `sandboxEnabled`. `launch.json` can say "this
on Windows, that on Linux"; `mcp.json` cannot. One committed file is therefore
wrong on whichever platform it was not committed from, and on a checkout opened
on both it would have to be edited and re-edited.

Every path in the generated file is written in terms of `${workspaceFolder}`, so
it carries no absolute paths and survives moving or renaming the checkout. That is
also the reason the server lives *inside* the repository rather than in a global
tools directory: a user-profile registration would have to hardcode an absolute
path into this project, and would break the moment the project moved.

`write_mcp_config.py` replaces only the `screenshot` entry, so any other servers
and the `inputs` array in the file survive. If the file has been hand-edited into
something that is not valid JSON it refuses to overwrite it, and says so — a
half-finished edit is not ours to discard.

After the setup script, reload the window, then VS Code asks you to trust the
server the first time it starts. Manage it with **MCP: List Servers**.

Nothing needs to be configured to reach the display. A host that curates the
environment for its child processes can drop `DISPLAY` — the reference Python
stdio client does exactly that, keeping only `HOME`, `LOGNAME`, `PATH`, `SHELL`,
`TERM` and `USER` — and the server then starts believing there is no X server. If
that happens, `--selftest` says so in those words, and adding the variable to the
server's `env` in `mcp.json` is the fix. VS Code passes its own environment
through, so it is not affected.

## Testing it without VS Code

The capture layer is exercisable straight from a shell, with no MCP client
involved:

```sh
.mcp/screenshot/.venv/bin/python .mcp/screenshot/server.py --selftest
.mcp/screenshot/.venv/bin/python .mcp/screenshot/server.py --list
.mcp/screenshot/.venv/bin/python .mcp/screenshot/server.py --capture-process vulkangears --out shot.png
.mcp/screenshot/.venv/bin/python .mcp/screenshot/server.py --capture-screen  --out screen.png
.mcp/screenshot/.venv/bin/python .mcp/screenshot/server.py --convert-ppm shot.ppm --out shot.png
```

And end to end over the real protocol, in a subprocess, the way a host does it:

```sh
.mcp/screenshot/.venv/bin/python .mcp/screenshot/smoke_test.py
```

There is also `run.sh` (`run.cmd` on Windows), which builds the virtualenv on
first use so it works from a fresh clone with no setup step at all:

```sh
.mcp/screenshot/run.sh --selftest
.mcp/screenshot/run.sh --capture-process vulkangears --out shot.png
```

## Why the MCP `command` names the interpreter and not the launcher

`run.sh` is the friendlier thing to point the configuration at — it bootstraps the
venv, so a fresh clone would need no setup step. On Linux that would work, since a
shell script with the executable bit and a shebang spawns like any other program.
On Windows it does not work, and the failure is invisible rather than loud.

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

So `mcp.json` names the interpreter directly on both platforms — symmetric, and
one fewer thing to go wrong — and the setup script stays the required
once-per-clone step for the VS Code path. `run.sh` and `run.cmd` remain useful by
hand, and as the launcher for anyone who would rather not think about the venv at
all. Neither is ever the thing `mcp.json` points at, so the `.cmd` problem cannot
bite, and the generated file needs no platform-specific thinking in it.

## Working on this from a shell

`run.sh` and `setup.sh` are bash scripts, not POSIX `sh`: they use `BASH_SOURCE`
to find their own directory and `set -euo pipefail`. Run them with `bash` (or
`./setup.sh`, which reads the shebang fallback in `/usr/bin/env bash`). On Windows
use `run.cmd` and `setup.ps1`, or Git Bash, which provides bash.

## How a window is captured

Two routes, tried in order, and the same shape on both backends: ask for the
window's own pixels first, and read the screen only if that is refused.

**Windows**

1. **The window renders itself** — `PrintWindow` with `PW_RENDERFULLCONTENT`.
   This is the normal path. It needs no focus and works while the window sits
   behind others, so nothing is raised and nothing steals focus. It works for
   GPU-composited surfaces too, which was the thing worth checking: verified
   against a running `vulkangears` while occluded, reporting a live frame
   counter, so the Vulkan swapchain comes through.
2. **Read the screen** at the window's rectangle, raising it first. Only used if
   the window refuses to render. A uniformly black result counts as a refusal,
   because that is `PrintWindow`'s failure mode rather than an error it reports.

**X11** (`libX11.so.6`, plus `libXcomposite.so.1` when it is installed)

1. **`XCompositeNameWindowPixmap`** — asks the compositor for the pixmap that
   holds the window's own pixels. This is the X11 counterpart of `PrintWindow`:
   the pixels are the application's, not the screen's, so occlusion does not
   matter and a minimised window can still be read. `xwd`, `import` and
   `gnome-screenshot` cannot do this — they all photograph the screen, so they
   can only return the wrong window. That is why this talks to Xlib directly
   instead of shelling out.
2. **`XGetImage` on the root window**, raising the target first, when the
   compositor has no copy to hand over.

The route used is named in the text next to the image. That is not decoration:
route 2 captures whatever is physically on the monitor, so it can return a
picture of a *different window* that happens to be on top. Knowing which route
ran is the only way to tell.

That failure is not hypothetical — it is why route 1 exists. The first version
of this server only read the screen and raised the target. Called through the
agent it returned a screenshot of the code editor instead of the application,
with nothing to indicate anything was wrong. `SetForegroundWindow` from a
background process is at the mercy of the Windows foreground lock, and when it
fails silently the capture is simply of whatever was on top. Raising a window on
X11 is a request to the window manager rather than a command, so it has the same
character.

### Details that only turned up by testing

There is a third Windows case, sitting between the two routes and easy to miss. A
DPI-**unaware** window renders itself at its own logical size. Ask for a bitmap
the size of the physical window and the content comes back shrunk into a corner
with the rest left black — not a refusal, just a plausible-looking image with most
of the window missing. Measured on the SDL2 + OpenGL demo, launched at 800x600 on
a 150% scaled display: the window is 1224x959 physical, and `PrintWindow`
delivered 816x631 of content, which is 1224 divided by 1.5. Since a real window
does not have a wholly black bottom edge *and* right edge at the same time, that
pattern is treated as a refusal too and the screen route takes over.

On X11 the equivalent trap is subtler, and it is the reason the compositor pixmap
is decoded pixels-at-a-time rather than assumed: a **pixmap has no visual**, so
Xlib reports zero red, green and blue masks for it even though the window it
belongs to is a perfectly ordinary 24-bit one. A zero mask converts every channel
to zero, which reads as a solid black image, which reads as a refusal — so the
route was silently rejected and quietly fell back to the screen until the root
window's visual was consulted for the real masks. And "black means refusal" is
right for `PrintWindow`, but wrong in general: a window that really is black is
indistinguishable from one that declined, so on X11 the two are told apart by the
error path below rather than by the pixels.

Two more facts about Xlib that the code depends on:

- **Errors arrive one round trip late.** `XCompositeNameWindowPixmap` answers a
  request for an unredirected window with `BadMatch` *and* a junk pixmap id, so
  reading the id alone is not enough to know whether it worked. An `XSync` after
  each guarded call is what makes the error visible in time; without it a refused
  request is indistinguishable from a successful one, and the difference here is
  between a real pixmap and a number that merely looks like one. Freeing the
  latter ends the process, since Xlib's default error handler calls `exit()`.
- **Xlib delivers that error asynchronously, to a handler**, so the handler is
  installed, the call is made, and the flag it sets is read — every capture, not
  once at startup.

## Relationship to `scripts/capture_window.ps1`

They are two independent implementations of the same small idea, and neither
depends on the other. The PowerShell script stays so that someone who clones the
project on Windows can take a screenshot without installing an MCP server at all;
this server reimplements it in-process so it needs no external tooling and can
hand back bytes instead of a file. There is no Linux equivalent, because the X11
route here does something the script cannot: it reads the compositor's copy of the
window rather than the screen. The nearest shell equivalents on Linux are `import`
and `gnome-screenshot`, and both only see the screen.

The Windows backend was ported from that script deliberately, because its details
were arrived at by testing rather than by reading documentation:

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

- **Requires a window system, and on Linux that means X11.** This is not a choice:
  the capture route is the compositor's copy of the window, and on X11 that is
  `XCompositeNameWindowPixmap`. A Wayland-only session has no root window to read
  and no way for a client to name another client's buffer, so capture simply does
  not exist there. An application running through XWayland is an X11 client and is
  captured like any other. To check which session you are in:
  `echo $XDG_SESSION_TYPE`.
- **On X11 the window a capture returns is the one the X server knows about**, so
  title and geometry come from the window manager's properties (`_NET_WM_NAME`,
  `_NET_WM_STATE`, `_NET_WM_PID`, `_NET_CLIENT_LIST`). A window that sets none of
  them still appears in the list but with a plain title, matched by its executable
  name instead.
- `capture_window` normally does **not** disturb focus, because the window's own
  pixels are available without one. The exception is the fallback to reading the
  screen, which requires the window to be visible. On Windows a minimised target
  is restored for the same reason; on X11 it is not, because the compositor still
  holds its pixels — a minimised window can be captured without the desktop
  changing at all.
- **Full-screen capture means the whole virtual desktop**, so on a multi-monitor
  setup it spans every monitor.
- **Mixed-DPI multi-monitor** is a Windows-specific place geometry can still
  surprise: per-monitor-v2 awareness makes each window's own size correct, but a
  full-screen grab across monitors at different scale factors is taken at face
  value. X11 has no per-window scale factor, so the question does not arise.
- **Any window can be captured**, including password managers and private
  conversations, and this tool is invocable by an agent. Keep that in mind before
  pointing it at a screen that should not be photographed.
- The `.mcp/` directory (this tool's code) and `.vscode/mcp.json` (VS Code's
  configuration) are unrelated things that happen to share a name — that
  collision is VS Code's naming, not ours.

## Troubleshooting

Run `--selftest` first. It names the backend, the display it connected to, the
virtual screen rectangle and the windows it can see:

```
python      : /home/you/testdsh/.mcp/screenshot/.venv/bin/python
backend     : x11 (DISPLAY=':0', screen 1366x768, compositor present, so occluded windows can still be read)
virtual scr : (0, 0, 1366, 768)
```

`libXcomposite absent` in that line means only the screen route is available, so a
window has to be visible to be captured and an occluded capture returns whatever
is on top. `no compositor` says the same thing for a compositing manager that is
not running. Both are warnings, not failures.

If the server does not appear in VS Code, the usual cause is a missing venv — run
`setup.sh`. To see why a server is failing to start, use **MCP: List Servers** and
choose **Show Output**.

If it worked yesterday and not today, and yesterday was the other operating
system, the venv is the problem: it belongs to the platform that built it. Re-run
the setup script for this platform, which rebuilds it, then reload the window.
`.vscode/mcp.json` is written at the same time, so both halves are fixed by the
one command.

If `--selftest` works but the VS Code tools fail to start, the registration is
pointing somewhere stale — `write_mcp_config.py` will not overwrite a file whose
JSON has been broken by hand, so check that the `command` in `.vscode/mcp.json`
names this checkout's `.venv`, and re-run the setup script.

`no X server is reachable` from `--selftest` or from any tool means the process
has no `DISPLAY`. Run it from the desktop session, or set `DISPLAY` in the
server's `env` in `mcp.json`; on a Wayland-only desktop, see the first note above.

On Windows, `$'\r': command not found` from a shell script means the checkout has
CRLF line endings. `.gitattributes` pins `*.sh` and `*.py` to LF to prevent that;
if a file predates it, re-checkout the file or run `git add --renormalize .`.
