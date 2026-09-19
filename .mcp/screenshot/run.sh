#!/usr/bin/env bash
#
# Launcher for the screenshot MCP server.
#
# Why this exists: it builds the virtualenv on first use, so a fresh clone needs
# no setup step at all. Pointing an MCP `command` straight at .venv/bin/python
# works too, but fails with an unhelpful error if the venv has not been built;
# this recovers instead.
#
# Also handy by hand, from the repository root:
#     .mcp/screenshot/run.sh --selftest
#     .mcp/screenshot/run.sh --list
#     .mcp/screenshot/run.sh --capture-process firefox --out shot.png
#
# IMPORTANT: stdout carries the MCP JSON-RPC stream when this runs as a server.
# Nothing on the bootstrap path may write to it, which is why the setup script
# below is run with its output redirected to stderr. A stray line on stdout
# corrupts the protocol and the host drops the connection.

set -euo pipefail

HERE="$(cd -P "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON="$HERE/.venv/bin/python"

if [ ! -x "$PYTHON" ]; then
    printf '[screenshot] building the virtualenv -- this happens once per clone\n' >&2
    # setup.sh already writes everything it says to stderr; the redirect is here
    # so that stays true even if someone adds a stray echo to it later.
    if ! bash "$HERE/setup.sh" >&2; then
        printf '[screenshot] could not build the virtualenv\n' >&2
        exit 1
    fi
fi

if [ ! -x "$PYTHON" ]; then
    printf '[screenshot] no interpreter at %s\n' "$PYTHON" >&2
    exit 1
fi

exec "$PYTHON" "$HERE/server.py" "$@"
