// Playback Machine — Windows GUI (raw Win32 + OpenGL + waveOut).
//
// A single top-level window hosts:
//   * a child OpenGL window (the video area), and
//   * a bottom control bar (Open / Play / frame-step / ±5s skip / reverse /
//     loop / speed / seek bar / time).
//
// The playback engine (engine.cpp) decodes on its own threads; this file only
// polls currentFrame() to repaint and forwards user input to the engine.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>

#include <cstdio>
#include <string>

#include "audiosink_win.h"
#include "engine.h"
#include "gui_win.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

// ---------------------------------------------------------------------------
// Control ids
// ---------------------------------------------------------------------------
#define IDC_OPEN      1001
#define IDC_PLAY      1002
#define IDC_STEPBACK  1003
#define IDC_STEPFWD   1004
#define IDC_SKIPBACK  1005
#define IDC_SKIPFWD   1006
#define IDC_REVERSE   1007
#define IDC_LOOP      1008
#define IDC_SPDDOWN   1009
#define IDC_SPDUP     1010
#define IDC_SPDLABEL  1011
#define IDC_VOLDOWN   1012
#define IDC_VOLUP     1013
#define IDC_SEEK      1014
#define IDC_TIME      1015
#define IDC_CODEC     1016

static const int kBarHeight = 100;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static PlaybackEngine  g_engine;
static WaveOutSink     g_sink;
static HWND g_hwnd = nullptr;      // main window
static HWND g_hvideo = nullptr;    // OpenGL child
static HWND g_btnOpen = nullptr, g_btnPlay = nullptr, g_btnStepBack = nullptr,
           g_btnStepFwd = nullptr, g_btnSkipBack = nullptr, g_btnSkipFwd = nullptr,
           g_btnReverse = nullptr, g_btnLoop = nullptr, g_btnSpdDown = nullptr,
           g_btnSpdUp = nullptr, g_lblSpd = nullptr, g_btnVolDown = nullptr,
           g_btnVolUp = nullptr, g_seek = nullptr, g_lblTime = nullptr,
           g_lblCodec = nullptr;
static bool    g_seeking = false;  // true while the user drags the seek bar
static DWORD   g_lastTrackingTick = 0;
static int     g_volume = 100;
static uint64_t g_lastToken = 0;
static std::string g_curFile;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static void fmtTime(double sec, char *out, size_t n) {
    if (sec < 0) sec = 0;
    int t = (int)sec;
    int h = t / 3600, m = (t % 3600) / 60, s = t % 60;
    if (h > 0)
        std::snprintf(out, n, "%d:%02d:%02d", h, m, s);
    else
        std::snprintf(out, n, "%d:%02d", m, s);
}

static void setButton(HWND h, const char *text) {
    if (h) SetWindowTextA(h, text);
}

static void updatePlayButton() {
    bool playing = g_engine.state() == PlayerState::Playing;
    setButton(g_btnPlay, playing ? "Pause" : "Play");
}

static void updateSpeedLabel() {
    if (!g_lblSpd) return;
    char b[32];
    std::snprintf(b, sizeof(b), "%.2fx", g_engine.speed());
    setButton(g_lblSpd, b);
}

static void updateCodecLabel() {
    if (!g_lblCodec) return;
    const EngineInfo &inf = g_engine.info();
    char b[160];
    if (inf.hasVideo)
        std::snprintf(b, sizeof(b), "%dx%d %s / %s %s", inf.width, inf.height,
                      inf.videoCodec.c_str(), inf.audioCodec.empty() ? "-" : inf.audioCodec.c_str(),
                      inf.hwAccel ? ("[" + inf.hwName + "]").c_str() : "");
    else
        std::snprintf(b, sizeof(b), "%s", inf.audioCodec.c_str());
    setButton(g_lblCodec, b);
}

static void updateReverseButton() {
    if (g_btnReverse)
        SendMessageA(g_btnReverse, BM_SETCHECK, g_engine.isReverse() ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void updateLoopButton() {
    if (g_btnLoop)
        SendMessageA(g_btnLoop, BM_SETCHECK, g_engine.isLoop() ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void updateUI() {
    double dur = g_engine.duration();
    double pos = g_engine.position();
    if (dur > 0) {
        if (!g_seeking && g_seek)
            SendMessageA(g_seek, TBM_SETPOS, TRUE, (LPARAM)(pos * 1000.0));
        char a[32], b[32], full[96];
        fmtTime(pos, a, sizeof(a));
        fmtTime(dur, b, sizeof(b));
        std::snprintf(full, sizeof(full), "%s / %s", a, b);
        setButton(g_lblTime, full);
    }
    updatePlayButton();
    updateSpeedLabel();
}

static void openFile(const char *path) {
    std::string err;
    g_engine.close();
    g_lastToken = 0;
    if (!g_engine.open(path, &err)) {
        if (!err.empty())
            MessageBoxA(g_hwnd, err.c_str(), "Playback Machine", MB_ICONERROR | MB_OK);
        return;
    }
    g_curFile = path;
    double dur = g_engine.duration();
    if (g_seek) {
        SendMessageA(g_seek, TBM_SETRANGE, TRUE, MAKELPARAM(0, (int)(dur > 0 ? dur * 1000.0 : 60000)));
        SendMessageA(g_seek, TBM_SETPOS, TRUE, 0);
    }
    updateCodecLabel();
    updateUI();
    if (g_hvideo)
        gui::glClear();
    std::string title = std::string("Playback Machine — ") + path;
    SetWindowTextA(g_hwnd, title.c_str());
}

static void seekToTime(double seconds) {
    g_engine.seekTo(seconds);
    if (g_seek)
        SendMessageA(g_seek, TBM_SETPOS, TRUE, (LPARAM)(seconds * 1000.0));
}

// ---------------------------------------------------------------------------
// Control bar layout
// ---------------------------------------------------------------------------
static void layoutControls(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    int barY = ch - kBarHeight;

    // Video child fills the area above the control bar.
    if (g_hvideo)
        MoveWindow(g_hvideo, 0, 0, cw, barY > 0 ? barY : 0, TRUE);

    if (barY < 0)
        return;

    // Row 1: buttons (y = barY + 6, h = 28)
    int y = barY + 6, h = 28, x = 8, w;
    auto place = [&](HWND h, int bw) {
        if (h) MoveWindow(h, x, y, bw, h, TRUE);
        x += bw + 6;
    };
    place(g_btnOpen, 64);
    place(g_btnPlay, 64);
    place(g_btnStepBack, 34);
    place(g_btnStepFwd, 34);
    place(g_btnSkipBack, 48);
    place(g_btnSkipFwd, 48);
    place(g_btnReverse, 64);
    place(g_btnLoop, 48);
    place(g_btnSpdDown, 34);
    place(g_btnSpdUp, 34);
    place(g_lblSpd, 52);
    place(g_btnVolDown, 34);
    place(g_btnVolUp, 34);

    // Row 2: seek bar + time (y = barY + 42, h = 28)
    int y2 = barY + 42;
    if (g_seek)
        MoveWindow(g_seek, 8, y2, 260, 28, TRUE);
    if (g_lblTime)
        MoveWindow(g_lblTime, 278, y2 + 2, 130, 24, TRUE);
    if (g_lblCodec)
        MoveWindow(g_lblCodec, 416, y2 + 2, cw - 416 - 8, 24, TRUE);
}

static HWND makeButton(HWND parent, const char *text, int id, int style) {
    return CreateWindowExA(0, "BUTTON", text, WS_CHILD | WS_VISIBLE | style,
                           0, 0, 10, 10, parent, (HMENU)(INT_PTR)id,
                           GetModuleHandleA(nullptr), nullptr);
}

// ---------------------------------------------------------------------------
// Video child window procedure
// ---------------------------------------------------------------------------
static LRESULT CALLBACK videoProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1; // OpenGL paints; avoid flicker
    case WM_PAINT: {
        // Re-render the last frame after a resize / expose.
        PlaybackEngine::Frame f = g_engine.currentFrame();
        if (f.rgba && f.w > 0 && f.h > 0)
            gui::glDrawFrame(f.rgba, f.w, f.h);
        else
            gui::glClear();
        return 0;
    }
    }
    return DefWindowProcA(h, m, w, l);
}

// ---------------------------------------------------------------------------
// Main window procedure
// ---------------------------------------------------------------------------
static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        g_hwnd = h;
        HINSTANCE inst = ((LPCREATESTRUCT)l)->hInstance;

        g_hvideo = CreateWindowExA(0, "PMVideo", "", WS_CHILD | WS_VISIBLE,
                                   0, 0, 10, 10, h, nullptr, inst, nullptr);
        if (!g_hvideo || !gui::glInit(g_hvideo)) {
            MessageBoxA(h, "Could not create the OpenGL context.\n"
                           "A DirectX-capable graphics driver is required.",
                        "Playback Machine", MB_ICONERROR);
            return -1;
        }

        g_btnOpen     = makeButton(h, "Open",   IDC_OPEN,     BS_PUSHBUTTON);
        g_btnPlay     = makeButton(h, "Play",   IDC_PLAY,     BS_PUSHBUTTON);
        g_btnStepBack = makeButton(h, "<|",     IDC_STEPBACK, BS_PUSHBUTTON);
        g_btnStepFwd  = makeButton(h, "|>",     IDC_STEPFWD,  BS_PUSHBUTTON);
        g_btnSkipBack = makeButton(h, "-5s",    IDC_SKIPBACK, BS_PUSHBUTTON);
        g_btnSkipFwd  = makeButton(h, "+5s",    IDC_SKIPFWD,  BS_PUSHBUTTON);
        g_btnReverse  = makeButton(h, "Reverse",IDC_REVERSE,  BS_AUTOCHECKBOX);
        g_btnLoop     = makeButton(h, "Loop",   IDC_LOOP,     BS_AUTOCHECKBOX);
        g_btnSpdDown  = makeButton(h, "-",      IDC_SPDDOWN,  BS_PUSHBUTTON);
        g_btnSpdUp    = makeButton(h, "+",      IDC_SPDUP,    BS_PUSHBUTTON);
        g_lblSpd      = makeButton(h, "1.00x",  IDC_SPDLABEL, BS_PUSHBUTTON | WS_DISABLED);
        g_btnVolDown  = makeButton(h, "Vol-",   IDC_VOLDOWN,  BS_PUSHBUTTON);
        g_btnVolUp    = makeButton(h, "Vol+",   IDC_VOLUP,    BS_PUSHBUTTON);

        g_seek = CreateWindowExA(0, WINDOWCLASS_TRACKBAR, "",
                                 WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
                                 0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_SEEK, inst, nullptr);
        SendMessageA(g_seek, TBM_SETRANGE, TRUE, MAKELPARAM(0, 60000));
        SendMessageA(g_seek, TBM_SETTICKFREQ, 1000, 0);

        g_lblTime  = makeButton(h, "0:00 / 0:00", IDC_TIME,  BS_LEFT);
        g_lblCodec = makeButton(h, "",            IDC_CODEC, BS_LEFT);

        layoutControls(h);
        SetTimer(h, 1, 100, nullptr);
        return 0;
    }

    case WM_SIZE:
        layoutControls(h);
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(w), code = HIWORD(w);
        switch (id) {
        case IDC_OPEN: {
            char file[MAX_PATH] = {0};
            OPENFILENAMEA ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = h;
            ofn.lpFilter = "Video & audio files\0*.*\0All files\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
            if (GetOpenFileNameA(&ofn))
                openFile(file);
            return 0;
        }
        case IDC_PLAY:
            g_engine.togglePlayPause();
            updatePlayButton();
            return 0;
        case IDC_STEPBACK:
            g_engine.stepFrame(-1);
            updatePlayButton();
            return 0;
        case IDC_STEPFWD:
            g_engine.stepFrame(+1);
            updatePlayButton();
            return 0;
        case IDC_SKIPBACK:
            g_engine.skipBy(-5.0);
            return 0;
        case IDC_SKIPFWD:
            g_engine.skipBy(+5.0);
            return 0;
        case IDC_REVERSE:
            g_engine.setReverse(!g_engine.isReverse());
            updateReverseButton();
            return 0;
        case IDC_LOOP:
            g_engine.setLoop(!g_engine.isLoop());
            updateLoopButton();
            return 0;
        case IDC_SPDDOWN: {
            double s = g_engine.speed() / 1.25;
            if (s < 0.1) s = 0.1;
            g_engine.setSpeed(s);
            updateSpeedLabel();
            return 0;
        }
        case IDC_SPDUP: {
            double s = g_engine.speed() * 1.25;
            if (s > 8.0) s = 8.0;
            g_engine.setSpeed(s);
            updateSpeedLabel();
            return 0;
        }
        case IDC_VOLDOWN:
            g_volume -= 10; if (g_volume < 0) g_volume = 0;
            g_engine.setVolume(g_volume);
            return 0;
        case IDC_VOLUP:
            g_volume += 10; if (g_volume > 100) g_volume = 100;
            g_engine.setVolume(g_volume);
            return 0;
        case IDC_SEEK:
            if (code == HCS_TRACKING) {
                g_seeking = true;
                g_lastTrackingTick = GetTickCount();
                int ms = (int)SendMessageA(g_seek, TBM_GETPOS, 0, 0);
                seekToTime(ms / 1000.0);
            }
            return 0;
        }
        return 0;
    }

    case WM_KEYDOWN:
        switch (w) {
        case VK_SPACE:
            g_engine.togglePlayPause();
            updatePlayButton();
            return 0;
        case VK_LEFT:
            g_engine.stepFrame(-1);
            updatePlayButton();
            return 0;
        case VK_RIGHT:
            g_engine.stepFrame(+1);
            updatePlayButton();
            return 0;
        case VK_UP:
            g_engine.setSpeed(g_engine.speed() * 1.25 > 8 ? 8 : g_engine.speed() * 1.25);
            updateSpeedLabel();
            return 0;
        case VK_DOWN:
            g_engine.setSpeed(g_engine.speed() / 1.25 < 0.1 ? 0.1 : g_engine.speed() / 1.25);
            updateSpeedLabel();
            return 0;
        case 'R':
            g_engine.setReverse(!g_engine.isReverse());
            updateReverseButton();
            return 0;
        }
        return 0;

    case WM_TIMER:
        if (w == 1) {
            if (g_seeking && (GetTickCount() - g_lastTrackingTick) > 200)
                g_seeking = false; // the thumb was released
            updateUI();
        }
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)l;
        mmi->ptMinTrackSize.x = 640;
        mmi->ptMinTrackSize.y = 400;
        return 0;
    }

    case WM_CLOSE:
        KillTimer(h, 1);
        if (g_hvideo) {
            gui::glShutdown();
            DestroyWindow(g_hvideo);
            g_hvideo = nullptr;
        }
        g_engine.close();
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow) {
    SetProcessDPIAware();

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    std::string err;
    if (!PlaybackEngine::ffLoad(&err)) {
        MessageBoxA(nullptr,
                    (std::string("Could not load the FFmpeg libraries.\n") +
                     "Make sure the FFmpeg DLLs are next to the executable.\n\n" + err)
                        .c_str(),
                    "Playback Machine", MB_ICONERROR | MB_OK);
        return 1;
    }

    WNDCLASSESA wc{};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "PMMain";
    wc.hIcon = LoadIconA(hInst, MAKEINTRESOURCE(140));
    if (!RegisterClassA(&wc))
        return 1;

    WNDCLASSESA vw{};
    vw.style = CS_HREDRAW | CS_VREDRAW;
    vw.lpfnWndProc = videoProc;
    vw.hInstance = hInst;
    vw.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    vw.hbrBackground = nullptr;
    vw.lpszClassName = "PMVideo";
    if (!RegisterClassA(&vw))
        return 1;

    g_engine.setAudioSink(&g_sink);
    g_engine.setVolume(100);

    int sw = 1280, sh = 720 + kBarHeight;
    RECT mon{0, 0, 0, 0};
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &mon, 0);
    if (sw > mon.right - mon.left) sw = mon.right - mon.left;
    if (sh > mon.bottom - mon.top) sh = mon.bottom - mon.top;
    if (sw < 480) sw = 480;
    if (sh < 300) sh = 300;

    HWND hwnd = CreateWindowExA(0, "PMMain", "Playback Machine",
                                 WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                 sw, sh, nullptr, nullptr, hInst, nullptr);
    if (!hwnd)
        return 1;

    // A file passed on the command line (or dropped) opens immediately.
    char *cmd = GetCommandLineA();
    if (cmd && cmd[0] == '"') {
        // find the closing quote
        const char *q = strrchr(cmd, '"');
        if (q && q > cmd + 1) {
            std::string path(cmd + 1, (size_t)(q - cmd - 1));
            // Open after the window is shown.
            g_curFile = path;
        }
    }

    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);
    if (!g_curFile.empty())
        openFile(g_curFile.c_str());

    // Render + input loop. The engine paces frames; we repaint only when a
    // new frame is shown (token changes) and idle otherwise.
    for (;;) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)
                return (int)msg.wParam;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        PlaybackEngine::Frame f = g_engine.currentFrame();
        if (f.token != g_lastToken && f.rgba && f.w > 0 && f.h > 0) {
            gui::glDrawFrame(f.rgba, f.w, f.h);
            g_lastToken = f.token;
        }
        Sleep(2);
    }
}
