# Capture a top-level window to a PNG, either by owning process name or by
# (partial) window title.
#
#   powershell -File scripts/capture_window.ps1 -Process vulkangears -Out shot.png
#   powershell -File scripts/capture_window.ps1 -Title "Vega 8"    -Out shot.png
#
# Prefer -Process: title matching can hit an unrelated window whose title merely
# mentions the name (e.g. an editor with the project folder open).
# With neither given it captures the whole virtual screen.
param(
    [string]$Process = "",
    [string]$Title = "",
    [string]$Out = "capture.png",
    [int]$DelayMs = 0
)

$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class WinCap {
    [DllImport("user32.dll")] public static extern IntPtr FindWindow(string cls, string name);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern int GetWindowTextLength(IntPtr h);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }

    public static IntPtr Find(string partial) {
        IntPtr found = IntPtr.Zero;
        string want = partial.ToLowerInvariant();
        EnumWindows(delegate(IntPtr h, IntPtr p) {
            if (!IsWindowVisible(h)) return true;
            int len = GetWindowTextLength(h);
            if (len == 0) return true;
            var sb = new System.Text.StringBuilder(len + 1);
            GetWindowText(h, sb, sb.Capacity);
            if (sb.ToString().ToLowerInvariant().Contains(want)) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@

if ($DelayMs -gt 0) { Start-Sleep -Milliseconds $DelayMs }

# Without this, Windows reports window rects in logical pixels under display
# scaling and CopyFromScreen (physical pixels) captures a cropped corner.
[void][WinCap]::SetProcessDPIAware()

if ($Process -ne "") {
    $proc = Get-Process -Name $Process -ErrorAction SilentlyContinue |
            Where-Object { $_.MainWindowHandle -ne 0 } |
            Select-Object -First 1
    if (-not $proc) {
        Write-Error "no process named '$Process' with a visible window"
        exit 1
    }
    $hwnd = $proc.MainWindowHandle
} elseif ($Title -ne "") {
    $hwnd = [WinCap]::Find($Title)
    if ($hwnd -eq [IntPtr]::Zero) {
        Write-Error "no visible window whose title contains '$Title'"
        exit 1
    }
} else {
    $hwnd = [IntPtr]::Zero
}

if ($hwnd -ne [IntPtr]::Zero) {
    if ([WinCap]::IsIconic($hwnd)) { [void][WinCap]::ShowWindow($hwnd, 9) }  # SW_RESTORE
    [void][WinCap]::SetForegroundWindow($hwnd)
    Start-Sleep -Milliseconds 250

    # Prefer the client area so the render surface isn't framed by chrome.
    $rect = New-Object WinCap+RECT
    $origin = New-Object WinCap+POINT
    if ([WinCap]::GetClientRect($hwnd, [ref]$rect)) {
        [void][WinCap]::ClientToScreen($hwnd, [ref]$origin)
        $x = $origin.X; $y = $origin.Y
        $w = $rect.R - $rect.L; $h = $rect.B - $rect.T
    } else {
        [void][WinCap]::GetWindowRect($hwnd, [ref]$rect)
        $x = $rect.L; $y = $rect.T
        $w = $rect.R - $rect.L; $h = $rect.B - $rect.T
    }
} else {
    $vs = [System.Windows.Forms.SystemInformation]::VirtualScreen
    $x = $vs.X; $y = $vs.Y
    $w = $vs.Width; $h = $vs.Height
}

if ($w -le 0 -or $h -le 0) { Write-Error "window has zero size"; exit 1 }

$bmp = New-Object System.Drawing.Bitmap($w, $h)
$gfx = [System.Drawing.Graphics]::FromImage($bmp)
$gfx.CopyFromScreen($x, $y, 0, 0, (New-Object System.Drawing.Size($w, $h)))

$dir = Split-Path -Parent $Out
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }

$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$gfx.Dispose(); $bmp.Dispose()

Write-Host "captured ${w}x${h} -> $Out"
