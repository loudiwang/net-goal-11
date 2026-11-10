# Build all native Net Goal 11 binaries with Zig (no Visual Studio required).
# Usage:
#   powershell -ExecutionPolicy Bypass -File .\native\build\build_all.ps1
#   powershell -ExecutionPolicy Bypass -File .\native\build\build_all.ps1 -Zig "C:\path\to\zig.exe"
param(
    [string]$Zig = $env:ZIG,
    [string]$Win7Gcc = $env:WIN7_GCC
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not (Test-Path (Join-Path $root 'native\common\protocol.h'))) {
    $root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}
$native = Join-Path $root 'native'

if (-not $Zig) {
    $candidates = @(
        (Join-Path $env:TEMP 'opencode\zig\zig-x86_64-windows-0.17.0\zig.exe'),
        (Join-Path $env:LOCALAPPDATA 'Temp\opencode\zig\zig-x86_64-windows-0.17.0\zig.exe'),
        (Join-Path $env:TEMP 'opencode\zig\zig013\zig-windows-x86_64-0.13.0\zig.exe'),
        (Join-Path $env:LOCALAPPDATA 'Temp\opencode\zig\zig013\zig-windows-x86_64-0.13.0\zig.exe'),
        'C:\zig\zig.exe'
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) { $Zig = $candidate; break }
    }
}
if (-not $Zig -or -not (Test-Path $Zig)) {
    throw 'zig.exe not found. Set ZIG or pass -Zig C:\path\to\zig.exe'
}
Write-Host "Using Zig: $Zig"

$dist = Join-Path $native 'dist'
$hookOut = Join-Path $dist 'x86'
$clientOut = Join-Path $dist 'client-win-x86'
$serverWinOut = Join-Path $dist 'server-win-x64'
$serverLinuxOut = Join-Path $dist 'server-linux-x64'
foreach ($dir in @($hookOut, $clientOut, $serverWinOut, $serverLinuxOut)) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
}

function Invoke-Zig([string[]]$arguments) {
    & $Zig @arguments
    if ($LASTEXITCODE -ne 0) { throw "zig failed with exit code $LASTEXITCODE" }
}

Write-Host 'Building Net Goal 11.Hook.dll (x86)...'
Invoke-Zig @(
    'c++', '-target', 'x86-windows-gnu', '-O2', '-s', '-shared', '-std=c++17',
    '-D_WIN32_LEAN_AND_MEAN',
    '-o', (Join-Path $hookOut 'Net Goal 11.Hook.dll'),
    (Join-Path $native 'hook\hook_direct.cpp'),
    '-lws2_32', '-liphlpapi', '-luser32', '-ladvapi32', '-lshell32'
)
Remove-Item (Join-Path $hookOut 'hook_direct.lib') -ErrorAction SilentlyContinue

Write-Host 'Building Net Goal 11.exe (x86)...'
Invoke-Zig @(
    'c++', '-target', 'x86-windows-gnu', '-O2', '-s', '-municode',
    '-Wl,--subsystem,windows', '-std=c++17',
    '-D_WIN32_LEAN_AND_MEAN', '-DUNICODE', '-D_UNICODE',
    '-o', (Join-Path $clientOut 'Net Goal 11.exe'),
    (Join-Path $native 'client\client.cpp'),
    (Join-Path $native 'client\login.cpp'),
    '-lws2_32', '-lcomdlg32', '-lcomctl32', '-lshell32', '-luser32', '-lgdi32',
    '-ladvapi32', '-lcrypt32', '-lgdiplus'
)
Remove-Item (Join-Path $clientOut 'Net Goal 11.pdb') -ErrorAction SilentlyContinue

Write-Host 'Building Windows test server (x64)...'
Invoke-Zig @(
    'c++', '-target', 'x86_64-windows-gnu', '-O2', '-s', '-std=c++17',
    '-D_WIN32_LEAN_AND_MEAN',
    '-o', (Join-Path $serverWinOut 'Net Goal 11.Server.exe'),
    (Join-Path $native 'server\server.cpp'),
    '-lws2_32'
)

Write-Host 'Building Linux server (x86_64, static musl)...'
Invoke-Zig @(
    'c++', '-target', 'x86_64-linux-musl', '-O2', '-s', '-static', '-std=c++17',
    '-o', (Join-Path $serverLinuxOut 'Net Goal 11.Server'),
    (Join-Path $native 'server\server.cpp')
)

Copy-Item (Join-Path $hookOut 'Net Goal 11.Hook.dll') (Join-Path $clientOut 'Net Goal 11.Hook.dll') -Force
Copy-Item (Join-Path $native 'client\login_bg.jpg') (Join-Path $clientOut 'login_bg.jpg') -Force
Copy-Item (Join-Path $native 'client\main_banner.jpg') (Join-Path $clientOut 'main_banner.jpg') -Force
Copy-Item (Join-Path $native 'client\app.ico') (Join-Path $clientOut 'app.ico') -Force
Copy-Item (Join-Path $native 'client\client_config.ini') (Join-Path $clientOut 'client_config.ini') -Force
Copy-Item (Join-Path $native 'README.md') (Join-Path $clientOut 'README.txt') -Force -ErrorAction SilentlyContinue
Get-ChildItem $dist -Recurse -File -Include *.pdb,*.lib | Remove-Item -Force -ErrorAction SilentlyContinue

# Embed the shared app.ico into every Windows executable so Explorer, the
# taskbar and Alt+Tab use the same icon even before the app loads app.ico.
if (-not ('IconResourceUpdater' -as [type])) {
    Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class IconResourceUpdater {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern IntPtr BeginUpdateResource(string fileName, bool deleteExisting);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern bool UpdateResource(IntPtr hUpdate, IntPtr type, IntPtr name, ushort language, byte[] data, uint cb);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool EndUpdateResource(IntPtr hUpdate, bool discard);
}
"@
}
function Set-ExeIcon([string]$ExePath, [string]$IcoPath) {
    if (-not (Test-Path $ExePath)) { return }
    $ico = [IO.File]::ReadAllBytes($IcoPath)
    $count = [BitConverter]::ToUInt16($ico, 4)
    $h = [IconResourceUpdater]::BeginUpdateResource($ExePath, $false)
    if ($h -eq [IntPtr]::Zero) {
        throw "Cannot update icon resources in $ExePath (is it running?)"
    }
    $group = New-Object IO.MemoryStream
    $writer = New-Object IO.BinaryWriter($group)
    $writer.Write([uint16]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]$count)
    for ($i = 0; $i -lt $count; ++$i) {
        $entry = 6 + 16 * $i
        $width = $ico[$entry]
        $height = $ico[$entry + 1]
        $colors = $ico[$entry + 2]
        $reserved = $ico[$entry + 3]
        $planes = [BitConverter]::ToUInt16($ico, $entry + 4)
        $bits = [BitConverter]::ToUInt16($ico, $entry + 6)
        $length = [BitConverter]::ToUInt32($ico, $entry + 8)
        $offset = [BitConverter]::ToUInt32($ico, $entry + 12)
        $data = New-Object byte[] $length
        [Array]::Copy($ico, $offset, $data, 0, $length)
        [void][IconResourceUpdater]::UpdateResource($h, [IntPtr]3, [IntPtr]($i + 1), 0x0409, $data, $length)
        $writer.Write([byte]$width)
        $writer.Write([byte]$height)
        $writer.Write([byte]$colors)
        $writer.Write([byte]$reserved)
        $writer.Write([uint16]$planes)
        $writer.Write([uint16]$bits)
        $writer.Write([uint32]$length)
        $writer.Write([uint16]($i + 1))
    }
    $writer.Flush()
    $groupData = $group.ToArray()
    [void][IconResourceUpdater]::UpdateResource($h, [IntPtr]14, [IntPtr]1, 0x0409, $groupData, $groupData.Length)
    [void][IconResourceUpdater]::EndUpdateResource($h, $false)
    $writer.Dispose()
    $group.Dispose()
    Write-Host "Icon embedded: $ExePath"
}
$appIcon = Join-Path $native 'client\app.ico'
Set-ExeIcon (Join-Path $clientOut 'Net Goal 11.exe') $appIcon
Set-ExeIcon (Join-Path $serverWinOut 'Net Goal 11.Server.exe') $appIcon

$clientFiles = @(
    (Join-Path $clientOut 'Net Goal 11.exe'),
    (Join-Path $clientOut 'Net Goal 11.Hook.dll'),
    (Join-Path $clientOut 'login_bg.jpg'),
    (Join-Path $clientOut 'main_banner.jpg'),
    (Join-Path $clientOut 'app.ico'),
    (Join-Path $clientOut 'client_config.ini'),
    (Join-Path $clientOut 'README.txt')
)
$clientZip = Join-Path $dist 'Net Goal 11-client-win-x86.zip'
if (Test-Path $clientZip) { Remove-Item $clientZip -Force }
Compress-Archive -Path $clientFiles -DestinationPath $clientZip -Force

$serverFiles = @(
    (Join-Path $serverLinuxOut 'Net Goal 11.Server'),
    (Join-Path $native 'server\net-goal-11.service'),
    (Join-Path $native 'server\deploy-linux.sh')
)
$serverZip = Join-Path $dist 'Net Goal 11-server-linux-x64.zip'
if (Test-Path $serverZip) { Remove-Item $serverZip -Force }
Compress-Archive -Path $serverFiles -DestinationPath $serverZip -Force

$serverWinFiles = @(
    (Join-Path $serverWinOut 'Net Goal 11.Server.exe')
)
$serverWinZip = Join-Path $dist 'Net Goal 11-server-win-x64.zip'
if (Test-Path $serverWinZip) { Remove-Item $serverWinZip -Force }
Compress-Archive -Path $serverWinFiles -DestinationPath $serverWinZip -Force

# ---------------------------------------------------------------------------
# Optional Windows 7 build.
#
# Zig's bundled mingw-w64 runtime is UCRT based and imports
# GetSystemTimePreciseAsFileTime (Windows 8+), so those binaries do not start
# on a plain Windows 7 SP1.  Passing -Win7Gcc (an i686 mingw-w64 GCC using the
# msvcrt runtime, e.g. winlibs ...msvcrt...) produces a second client that only
# needs msvcrt.dll and APIs present since Windows 7.
# ---------------------------------------------------------------------------
if ($Win7Gcc) {
    if (-not (Test-Path $Win7Gcc)) { throw "Windows 7 g++ not found: $Win7Gcc" }
    $win7Out = Join-Path $dist 'client-win-x86-win7'
    New-Item -ItemType Directory -Force -Path $win7Out | Out-Null
    $win7Flags = @(
        '-O2', '-s', '-std=c++17', '-D_WIN32_LEAN_AND_MEAN', '-DUNICODE', '-D_UNICODE',
        '-D_WIN32_WINNT=0x0601', '-DWINVER=0x0601',
        '-static', '-static-libgcc', '-static-libstdc++',
        '-Wl,--major-os-version,6', '-Wl,--minor-os-version,1',
        '-Wl,--major-subsystem-version,6', '-Wl,--minor-subsystem-version,1'
    )

    Write-Host 'Building Windows 7 Net Goal 11.Hook.dll (x86)...'
    & $Win7Gcc @win7Flags '-shared' '-o' (Join-Path $win7Out 'Net Goal 11.Hook.dll') `
        (Join-Path $native 'hook\hook_direct.cpp') `
        '-lws2_32' '-liphlpapi' '-luser32' '-ladvapi32' '-lshell32'
    if ($LASTEXITCODE -ne 0) { throw "Windows 7 hook build failed with exit code $LASTEXITCODE" }

    Write-Host 'Building Windows 7 Net Goal 11.exe (x86)...'
    & $Win7Gcc @win7Flags '-municode' '-Wl,--subsystem,windows' `
        '-o' (Join-Path $win7Out 'Net Goal 11.exe') `
        (Join-Path $native 'client\client.cpp') `
        (Join-Path $native 'client\login.cpp') `
        '-lws2_32' '-lcomdlg32' '-lcomctl32' '-lshell32' '-luser32' '-lgdi32' `
        '-ladvapi32' '-lcrypt32' '-lgdiplus'
    if ($LASTEXITCODE -ne 0) { throw "Windows 7 client build failed with exit code $LASTEXITCODE" }

    Copy-Item (Join-Path $native 'client\login_bg.jpg') (Join-Path $win7Out 'login_bg.jpg') -Force
    Copy-Item (Join-Path $native 'client\main_banner.jpg') (Join-Path $win7Out 'main_banner.jpg') -Force
    Copy-Item (Join-Path $native 'client\app.ico') (Join-Path $win7Out 'app.ico') -Force
    Copy-Item (Join-Path $native 'client\client_config.ini') (Join-Path $win7Out 'client_config.ini') -Force
    Copy-Item (Join-Path $native 'README.md') (Join-Path $win7Out 'README.txt') -Force -ErrorAction SilentlyContinue
    Set-ExeIcon (Join-Path $win7Out 'Net Goal 11.exe') $appIcon

    $win7Files = @(
        (Join-Path $win7Out 'Net Goal 11.exe'),
        (Join-Path $win7Out 'Net Goal 11.Hook.dll'),
        (Join-Path $win7Out 'login_bg.jpg'),
        (Join-Path $win7Out 'main_banner.jpg'),
        (Join-Path $win7Out 'app.ico'),
        (Join-Path $win7Out 'client_config.ini'),
        (Join-Path $win7Out 'README.txt')
    )
    $win7Zip = Join-Path $dist 'Net Goal 11-client-win-x86-win7.zip'
    if (Test-Path $win7Zip) { Remove-Item $win7Zip -Force }
    Compress-Archive -Path $win7Files -DestinationPath $win7Zip -Force
}

Write-Host ''
Write-Host 'Build complete:'
Get-ChildItem $dist -Recurse -File |
    Where-Object { $_.Name -notlike '*.pdb' -and $_.Name -notlike '*.lib' -and $_.Name -notlike '*.png' } |
    Select-Object FullName, Length |
    Format-Table -AutoSize
