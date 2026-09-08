// window.cpp -- the snippet's WINDOW and OVERLAY surface. Split out of
// eclipse_console.cpp. It OWNS the sink pointers (g_window_*, g_overlay_*) that
// eclipse_ui registers through the ec::set_* setters below, and it exposes them
// to snippets as fx_window_* / fx_overlay_*. A module owning its own state, not
// a leftovers pile: everything here is one feature -- the page a snippet can put
// up and the overlay it can draw to. The CLI binary wires no sinks, so every
// entry point is a no-op there. Comments travel verbatim.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <string>

#include "fx.h"
#include "eclipse_core.h"    // ec::WindowOpenFn etc., ec::logf decl
#include "core_internal.h"   // window_run_end (teardown release)
#include "run.h"             // core_run_current_id / core_run_label

using ec::logf;   // fx_window_navigate logs the file it points at

// ---- the two surfaces, split by nature (docs/multi-run.md phase 2) ---------
// The WINDOW is per run now: each run that opens one gets its own instance in
// snipwin's map, so there is nothing to claim and nothing to refuse -- the
// exports resolve the calling run's id here (on the snippet's own thread,
// where the current-run TLS is valid) and pass it through the sinks. The
// per-run message deque that comes with it is the point: one shared deque let
// snippet B pop a message produced by A's page.
//
// The OVERLAY stays FIRST-WINS. fx_overlay_post feeds one draw list for one
// drawing surface; per-run overlay means merging N draw lists per composed
// frame and deciding z-order between runs -- a renderer change, not a state
// split, and explicitly not this job. So it is owned by the first run to take
// it, and a second claimant is REFUSED BY NAME rather than silently
// clobbering the holder.
//
// id 0 is the CLI / un-adopted path: single-run and the sinks are no-ops
// there, so it never contends -- it is allowed through without claiming.
static std::atomic<unsigned> g_overlay_owner{0};  // run that turned the overlay on

// May the CALLING run drive this surface? True if it owns it, or on the CLI.
static bool owns(std::atomic<unsigned>& owner) {
    unsigned me = core_run_current_id();
    return me == 0 || owner.load() == me;
}

// Take the surface, or refuse by name. Returns false (and logs) if another run
// holds it.
static bool claim(std::atomic<unsigned>& owner, const char* what) {
    unsigned me = core_run_current_id();
    if (me == 0) return true;                 // CLI / un-adopted
    unsigned cur = owner.load();
    if (cur == me) return true;               // already ours
    if (cur == 0) {
        unsigned expect = 0;
        if (owner.compare_exchange_strong(expect, me)) return true;
        if (owner.load() == me) return true;
        cur = owner.load();
    }
    std::string who = core_run_label(cur);
    logf("x %s is in use by %s\n", what, who.empty() ? "another run" : who.c_str());
    return false;
}

static ec::OverlayModeFn g_overlay_mode  = nullptr;
static ec::OverlayDataFn g_overlay_data  = nullptr;
static ec::WindowOpenFn    g_window_open    = nullptr;
static ec::WindowHtmlFn    g_window_html    = nullptr;
static ec::WindowNavFn     g_window_nav     = nullptr;
static ec::WindowEvalFn    g_window_eval    = nullptr;
static ec::WindowCloseFn   g_window_close   = nullptr;
static ec::WindowGetMsgFn  g_window_get_msg = nullptr;
static ec::WindowDockFn    g_window_dock    = nullptr;
static ec::WindowPassFn    g_window_pass    = nullptr;

// ---- overlay + window exports ----------------------------------------------
// Overlay hooks. All three are no-ops when the sink is unset -- the CLI
// binary never wires them, so a snippet using overlay still runs and simply
// produces no visual output there.
FX_API void fx_overlay_on(void) {
    // One draw list, one drawing surface: two runs cannot co-own it. The refusal
    // states it, rather than one silently winning (docs/multi-run.md).
    if (!claim(g_overlay_owner, "the overlay")) return;
    if (g_overlay_mode) g_overlay_mode(1);
}

FX_API void fx_overlay_off(void) {
    if (!owns(g_overlay_owner)) return;
    if (g_overlay_mode) g_overlay_mode(0);
    g_overlay_owner.store(0);            // released; another run may take it now
}

FX_API void fx_overlay_post(const char* json) {
    if (!owns(g_overlay_owner)) return;
    if (g_overlay_data) g_overlay_data(json ? json : "[]");
}

// Snippet window primitives, PER RUN: the id resolved here on the calling
// thread routes every call to that run's own window instance in snipwin.
// Same no-op pattern as the overlay hooks: the CLI binary never wires the
// sinks so a snippet using them runs there too, just without any UI.
FX_API void fx_window_open(const char* title, int w, int h) {
    if (g_window_open) g_window_open(core_run_current_id(),
                                     title ? title : "Snippet", w, h);
}

FX_API void fx_window_html(const char* html) {
    if (g_window_html) g_window_html(core_run_current_id(), html ? html : "");
}

// Point the window at a real file instead of a string.
//
// fx_window_html goes through NavigateToString, which gives the page an OPAQUE
// ORIGIN: no relative URLs, no local files, no fetch. Everything has to be
// inline. That is fine for a status panel and painful for anything larger.
// A file:// page is an ordinary page -- <link>, <img>, <script src>, fetch of
// its neighbours, all work.
//
// A bare path is turned into a file:// URL; anything already carrying a scheme
// is passed through, so https:// works too.
//
// This grants a snippet nothing it did not have: it is native code in this
// process and could already open any file it liked. What it changes is where
// the PAGE may read from, and the page is content the snippet itself wrote.
FX_API void fx_window_navigate(const char* path_or_url) {
    if (!g_window_nav || !path_or_url || !*path_or_url) return;

    std::string u = path_or_url;
    if (u.find("://") == std::string::npos) {
        // Check it is there BEFORE navigating. A file:// URL that misses gets
        // Edge's "File not found" page, which does not say what it looked for
        // -- so a typo in the path is indistinguishable from the feature being
        // broken, and there is nowhere to read the answer.
        if (GetFileAttributesA(u.c_str()) == INVALID_FILE_ATTRIBUTES) {
            logf("x fx_window_navigate: no such file\n");
            logf("    %s\n", u.c_str());
            return;
        }
        for (char& c : u) if (c == '\\') c = '/';
        u = "file:///" + u;
    }
    logf("  window -> %s\n", u.c_str());
    g_window_nav(core_run_current_id(), u.c_str());
}

FX_API void fx_window_eval(const char* js) {
    if (g_window_eval) g_window_eval(core_run_current_id(), js ? js : "");
}

// fx_window_text is a convenience wrapper: escapes the string and wraps it in
// a <pre> so it renders as fixed-width plain text. The GDI predecessor did the
// same job -- keeping the entry point means old snippets still compile.
FX_API void fx_window_text(const char* text) {
    if (!g_window_html) return;
    std::string s = text ? text : "";
    std::string html;
    html.reserve(s.size() + 128);
    html +=
        "<html><body style=\"margin:0;padding:10px;background:#141418;"
        "color:#dcdcdc;font:14px system-ui,Segoe UI,sans-serif;\">"
        "<pre style=\"margin:0;white-space:pre-wrap;font-family:inherit\">";
    for (char c : s) {
        switch (c) {
            case '&': html += "&amp;";  break;
            case '<': html += "&lt;";   break;
            case '>': html += "&gt;";   break;
            default:  html += c;
        }
    }
    html += "</pre></body></html>";
    g_window_html(core_run_current_id(), html.c_str());
}

FX_API void fx_window_close(void) {
    if (g_window_close) g_window_close(core_run_current_id());
}

FX_API void fx_window_passthrough(int on) {
    if (g_window_pass) g_window_pass(core_run_current_id(), on ? 1 : 0);
}

FX_API void fx_window_dock(int mode, int corner, int dx, int dy) {
    if (g_window_dock) g_window_dock(core_run_current_id(), mode, corner, dx, dy);
}

FX_API const char* fx_window_get_msg(void) {
    // Pops the CALLING run's own deque -- per-run isolation by construction,
    // so snippet B can never pop a message produced by A's page (the exact
    // silent-cross-talk the per-run design exists to prevent).
    thread_local std::string buf;
    buf.clear();
    if (g_window_get_msg &&
        g_window_get_msg(core_run_current_id(), &buf) && !buf.empty()) {
        return buf.c_str();
    }
    return nullptr;
}

// Teardown release, called from build_and_run when a run ends. Closes the
// run's own window if it left one open (the sink no-ops when it did not) and
// drops the run's overlay claim, so a snippet that forgot to close does not
// strand a surface forever. id 0 (CLI) has no UI sinks and nothing per-run to
// tidy, so it is skipped.
void window_run_end(unsigned id) {
    if (id == 0) return;
    if (g_window_close) g_window_close(id);
    if (g_overlay_owner.load() == id) {
        g_overlay_owner.store(0);
        if (g_overlay_mode) g_overlay_mode(0);
    }
}

namespace ec {

// The sink registration the host (eclipse_ui) calls through. This TU owns the
// pointers; publishing them by reference instead would be the same failure as
// a mutex published by reference.
void set_overlay_mode(OverlayModeFn fn) { g_overlay_mode = fn; }
void set_overlay_data(OverlayDataFn fn) { g_overlay_data = fn; }

void set_window_open   (WindowOpenFn   fn) { g_window_open    = fn; }
void set_window_html   (WindowHtmlFn   fn) { g_window_html    = fn; }
void set_window_nav    (WindowNavFn    fn) { g_window_nav     = fn; }
void set_window_eval   (WindowEvalFn   fn) { g_window_eval    = fn; }
void set_window_close  (WindowCloseFn  fn) { g_window_close   = fn; }
void set_window_get_msg(WindowGetMsgFn fn) { g_window_get_msg = fn; }
void set_window_dock   (WindowDockFn fn)    { g_window_dock    = fn; }
void set_window_pass   (WindowPassFn fn)    { g_window_pass    = fn; }

}  // namespace ec
