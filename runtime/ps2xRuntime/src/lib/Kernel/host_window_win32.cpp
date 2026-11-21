// The game window's hand-off from a launcher (ps2_runtime.cpp openHostWindow). Kept here so <windows.h> stays out of
// ps2_runtime.cpp (raylib's names clash with it).
//
// Why the window is replaced: a launcher that drew into the window with OpenGL while it filled its monitor (borderless
// fullscreen) leaves it as a swap chain Windows shows straight on the screen, skipping composition. The game then shows
// its picture in PCSX2's GS child window and the host window stops presenting, so the launcher's last frame stayed on
// screen until something forced composition (the Start menu, F11). Started without the launcher, the window never
// presents and the picture shows (K, Nov 17). Wine (Proton, the Steam Deck) has no such composition step: the window is
// kept there, as before.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>

namespace
{
    HWND g_cover = nullptr;
    constexpr wchar_t kCoverClass[] = L"PS2XHandoffCover";
    constexpr UINT_PTR kCoverTimer = 1;

    // Failsafe: the cover goes by itself if the game never presents (ps2xHostHandoffCoverHide normally removes it).
    LRESULT CALLBACK coverProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_TIMER && wParam == kCoverTimer)
        {
            DestroyWindow(hwnd);
            return 0;
        }
        if (msg == WM_DESTROY && hwnd == g_cover)
            g_cover = nullptr;
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// Keeps the display (and the PC) awake while the program runs (issue #12): a controller's input doesn't count as the
// user being active, so Windows' "turn off my screen after N minutes" blanked the monitor mid-game (no signal; the game
// still running; a key, Alt+Tab or a re-plugged cable woke it). GLFW doesn't ask for this in a borderless window; SDL
// would, but we start only its gamepad part. Bound to the calling thread (the main / window thread, which lives as
// long as the program) until it exits.
void ps2xHostKeepDisplayAwake() noexcept
{
    static bool done = false;
    if (done)
        return;
    done = true;
    if (SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED) == 0)
        std::fprintf(stderr, "[window] couldn't keep the display awake (SetThreadExecutionState failed)\n");
}

// True on Windows itself, false under Wine (ntdll exports wine_get_version there).
bool ps2xHostReplacesLauncherWindow() noexcept
{
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return !(ntdll && GetProcAddress(ntdll, "wine_get_version"));
}

// A plain black window (GDI, no swap chain) over the monitor the given window is on, so the screen stays black while
// the window is closed and made again and until the game's first frames are on screen (the main loop hides it once
// the GS has presented), instead of showing the desktop or the new, still empty window. Never activated.
void ps2xHostHandoffCoverShow(void *window) noexcept
{
    if (g_cover)
        return;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered)
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = coverProc;
        wc.hInstance = instance;
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.hCursor = nullptr;
        wc.lpszClassName = kCoverClass;
        registered = RegisterClassW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        if (!registered)
            return;
    }
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(MonitorFromWindow(static_cast<HWND>(window), MONITOR_DEFAULTTONEAREST), &info))
        return;
    const RECT &r = info.rcMonitor;
    g_cover = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, kCoverClass, L"", WS_POPUP,
                              r.left, r.top, r.right - r.left, r.bottom - r.top, nullptr, nullptr, instance, nullptr);
    if (!g_cover)
        return;
    ShowWindow(g_cover, SW_SHOWNOACTIVATE);
    UpdateWindow(g_cover); // painted black now (the class brush), before the game window goes
    SetTimer(g_cover, kCoverTimer, 10000, nullptr);
}

void ps2xHostHandoffCoverHide() noexcept
{
    if (!g_cover)
        return;
    DestroyWindow(g_cover);
    g_cover = nullptr;
}
#endif
