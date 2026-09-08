#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <deque>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>

#include "webview/webview.h"
#include "eclipse_core.h"
#include "run.h"             // core_run_label -- the dock refusal names its holder

#include "host.h"
#include "overlay.h"
#include "snipwin.h"

// ---- snippet-driven windows, ONE PER RUN -----------------------------------
//
// Multi-run phase 2 (docs/multi-run.md "Windows and the overlay"): fx_window_*
// is per run. Each run that opens a window gets its own SnipWin -- its own
// WebView2 wrapper, its own bounded message deque, its own replay state --
// keyed by run id in the map this TU owns. The fx exports resolve the calling
// run in core/features/window.cpp and pass the id through the sinks, so fx.h
// is unchanged and a snippet still sees exactly one window: its own.
//
// The per-run message deque is the point of the split. One shared deque meant
// snippet A could pop a message produced by B's page -- A acting on B's button
// click, silently and intermittently. Per-run makes that impossible by
// construction: the floating window's eclipse_send binding is a closure that
// captures the owning run id, and the docked relay carries the id in the
// protocol (SNIPMSG:<run>:<payload>).
//
// DOCKING STAYS SINGULAR. dock_apply_mapping maps ONE origin onto one corner
// of the composed overlay core -- N floating windows, first-come on the dock,
// a second claimant refused by name. Per-corner docking is a separate later
// job. fx_overlay_* stays first-wins too, over in window.cpp.
//
// Floating windows are secondary webview::webview instances sharing the main
// thread's message pump -- run() is only ever called on the console's webview.
// Shared window-refcount inside the library: destroying a snippet window
// decrements it; the console's own close (snip_close, WM_CLOSE) takes every
// remaining one down so the count reaches 0 and the app exits cleanly.
//
// User closes with X: the HWND is destroyed but the webview object still
// holds COM references. The per-instance gc notices via IsWindow() on the
// next op and drops the object; the run itself was already told to stop by
// the WM_CLOSE subclass.

struct SnipWin {
    unsigned run = 0;          // owning run id (map key, repeated for lookups)
    std::unique_ptr<webview::webview> wv;   // floating host (mode 0)
    WNDPROC orig_wndproc = nullptr;
    bool    docked = false;    // this run's page is THE overlay dock panel
    bool    busy   = false;    // creating/switching; keep ticks + hooks out

    // Replay state: what the snippet last asked its window to show, so a dock
    // toggle can restore it into the freshly built backing.
    int         mode  = 0;                // last fx_window_dock mode
    std::string title = "Snippet";
    int         w = 240, h = 150;
    int         ckind = 0;                // 0 blank, 1 html, 2 url
    std::string content;                  // the html or the url

    // What fx_window_dock asked for. corner: 0 TL, 1 TR, 2 BL, 3 BR; dx/dy
    // are insets from that corner. The overlay page turns them into CSS
    // anchors, so a panel keeps its place across game-window moves and
    // resizes with no tick doing arithmetic.
    int dock_corner = 1;
    int dock_dx = 12, dock_dy = 12;
    // fx_window_passthrough. Lives with the dock REQUEST rather than the
    // docked-panel plumbing below, because like corner/insets it is remembered
    // while floating and applies the moment the window docks.
    bool dock_pass = false;

    // Docked-panel plumbing, meaningful only while docked==true.
    unsigned    dock_page = 0;            // page file counter within a dock
    std::string dock_host;                // "snipN.eclipse" while docked
    std::string dock_file;                // the served file, deleted on replace/close
    std::string dock_src;                 // what the overlay iframe shows

    // Page -> snippet queue. Its own mutex, because the snippet thread pops
    // (window_get_msg_sink) while the UI thread pushes.
    std::mutex              msg_mux;
    std::deque<std::string> msgs;
};

// The map. WRITTEN only on the UI thread (every sink except get_msg
// dispatches there); the mutex exists for the snippet-thread readers --
// get_msg and the enqueue path -- which take a shared_ptr under it and then
// work on the instance's own msg_mux. shared_ptr, so an instance being torn
// down on the UI thread cannot evaporate under a reader.
static std::mutex g_wins_mux;
static std::map<unsigned, std::shared_ptr<SnipWin>> g_wins;

// The ONE dock: which run's page is the overlay panel. 0 = free. gen numbers
// give each docked snippet a FRESH origin (and cache-proof page file names):
// dock N+1 must not inherit dock N's localStorage any more than it may touch
// the console's DOM -- so the counter is global across runs, not per run.
static unsigned    g_dock_run = 0;
static unsigned    g_dock_gen = 0;
static std::string g_dock_dir;         // %TEMP%\eclipse_dock, the mapped folder

// UI-thread lookups (no dispatch in the callers' path).
static std::shared_ptr<SnipWin> win_find(unsigned run) {
    std::lock_guard<std::mutex> lk(g_wins_mux);
    auto it = g_wins.find(run);
    return it == g_wins.end() ? nullptr : it->second;
}
static std::shared_ptr<SnipWin> win_get(unsigned run) {   // find-or-create, UI thread
    std::lock_guard<std::mutex> lk(g_wins_mux);
    auto& p = g_wins[run];
    if (!p) { p = std::make_shared<SnipWin>(); p->run = run; }
    return p;
}
static void win_erase(unsigned run) {
    std::lock_guard<std::mutex> lk(g_wins_mux);
    g_wins.erase(run);
}
static std::shared_ptr<SnipWin> dock_owner() {
    return g_dock_run ? win_find(g_dock_run) : nullptr;
}

static HWND win_hwnd_raw(SnipWin& sw) {
    if (sw.docked || !sw.wv) return nullptr;
    auto res = sw.wv->window();
    return (res.ok() && res.value()) ? (HWND)res.value() : nullptr;
}

// Subclass on the FLOATING snippet HWNDs. The library's own wndproc calls
// DestroyWindow on WM_CLOSE, which leaves the owning run's fx loop running
// (silent no-op on further fx_window_* calls) and the console showing it as
// running. Catching WM_CLOSE here lets us stop the run that OWNS the window
// -- not every run: with several snippets live, closing one's window must
// not take the others down. Programmatic DestroyWindow from teardown skips
// WM_CLOSE entirely, so snippet-driven close is never confused with
// user-driven close. GWLP_USERDATA belongs to the library, so the instance
// is found by walking the map -- at most CORE_RUN_CAP entries, on WM_CLOSE
// only. The DOCKED kind has no caption and no user-close path, exactly like
// the panel, so it needs no equivalent.
static LRESULT CALLBACK snip_wndproc_hook(HWND h, UINT m, WPARAM w, LPARAM l) {
    std::shared_ptr<SnipWin> sw;
    {
        std::lock_guard<std::mutex> lk(g_wins_mux);
        for (auto& kv : g_wins)
            if (kv.second && !kv.second->docked && kv.second->wv) {
                auto res = kv.second->wv->window();
                if (res.ok() && (HWND)res.value() == h) { sw = kv.second; break; }
            }
    }
    if (!sw) return DefWindowProcW(h, m, w, l);   // torn down mid-flight
    // The z-move watch (overlay.h): who moves a SNIPPET window. Observation
    // only; see the APP twin in eclipse_ui.cpp's wndproc.
    if (m == WM_WINDOWPOSCHANGED) zwatch_poschanged("SNIP", h, (const void*)l);
    if (m == WM_CLOSE) {
        if (sw->run) ec::stop_run(sw->run);
        else         ec::stop_snippet();   // rid-0 legacy path: single-run stop
    }
    return CallWindowProcW(sw->orig_wndproc ? sw->orig_wndproc : DefWindowProcW,
                           h, m, w, l);
}

// The webview library packs a bind() call's args as a JSON array. For our
// single-argument eclipse_send(string) that's always ["<escaped>"]. We only
// need to unescape a single string; a full JSON parser would be overkill.
// Unknown \uXXXX escapes are passed through unmodified -- if a page needs
// binary-safe encoding it can pack its own base64.
std::string extract_first_json_arg(const std::string& arr) {
    size_t i = 0;
    while (i < arr.size() && arr[i] != '"') i++;
    if (i >= arr.size()) return {};
    i++;
    std::string out;
    while (i < arr.size() && arr[i] != '"') {
        if (arr[i] == '\\' && i + 1 < arr.size()) {
            switch (arr[i + 1]) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case '"': out += '"';  break;
                case '\\': out += '\\'; break;
                case '/': out += '/';  break;
                default:  out += arr[i]; out += arr[i + 1]; break;
            }
            i += 2;
        } else {
            out.push_back(arr[i++]);
        }
    }
    return out;
}

// Queue one page->snippet string INTO ITS OWN RUN'S deque. The floating
// binding's closure knows the run; the docked relay's id is parsed below.
static void snip_enqueue_msg_run(unsigned run, std::string msg) {
    auto sw = win_find(run);
    if (!sw) return;                       // window already gone; drop
    std::lock_guard<std::mutex> lk(sw->msg_mux);
    if (sw->msgs.size() >= 1024) sw->msgs.pop_front();  // bound a spam loop
    sw->msgs.push_back(std::move(msg));
}

// The overlay page's SNIPMSG relay: "SNIPMSG:<run>:<payload>" (the id was
// handed to the page in the DOCKP snapshot). The id is checked against the
// LIVE dock owner rather than trusted: an in-flight message from a previous
// dock's page, arriving across a teardown/re-dock boundary, must not be
// credited to the run that docks next.
void snip_dock_msg(const std::string& body) {
    size_t colon = body.find(':');
    if (colon == std::string::npos) return;
    unsigned run = (unsigned)strtoul(body.c_str(), nullptr, 10);
    if (!run || run != g_dock_run) return;   // stale or forged origin: drop
    snip_enqueue_msg_run(run, body.substr(colon + 1));
}

// Is h a live floating snippet window? Any run's -- the app-mode band rule
// must treat EVERY snippet window as a third-party app, not just the first
// (see the demote in overlay.cpp). Answered without the gc side effect: the
// band rule asks this per tick and must not become an owner of the windows'
// lifetime.
bool snip_is_window(HWND h) {
    if (!h) return false;
    std::lock_guard<std::mutex> lk(g_wins_mux);
    for (auto& kv : g_wins) {
        if (!kv.second || kv.second->docked || !kv.second->wv) continue;
        auto res = kv.second->wv->window();
        if (res.ok() && res.value() && (HWND)res.value() == h) return true;
    }
    return false;
}

// Only the FLOATING window can be closed out from under us (its X button).
// The docked panel is an element of our own overlay page and has no such
// path, so its liveness is just whether we still hold it.
static void snip_gc_if_dead(SnipWin& sw) {
    if (sw.docked || !sw.wv) return;
    auto res = sw.wv->window();
    if (res.ok() && res.value() && IsWindow((HWND)res.value())) return;
    // HWND is gone (user hit X). The destructor releases the COM references
    // and destroys any leftover child widget window WebView2 owns.
    sw.wv.reset();
    sw.orig_wndproc = nullptr;
    std::lock_guard<std::mutex> lk(sw.msg_mux);
    sw.msgs.clear();
}

static HWND snip_hwnd(SnipWin& sw) {
    if (sw.docked) return nullptr;
    snip_gc_if_dead(sw);
    return win_hwnd_raw(sw);
}

// The overlay side of the dock lives in overlay.cpp: the docked panel needs
// the overlay up (ensure_overlay pumps its creation) and its own origin
// mapped onto the overlay's core (dock_apply_mapping / dock_clear_mapping).
// All three come in through overlay.h.

// The folder the virtual host serves from. Temp rather than the install dir:
// the content is per-run user data, and the installer's tree must never grow
// files the uninstaller does not know about.
static bool dock_dir_ensure() {
    if (!g_dock_dir.empty()) return true;
    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (!n || n >= MAX_PATH) return false;
    std::wstring d = std::wstring(tmp) + L"eclipse_dock";
    CreateDirectoryW(d.c_str(), nullptr);   // exists already: fine
    g_dock_dir = narrow(d.c_str());
    return !g_dock_dir.empty();
}

// The docked panel, described to the overlay page as a SNAPSHOT ("DOCKP:"),
// like STATE and SNIPS: deduped in hud_push, replayed on HUD_READY -- which is
// what carries a docked panel across an overlay rebuild (detach/reattach) with
// no special case, because the served file and this description both outlive
// the window. Carries the owning RUN ID so the page can stamp it onto the
// SNIPMSG relay -- the docked path's substitute for the closure a floating
// window gets for free.
void push_dock() {
    auto sw = dock_owner();
    std::ostringstream j;
    if (!sw || !sw->docked) {
        j << "{\"on\":false}";
    } else {
        j << "{\"on\":true,\"run\":" << sw->run
          << ",\"w\":" << sw->w << ",\"h\":" << sw->h
          << ",\"corner\":" << sw->dock_corner
          << ",\"dx\":" << sw->dock_dx << ",\"dy\":" << sw->dock_dy
          << ",\"pass\":" << (sw->dock_pass ? "true" : "false")
          << ",\"src\":\"" << json_escape(sw->dock_src) << "\"}";
    }
    hud_post("DOCKP:" + j.str());
}

// Re-derive the iframe's src from the replay state. html content is WRITTEN TO
// A FILE under the mapped folder -- NavigateToString has no equivalent for an
// iframe that must keep its own origin -- with a fresh name each time, so no
// cache decision is ever part of the picture. The previous file is deleted
// rather than left to accumulate under a snippet that re-sets its page.
static void dock_update_src(SnipWin& sw) {
    if (!sw.docked) return;
    if (!sw.dock_file.empty()) {
        DeleteFileW(widen(sw.dock_file).c_str());
        sw.dock_file.clear();
    }
    if (sw.ckind == 2) {
        sw.dock_src = sw.content;   // already a full URL; fx normalises paths
    } else if (dock_dir_ensure()) {
        char fb[64];
        snprintf(fb, sizeof fb, "page%u_%u.html", g_dock_gen, ++sw.dock_page);
        std::string path = g_dock_dir + "\\" + fb;
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (f) {
            if (sw.ckind == 1) f << sw.content;
            else f << "<!doctype html><html><body style=\"margin:0;"
                      "background:#141418\"></body></html>";
            f.close();
            sw.dock_file = path;
            dock_apply_mapping();
            sw.dock_src = "https://" + sw.dock_host + "/" + fb;
        } else {
            sw.dock_src.clear();
        }
    } else {
        sw.dock_src.clear();
    }
    push_dock();
}

// Push the replay state into whichever backing currently holds the window.
// Called after a create and after a dock toggle. The floating host paints its
// background opaque (kit ground); the docked panel sits in the alpha-0
// overlay, so a page that sets its own background looks identical and one
// that does not now shows the game -- the capability, opt-in by leaving the
// background unpainted.
static void snip_apply_content(SnipWin& sw) {
    if (sw.docked) {
        dock_update_src(sw);
    } else {
        if (!sw.wv) return;
        if (sw.ckind == 1)      sw.wv->set_html(sw.content);
        else if (sw.ckind == 2) sw.wv->navigate(sw.content);
        else sw.wv->set_html(
            "<!doctype html><html><body style=\"margin:0;background:#141418;"
            "color:#dcdcdc;font:14px system-ui,Segoe UI,sans-serif\"></body></html>");
    }
}

// Build the FLOATING (windowed) backing. Extracted so snip_open and a dock
// toggle share it.
static bool snip_build_floating(SnipWin& sw) {
    try {
        // The constructor pumps a nested loop until WebView2 spins up
        // (~200-500 ms). That's the reason a window op has real latency.
        sw.wv.reset(new webview::webview(false, nullptr));
    } catch (const std::exception& e) {
        ec::logf("snippet window: WebView2 init failed (%s)", e.what());
        sw.wv.reset();
        return false;
    } catch (...) {
        ec::logf("snippet window: WebView2 init failed");
        sw.wv.reset();
        return false;
    }

    sw.wv->set_title(sw.title);
    sw.wv->set_size(sw.w, sw.h, WEBVIEW_HINT_NONE);

    // WebView2's default background is WHITE and shows wherever the surface is
    // not painted yet. Harmless on a floating window against a desktop, but
    // set it to the kit ground so the first frame is not a flash.
    if (auto ctrl = sw.wv->browser_controller(); ctrl.ok() && ctrl.value()) {
        static const GUID kController2 =
            { 0xc979903e, 0xd4ca, 0x4228, { 0x92, 0xeb, 0x47, 0xee, 0x3f, 0xa9, 0x6e, 0xab } };
        auto* c = static_cast<ICoreWebView2Controller*>(ctrl.value());
        ICoreWebView2Controller2* c2 = nullptr;
        if (SUCCEEDED(c->QueryInterface(kController2, (void**)&c2)) && c2) {
            COREWEBVIEW2_COLOR bg{ 255, 0x0d, 0x0d, 0x10 };   // --bg-1
            c2->put_DefaultBackgroundColor(bg);
            c2->Release();
        }
    }

    // Page-to-snippet channel. JS calls window.eclipse_send("payload"); the
    // owning run reads it via fx_window_get_msg. The library wraps args as a
    // JSON array (["payload"]), so we peel one string off. The closure
    // captures the RUN ID -- this is what makes the floating path's message
    // isolation free. Return "" to resolve the JS promise harmlessly.
    unsigned run = sw.run;
    sw.wv->bind("eclipse_send", [run](const std::string& req) -> std::string {
        snip_enqueue_msg_run(run, extract_first_json_arg(req));
        return "\"\"";
    });

    if (auto res = sw.wv->window(); res.ok() && res.value()) {
        HWND hwnd = (HWND)res.value();
        // The library ctor doesn't ShowWindow -- patched out to hide the
        // blank-chrome flash on main-window startup. Show it now that its
        // binding and page are set. The WM_CLOSE hook goes on too, so a user
        // X-click stops the owning run.
        sw.orig_wndproc = (WNDPROC)SetWindowLongPtrW(
            hwnd, GWLP_WNDPROC, (LONG_PTR)snip_wndproc_hook);
    }
    return true;
}

// Build the DOCKED backing: an iframe panel in the overlay page. Content is
// applied by the caller through snip_apply_content, same as the floating
// build; all this does is claim the dock, claim a fresh origin and make the
// overlay exist. THE DOCK IS SINGULAR: first-come, a second claimant is
// refused BY NAME (docs/multi-run.md -- per-corner docking is a separate
// later job) and its window stays floating.
static bool snip_build_docked(SnipWin& sw) {
    if (g_dock_run && g_dock_run != sw.run) {
        std::string who = core_run_label(g_dock_run);
        ec::logf("x the dock is in use by %s - the window stays floating\n",
                 who.empty() ? "another run" : who.c_str());
        return false;
    }
    if (!ensure_overlay()) return false;
    g_dock_run = sw.run;
    g_dock_gen++;
    sw.dock_page = 0;
    char hb[32];
    snprintf(hb, sizeof hb, "snip%u.eclipse", g_dock_gen);
    sw.dock_host = hb;
    sw.docked = true;
    return true;
}

static void snip_open(unsigned run, const char* title, int w, int h) {
    if (w < 200)  w = 200;
    if (w > 3000) w = 3000;
    if (h > 3000) h = 3000;
    auto swp = win_get(run);
    SnipWin& sw = *swp;
    // THE HEIGHT FLOOR IS FOR FLOATING WINDOWS ONLY.
    //
    // A floating panel needs room for its own chrome to be usable at all, so
    // 100 stands there. A DOCKED one has no chrome -- it is a rect the page
    // paints -- and the panels collapse to a header strip about 35px tall.
    // Clamping those to 100 left ~65px of rect below the header that the page
    // does not paint: transparent, but still the panel's, so it swallows
    // clicks meant for the game and reads as dead space under the bar.
    // Reported against autoparry and the hub together, which is the tell that
    // it was the host and not either page.
    if (h < (sw.docked ? 28 : 100)) h = sw.docked ? 28 : 100;
    sw.title = title ? title : "Snippet";
    sw.w = w; sw.h = h;

    snip_gc_if_dead(sw);

    if (sw.docked || snip_hwnd(sw)) {
        // Already open: retitle and resize, leave content alone so a snippet
        // that resizes mid-run doesn't wipe its own DOM. The docked panel has
        // no caption to retitle; its size change is a page relayout, and the
        // page re-reports its rects when it applies the snapshot.
        if (sw.docked) {
            push_dock();
        } else if (sw.wv) {
            sw.wv->set_title(sw.title);
            sw.wv->set_size(sw.w, sw.h, WEBVIEW_HINT_NONE);
        }
        return;
    }

    // Fresh window. Build the kind the run's dock mode calls for -- a snippet
    // is allowed to dock before it opens. Docking might fall back to floating
    // on a machine whose overlay refuses to exist (or when another run holds
    // the dock); better a window of the wrong kind than a snippet driving
    // nothing.
    sw.busy = true;
    bool ok = (sw.mode != 0) ? snip_build_docked(sw) : snip_build_floating(sw);
    if (!ok && sw.mode != 0) {
        ec::logf("! dock unavailable - showing a floating window instead\n");
        ok = snip_build_floating(sw);
    }
    if (ok) {
        snip_apply_content(sw);
        HWND hw = snip_hwnd(sw);
        if (hw && !sw.docked) ShowWindow(hw, SW_SHOWNOACTIVATE);
        // The docked panel is shown by the overlay page when it applies the
        // DOCKP snapshot; there is no window of its own to ShowWindow.
    }
    sw.busy = false;
}

// Tear down whichever backing is live, WITHOUT clearing the replay state --
// a dock toggle rebuilds immediately after.
static void snip_teardown_backing(SnipWin& sw) {
    if (sw.docked) {
        sw.docked = false;
        push_dock();            // the page removes the iframe: that IS the destroy
        dock_clear_mapping();   // and the origin dies with the panel
        if (!sw.dock_file.empty()) {
            DeleteFileW(widen(sw.dock_file).c_str());
            sw.dock_file.clear();
        }
        sw.dock_src.clear();
        sw.dock_host.clear();
        g_dock_run = 0;         // the dock is free for the next claimant
        // The overlay window itself is the panel's business: if the console
        // panel is off too, the next hud_tick sees nothing wanting it and
        // takes it down.
        return;
    }
    if (!sw.wv) return;
    auto res = sw.wv->window();
    HWND h = res.ok() ? (HWND)res.value() : nullptr;
    if (h && IsWindow(h)) DestroyWindow(h);   // not terminate() -- that quits the app
    sw.wv.reset();
    sw.orig_wndproc = nullptr;
}

// Switch a run's window between floating and docked, preserving what it shows.
static void snip_set_dock_mode(unsigned run, int mode) {
    bool want_docked = (mode != 0);
    auto swp = win_get(run);   // dock-before-open must have somewhere to land
    SnipWin& sw = *swp;
    sw.mode = mode;
    if (!sw.docked && !snip_hwnd(sw)) return;   // no window yet; snip_open honours sw.mode
    if (want_docked == sw.docked) {
        // Same kind, new parameters: a docked snippet re-calling
        // fx_window_dock(1, corner, dx, dy) is how it MOVES. The page owns
        // the placement now, so moving is a snapshot, not a SetWindowPos.
        if (sw.docked) push_dock();
        return;
    }

    // Check hostability BEFORE tearing the floating window down: failing
    // between the two used to leave the snippet with no window at all. The
    // dock being held by another run is the same shape of failure and gets
    // the same treatment -- refuse first, tear down nothing.
    if (want_docked && g_dock_run && g_dock_run != sw.run) {
        std::string who = core_run_label(g_dock_run);
        ec::logf("x the dock is in use by %s - the window stays floating\n",
                 who.empty() ? "another run" : who.c_str());
        return;
    }
    if (want_docked && !ensure_overlay()) {
        ec::logf("! dock unavailable - the window stays floating\n");
        return;
    }

    sw.busy = true;
    snip_teardown_backing(sw);
    bool ok = want_docked ? snip_build_docked(sw) : snip_build_floating(sw);
    if (ok) {
        snip_apply_content(sw);
        HWND hw = snip_hwnd(sw);
        if (hw && !sw.docked) ShowWindow(hw, SW_SHOWNOACTIVATE);
    } else {
        ec::logf("snippet window: could not switch dock mode");
    }
    sw.busy = false;
}

static void snip_html(unsigned run, const char* html) {
    auto swp = win_find(run);
    if (!swp) return;
    snip_gc_if_dead(*swp);
    swp->ckind = 1;
    swp->content = html ? html : "";
    if (!snip_hwnd(*swp)) return;   // docked included: store-only, as always
    snip_apply_content(*swp);
}

static void snip_navigate(unsigned run, const char* url) {
    auto swp = win_find(run);
    if (!swp) return;
    snip_gc_if_dead(*swp);
    swp->ckind = 2;
    swp->content = url ? url : "about:blank";
    if (!snip_hwnd(*swp)) return;   // docked included: store-only, as always
    snip_apply_content(*swp);
}

static void snip_eval(unsigned run, const char* js) {
    auto swp = win_find(run);
    if (!swp) return;
    snip_gc_if_dead(*swp);
    if (!js || !*js) return;
    // Eval is transient DOM poking and is NOT part of the replay state: a
    // snippet drives its window from a loop, so it re-derives after a dock
    // toggle rebuilds the page. Replaying eval would double-apply.
    //
    // Docked, the script has to cross into the iframe: the overlay page
    // relays it (DOCKEVAL -> postMessage -> the injected frame shim evals).
    // The console page never evaluates the string itself, only carries it.
    // Same dispatch queue as DOCKP, so eval cannot overtake the panel it
    // targets. One dock, so the message needs no run id -- the panel it
    // reaches is the caller's own by the owner guard above.
    if (swp->docked) hud_post(std::string("DOCKEVAL:") + js);
    else if (swp->wv) swp->wv->eval(js);
}

// Close ONE run's window and forget its replay state.
static void snip_close_run(unsigned run) {
    auto swp = win_find(run);
    if (!swp) return;
    snip_teardown_backing(*swp);
    win_erase(run);
}

// Close EVERY window (console WM_CLOSE, teardown, detach's stop-all path
// closes per run instead). Every extra webview window must be destroyed
// before the main window's, or the library's shared refcount never reaches
// zero and the process outlives the console as an orphaned window.
void snip_close() {
    std::vector<unsigned> ids;
    {
        std::lock_guard<std::mutex> lk(g_wins_mux);
        for (auto& kv : g_wins) ids.push_back(kv.first);
    }
    for (unsigned id : ids) snip_close_run(id);
}

// Sinks. The run id was resolved on the SNIPPET'S thread by the fx export
// (core_run_current_id there), because these dispatch to the UI thread where
// the current-run TLS is nobody's. window_open_sink BLOCKS the snippet
// thread until snip_open finishes on the UI thread. Otherwise a snippet that
// calls open then immediately html races: html's dispatched lambda runs
// inside the nested pump of the ctor and sees no window yet. Blocking gives
// the snippet FIFO semantics.
//
// html/eval/close are non-blocking dispatch -- they arrive at the UI thread in
// call order (PostMessageW is FIFO for the same window), so the snippet's
// call order is preserved without a wait per call.
void window_open_sink(unsigned run, const char* title, int w, int h) {
    if (!ui_page_live()) return;
    std::string t = title ? title : "";
    std::promise<void> p;
    auto f = p.get_future();
    ui_dispatch([&, run, t]() { snip_open(run, t.c_str(), w, h); p.set_value(); });
    f.wait();
}
void window_html_sink(unsigned run, const char* html) {
    if (!ui_page_live()) return;
    std::string s = html ? html : "";
    ui_dispatch([run, s]() { snip_html(run, s.c_str()); });
}
void window_nav_sink(unsigned run, const char* url) {
    if (!ui_page_live()) return;
    std::string s = url ? url : "";
    ui_dispatch([run, s]() { snip_navigate(run, s.c_str()); });
}
void window_eval_sink(unsigned run, const char* js) {
    if (!ui_page_live()) return;
    std::string s = js ? js : "";
    ui_dispatch([run, s]() { snip_eval(run, s.c_str()); });
}
void window_close_sink(unsigned run) {
    if (!ui_page_live()) return;
    ui_dispatch([run]() { snip_close_run(run); });
}
bool window_get_msg_sink(unsigned run, std::string* out) {
    // No UI dispatch -- the queues are mutex-protected. Snippet thread calls
    // this from fx_window_get_msg's implementation, gets the answer in place.
    auto swp = win_find(run);
    if (!swp) return false;
    std::lock_guard<std::mutex> lk(swp->msg_mux);
    if (swp->msgs.empty()) return false;
    *out = std::move(swp->msgs.front());
    swp->msgs.pop_front();
    return true;
}

// dock_tick IS GONE, and that is the fix. The docked panel used to be a
// second window whose placement and z-order ran here, sharing hud_tick's
// queries and skipping past the panel in the slot above the game -- and two
// windows meant two sequential raises with a composable frame between them,
// the last cause of the tab-in blink. The panel is now an element of the
// overlay page: placement is CSS anchored to the corner (free across game
// moves/resizes, no tick arithmetic), z-order is the overlay's, and its
// clickable rect rides the page's RECTS report.

// fx_window_passthrough. Non-blocking, unlike window_dock_sink: this changes
// only how the panel's rect is REPORTED, it never rebuilds the window, so
// there is no nested pump for a following call to be dispatched into.
void window_pass_sink(unsigned run, int on) {
    if (!ui_page_live()) return;
    ui_dispatch([run, on]() {
        auto swp = win_get(run);
        if (swp->dock_pass == (on != 0)) return;
        swp->dock_pass = (on != 0);
        // Only the docked panel HAS a reported rect; floating windows own
        // their clicks as real HWNDs. Remembered either way, so a snippet may
        // set it before it docks.
        if (swp->docked) push_dock();
    });
}

void window_dock_sink(unsigned run, int mode, int corner, int dx, int dy) {
    if (!ui_page_live()) return;
    // BLOCKS the snippet thread until the switch finishes, for the same reason
    // window_open_sink does: switching KIND rebuilds the window, and the
    // rebuild pumps a nested message loop (the floating webview ctor / the
    // composed create). A following non-blocking fx_window_* -- above all the
    // fx_window_close that a snippet runs right after undocking on exit --
    // would otherwise be dispatched INTO that pump and run before the new
    // window exists, closing nothing and leaking the rebuilt window. Blocking
    // gives the snippet FIFO semantics and keeps the rebuild un-nested.
    std::promise<void> p;
    auto f = p.get_future();
    ui_dispatch([&]() {
        auto swp = win_get(run);
        swp->dock_corner = (corner < 0 || corner > 3) ? 1 : corner;
        swp->dock_dx     = dx;
        swp->dock_dy     = dy;
        // Switch the window's KIND to match. Docking moves the page into an
        // overlay iframe panel; undocking rebuilds the ordinary floating
        // window. snip_set_dock_mode replays the page into the new backing,
        // re-describes a panel whose corner/insets changed, and is a no-op
        // when nothing is open yet.
        snip_set_dock_mode(run, mode);
        p.set_value();
    });
    f.wait();
}

// The three facts the host still needs to read back: the overlay-existence
// gate and the settings parser consult the docked flag, and the overlay's
// virtual-host mapping (dock_apply_mapping / dock_clear_mapping, which run on
// the overlay's core and so live with it) needs the origin.
bool snip_docked() { return g_dock_run != 0; }
std::string dock_host() {
    auto sw = dock_owner();
    return sw ? sw->dock_host : std::string();
}
std::string dock_dir()  { return g_dock_dir; }
