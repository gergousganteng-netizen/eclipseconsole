// host.h -- what the split-out TUs call back into eclipse_ui.cpp for.
//
// FUNCTIONS ONLY, deliberately. The split rule is one owner per piece of
// state: each extracted TU owns its own globals, eclipse_ui.cpp keeps the
// rest, and the two sides talk through functions. Nothing here may ever
// declare an `extern` global -- that is how a second owner gets born.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <functional>
#include <string>

// The UI message funnel. Defined in eclipse_ui.cpp with the dispatch it
// needs; see the comments there. ui_log moved to log.h with the queue
// that backs it.
void ui_post(const std::string& msg);

// The log funnel's host side, for log.cpp: whether a page exists to queue
// for, the UI-thread hop that runs flush_logs, and delivery of one funnel
// message to BOTH views (the desktop page and the in-game panel).
bool ui_page_live();
void ui_dispatch_log_flush();
void ui_deliver_logmsg(const std::string& msg);

// Run fn on the UI thread (WebView2 accepts calls nowhere else); no-op when
// no page exists. The snippet-window sinks marshal through this.
void ui_dispatch(std::function<void()> fn);

// Post one message to the in-game panel (the overlay machine's hud_push
// feed; the wrapper lives in eclipse_ui.cpp). The overlay side of the docked
// snippet panel -- ensure_overlay, dock_apply_mapping, dock_clear_mapping --
// moved with the machine to overlay.h.
void hud_post(const std::string& msg);

// The shared string helpers from the top of eclipse_ui.cpp.
std::string narrow(const wchar_t* w);
std::wstring widen(const std::string& s);
std::string json_escape(const std::string& s);

// Sends the offset table to the page (eclipse_ui.cpp).
void push_offsets();

// Sends pid/base/DataModel/identity to the page and the in-game panel
// (eclipse_ui.cpp; reads the identity through attach.h's getters).
void push_state();

// The offsets conf path, set once at startup (eclipse_ui.cpp).
std::string ui_conf();

// Re-reads the snippets tree and re-sends it to the page and the in-game
// panel (eclipse_ui.cpp). Idempotent; safe from any thread.
void push_snippets();

// %APPDATA%\EclipseConsole (created on first use), or the exe's own directory
// when %APPDATA% is missing. Defined in eclipse_ui.cpp with the snippets dir.
std::string appdata_home();

// What the overlay machine (overlay.cpp) calls back for, added when it moved
// out whole. All defined in eclipse_ui.cpp beside the state they read.

// The page-protocol dispatcher. The overlay page shares the console's
// message set (HUD_ARM_TOGGLE, STOP and the rest funnel through it).
void handle(const std::string& msg);

// The RUNSTATE and SNIPS snapshots for the in-game panel, replayed on its
// HUD_READY handshake.
void push_runstate();
void push_hud_snippets();

// The main window (app mode positions it over the game) and the main page's
// WebView2 controller (ICoreWebView2Controller*, or null before the page is
// up) -- the composed overlay reuses its environment.
HWND ui_main_hwnd();
void* ui_browser_controller();

// ONE GEOMETRY, ONE STORE. The app window is the same HWND on the desktop and
// in the game, so where it sits and how big it is is a single fact and
// window.txt is the only place it lives. Nothing in the overlay keeps a copy;
// the client-relative anchor it needs is derived at the open and thrown away
// -- see app_anchor_capture in overlay.cpp. The two window-memory switches
// live with the store, so no caller has to ask about them.

// The app window ended a move or a resize, in EITHER mode: this is the whole
// rect, in screen coordinates, and the only writer of the store. Whichever
// half its switch declines is left as it stands.
void ui_geometry_changed(int x, int y, int w, int h);
// One of the two switches was just turned off: drop that half NOW rather than
// at the next close, so the effect is visible on the next open. The other half
// has to survive it.
void ui_geometry_forget(bool pos, bool size);

// Inside the main window's modal move/size loop; the overlay tick must not
// re-glue the app window mid-drag.
bool ui_in_sizemove();
