// overlay.h -- the seam into the overlay machine (overlay.cpp): the overlay
// window, the in-game panel and badge, the overlay side of docked snippet
// panels, the hit-test region, focus mode, the hooks, the band rule and app
// mode. The machine was moved out of eclipse_ui.cpp WHOLE -- see the note at
// the top of overlay.cpp: one subsystem, cross-referential invariants, do
// not split it.
//
// This TU OWNS every piece of overlay state (the mode flags, the app-mode
// anchor, the panel and its composed host, the hooks, the band bookkeeping).
// FUNCTIONS ONLY, like host.h and the rest of the seams. No `extern`
// globals -- that is how a second owner gets born.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

// Post one message to the in-game panel. Dispatches to the UI thread itself;
// snapshots (STATE, RUNSTATE, SNIPS, HEALTH, DOCKP, HUDVIS) are deduped
// against the last copy sent, LOGS deliberately are not. The host's log and
// state funnels feed it (hud_post in host.h is the other TUs' route in).
void hud_push(const std::string& msg);

// The 16ms WM_TIMER tick, UI thread only: hud_tick glues the panel to the
// client and holds its band, overlay_tick owns the hooks and the focus
// hand-back, health_tick polls the panel's health chip at 4 Hz.
void hud_tick();
void overlay_tick();
void health_tick();

// Teardown, called from WM_CLOSE and the end of WinMain. hud_destroy brings
// the panel/overlay window down (the hooks stay -- overlay_tick releases
// them when nothing is left that wants them); destroy_overlay_window is the
// snippet-driven GDI overlay's; overlay_release_hooks drops the WH_MOUSE_LL
// hook and the WinEvent hook BEFORE the process goes -- while the LL hook is
// registered and unanswered every mouse event in the session waits out
// LowLevelHooksTimeout, felt as the cursor freezing.
void hud_destroy();
void destroy_overlay_window();
void overlay_release_hooks();

// The engine's overlay callbacks (fx_overlay_on/off, fx_overlay_data),
// registered against ec:: in WinMain. Both hop to the UI thread themselves.
void overlay_mode_sink(int on);
void overlay_data_sink(const char* json);

// App mode's doors that open from the host: the titlebar CLOSE asks to close
// (hand_to_game says who gets focus after);
// app_abort_restore is every non-animated way out (WM_CLOSE, teardown);
// app_drag_settled folds a finished modal move/size back into the one
// geometry store (subclass's WM_EXITSIZEMOVE).
//
// There is no in-game geometry to forget or to mirror: the window carries its
// screen rect into the game and back out again, and window.txt is the only
// place it is written. See the note over app_anchor_capture in overlay.cpp.
void app_close_begin(bool hand_to_game);
void app_abort_restore();
void app_drag_settled();
// The app page's paint witness for the open leg (APPPAINTED): the window
// shown under the travel box has a frame on glass, so it may come forward.
void app_painted();

// State the host consults: whether app mode is up right now (the WIN:
// handlers and the frameless-window plumbing branch on it), and the standing
// overlay-mode preference (reveal_window's startup gate).
bool overlay_app_mode();
bool overlay_pref();

// The transitions the settings parser and WinMain hand over whole, so the
// flags keep one owner: the overlay-mode preference flip, the in-game-panel
// switch, and the startup load of the preference file.
void overlay_pref_set(bool want);
void overlay_hud_set(bool want);
void overlay_load_pref();

// The overlay side of the docked snippet panel (snipwin.cpp calls these;
// the origin itself is read back through dock_host()/dock_dir()): make the
// composed overlay exist, and map/clear the docked origin onto its core.
bool ensure_overlay();
void dock_apply_mapping();
void dock_clear_mapping();

// The z-move watch (ECLIPSE_ZLOG=1 only, no-op otherwise): the app's wndproc
// and the snippet windows' subclass forward WM_WINDOWPOSCHANGED here, and
// z-order-changing moves are logged with the CALLER'S STACK -- the one fact
// that separates "our code asked" from "USER32/another process moved us",
// which no external poller can attribute. wp is the LPARAM's WINDOWPOS.
void zwatch_poschanged(const char* who, HWND h, const void* wp);

// The app window was restacked by someone other than us (its own activation
// on a click, USER32's owner enforcement): the app is owned by the client,
// so the raise carries the client above the overlay. Re-glues the overlay
// directly under the app, synchronously, from the app's wndproc
// WM_WINDOWPOSCHANGED -- so the dock never reaches a composed frame behind
// the game. No-op for our own moves, mid-choreography, and non-z moves.
void app_zorder_changed(const void* wp);
