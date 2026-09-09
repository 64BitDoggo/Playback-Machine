# Playback Machine — native Windows build (MSVC).
#
# Builds PlaybackMachine.exe on a Windows machine and bundles the FFmpeg
# DLLs next to it.
#
# Usage (PowerShell, from the repo root):
#   .\scripts\build_windows.ps1 -FFmpegRoot C:\ffmpeg-win64
#   .\scripts\build_windows.ps1 -FFmpegRoot C:\ffmpeg-win64 -Static
#
# -FFmpegRoot : a FFmpeg win64 build folder with include\ and lib\
#               (e.g. an extracted FFmpeg-Builds win64 gpl-shared zip).
# -Static     : link the FFmpeg static libs (lib\*.lib) instead of the shared
#               DLLs. No DLLs are bundled in this case.
#
# Requires the Visual Studio 2019/2022 "Desktop development with C++"
# workload (provides cl.exe, link.exe, rc.exe). Run it from a "x64 Native
# Tools Command Prompt" so cl/link/rc are on PATH, or set $env:Path.

param(
    [Parameter(Mandatory=$true)][string]$FFmpegRoot,
    [switch]$Static
)

$ErrorActionPreference = "Stop"
$Root    = Split-Path -Parent $PSScriptRoot
# app.rc references resources/* relative to the repo root, so run from there.
Set-Location $Root
$OutDir  = Join-Path $Root "dist\PlaybackMachine"
$Sources = @("main_win.cpp","gui_win.cpp","engine.cpp","ffdyn.cpp") |
           ForEach-Object { Join-Path $Root "src\$_" }

if (-not (Test-Path (Join-Path $FFmpegRoot "include"))) {
    throw "FFmpegRoot\include not found: $FFmpegRoot"
}
if (-not (Test-Path (Join-Path $FFmpegRoot "lib"))) {
    throw "FFmpegRoot\lib not found: $FFmpegRoot"
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw "cl.exe not found. Run this from a VS 'x64 Native Tools Command Prompt'."
}

# Remove any previous output.
if (Test-Path $OutDir) { Remove-Item -Recurse -Force $OutDir }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$Exe = Join-Path $OutDir "PlaybackMachine.exe"
$Inc = Join-Path $FFmpegRoot "include"
$Lib = Join-Path $FFmpegRoot "lib"

$SystemLibs = @("ole32.lib","winmm.lib","opengl32.lib","comctl32.lib",
                "comdlg32.lib","gdi32.lib","user32.lib","shell32.lib")
$FfLibs = @("avformat.lib","avcodec.lib","avutil.lib","swscale.lib","swresample.lib")

Write-Host "==> Building with MSVC (cl/link) ..."

# Compile the resources.
& rc.exe /nologo /fo (Join-Path $OutDir "app.res") (Join-Path $Root "src\app.rc")
if ($LASTEXITCODE -ne 0) { throw "rc failed" }

$objFiles = @()
foreach ($s in $Sources) {
    $obj = Join-Path $OutDir ((Split-Path $s -Leaf) -replace "\.cpp$",".obj")
    & cl.exe /nologo /std:c++17 /O2 /EHsc /DUNICODE /D_UNICODE "/I$Inc" /c $s /Fo$obj
    if ($LASTEXITCODE -ne 0) { throw "cl failed on $s" }
    $objFiles += $obj
}

$inputs = @((Join-Path $OutDir "app.res")) + $objFiles
& link.exe /nologo /SUBSYSTEM:WINDOWS /OUT:$Exe `
    "-LIBPATH:$Lib" $inputs @FfLibs @SystemLibs
if ($LASTEXITCODE -ne 0) { throw "link failed" }

# Bundle the FFmpeg DLLs (shared build only).
if (-not $Static) {
    $dllDir = if (Test-Path (Join-Path $FFmpegRoot "bin")) { Join-Path $FFmpegRoot "bin" } else { $Lib }
    if (Test-Path (Join-Path $dllDir "*.dll")) {
        Write-Host "==> Bundling FFmpeg DLLs from $dllDir ..."
        Copy-Item (Join-Path $dllDir "*.dll") -Destination $OutDir -Force
    } else {
        Write-Host "note: no FFmpeg DLLs to bundle."
    }
}

Write-Host ""
Write-Host "Done: $Exe"
Get-ChildItem $OutDir | Format-Table Name, Length -AutoSize
