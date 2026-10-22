# 只读探测：TTVoice 主窗口信息 + UIA 语义树可达性
$ErrorActionPreference = 'SilentlyContinue'

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class W32 {
    public delegate bool EnumWindowsProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    public struct RECT { public int L, T, R, B; }
}
"@

$targetPids = (Get-Process -Name TTVoice -ErrorAction SilentlyContinue).Id
if (-not $targetPids) { Write-Output "TTVoice 进程未运行"; exit 0 }
Write-Output "TTVoice PID(s): $($targetPids -join ',')"

$wins = New-Object System.Collections.ArrayList
$cb = [W32+EnumWindowsProc]{
    param($h, $l)
    $pid2 = 0
    [void][W32]::GetWindowThreadProcessId($h, [ref]$pid2)
    if ($targetPids -contains $pid2) {
        $sb = New-Object System.Text.StringBuilder 512
        [void][W32]::GetWindowText($h, $sb, 512)
        $title = $sb.ToString()
        $cb2 = New-Object System.Text.StringBuilder 512
        [void][W32]::GetClassName($h, $cb2, 512)
        $r = New-Object W32+RECT
        [void][W32]::GetWindowRect($h, [ref]$r)
        $dpi = [W32]::GetDpiForWindow($h)
        [void]$wins.Add([pscustomobject]@{
            HWND = $h; Title = $title; Class = $cb2.ToString()
            Rect = "$($r.L),$($r.T),$($r.R),$($r.B)"
            Width = $r.R - $r.L; Height = $r.B - $r.T
            DPI = $dpi; Visible = [W32]::IsWindowVisible($h); Minimized = [W32]::IsIconic($h)
            Parent = [W32]::GetParent($h)
        })
    }
    return $true
}
[void][W32]::EnumWindows($cb, [IntPtr]::Zero)

Write-Output "=== Windows ==="
$wins | Format-Table HWND, Class, Width, Height, DPI, Visible, Minimized, Parent, Title -AutoSize | Out-String -Width 300

# 选面积最大且可见的窗口作为主窗口
$main = $wins | Where-Object { $_.Visible -and -not $_.Minimized -and $_.Width -gt 300 } |
        Sort-Object { $_.Width * $_.Height } -Descending | Select-Object -First 1
if (-not $main) { Write-Output "未找到可见主窗口"; exit 0 }

Write-Output "=== UIA Probe on HWND=$($main.HWND) ==="
$ae = [System.Windows.Automation.AutomationElement]::FromHandle([IntPtr]$main.HWND)
if (-not $ae) { Write-Output "UIA: FromHandle 返回空"; exit 0 }

$all = $ae.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                   [System.Windows.Automation.Condition]::TrueCondition)
Write-Output "UIA Descendants Count: $($all.Count)"

$i = 0
foreach ($e in $all) {
    $i++
    if ($i -gt 100) { break }
    $ci = $e.Current
    $r = $ci.BoundingRectangle
    Write-Output ("[{0}] {1} | cls='{2}' | name='{3}' | id='{4}' | rect={5},{6},{7}x{8}" -f `
        $i, $ci.ControlType.ProgrammaticName, $ci.ClassName, $ci.Name, $ci.AutomationId, `
        [int]$r.X, [int]$r.Y, [int]$r.Width, [int]$r.Height)
}
Write-Output "=== UIA Probe End ==="
