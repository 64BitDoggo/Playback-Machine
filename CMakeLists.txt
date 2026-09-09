# Playback Machine — CMake build (native Windows: MSVC or MinGW).
#
# Configure (MinGW example):
#   cmake -G "MinGW Makefiles" -DFFMPEG_ROOT=C:/ffmpeg-win64 -DCMAKE_BUILD_TYPE=Release .
#   cmake --build .
#
# FFMPEG_ROOT must point at a FFmpeg win64 build with include/ and lib/.
# For a cross-build from Linux, use scripts/build_cross.sh (mingw-w64) instead.

cmake_minimum_required(VERSION 3.16)
project(PlaybackMachine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT FFMPEG_ROOT)
  message(FATAL_ERROR "Set -DFFMPEG_ROOT=<ffmpeg win64 build dir with include/ and lib/>")
endif()

set(SOURCES
    src/main_win.cpp
    src/gui_win.cpp
    src/engine.cpp
    src/ffdyn.cpp
    src/app.rc
)

add_executable(PlaybackMachine WIN32 ${SOURCES})

target_include_directories(PlaybackMachine PRIVATE "${FFMPEG_ROOT}/include")
target_link_directories(PlaybackMachine PRIVATE "${FFMPEG_ROOT}/lib")

target_compile_definitions(PlaybackMachine PRIVATE UNICODE=_UNICODE)

# FFmpeg shared libs.
target_link_libraries(PlaybackMachine PRIVATE
    avformat avcodec avutil swscale swresample
    ole32 winmm opengl32 comctl32 comdlg32 gdi32 user32 shell32
)

if(MSVC)
  target_compile_options(PlaybackMachine PRIVATE /O2 /utf-8 /W3)
  target_link_options(PlaybackMachine PRIVATE /SUBSYSTEM:WINDOWS)
  # Embed the manifest + icon via the .rc (already in SOURCES).
else()
  target_compile_options(PlaybackMachine PRIVATE -O2 -Wall)
endif()

# Bundle the FFmpeg DLLs next to the exe.
add_custom_command(TARGET PlaybackMachine POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${FFMPEG_ROOT}/bin" "$<TARGET_FILE_DIR:PlaybackMachine>"
  COMMENT "Bundling FFmpeg DLLs"
  VERBATIM)

set_target_properties(PlaybackMachine PROPERTIES
  OUTPUT_NAME "PlaybackMachine"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/dist/PlaybackMachine")
