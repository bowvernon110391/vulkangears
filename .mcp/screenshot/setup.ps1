# Create the private virtualenv this MCP server runs in.
#
#   powershell -ExecutionPolicy Bypass -File .mcp/screenshot/setup.ps1
#
# The venv lives inside this folder and is gitignored, so it never ends up in
# the repository but is always beside the code that needs it.  Re-running is
# cheap and safe: it only creates the venv when one is missing.
#
# -Force throws the existing venv away first, which is the fix for a venv that
# has been broken by a Python upgrade or an interrupted install.  That case is
# also caught automatically: a venv that has no interpreter for this platform is
# rebuilt without being asked, which covers a venv left behind by Linux on a
# checkout that is opened on both systems.
#
# It finishes by writing .vscode/mcp.json, so VS Code starts the server with this
# machine's interpreter.  That file is generated rather than committed because the
# venv path differs per platform and the MCP schema has no per-platform
# conditional; see write_mcp_config.py.
param(
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$venv = Join-Path $here ".venv"
$python = Join-Path $venv "Scripts\python.exe"

if ($Force -and (Test-Path $venv)) {
    Write-Host "removing existing venv: $venv"
    Remove-Item -Recurse -Force $venv
}

if (-not (Test-Path $python)) {
    # Only reached when this platform's interpreter is absent, so an existing
    # .venv here was built by the other platform -- or is broken by a Python
    # upgrade, which is the same thing from here: unusable.  It has to go rather
    # than be reused, because venv would create its own Scripts/ (or bin/)
    # alongside the foreign one and rewrite pyvenv.cfg to point at the other
    # system's Python, leaving a hybrid that neither platform can explain.
    # Deleting it also makes a broken venv self-healing, which is what -Force
    # existed to do by hand.
    if (Test-Path $venv) {
        Write-Host "the existing venv was built by another platform or is broken; rebuilding it"
        Remove-Item -Recurse -Force $venv
    }

    # Prefer the `py` launcher so we get a real 3.x interpreter rather than
    # whatever `python` happens to be first on PATH (a Microsoft Store stub, or
    # a 2.7 from a compiler toolchain, both of which exist on real machines).
    $launcher = Get-Command py -ErrorAction SilentlyContinue
    if ($launcher) {
        Write-Host "creating venv with: py -3"
        & py -3 -m venv $venv
    } else {
        Write-Host "creating venv with: python"
        & python -m venv $venv
    }
    if (-not (Test-Path $python)) {
        throw "venv creation failed: $python was not produced"
    }
}

Write-Host "installing dependencies"
& $python -m pip install --upgrade pip --quiet
& $python -m pip install --requirement (Join-Path $here "requirements.txt")
if ($LASTEXITCODE -ne 0) {
    throw "pip install failed (exit $LASTEXITCODE)"
}

# Failing here rather than at server start-up means a broken environment is
# reported once, at the point the user asked for it, with the real error.
& $python -c "import mcp; print('mcp', mcp.__version__ if hasattr(mcp, '__version__') else 'ok')"
if ($LASTEXITCODE -ne 0) {
    throw "the venv exists but 'import mcp' fails"
}

# Register with VS Code.  A venv keeps its interpreter in Scripts/ here and bin/
# on Linux, and the MCP configuration schema has no per-platform conditional, so
# .vscode/mcp.json cannot be committed with a value that works on both.  It is
# written from here instead, by whichever platform just built the venv, and kept
# out of source control.  Not fatal if it fails: run.cmd still works.
& $python (Join-Path $here "write_mcp_config.py")
if ($LASTEXITCODE -ne 0) {
    Write-Warning "the venv is ready, but registering it with VS Code failed; point the command in .vscode/mcp.json at $python by hand if the tools do not appear"
}

Write-Host ""
Write-Host "ready. python: $python"
Write-Host ""
Write-Host "In VS Code, reload the window to pick up the registration."
