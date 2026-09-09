// Playback Machine — OpenGL video renderer (Windows). See gui_win.h.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <cmath>
#include "gui_win.h"

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace gui {
namespace {

HWND   g_hwnd = nullptr;
HGLRC  g_rc = nullptr;
GLuint g_tex = 0;
int    g_texW = 0, g_texH = 0;

void makeCurrent() {
    if (!g_hwnd || !g_rc) return;
    HDC wdc = GetDC(g_hwnd);
    wglMakeCurrent(wdc, g_rc);
}

void setupView(int cw, int ch) {
    glViewport(0, 0, cw, ch);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    // (0,0) top-left, +x right, +y down — matches window pixel space.
    glOrtho(0, (GLdouble)cw, 0, (GLdouble)ch, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

void drawLetterboxed(const uint8_t *rgba, int fw, int fh, int cw, int ch) {
    if (fw <= 0 || fh <= 0) return;
    // Upload texture (reallocate only when the frame size changes).
    glBindTexture(GL_TEXTURE_2D, g_tex);
    if (fw != g_texW || fh != g_texH) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fw, fh, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, rgba);
        g_texW = fw; g_texH = fh;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fw, fh, GL_RGBA,
                        GL_UNSIGNED_BYTE, rgba);
    }
    // Largest rectangle with the frame's aspect ratio that fits the window.
    double s = std::min((double)cw / fw, (double)ch / fh);
    double dw = fw * s, dh = fh * s;
    double x0 = (cw - dw) * 0.5, y0 = (ch - dh) * 0.5;
    // Frame is top-down RGBA, so v=1 at the top row.
    glBegin(GL_QUADS);
    glTexCoord2d(0.0, 1.0); glVertex2d(x0,        y0);
    glTexCoord2d(1.0, 1.0); glVertex2d(x0 + dw,   y0);
    glTexCoord2d(1.0, 0.0); glVertex2d(x0 + dw,   y0 + dh);
    glTexCoord2d(0.0, 0.0); glVertex2d(x0,        y0 + dh);
    glEnd();
}

} // namespace

bool glInit(HWND hwnd) {
    g_hwnd = hwnd;
    HDC wdc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;
    int pf = ChoosePixelFormat(wdc, &pfd);
    if (!pf || !SetPixelFormat(wdc, pf, &pfd)) {
        ReleaseDC(hwnd, wdc);
        return false;
    }
    HGLRC rc = wglCreateContext(wdc);
    if (!rc) {
        ReleaseDC(hwnd, wdc);
        return false;
    }
    if (!wglMakeCurrent(wdc, rc)) {
        wglDeleteContext(rc);
        ReleaseDC(hwnd, wdc);
        return false;
    }
    ReleaseDC(hwnd, wdc);
    g_rc = rc;
    glClearColor(0, 0, 0, 1);
    glEnable(GL_TEXTURE_2D);
    glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return true;
}

void glShutdown() {
    if (g_rc) {
        makeCurrent();
        if (g_tex) {
            glDeleteTextures(1, &g_tex);
            g_tex = 0;
        }
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(g_rc);
        g_rc = nullptr;
    }
    g_hwnd = nullptr;
    g_texW = g_texH = 0;
}

void glDrawFrame(const uint8_t *rgba, int w, int h) {
    if (!g_rc || !rgba) return;
    makeCurrent();
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (cw < 1 || ch < 1) {
        wglMakeCurrent(nullptr, nullptr);
        return;
    }
    setupView(cw, ch);
    glClear(GL_COLOR_BUFFER_BIT);
    drawLetterboxed(rgba, w, h, cw, ch);
    HDC wdc = GetDC(g_hwnd);
    SwapBuffers(wdc);
    ReleaseDC(g_hwnd, wdc);
    wglMakeCurrent(nullptr, nullptr);
}

void glClear() {
    if (!g_rc) return;
    makeCurrent();
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (cw < 1 || ch < 1) {
        wglMakeCurrent(nullptr, nullptr);
        return;
    }
    glViewport(0, 0, cw, ch);
    glClear(GL_COLOR_BUFFER_BIT);
    HDC wdc = GetDC(g_hwnd);
    SwapBuffers(wdc);
    ReleaseDC(g_hwnd, wdc);
    wglMakeCurrent(nullptr, nullptr);
}

} // namespace gui
