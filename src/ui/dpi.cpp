// ---- DPI -------------------------------------------------------------------
// Without this the process is DPI-*unaware*: Windows bitmap-stretches its
// 96-DPI output, so above 100% the whole UI is soft. Declaring awareness
// gets real pixels and makes the scaling our job -- the helpers below.
// Everything resolves at runtime, not link time: SetProcessDpiAwarenessContext
// is Win10 1703+, GetDpiForWindow 1607+, and a hard import would stop the
// exe loading at all on anything older.

#include <fstream>
#include <string>

#include "host.h"
#include "dpi.h"

// MUST be called before any window exists; after that Windows ignores it.
void enable_dpi_awareness() {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (!u) return;

    typedef BOOL (WINAPI *SetCtxFn)(HANDLE);
    if (auto set_ctx = (SetCtxFn)GetProcAddress(u, "SetProcessDpiAwarenessContext")) {
        // -4 = PER_MONITOR_AWARE_V2: also scales the non-client area and sends
        // WM_DPICHANGED with a suggested rect. -3 is the older per-monitor mode.
        if (set_ctx((HANDLE)-4)) return;
        if (set_ctx((HANDLE)-3)) return;
    }
    typedef BOOL (WINAPI *SetAwareFn)(void);   // Vista..Win8.1: system-DPI only
    if (auto set_aware = (SetAwareFn)GetProcAddress(u, "SetProcessDPIAware")) set_aware();
}

UINT dpi_for(HWND h) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        typedef UINT (WINAPI *GetDpiFn)(HWND);
        if (auto get_dpi = (GetDpiFn)GetProcAddress(u, "GetDpiForWindow")) {
            UINT d = get_dpi(h);
            if (d) return d;
        }
    }
    return 96;
}

// Sizes below are written for 96 DPI and scaled through here. MulDiv rounds,
// which matters at 150%: plain integer division would quietly lose a pixel.
int scaled(int base, UINT dpi) { return MulDiv(base, (int)dpi, 96); }

// ---- remembered window rect -------------------------------------------------
// Settings -> Window. Stored as one line of "x y w h" next to the snippets,
// because a rect does not justify a format.
//
// The SETTING controls saving, not restoring: it arrives from the page long
// after the window has been created and placed, so a restore gated on it would
// always be too late. Turning it off deletes the file instead, which is also
// the honest reading of "don't remember this".
std::string window_rect_path() { return appdata_home() + "\\window.txt"; }

// The overlay preference, host-side and in its own file for one reason: it is
// needed BEFORE any page has loaded (it decides whether the desktop window is
// shown at all), and the page that owns the setting may be the one inside the
// overlay. The page still owns the value -- this is a cache of the last thing
// it said, not a second source of truth.
static std::string overlay_pref_path() { return appdata_home() + "\\overlay.txt"; }

void save_overlay_pref(bool on) {
    std::ofstream o(overlay_pref_path(), std::ios::binary);
    o << (on ? 1 : 0) << "\n";
}

void load_overlay_pref(bool& pref) {
    std::ifstream f(overlay_pref_path());
    int v = 0;
    if (f >> v) pref = (v != 0);
}
