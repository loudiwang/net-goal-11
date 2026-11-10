param(
    [string]$Action = "home"
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class Win {
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr hWnd, EnumProc cb, IntPtr lParam);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int max);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr hWnd, StringBuilder text, int max);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool SetWindowText(IntPtr hWnd, string text);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

function Get-Text([IntPtr]$h) {
    $sb = New-Object System.Text.StringBuilder 512
    [Win]::GetWindowText($h, $sb, 512) | Out-Null
    return $sb.ToString()
}
function Get-Class([IntPtr]$h) {
    $sb = New-Object System.Text.StringBuilder 256
    [Win]::GetClassName($h, $sb, 256) | Out-Null
    return $sb.ToString()
}
function Get-Children([IntPtr]$parent) {
    $list = New-Object System.Collections.ArrayList
    $cb = [Win+EnumProc]{
        param($h, $l)
        [void]$list.Add($h)
        return $true
    }
    [Win]::EnumChildWindows($parent, $cb, [IntPtr]::Zero) | Out-Null
    return $list
}
function Find-ChildButton([IntPtr]$parent, [string]$text) {
    foreach ($h in (Get-Children $parent)) {
        if ((Get-Class $h) -eq "Button" -and (Get-Text $h) -eq $text) { return $h }
    }
    return [IntPtr]::Zero
}
function Click-Button([IntPtr]$h) {
    [Win]::SendMessage($h, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
}
function Click-At([IntPtr]$h, [int]$x, [int]$y) {
    $lp = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
    [Win]::PostMessage($h, 0x0201, [IntPtr]1, $lp) | Out-Null
    Start-Sleep -Milliseconds 60
    [Win]::PostMessage($h, 0x0202, [IntPtr]0, $lp) | Out-Null
}
function Save-Shot([IntPtr]$h, [string]$path) {
    $r = New-Object Win+RECT
    [Win]::GetWindowRect($h, [ref]$r) | Out-Null
    $w = $r.Right - $r.Left
    $hh = $r.Bottom - $r.Top
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [Win]::PrintWindow($h, $hdc, 2) | Out-Null
    $g.ReleaseHdc($hdc)
    $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host "saved $path ($w x $hh)"
}

$proc = Get-Process "Net Goal 11" -ErrorAction SilentlyContinue | Select-Object -First 1
$login = [IntPtr]::Zero
if ($proc) {
    $h = $proc.MainWindowHandle
    if ((Get-Class $h) -eq "NetGoal11LoginWindow") { $login = $h }
}
if ($login -ne [IntPtr]::Zero) {
    $btn = Find-ChildButton $login "登 录"
    if ($btn -eq [IntPtr]::Zero) { $btn = Find-ChildButton $login "登录" }
    if ($btn -eq [IntPtr]::Zero) {
        foreach ($ch in (Get-Children $login)) {
            if ((Get-Class $ch) -eq "Button") {
                $txt = Get-Text $ch
                if ($txt -match "录" -or $txt -match "½") { $btn = $ch; break }
            }
        }
    }
    Write-Host "login window found, button=$btn"
    if ($btn -ne [IntPtr]::Zero) { Click-Button $btn }
    Start-Sleep -Seconds 3
}

$proc = Get-Process "Net Goal 11" -ErrorAction SilentlyContinue | Select-Object -First 1
$main = [IntPtr]::Zero
if ($proc) {
    $h = $proc.MainWindowHandle
    if ((Get-Class $h) -eq "NetGoal11Client") { $main = $h }
}
if ($main -eq [IntPtr]::Zero) { Write-Host "main window not found"; exit 1 }
[Win]::SetForegroundWindow($main) | Out-Null
Start-Sleep -Milliseconds 500
Write-Host "main window: $main"
$dir = Split-Path -Parent $PSScriptRoot
Save-Shot $main (Join-Path $PSScriptRoot "shot_home.png")
