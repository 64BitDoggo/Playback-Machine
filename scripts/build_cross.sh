#!/bin/bash
# Playback Machine — cross-build a Windows x86_64 .exe from Linux using zig.
#
# Requires:
#   * zig (>= 0.13) with the x86_64-windows-gnu target, and
#   * a FFmpeg win64 build with include/ and lib/.
#     Set FFMPEG_ROOT to that folder.
#
#   - For a SHARED build (BtbN win64-gpl-shared zip): the lib/ import libs and
#     the DLLs are bundled automatically.
#   - For a STATIC build: point FFMPEG_ROOT at the prefix (lib/ has the .libs).
#
# Usage:
#   FFMPEG_ROOT=/path/to/ffmpeg-win64 ./scripts/build_cross.sh
#   FFMPEG_ROOT=/path/to/ffmpeg-win64 ./scripts/build_cross.sh --static

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

: "${FFMPEG_ROOT:?Set FFMPEG_ROOT to the FFmpeg win64 build (with include/ and lib/)}"
FF_INC="$FFMPEG_ROOT/include"
FF_LIB="$FFMPEG_ROOT/lib"
[ -d "$FF_INC" ] || { echo "FFMPEG_ROOT/include not found: $FF_INC"; exit 1; }
[ -d "$FF_LIB" ] || { echo "FFMPEG_ROOT/lib not found: $FF_LIB"; exit 1; }

STATIC=0
[ "${1:-}" = "--static" ] && STATIC=1

OUT="$ROOT/dist/PlaybackMachine"
rm -rf "$OUT"
mkdir -p "$OUT"

# Pick the FFmpeg import/static libs. Shared builds (BtbN) name them
# lib*.lib; a --build-suffix=win static build names them *win.lib.
declare -a FFLIBS=()
add_ff() {
  for cand in "$FF_LIB/lib$1.lib" "$FF_LIB/$1.lib" \
              "$FF_LIB/lib${1}win.lib" "$FF_LIB/${1}win.lib" \
              "$FF_LIB/lib$1.a" "$FF_LIB/$1.a"; do
    if [ -f "$cand" ]; then FFLIBS+=("$cand"); return; fi
  done
  echo "warning: could not find FFmpeg lib for '$1' in $FF_LIB"; return
}
add_ff avformat
add_ff avcodec
add_ff swscale
add_ff swresample
add_ff avutil

echo "==> Compiling PlaybackMachine.exe (x86_64-windows) ..."
zig cc -target x86_64-windows-gnu -O2 \
    -I "$FF_INC" \
    src/main_win.cpp \
    src/gui_win.cpp \
    src/engine.cpp \
    src/ffdyn.cpp \
    src/app.rc \
    "${FFLIBS[@]}" \
    -lole32 -lwinmm -lopengl32 -lcomctl32 -lcomdlg32 \
    -lgdi32 -luser32 -lshell32 \
    -mwindows \
    -o "$OUT/PlaybackMachine.exe"

if [ "$STATIC" -eq 0 ]; then
  echo "==> Bundling FFmpeg DLLs ..."
  if [ -d "$FFMPEG_ROOT/bin" ] && ls "$FFMPEG_ROOT/bin"/*.dll >/dev/null 2>&1; then
    cp -f "$FFMPEG_ROOT/bin"/*.dll "$OUT"/
  elif ls "$FF_LIB"/*.dll >/dev/null 2>&1; then
    cp -f "$FF_LIB"/*.dll "$OUT"/
  else
    echo "note: no FFmpeg DLLs found to bundle (static build?); skipping."
  fi
fi

echo "==> Done."
ls -la "$OUT"
echo
echo "Executable: $OUT/PlaybackMachine.exe"
echo "Copy the whole PlaybackMachine/ folder to a Windows machine and run it."
