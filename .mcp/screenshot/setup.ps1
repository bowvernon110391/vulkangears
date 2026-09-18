# Create the private virtualenv this MCP server runs in.
#
#   powershell -ExecutionPolicy Bypass -File .mcp/screenshot/setup.ps1
#
# The venv lives inside this folder and is gitignored, so it never ends up in
# the repository but is always beside the code that needs it.  Re-running is
# cheap and safe: it only creates the venv when one is missing.
#
# -Force throws the existing venv away first, which is the fix for a venv that
# has been broken by a Python upgrade or an interrupted install.
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

Write-Host ""
Write-Host "ready. python: $python"
