param(
    [string]$Action = "join",
    [int]$Row = 0
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class UW {
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr hWnd, EnumProc cb, IntPtr lParam);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int max);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr hWnd, StringBuilder text, int max);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool SetWindowText(IntPtr hWnd, string text);
    [DllImport("user32.dll")] public static extern int GetWindowTextLength(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr hWnd, int nIndex);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

[UW]::SetProcessDPIAware() | Out-Null

function Get-Text([IntPtr]$h) {
    $sb = New-Object System.Text.StringBuilder 512
    [UW]::GetWindowText($h, $sb, 512) | Out-Null
    return $sb.ToString()
}
function Get-Class([IntPtr]$h) {
    $sb = New-Object System.Text.StringBuilder 256
    [UW]::GetClassName($h, $sb, 256) | Out-Null
    return $sb.ToString()
}
function Get-Children([IntPtr]$parent) {
    $list = New-Object System.Collections.ArrayList
    $cb = [UW+EnumProc]{ param($h, $l) [void]$list.Add($h); return $true }
    [UW]::EnumChildWindows($parent, $cb, [IntPtr]::Zero) | Out-Null
    return $list
}
function Find-ChildButton([IntPtr]$parent, [string]$text) {
    foreach ($h in (Get-Children $parent)) {
        if ((Get-Class $h) -eq "Button" -and (Get-Text $h) -eq $text) { return $h }
    }
    return [IntPtr]::Zero
}
function Click-Button([IntPtr]$h) {
    [UW]::PostMessage($h, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
}
function Click-At([IntPtr]$h, [int]$x, [int]$y) {
    $lp = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
    [UW]::PostMessage($h, 0x0201, [IntPtr]1, $lp) | Out-Null
    Start-Sleep -Milliseconds 60
    [UW]::PostMessage($h, 0x0202, [IntPtr]0, $lp) | Out-Null
}
function Save-Shot([IntPtr]$h, [string]$path) {
    $r = New-Object UW+RECT
    [UW]::GetWindowRect($h, [ref]$r) | Out-Null
    $w = $r.Right - $r.Left
    $hh = $r.Bottom - $r.Top
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [UW]::PrintWindow($h, $hdc, 2) | Out-Null
    $g.ReleaseHdc($hdc)
    $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host "saved $path ($w x $hh)"
}
function Find-WindowByClass([string]$className) {
    $script:found = [IntPtr]::Zero
    $cb = [UW+EnumProc]{
        param($h, $l)
        if ((Get-Class $h) -eq $className) { $script:found = $h; return $false }
        return $true
    }
    [UW]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    return $script:found
}

$proc = Get-Process "Net Goal 11" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $proc) { Write-Host "client not running"; exit 1 }

$login = [IntPtr]::Zero
$h = $proc.MainWindowHandle
if ((Get-Class $h) -eq "NetGoal11LoginWindow") { $login = $h }
if ($login -ne [IntPtr]::Zero) {
    $btn = Find-ChildButton $login "登 录"
    if ($btn -eq [IntPtr]::Zero) { $btn = Find-ChildButton $login "登录" }
    Write-Host "clicking login ($btn)"
    if ($btn -ne [IntPtr]::Zero) { Click-Button $btn }
    Start-Sleep -Seconds 3
}

$main = Find-WindowByClass "NetGoal11Client"
if ($main -eq [IntPtr]::Zero) { Write-Host "main window not found"; exit 1 }
[UW]::SetForegroundWindow($main) | Out-Null

if ($Action -eq "join") {
    $x = 710
    $y = 232 + $Row * 60 + 26
    Write-Host "joining row $Row at $x,$y"
    Click-At $main $x $y
    Start-Sleep -Seconds 3
    $room = Find-WindowByClass "NetGoal11Room"
    if ($room -eq [IntPtr]::Zero) {
        Write-Host "room window not found"
        Save-Shot $main (Join-Path $PSScriptRoot "shot_join_failed.png")
        exit 1
    }
    [UW]::SetForegroundWindow($room) | Out-Null
    Start-Sleep -Milliseconds 400
    Save-Shot $room (Join-Path $PSScriptRoot "shot_room.png")
} elseif ($Action -eq "opendialog") {
    $btn = Find-ChildButton $main "新建房间"
    Click-Button $btn
    Start-Sleep -Seconds 1
    $dialog = Find-WindowByClass "NetGoal11RoomDialog"
    if ($dialog -eq [IntPtr]::Zero) { Write-Host "dialog not found"; exit 1 }
    $edits = New-Object System.Collections.ArrayList
    foreach ($ch in (Get-Children $dialog)) {
        if ((Get-Class $ch) -eq "Edit") { [void]$edits.Add($ch) }
    }
    Write-Host "dialog=$dialog edits=$($edits.Count)"
    foreach ($e in $edits) { Write-Host "before [$([UW]::GetWindowTextLength($e))] '$((Get-Text $e))'" }
    if ($edits.Count -gt 0) {
        [UW]::SetWindowText($edits[0], "TEST22") | Out-Null
        Write-Host "after0 '$((Get-Text $edits[0]))'"
    }
    if ($edits.Count -gt 1) {
        Write-Host "before1 '$((Get-Text $edits[1]))'"
    }
    Save-Shot $dialog (Join-Path $PSScriptRoot "shot_dialog.png")
} elseif ($Action -eq "kick") {
    $room = Find-WindowByClass "NetGoal11Room"
    if ($room -eq [IntPtr]::Zero) { Write-Host "room window not found"; exit 1 }
    Click-At $room 835 224
    Start-Sleep -Seconds 1
    Save-Shot $room (Join-Path $PSScriptRoot "shot_room_kicked.png")
} elseif ($Action -eq "leave") {
    $room = Find-WindowByClass "NetGoal11Room"
    if ($room -ne [IntPtr]::Zero) {
        [UW]::PostMessage($room, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        Start-Sleep -Seconds 2
    }
    $main = Find-WindowByClass "NetGoal11Client"
    if ($main -ne [IntPtr]::Zero) { Save-Shot $main (Join-Path $PSScriptRoot "shot_home2.png") }
} elseif ($Action -eq "create") {
    $btn = Find-ChildButton $main "新建房间"
    Write-Host "new room button=$btn"
    Click-Button $btn
    Start-Sleep -Seconds 1
    $dialog = Find-WindowByClass "NetGoal11RoomDialog"
    if ($dialog -eq [IntPtr]::Zero) { Write-Host "dialog not found"; exit 1 }
    $edits = New-Object System.Collections.ArrayList
    foreach ($ch in (Get-Children $dialog)) {
        if ((Get-Class $ch) -eq "Edit") { [void]$edits.Add($ch) }
    }
    Write-Host "dialog edits: $($edits.Count)"
    if ($edits.Count -gt 0) { [UW]::SetWindowText($edits[0], "TEST22") }
    if ($edits.Count -gt 1) { [UW]::SetWindowText($edits[1], "") }
    $radio = Find-ChildButton $dialog "2v2"
    if ($radio -ne [IntPtr]::Zero) { Click-Button $radio }
    $ok = Find-ChildButton $dialog "创建"
    Write-Host "create buttons: ok=$ok radio=$radio"
    Click-Button $ok
    Start-Sleep -Seconds 3
    $room = Find-WindowByClass "NetGoal11Room"
    if ($room -eq [IntPtr]::Zero) {
        Write-Host "room window not found after create"
        Save-Shot $main (Join-Path $PSScriptRoot "shot_create_failed.png")
        exit 1
    }
    [UW]::SetForegroundWindow($room) | Out-Null
    Save-Shot $room (Join-Path $PSScriptRoot "shot_room_host1.png")
} elseif ($Action -eq "create2") {
    $btn = Find-ChildButton $main "新建房间"
    Click-Button $btn
    Start-Sleep -Seconds 1
    $dialog = Find-WindowByClass "NetGoal11RoomDialog"
    if ($dialog -eq [IntPtr]::Zero) { Write-Host "dialog not found"; exit 1 }
    $edits = New-Object System.Collections.ArrayList
    foreach ($ch in (Get-Children $dialog)) {
        if ((Get-Class $ch) -eq "Edit") { [void]$edits.Add($ch) }
    }
    Write-Host "edits: $($edits.Count)"
    for ($i=0; $i -lt $edits.Count; $i++) { Write-Host "edit$i before='$((Get-Text $edits[$i]))'" }
    if ($edits.Count -gt 0) { [UW]::SetWindowText($edits[0], "TEST22") | Out-Null }
    if ($edits.Count -gt 1) { [UW]::SetWindowText($edits[1], "") | Out-Null }
    for ($i=0; $i -lt $edits.Count; $i++) { Write-Host "edit$i after='$((Get-Text $edits[$i]))'" }
    $radio = Find-ChildButton $dialog "3v3"
    Write-Host "radio=$radio"
    if ($radio -ne [IntPtr]::Zero) { Click-Button $radio }
    Start-Sleep -Milliseconds 400
    Save-Shot $dialog (Join-Path $PSScriptRoot "shot_dialog3.png")
    $ok = Find-ChildButton $dialog "创建"
    Click-Button $ok
    Start-Sleep -Seconds 3
    $still = Find-WindowByClass "NetGoal11RoomDialog"
    Write-Host "dialog still open=$still"
    $room = Find-WindowByClass "NetGoal11Room"
    if ($room -eq [IntPtr]::Zero) { Write-Host "room window not found after create"; exit 1 }
    [UW]::SetForegroundWindow($room) | Out-Null
    Save-Shot $room (Join-Path $PSScriptRoot "shot_room_host1.png")
} elseif ($Action -eq "closedialog") {
    $dialog = Find-WindowByClass "NetGoal11RoomDialog"
    if ($dialog -ne [IntPtr]::Zero) {
        [UW]::PostMessage($dialog, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        Start-Sleep -Seconds 1
    }
} elseif ($Action -eq "inspectdialog") {
    $dialog = Find-WindowByClass "NetGoal11RoomDialog"
    if ($dialog -eq [IntPtr]::Zero) { Write-Host "dialog not found"; exit 1 }
    foreach ($ch in (Get-Children $dialog)) {
        if ((Get-Class $ch) -eq "Button") {
            $r = New-Object UW+RECT
            [UW]::GetWindowRect($ch, [ref]$r) | Out-Null
            Write-Host "button '$((Get-Text $ch))' rect=$($r.Left),$($r.Top),$($r.Right),$($r.Bottom)"
        }
    }
    $five = Find-ChildButton $dialog "5v5"
    Write-Host "five=$five"
    if ($five -ne [IntPtr]::Zero) { Click-Button $five }
    Start-Sleep -Milliseconds 500
    Save-Shot $dialog (Join-Path $PSScriptRoot "shot_dialog5b.png")
} elseif ($Action -eq "create5") {
    $btn = Find-ChildButton $main "新建房间"
    Click-Button $btn
    Start-Sleep -Seconds 1
    $dialog = Find-WindowByClass "NetGoal11RoomDialog"
    if ($dialog -eq [IntPtr]::Zero) { Write-Host "dialog not found"; exit 1 }
    $radio = Find-ChildButton $dialog "5v5"
    if ($radio -ne [IntPtr]::Zero) { Click-Button $radio }
    Start-Sleep -Milliseconds 300
    Save-Shot $dialog (Join-Path $PSScriptRoot "shot_dialog5.png")
    $ok = Find-ChildButton $dialog "创建"
    Click-Button $ok
    Start-Sleep -Seconds 3
    $room = Find-WindowByClass "NetGoal11Room"
    if ($room -eq [IntPtr]::Zero) { Write-Host "room window not found after create"; exit 1 }
    [UW]::SetForegroundWindow($room) | Out-Null
    Save-Shot $room (Join-Path $PSScriptRoot "shot_room_host5.png")
} elseif ($Action -eq "sendchat") {
    $room = Find-WindowByClass "NetGoal11Room"
    if ($room -eq [IntPtr]::Zero) { Write-Host "room window not found"; exit 1 }
    $input = [IntPtr]::Zero
    foreach ($ch in (Get-Children $room)) {
        if ((Get-Class $ch) -eq "Edit") {
            $style = [UW]::GetWindowLong($ch, -16)
            if (($style -band 0x800) -eq 0) { $input = $ch; break }
        }
    }
    Write-Host "chat input=$input"
    if ($input -eq [IntPtr]::Zero) { exit 1 }
    foreach ($c in "hello".ToCharArray()) {
        [UW]::PostMessage($input, 0x0102, [IntPtr]([int]$c), [IntPtr]1) | Out-Null
    }
    Start-Sleep -Milliseconds 200
    $send = Find-ChildButton $room "发送"
    Write-Host "send=$send"
    if ($send -ne [IntPtr]::Zero) { Click-Button $send }
    Start-Sleep -Seconds 1
    Save-Shot $room (Join-Path $PSScriptRoot "shot_room_chat.png")
} elseif ($Action -eq "start") {
    $room = Find-WindowByClass "NetGoal11Room"
    if ($room -eq [IntPtr]::Zero) { Write-Host "room window not found"; exit 1 }
    Click-At $room 690 624
    Start-Sleep -Seconds 1
    $box = Find-WindowByClass "#32770"
    if ($box -ne [IntPtr]::Zero) {
        Save-Shot $box (Join-Path $PSScriptRoot "shot_start_prompt.png")
        [UW]::PostMessage($box, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    } else {
        Write-Host "no message box"
    }
} elseif ($Action -eq "shot") {
    Save-Shot $main (Join-Path $PSScriptRoot "shot_home.png")
    $room = Find-WindowByClass "NetGoal11Room"
    if ($room -ne [IntPtr]::Zero) { Save-Shot $room (Join-Path $PSScriptRoot "shot_room.png") }
}
