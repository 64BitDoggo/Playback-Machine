#!/bin/bash
# Playback Machine — cross-build a Windows x86_64 .exe from Linux/macOS.
#
# Uses a mingw-w64 C++ cross-compiler (it ships the C++ standard library,
# which is required). On Debian/Ubuntu:
#     sudo apt-get install g++-mingw-w64-x86-64
# (this provides x86_64-w64-mingw32-g++ and x86_64-w64-mingw32-windres)
#
# Requires a FFmpeg win64 build with include/ and lib/:
#     - SHARED: an extracted FFmpeg-Builds win64-gpl-shared zip
#               (bin/*.dll + lib/*.lib import libs + include/).
#     - STATIC: a FFmpeg built with --enable-static --disable-shared.
#   Set FFMPEG_ROOT to that folder.
#
# Usage:
#   FFMPEG_ROOT=/path/to/ffmpeg-win64 ./scripts/build_cross.sh
#   FFMPEG_ROOT=/path/to/ffmpeg-win64 ./scripts/build_cross.sh --static

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

: "${FFMPEG_ROOT:?Set FFMPEG_ROOT to a FFmpeg win64 build (with include/ and lib/)}"
FF_INC="$FFMPEG_ROOT/include"
FF_LIB="$FFMPEG_ROOT/lib"
[ -d "$FF_INC" ] || { echo "FFMPEG_ROOT/include not found: $FF_INC"; exit 1; }
[ -d "$FF_LIB" ] || { echo "FFMPEG_ROOT/lib not found: $FF_LIB"; exit 1; }

# --- find the mingw-w64 C++ cross-compiler ---
CXX=""
for c in x86_64-w64-mingw32-g++-posix x86_64-w64-mingw32-g++ \
         g++-mingw-w64-x86-64-posix g++-mingw-w64-x86-64; do
  if command -v "$c" >/dev/null 2>&1; then CXX="$c"; break; fi
done
if [ -z "$CXX" ]; then
  echo "error: no mingw-w64 g++ found."
  echo "install it, e.g.: sudo apt-get install g++-mingw-w64-x86-64"
  exit 1
fi
echo "==> Using $CXX ($($CXX --version | head -1))"

# --- windres for the .rc (icon / manifest / version) ---
WINDRES=""
for w in x86_64-w64-mingw32-windres windres; do
  if command -v "$w" >/dev/null 2>&1; then WINDRES="$w"; break; fi
done

OUT="$ROOT/dist/PlaybackMachine"
rm -rf "$OUT"; mkdir -p "$OUT"

# --- locate the FFmpeg libs (shared import libs or static archives) ---
declare -a FFLIBS=()
add_ff() {
  for cand in "$FF_LIB/lib$1.lib" "$FF_LIB/$1.lib" \
              "$FF_LIB/lib${1}win.lib" "$FF_LIB/${1}win.lib" \
              "$FF_LIB/lib$1.a" "$FF_LIB/$1.a"; do
    if [ -f "$cand" ]; then FFLIBS+=("$cand"); return; fi
  done
  echo "warning: could not find FFmpeg lib for '$1' in $FF_LIB"
}
add_ff avformat
add_ff avcodec
add_ff swscale
add_ff swresample
add_ff avutil

CXXFLAGS="-O2 -std=c++17 -I$FF_INC"
LDFLAGS="-L$FF_LIB -static-libstdc++ -static-libgcc \
  -lole32 -lwinmm -lopengl32 -lcomctl32 -lcomdlg32 \
  -lgdi32 -luser32 -lshell32 -mwindows"

# compile the resources into an object (if windres is available)
RES_OBJ=""
if [ -n "$WINDRES" ]; then
  echo "==> Compiling resources (app.rc) ..."
  # app.rc references resources/app.ico and resources/app.manifest relative to
  # the repo root, so run windres from the root.
  "$WINDRES" -i src/app.rc -O coff -o "$OUT/app_res.o" && RES_OBJ="$OUT/app_res.o"
fi

echo "==> Compiling & linking PlaybackMachine.exe ..."
# shellcheck disable=SC2086
"$CXX" $CXXFLAGS \
  src/main_win.cpp src/gui_win.cpp src/engine.cpp src/ffdyn.cpp \
  ${RES_OBJ:+"$RES_OBJ"} \
  "${FFLIBS[@]}" $LDFLAGS \
  -o "$OUT/PlaybackMachine.exe"

# --- bundle the FFmpeg DLLs (shared build) ---
if ls "$FF_LIB"/*.dll >/dev/null 2>&1; then
  echo "==> Bundling FFmpeg DLLs from $FF_LIB ..."
  cp -f "$FF_LIB"/*.dll "$OUT"/
elif [ -d "$FFMPEG_ROOT/bin" ] && ls "$FFMPEG_ROOT/bin"/*.dll >/dev/null 2>&1; then
  echo "==> Bundling FFmpeg DLLs from $FFMPEG_ROOT/bin ..."
  cp -f "$FFMPEG_ROOT/bin"/*.dll "$OUT"/
else
  echo "note: no FFmpeg DLLs to bundle (static build); exe is self-contained."
fi

echo "==> Done."
ls -la "$OUT"
echo
echo "Executable: $OUT/PlaybackMachine.exe"
echo "Copy the whole PlaybackMachine/ folder to a Windows machine and run it."
