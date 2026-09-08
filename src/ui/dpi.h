// dpi.h -- DPI awareness/scaling plus the remembered-window-rect and
// overlay-preference files. Split out of eclipse_ui.cpp; the reasoning lives
// with the code in dpi.cpp.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

// MUST be called before any window exists; after that Windows ignores it.
void enable_dpi_awareness();
UINT dpi_for(HWND h);
// Sizes in this app are written for 96 DPI and scaled through here. MulDiv
// rounds, which matters at 150%: plain integer division would quietly lose a
// pixel.
int scaled(int base, UINT dpi);

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

// One definition of the floor for both the resize clamp and the webview hint
// -- separate copies drift apart. 900x600 is measured, not round: below it
// the bottom bar's status block collides with the buttons and the console
// and editor collapse. At 96 DPI, scaled() before use -- the limit is about
// how much LAYOUT fits, and the layout is in CSS pixels, so at 150% it
// genuinely needs 1350x900 of screen.
static const int kMinW = 900;
static const int kMinH = 600;

// Preferred opening size, used only if it fits. Also at 96 DPI.
static const int kOpenW = 1180;
static const int kOpenH = 780;

std::string window_rect_path();
// The overlay preference. The value itself is eclipse_ui.cpp's
// g_overlay_pref -- passed in and out rather than read here, so this TU owns
// no state. load only assigns when the file parses.
void save_overlay_pref(bool on);
void load_overlay_pref(bool& pref);
