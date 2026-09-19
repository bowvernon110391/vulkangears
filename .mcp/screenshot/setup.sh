#!/usr/bin/env bash
#
# Create the private virtualenv this MCP server runs in.
#
#   bash .mcp/screenshot/setup.sh
#
# The venv lives inside this folder and is gitignored, so it never ends up in
# the repository but is always beside the code that needs it. Re-running is
# cheap and safe: it only creates the venv when one is missing.
#
# --force throws the existing venv away first, which is the fix for a venv that
# has been broken by a Python upgrade or an interrupted install.  That case is
# also caught automatically: a venv that has no interpreter for this platform is
# rebuilt without being asked, which covers a venv left behind by Windows on a
# checkout that is opened on both systems.
#
# It finishes by writing .vscode/mcp.json, so VS Code starts the server with this
# machine's interpreter.  That file is generated rather than committed because the
# venv path differs per platform and the MCP schema has no per-platform
# conditional; see write_mcp_config.py.
#
# Debian and Ubuntu ship Python without pip and without the venv support that
# would give it one, so this script has a second way in that needs no root:
# it builds the venv with --without-pip and fetches pip from the PyPA bootstrap
# script, the same "vendor it yourself" approach scripts/fetch_deps.sh takes for
# the C++ dependencies. The sudo route is offered as a message, never run.
#
# Everything this script says goes to stderr, including the successes. That is
# not tidiness: run.sh can end up being spawned by a host that treats this
# server's stdout as a JSON-RPC stream, and one stray line on stdout is a
# protocol error that looks like the server crashing.

set -euo pipefail

HERE="$(cd -P "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV="$HERE/.venv"
PYTHON="$VENV/bin/python"
PIP_CACHE="$HERE/.pip-bootstrap"
FORCE=0

note() { printf '[setup] %s\n' "$*" >&2; }
warn() { printf '[setup] warning: %s\n' "$*" >&2; }
die()  { printf '[setup] %s\n' "$*" >&2; exit 1; }

usage() {
    cat >&2 <<EOF
usage: $(basename "$0") [--force]

Creates $VENV and installs requirements.txt into it.

  --force   remove the existing virtualenv first (the fix for one that has
            been broken by a Python upgrade or an interrupted install)
  -h        this message
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --force|-f) FORCE=1 ;;
        -h|--help) usage; exit 0 ;;
        *) note "unknown argument: $1"; usage; exit 2 ;;
    esac
    shift
done

if [ "$FORCE" = 1 ] && [ -e "$VENV" ]; then
    note "removing existing venv: $VENV"
    rm -rf "$VENV"
fi

# ---------------------------------------------------------------------------
# Find an interpreter and build the venv
# ---------------------------------------------------------------------------

# The version matters: the MCP SDK this server is built on needs 3.10 or later,
# and a venv built from an older one produces an import error much further from
# the cause than this check does.
pick_interpreter() {
    local candidate
    for candidate in ${PYTHON_BIN:-python3 python}; do
        if command -v "$candidate" >/dev/null 2>&1 \
            && "$candidate" -c 'import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)' 2>/dev/null
        then
            printf '%s' "$candidate"
            return 0
        fi
    done
    return 1
}

# ---------------------------------------------------------------------------
# pip, which is the one thing a Debian venv can be missing
# ---------------------------------------------------------------------------

# The message for the case where nothing worked, including the bootstrap below.
# Kept as a function so that every failure path can repeat it verbatim.
apt_hint() {
    cat >&2 <<EOF
        On Debian and Ubuntu this is the missing venv support package. Installing
        it needs root, so it is yours to run:

            sudo apt install python3-venv python3-pip

        Fedora / RHEL:      sudo dnf install python3-pip
        Arch:               pacman -S python python-pip
        macOS + Homebrew:   brew install python (pip is included)

        Then run this script again.
EOF
}

# Fetch pip without root. The venv was built by an interpreter with no ensurepip,
# so the only way it can get a pip is from the outside: PyPA publishes the
# bootstrap script that does exactly this, and it is the same script the "no
# pip?" instructions on pip's own site tell you to use.
fetch_pip() {
    local downloader url target
    if command -v curl >/dev/null 2>&1; then
        downloader="curl -fsSL --max-time 120 -o"
    elif command -v wget >/dev/null 2>&1; then
        downloader="wget -q -T 120 -O"
    else
        warn "neither curl nor wget is available, so pip cannot be fetched"
        return 1
    fi

    url="https://bootstrap.pypa.io/get-pip.py"
    target="$PIP_CACHE/get-pip.py"
    if [ ! -s "$target" ]; then
        mkdir -p "$PIP_CACHE"
        note "fetching pip from $url (no root needed)"
        # shellcheck disable=SC2086  # word splitting is how the downloader is chosen
        if ! $downloader "$target" "$url" 2>&1; then
            rm -f "$target"
            warn "could not download $url"
            return 1
        fi
    else
        note "using the cached bootstrap script: $target"
    fi

    # -q would hide the reason a build failed, which is the one thing the user
    # would need; pip's own progress goes to stderr in any case.
    if ! "$PYTHON" "$target"; then
        warn "the bootstrap script failed"
        return 1
    fi
    return 0
}

ensure_pip() {
    if "$PYTHON" -m pip --version >/dev/null 2>&1; then
        return 0
    fi
    # ensurepip is what normally provides it, and it is worth trying because the
    # venv may have been built by a different interpreter than the one the
    # system uses for python3 -m venv.
    if "$PYTHON" -m ensurepip --upgrade >/dev/null 2>&1; then
        "$PYTHON" -m pip --version >/dev/null 2>&1 && return 0
    fi
    fetch_pip || true
    if "$PYTHON" -m pip --version >/dev/null 2>&1; then
        return 0
    fi
    return 1
}

# ---------------------------------------------------------------------------
# Build the venv
# ---------------------------------------------------------------------------

created_here=0
if [ ! -x "$PYTHON" ]; then
    # Only reached when this platform's interpreter is absent, so an existing
    # .venv here was built by the other platform -- or is broken by a Python
    # upgrade, which is the same thing from here: unusable.  It has to go rather
    # than be reused, because venv would create its own bin/ (or Scripts/)
    # alongside the foreign one and rewrite pyvenv.cfg to point at the other
    # system's Python, leaving a hybrid that neither platform can explain.
    # Deleting it also makes a broken venv self-healing, which is what --force
    # existed to do by hand.
    if [ -e "$VENV" ]; then
        note "the existing venv was built by another platform or is broken; rebuilding it"
        rm -rf "$VENV"
    fi
    if ! INTERPRETER="$(pick_interpreter)"; then
        die "no Python 3.10 or newer found (looked for \$PYTHON_BIN, python3, python)"
    fi
    note "creating venv with: $INTERPRETER -m venv $VENV"
    # Which form to ask for is decided by probing rather than by trying the good
    # one and reading the wreckage. On Debian "python3 -m venv" prints a
    # paragraph about a virtual environment that was not created and leaves the
    # directory behind, and none of that helps someone who only wanted a server
    # installed -- so ensurepip is asked first and the venv is built in the form
    # that will actually work.
    venv_args=()
    if ! "$INTERPRETER" -m ensurepip --version >/dev/null 2>&1; then
        note "this Python has no ensurepip; building the venv without pip and fetching it after"
        venv_args+=(--without-pip)
    fi
    if ! "$INTERPRETER" -m venv "${venv_args[@]}" "$VENV"; then
        rm -rf "$VENV"
        warn "could not create a virtualenv at all"
        apt_hint
        exit 1
    fi
    created_here=1
fi

if [ ! -x "$PYTHON" ]; then
    die "venv creation failed: $PYTHON was not produced"
fi

if ! ensure_pip; then
    if [ "$created_here" = 1 ]; then
        rm -rf "$VENV"
    fi
    warn "the venv exists but has no pip, and one could not be fetched"
    apt_hint
    exit 1
fi

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

note "installing dependencies"
if ! "$PYTHON" -m pip install --quiet --upgrade pip; then
    warn "could not upgrade pip; continuing with the one already installed"
fi
if ! "$PYTHON" -m pip install --quiet --requirement "$HERE/requirements.txt"; then
    die "pip install failed. This server's only dependency is the MCP SDK, so if
        this is a network or proxy problem nothing else will need installing."
fi

# Failing here rather than at server start-up means a broken environment is
# reported once, at the point the user asked for it, with the real error.
if ! "$PYTHON" -c "import mcp; print('mcp', getattr(mcp, '__version__', 'ok'))" >&2; then
    die "the venv exists but 'import mcp' fails"
fi

# ---------------------------------------------------------------------------
# Register with VS Code
# ---------------------------------------------------------------------------
# A venv keeps its interpreter in bin/ here and Scripts/ on Windows, and the MCP
# configuration schema has no per-platform conditional, so .vscode/mcp.json
# cannot be committed with a value that works on both.  It is written from here
# instead, by whichever platform just built the venv, and kept out of source
# control.  Not fatal if it fails: the server is usable from run.sh either way.
if ! "$PYTHON" "$HERE/write_mcp_config.py"; then
    warn "the venv is ready, but registering it with VS Code failed; point the"
    warn "command in .vscode/mcp.json at $PYTHON by hand if the tools do not appear"
fi

note ""
note "ready. python: $PYTHON"
note ""
note "Try it without an MCP host:"
note "    $HERE/run.sh --selftest"
note "    $HERE/run.sh --list"
note "    $HERE/run.sh --capture-process firefox --out /tmp/shot.png"
note ""
note "In VS Code, reload the window to pick up the registration."
