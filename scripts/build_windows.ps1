# Playback Machine — native Windows build (MSVC or zig).
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
# workload (provides cl.exe, link.exe, rc.exe), OR zig on PATH.

param(
    [Parameter(Mandatory=$true)][string]$FFmpegRoot,
    [switch]$Static,
    [switch]$UseZig
)

$ErrorActionPreference = "Stop"
$Root    = Split-Path -Parent $PSScriptRoot
$OutDir  = Join-Path $Root "dist\PlaybackMachine"
$Sources = @("main_win.cpp","gui_win.cpp","engine.cpp","ffdyn.cpp") |
           ForEach-Object { Join-Path $Root "src\$_" }

if (-not (Test-Path (Join-Path $FFmpegRoot "include"))) {
    throw "FFmpegRoot\include not found: $FFmpegRoot"
}
if (-not (Test-Path (Join-Path $FFmpegRoot "lib"))) {
    throw "FFmpegRoot\lib not found: $FFmpegRoot"
}

# Remove any previous output.
if (Test-Path $OutDir) { Remove-Item -Recurse -Force $OutDir }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$Exe = Join-Path $OutDir "PlaybackMachine.exe"
$Inc = Join-Path $FFmpegRoot "include"
$Lib = Join-Path $FFmpegRoot "lib"

$SystemLibs = @("ole32.lib","winmm.lib","opengl32.lib","comctl32.lib",
                "comdlg32.lib","gdi32.lib","user32.lib","shell32.lib")
$FfLibs = if ($Static) {
    @("avformat.lib","avcodec.lib","avutil.lib","swscale.lib","swresample.lib")
} else {
    # Import libraries for the shared DLLs (BtbN names them lib*.lib).
    @("avformat.lib","avcodec.lib","avutil.lib","swscale.lib","swresample.lib")
}

if ($UseZig -or (Get-Command zig -ErrorAction SilentlyContinue) -and -not (Get-Command cl -ErrorAction SilentlyContinue)) {
    Write-Host "==> Building with zig ..."
    $zigLibs = @("-lavformat","-lavcodec","-lavutil","-lswscale","-lswresample",
                 "-lole32","-lwinmm","-lopengl32","-lcomctl32","-lcomdlg32",
                 "-lgdi32","-luser32","-lshell32")
    & zig cc -target x86_64-windows-msvc -O2 "-I$Inc" `
        @Sources (Join-Path $Root "src\app.rc") "-L$Lib" @zigLibs `
        -mwindows -o $Exe
    if ($LASTEXITCODE -ne 0) { throw "zig build failed" }
}
else {
    Write-Host "==> Building with MSVC (cl/link) ..."
    # Compile the resources.
    & rc.exe /nologo /fo (Join-Path $OutDir "app.res") (Join-Path $Root "src\app.rc")
    if ($LASTEXITCODE -ne 0) { throw "rc failed" }

    $objFiles = @()
    foreach ($s in $Sources) {
        $obj = Join-Path $OutDir ((Split-Path $s -Leaf) -replace "\.cpp$",".obj")
        & cl.exe /nologo /O2 /EHsc /DUNICODE /D_UNICODE "/I$Inc" /c $s /Fo$obj
        if ($LASTEXITCODE -ne 0) { throw "cl failed on $s" }
        $objFiles += $obj
    }

    $allLibs = @((Join-Path $OutDir "app.res")) + $objFiles
    & link.exe /nologo /SUBSYSTEM:WINDOWS /OUT:$Exe `
        "-LIBPATH:$Lib" $allLibs @FfLibs @SystemLibs
    if ($LASTEXITCODE -ne 0) { throw "link failed" }
}

# Bundle the FFmpeg DLLs (shared build only).
if (-not $Static) {
    $dllDir = if (Test-Path (Join-Path $FFmpegRoot "bin")) { Join-Path $FFmpegRoot "bin" } else { $Lib }
    Write-Host "==> Bundling FFmpeg DLLs from $dllDir ..."
    Copy-Item (Join-Path $dllDir "*.dll") -Destination $OutDir -Force
}

Write-Host ""
Write-Host "Done: $Exe"
Get-ChildItem $OutDir | Format-Table Name, Length -AutoSize
