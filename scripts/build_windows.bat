@echo off
REM ============================================================
REM  Playback Machine - native Windows build (MSVC)
REM
REM  Run this from a Visual Studio "x64 Native Tools Command
REM  Prompt" (so cl.exe, link.exe and rc.exe are on PATH).
REM
REM  Usage:
REM      scripts\build_windows.bat <FFmpegRoot> [static]
REM
REM  <FFmpegRoot>  A FFmpeg win64 build folder containing
REM                include\ and lib\ (e.g. an extracted
REM                FFmpeg-Builds win64-gpl-shared zip).
REM  [static]      Add this word to link the FFmpeg static libs
REM                instead of the shared DLLs (nothing bundled).
REM ============================================================
REM NOTE: delayed expansion is deliberately NOT enabled, so a "!" in a
REM folder name (e.g. "...! Projects...") stays literal and isn't mangled.
setlocal EnableExtensions

if "%~1"=="" (
    echo Usage: build_windows.bat ^<FFmpegRoot^> [static]
    echo   ^<FFmpegRoot^> must contain include\ and lib\
    exit /b 1
)
set "FF=%~1"
set "STATIC=0"
if /I "%~2"=="static" set "STATIC=1"

REM Resolve the repo root (parent of this scripts\ folder) as a clean,
REM fully-qualified path - works even when the path has spaces or "!".
cd /d "%~dp0.."
for %%I in (.) do set "ROOT=%%~fI"

set "OUT=%ROOT%\dist\PlaybackMachine"
set "EXE=%OUT%\PlaybackMachine.exe"
set "INC=%FF%\include"
set "LIB=%FF%\lib"

if not exist "%INC%" (
    echo [error] FFmpegRoot\include not found: %FF%
    exit /b 1
)
if not exist "%LIB%" (
    echo [error] FFmpegRoot\lib not found: %FF%
    exit /b 1
)
where cl.exe >nul 2>nul
if errorlevel 1 (
    echo [error] cl.exe not found. Run this from a VS "x64 Native Tools Command Prompt".
    exit /b 1
)

REM We're already in the repo root (cd'd above); app.rc's resources\* paths
REM resolve from here.
cd /d "%ROOT%"

if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%"

echo == [1/4] Compiling resources (app.rc) ==
rc.exe /nologo /fo "%OUT%\app.res" "%ROOT%\src\app.rc"
if errorlevel 1 ( echo [error] rc failed & exit /b 1 )

echo == [2/4] Compiling C++ sources ==
for %%s in (main_win gui_win engine ffdyn) do (
    echo     cl %%s.cpp
    cl.exe /nologo /O2 /EHsc /DUNICODE /D_UNICODE /I"%INC%" /c "%ROOT%\src\%%s.cpp" /Fo"%OUT%\%%s.obj"
    if errorlevel 1 ( echo [error] cl failed on %%s.cpp & exit /b 1 )
)

echo == [3/4] Linking PlaybackMachine.exe ==
link.exe /nologo /SUBSYSTEM:WINDOWS /OUT:"%EXE%" -LIBPATH:"%LIB%" ^
    "%OUT%\app.res" ^
    "%OUT%\main_win.obj" "%OUT%\gui_win.obj" "%OUT%\engine.obj" "%OUT%\ffdyn.obj" ^
    avformat.lib avcodec.lib avutil.lib swscale.lib swresample.lib ^
    ole32.lib winmm.lib opengl32.lib comctl32.lib comdlg32.lib ^
    gdi32.lib user32.lib shell32.lib
if errorlevel 1 ( echo [error] link failed & exit /b 1 )

echo == [4/4] Bundling FFmpeg DLLs ==
if "%STATIC%"=="0" (
    if exist "%FF%\bin\*.dll" (
        echo     from %FF%\bin
        copy /y "%FF%\bin\*.dll" "%OUT%\" >nul
    ) else if exist "%LIB%\*.dll" (
        echo     from %LIB%
        copy /y "%LIB%\*.dll" "%OUT%\" >nul
    ) else (
        echo     note: no FFmpeg DLLs to bundle (static build); exe is self-contained.
    )
) else (
    echo     skipped (static build).
)

echo.
echo ============================================================
echo  Done.  Executable: %EXE%
echo  The dist\PlaybackMachine folder is portable - copy it
echo  anywhere (or zzip it) and run PlaybackMachine.exe.
echo ============================================================
dir /b "%OUT%"
endlocal
