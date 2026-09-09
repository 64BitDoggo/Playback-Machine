# Playback Machine

A fast, format-agnostic video player for Windows. One small `.exe` that plays
almost any video or audio file, with frame-accurate stepping, a seek bar,
±5-second skips, adjustable speed, and **true reverse playback**.

Built with **C++17**, a raw **Win32** UI, **OpenGL** video rendering, and
**FFmpeg** for demuxing/decoding — no Qt, no heavy frameworks. It renders
through the desktop compositor, so it works cleanly with **Discord screen
share** (and other desktop-capture tools).

---

## Features

| Feature | How |
| --- | --- |
| Plays almost any format/codec | Anything FFmpeg supports (MP4, MKV, WebM, AVI, MOV, FLV, TS, MPEG, …) |
| Frame stepping | `‹\|` / `\|›` buttons or **Left/Right** arrows (pauses automatically) |
| Seek bar | Drag the slider to scrub; it snaps back while you drag |
| ±5-second skip | `-5s` / `+5s` buttons |
| Speed 0.1× – 8× | `-` / `+` buttons or **Up/Down** (audio is re-clocked, stays in sync) |
| Reverse playback | `Reverse` toggle or **R** (frame history + reversed audio) |
| Loop | `Loop` toggle restarts from the beginning at the end |
| Volume | `Vol-` / `Vol+` |
| Hardware decode | d3d11va / dxva2 / d3d12va auto-detected when available |

**Keyboard:** `Space` play/pause · `←`/`→` step · `↑`/`↓` speed · `R` reverse

---

## How it works

- **`engine.cpp`** — the framework-free playback core. A decode thread
  demuxes + decodes with FFmpeg and maintains a bounded decoded-frame queue
  (back-pressured to ~1 s of lookahead) plus a decoded-frame **history** ring
  used for backward stepping and reverse. A present thread schedules each
  frame — audio-master when a real audio device is present, wall-clock
  otherwise — and converts only the displayed frame to RGBA.
- **`main_win.cpp`** — the Win32 window: a child OpenGL window for video and
  a bottom control bar (Open / Play / step / skip / reverse / loop / speed /
  seek / time). It polls `engine.currentFrame()` and repaints only when a new
  frame is shown.
- **`gui_win.cpp`** — a small fixed-function OpenGL renderer (letterboxed
  RGBA texture, black bars).
- **`audiosink_win.h`** — a `waveOut` PCM sink with a small buffer pool and a
  consumption clock that freezes while paused (so A/V sync holds).
- **`ffdyn.cpp`** — loads the FFmpeg DLLs at runtime (the FFmpeg API is
  accessed through a thin dynamic-binding layer so the player reports a
  clear error if the DLLs are missing).

Reverse playback works by keeping a history of already-decoded frames; when
the history runs out the engine re-seeks backward and re-decodes, so
stepping/reverse never dead-ends.

---

## Building

### On Windows (MSVC)

1. Install the **Visual Studio 2019/2022** “Desktop development with C++”
   workload.
2. Get a FFmpeg **win64** build with `include\` and `lib\`. An easy source is
   a [FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds/releases)
   `win64-gpl-shared` zip (extract it — it has `bin\*.dll`, `lib\*.lib`,
   `include\`).
3. From the repo root:

   ```powershell
   .\scripts\build_windows.ps1 -FFmpegRoot C:\path\to\ffmpeg-win64
   ```

   This produces `dist\PlaybackMachine\PlaybackMachine.exe` with the FFmpeg
   DLLs copied next to it. Add `-Static` to link the static libs instead
   (no DLLs to bundle).

### Cross-building a Windows `.exe` from Linux

Use a **mingw-w64 C++ cross-compiler** (it ships the C++ standard library):

```bash
sudo apt-get install g++-mingw-w64-x86-64     # Debian/Ubuntu
FFMPEG_ROOT=/path/to/ffmpeg-win64 ./scripts/build_cross.sh
```

This produces a self-contained `dist/PlaybackMachine/PlaybackMachine.exe`
(the C++ runtime is statically linked; FFmpeg DLLs are bundled for a shared
FFmpeg). A `CMakeLists.txt` is also provided for native Windows CMake builds
(`-DFFMPEG_ROOT=...`).

> **Note:** the FFmpeg build must include the codecs you want. The bundled
> GPL builds include everything (H.264/HEVC/AV1/VP9/Vorbis/Opus/MP3/… ).

---

## Packaging (installer)

After building, produce an installer with [NSIS](https://nsis.io):

```bash
makensis installer/setup.nsi
```

→ `dist/PlaybackMachine-Setup.exe` (installs to Program Files, adds Start
Menu + desktop shortcuts, and an uninstaller).

A portable distribution is just the `dist\PlaybackMachine\` folder (exe +
FFmpeg DLLs) — copy it anywhere and run it.

---

## Discord screen share

The player renders to a standard composited window, so Discord’s default
hardware-acceleration screen capture picks it up with no special setup.
Share the **Playback Machine window** (or your monitor) and start streaming.

---

## License

The application code is provided as-is. **FFmpeg is GPL** — any build that
links the GPL FFmpeg (or includes its codecs) is subject to the GPL. See the
FFmpeg project for details.
