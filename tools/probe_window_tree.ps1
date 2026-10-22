# Read-only probe: TTVoice window tree (children) + styles
$ErrorActionPreference = 'SilentlyContinue'

Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class W2 {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int idx);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    public struct RECT { public int L, T, R, B; }
    public struct POINT { public int X, Y; }
}
"@

$pids = (Get-Process -Name TTVoice).Id
if (-not $pids) { Write-Output 'TTVoice not running'; exit 0 }

$script:main = [IntPtr]::Zero
$cb = [W2+EnumProc]{
    param($h, $l)
    $p = 0; [void][W2]::GetWindowThreadProcessId($h, [ref]$p)
    if ($pids -contains $p) {
        $sb = New-Object System.Text.StringBuilder 256
        [void][W2]::GetClassName($h, $sb, 256)
        if ($sb.ToString() -eq 'FLUTTER_RUNNER_WIN32_WINDOW' -and [W2]::IsWindowVisible($h)) {
            $script:main = $h
        }
    }
    return $true
}
[void][W2]::EnumWindows($cb, [IntPtr]::Zero)
if ($script:main -eq [IntPtr]::Zero) { Write-Output 'main window not found'; exit 0 }

$r = New-Object W2+RECT
[void][W2]::GetWindowRect($script:main, [ref]$r)
$cr = New-Object W2+RECT
[void][W2]::GetClientRect($script:main, [ref]$cr)
$origin = New-Object W2+POINT
[void][W2]::ClientToScreen($script:main, [ref]$origin)
Write-Output ("MAIN hwnd={0} rect={1},{2},{3},{4} client={5}x{6} clientOrigin={7},{8} dpi={9} exstyle=0x{10:X} minimized={11}" -f `
    $script:main, $r.L, $r.T, $r.R, $r.B, ($cr.R-$cr.L), ($cr.B-$cr.T), $origin.X, $origin.Y, [W2]::GetDpiForWindow($script:main), [W2]::GetWindowLong($script:main, -20), [W2]::IsIconic($script:main))

Write-Output '=== Children ==='
$script:idx = 0
$cb2 = [W2+EnumProc]{
    param($h, $l)
    $script:idx++
    $sb = New-Object System.Text.StringBuilder 256
    [void][W2]::GetClassName($h, $sb, 256)
    $t = New-Object System.Text.StringBuilder 128
    [void][W2]::GetWindowText($h, $t, 128)
    $rr = New-Object W2+RECT
    [void][W2]::GetWindowRect($h, [ref]$rr)
    $cc = New-Object W2+RECT
    [void][W2]::GetClientRect($h, [ref]$cc)
    Write-Output ("[{0}] hwnd={1} cls='{2}' title='{3}' rect={4},{5},{6},{7} client={8}x{9} vis={10} style=0x{11:X} ex=0x{12:X}" -f `
        $script:idx, $h, $sb.ToString(), $t.ToString(), $rr.L, $rr.T, $rr.R, $rr.B, ($cc.R-$cc.L), ($cc.B-$cc.T), [W2]::IsWindowVisible($h), [W2]::GetWindowLong($h, -16), [W2]::GetWindowLong($h, -20))
    return $true
}
[void][W2]::EnumChildWindows($script:main, $cb2, [IntPtr]::Zero)
Write-Output '=== End ==='
