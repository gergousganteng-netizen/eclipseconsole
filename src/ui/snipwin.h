// snipwin.h -- the snippet-driven windows: PER-RUN floating WebView2
// wrappers and the ONE docked overlay-iframe panel, one replay state per
// run across both backings. Split out of eclipse_ui.cpp; the design (why
// two backings, why the dock is a page element and singular, the origin
// isolation, the per-run message routing) lives with the code in
// snipwin.cpp.
//
// This TU OWNS the windows' state -- the per-run map, the replay states,
// the page->snippet queues, the dock claim and origin. The overlay side of
// the dock stays with the overlay machine (ensure_overlay,
// dock_apply_mapping, dock_clear_mapping in overlay.h) and reads the origin
// back through dock_host()/dock_dir(); the overlay-existence gate reads
// snip_docked().
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

// True while a snippet's DOCKED panel lives inside the overlay page. One of
// overlay_wanted()'s four reasons; the settings parser consults it too.
bool snip_docked();

// True iff h is a live FLOATING snippet window (top-level HWND). The app-mode
// band rule asks this to tell "the app is focused" from "a window of this
// process is focused": both read as fgpid == GetCurrentProcessId(), but a
// focused snippet window must be treated like a THIRD-PARTY app -- demoting
// the app to HWND_NOTOPMOST above it is what buried it (see the band rule's
// demote in overlay.cpp). Docked panels have no HWND of their own and never
// match. UI thread only, like every other window query here.
bool snip_is_window(HWND h);

// The docked origin, for the virtual-host mapping on the overlay's core.
// Empty while nothing is docked.
std::string dock_host();
std::string dock_dir();

// The DOCKP snapshot: describes the docked panel to the overlay page
// (deduped in hud_push, replayed on HUD_READY).
void push_dock();

// The overlay page's SNIPMSG relay: "<run>:<payload>" (the run id the DOCKP
// snapshot handed the page). Routed to that run's queue after checking the
// id against the live dock owner -- a stale in-flight message from a
// previous dock's page must not be credited to the run that docks next. The
// floating wrapper's binding needs no relay: its closure owns the run id.
void snip_dock_msg(const std::string& body);

// Peel the single string argument off a webview bind() JSON array. The
// panel's own eclipse_send binding shares it.
std::string extract_first_json_arg(const std::string& arr);

// Tear EVERY run's window down and clear the replay states (app exit,
// teardown). A single run's window goes through its close sink instead.
void snip_close();

// The fx_window_* sinks, registered against ec:: in WinMain. run is the
// CALLING run's id, resolved by the fx export on the snippet's own thread
// (the sinks dispatch to the UI thread, where the current-run TLS is
// nobody's).
void window_open_sink(unsigned run, const char* title, int w, int h);
void window_html_sink(unsigned run, const char* html);
void window_nav_sink(unsigned run, const char* url);
void window_eval_sink(unsigned run, const char* js);
void window_close_sink(unsigned run);
bool window_get_msg_sink(unsigned run, std::string* out);
void window_dock_sink(unsigned run, int mode, int corner, int dx, int dy);
void window_pass_sink(unsigned run, int on);
