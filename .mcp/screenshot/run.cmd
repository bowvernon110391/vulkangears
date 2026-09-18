@echo off
rem ---------------------------------------------------------------------------
rem Launcher for the screenshot MCP server.
rem
rem Why this exists: it builds the virtualenv on first use, so a fresh clone
rem needs no setup step at all. Pointing an MCP `command` straight at
rem .venv\Scripts\python.exe works too, but fails with an unhelpful error if the
rem venv has not been built; this recovers instead.
rem
rem Also handy by hand, from the repository root:
rem     .mcp\screenshot\run.cmd --selftest
rem     .mcp\screenshot\run.cmd --list
rem     .mcp\screenshot\run.cmd --capture-process vulkangears --out shot.png
rem
rem IMPORTANT: stdout carries the MCP JSON-RPC stream when this runs as a
rem server. Nothing on the bootstrap path may write to it, which is why all of
rem the progress chatter below is redirected to stderr (1>&2). A stray line on
rem stdout corrupts the protocol and the host drops the connection.
rem ---------------------------------------------------------------------------
setlocal

set "HERE=%~dp0"
set "VENV=%HERE%.venv"
set "PY=%VENV%\Scripts\python.exe"

if not exist "%PY%" call :bootstrap
if not exist "%PY%" (
    echo [screenshot] could not build the virtualenv 1>&2
    exit /b 1
)

"%PY%" "%HERE%server.py" %*
exit /b %ERRORLEVEL%


:bootstrap
    echo [screenshot] building the virtualenv -- this happens once per clone 1>&2

    rem Prefer the `py` launcher: `python` on PATH can be a Microsoft Store stub
    rem or a 2.x from a compiler toolchain.
    set "LAUNCHER=python"
    where py >nul 2>nul && set "LAUNCHER=py -3"

    %LAUNCHER% -m venv "%VENV%" 1>&2
    if not exist "%PY%" (
        echo [screenshot] venv creation failed; falling back is not possible 1>&2
        exit /b 1
    )

    "%PY%" -m pip install --quiet --upgrade pip 1>&2
    "%PY%" -m pip install --quiet --requirement "%HERE%requirements.txt" 1>&2
    if errorlevel 1 (
        echo [screenshot] pip install failed 1>&2
        exit /b 1
    )

    echo [screenshot] ready 1>&2
    exit /b 0
