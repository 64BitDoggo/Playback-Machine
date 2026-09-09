// Playback Machine — OpenGL video renderer (Windows).
//
// Owns a WGL context on a dedicated child window. Each displayed frame is
// uploaded as an RGBA texture and drawn as a letterboxed quad (black
// bars). Fixed-function pipeline (GL 1.1) so it works with the default
// context on every Windows GPU / driver.

#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>

namespace gui {

// Create the WGL context on hwnd. Returns false on failure.
bool glInit(HWND hwnd);

// Destroy the context (call before the window is destroyed).
void glShutdown();

// Render an RGBA, top-down frame, letterboxed to the window client area.
void glDrawFrame(const uint8_t *rgba, int w, int h);

// Paint the window black (no frame yet / between files).
void glClear();

} // namespace gui
