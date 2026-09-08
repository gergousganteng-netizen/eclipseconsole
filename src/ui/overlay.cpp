// overlay.cpp -- the in-game overlay machine, moved out of eclipse_ui.cpp
// WHOLE. Everything here -- the dedicated GDI overlay window, the composed
// in-game panel and badge, the overlay side of docked snippet panels, the
// page-reported hit-test region, focus mode, the hooks, the band rule and
// app mode -- is one subsystem with cross-referential invariants (z-order,
// click pre-emption, ownership, message protocol). It was relocated as one
// unit precisely so those comments still point at each other and the whole
// stays readable in one pass; do NOT split it into pieces. eclipse_ui.cpp
// keeps the entry point, the frameless window, the message pump and the page
// protocol, and the host services this machine calls back into through
// host.h; overlay.h is the seam in the other direction.

#define WIN32_LEAN_AND_MEAN
// The in-game panel hosts WebView2 through DirectComposition on a
// WS_EX_NOREDIRECTIONBITMAP window; both are gated behind Windows 8+ API
// levels and MinGW still defaults to 0x0601. Raising the target only reveals
// newer declarations -- the app already requires Windows 10+ for WebView2.
#undef  _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#undef  WINVER
#define WINVER       0x0A00
#include <windows.h>
#include <windowsx.h>      // GET_X_LPARAM
#include <dwmapi.h>
#include <dcomp.h>         // the in-game panel's composition visual tree

#include <atomic>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "webview/webview.h"
#include "eclipse_core.h"

#include "host.h"
#include "overlay.h"
#include "attach.h"
#include "dpi.h"
#include "log.h"
#include "snipwin.h"

// APP MODE: the console's own window (g_hwnd), shown OVER the attached client
// as a tool window. One flag up here because overlay_wanted() and the settings
// parser consult it; the mechanics live far lower with the overlay plumbing.
// There is no frame routing any more -- the app IS the desktop page in the
// desktop window, so every push goes where it always went.
static bool g_app_mode  = false;
// The two choreography legs (see the app-mode section for the sequence).
// Up here because the foreground hook and the tick consult them.
static bool  g_app_opening    = false;  // APPOPEN sent, window not yet shown
static bool  g_app_closing    = false;  // APPCLOSE sent, window not yet hidden
// OPEN LEG ONLY: the travel box is held ABOVE the app so it covers the newly
// shown WebView2 window until that window has painted a frame. Set at
// app_open_finish (the show), cleared by app_open_uncover -- on the APP
// PAGE's paint witness (APPPAINTED, two rAFs after the show), or by the
// tick's 250ms belt if that message is lost. Without the hold the app is
// promoted TOPMOST (box moved BELOW it) while WebView2 is still blank -- the
// game shows through the app rect for ~30ms and the badge blinks with it
// (Fault A, 2026-09-04; DwmFlush waits on DWM, not on the WebView2 renderer).
//
// WHILE IT HOLDS, NOTHING MAY CROSS THE APP ABOVE THE OVERLAY, and once it
// clears nothing may cross the overlay back above the app. The first cut
// (2026-09-04) did app_swp(TOPMOST) + raise_overlays() + force_foreground
// under the hold, then waited for the hud page's box drop: the app crossed
// above the box twice (the promote, then the activation raise) and was
// re-covered by raise_overlays' TOPMOST branch each time, so a painted app
// was SEEN for a frame and then the box sat on top of it for 56-63ms; and
// the hud page dropped the box a few ms before the host glued the overlay
// under, so a docked panel showed over the app for 1-3 frames (DXGI
// duplication, five opens: box 8-11 frames, dock 1-3). Now the app is
// promoted and the overlay glued under it BEFORE the page is told to drop
// the box, so the drop happens beneath the app and the app crosses the box
// exactly once, painted. The close leg does NOT set this: there the box is
// correctly BENEATH the already-painted app and the hide reveals it (fault
// 4's fix), so the two legs are asymmetric on purpose.
static bool  g_app_box_covering = false;
static DWORD g_app_cover_at     = 0;    // stamp for the box-covering belt
// A travel's click-through is in force (open begin -> APPSHOWN, close begin ->
// APPIDLE). Deliberately NOT the phase flags: app_close_finish clears
// g_app_closing before the box travels home, so the VISIBLE half of every
// close falls outside both flags -- tying the cursor hold to them measured as
// 14-16 hidden samples per close. This flag spans click to travel-end.
static bool  g_travel_ct      = false;  // see ct_follow_apply for the rule
static bool  g_app_close_hand = false;  // hand focus to the game when it lands
static bool  g_app_close_keep_owner = false;  // keep the app/overlay owned through THIS close (a third app was foreground)
static DWORD g_app_phase_at   = 0;      // watchdog stamp for both legs

// OVERLAY MODE, the user's standing preference (Settings), as opposed to
// g_app_mode which is "the app is open right now". Set means: while a client
// is attached the console LIVES in the game -- no desktop window, and the
// badge opens the APP rather than the panel. Held here rather than only in
// the page's localStorage because the host needs it before any page has
// loaded, and because the page that owns it may itself be inside the overlay.
static bool g_overlay_pref = false;

// The in-game panel owns an HWND, so hud_push is implemented at the bottom
// of this file with the rest of the window plumbing. Declared up here (and
// in overlay.h) because the host's log and state funnels feed it.
void hud_push(const std::string& msg);

// The snippet-facing primitives, which the UI links against anyway -- fx.h is
// not included here (it is the snippet surface, and pulls its own helpers), so
// the four that are needed are declared directly.
extern "C" {
    size_t   fx_rdblk(uint64_t addr, void* buf, size_t n);
    uint64_t fx_off(const char* key);
    uint64_t fx_character(void);
    uint64_t fx_part(uint64_t model, const char* name);
    uint64_t fx_place(void);
}
void hud_tick();
void hud_destroy();
// Unhook BEFORE the process goes. WH_MOUSE_LL routes every mouse event in the
// session through this process; while it is registered and we are not
// answering, each event waits out LowLevelHooksTimeout -- felt as the CURSOR
// FREEZING. See the definition.
void overlay_release_hooks();
// Owns what serves every overlay panel: the hooks and the focus hand-back.
void overlay_tick();
void health_tick();
// Snapshots the overlay page can be handed at any time (deduped in hud_push,
// replayed on HUD_READY like STATE/RUNSTATE/SNIPS).
static void push_hudvis();
static void push_overlay_pref();
// Up here rather than beside the panel code: the settings parser runs long
// before that and has to be able to flip it. Also cleared for the session if
// the panel's WebView2 refuses to start.
static bool g_hud_enabled = true;
// APP MODE (g_app_mode declared at the top of this file): the console's own WINDOW,
// positioned over the client while the overlay animates the hand-off
// (overlay-plan.md step 3, re-architected 2026-08-22 -- the app is a real
// window again, not an iframe). It is a THIRD reason the overlay window must
// exist (the travel box and the close badge live there), so the same gates
// that consult g_hud_enabled / snip_docked() consult it too.
static void app_open_begin();
static void app_open_finish();
static void app_open_uncover();
void app_close_begin(bool hand_to_game);
// Lets clicks fall through the overlay while a travel is animating; see
// the definition for why the region alone cannot express that.
static void overlay_clickthrough(bool on);
// The same for the app window while it waits to hide during a close.
static void app_clickthrough(bool on);
static void app_close_finish();
void app_abort_restore();
static void app_target_rect(int cw, int ch, int* l, int* t, int* w, int* h);

// Does anything want the overlay window to exist? Four independent reasons,
// and they were three separate copies of the same disjunction until one of
// them drifted. Duplication of exactly this shape is what silently stripped a
// docked window of its promote when the panel was switched off.
static bool overlay_wanted() {
    return g_hud_enabled || snip_docked() || g_app_mode || g_overlay_pref;
}

// ----------------------------- overlay pipe ---------------------------------
//
// Data updates from a snippet are CONFLATED, not batched. The renderer only
// wants the LATEST target list -- stale ones are useless, and dropping them
// on the floor is the correct behaviour. Same one-flush-in-flight bound as
// the log pipeline for the same reason (STOP/ARM share the pump).
//
// Mode changes (fx_overlay_on/off) go through a separate direct dispatch:
// they must always deliver, or the window gets stuck transparent-and-topmost
// while the "off" signal sits behind a queue. Rare events, no throttling
// needed. The mode sink also does the WINDOW state change on the UI thread
// (see apply_overlay_mode, defined near the WM_ handlers below).

// Forward-declared: implementations sit near WinMain because they own an HWND
// lifecycle that logically groups with the other window plumbing.
static void apply_overlay_mode(bool on);
static void overlay_set_targets(const char* json);

static std::mutex        g_ovlmux;
static std::string       g_ovlslot;                // JSON, latest wins
static bool              g_ovlhave  = false;
static std::atomic<bool> g_ovlflush{false};

static void flush_overlay() {
    g_ovlflush = false;
    std::string payload;
    {
        std::lock_guard<std::mutex> lk(g_ovlmux);
        if (!g_ovlhave) return;
        payload.swap(g_ovlslot);
        g_ovlhave = false;
    }
    // Overlay data goes to the dedicated GDI window (see overlay_set_targets),
    // NOT to the console's WebView2 -- the console UI has nothing to do with
    // the overlay.
    overlay_set_targets(payload.c_str());
}

void overlay_data_sink(const char* json) {
    if (!ui_page_live() || !json) return;
    {
        std::lock_guard<std::mutex> lk(g_ovlmux);
        g_ovlslot = json;
        g_ovlhave = true;
    }
    if (!g_ovlflush.exchange(true)) ui_dispatch(flush_overlay);
}

// Forward-declared. Implemented in the WM_ handler block below, on the UI
// thread; it changes the window's ex-style, resizes to the monitor rect, and
// sets the WebView2 background transparent (or restores everything on off).
static void apply_overlay_mode(bool on);

void overlay_mode_sink(int on) {
    if (!ui_page_live()) return;
    bool wanted = (on != 0);
    // Overlay is a separate GDI window; the console page is not involved.
    // Still hop to the UI thread -- CreateWindow/DestroyWindow have to run on
    // the thread that owns the message pump.
    ui_dispatch([wanted]() { apply_overlay_mode(wanted); });
}

// ---- the app's in-game position: DERIVED, never stored -----------------------
// ONE GEOMETRY, ONE STORE. The app window is the same HWND in app mode and in
// hud mode, so how big it is and where it sits is ONE fact, and window.txt
// (eclipse_ui.cpp) is the only place it lives. This file used to keep a second
// copy -- overlay_app.txt, a client-relative anchor -- and two stores that had
// to agree IS the bug that made position disagree between the modes for a week
// (docs/window-geometry.md records the five patches that tried to keep them in
// step). The file is gone; these are live session state only.
//
// The anchor still exists because the window must FOLLOW the client while it
// lives in the game -- a screen rect would strand it the moment the user moved
// or resized the Roblox window. It is DERIVED from the window's own screen
// rect when hud mode opens (app_anchor_capture), and re-derived when a drag
// ends. Nothing reads it before that, so there is no startup ordering to get
// wrong and no settings flag to consult too early.
static int  g_app_right = -1;   // inset of the app's right edge from the client's
static int  g_app_cy    = 0;    // centre Y in client px; 0 = never placed (centre)
static int  g_app_w     = 0;    // the size it carries into the game; 0 = default fit
static int  g_app_h     = 0;

// The screen rect WE last put the app window at (the open, the follow, the
// drag snap-back). The question at every open is "has the window moved on the
// desktop since we last placed it?", and this is the only honest way to ask
// it: the window is HIDDEN by the time the badge is clicked -- the overlay
// valve hides it the moment a client attaches -- so "is it visible now" answers
// the wrong question and kept the stale anchor for a window the user had
// visibly moved. Zero-width means never placed.
static RECT g_app_placed{};
static void app_placed(int l, int t, int w, int h) {
    g_app_placed = RECT{ l, t, l + w, t + h };
}
static bool app_moved_since_placed(const RECT& now) {
    if (g_app_placed.right <= g_app_placed.left) return true;   // never placed
    return now.left  != g_app_placed.left  || now.top    != g_app_placed.top ||
           now.right != g_app_placed.right || now.bottom != g_app_placed.bottom;
}

// Fold a SCREEN rect into the anchor, clamped inside the client. The one
// converter: hud mode's opening rect is whatever the desktop window was
// occupying a moment ago, which is what makes the two modes agree by
// construction rather than by two stores being kept in step.
static void app_anchor_capture(const RECT& win, const RECT& client) {
    int cw = client.right - client.left, ch = client.bottom - client.top;
    int w  = win.right - win.left,       h  = win.bottom - win.top;
    if (cw <= 0 || ch <= 0 || w <= 0 || h <= 0) return;
    // The size has to fit the client with the same 12px margins app_target_rect
    // uses, or a window sized against a large monitor strands itself in a small
    // client -- and then the anchor derived from it would be clamped too.
    if (w > cw - 24) w = cw - 24;
    if (h > ch - 24) h = ch - 24;
    if (w < 320) w = 320;
    if (h < 240) h = 240;
    g_app_w = w; g_app_h = h;
    g_app_right = client.right - win.right;
    g_app_cy    = (win.top - client.top) + h / 2;
    int maxr = cw - w; if (maxr < 0) maxr = 0;
    if (g_app_right > maxr) g_app_right = maxr;
    if (g_app_right < 0)    g_app_right = 0;
    int half = h / 2;
    if (g_app_cy < half)      g_app_cy = half;
    if (g_app_cy > ch - half) g_app_cy = ch - half;
}

// Coming back out of the game: SHOW IT, and change nothing else.
//
// Both exits used to SetWindowPlacement a snapshot taken before hud mode
// opened, which teleported the window back to wherever it had been on the
// desktop and threw away every move made in the game. Keeping the maximised
// half of that snapshot was the same mistake in miniature: un-maximise inside
// the game and it came back full-screen regardless. There is no snapshot any
// more. It is one window; switching modes changes its chrome, its owner and
// its z-order, and nothing about its geometry or its show state.
static void app_restore_show(UINT cmd) {
    if (!ui_main_hwnd()) return;
    ShowWindow(ui_main_hwnd(), (int)cmd);
}

// ---- overlay: dedicated top-level GDI window --------------------------------
// The console stays exactly as it is. Overlay mode instead creates its OWN
// top-level window -- WS_POPUP, WS_EX_LAYERED with a colour-key stencil,
// WS_EX_TOOLWINDOW (no taskbar), WS_EX_TRANSPARENT (clicks fall through),
// WS_EX_NOACTIVATE (does not steal focus from the game).
//
// Why not WebView2 in that window? WebView2 renders through DirectComposition,
// which bypasses LWA_COLORKEY -- you get an opaque magenta rectangle instead
// of transparency (verified). GDI drawing composites through the layered path
// normally, so the colour key stencils out cleanly. HTML/CSS-styled overlays
// would need a DComp-hosted WebView2 (larger change) -- that door stays open
// for later.
//
// apply_overlay_mode is a one-liner that dispatches to the create/destroy
// pair (implemented in the next section, with the window's paint code).
static void create_overlay_window();
void destroy_overlay_window();

static void apply_overlay_mode(bool on) {
    if (on) create_overlay_window();
    else    destroy_overlay_window();
}

// ---- overlay window implementation -----------------------------------------
//
// Everything below is the GDI overlay window: HWND lifecycle, WM_PAINT
// drawing, and a small JSON parser that turns the snippet's payload into
// something the paint code can iterate.

struct OvlTarget {
    std::string name;
    float sx = 0, sy = 0, w = 0, h = 0;
    float dist = 0;
    // Non-zero turns the entry into a CIRCLE of this pixel radius, and then
    // sx/sy mean its CENTRE rather than a top-left corner. Added for the
    // cursor aim's FOV ring; a ring has no name or distance to label.
    float r = 0;
    // 0xRRGGBB for the outline and the name, or -1 for the default white.
    // Lets a snippet say "this one is different" -- a teammate, a low-health
    // target -- without the overlay needing to know what the difference means.
    int rgb = -1;
};

static HWND                     g_overlay_hwnd = nullptr;
static std::mutex               g_ovl_targets_mux;
static std::vector<OvlTarget>   g_ovl_targets;
static bool                     g_overlay_cls_registered = false;

// Colour-key colour. Anywhere the window paints this exact RGB, Windows
// composites transparent instead. Picked as pure magenta because nothing else
// in the marker palette uses it, so anti-aliasing does not produce halos.
static const COLORREF kOverlayKey = RGB(255, 0, 255);

// Minimal JSON-array-of-objects parser matched to what esp_overlay.cpp
// serialises: `[{"n":"...","sx":N,"sy":N,"w":N,"h":N,"d":N},...]`.
// Not general-purpose (no nested objects, no arrays inside values, no unicode
// escapes) -- just enough for our payload.
static void skip_ws(const char*& p) { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; }

static bool parse_string(const char*& p, std::string& out) {
    skip_ws(p);
    if (*p != '"') return false;
    p++;
    out.clear();
    while (*p && *p != '"') {
        if (*p == '\\' && *(p+1)) { p++; out += *p++; }
        else                       { out += *p++; }
    }
    if (*p != '"') return false;
    p++;
    return true;
}

static bool parse_number(const char*& p, float& out) {
    skip_ws(p);
    char* end = nullptr;
    out = strtof(p, &end);
    if (end == p) return false;
    p = end;
    return true;
}

static bool parse_target(const char*& p, OvlTarget& t) {
    skip_ws(p);
    if (*p != '{') return false;
    p++;
    while (*p && *p != '}') {
        skip_ws(p);
        std::string key;
        if (!parse_string(p, key)) return false;
        skip_ws(p);
        if (*p != ':') return false;
        p++;
        skip_ws(p);
        if (key == "n") {
            if (!parse_string(p, t.name)) return false;
        } else {
            float v;
            if (!parse_number(p, v)) return false;
            if      (key == "sx") t.sx  = v;
            else if (key == "sy") t.sy  = v;
            else if (key == "w")  t.w   = v;
            else if (key == "h")  t.h   = v;
            else if (key == "d")  t.dist = v;
            else if (key == "r")  t.r   = v;
            else if (key == "c")  t.rgb = (int)v;
            // unknown keys: silently ignored (forward-compatible)
        }
        skip_ws(p);
        if (*p == ',') { p++; continue; }
        break;
    }
    if (*p != '}') return false;
    p++;
    return true;
}

static void overlay_set_targets(const char* json) {
    std::vector<OvlTarget> out;
    if (json && *json) {
        const char* p = json;
        skip_ws(p);
        if (*p == '[') {
            p++;
            skip_ws(p);
            while (*p && *p != ']') {
                OvlTarget t;
                if (!parse_target(p, t)) break;
                out.push_back(std::move(t));
                skip_ws(p);
                if (*p == ',') { p++; continue; }
                break;
            }
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_ovl_targets_mux);
        g_ovl_targets.swap(out);
    }
    if (g_overlay_hwnd) InvalidateRect(g_overlay_hwnd, nullptr, FALSE);
}

// Draw one line of text with an 8-neighbour black outline plus a coloured
// fill on top. GDI has no native outlined text, but 9 DrawText calls per
// label is cheap at overlay counts (20 targets max), and it reads clearly on
// any Roblox background.
static void draw_outlined_text(HDC hdc, int x, int y, const std::wstring& s, COLORREF fill) {
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(0, 0, 0));
    for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) {
        if (!dx && !dy) continue;
        TextOutW(hdc, x + dx, y + dy, s.c_str(), (int)s.size());
    }
    SetTextColor(hdc, fill);
    TextOutW(hdc, x, y, s.c_str(), (int)s.size());
}

static LRESULT CALLBACK overlay_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);

        RECT rc;
        GetClientRect(h, &rc);

        // Double-buffer through a memory DC so single-frame flashes never
        // occur -- the client area is a whole monitor, painting it twice is
        // still cheap on any modern GPU.
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HBITMAP oldBmp = (HBITMAP)SelectObject(mem, bmp);

        // Wipe to the colour-key colour so the whole surface starts
        // transparent, then draw markers on top.
        HBRUSH keyBrush = CreateSolidBrush(kOverlayKey);
        FillRect(mem, &rc, keyBrush);
        DeleteObject(keyBrush);

        // Snapshot targets under the lock, so paint runs against a stable
        // list even if a new posting lands mid-frame.
        std::vector<OvlTarget> targets;
        {
            std::lock_guard<std::mutex> lk(g_ovl_targets_mux);
            targets = g_ovl_targets;
        }

        // Set up drawing objects once for the whole batch.
        //
        // 1 px white marker instead of 2 px red -- less visually noisy, and
        // white works better on almost any Roblox background (day/night,
        // grass/asphalt) than a single tuned red would.
        HPEN whitePen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
        HPEN oldPen = (HPEN)SelectObject(mem, whitePen);
        HBRUSH oldBrush = (HBRUSH)SelectObject(mem, GetStockObject(NULL_BRUSH));

        // System default UI font (whatever Windows is set to). Non-bold at 14
        // for names; the outlined-text pass gives the contrast without a bold
        // weight. Distance stays a bit smaller.
        HFONT nameFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                     CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                     DEFAULT_PITCH | FF_SWISS, nullptr);
        HFONT distFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                     CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                     DEFAULT_PITCH | FF_SWISS, nullptr);
        HFONT oldFont = (HFONT)SelectObject(mem, nameFont);

        // Swapped only when the colour actually changes, so a list that is all
        // one colour creates nothing extra. whitePen is never deleted here --
        // it is freed once at the end like everything else.
        HPEN     curPen = whitePen;
        COLORREF curCol = RGB(255, 255, 255);

        for (const OvlTarget& t : targets) {
            COLORREF want = (t.rgb >= 0)
                          ? RGB((t.rgb >> 16) & 0xFF, (t.rgb >> 8) & 0xFF, t.rgb & 0xFF)
                          : RGB(255, 255, 255);
            if (want != curCol) {
                SelectObject(mem, whitePen);
                if (curPen != whitePen) DeleteObject(curPen);
                curPen = (want == RGB(255, 255, 255))
                       ? whitePen : CreatePen(PS_SOLID, 1, want);
                SelectObject(mem, curPen);
                curCol = want;
            }

            // A ring rather than a box. sx/sy is its CENTRE here -- the one
            // place in this format where they are not a top-left corner --
            // and there is no name or distance to label, so it draws and
            // moves on.
            if (t.r > 0.0f) {
                int cx = (int)t.sx, cy = (int)t.sy, rr = (int)t.r;
                Ellipse(mem, cx - rr, cy - rr, cx + rr, cy + rr);
                continue;
            }

            int x = (int)t.sx, y = (int)t.sy;
            int ww = (int)t.w, hh = (int)t.h;

            Rectangle(mem, x, y, x + ww, y + hh);

            // Name centred above the box.
            SelectObject(mem, nameFont);
            std::wstring wn = widen(t.name);
            SIZE sz{};
            GetTextExtentPoint32W(mem, wn.c_str(), (int)wn.size(), &sz);
            int nx = x + ww / 2 - sz.cx / 2;
            int ny = y - sz.cy - 2;
            draw_outlined_text(mem, nx, ny, wn, want);

            // Distance centred below the box.
            char db[24];
            snprintf(db, sizeof db, "%d studs", (int)t.dist);
            std::wstring wd = widen(db);
            SelectObject(mem, distFont);
            GetTextExtentPoint32W(mem, wd.c_str(), (int)wd.size(), &sz);
            int dx = x + ww / 2 - sz.cx / 2;
            int dy = y + hh + 2;
            draw_outlined_text(mem, dx, dy, wd, RGB(255, 207, 122));
        }

        SelectObject(mem, oldPen);
        SelectObject(mem, oldBrush);
        SelectObject(mem, oldFont);
        if (curPen != whitePen) DeleteObject(curPen);
        DeleteObject(whitePen);
        DeleteObject(nameFont);
        DeleteObject(distFont);

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);

        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);

        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND) {
        // We paint the whole client in WM_PAINT; letting the default erase
        // run would flash the class brush.
        return 1;
    }
    if (m == WM_NCHITTEST) {
        // Belt-and-suspenders click-through in addition to WS_EX_TRANSPARENT:
        // some tools filter by hit-test result, and HTTRANSPARENT is the
        // universal "pretend I am not here".
        return HTTRANSPARENT;
    }
    return DefWindowProcW(h, m, w, l);
}

static void create_overlay_window() {
    if (g_overlay_hwnd) return;

    if (!g_overlay_cls_registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc   = overlay_wndproc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.hbrBackground = nullptr;                     // We paint every pixel.
        wc.hCursor       = nullptr;   // WS_EX_TRANSPARENT: cursor never lands here.
        wc.lpszClassName = L"EclipseOverlay";
        RegisterClassW(&wc);
        g_overlay_cls_registered = true;
    }

    HMONITOR mon = MonitorFromWindow(ui_main_hwnd() ? ui_main_hwnd() : GetDesktopWindow(),
                                     MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);

    // WS_POPUP: no frame, no titlebar, no min/max/close.
    // WS_EX_LAYERED       : lets us set a colour-key stencil.
    // WS_EX_TOOLWINDOW    : no taskbar entry, no Alt+Tab.
    // WS_EX_TOPMOST       : draws over Roblox.
    // WS_EX_TRANSPARENT   : mouse events pass through to whatever is below.
    // WS_EX_NOACTIVATE    : does not steal focus from the game.
    g_overlay_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST |
        WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        L"EclipseOverlay", L"",
        WS_POPUP,
        // Height MINUS ONE. The shell treats a topmost window whose rect
        // exactly covers a monitor as a fullscreen app and hides the taskbar
        // for it -- the overlay does not paint over the taskbar, the taskbar
        // withdraws. One pixel short fails that test, and the row given up is
        // the bottom edge of the screen where nothing is drawn anyway.
        mi.rcMonitor.left, mi.rcMonitor.top,
        mi.rcMonitor.right  - mi.rcMonitor.left,
        mi.rcMonitor.bottom - mi.rcMonitor.top - 1,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (!g_overlay_hwnd) return;

    SetLayeredWindowAttributes(g_overlay_hwnd, kOverlayKey, 0, LWA_COLORKEY);
    ShowWindow(g_overlay_hwnd, SW_SHOWNOACTIVATE);
}

void destroy_overlay_window() {
    if (!g_overlay_hwnd) return;
    DestroyWindow(g_overlay_hwnd);
    g_overlay_hwnd = nullptr;
    std::lock_guard<std::mutex> lk(g_ovl_targets_mux);
    g_ovl_targets.clear();
}

// One composed host per window, and there are TWO windows on these terms: the
// console's panel and a snippet's DOCKED window. They share this one
// implementation deliberately -- the dock kept lagging behind the panel
// (redirection-bitmap hosting, missed guards, slower alt-tab correction)
// precisely because it was a second copy of the same idea.
struct ComposedHost {
    HWND                                hwnd  = nullptr;
    ICoreWebView2CompositionController* comp  = nullptr;
    ICoreWebView2Controller*            ctrl  = nullptr;
    ICoreWebView2*                      core  = nullptr;
    IDCompositionDesktopDevice*         dcdev = nullptr;
    IDCompositionTarget*                dctgt = nullptr;
    IDCompositionVisual2*               dcvis = nullptr;
    HCURSOR                             cur   = nullptr;
    bool                                tracking = false;   // TrackMouseEvent armed
    void (*on_msg)(const std::string&)  = nullptr;          // page -> host strings
    DWORD                               browser_pid = 0;     // WebView2's browser process; see hud_on_foreground
    EventRegistrationToken              tok_msg{}, tok_cur{}, tok_key{};
};

// Defined in the in-game-panel section below; the snippet window drives a
// composed host of its own when docked.
static bool composed_create(ComposedHost& ch, const wchar_t* title, int w, int h,
                            void (*on_msg)(const std::string&),
                            const std::wstring* init_script);
static void composed_destroy(ComposedHost& ch);


// Whether the console's own panel ELEMENT is visible in the overlay. The
// window's existence is a separate question (a docked panel keeps it alive
// with the console panel off), which is exactly why this is a message and not
// window teardown.
static void push_hudvis() {
    hud_push(std::string("HUDVIS:") + (g_hud_enabled ? "1" : "0"));
}

// What the badge OPENS. In overlay mode the console lives in the game, so the
// badge is the way into the app; otherwise it is the panel's toggle, exactly
// as before. The page needs telling because the badge is its element -- the
// host deciding from a click position would be a second copy of the page's
// layout, the same reason the hit rects are page-reported.
static void push_overlay_pref() {
    hud_push(std::string("OVERLAYMODE:") + (g_overlay_pref ? "1" : "0"));
}


// ----------------------------- in-game panel --------------------------------
//
// A second WebView2 window glued to the attached client: the console's log and
// arm state, readable without alt-tabbing. It is a VIEW onto the same host
// state, not a second app -- every line it shows came through ui_log like any
// other, so the two can never disagree about whether writes are armed.
//
// Four things here are load-bearing, each of them learned the hard way while
// prototyping this as a snippet:
//
//   * WS_EX_NOACTIVATE goes on BEFORE the first ShowWindow. Applied afterwards
//     the window has already activated once, which pulls focus off Roblox --
//     the exact thing the flag exists to prevent.
//   * hWndInsertAfter names the window to sit BELOW. Passing the game HWND
//     puts the panel behind the game, which looks identical to it never having
//     been created. HWND_TOPMOST while the client is focused is what "over the
//     game" actually is.
//   * Z-order is not the mechanism for "under your other apps". Toggling bands
//     on focus blinks every time you tab back, because Windows raises the game
//     immediately and we only find out on the next tick -- a race we are always
//     second in. Topmost while the client is focused, and directly above the
//     game otherwise, has no such gap.
//   * Keystrokes never arrive: they go to the focus window and this one refuses
//     focus by design. Clicks and the wheel do arrive. So the panel is
//     click-and-read, and text entry stays in the app.

static std::unique_ptr<webview::webview> g_hud_wv;
static HWND  g_hud_hwnd     = nullptr;
static bool  g_hud_expanded = true;
static bool  g_hud_creating = false;  // the ctor pumps; keeps WM_TIMER out
static bool  g_hud_shown    = false;
// The window we handed ownership to, or null when the z-order fallback is
// running. Compared against the current client window each tick, so a
// client that restarts is re-owned rather than silently left orphaned.
static HWND  g_hud_owner    = nullptr;
static int   g_hud_cursor_added = 0;      // ShowCursor(TRUE) calls to undo
// Last snapshot of each type actually sent, so unchanged ones can be dropped.
// Cleared on the page's ready handshake -- see HUD_READY.
//
// GUARDED: hud_push mutates this BEFORE its ui_dispatch hop, on the caller's
// thread, and the callers are genuinely multi-threaded -- the snippets folder
// watcher pushes SNIPS from its own thread on every save (watch.cpp), the
// detached run thread pushes RUNSTATE/STATE at run begin/end (spawn_run and
// the set_run_phase callback), the attach watcher and the placename fetch
// push STATE, while the UI thread pushes HEALTH from the tick and clears the
// map on HUD_READY. Concurrent std::map mutation corrupts the heap
// (0xc0000374); the lock is leaf-level and never held across the dispatch.
static std::mutex g_hud_last_mux;
static std::map<std::string, std::string> g_hud_last;

// ---- composition-hosted panel (the preferred path) --------------------------
// The panel is hosted through CreateCoreWebView2CompositionController on our
// own WS_EX_NOREDIRECTIONBITMAP window rather than through the webview wrapper,
// because it is the only hosting mode with per-pixel alpha: the page's
// transparent pixels genuinely show the game, so the badge's rounded corners
// are anti-aliased CSS instead of a 1-bit SetWindowRgn mask. The window region
// is still applied, but demoted to HIT-TESTING: a plain rectangle around the
// badge, whose corners fall on transparent pixels. (Measured with a probe:
// DWM does clip the composited pixels to the region, 1-bit, so the region
// must fully CONTAIN the CSS shape -- never trace it.)
//
// The cost of this mode is that nothing routes input for us: the composition
// controller has no HWND of its own, so the wndproc below forwards every mouse
// message via SendMouseInput and mirrors the browser's cursor from
// CursorChanged. Keyboard is NOT forwarded -- the panel refuses focus by
// design, so there is nothing to forward; identical to the windowed path.
//
// The old wrapper+region path is kept below as the runtime fallback for a
// machine where any piece of this refuses (runtime too old for the
// controller, dcomp missing). Same page, same protocol; the page is told
// which mode it is in with ?c=1 and keeps its opaque styling otherwise.
static bool         g_hud_composed = false;   // the panel took the composed path
static ComposedHost g_hudc;

// IIDs spelled out for the same reason as ICoreWebView2Settings9 above:
// WebView2.h carries no __CRT_UUID_DECL for them under MinGW. Values are the
// MIDL_INTERFACE attributes in WebView2.h. kIID_Unknown avoids pulling in
// libuuid for the one constant.
static const GUID kIID_Unknown =
    { 0x00000000, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const GUID kIID_WebView2_2 =
    { 0x9e8f0cf8, 0xe670, 0x4b5e, { 0xb2, 0xbc, 0x73, 0xe0, 0x61, 0xe3, 0x18, 0x4c } };
static const GUID kIID_Environment3 =
    { 0x80a22ae3, 0xbe7c, 0x4ce2, { 0xaf, 0xe1, 0x5a, 0x50, 0x05, 0x6c, 0xde, 0xeb } };
static const GUID kIID_CompCtrlCompleted =
    { 0x02fab84b, 0x1428, 0x4fb7, { 0xad, 0x45, 0x1b, 0x2e, 0x64, 0x73, 0x61, 0x84 } };
static const GUID kIID_HudController =
    { 0x4d00c0d1, 0x9434, 0x4eb6, { 0x80, 0x78, 0x86, 0x97, 0xa5, 0x60, 0x33, 0x4f } };
static const GUID kIID_HudController2 =
    { 0xc979903e, 0xd4ca, 0x4228, { 0x92, 0xeb, 0x47, 0xee, 0x3f, 0xa9, 0x6e, 0xab } };
static const GUID kIID_CursorChanged =
    { 0x9da43ccc, 0x26e1, 0x4dad, { 0xb5, 0x6c, 0xd8, 0x96, 0x1c, 0x94, 0xc5, 0x71 } };
static const GUID kIID_WebMsgReceived =
    { 0x57213f19, 0x00e6, 0x49fa, { 0x8e, 0x07, 0x89, 0x8e, 0xa0, 0x1e, 0xcb, 0xd2 } };

// Minimal COM handlers: refcounted, one lambda each. WRL is MSVC furniture;
// this is the whole contract.
template <typename Fn>
struct HudCompDone : public ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler {
    ULONG r = 1; Fn fn;
    HudCompDone(Fn f) : fn(f) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return ++r; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG x = --r; if (!x) delete this; return x; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** o) override {
        if (riid == kIID_Unknown || riid == kIID_CompCtrlCompleted) { *o = this; AddRef(); return S_OK; }
        *o = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2CompositionController* c) override { return fn(hr, c); }
};
template <typename Fn>
struct HudCursorChanged : public ICoreWebView2CursorChangedEventHandler {
    ULONG r = 1; Fn fn;
    HudCursorChanged(Fn f) : fn(f) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return ++r; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG x = --r; if (!x) delete this; return x; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** o) override {
        if (riid == kIID_Unknown || riid == kIID_CursorChanged) { *o = this; AddRef(); return S_OK; }
        *o = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2CompositionController* s, IUnknown*) override { return fn(s); }
};
template <typename Fn>
struct HudMsgReceived : public ICoreWebView2WebMessageReceivedEventHandler {
    ULONG r = 1; Fn fn;
    HudMsgReceived(Fn f) : fn(f) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return ++r; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG x = --r; if (!x) delete this; return x; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** o) override {
        if (riid == kIID_Unknown || riid == kIID_WebMsgReceived) { *o = this; AddRef(); return S_OK; }
        *o = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* s, ICoreWebView2WebMessageReceivedEventArgs* a) override { return fn(s, a); }
};
static const GUID kIID_AddScriptDone =
    { 0xb99369f3, 0x9b11, 0x47b5, { 0xbc, 0x6f, 0x8e, 0x78, 0x95, 0xfc, 0xea, 0x17 } };
template <typename Fn>
struct HudScriptDone : public ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler {
    ULONG r = 1; Fn fn;
    HudScriptDone(Fn f) : fn(f) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return ++r; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG x = --r; if (!x) delete this; return x; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** o) override {
        if (riid == kIID_Unknown || riid == kIID_AddScriptDone) { *o = this; AddRef(); return S_OK; }
        *o = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, LPCWSTR id) override { return fn(hr, id); }
};
static const GUID kIID_AccelKey =
    { 0xb29c7e28, 0xfa79, 0x41a8, { 0x8e, 0x44, 0x65, 0x81, 0x1c, 0x76, 0xdc, 0xb2 } };
template <typename Fn>
struct HudAccelKey : public ICoreWebView2AcceleratorKeyPressedEventHandler {
    ULONG r = 1; Fn fn;
    HudAccelKey(Fn f) : fn(f) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return ++r; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG x = --r; if (!x) delete this; return x; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** o) override {
        if (riid == kIID_Unknown || riid == kIID_AccelKey) { *o = this; AddRef(); return S_OK; }
        *o = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2Controller* s, ICoreWebView2AcceleratorKeyPressedEventArgs* a) override { return fn(s, a); }
};

// ---- docked snippet panels --------------------------------------------------
// fx_window_dock turns a snippet's window into an in-game panel on the same
// terms the console's panel uses -- glued to the client, refusing focus, no
// caption, no taskbar entry -- by making it an ELEMENT of the console's one
// overlay window (see the snippet-window section above). Everything else
// about the window -- html, eval, messages, close -- is unchanged, which is
// why this is one call rather than a parallel window API.
static bool  g_hud_was_over  = false;
// Set when a click actually lands ON the panel, cleared once that click has
// bought a focus hand-back. Hovering never sets it.
static bool  g_hud_used      = false;
// True while the page's badge-target flyout is out (FLYOUT:1/0 from the
// page). One job: a click landing OUTSIDE the overlay must dismiss the
// flyout, and the page cannot see those clicks -- outside its reported rects
// they route to the game -- so the LL hook, which sees every button, sends
// CLICKAWAY. Everything else about the flyout (the target, the animation,
// the region rect) is the page's alone.
static bool  g_fly_open      = false;
// The app-mode close GESTURE: a mouse-down that landed on the client, when
// stamped, and whether the app window was in front at that moment. Set in
// hud_mouse_ll, read by the foreground handler -- a foreground event on its
// own cannot tell a user's click from the client re-asserting itself, and it
// does re-assert. See both sites.
static DWORD g_game_click_at   = 0;
static bool  g_game_click_from = false;
// A mouse-down on the APP WINDOW ITSELF while a THIRD app was foreground:
// the user is returning to the console, and the console is a HUD over the
// client, so the client must come up under it (user, 2026-08-22: "the
// console is behind it instead of the roblox focusing too"). Stamped in the
// hook -- which runs BEFORE the click lands, when we own no foreground
// permission and a raise of the client is refused -- and consumed by the
// foreground event the click causes, when we ARE the foreground process and
// a z-only raise is honoured. The sibling of the badge summon, for the
// app's own body. Deliberately separate from g_game_click_at: a click on
// the app is not a click on the client and must not arm the close gesture.
static DWORD g_app_arrive_at   = 0;
// The app's owner is held OFF for a moment after the return-from-a-third-app
// click (hook path). The click's own activation of the client follows the
// hook, and USER32's owner carry then re-places the OWNED app -- measured
// 2026-09-04 (0.2ms sampler, ZWATCH with owner=client and a pump-only
// stack): the app dropped under the topmost overlay for 11.5ms until "APP
// stays" put it back. Unowned, there is nothing to carry: the app is TOPMOST
// by our own hand above the client, the overlay directly under it. The tick
// re-owns it once the window has passed; the band rule is unaffected.
static DWORD g_app_owner_holdoff = 0;
// Defined with the focus plumbing further down; the foreground handler above
// it needs to hand the app the keyboard when the user comes back to the game
// from a third app.
static bool force_foreground(HWND target);
// FOCUS MODE: the overlay holds REAL keyboard focus so the user can type into
// it (real focus is what gives real IME and clipboard; synthesised keys give
// neither). Entered when the PAGE says a click asked for it (HUD_FOCUS:1 --
// intent stays with the page, like the hit rects); left by Esc, by the page
// (HUD_FOCUS:0), or by anything else taking foreground. While set, the overlay
// behaves as an ordinary focused window: clicks on it are not borrows to be
// handed back, and the tick keeps it in the topmost band.
//
// WHO ENTERS IT: a DOCKED SNIPPET PANEL'S TEXT FIELD, and nothing else
// (2026-09-04). The frame shim (kOverlayFrameShim) sends HUD_FOCUS:1 when a
// click lands DOM focus on an <input>/<textarea>/contenteditable in the
// docked page and HUD_FOCUS:0 when DOM focus leaves one; the game gets the
// keyboard back on that, on Esc, or on any click that lands on it. It was
// built for APP MODE (overlay-plan.md step 3), which is a real window now and
// needs none of it. THE CONSOLE PANEL MUST NEVER USE IT: the panel's whole
// job is to read as a Roblox-owned UI, and it was wired to take focus on open
// for exactly one build -- that put an accent band around it and changed how
// Windows rendered its text, because a focused window is drawn differently.
// Both were instantly visible as "this is not part of the game". The docked
// panel is different in kind: it is the user's own UI, the user clicked into
// its field, and the caret appearing is what they asked for. The accent band
// is a DWM FRAME property and this host has no frame: measured 2026-09-04
// (build\border_probe, deleted) -- a bare WS_POPUP in this host's exact
// styles draws 0 border pixels inactive, 0 FOREGROUND with WS_EX_NOACTIVATE
// cleared (the state overlay_focus_enter puts it in), and 0 with
// DWMWA_BORDER_COLOR set to red and asked for; a real captioned frame beside
// it draws the 1px border on all four sides unasked, in both states (293/300
// rows left and right, 386/386 columns bottom). The accent colour recolours
// a border that exists; it cannot create one. The wrapper-hosted FALLBACK
// has a frame, which is why it carries DWMWA_BORDER_COLOR=NONE and why the
// band was seen at all.
static bool  g_ov_focus = false;
static bool  overlay_focus_enter();
static void  overlay_focus_exit(bool hand_to_game);
static bool  g_hud_was_fg   = false;
static int   g_hud_lastx = -1, g_hud_lasty = -1, g_hud_lastw = -1, g_hud_lasth = -1;

// THE TOPMOST-BAND FIGHT, instrumented (2026-08-21). The band was observed
// lost with the trace showing no request of ours -- and the zlog can only see
// OUR calls. Harness-measured eliminations, so they are not re-suspected:
//   * generic Windows fullscreen is NOT the mechanism. A fake fullscreen
//     foreground window (system agreed: notification state BUSY) neither
//     denied nor stripped another process's HWND_TOPMOST across 18s of
//     re-asserts (build	opmost_probe + buildakefs, 2026-08-21).
//   * a quiet SetWindowLong strip is impossible: user32 ignores WS_EX_TOPMOST
//     changes through it, measured in both directions.
//   * an owner going NOTOPMOST DOES strip its owned windows' topmost without
//     them asking -- but the next SetWindowPos heal succeeds, and the overlay
//     owner (the client) never rides the topmost band, so that vector
//     stays dead with the overlay owned (hud_owner_mode).
// THE MECHANISM, pinned live 2026-08-22 (probe harnesses in build\, deleted;
// series in the session report). It is a Windows-side ENTRY GATE, not a
// strip, not the client's shape, and not this window:
//   * SetWindowPos(HWND_TOPMOST) returns ret=1 err=0 with the bit simply not
//     applied, and the same silent refusal hits the RELATIVE above-game
//     insert (the glue), so the overlay can end up under the game entirely.
//   * reproduced with the client WINDOWED (fso=0, restored 1797x960) -- the
//     earlier "fullscreen optimizations is the necessary condition" was
//     falsified by its own diag line. A fresh mimic window in a fresh
//     process is refused in the same state (so not our window), and enters
//     cleanly in the open state (so the probe can fail).
//   * the gate OPENS around input-driven activation of the game -- a real
//     click landing on it (the LL hook and the foreground event fire exactly
//     then, which is why those promotes stick) -- and measurably stays open
//     for tens of seconds after a synthetic activation click. It does NOT
//     open for a programmatic SetForegroundWindow: the overlay_handback /
//     focus-exit hand-off leaves the gate closed, which is why every use of
//     the overlay from an unfocused game used to start a heal storm.
//   * once IN the band the bit persists (held 20+ min under refusal of new
//     entries); the losses are our own demote on foreground flaps.
// Consequences in code: raise_overlays clears the band-lost latch on any
// successful TOPMOST (the hook paths are the ones that run while the gate is
// open), and the desktop-window valve waits for the backoff to fail too.
// The instruments stay: hud_swp records ret/err, and hud_host_wndproc counts
// z-moves that arrive while we are NOT inside hud_swp -- a cross-process
// SetWindowPos on this window lands as WM_WINDOWPOSCHANGING on this thread,
// so an external actor cannot move us without incrementing g_hud_ext_zmoves.
static bool  g_hud_in_swp      = false; // inside one of OUR SetWindowPos calls
// NB since overlay ownership (2026-08-22) a NON-ZERO ext count is no longer
// automatically an attacker: while the overlay is owned, USER32 itself
// repositions it during the owner group's activation (the enforcement ride
// that removes the tab-in blink), and each such move lands here as one
// external z-move -- observed shape: after= the app window or a group
// neighbour, flags=0x1813, once per activation, followed by at most one
// successful heal. A STORM of them, or externals while the game is NOT
// being activated, is still the old tripwire.
static int   g_hud_ext_zmoves  = 0;     // overlay z-moves we did not ask for
static BOOL  g_hud_swp_ret     = TRUE;  // last hud_swp result, for the diag line
static DWORD g_hud_swp_err     = 0;
static bool  g_hud_band_lost   = false; // TOPMOST will not stick; ride above the game
static int   g_hud_band_fails  = 0;     // consecutive failed heals
static DWORD g_hud_band_retry  = 0;     // backoff clock for the 1/s retry
static bool  g_hud_band_told   = false; // the WARN + diag fired for this incident
static bool  g_hud_suppressed  = false; // both placements refused: desktop window back

static const int kHudPanel  = 360;
static const int kHudStrip  = 44;
static const int kHudHeight = 460;
static const int kHudBadgeH = 66;   // collapsed: a badge, not a full-height rail

// Roblox owns several top-level windows over its life, and the one that is
// focused is not necessarily the first enumerated -- the same trap fx_focused
// avoids by comparing pids. Take the largest visible unowned window.
struct HudFind { DWORD pid; HWND best; long area; };

static BOOL CALLBACK hud_pick_window(HWND h, LPARAM lp) {
    HudFind* f = (HudFind*)lp;
    DWORD p = 0;
    GetWindowThreadProcessId(h, &p);
    if (p != f->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT r;
    if (!GetWindowRect(h, &r)) return TRUE;
    long a = (r.right - r.left) * (r.bottom - r.top);
    if (a > f->area) { f->area = a; f->best = h; }
    return TRUE;
}

// Cached, because EnumWindows walks every top-level window in the session and
// the answer changes only when the client's window does. Shared by the panel
// and by docked snippet windows: the docked one had no cache at all, so a
// snippet with a docked panel enumerated the whole desktop sixty times a
// second.
static HWND hud_client_cached(uint32_t pid) {
    static HWND     s_win = nullptr;
    static DWORD    s_at  = 0;
    static uint32_t s_pid = 0;
    if (!pid) return nullptr;
    DWORD now = GetTickCount();
    if (s_pid != pid || !s_win || !IsWindow(s_win) || now - s_at > 1000) {
        HudFind f{ (DWORD)pid, nullptr, 0 };
        EnumWindows(hud_pick_window, (LPARAM)&f);
        s_win = f.best;
        s_pid = pid;
        s_at  = now;
    }
    return s_win;
}

// "fgpid == pid" is the band rule's PROXY for "the client is what the user is
// looking at", and the proxy has a real counterexample: a freshly launched,
// never-focused client can BECOME the pid GetForegroundWindow reports without
// any activation -- no EVENT_SYSTEM_FOREGROUND fires and nothing raises the
// window, so its one visible window stays buried near the bottom of the
// z-order under other apps. (Observed in three traces, 2026-08-28: fg-rank ==
// game-rank == 42 -- same window, so NOT a wrong pick -- held for seconds,
// while a live enumeration of the staged repro state showed that window as
// the client's ONLY visible unowned window, covered by a third app at its
// centre. Who hands foreground over without an activation is deliberately
// left open; the timing pointed at the client's own load-complete, and no
// FF/force_foreground line of ours was in any trace.) Promoting on the bare
// proxy paints the panel into the topmost band over whatever really covers
// the client's rect -- the user's screenshot had it over Chrome.
//
// So the proxy is corroborated by an effect outside this process: whose
// window is visually on top at the centre of the client's rect.
// WindowFromPoint respects regions, layered hit-test transparency and
// ordinary clipping, so the click-through overlay does not shadow the answer.
// Our own pid counts as "client visible": everything of ours that sits over
// the client (overlay, app mode, snippet windows) does so by design, and in
// app mode the app window IS what covers the centre.
//
// FAIL OPEN (house rule for filters): if the probe cannot answer -- no game
// window, no rect, minimized -32000 coords, dead point -- believe the proxy.
// The filter exists to catch a corroborated "covered", never to drop a
// foreground client it merely cannot check.
//
// Known accepted cost: a third-party ALWAYS-ON-TOP window parked exactly over
// the client's centre (a PiP video, say) reads as covered while the user
// plays, demoting the panel from the topmost band to the slot directly above
// the game -- where it is still visible over the focused game, just not over
// the PiP. That is arguably the right z-order anyway.
static bool hud_client_covered(HWND game, uint32_t pid) {
    RECT r;
    if (!game || !GetWindowRect(game, &r)) return false;
    POINT c{ (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
    HWND at = WindowFromPoint(c);
    if (!at) return false;
    HWND root = GetAncestor(at, GA_ROOT);
    if (!root) return false;
    DWORD p = 0;
    GetWindowThreadProcessId(root, &p);
    if (!p) return false;
    if (p == pid) return false;               // the client itself: not covered

    // BY WINDOW, NOT BY PID. Exempting our whole process was too coarse and
    // put the original bug straight back: the DESKTOP console window is an
    // ordinary window that covers the client exactly like Discord does, and
    // when it sits over the client's centre this read "clear" while the client
    // was buried at z-rank 43. MEASURED (%TEMP% eclipse_z.log, t=304478):
    // raw=1 fgsame=1 gamez=43 covered=0 wfp=<ours> -> fg=1 -> ovz=9, topmost
    // over the wrong window for 2.3 seconds.
    //
    // Only the two surfaces that ARE the in-game UI are exempt: the overlay
    // itself, and the app window while app mode has it over the client. Both
    // are things the user is looking at THROUGH, not obstacles proving the
    // client is buried.
    if (root == g_hud_hwnd) return false;
    if (g_app_mode && root == ui_main_hwnd()) return false;
    return true;
}

// THE WINDOW IS NEVER RESIZED by anything that animates. Collapsing clips it
// with a region instead.
//
// This was tried, abandoned, and come back to, so the reasoning is worth
// keeping. Resizing is what produces every artifact seen while building this:
// WebView2's rendering surface is the size of the window, so growing the window
// displays the OLD surface stretched -- the badge stranded in a corner of a
// black rectangle -- until the renderer delivers a frame at the new size. That
// is not a layout problem and no page-side change fixes it; a fixed-size page
// anchored to the window's right edge was tried and looked identical.
//
// A region changes what is visible without touching the surface, so there is
// nothing to redraw and nothing to be late. Its one cost is the window's drop
// shadow, which is drawn from the window RECT and which a region does not clip
// -- so the shadow is removed outright below.
//
// FALLBACK ONLY now. The composed overlay's region is the union of the
// interactive rects the PAGE reports (see hud_set_rects); this fixed math
// serves the wrapper-hosted panel, where the region still IS the shape.
static void hud_apply_region(bool expanded) {
    if (!g_hud_hwnd || g_hud_composed) return;

    if (expanded) {
        SetWindowRgn(g_hud_hwnd, nullptr, TRUE);
        return;
    }
    // The badge: the right-hand strip, vertically centred. Matches #tab's
    // 44px column and the 66px band the page centres its contents in.
    int top = (kHudHeight - kHudBadgeH) / 2;
    HRGN rgn = CreateRoundRectRgn(kHudPanel - kHudStrip, top,
                                  kHudPanel + 1, top + kHudBadgeH + 1, 14, 14);
    SetWindowRgn(g_hud_hwnd, rgn, TRUE);        // the window owns it now
}

// ---- the hit-test region, page-reported --------------------------------------
//
// The composed overlay covers the whole client, so with no region it would
// swallow every click over the game. Its region is therefore the UNION OF THE
// VISIBLE PIECES -- console panel (or badge), docked snippet panel -- and the
// PAGE reports those rects ("RECTS:x,y,w,h;...", window px, which are CSS px
// because the rasterization scale is pinned to 1.0) whenever its layout
// changes.
//
// The page reports rather than the host deriving, deliberately. The host
// COULD compute today's two rects -- it places the dock and knows the badge
// math -- but that is a second copy of the page's layout, and duplicated
// geometry drifting apart is this project's recurring bug class. It also
// cannot survive the plan's step 3, where the page itself animates the badge
// into the full app: only the page knows the two ordering-critical instants
// (grow the region BEFORE expanding into it, shrink it only after
// transitionend -- DWM clips composited pixels to the region, 1-bit,
// measured). Polling was the third option and loses on both latency and
// cost. The page computes rects from STATIC geometry, never from an element
// mid-animation, so a report is valid for the whole animation it brackets.
//
// Trust: the strings come from OUR page (snippet iframes cannot post as it),
// but the parse still clamps -- count-capped, size-positive -- because a
// malformed report must degrade to "keep the last region", never to a
// client-sized click sponge.
static std::vector<RECT> g_ov_rects;    // SOLID rects only; also the cursor test
// Any SOFT rect in the current report. Soft means "drawn, but not a surface of
// ours", and the REGION cannot express that on its own: a rect in the region is
// a place the window exists, so it takes the click. What expresses it is the
// pointer-following click-through below, which until now only ran during an app
// travel. It is switched on whenever a soft rect is present instead -- and that
// is not a widening of behaviour, because with NO soft rect the region is
// exactly the solid set and region hit-testing already gives the same answer
// the pointer rule would.
static bool g_ov_soft = false;

// THE REGION KNOWS NOTHING ABOUT THE APP WINDOW. While the app is open the
// overlay -- badge, docked panel, flyout, all of it -- sits DIRECTLY BELOW
// the app in the z-order (the under-app glue in hud_tick / raise_overlays),
// so the compositor occludes it where the app is and hit-testing lands on
// the app there, both with the app's exact composed shape and every frame.
//
// SUPERSEDED THE DAY IT SHIPPED (2026-09-04): THE APP HOLE. For half a day
// the overlay rode ABOVE the app (the badge as its close control) and a
// docked panel was put "behind" the app by subtracting the app's rect from
// the region -- DWM clips composited pixels to the region, so pixels and
// clicks both fell to the app. It worked, and it was a 1-bit RECT standing
// in for the app's real shape, re-applied at the tick rate: the app's
// DWM-rounded corners showed the GAME through the hole's square corners,
// and a drag moved the window per frame while the hole followed at ~30Hz
// (a region rebuild per pointer move is a stutter machine by measurement,
// docs/overlay-plan.md "Drag cost") -- the "dock and app not seamless"
// report. Z-order has neither cost. Its price is the badge going behind
// the app too, which the user chose over the seam; the titlebar's close
// and Insert are the ways out. Do not bring the hole back for a
// badge-above variant without re-reading this.
static std::vector<RECT> g_ov_region;   // every reported rect, soft included
static bool zlog_on();
static void zlog(const char* fmt, ...);

// Rebuild the window region from the stored report.
static void hud_region_apply() {
    if (!g_hud_composed || !g_hud_hwnd) return;
    // An empty list is a real state -- panel off, nothing docked -- and maps
    // to an empty region: the window swallows nothing. NULL (no region) would
    // mean the opposite.
    HRGN acc = CreateRectRgn(0, 0, 0, 0);
    for (const RECT& r : g_ov_region) {
        HRGN one = CreateRectRgn(r.left, r.top, r.right, r.bottom);
        CombineRgn(acc, acc, one, RGN_OR);
        DeleteObject(one);
    }
    LARGE_INTEGER f{}, t0{}, t1{};
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    SetWindowRgn(g_hud_hwnd, acc, TRUE);        // the window owns acc now
    QueryPerformanceCounter(&t1);
    if (zlog_on())
        zlog("REGION apply: %d rects %.2fms", (int)g_ov_region.size(),
             (t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart);
}

// A rect prefixed "~" is SOFT: it joins the region (DWM clips composited
// pixels to the region, so the travel box must be inside it to be drawn) but
// is NOT a surface of ours -- it does not own input and does not hold the
// cursor. The page marks the travel's bounding box soft; badge and dock stay
// solid. Without the split, "over one of our rects" was true across the whole
// travel corridor, which is exactly wrong for both the cursor and the clicks.
static void hud_set_rects(const std::string& body) {
    if (!g_hud_composed || !g_hud_hwnd) return;
    std::vector<RECT> rects, region;
    size_t p = 0;
    while (p < body.size() && region.size() < 64) {
        bool soft = false;
        if (body[p] == '~') { soft = true; p++; }
        int x = 0, y = 0, w = 0, h = 0;
        if (sscanf(body.c_str() + p, "%d,%d,%d,%d",
                   &x, &y, &w, &h) == 4 && w > 0 && h > 0) {
            region.push_back(RECT{ x, y, x + w, y + h });
            if (!soft) rects.push_back(RECT{ x, y, x + w, y + h });
        }
        size_t semi = body.find(';', p);
        if (semi == std::string::npos) break;
        p = semi + 1;
    }
    g_ov_rects = rects;
    bool had_soft = g_ov_soft;
    g_ov_soft = region.size() != rects.size();
    // Gone soft-free outside a travel: hand the clicks back explicitly. The
    // pointer rule stops being applied the moment the flag drops, so nothing
    // else would ever clear a WS_EX_TRANSPARENT it left set.
    if (had_soft && !g_ov_soft && !g_travel_ct) overlay_clickthrough(false);

    g_ov_region = region;
    hud_region_apply();
}

// CLICK-THROUGH FOLLOWS THE POINTER during a travel. The cursor's visibility
// is a PER-INPUT-QUEUE ShowCursor count, and the queue that governs is the
// one owning the window under the pointer. While the overlay is
// WS_EX_LAYERED|WS_EX_TRANSPARENT the pointer is logically over the GAME, so
// the game's count rules (measured: CURSOR_SHOWING clear and hCursor NULL for
// the whole close travel, ~260ms) and nothing this process adds can win it
// back -- clicks-through and cursor-visible are mutually exclusive FOR A
// POINT. But both only exist AT the pointer, so the conflict dissolves by
// deciding ownership per pointer position: over a solid piece (badge, dock)
// the overlay is opaque -- our queue governs, the top-up holds the cursor,
// and a click there is a click on our control, which is correct; anywhere
// else it is transparent -- the click reaches the game and the cursor is the
// game's business, same as hovering the game normally. Re-evaluated every
// hud_tick and on hook-time mouse moves, so a click cannot land in the gap
// between pointer move and tick.
static void overlay_clickthrough(bool on);
// Is the pointer rule in force? A travel (a soft bbox spanning the corridor) or
// a passthrough docked panel (fx_window_passthrough) -- both are cases where
// something of ours is DRAWN over a place the game must still own.
static bool ct_following() { return g_travel_ct || g_ov_soft; }
static void ct_follow_apply() {
    if (!ct_following() || !g_hud_hwnd) return;
    POINT c{};
    bool solid = false;
    if (GetCursorPos(&c) && g_hud_lastx != -1) {
        for (const RECT& pr : g_ov_rects) {
            RECT sr{ g_hud_lastx + pr.left, g_hud_lasty + pr.top,
                     g_hud_lastx + pr.right, g_hud_lasty + pr.bottom };
            if (PtInRect(&sr, c)) { solid = true; break; }
        }
    }
    overlay_clickthrough(!solid);
}

// OPEN AND CLOSE, and why the WINDOW never animates.
//
// Approaches that were tried and are recorded so they are not re-invented:
//
//   * interpolating the window RECT per frame. Every resize relayouts
//     WebView2 a frame or two late, which showed as the panel's right and
//     bottom edges trailing the window with a gap behind them, and as the
//     badge drifting vertically as the height changed. Raising the timer
//     resolution made it smoother without making it correct.
//   * a REGION on the wrapper-hosted window, clipping the fixed panel to the
//     badge. Works, and is the shipping FALLBACK -- but a region is a 1-bit
//     mask, so the badge's corners are jagged, and it clips content but NOT
//     the window's shadow (DWMWA_NCRENDERING_POLICY did not remove it; only
//     clearing CS_DROPSHADOW on the class did).
//
// What ships: a fixed-size window in BOTH modes, and all motion is the
// page's, which the compositor runs on its own clock for free. In the
// composed mode the page also owns the SHAPE -- transparent pixels show the
// game -- so open/close is a CSS fold of the panel into the badge and the
// region is just the collapsed hit rectangle. In the fallback the region is
// the shape, so the motion is only the panel's cross-fade inside it.

// Foreground changes arrive as an EVENT, not on the next tick. Polling for them
// is what made the panel blink: Windows raises the game the instant it is
// activated, and a tick means up to a frame of the game sitting on top of us
// before we re-assert.
//
// Registered on the UI thread on purpose: WINEVENT_OUTOFCONTEXT delivers
// through that thread's message queue, which the webview loop already pumps.
// NOT SKIPOWNPROCESS -- focusing the console itself is a transition the panel
// has to react to as well.
//
// HOW LATE IS THIS PATH? Measured with a QPC poll of the z-order relation
// while scripted alt-tabs ran against the live client: the panel is back
// above the game 0.5-1.4ms after Windows raises it. At 60Hz composition
// that mis-ordered window is caught by a composed frame only occasionally
// (roughly gap/16ms of alt-tabs), which is the rare, barely-visible blink the scheme is known for.
// Reacting faster cannot remove it -- the raise happens first by design of
// the scheme -- and a dedicated hook thread was rejected because cross-thread
// SetWindowPos synchronises with this thread anyway. Do not spend more time
// below ~1ms.
//
// AND DO NOT TRY: that floor is why the blink is removed by OWNERSHIP
// instead (hud_tick, "OWNERSHIP is what actually keeps the app above the
// game"). The app window is owned by the client, so USER32 orders it above
// the client as part of the activation and this path has nothing left to
// race for it. The OVERLAY is owned too since 2026-08-22 (second attempt --
// see hud_swp's bracket for why the first broke focusing other apps), so on
// tab-in this path is belt-and-braces; it still owns the click pre-emption
// and the band the overlay rides over OTHER apps.
// The user-visible proof: the app stopped flashing on alt-tab returns the
// moment ownership landed, after every attempt to win the race had failed.
static HWINEVENTHOOK g_hud_evhook = nullptr;
static DWORD         g_hud_preempt_at = 0;   // see the mouse hook below

// OVERLAY OWNERSHIP MODE (2026-08-22). The overlay takes the client as its
// OWNER while it rides the normal band (a third app foreground) -- that is
// what removes its tab-in blink: USER32 carries an owned window above its
// owner through the owner group's activation. Precisely measured, the carry
// is an enforcement pass, not an atomic move -- at microsecond sampling the
// inversion vs the game is bounded at ~2ms worst app-closed and 0.2ms worst
// app-open, against 8.8ms unowned with the app open (more than a composed
// frame, the blink the user saw); an unowned control window is passed
// PERMANENTLY and only our reactive raise saves it. The regression that
// forced the first attempt out is avoided by the BRACKET in hud_swp below
// and by dropping the owner while the game is foreground (cursor bleed --
// see the ownership block in hud_tick).
//
// THE COVERAGE BOUNDARY -- what ownership settles and what it does not,
// written down because it is narrower than it first looks:
//   * the APP is owned for its whole visible lifetime (app_owns_window):
//     app-vs-client is settled structurally, in any band, whatever the
//     entry gate does. That is what let app_ensure_above be deleted.
//   * the OVERLAY is owned only while a THIRD APP is foreground. The moment
//     the client is foreground the owner is dropped, so in normal play the
//     band machinery -- raise_overlays and the tick's promote/heal -- is
//     still the ONLY thing holding the overlay up. None of it is redundant.
//   * ownership orders each window against THE CLIENT only. Overlay-vs-app
//     (the overlay rides directly under the open app, overlay_glue_under_app),
//     anything-vs-third-apps (the band rules) and the below-third-apps
//     demote slot stay ours.
//   * mode 0 has no owner at all, so every band path also remains the whole
//     story for the kill-switch configuration.
// Env knob, shipped deliberately: the
// first ownership attempt broke the user's desktop, so 0 is the instant
// no-rebuild revert, and 1 is the reproducible negative control that keeps
// the coverage instrument honest.
//   ECLIPSE_HUD_OWNER=0  no owner (the pre-2026-08-22 behaviour)
//   ECLIPSE_HUD_OWNER=1  owner WITHOUT the bracket -- the known-bad
//                        configuration; exists so the coverage regression
//                        can be reproduced on demand when re-verifying
//   unset / anything else: owner + bracket (the shipped behaviour)
static int hud_owner_mode() {
    static int m = -1;
    if (m < 0) {
        char* v = getenv("ECLIPSE_HUD_OWNER");
        m = !v ? 2 : (*v == '0' ? 0 : (*v == '1' ? 1 : 2));
    }
    return m;
}
static int g_hud_owner_pauses = 0;   // bracket count, for churn measurement
static void zlog(const char* fmt, ...);

// EVERY place that needs our windows above the game calls this.
//
// There were three copies of "promote the panel, and maybe the dock": the
// foreground hook, the click hook, and the focus hand-back. The hand-back only
// ever raised the PANEL -- which is exactly why clicking the panel made the
// DOCK blink. Three copies meant a fix had to be written three times and one
// was always missed, which is the same duplication that let the docked path
// lag the panel all week.
//
// The bridge stamp does NOT live here any more (2026-08-28). It did -- an
// unconditional g_hud_preempt_at at the end of raise_overlays -- which made
// every caller grant 400ms of "pretend the client is focused", including
// callers with no client activation anywhere near them (the app-band promote
// would even self-extend the fg it was conditioned on). The stamp is the
// CALLER's statement that a client activation is imminent or in flight, so
// each site that means it says it explicitly: the click hook (pre-raise, the
// original bridge), the hand-back and the app close (both about to
// force_foreground the client), the app-mode overlay click (a summon does the
// hand-back a tick later), and the foreground-event handler -- which now
// corroborates the event first, because the client's load-complete grab can
// fire EVENT_SYSTEM_FOREGROUND without any raise and without
// GetForegroundWindow ever agreeing (the auto-attach flash; see the handler).
// Every z-order change we make to the overlay goes through here: the guard
// lets hud_host_wndproc tell OUR moves from an external process's, and the
// saved ret/err are what the band-lost diag reports. Captured always, read
// on failure -- the whole point is that nobody captured it the first time
// this fight was seen.
static BOOL hud_swp(HWND after, int x, int y, int w, int h, UINT flags) {
    // THE OWNERSHIP BRACKET. An owned window LEAVING the topmost band hoists
    // its whole owner group -- game included -- to the top of the normal
    // band, covering whatever app the user just clicked (mimic-measured
    // 2026-08-22: NOTOPMOST and a relative insert into the normal band both
    // hoist; a TOPMOST entry, in-band moves and setting the owner itself
    // never do -- the owner-set pulls the owned window DOWN to the group).
    // So every non-TOPMOST move of the overlay happens UNOWNED: drop the
    // owner, move, take it back. The exposure is microseconds inside one
    // call on this thread; the client activating inside it costs one old-
    // style blink at worst. Mode 1 skips the bracket deliberately -- it is
    // the negative control that reproduces the coverage regression.
    //
    // TOPMOST moves are bracketed too (2026-08-27). The original exemption
    // leaned on "TOPMOST entries never hoist" -- true of the OWNER, and
    // incomplete: a TOPMOST *entry* of the owned overlay makes USER32
    // restack the rest of the owner group -- the app window AND the game --
    // to the top of the normal band inside the same SetWindowPos call,
    // burying whatever unowned window sat above them. Caught live by the
    // ZWATCH trace (the app re-placed at the top of the normal band 13ms
    // after a floating snippet window's activation raise, stack inside this
    // function -- the user's "roblox + the app go above" and the radar that
    // could not be focused), then probe-reproduced: owned entry = app+game
    // above the just-raised radar; bracketed entry = radar stays on top.
    // The intermittency was this too: an overlay already TOPMOST needs no
    // heal, so no restack -- "sometimes works". The owner-carry that mode 2
    // exists for happens during the GAME'S activation, outside our calls,
    // so the bracket costs it nothing.
    bool pause = g_hud_owner && hud_owner_mode() == 2;
    g_hud_in_swp = true;   // owner writes can reorder us down; that is ours too
    if (pause) {
        SetWindowLongPtrW(g_hud_hwnd, GWLP_HWNDPARENT, 0);
        ++g_hud_owner_pauses;
    }
    SetLastError(0);
    BOOL ok = SetWindowPos(g_hud_hwnd, after, x, y, w, h, flags);
    g_hud_swp_err = GetLastError();
    g_hud_swp_ret = ok;
    if (pause)
        SetWindowLongPtrW(g_hud_hwnd, GWLP_HWNDPARENT, (LONG_PTR)g_hud_owner);
    g_hud_in_swp  = false;
    return ok;
}

// UNDER THE APP (2026-09-04). While the app window is open and steady -- no
// choreography leg in flight, window visible -- the overlay's slot is
// DIRECTLY BELOW THE APP, whichever band the app is in: the app is the thing
// in front, and the overlay is treated exactly as it treats a third app,
// with the below-it slot the demote glue has always used. Inserting after a
// topmost app lands in the topmost band under it, after a normal-band app
// in the normal band under it; either way above the client, which the app
// (owned by the client) never rests below. The two chooser predicates and
// the glue live here so raise_overlays and hud_tick share ONE rule -- the
// old design had raise_overlays put the overlay TOPMOST and the tick agree,
// and every caller that meant "ours above the game" would otherwise now
// mean "dock over the console" for a tick.
//
// Why not ownership (app owned by the overlay, or chained): both directions
// were measured and rejected (docs/overlay-plan.md, the 21-external-moves
// table and the cursor bleed); this is a plain z-order slot.
//
// Mid-leg the overlay is TOPMOST as ever: the travel box has to compose OVER
// the window it is covering, and the leg flags are what exempt it.
static void raise_overlays();   // defined just below; app_zorder_changed needs it
// Break the cross-process owner/owned pair, now, before a click is routed.
// WHY IT EXISTS: while the user is in a third app the overlay is OWNED by the
// client so its activation carries the badge up with no gap. But an owner whose
// last active popup is WS_EX_NOACTIVATE hands the activation to that popup,
// which refuses it -- so clicking the game activated NOTHING and the third app
// kept focus (2026-09-08). Dropping the owner for the click alone keeps the
// carry everywhere else; the tick re-owns within 16ms.
static void overlay_owner_drop();
static void zsnap(const char* tag);   // z-rank snapshot, defined with z_rank
static bool app_steady() {
    return g_app_mode && !g_app_opening && !g_app_closing &&
           ui_main_hwnd() && IsWindowVisible(ui_main_hwnd());
}
// The looser gate the slot actually keys on (2026-09-04, fault 4): the app
// window is ON GLASS, legs included. The overlay sits under the app for as
// long as the app is visible -- the travel box does not need to be ABOVE the
// window it covers, only composed BEFORE that window hides (close_finish's
// DwmFlush pair) or shown before it drops (APPSHOWN); beneath it, the hide
// reveals it and the drop changes nothing on screen. What the legs DO need
// is the overlay above the CLIENT's coming activation raise, and that is the
// app's TOPMOST plus the slot under it, not the overlay above the app. The
// old "TOPMOST while a leg is in flight" put the whole overlay -- dock
// included -- over the still-visible app for the ~50ms between APPCLOSE and
// the hide: the user's "dock flashes in front of the app" on every close.
static bool app_in_front() {
    return g_app_mode && ui_main_hwnd() && IsWindowVisible(ui_main_hwnd());
}
// Already DIRECTLY under the app? Directly, not merely "somewhere between
// the app and the client": the looser test was the first cut, and it let
// the overlay rest in the normal band under a just-promoted topmost app --
// where the next foreground change's owner-group hoist (the client rising
// to the top of the normal band) crossed it. Measured on the return-from-a-
// third-app click, 0.2ms sampler: 5-14ms of overlay-under-game per return
// with the loose test, none once the slot is the adjacent one. Every steady
// state is adjacent anyway (USER32's own owner carry places it there too).
// INVISIBLE windows between the two do not count: our own `MSCTFIME UI`
// (an IME helper, never shown) sits directly under the app after every
// promote, and the strict next-window test read that as "not under the
// app" on every open -- an insert that changed nothing, `fails` ticking
// toward the backoff, and a false `did not take` line per tick. Nothing
// invisible is composed, so it cannot be between them on screen; the band
// bits are compared as well so the loose test can never accept an overlay
// resting in the normal band under a topmost app (the dead end above).
static bool overlay_under_app(HWND /*game*/) {
    HWND app = ui_main_hwnd();
    if (!app || !g_hud_hwnd) return false;
    HWND n = GetWindow(app, GW_HWNDNEXT);
    while (n && n != g_hud_hwnd && !IsWindowVisible(n)) n = GetWindow(n, GW_HWNDNEXT);
    if (n != g_hud_hwnd) return false;
    return ((GetWindowLongW(app, GWL_EXSTYLE) ^ GetWindowLongW(g_hud_hwnd, GWL_EXSTYLE))
            & WS_EX_TOPMOST) == 0;
}
// Put it there. `place` carries the tick's rect glue in the same call so a
// client move and a slot fix cost one SetWindowPos. Verified by position
// after the call, not by a style bit (the entry gate returns success without
// applying, docs/overlay-plan.md): a refused insert leaves the overlay where
// it was -- still above the client -- and is retried, backing off to once a
// second after 8 straight refusals so a closed gate is not a 30Hz storm.
static void overlay_glue_under_app(HWND game, int x, int y, int w, int h, bool place) {
    static int   fails = 0;
    static DWORD retry = 0;
    bool under = overlay_under_app(game);
    if (under) {
        fails = 0;
        if (place)
            hud_swp(nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        return;
    }
    if (fails >= 8) {
        DWORD now = GetTickCount();
        if (now - retry < 1000) {
            if (place)
                hud_swp(nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            return;
        }
        retry = now;
    }
    zlog("overlay -> under app (owner=%p)", (void*)g_hud_owner);
    hud_swp(ui_main_hwnd(), x, y, w, h,
            place ? (SWP_NOACTIVATE | SWP_SHOWWINDOW)
                  : (SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
    if (overlay_under_app(game)) fails = 0;
    else {
        ++fails;
        // Name what sits directly under the app instead: the adjacency test
        // cannot tell a refused insert from an interloper.
        HWND nx = GetWindow(ui_main_hwnd(), GW_HWNDNEXT);
        char cls[48] = "";
        if (nx) GetClassNameA(nx, cls, sizeof cls);
        DWORD np = 0; if (nx) GetWindowThreadProcessId(nx, &np);
        zlog("   under-app insert did not take (ret=%d err=%lu fails=%d) under-app=%p cls=%s pid=%lu own=%p",
             (int)g_hud_swp_ret, g_hud_swp_err, fails, (void*)nx, cls, np,
             nx ? (void*)GetWindow(nx, GW_OWNER) : nullptr);
    }
}

// THE APP WINDOW'S OWNERSHIP BRACKET (2026-09-04), hud_swp's for the app.
// The app is owned by the client for its whole visible lifetime, and an
// OWNED window changing band restacks its owner group inside the call: the
// client is hoisted to the top of the normal band and the overlay -- which
// now lives directly under the app, in the normal band whenever the app is
// -- is crossed. Measured with a 0.2ms z-order sampler: the hook's TOPMOST
// on a return click put the overlay under the game for 11.4ms (the call
// itself blocks ~16ms synchronising with the owner's thread), the tick's
// demote for 8.3ms, "APP stays" for 1.5-2.7ms -- badge and dock blinking
// out on every return from a third app. The old note "the app's band exits
// measurably do not hoist" was made with the overlay TOPMOST above the app,
// where nothing of ours could be crossed. Every band move of the app goes
// through here: drop the owner, move, take it back. The owner-carry that
// ownership exists for happens during the CLIENT's activation, outside our
// calls, so it costs that nothing (same argument as hud_swp's).
// The two halves, for the calls that are not a SetWindowPos of the app but
// still move its owner group: the summon's client insert, and the
// return-from-a-third-app hand-over (activating an OWNED window hoists the
// group too, and USER32 re-placed the two owned siblings overlay-above-app
// for the ~10ms the cross-process raise took -- measured, 0.2ms sampler).
static HWND app_owner_drop() {
    HWND app = ui_main_hwnd();
    if (!app) return nullptr;
    HWND owner = (HWND)GetWindowLongPtrW(app, GWLP_HWNDPARENT);
    if (owner) SetWindowLongPtrW(app, GWLP_HWNDPARENT, 0);
    return owner;
}
static void app_owner_restore(HWND owner) {
    if (owner && ui_main_hwnd())
        SetWindowLongPtrW(ui_main_hwnd(), GWLP_HWNDPARENT, (LONG_PTR)owner);
}
// THE CLIENT'S TOPMOST BIT IS NEVER OURS TO SET, and a set bit is a broken
// desktop (Roblox over everything, no other app focusable) that outlives every
// gesture -- so it is checked after the one client z-move we make (the
// summon insert) and once per tick, and stripped on sight. A NOTOPMOST from
// our process on the client's window works (probed: ret=1, bit clear after).
// Logged as a WARN once so a recurrence is visible to the user, not silent.
static void client_topmost_guard(HWND game, const char* where) {
    if (!game || !IsWindow(game)) return;
    if (!(GetWindowLongW(game, GWL_EXSTYLE) & WS_EX_TOPMOST)) return;
    BOOL ok = SetWindowPos(game, HWND_NOTOPMOST, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    bool still = (GetWindowLongW(game, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    zlog("CLIENT TOPMOST at %s -> stripped ret=%d still=%d", where, (int)ok, (int)still);
    static bool told = false;
    if (!told) {
        told = true;
        ui_log("WARN", std::string("! the client was left topmost - cleared it (report this: ") + where + ")");
    }
}
static bool g_app_in_swp = false;   // inside one of OUR z-moves of the app
static BOOL app_swp(HWND after) {
    HWND app = ui_main_hwnd();
    if (!app) return FALSE;
    HWND owner = app_owner_drop();
    g_app_in_swp = true;
    BOOL ok = SetWindowPos(app, after, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    DWORD err = ok ? 0 : GetLastError();
    g_app_in_swp = false;
    app_owner_restore(owner);
    // The entry gate returns success without applying (measured, the
    // topmost-fight notes), so the read-back is by bit AND position: is the
    // app above the overlay? A TOPMOST that reads top=1 above_hud=0 was
    // swallowed. Cold-open finding 2026-09-05.
    if (after == HWND_TOPMOST && g_hud_hwnd) {
        bool above = false;
        for (HWND h = app; h; h = GetWindow(h, GW_HWNDNEXT))
            if (h == g_hud_hwnd) { above = true; break; }
        zlog("APP swp TOPMOST ret=%d err=%lu top=%d above_hud=%d fgpid=%lu",
             (int)ok, err, (GetWindowLongW(app, GWL_EXSTYLE) & WS_EX_TOPMOST) ? 1 : 0,
             above ? 1 : 0, [] { DWORD p = 0; GetWindowThreadProcessId(GetForegroundWindow(), &p); return p; }());
    }
    return ok;
}

// The app window was restacked by someone who is NOT us (its own activation
// on a click anywhere on it, another process, USER32's owner enforcement).
// The app is owned by the client, so an activation raise carries the owner
// group -- the CLIENT rises with it, ABOVE the overlay, which is not part of
// the group while the console is foreground. Measured 2026-09-04 (0.2ms
// sampler): a click on the app's TITLE BAR dropped the dock under the game
// for ~26ms until the tick's glue caught it -- the user's "clicking the top
// bar makes the docks flash". Re-glue NOW, inside the same message the
// restack arrived on, so it never reaches a composed frame. Called from the
// app's wndproc (eclipse_ui.cpp) on every z-order WM_WINDOWPOSCHANGED.
void app_zorder_changed(const void* wp) {
    if (g_app_in_swp) return;                       // our own move; already handled
    const WINDOWPOS* p = (const WINDOWPOS*)wp;
    if (p->flags & SWP_NOZORDER) return;            // move/size/show, not z
    if (!app_in_front() || !g_hud_hwnd) return;     // app hidden: the overlay is TOPMOST
    // NO overlay_under_app early-out. Measured 2026-09-04 (Z snapshot inside
    // this handler): by the time the app's activation delivers this message,
    // USER32 has ALREADY carried the client above the overlay (app rank 28,
    // game 31, overlay 40) -- the carry is a move on the CLIENT, another
    // process, so it sends us nothing and only this app message is left to
    // react to. An "already under" test read the pre-carry instant and
    // skipped the fix. raise_overlays re-asserts the slot (overlay to rank 29,
    // under the app) in the same message; it no-ops its own SetWindowPos when
    // the position is right. Titlebar-click flash: 26ms -> ~7ms (the residual
    // is the window between the kernel carry and this thread's message).
    zlog("APP external z-move -> re-glue overlay under app");
    raise_overlays();
}

static void overlay_owner_drop() {
    if (!g_hud_hwnd || !g_hud_owner) return;
    SetWindowLongPtrW(g_hud_hwnd, GWLP_HWNDPARENT, 0);
    g_hud_owner = nullptr;
    zlog("overlay owner -> 0 (click on client: activation must not redirect)");
}

static void raise_overlays() {
    // ONE SetWindowPos, because there is one window. That is not an
    // implementation detail, it is the fix for the last flicker: two overlay
    // windows were two sequential z-order changes with a composable frame
    // between them, and whichever was raised LAST spent that frame under the
    // game (proven by swapping the order and watching the blink move).
    // DeferWindowPos batching was tried and made it MORE frequent -- it is
    // documented as unreliable for topmost transitions -- so there was no
    // ordering fix; the dock had to become an element of this window
    // (docs/overlay-plan.md). Do not add a second composed window here
    // without re-reading that.
    //
    // With the app open and steady "ours above the game" means UNDER THE
    // APP, above the client -- see overlay_glue_under_app. Every caller
    // keeps its meaning: the client's raise still cannot cross the overlay,
    // because the app (owned by the client) is above both.
    if (g_hud_hwnd && app_in_front() && !g_app_box_covering) {
        overlay_glue_under_app(hud_client_cached(ec::state().pid), 0, 0, 0, 0, false);
        return;
    }
    // Open leg with the box still covering: fall through to the TOPMOST branch
    // so the overlay stays ABOVE the app and the box keeps hiding the unpainted
    // window (Fault A). The steady under-app slot is taken the moment the page
    // drops the box (APPUNCOVER -> raise_overlays with g_app_box_covering clear).
    if (g_hud_hwnd) {
        hud_swp(HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        // This call is the one that runs at the moments Windows' entry gate
        // is OPEN (the LL click hook, the foreground event -- see the globals
        // block for the measured gate), so a win here is the band fight
        // ending: clear the latch state that the tick's 1/s heals could
        // never clear themselves, or the desktop window stays restored and
        // the backoff stays on long after the overlay is back on top.
        if (GetWindowLongW(g_hud_hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) {
            g_hud_band_fails = 0; g_hud_band_told = false;
            g_hud_band_lost  = false;
            g_hud_suppressed = false;   // hud_tick re-hides the desktop window
        }
        // While the band is refused (hud_tick's heal keeps failing), TOPMOST
        // is a no-op here and the pre-emption this call exists for would be
        // lost with it -- so fall through to the same above-game slot the
        // demote path uses. Riding the normal band directly above the game
        // is what keeps the overlay visible when topmost cannot be held.
        if (g_hud_band_lost &&
            !(GetWindowLongW(g_hud_hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
            HWND game = hud_client_cached(ec::state().pid);
            if (game) {
                HWND above = GetWindow(game, GW_HWNDPREV);
                if (above != g_hud_hwnd)
                    hud_swp(above ? above : HWND_TOP, 0, 0, 0, 0,
                            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }
    }
    // No pre-emption stamp here -- see the comment above hud_swp. Raising is
    // mechanics; "trust this for 400ms" is a claim about the client's focus
    // that only a caller can make.
}

// TEMPORARY z-order tracing. Off unless ECLIPSE_ZLOG=1 is set, so it costs a
// getenv once and nothing after. Writes to %TEMP%\eclipse_z.log.
//
// Here because pixel sampling proved unreliable twice: it cannot tell OUR
// demote from DWM's activation animation, and both look like a blink. This
// records what this process actually asks for, with timestamps, which is the
// one thing no capture can misattribute.
static bool zlog_on() {
    static int on = -1;
    if (on < 0) { char* v = getenv("ECLIPSE_ZLOG"); on = (v && *v == '1') ? 1 : 0; }
    return on == 1;
}

// Instrument for the BAND-DIAG line in hud_tick. Position of a window in the
// desktop's top-level z-order, 0 = frontmost, -1 = not found. The count
// includes invisible windows, so an absolute rank is only comparable to
// another rank from the same walk -- equality is the meaningful read (it is
// how fg-window == picked-window was pinned in the 2026-08-28 hunt).
//
// A WITNESS, NOT A MIRROR. Reading back our own WS_EX_TOPMOST bit only proves
// we asked; it cannot go red when the ask did not take, which is the failure
// being hunted. Rank against the FOREGROUND window answers the actual
// question -- "are we drawing over the thing the user is looking at" --
// without trusting anything this process set.
static int z_rank(HWND target) {
    if (!target) return -1;
    int i = 0;
    for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT), i++)
        if (h == target) return i;
    return -1;
}
// One line with the ranks of app / overlay / client, for stepping through
// a multi-call sequence (the hook's return path). Ranks from one walk each,
// so only their ORDER is meaningful.
static void zsnap(const char* tag) {
    if (!zlog_on()) return;
    HWND game = hud_client_cached(ec::state().pid);
    zlog("Z %s: app=%d hud=%d game=%d", tag,
         z_rank(ui_main_hwnd()), z_rank(g_hud_hwnd), z_rank(game));
}

// With z_rank. The first REAL window sitting under `ours` -- i.e.
// what the overlay is covering. Round one of this diagnostic asked "are we
// above the foreground window", which is true and CORRECT whenever the client
// itself is foreground, so it could not separate the bug from normal
// operation. Naming the covered window's process can: if it comes back as a
// second Roblox pid, the panel is on the wrong client and that is the report.
//
// "Real" filters what the desktop is always full of -- invisible shells,
// owned popups, zero-size message windows -- none of which the user can see
// being covered. 200px is well under any client window and well over the
// helper windows.
static HWND z_under(HWND ours, DWORD* pid_out, int* rank_out) {
    bool past = false;
    int i = 0;
    for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT), i++) {
        if (h == ours) { past = true; continue; }
        if (!past) continue;
        if (!IsWindowVisible(h) || IsIconic(h) || GetWindow(h, GW_OWNER)) continue;
        RECT r;
        if (!GetWindowRect(h, &r)) continue;
        if ((r.right - r.left) < 200 || (r.bottom - r.top) < 200) continue;
        DWORD p = 0;
        GetWindowThreadProcessId(h, &p);
        if (pid_out)  *pid_out  = p;
        if (rank_out) *rank_out = i;
        return h;
    }
    return nullptr;
}

static void zlog(const char* fmt, ...) {
    if (!zlog_on()) return;
    static LARGE_INTEGER freq{}, start{};
    if (!freq.QuadPart) { QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&start); }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double ms = (double)(now.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;

    char path[MAX_PATH];
    GetTempPathA(MAX_PATH, path);
    strcat(path, "eclipse_z.log");
    FILE* f = fopen(path, "a");
    if (!f) return;
    char body[256];
    va_list a;
    va_start(a, fmt);
    vsnprintf(body, sizeof body, fmt, a);
    va_end(a);
    // The tick beside the QPC time: an external sampler stamps its own
    // GetTickCount64, and this is what lines the two up.
    fprintf(f, "%10.2f t%llu  %s\n", ms,
            (unsigned long long)(GetTickCount64() % 100000000ull), body);
    fclose(f);
}

// Z-MOVE WATCH on the windows the zlog cannot see from the inside. The zlog
// records what overlay.cpp ASKS for; a z-move performed by anything else --
// another TU, the webview library, USER32's owner enforcement, another
// process -- reaches the window only as WM_WINDOWPOSCHANGED. The app's
// wndproc (eclipse_ui.cpp) and the snippet windows' subclass (snipwin.cpp)
// forward those here. The CALL STACK is the discriminator no poller has: an
// in-process SetWindowPos carries its caller's frames above user32; a move
// asked for by another process (or by USER32's own enforcement passes)
// arrives with nothing of ours on the stack but the message pump. Frames are
// logged as module+offset; resolve offsets against the built exe with nm or
// objdump. Inert without ECLIPSE_ZLOG=1, like everything else in the trace.
// Built for the 2026-08-27 hunt (app raised over a focused snippet window
// ~150ms after its activation, no zlog line); kept, because "who moved this
// window" recurs in this tree.
void zwatch_poschanged(const char* who, HWND h, const void* wp) {
    if (!zlog_on()) return;
    const WINDOWPOS* p = (const WINDOWPOS*)wp;
    if (p->flags & SWP_NOZORDER) return;   // move/size/show traffic, not z
    zlog("ZWATCH %s hwnd=%p after=%p flags=%04X owner=%p fg=%p",
         who, (void*)h, (void*)p->hwndInsertAfter, (unsigned)p->flags,
         (void*)GetWindow(h, GW_OWNER), (void*)GetForegroundWindow());
    void*  frames[10];
    USHORT n = CaptureStackBackTrace(2, 10, frames, nullptr);
    char   stk[240];
    size_t off = 0;
    for (USHORT i = 0; i < n && off + 32 < sizeof stk; i++) {
        HMODULE m = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)frames[i], &m);
        char path[MAX_PATH] = "?";
        if (m) GetModuleFileNameA(m, path, MAX_PATH);
        const char* base = strrchr(path, '\\');
        base = base ? base + 1 : path;
        char name[24];
        snprintf(name, sizeof name, "%s", base);
        if (char* dot = strrchr(name, '.')) *dot = 0;   // basename, no extension
        off += snprintf(stk + off, sizeof stk - off, " %s+%llx", name,
                        (unsigned long long)((char*)frames[i] - (char*)m));
    }
    zlog("ZWATCH %s stk:%s", who, stk);
}

// PROMOTE INSTANTLY, DEMOTE ONLY ONCE THE CHANGE HAS SETTLED.
//
// An alt-tab is not one foreground change, it is three: the game loses focus,
// EXPLORER's switcher window takes it for a few milliseconds, then the target
// gets it. Reacting to the middle one means demoting our windows out of the
// topmost band and re-inserting them above the game -- and then promoting them
// straight back when the game activates. Traced on a live alt-tab:
//
//     17794.67  FG-EVENT = GAME
//     17797.55  panel -> TOPMOST
//     17804.29  FG-EVENT = other (explorer, the switcher)
//     17805.84  panel -> NOTOPMOST
//     17808.18  panel -> insert after ...      <- strips topmost
//
// That demote/promote cycle happens on EVERY alt-tab and is the flicker the
// user kept reporting while three rounds of pixel measurement blamed DWM's
// activation animation. The animation is real, but this is ours.
//
// Losing focus is not urgent -- nothing bad happens if we sit above the game a
// moment longer -- while regaining it is, because the game is raised the
// instant it activates. So the two directions get different rules: up now,
// down only after the foreground has stayed off the client for kSettleMs.
// TRIED AND REVERTED: delaying the demote until the foreground had stayed off
// the client for 200ms. It did what it claimed -- the trace showed demotes
// moving from ~10ms after the switcher took focus to 128-177ms, so we no longer
// demote mid-alt-tab -- and the flicker remained. So the demote above is real
// but is NOT what is seen. Its cost was immediate: our window then sat over a
// newly focused FULLSCREEN app for the whole settle. Do not re-add it without
// evidence that the demote is what someone is actually looking at.

// The app's answer to the overlay's band-lost glue, and it checks POSITION,
// not the topmost bit. Windows' entry gate can refuse a TOPMOST promote
// while returning success -- and it has also been WATCHED (2026-08-22,
// passive pixel/z capture of the user's own returns) granting the BIT
// without applying the MOVE: the app sat below a foreground client for
// 900ms+ while the band rule read app_top=1 and never retried; one promote
// line in the whole trace. A bit-only check cannot go red on that, which is
// exactly how the app kept "flashing" after the promote-ordering fix. The
// overlay never shows this failure because its demote glue re-anchors by
// POSITION every tick; this is that mechanism for the app. No-op whenever
// the app really is above the client, so the band rule stays the band's one
// owner -- this only places the window inside the normal band when the band
// mechanism has failed, the same ladder raise_overlays uses when the band
// is refused. Visible is the requirement; the band is the optimisation.
// app_ensure_above IS GONE (removed 2026-08-22, hours after it shipped) and
// must not come back while the app is owned. It was a per-tick POSITION heal
// for exactly the state described above -- and ownership made that state
// unreachable: the app is owned by the client for its whole visible lifetime
// (app_owns_window below), and USER32 will not let an owned window REST
// below its owner. Probed with a hostile external SetWindowPos inserting an
// owned window directly below the game: the call returns success and USER32
// restores the window in 0.24ms -- ~100x faster than the 16ms tick this heal
// ran on -- while the same insert on an UNOWNED control lands and stays (the
// control that proves the probe can fail). The 900ms burial that justified
// the heal predates the app's ownership. If the app is ever seen below a
// foreground client again, the owner link is what broke; fix that, not this.

static void CALLBACK hud_on_foreground(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    // The event names the NEW foreground window; GetForegroundWindow here can
    // already disagree with it. Two fast corrections go before the full tick
    // because this callback races DWM's next composed frame:
    DWORD pid = 0;
    if (hwnd) GetWindowThreadProcessId(hwnd, &pid);
    zlog("FG-EVENT hwnd=%p pid=%lu %s", (void*)hwnd, pid,
         (pid && pid == ec::state().pid) ? "= GAME" : "= other");
    // Focus mode ends when the foreground moves ANYWHERE that is not the
    // overlay: the game (a click through the region gap), another app
    // (alt-tab), or our own main window. The user chose that focus, so it is
    // not stolen back -- no hand-back from here. WebView2's browser process
    // is exempt: MoveFocus parks Win32 focus on the browser's input window,
    // and treating that as "someone else" would end the mode as it began.
    if (g_ov_focus && hwnd &&
        GetAncestor(hwnd, GA_ROOT) != g_hud_hwnd &&
        !(g_hudc.browser_pid && pid == g_hudc.browser_pid))
        overlay_focus_exit(false);
    // The stamped click-on-the-app from a third app (g_app_arrive_at) lands
    // here as OUR OWN foreground event. Now the raise is legal: we are the
    // foreground process, so a z-only SetWindowPos on the client is
    // honoured (the identical call from the hook is refused -- measured for
    // the badge summon). The client comes up as BACKDROP, not focus: the
    // user clicked the console to use it, so the keyboard stays here and
    // nothing calls SetForegroundWindow. Insert the client directly below
    // the app -- the app is simply the focused window at the top of the
    // normal band, so the band rule has nothing new to own -- and put the
    // overlay back between them (its slot is directly under the app; the
    // client's insert can leave it below the client).
    if (g_app_arrive_at && GetTickCount() - g_app_arrive_at < 600 &&
        pid && (pid == GetCurrentProcessId() ||
                (g_hudc.browser_pid && pid == g_hudc.browser_pid)) &&
        g_app_mode && !g_app_opening && !g_app_closing &&
        ui_main_hwnd() && IsWindowVisible(ui_main_hwnd())) {
        g_app_arrive_at = 0;
        HWND game = hud_client_cached(ec::state().pid);
        if (game && !IsIconic(game)) {
            // Directly under the OVERLAY when the overlay is already in its
            // slot under the app -- which it is: USER32's owner carry places
            // it there during the app's activation raise (the `EXTERNAL
            // z-move: after=<app>` line, 2026-09-04). Inserting after the APP
            // put the client BETWEEN app and overlay, and a cross-process
            // SetWindowPos on the client returns ~20ms later (its thread's
            // frame), so badge and dock sat under the game for a frame before
            // the raise below corrected it. After the app when the overlay is
            // not in its slot; the raise then glues it.
            HWND after = overlay_under_app(game) ? g_hud_hwnd : ui_main_hwnd();
            // NEVER INSERT THE CLIENT AFTER A TOPMOST WINDOW. SetWindowPos
            // makes a window placed after a topmost window TOPMOST ITSELF,
            // and nothing of ours would ever clear it: Roblox stuck above
            // every app on the desktop until restarted (user, 2026-09-04,
            // x-close from a third app: client ex 0x100 -> 0x108, z 39 -> 12).
            // Reachable since the app rides TOPMOST while the console is
            // foreground (console_fg): the x click activates the app, the
            // tick promotes app+overlay, and THIS insert then lands after the
            // topmost overlay -- a race the badge path does not run (the
            // badge never activates). When the slot is topmost, the client's
            // place is the top of the NORMAL band -- HWND_TOP on a non-topmost
            // window -- which is directly under every topmost window, app and
            // overlay included, with no bit.
            bool after_top = (GetWindowLongW(after, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
            zlog("APP summon: raising client under the %s%s",
                 after == g_hud_hwnd ? "overlay" : "app",
                 after_top ? " (topmost -> HWND_TOP of the normal band instead)" : "");
            if (after_top) after = HWND_TOP;
            // The app's owner dropped around the client's move, or USER32
            // re-places the owned app directly above its owner inside the
            // call -- under the overlay (4ms of dock-over-console measured).
            HWND owner = app_owner_drop();
            SetWindowPos(game, after, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            app_owner_restore(owner);
            client_topmost_guard(game, "summon");
            raise_overlays();
        }
    }
    // APP MODE closes when the user goes back to THE GAME, and only then.
    // Clicking into Roblox is the designed close (chosen over keeping the app
    // topmost: a buried window shows nothing, a closing one shows where the
    // state went). Any OTHER app taking foreground is not a decision about
    // the console: alt-tabbing to a browser and coming back to find the
    // console shut -- and your place in it gone -- is worse than finding it
    // behind the window you switched to, which is what every other window
    // does. So the test is the attached client's pid, not "not ours".
    // Not during either choreography leg: mid-open the game is still
    // foreground by design, and mid-close this would re-enter.
    // hand_to_game=false: the foreground already went where the user sent it.
    if (g_app_mode && !g_app_opening && !g_app_closing &&
        pid && pid == ec::state().pid) {
        // Three ways the client can come forward, and only one of them means
        // "close":
        //   * clicked, WITH the app in front -- the deliberate gesture. Close.
        //   * clicked, arriving from somewhere else -- the user is returning
        //     to the game the console sits over, so the console comes back
        //     with it rather than vanishing.
        //   * not clicked at all -- the client re-asserting foreground on its
        //     own, which it does about a second after we activate. Ignoring
        //     this is the whole reason the gesture is captured in the hook.
        bool clicked = (GetTickCount() - g_game_click_at) < 600;
        if (clicked && g_game_click_from) {
            zlog("APP close gesture (clicked the client from the app)");
            app_close_begin(false);
        } else if (ui_main_hwnd() && IsWindowVisible(ui_main_hwnd())) {
            // The app stays. Whether it also takes FOCUS depends on why the
            // client came forward, and the two cases are opposites:
            //
            //   clicked -- the user deliberately came back from a third app.
            //     The console comes back WITH the game and takes the
            //     keyboard, so it is usable immediately and the next click on
            //     the client is the ordinary close gesture. Asked for
            //     directly: "app shouldve take focus when roblox was
            //     focused".
            //   not clicked -- the client re-asserted foreground BY ITSELF,
            //     which it does every second or two. Taking focus there would
            //     yank the keyboard out of the game mid-play, repeatedly. It
            //     only gets raised.
            //
            // ONE move, into the band the tick would have chosen anyway.
            // This used to raise to HWND_TOP -- the top of the NON-topmost
            // band -- and leave hud_tick's band rule to promote it into the
            // topmost band up to 16ms later. Two z-order changes with the
            // client's own alt-tab raise between them. MEASURED 2026-08-22
            // (z-order sampled at ~1ms through staged alt-tab returns, 8
            // trials each): HWND_TOP + tick promote left the app under the
            // game avg 12.6ms / worst 51ms per return -- one to three
            // composed frames, the flash the user reported. This single
            // HWND_TOPMOST: avg 1.8ms / worst 2.8ms, which is the WinEvent
            // delivery latency itself and the same cost the panel rides
            // (0.7ms panel-only). The tick finds nothing to correct.
            zlog("APP stays (clicked=%d from_app=%d)%s",
                 (int)clicked, (int)g_game_click_from,
                 clicked ? " -> take focus" : " -> raise only");
            app_swp(HWND_TOPMOST);
            // The overlay back into its slot directly under the app (the
            // app's TOPMOST just left it behind in the normal band).
            raise_overlays();
            // A gate-swallowed promote no longer needs a position heal here:
            // the app is OWNED, so it cannot rest below the client whatever
            // happened to the TOPMOST move (see the app_ensure_above note).
            if (clicked) force_foreground(ui_main_hwnd());
        }
    }
    if (pid && pid == ec::state().pid) {
        // RAISE THE GAME ITSELF FIRST, if something is still on top of it.
        //
        // Windows gives foreground to a clicked window but does not always
        // RAISE it above an unrelated app, and when it does not, nothing here
        // ever did it for us -- every raise in this file is for OUR windows.
        // MEASURED 2026-09-08 (ECLIPSE_ZLOG, app closed, returning from a
        // terminal), the failing state:
        //
        //   ovz=9  gamez=29  wfp=10148  COVERING=10148 <== WRONG-WINDOW
        //
        // The overlay topmost at rank 9, ROBLOX buried at 29 with the terminal
        // over it. Foreground had reached the game -- the trace shows it every
        // cycle -- so this was never a lost hand-over. The game simply stayed
        // underneath, which is why the taskbar kept drawing and why the state
        // HELD until the user clicked a second time: nothing on our side ever
        // moved the game, and the second click is what finally did.
        //
        // The game came foreground: raise the overlay NOW, ahead of
        // hud_tick's preamble.
        //
        // Historical, and the reason there is only one window to raise:
        // PROMOTE ORDER DECIDED WHO BLINKED. With the panel and the dock as
        // two structurally identical windows (same class, ex-styles, hosting
        // -- probed, not assumed), whichever was raised LAST spent a frame
        // under the game, proven by swapping the order and watching the
        // blink move; DeferWindowPos made it worse, and no order put both
        // first. Merging the dock into this window is what removed the
        // choice. See raise_overlays and docs/overlay-plan.md.
        // THE EVENT IS A CLAIM, NOT A FACT, and it must be corroborated
        // before it buys a raise and a bridge. The client's own load-complete
        // foreground grab fires EVENT_SYSTEM_FOREGROUND with NO raise and
        // with GetForegroundWindow never agreeing -- captured twice at
        // auto-attach with the user in another app (round5 t=37724, live
        // t=159801): FG-EVENT = GAME, then 3-5ms later raw=0, the client
        // still buried (gamez 36/37) and Discord both foreground and covering
        // it. The old unconditional raise+stamp here put the panel in the
        // topmost band over Discord for the full 400ms bridge -- the
        // auto-attach flash. And because foreground never actually changed,
        // no later FG event arrived to clear the stamp early.
        //
        // The corroboration is the same covered-witness the band rule uses,
        // and it is SAFE AT THIS POINT for the same measured reason the
        // tab-in blink work records: on a REAL activation Windows raises the
        // client BEFORE this event is delivered (0.5-1.4ms), so by now the
        // centre probe already reads the client and a genuine alt-tab or
        // click never takes the skip. The skip is only for a grab that did
        // not really happen. (The CLICK bridge in the hook stays
        // unconditional -- it runs PRE-raise, when the client legitimately
        // still reads covered; gating that one would kill the pre-emption in
        // exactly its window of usefulness.)
        HWND game = hud_client_cached(ec::state().pid);
        if (game && hud_client_covered(game, ec::state().pid)) {
            zlog("FG-EVENT GAME uncorroborated (client still covered) -> no raise, no bridge");
        } else {
            raise_overlays();

            // AND TELL THE TICKS TO BELIEVE IT, for the same 400ms the click
            // pre-emption uses.
            //
            // The raise above is undone moments later without this. The ticks
            // below recompute focus from GetForegroundWindow, which -- as the
            // comment at the top of this function says -- can still disagree
            // with the event that just fired. When it does, they take their
            // "not focused" branch and re-insert the window directly above
            // the game BY Z-ORDER, and placing a topmost window below a
            // non-topmost one STRIPS its topmost status. So the sequence is:
            // hook raises, tick demotes, a later tick raises again -- a
            // visible blink caused entirely by us, on top of whatever DWM's
            // activation animation is doing.
            //
            // An alt-tab has no click to set the bridge, which is why this
            // path stamps it too.
            g_hud_preempt_at = GetTickCount();
        }
    } else if (pid) {
        // Foreground resolved to some OTHER app, so any live click-preempt
        // bridge is stale. Without this, clicking the game and alt-tabbing
        // away inside the bridge window left the panel topmost over the new
        // app until the 400ms expiry.
        g_hud_preempt_at = 0;
    }
    hud_tick();
}

// Clicking the game is the one focus change we can see COMING. A low-level
// mouse hook runs before the button-down is delivered to its target, so the
// panel can be topmost before Windows raises the game -- no correction
// afterwards, nothing to be late with.
//
// It fires on the ACT of focusing, not on the cursor resting somewhere, so the
// panel never climbs above an app just because the pointer drifted over the
// game. Every button counts, X1 and X2 included.
//
// The hook is on the UI thread and must return promptly, so the body is a
// couple of comparisons and one z-order-only SetWindowPos.
static HHOOK  g_hud_mouse_hook = nullptr;

static LRESULT CALLBACK hud_mouse_ll(int code, WPARAM w, LPARAM l) {
    // Travel click-through tracks the pointer HERE as well as in the tick:
    // the hook runs before the event is routed, so the style is right by the
    // time win32k decides who gets the click -- a press within one tick of
    // sliding off the badge would otherwise be eaten by a still-opaque
    // overlay (or pass through a still-transparent one over the badge).
    if (code == HC_ACTION && ct_following()) ct_follow_apply();
    // g_hud_hwnd, not g_hud_enabled: the overlay window serves a docked
    // snippet panel with the console panel switched off, and gating on the
    // setting is exactly the coupling that once stripped a docked window of
    // its pre-emption.
    if (code == HC_ACTION && g_hud_hwnd &&
        (w == WM_LBUTTONDOWN || w == WM_RBUTTONDOWN ||
         w == WM_MBUTTONDOWN || w == WM_XBUTTONDOWN)) {
        const MSLLHOOKSTRUCT* m = (const MSLLHOOKSTRUCT*)l;
        // NOT WindowFromPoint: it returns a WS_EX_TRANSPARENT window even
        // though the click passes straight through it, so while a travel had
        // the overlay click-through every click on the CLIENT was classified
        // as a click on the overlay -- and the client branch below, which is
        // the close gesture and the promote, never ran. CWP_SKIPTRANSPARENT
        // (with invisible and disabled) is the test that matches where the
        // click actually lands. Verified the two disagree: mid-travel,
        // WindowFromPoint said EclipseHudHost and this said WINDOWSCLIENT.
        HWND under = ChildWindowFromPointEx(GetDesktopWindow(), m->pt,
                                            CWP_SKIPINVISIBLE | CWP_SKIPDISABLED |
                                            CWP_SKIPTRANSPARENT);
        if (under) under = GetAncestor(under, GA_ROOT);
        DWORD upid = 0;
        if (under) GetWindowThreadProcessId(under, &upid);

        // The badge-target flyout dismisses on any click that is not on the
        // overlay. The page cannot hear those -- outside its rects the click
        // routes to the game -- so this hook is the only place that can say
        // "elsewhere". Cleared here as well as by the page's answering
        // FLYOUT:0, so a slow retract cannot buy a second CLICKAWAY.
        // ...but only for a click on the CLIENT. A click in another app is
        // not a dismissal: the user switched away, and coming back to the
        // game to find the switch gone is a menu closing behind their back.
        // The overlay's own clicks are the page's business (it dismisses
        // itself in capture), so this is exactly "clicked the game".
        if (g_fly_open && under != g_hud_hwnd &&
            upid && upid == ec::state().pid) {
            g_fly_open = false;
            zlog("FLYOUT clickaway");
            hud_push("CLICKAWAY");
        }

        // A click on the overlay itself is what counts as USING it, which is
        // what later hands focus back to the game. Hovering never counts:
        // dragging across it on the way somewhere is not an interaction.
        // Panel and docked panel are one window whose region is only the
        // visible pieces, so "on the overlay" is exactly "on something
        // clickable" -- WindowFromPoint respects the region.
        // In focus mode the overlay IS the focused app: a click on it is
        // ordinary use, not a borrow to be handed back.
        if (!g_ov_focus &&
            (under == g_hud_hwnd ||
             (under && GetAncestor(under, GA_ROOT) == g_hud_hwnd))) {
            // ...EXCEPT with app mode open, where the console is the thing in
            // front and a click on the overlay -- the badge, a docked
            // snippet's panel -- is ordinary use of the console, not a
            // borrowed click to give back. Handing focus to the client there
            // raised it, and the app and the overlay then reshuffled inside
            // the topmost band: the app flickered and the badge and dock
            // ended up BEHIND it (user report, clicking a dock with the app
            // open). Keep the focus where it is, and put the overlay back in
            // its slot against the app (under it, since 2026-09-04).
            // SUMMONED FROM ANOTHER APP. A click on the overlay while some
            // third app is foreground means "back to the game, with the
            // console" -- the overlay is a HUD over the client, so bringing
            // it up without the client leaves it floating over a browser.
            // Here rather than in the app-open path because this hook runs
            // BEFORE the click is delivered, and because it is the one place
            // that sees every badge, panel and dock click -- one owner, so
            // the panel target gets it for free.
            //
            // Raise ours before AND after the hand-over: focusing the client
            // raises the client, and anything of ours outside the band lands
            // under it (overlay_handback has carried that line since the
            // panel existed).
            {
                HWND  fgw   = GetForegroundWindow();
                DWORD fgpid = 0;
                if (fgw) GetWindowThreadProcessId(fgw, &fgpid);
                bool ours   = (fgpid == GetCurrentProcessId()) ||
                              (g_hudc.browser_pid && fgpid == g_hudc.browser_pid);
                bool client = (fgpid && fgpid == ec::state().pid);
                HWND game   = hud_client_cached(ec::state().pid);
                // NOT IN APP MODE. The summon is the badge-over-game gesture:
                // a click on the HUD while a third app is foreground means
                // "back to the game, bring the console". With the app OPEN
                // the console is already a full window, and a badge/dock
                // click is a CLOSE (the page sends WIN:CLOSE / DOCKCLICK ->
                // app_close_begin), which hands foreground to the game
                // itself. Marking the summon here ran overlay_handback's
                // OWN force_foreground(game) a tick later, racing the close's
                // -- two hand-overs and two owner rewrites in flight at once,
                // the app and overlay dropping behind the game between them
                // (user report 2026-09-04: badge-close from a third app
                // blinks both behind Roblox). One owner for the app-mode
                // close: the close path.
                if (!g_app_mode && !ours && !client && game) {
                    // MARK IT, do not do it here. Doing the hand-over inside
                    // the hook was measured REFUSED (`FF attach path ->
                    // ok=0`) and so was a plain z-order raise of the client:
                    // this runs BEFORE the click is delivered, so we have not
                    // yet earned the "owns the last input event" permission
                    // that makes handing foreground to another process legal.
                    //
                    // The PANEL has always got this right by doing it a tick
                    // later, from overlay_handback, once the click has landed
                    // on us. Same flag, same path -- app mode just has to
                    // stop opting out of it.
                    zlog("overlay clicked from another app -> summon client");
                    g_hud_used = true;
                }
            }
            if (g_app_mode) {
                raise_overlays();
                // Stamped because both meanings of this click hold the band:
                // ordinary app-mode use has fg true via the our-process
                // override anyway, and a summon from a third app hands
                // foreground to the client from overlay_handback a tick
                // later -- the bridge covers the gap so the badge does not
                // dip out of the band mid-choreography.
                g_hud_preempt_at = GetTickCount();
            } else {
                g_hud_used = true;
            }
        }

        // A click on the APP WINDOW while a third app is foreground: stamp
        // it (see g_app_arrive_at). Ordinary use -- the client or anything
        // of ours already foreground -- must not trip it, and nothing else
        // happens here: this hook runs before the click is delivered, so
        // the raise has to wait for the foreground event the click causes.
        if (g_app_mode && !g_app_opening && !g_app_closing &&
            ui_main_hwnd() && IsWindowVisible(ui_main_hwnd()) && under == ui_main_hwnd()) {
            HWND  fgw   = GetForegroundWindow();
            DWORD fgpid = 0;
            if (fgw) GetWindowThreadProcessId(fgw, &fgpid);
            bool ours   = (fgw == ui_main_hwnd()) ||
                          (fgpid && fgpid == GetCurrentProcessId()) ||
                          (fgpid && g_hudc.browser_pid &&
                           fgpid == g_hudc.browser_pid);
            bool client = (fgpid && fgpid == ec::state().pid);
            if (!ours && !client) {
                zlog("APP clicked from another app -> summon client under it");
                g_app_arrive_at = GetTickCount();
            }
        }

        // A click on the game is the one focus change we can see coming: the
        // hook runs before the button-down reaches its target, so ours is
        // above the game before Windows raises it.
        if (upid && upid == ec::state().pid) {
            // App mode's close gesture, captured HERE because this is the
            // only place that can tell a deliberate click on the game from
            // the game simply taking foreground -- and it does take it back
            // on its own, about a second after we activate (measured: every
            // open logged "APP shown fg=1" and then a GAME foreground event
            // 0.4-2s later with nobody touching the mouse). Closing on the
            // foreground event alone therefore shut the console by itself.
            //
            // WHICH window the click came FROM is the other half: clicking
            // the game while the app is in front is "I am done here", while
            // arriving at the game from some other app is just coming back
            // to what the console sits on top of, and must not close it.
            g_game_click_at = GetTickCount();
            // WHAT THE CLICK MEANS depends on where the user already was.
            // Closing is right when they are ALREADY at the console-over-game
            // pair -- whether the app itself has focus, or the app is open
            // and riding above a focused client and they click the game to
            // get back to it. It is wrong when the click is the ARRIVAL from
            // a third app: that is "back to what I was playing", and the
            // console coming with it is the point of the mode.
            {
                HWND  fgw   = GetForegroundWindow();
                DWORD fgpid = 0;
                if (fgw) GetWindowThreadProcessId(fgw, &fgpid);
                // "Already at the console/game pair" is the test, and OURS
                // has three faces: the app window, this process (dialogs,
                // floating snippet windows), and WebView2's BROWSER process
                // -- clicking anything inside a page, a docked snippet panel
                // included, parks Win32 focus on the browser's in-process
                // input child. Leaving the browser out cost a click: after
                // clicking a dock, the next click on the client read as
                // "arriving from somewhere else", took the return branch,
                // and only the click AFTER that closed the app.
                g_game_click_from = (fgw == ui_main_hwnd()) ||
                                    (fgpid && fgpid == ec::state().pid) ||
                                    (fgpid && fgpid == GetCurrentProcessId()) ||
                                    (fgpid && g_hudc.browser_pid &&
                                     fgpid == g_hudc.browser_pid);

                // ACT HERE, not in the foreground handler. This hook runs
                // BEFORE the click reaches the client, which is the whole
                // reason the panel's promote lives here too: by the time a
                // foreground event arrives, Windows has already activated
                // and raised the client, and anything we do is a frame or
                // more late -- which is exactly what "clicking Roblox does
                // nothing to the app" looked like, and what put the client's
                // raise above the app during a close.
                //
                // The foreground handler still runs and is still correct; it
                // is the path for the client coming forward WITHOUT a click
                // (it re-asserts foreground on its own every second or two),
                // and both paths funnel into the same two calls, so there is
                // no second copy of the decision.
                if (g_app_mode && !g_app_opening && !g_app_closing &&
                    ui_main_hwnd() && IsWindowVisible(ui_main_hwnd())) {
                    if (g_game_click_from) {
                        zlog("APP close gesture (hook: click on client)");
                        app_close_begin(false);
                    } else {
                        zlog("APP return (hook: click on client from elsewhere)");
                        zsnap("return.0");
                        // EXACTLY overlay_handback's summon sequence, because
                        // this is the same wish spelled backwards: there, a
                        // click on OUR badge from a third app means "back to
                        // the game, bring the console"; here a click on the
                        // GAME from a third app means the same thing. Raise
                        // ours, then hand foreground to the client -- and let
                        // the client's own activation carry the app up with
                        // it, which it does because the app is OWNED by it.
                        //
                        // WHAT WAS HERE AND WHY IT WENT (2026-09-07): this
                        // took foreground to the APP, with an app_swp(TOPMOST)
                        // lift and an app_owner_drop() ahead of it. Both
                        // existed to serve that destination. Focusing the app
                        // left the client unactivated, so it never rose above
                        // the third app and the stack came out game / third
                        // app / console -- the third app sandwiched.
                        //
                        // Sending foreground to the client fixed the stack but
                        // kept the other two lines, and they made it arrive in
                        // TWO VISIBLE STEPS: the TOPMOST lift dropped the third
                        // app behind the console immediately, then the
                        // hand-over -- >100ms on the AttachThreadInput path --
                        // dropped it behind the game. Reported as "it goes
                        // behind the app then behind roblox instead of
                        // instantly". The lift is the first of those steps and
                        // the disown is what stops the client's activation
                        // carrying the app in the second, so both go: one
                        // activation, one frame, one move.
                        //
                        // NOT a z-order raise of the client here instead --
                        // that is measured REFUSED in this hook (see the
                        // summon branch above): it runs BEFORE the click is
                        // delivered, so we have not yet earned the
                        // owns-the-last-input-event permission.
                        raise_overlays();
                        zsnap("return.1 overlay glued");
                        // NO app_owner_drop HERE, and that is a MEASURED
                        // negative, not an oversight. It was put back on
                        // 2026-09-07 to chase "clicking Roblox from a third app
                        // sometimes does not give focus back", on the theory
                        // that handing foreground across a live owner
                        // relationship was flaky. IT DID NOT FIX IT, and the
                        // symptom turned out to predate the whole return-path
                        // rewrite -- it also happens with the app CLOSED, where
                        // this branch never runs (it is guarded on g_app_mode).
                        // So the disown bought nothing and cost the single-step
                        // arrival, which is what it was removed for.
                        HWND back = hud_client_cached(ec::state().pid);
                        force_foreground(back ? back : ui_main_hwnd());
                        zsnap("return.2 fg to client");
                    }
                }
            }
            // No cover is needed here for the close-path content flash any
            // more: the app rides the topmost band whenever the client or
            // the console is foreground (the band rule in hud_tick), so the
            // client's activation raise cannot compose over it before the
            // close box does. A hook-time TOPMOST lift was shipped for one
            // build and removed as redundant when the band rule landed --
            // its tick backstop and the band rule demoted/promoted each
            // other sixty times a second (1067 fight lines in one trace).
            // Do not gate this raise: it is one of the few moments Windows'
            // topmost entry gate is open, and skipping it leaves the badge to
            // wait for the tick's reactive heal -- a visible blink (tried).
            // BEFORE THE CLICK IS ROUTED. The hook runs ahead of delivery, so
            // this is the one place the pair can be broken in time for the
            // activation the click is about to cause.
            overlay_owner_drop();
            raise_overlays();
            // THE ORIGINAL BRIDGE, and the one stamp that is deliberately
            // unconditional: this hook runs BEFORE the click is delivered,
            // so the client is legitimately still covered/unraised here --
            // the pre-emption exists precisely for that window. The
            // activation is certain: the click is on its way to the client.
            g_hud_preempt_at = GetTickCount();
        }
    }
    return CallNextHookEx(nullptr, code, w, l);
}

// The hooks and the focus hand-back serve the overlay window whatever it
// carries, so they are owned by "is any overlay up", not by the panel.
//
// They used to live in the panel's create/destroy, and hud_tick calls
// hud_destroy the moment the panel is switched off in Settings -- so turning
// the panel off silently stripped a docked snippet window of its instant
// promote, its click pre-emption and its focus hand-back, leaving it on the
// 16ms tick alone. That is the "dock misbehaves with the HUD off" bug, and
// the reason the overlay window (not the setting) is the lifetime signal.
// The Insert hotkey -- the one door into and out of app mode that depends on
// no z-order, no hit region and no window being on top, which is what makes
// every other door testable when they misbehave. Registered on the overlay
// window (a thread-queue hotkey with a null HWND is not dispatched by this
// pump), and only while a client is attached: the hooks' install/release pair
// is exactly that lifetime, so the hotkey rides it. ONE named key.
static const UINT kAppHotkeyVk = VK_INSERT;
static const int  kAppHotkeyId = 0x0EC1;      // arbitrary, ours
static HWND       g_hotkey_hwnd = nullptr;    // where it is registered, if it is

static void app_hotkey_register() {
    if (g_hotkey_hwnd || !g_hud_hwnd) return;
    // MOD_NOREPEAT: holding Insert must not machine-gun the toggle.
    if (RegisterHotKey(g_hud_hwnd, kAppHotkeyId, 0x4000 /*MOD_NOREPEAT*/, kAppHotkeyVk)) {
        g_hotkey_hwnd = g_hud_hwnd;
        zlog("HOTKEY registered on %p", (void*)g_hud_hwnd);
    } else {
        // Once per overlay window, not per tick: the register is retried by
        // every hud_install_hooks call, and a key owned by another app would
        // otherwise WARN sixty times a second.
        static HWND told = nullptr;
        if (told != g_hud_hwnd) {
            told = g_hud_hwnd;
            ui_log("WARN", "! Insert hotkey unavailable - another app owns it");
        }
    }
}

static void app_hotkey_unregister() {
    if (!g_hotkey_hwnd) return;
    UnregisterHotKey(g_hotkey_hwnd, kAppHotkeyId);
    g_hotkey_hwnd = nullptr;
}

void overlay_release_hooks() {
    if (g_hud_evhook) { UnhookWinEvent(g_hud_evhook); g_hud_evhook = nullptr; }
    if (g_hud_mouse_hook) { UnhookWindowsHookEx(g_hud_mouse_hook); g_hud_mouse_hook = nullptr; }
    app_hotkey_unregister();
}

// SetForegroundWindow with its documented fallback, shared by the hand-back
// and by focus mode's enter (the two directions of one problem). It refuses
// SILENTLY from a background process; borrowing the foreground thread's input
// queue for the one call is the documented way through -- detached again at
// once, because a lasting attachment is what made the cursor vanish over the
// panel when the window was owned. Returns whether the target took foreground.
static bool force_foreground(HWND target) {
    SetForegroundWindow(target);
    HWND fgw = GetForegroundWindow();
    if (fgw == target) { zlog("FF plain ok -> %p", (void*)target); return true; }
    DWORD me  = GetCurrentThreadId();
    DWORD his = fgw ? GetWindowThreadProcessId(fgw, nullptr) : 0;
    if (his && his != me && AttachThreadInput(me, his, TRUE)) {
        SetForegroundWindow(target);
        AttachThreadInput(me, his, FALSE);
    }
    bool ok = GetForegroundWindow() == target;
    zlog("FF attach path -> %p ok=%d", (void*)target, (int)ok);
    return ok;
}

// Hand the game its focus back after the overlay was USED. Lives out here for
// the same reason: a docked window needs it whether or not the panel exists.
static void overlay_handback(uint32_t pid, HWND game) {
    // Focus mode suspends the hand-back outright: the point of the mode is
    // that the overlay KEEPS focus while the user types. Exit clears the flag
    // before it asks for a hand-back, so the two cannot fight.
    if (g_ov_focus) return;
    // APP MODE suspends it for the same reason: the console is a real focused
    // window in front, so a click on the badge or a docked panel is ordinary
    // use of the console, not a click borrowed from the game. The hook stops
    // SETTING g_hud_used while app mode is open; this is the second half,
    // for a flag set just before the app opened -- otherwise the hand-back
    // fires late, raises the client, and the app and overlay reshuffle in the
    // band (the flicker, and the badge landing behind the app).
    //
    // EXCEPT when the click came from a THIRD app: then this is the summon,
    // and the client has to come up with us. It is the same work the panel
    // has always done here -- the reason it must happen on this path and not
    // in the hook is that the permission to hand foreground across processes
    // only exists once the click has been delivered to us.
    {
        HWND  fgw   = GetForegroundWindow();
        DWORD fgpid = 0;
        if (fgw) GetWindowThreadProcessId(fgw, &fgpid);
        bool ours   = (fgpid == GetCurrentProcessId()) ||
                      (g_hudc.browser_pid && fgpid == g_hudc.browser_pid);
        bool summon = !ours && !(fgpid && fgpid == pid);
        if (g_app_mode && !summon) { g_hud_used = false; return; }
    }
    if (!g_hud_used || !game) return;
    // Never mid-drag: dragging across an overlay is not an interaction.
    if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) ||
        (GetAsyncKeyState(VK_RBUTTON) & 0x8000)) return;

    HWND fgw = GetForegroundWindow();
    DWORD fgpid = 0;
    GetWindowThreadProcessId(fgw, &fgpid);
    if (fgpid == pid) { g_hud_used = false; return; }   // already where it belongs

    // Raise ours BEFORE handing focus over: focusing the game raises the game,
    // and anything of ours still out of the band lands under it.
    raise_overlays();
    // The bridge, stamped by the caller that means it: the activation is not
    // just imminent, the next line performs it.
    g_hud_preempt_at = GetTickCount();
    // Windows allows this from the process that received the LAST INPUT EVENT,
    // which is us -- the click landed on our window -- so it is not gated on
    // being the foreground process; force_foreground carries the input-queue
    // fallback for when it still refuses.
    force_foreground(game);
    g_hud_used = false;
}

// ---- focus mode ---------------------------------------------------------------
// Entered on the PAGE's say-so (HUD_FOCUS:1). The one sender is the docked
// snippet iframe's shim, relayed by the overlay page, when a click puts DOM
// focus on a text field in the snippet's page (kOverlayFrameShim has the
// protocol). App mode used to send it (step 2) and no longer needs to: it is
// a real focused window now. The Esc escape hatch is in the accelerator
// handler, controller-side, so it works from inside the iframe.
// ORDER OF ARRIVAL matters and is benign: the click that asks for this has
// already passed the LL hook (g_hud_used set) and possibly a tick's
// overlay_handback -- a force_foreground(game) that lands before ours. Both
// are our own calls on windows we hold the permission for; the enter below
// takes the foreground back from wherever the hand-back left it.
// The click that asked for it is this process receiving the last input event,
// which is exactly the case SetForegroundWindow documents as permitted from a
// background process -- the same permission overlay_handback has been spending
// in the other direction all along, so no RegisterHotKey is needed for entry.
// MEASURED both ways: a plain SetForegroundWindow from a background probe is
// REFUSED, and this path reports TAKEN. If force_foreground ever starts
// reporting REFUSED (zlog "FOCUS enter"), RegisterHotKey is the documented
// escape hatch -- see docs/overlay-plan.md.
static bool overlay_focus_enter() {
    if (g_ov_focus) return true;
    if (!g_hud_composed || !g_hud_hwnd || !g_hudc.ctrl) return false;

    // Clearing WS_EX_NOACTIVATE at runtime takes effect immediately, no
    // re-show needed. MEASURED: SetForegroundWindow succeeds even with the
    // style still set, so the clear is not for the programmatic take -- it is
    // so ordinary clicks keep re-activating the overlay for the duration of
    // the mode. It goes back on at exit: never-take-focus is the normal state.
    LONG_PTR ex = GetWindowLongPtrW(g_hud_hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(g_hud_hwnd, GWL_EXSTYLE, ex & ~WS_EX_NOACTIVATE);

    g_hud_used = false;   // a pending click-borrow must not buy a hand-back now
    g_ov_focus = true;    // before the foreground change: the FG event handler
                          // and the tick must already see the mode when it lands
    raise_overlays();
    bool took = force_foreground(g_hud_hwnd);
    zlog("FOCUS enter -> %s", took ? "foreground TAKEN" : "foreground REFUSED");
    if (!took) {
        SetWindowLongPtrW(g_hud_hwnd, GWL_EXSTYLE, ex);
        g_ov_focus = false;
        return false;
    }
    // Composition hosting has no SendKeyboardInput counterpart to
    // SendMouseInput; keyboard is focus-bound, full stop. MoveFocus hands
    // Win32 focus to WebView2's own input window (browser-process side), and
    // from there keys, IME and clipboard are the browser's own -- nothing to
    // forward.
    g_hudc.ctrl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    // Taking foreground is an activation RAISE to the top of our band. With
    // the app open that puts the dock over the console for a tick, so the
    // slot is re-applied here, inside the same call chain (a no-op with the
    // app closed: the overlay is TOPMOST already).
    raise_overlays();
    hud_push("FOCUSMODE:1");
    return true;
}

static void overlay_focus_exit(bool hand_to_game) {
    if (!g_ov_focus) return;
    g_ov_focus = false;
    if (g_hud_hwnd && IsWindow(g_hud_hwnd)) {
        LONG_PTR ex = GetWindowLongPtrW(g_hud_hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(g_hud_hwnd, GWL_EXSTYLE, ex | WS_EX_NOACTIVATE);
    }
    if (hand_to_game) {
        // Reuse the ONE hand-back -- the raise-before-handover ordering and
        // the input-queue fallback live there -- rather than a second copy.
        HWND game = hud_client_cached(ec::state().pid);
        if (game) {
            g_hud_used = true;
            overlay_handback(ec::state().pid, game);
        }
    }
    hud_push("FOCUSMODE:0");
    zlog("FOCUS exit (hand_to_game=%d)", (int)hand_to_game);
}

static void hud_install_hooks() {
    if (!g_hud_evhook)
        g_hud_evhook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                       nullptr, hud_on_foreground, 0, 0,
                                       WINEVENT_OUTOFCONTEXT);
    if (!g_hud_mouse_hook)
        g_hud_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, hud_mouse_ll,
                                             GetModuleHandleW(nullptr), 0);
    app_hotkey_register();
}

// One handler for a page message, shared by both hosting modes: the windowed
// wrapper delivers it through the hud_send binding, the composed path through
// WebMessageReceived. Same strings either way.
static void hud_on_msg(const std::string& msg) {
    // Every inbound page message, under the same ECLIPSE_ZLOG gate as the
    // z-order trace. This is what distinguishes "the page never sent it"
    // from "the host ignored it" -- the two look identical from outside,
    // and a screen capture cannot tell them apart at all.
    zlog("MSG <%s>", msg.substr(0, 90).c_str());
    if (msg.rfind("HUD_TOGGLE:", 0) == 0) {
        bool want = (msg.substr(11) != "0");
        if (want != g_hud_expanded) {
            g_hud_expanded = want;
            // OPENING un-clips now, so the panel has somewhere to grow
            // into. CLOSING waits for the page to report its fade finished
            // -- clipping mid-animation would cut it off at frame one --
            // and that message arrives here as HUD_TOGGLE:0 from
            // transitionend.
            hud_apply_region(want);
        }
    } else if (msg.rfind("HUD_FOCUS:", 0) == 0) {
        // Which click means "give me the keyboard" is the PAGE's call -- the
        // host telling from a click position would be a second copy of the
        // page's layout, this project's recurring bug class. Sent by the
        // docked snippet's frame shim (text-field focus) and relayed by the
        // overlay page; opening the console panel never does (see g_ov_focus).
        if (msg.substr(10) != "0") overlay_focus_enter();
        else                       overlay_focus_exit(true);
    } else if (msg.rfind("DOCKCLICK:", 0) == 0) {
        // A click landed inside the DOCKED snippet panel (the frame shim's
        // mousedown, relayed by the overlay page). The dock is in-game UI, so
        // with the app window open this means "back to the game": close the
        // app to the badge and hand focus to the client -- the same reading
        // as clicking the client itself (app_close_begin, hand_to_game=true).
        // Asked for by the user 2026-09-04. ONLY while the app is open: with
        // it closed a dock click is ordinary use (focus mode etc.), and the
        // page only sends this for a non-passthrough dock, so a readout panel
        // never triggers it. The leg guard inside app_close_begin ignores a
        // click mid-choreography.
        if (g_app_mode && !g_app_opening && !g_app_closing)
            app_close_begin(true);
    } else if (msg.rfind("RECTS:", 0) == 0) {
        // The page's interactive rects -- the hit-test region. See
        // hud_set_rects for why the page reports them.
        hud_set_rects(msg.substr(6));
        zlog("RECTS -> %d solid rects", (int)g_ov_rects.size());  // parsed, not just received
    } else if (msg.rfind("ANIM:", 0) == 0) {
        // Page animation telemetry (frame-gap stats per open/close run). The
        // MSG trace above already recorded it; nothing else to do -- this
        // branch only keeps it out of handle()'s command chain.
    } else if (msg.rfind("SNIPMSG:", 0) == 0) {
        // A docked snippet page's eclipse_send, relayed by the overlay page
        // (the iframe cannot post to us directly: chrome.webview in a child
        // frame raises CoreWebView2Frame events nothing here subscribes to).
        // Body is "<run>:<payload>" -- the id the DOCKP snapshot stamped on
        // the page -- so the message lands in the OWNING run's deque, the
        // docked path's substitute for the floating binding's closure.
        snip_dock_msg(msg.substr(8));
    } else if (msg == "HUD_READY") {
        // The page is listening. Forget what was "already sent" -- a fresh
        // page knows none of it -- and hand it the current state. DOCKP is in
        // the replay set, which is what re-materialises a docked snippet
        // panel after an overlay rebuild (detach/reattach) for free.
        { std::lock_guard<std::mutex> lk(g_hud_last_mux); g_hud_last.clear(); }
        g_fly_open = false;   // a fresh page starts with the flyout closed
        push_state();
        push_runstate();
        push_hud_snippets();
        push_hudvis();
        push_overlay_pref();      // what the badge opens
        push_dock();
        // App mode does NOT replay: the app is a real window that survives an
        // overlay rebuild on its own. A rebuild mid-choreography is the
        // watchdog's case (hud_tick), not this one.
    } else if (msg == "APP_ENTER") {
        // The badge, clicked while collapsed in overlay mode: a REQUEST. The
        // host runs the whole choreography from here (APPOPEN back to the
        // page); the page changes nothing on its own say-so, so the two
        // sides cannot disagree about whether the app is open.
        app_open_begin();
    } else if (msg == "APP_EXIT") {
        // The badge, clicked while the app is open: the request to close.
        app_close_begin(true);
    } else if (msg.rfind("FLYOUT:", 0) == 0) {
        // The page's badge-target flyout, out or gone. Tracked only so the
        // LL hook knows when an off-overlay click owes the page a CLICKAWAY.
        g_fly_open = msg.substr(7) != "0";
    } else if (msg == "APPIDLE") {
        // The close travel landed: the overlay is back to the badge, so
        // it can take input again.
        g_travel_ct = false;
        overlay_clickthrough(false);
    } else if (msg == "APPUNCOVER") {
        // The page has DROPPED the open box. Since the paint-witness
        // sequencing (app_open_uncover) the overlay is already under the app
        // by the time APPSHOWN goes out, so this is a belt only: it clears
        // the hold if some path left it set and re-asserts the slot, which
        // no-ops when it already holds. It must never be the thing that
        // first moves the overlay under the app -- measured 1-3 frames of a
        // docked panel over the app when it was (the page's drop composes
        // before this message lands).
        g_app_box_covering = false;
        raise_overlays();
    } else if (msg == "APP_ATRECT") {
        // Open leg 2: the travel box has landed on the app's rect.
        app_open_finish();
    } else if (msg.rfind("APP_BOXUP", 0) == 0) {
        // Prefix match: the page suffixes which witness fired (":present
        // lag=..", ":timeout", ":raf") for the MSG trace line above.
        // Close leg 2: the box is composed over the window.
        app_close_finish();
    } else if (msg == "HUD_ARM_TOGGLE") {
        handle(ec::state().armed ? "ARM:0" : "ARM:1");
    } else {
        handle(msg);                          // STOP and anything else
    }
}

// Input, forwarded by hand: a composition-hosted webview has no HWND of its
// own, so every mouse message that lands on the host window is relayed with
// SendMouseInput. The COREWEBVIEW2_MOUSE_EVENT_KIND values ARE the WM_ codes,
// so the message number passes through unchanged. Keyboard is not forwarded
// and CANNOT be -- composition hosting has no SendKeyboardInput counterpart.
// It does not need one: WebView2 still creates an in-process input child
// (Chrome_WidgetWin_0) under this host, and once MoveFocus parks Win32 focus
// there (focus mode, overlay_focus_enter) keys, IME and clipboard are the
// browser's own. MEASURED in an isolated composed-host harness: host
// foreground alone delivers NO keys; foreground + MoveFocus delivers
// everything, Ctrl+C to the real clipboard included.
static void composed_forward_mouse(ComposedHost* ch, UINT m, WPARAM w, LPARAM l) {
    if (!ch || !ch->comp) return;
    UINT32 data = 0;
    DWORD  keys = GET_KEYSTATE_WPARAM(w);
    POINT  pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
    if (m == WM_MOUSEWHEEL || m == WM_MOUSEHWHEEL) {
        // Wheel messages carry SCREEN coordinates, alone among the set.
        data = (UINT32)(DWORD)GET_WHEEL_DELTA_WPARAM(w);
        ScreenToClient(ch->hwnd, &pt);
    } else if (m == WM_XBUTTONDOWN || m == WM_XBUTTONUP) {
        data = GET_XBUTTON_WPARAM(w);
    } else if (m == WM_MOUSELEAVE) {
        pt   = POINT{ 0, 0 };
        keys = 0;
    }
    ch->comp->SendMouseInput((COREWEBVIEW2_MOUSE_EVENT_KIND)m,
                             (COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS)keys,
                             data, pt);
}

// One wndproc for every composed host; the instance rides in GWLP_USERDATA
// (set immediately after CreateWindowEx, so the early creation messages see
// null and fall through to DefWindowProc, which is right for them).
static LRESULT CALLBACK hud_host_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    ComposedHost* ch = (ComposedHost*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!ch) return DefWindowProcW(h, m, w, l);
    switch (m) {
    case WM_HOTKEY:
        // Insert: the z-order-independent door. A leg already in flight is
        // left to land (mid-travel toggles are ignored by the begin
        // functions' guards, same as the badge).
        //
        // CLOSING STAYS HERE, OPENING ASKS THE PAGE. The two halves have
        // different requirements:
        //
        //   * Closing an open app must depend on nothing -- not z-order, not
        //     the hit region, not the page being alive. That is the property
        //     this hotkey exists for, and it is why the close is a direct
        //     call rather than a message.
        //   * Opening is a question only the PAGE can answer, because
        //     badgeTarget lives there: with the badge set to the panel,
        //     Insert should open the PANEL. It used to open the app whatever
        //     the badge said, which left no keyboard route to the panel --
        //     and a game holding the mouse makes the keyboard the only route
        //     there is (user report: mouse locked to centre, badge
        //     unclickable, Insert opened the wrong thing).
        //
        // Gated on g_overlay_pref -- APP-AS-HUD -- and NOT on
        // overlay_wanted(), which is merely "should the overlay window
        // exist" and is also true for a plain in-game panel with app mode
        // off. Routing on that sent Insert to the page in desktop mode with
        // the panel switched on, where the page has no overlayMode and so
        // toggled the PANEL instead of opening the app: a behaviour change
        // nobody asked for, in the mode this hotkey has always served
        // directly. g_overlay_pref is the same fact the page calls
        // overlayMode, so both sides branch on one condition.
        if ((int)w == kAppHotkeyId) {
            zlog("HOTKEY insert (app_mode=%d hud=%d pref=%d)",
                 (int)g_app_mode, (int)(g_hud_hwnd != nullptr), (int)g_overlay_pref);
            if (g_app_mode) {
                app_close_begin(true);
            } else if (g_hud_hwnd && g_overlay_pref) {
                // SUMMONED FROM ANOTHER APP. Pressing Insert while a third
                // app is foreground means "back to the game, with the
                // console" -- the panel is a HUD over the client, so raising
                // it without the client leaves it floating over a terminal.
                // Exactly what a badge click from a third app already does;
                // this marks the SAME flag and lets overlay_handback do the
                // work a tick later, rather than growing a second summon.
                // Doing it inline is refused anyway -- the panel has always
                // had to spend that permission from the hand-back path.
                HWND  fgw   = GetForegroundWindow();
                DWORD fgpid = 0;
                if (fgw) GetWindowThreadProcessId(fgw, &fgpid);
                bool ours   = (fgpid == GetCurrentProcessId()) ||
                              (g_hudc.browser_pid && fgpid == g_hudc.browser_pid);
                bool client = (fgpid && fgpid == ec::state().pid);
                if (!ours && !client && hud_client_cached(ec::state().pid)) {
                    zlog("HOTKEY from another app -> summon client");
                    g_hud_used = true;
                }
                hud_push("HOTKEY");
            } else {
                app_open_begin();
            }
            return 0;
        }
        break;
    case WM_WINDOWPOSCHANGING:
        // VETO an external raise while the app is on glass (2026-09-04,
        // fault 4's last half). A click on a docked panel focuses Chromium's
        // in-process input child, which ACTIVATES this host, and the
        // activation raises it to HWND_TOP -- above the app -- with
        // WM_WINDOWPOSCHANGING sent (flags 1803 in the trace: no
        // NOSENDCHANGING), so the move can be refused HERE, before it
        // happens, instead of corrected ~1ms after it on CHANGED (the
        // catcher below stays as the backstop). Only ACTIVATION raises: the
        // trace separates them cleanly -- an activation raise carries flags
        // 1803 (no SWP_NOACTIVATE), USER32's owner carry 1813 (NOACTIVATE
        // set) -- and the carry is a move we rely on (it places the overlay
        // above the rising client on a tab-in), so it passes, as does
        // anything already "after the app". Ours pass (hud_swp).
        if (!g_hud_in_swp && app_in_front()) {
            WINDOWPOS* p = (WINDOWPOS*)l;
            if (!(p->flags & SWP_NOZORDER) && !(p->flags & SWP_NOACTIVATE) &&
                p->hwndInsertAfter != ui_main_hwnd()) {
                zlog("overlay EXTERNAL z-move VETOED: after=%p flags=%04X",
                     (void*)p->hwndInsertAfter, (unsigned)p->flags);
                p->flags |= SWP_NOZORDER;
            }
        }
        break;
    case WM_WINDOWPOSCHANGED:
        // The actor-catcher for the topmost fight, and why it is on CHANGED
        // not CHANGING: an external SetWindowPos delivers these messages to
        // OUR thread (the zlog only records what WE asked for), and
        // SWP_NOSENDCHANGING suppresses WM_WINDOWPOSCHANGING but NOT
        // WM_WINDOWPOSCHANGED -- so an actor passing that flag was invisible
        // while we watched CHANGING. CHANGED is always sent, closing the
        // hole. Counted in one place so a normal move's CHANGING+CHANGED
        // pair is not double-counted. g_hud_in_swp filters our own calls;
        // SWP_NOZORDER filters our move/size/show traffic that is not a
        // z-order change.
        if (!g_hud_in_swp && !(((const WINDOWPOS*)l)->flags & SWP_NOZORDER)) {
            const WINDOWPOS* p = (const WINDOWPOS*)l;
            g_hud_ext_zmoves++;
            zlog("overlay EXTERNAL z-move: after=%p flags=%04X",
                 (void*)p->hwndInsertAfter, (unsigned)p->flags);
            // With the app open, an external z-move of the overlay is it
            // being RAISED ABOVE the app: clicking a docked snippet panel
            // parks Win32 focus on the browser's in-process child, whose
            // activation raises this host to the top of its band -- the dock
            // flashing IN FRONT of the app (fault 4, 2026-09-04). Re-glue it
            // directly under the app in the same message, so the raise never
            // reaches a composed frame. app_steady() keeps this out of the
            // choreography; raise_overlays no-ops when already under.
            if (app_in_front() && !overlay_under_app(hud_client_cached(ec::state().pid))) {
                zlog("   -> re-glue overlay under app");
                raise_overlays();
            }
        }
        break;
    case WM_MOUSEMOVE:
        // Arm a WM_MOUSELEAVE, or hover states stick when the cursor exits
        // onto the game -- the browser never hears that the pointer left.
        if (!ch->tracking) {
            TRACKMOUSEEVENT t{ sizeof t, TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            ch->tracking = true;
        }
        composed_forward_mouse(ch, m, w, l);
        return 0;
    case WM_MOUSELEAVE:
        ch->tracking = false;
        composed_forward_mouse(ch, m, w, l);
        return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
        // A button arriving while WS_EX_TRANSPARENT is set is a routing
        // failure by definition -- click-through means these must land on the
        // game -- so it is logged, not just dropped. This line is the
        // instrument that pinned the close-travel dead region: hit-test APIs
        // called WITH CWP_SKIPTRANSPARENT skip the window, but win32k's real
        // dispatch only skips a TRANSPARENT window when it is LAYERED too.
        if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TRANSPARENT) {
            zlog("overlay ATE msg=%04X at %d,%d while transparent (closing=%d)",
                 m, GET_X_LPARAM(l), GET_Y_LPARAM(l), (int)g_app_closing);
        }
        // Capture, so a drag on the log's scrollbar keeps sending moves after
        // the pointer leaves the window. Capture is not focus; nothing
        // activates.
        SetCapture(h);
        composed_forward_mouse(ch, m, w, l);
        return 0;
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
        if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TRANSPARENT) {
            zlog("overlay ATE msg=%04X at %d,%d while transparent (closing=%d)",
                 m, GET_X_LPARAM(l), GET_Y_LPARAM(l), (int)g_app_closing);
        }
        ReleaseCapture();
        composed_forward_mouse(ch, m, w, l);
        return 0;
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        composed_forward_mouse(ch, m, w, l);
        return 0;
    case WM_SETCURSOR:
        // The browser's cursor, mirrored from CursorChanged. Without this
        // the class arrow fights the page's pointer/text cursors.
        if (LOWORD(l) == HTCLIENT) {
            SetCursor(ch->cur ? ch->cur : LoadCursor(nullptr, IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_MOUSEACTIVATE:
        // Focus mode: an ordinary active window -- refusing activation here
        // would fight the clicks that move DOM focus while the user types.
        // Otherwise the old rule: belt to WS_EX_NOACTIVATE's braces.
        return g_ov_focus ? MA_ACTIVATE : MA_NOACTIVATE;
    }
    return DefWindowProcW(h, m, w, l);
}

// Failed-create and destroy share this; g_hud_composed doubles as "the
// composed path owns g_hud_hwnd".
static void composed_destroy(ComposedHost& ch) {
    if (ch.core) {
        ch.core->remove_WebMessageReceived(ch.tok_msg);
        ch.core->Release();
        ch.core = nullptr;
    }
    if (ch.comp) {
        ch.comp->remove_CursorChanged(ch.tok_cur);
        ch.comp->Release();
        ch.comp = nullptr;
    }
    if (ch.ctrl) {
        ch.ctrl->remove_AcceleratorKeyPressed(ch.tok_key);
        ch.ctrl->Close(); ch.ctrl->Release(); ch.ctrl = nullptr;
    }
    if (ch.dcvis) { ch.dcvis->Release(); ch.dcvis = nullptr; }
    if (ch.dctgt) { ch.dctgt->Release(); ch.dctgt = nullptr; }
    if (ch.dcdev) { ch.dcdev->Release(); ch.dcdev = nullptr; }
    if (ch.hwnd) {
        // Detach the wndproc's instance pointer BEFORE the window dies, so a
        // straggling message cannot dereference a dead host.
        SetWindowLongPtrW(ch.hwnd, GWLP_USERDATA, 0);
        if (IsWindow(ch.hwnd)) DestroyWindow(ch.hwnd);
        ch.hwnd = nullptr;
    }
    ch.cur         = nullptr;
    ch.tracking    = false;
    ch.on_msg      = nullptr;
    ch.browser_pid = 0;
}

static void hud_teardown_composed() {
    composed_destroy(g_hudc);
    if (g_hud_composed) g_hud_hwnd = nullptr;   // was a mirror of g_hudc.hwnd
    g_hud_composed = false;
}


// Everything a composed window needs, in one place: environment reuse, dcomp
// tree, controller, alpha-0 background, pinned rasterization scale, message
// and cursor wiring. Pumps while WebView2 finishes creating -- the caller must
// have g_hud_creating (or equivalent) holding the ticks out. Returns with
// ch.core live but NOT navigated; content is the caller's business.
//
// init_script, when given, is installed with AddScriptToExecuteOnDocumentCreated
// before this returns, so it is in force for whatever the caller loads first --
// the docked snippet window injects window.eclipse_send with it.
static bool composed_create(ComposedHost& ch, const wchar_t* title, int w, int h,
                            void (*on_msg)(const std::string&),
                            const std::wstring* init_script) {
    if (!ui_page_live() || ch.hwnd) return false;

    // The main window's environment, reused: one browser process, and the new
    // controller lands on the same UI thread everything else here runs on.
    auto* mainctl = static_cast<ICoreWebView2Controller*>(ui_browser_controller());
    if (!mainctl) return false;
    ICoreWebView2*             maincore = nullptr;
    ICoreWebView2_2*           core2    = nullptr;
    ICoreWebView2Environment*  env      = nullptr;
    ICoreWebView2Environment3* env3     = nullptr;
    if (FAILED(mainctl->get_CoreWebView2(&maincore)) || !maincore) return false;
    if (SUCCEEDED(maincore->QueryInterface(kIID_WebView2_2, (void**)&core2)) && core2) {
        core2->get_Environment(&env);
        core2->Release();
    }
    maincore->Release();
    if (!env) return false;
    HRESULT hr = env->QueryInterface(kIID_Environment3, (void**)&env3);
    env->Release();
    if (FAILED(hr) || !env3) return false;  // runtime predates composition hosting

    // dcomp by hand: no import-library dependency, and a machine without it
    // (should not exist on Win10+) just takes the fallback.
    static HMODULE dclib = LoadLibraryW(L"dcomp.dll");
    auto create2 = dclib ? (HRESULT(WINAPI*)(IUnknown*, REFIID, void**))
                            GetProcAddress(dclib, "DCompositionCreateDevice2")
                         : nullptr;
    if (!create2) { env3->Release(); return false; }

    static ATOM cls = 0;
    if (!cls) {
        WNDCLASSW wc{};
        wc.lpfnWndProc   = hud_host_wndproc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"EclipseHudHost";
        cls = RegisterClassW(&wc);
        if (!cls) { env3->Release(); return false; }
    }
    // NOACTIVATE before the first show, same as the fallback -- see the note
    // at the top of this section. NOREDIRECTIONBITMAP removes the window's
    // own surface, so the composition visual is ALL there is and its alpha
    // shows the game instead of a legacy redirection bitmap.
    ch.hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP,
                              L"EclipseHudHost", title, WS_POPUP,
                              0, 0, w, h,
                              nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!ch.hwnd) { env3->Release(); return false; }
    ch.on_msg = on_msg;
    SetWindowLongPtrW(ch.hwnd, GWLP_USERDATA, (LONG_PTR)&ch);

    // Win11 rounds some top-level windows on its own; that would clip the
    // visual's corners with a second, unrelated radius.
    { enum { CORNER_PREFERENCE = 33 }; int corner = 1;   // DWMWCP_DONOTROUND
      DwmSetWindowAttribute(ch.hwnd, CORNER_PREFERENCE, &corner, sizeof corner); }

    bool treeok = SUCCEEDED(create2(nullptr, __uuidof(IDCompositionDesktopDevice),
                                    (void**)&ch.dcdev)) && ch.dcdev &&
                  SUCCEEDED(ch.dcdev->CreateTargetForHwnd(ch.hwnd, TRUE, &ch.dctgt)) &&
                  SUCCEEDED(ch.dcdev->CreateVisual(&ch.dcvis)) &&
                  SUCCEEDED(ch.dctgt->SetRoot(ch.dcvis));
    if (!treeok) { env3->Release(); composed_destroy(ch); return false; }

    // Async by contract, pumped here until it lands -- the wrapper's ctor
    // pumps the same way, and the caller's creating-flag keeps re-entry out.
    bool done = false, failed = false;
    ComposedHost* pch = &ch;
    auto* cd = new HudCompDone(
        [&](HRESULT h2, ICoreWebView2CompositionController* c) -> HRESULT {
            if (SUCCEEDED(h2) && c) { pch->comp = c; pch->comp->AddRef(); }
            else failed = true;
            done = true;
            return S_OK;
        });
    hr = env3->CreateCoreWebView2CompositionController(ch.hwnd, cd);
    cd->Release();
    env3->Release();
    if (FAILED(hr)) { composed_destroy(ch); return false; }
    DWORD deadline = GetTickCount() + 15000;
    MSG msg;
    while (!done && GetTickCount() < deadline) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!done) MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
    }
    if (!done || failed || !ch.comp ||
        FAILED(ch.comp->QueryInterface(kIID_HudController, (void**)&ch.ctrl)) ||
        !ch.ctrl) {
        composed_destroy(ch);
        return false;
    }

    RECT b{ 0, 0, w, h };
    ch.ctrl->put_Bounds(b);
    ch.ctrl->put_IsVisible(TRUE);

    // Alpha 0: the page's transparent pixels ARE the window's shape. A page
    // that paints its background is exactly as opaque as it ever was; only
    // what the page leaves unpainted changes, from white to the game.
    { ICoreWebView2Controller2* c2 = nullptr;
      if (SUCCEEDED(ch.ctrl->QueryInterface(kIID_HudController2, (void**)&c2)) && c2) {
          COREWEBVIEW2_COLOR bg{ 0, 0, 0, 0 };
          c2->put_DefaultBackgroundColor(bg);
          c2->Release();
      } }

    // CSS px == device px, pinned. Composed hosting does not track DPI on its
    // own, and the geometry here -- window sizes, badge rect, hit region --
    // is written in physical pixels; letting a monitor scale factor in would
    // shear the page layout away from the region math. On a HiDPI monitor
    // the page renders at 100%: slightly small, and exactly where every
    // rectangle says it is (the windowed path, which cannot pin this,
    // instead scales the page and clips fixed layouts).
    { static const GUID kIID_HudController3 =
          { 0xf9614724, 0x5d2b, 0x41dc, { 0xae, 0xf7, 0x73, 0xd6, 0x2b, 0x51, 0x54, 0x3b } };
      ICoreWebView2Controller3* c3 = nullptr;
      if (SUCCEEDED(ch.ctrl->QueryInterface(kIID_HudController3, (void**)&c3)) && c3) {
          c3->put_ShouldDetectMonitorScaleChanges(FALSE);
          c3->put_RasterizationScale(1.0);
          c3->Release();
      } }

    if (FAILED(ch.ctrl->get_CoreWebView2(&ch.core)) || !ch.core) {
        composed_destroy(ch);
        return false;
    }
    // NO BROWSER CONTEXT MENU anywhere in the overlay. hud.html suppresses
    // `contextmenu` on its own document, but a DOCKED SNIPPET is a
    // cross-origin iframe, so that listener never sees its events and a right
    // click over a docked panel raised Chromium's Back/Refresh/Save as/Inspect
    // menu over the game. Killing it on the core covers every frame at once
    // and is the only place that can. Failure is ignored on purpose: an older
    // runtime just keeps the menu, which is what happened before anyway.
    { ICoreWebView2Settings* st = nullptr;
      if (SUCCEEDED(ch.core->get_Settings(&st)) && st) {
          st->put_AreDefaultContextMenusEnabled(FALSE);
          st->Release();
      } }
    { auto* mh = new HudMsgReceived(
          [pch](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
              LPWSTR s = nullptr;
              if (SUCCEEDED(a->TryGetWebMessageAsString(&s)) && s) {
                  if (pch->on_msg) pch->on_msg(narrow(s));
                  CoTaskMemFree(s);
              }
              return S_OK;
          });
      ch.core->add_WebMessageReceived(mh, &ch.tok_msg);
      mh->Release(); }
    { UINT32 bp = 0;
      if (SUCCEEDED(ch.core->get_BrowserProcessId(&bp))) ch.browser_pid = bp; }
    // Esc ends focus mode, handled HERE rather than in the page: the
    // controller sees the key stream whichever FRAME has DOM focus, so Esc
    // works from inside a docked snippet's iframe too, which a page-side
    // listener cannot (cross-origin). Doubles as a keyboard-path instrument
    // under ECLIPSE_ZLOG -- but note it fires only for ACCELERATOR keys
    // (Esc, F-keys, modified combos), NOT for plain letters (measured); a
    // silent trace under plain typing is normal.
    { auto* ak = new HudAccelKey(
          [](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* a) -> HRESULT {
              COREWEBVIEW2_KEY_EVENT_KIND kind{};
              UINT vk = 0;
              a->get_KeyEventKind(&kind);
              a->get_VirtualKey(&vk);
              zlog("ACCEL vk=%u kind=%d focus=%d", vk, (int)kind, (int)g_ov_focus);
              if (g_ov_focus && vk == VK_ESCAPE &&
                  (kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN ||
                   kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN)) {
                  a->put_Handled(TRUE);
                  // The focus-mode escape hatch, and the ONLY thing Esc
                  // closes in the game: a docked panel's focus. It never
                  // closed the console -- Insert and the badge do that.
                  overlay_focus_exit(true);
              }
              return S_OK;
          });
      ch.ctrl->add_AcceleratorKeyPressed(ak, &ch.tok_key);
      ak->Release(); }
    { auto* cc = new HudCursorChanged(
          [pch](ICoreWebView2CompositionController* s) -> HRESULT {
              HCURSOR c = nullptr;
              if (SUCCEEDED(s->get_Cursor(&c))) pch->cur = c;
              return S_OK;
          });
      ch.comp->add_CursorChanged(cc, &ch.tok_cur);
      cc->Release(); }

    if (init_script && !init_script->empty()) {
        // Must be IN FORCE before the first content loads, and the install is
        // async, so it is pumped to completion here rather than raced.
        bool sdone = false;
        auto* sh = new HudScriptDone([&](HRESULT, LPCWSTR) -> HRESULT {
            sdone = true; return S_OK; });
        if (SUCCEEDED(ch.core->AddScriptToExecuteOnDocumentCreated(init_script->c_str(), sh))) {
            DWORD sdl = GetTickCount() + 5000;
            while (!sdone && GetTickCount() < sdl) {
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                if (!sdone) MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
            }
        }
        sh->Release();
    }

    ch.comp->put_RootVisualTarget(ch.dcvis);
    ch.dcdev->Commit();
    return true;
}

// Injected into EVERY frame of the overlay (document-created scripts run in
// child frames too). Top frame: no-op. In an iframe -- a docked snippet page
// -- it recreates the page-side half of the snippet channel:
//   * window.eclipse_send relays through the PARENT page (SNIPMSG), because
//     chrome.webview.postMessage from a child frame raises per-frame events
//     nothing here subscribes to.
//   * the message listener is the receiving end of fx_window_eval (DOCKEVAL);
//     e.source===parent so a sibling frame could never feed it, and the eval
//     runs in the SNIPPET'S origin -- the console page only ever carries the
//     string.
//   * TEXT-FIELD FOCUS (2026-09-04). A docked panel is an element of a
//     WS_EX_NOACTIVATE window, so its page never gets keys -- unless the host
//     enters focus mode (overlay_focus_enter). The shim asks for it on the
//     page's behalf: __ec_focus:1 once a click has landed DOM focus on a
//     text-entry element, __ec_focus:0 once DOM focus has left one. The
//     overlay page relays them as HUD_FOCUS:1/0. No snippet-side call: the
//     field IS the intent, same as the hit rects, and the host never reads
//     it off a coordinate. Details that are load-bearing:
//       - mousedown + setTimeout(0), NOT focus/focusin, for the entry. Blink
//         moves document.activeElement on the click whether or not the page
//         is focused, but it dispatches the focus EVENT only while the page
//         is focused ("if page lost focus, event will be dispatched on page
//         focus") -- and an unfocused page is exactly the starting state.
//         The timeout runs after the mousedown default action (the focus
//         move), so activeElement is already the clicked field.
//       - focusin covers a Tab INTO a field while the mode is already on;
//         focusout + setTimeout(0) covers leaving it (activeElement is then
//         the destination: another field keeps the mode, anything else --
//         a button, the body after blur() -- ends it). A page-level focus
//         loss (Esc, a click on the game) dispatches blur only, never
//         focusout, and needs nothing from here: the host already left.
//       - the host dedupes (enter/exit are no-ops in the wrong state), so
//         a repeat is cheap and the page never has to track what the host
//         thinks; the host's state may have moved without the page's say-so.
//       - a page that calls field.focus() on load does NOT take the
//         keyboard: no mousedown, no message. Only a click does.
static const std::wstring kOverlayFrameShim =
    L"(function(){if(self===top)return;"
    L"window.eclipse_send=function(m){try{parent.postMessage({__ec_send:String(m)},'*');}catch(e){}};"
    L"window.addEventListener('message',function(e){"
    L"if(e.source===parent&&e.data&&typeof e.data.__ec_eval==='string'){"
    L"try{(0,eval)(e.data.__ec_eval);}catch(err){}}});"
    L"function te(el){if(!el||el===document.body)return false;var t=el.tagName;"
    L"if(t==='TEXTAREA'||el.isContentEditable)return true;if(t!=='INPUT')return false;"
    L"return !/^(button|checkbox|radio|submit|reset|file|image|color|range|hidden)$/.test(el.type);}"
    L"function fo(v){try{parent.postMessage({__ec_focus:v},'*');}catch(e){}}"
    L"function want(){if(te(document.activeElement))fo(1);}"
    L"function done(){if(!te(document.activeElement))fo(0);}"
    L"document.addEventListener('mousedown',function(){"
    L"try{parent.postMessage({__ec_click:1},'*');}catch(e){}setTimeout(want,0);},true);"
    L"document.addEventListener('focusin',want,true);"
    L"document.addEventListener('focusout',function(){setTimeout(done,0);},true);"
    L"})();";

// The docked snippet's origin, mapped onto the overlay's core. Re-run on
// every overlay create as well as on content change: a rebuilt overlay is a
// new CoreWebView2 and mappings do not survive it.
void dock_apply_mapping() {
    if (!g_hud_composed || !g_hudc.core ||
        dock_host().empty() || dock_dir().empty()) return;
    // MinGW's WebView2.h has no __CRT_UUID_DECL for this either.
    static const GUID kIID_WebView2_3 =
        { 0xa0d6df20, 0x3b92, 0x416d, { 0xaa, 0x0c, 0x43, 0x7a, 0x9c, 0x72, 0x78, 0x57 } };
    ICoreWebView2_3* c3 = nullptr;
    if (SUCCEEDED(g_hudc.core->QueryInterface(kIID_WebView2_3, (void**)&c3)) && c3) {
        // ALLOW rather than DENY_CORS: the isolation this design leans on is
        // same-origin policy (the iframe cannot reach the console DOM); the
        // access kind only governs resource fetches, and denying those breaks
        // legitimate snippet pages without adding a wall the snippet cares
        // about.
        c3->SetVirtualHostNameToFolderMapping(
            widen(dock_host()).c_str(), widen(dock_dir()).c_str(),
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        c3->Release();
    } else {
        ec::logf("! dock: runtime too old for virtual-host mapping\n");
    }
}

void dock_clear_mapping() {
    if (!g_hud_composed || !g_hudc.core || dock_host().empty()) return;
    static const GUID kIID_WebView2_3 =
        { 0xa0d6df20, 0x3b92, 0x416d, { 0xaa, 0x0c, 0x43, 0x7a, 0x9c, 0x72, 0x78, 0x57 } };
    ICoreWebView2_3* c3 = nullptr;
    if (SUCCEEDED(g_hudc.core->QueryInterface(kIID_WebView2_3, (void**)&c3)) && c3) {
        c3->ClearVirtualHostNameToFolderMapping(widen(dock_host()).c_str());
        c3->Release();
    }
}

static bool hud_create_composed() {
    // Sized to the attached client's window rect: the console panel and any
    // docked snippet panel are ELEMENTS of this one window, positioned by the
    // page. The rect can be briefly unknown right after attach; a placeholder
    // is fine -- the tick corrects it before the page has anything animating,
    // which is the only time a resize costs anything.
    int w = 800, h = 600;
    {
        HWND game = hud_client_cached(ec::state().pid);
        RECT g{};
        if (game && GetWindowRect(game, &g)) {
            w = g.right - g.left;
            h = g.bottom - g.top;
        }
    }
    if (!composed_create(g_hudc, L"Eclipse HUD", w, h,
                         hud_on_msg, &kOverlayFrameShim))
        return false;
    g_hud_hwnd     = g_hudc.hwnd;
    g_hud_composed = true;

    // EMPTY region until the page reports its rects: this window covers the
    // whole client, and with no region it would swallow every click over the
    // game. Empty, not null.
    SetWindowRgn(g_hud_hwnd, CreateRectRgn(0, 0, 0, 0), TRUE);
    g_ov_rects.clear();
    g_ov_region.clear();
    g_ov_soft = false;

    // A rebuilt overlay is a new core; the docked origin has to be re-mapped
    // before the page replays DOCKP and points its iframe at it.
    dock_apply_mapping();

    // ?c=1 tells the page it owns its shape (transparent background, CSS
    // corners, the fold animation). hud= is the console panel's visibility --
    // the window can exist purely for a docked panel. om=1 is overlay mode,
    // which also means START COLLAPSED (user decision 2026-08-22: the badge
    // alone comes up; the user opens the panel or the app deliberately).
    // Query flags are deterministic where an injected script would race the
    // document's own; runtime flips arrive as HUDVIS / OVERLAYMODE.
    g_hudc.core->Navigate(widen("file:///" + ec::self_dir() + "/web/hud.html?c=1&hud="
                                + (g_hud_enabled ? "1" : "0")
                                + (g_overlay_pref ? "&om=1" : "")).c_str());
    // Nothing is pushed here; the page announces itself with HUD_READY, for
    // the same reason as the fallback path below.
    return true;
}

// Composition hosting refused once this session -- do not re-pump a 15s
// creation attempt per tick for a dock on a machine that cannot host it.
static bool        g_overlay_refused = false;

// The overlay window on demand, for a dock arriving while the console panel
// is off (or before the tick has bothered). Composition hosting or nothing:
// the wrapper fallback has no per-pixel alpha and cannot host an iframe panel
// over the game, and a refusal is a machine property, so it is not re-pumped
// per tick.
bool ensure_overlay() {
    if (g_hud_composed) return true;
    if (g_hud_wv) return false;          // wrapper fallback is up: no overlay
    if (g_overlay_refused || g_hud_creating) return false;
    g_hud_creating = true;
    bool ok = hud_create_composed();
    if (ok) hud_install_hooks();
    else    g_overlay_refused = true;
    g_hud_creating = false;
    return ok;
}

void hud_destroy() {
    // A new overlay window starts the band fight from scratch. Suppression
    // clears too: with no overlay the ordinary lifecycle (top of hud_tick)
    // already decides what is shown.
    g_hud_band_lost = false; g_hud_band_fails = 0; g_hud_band_told = false;
    g_hud_suppressed = false;
    // If the overlay dies while app mode is up or mid-choreography (detach,
    // client exit), there is no page left to animate -- put the desktop
    // identity and placement back directly, or the console vanishes with no
    // window at all. Before overlay_focus_exit, which needs no game here.
    app_abort_restore();
    // The hotkey is registered ON this window; the OS does not take it back
    // when the window dies quietly enough to rely on.
    app_hotkey_unregister();
    // A dying window must not keep the keyboard: drop the focus-mode flag and
    // style first. No hand-back -- on detach there may be no game left.
    overlay_focus_exit(false);
    // NOT the hooks -- a docked snippet window may still need them. They are
    // released by overlay_tick when nothing is left that wants them.

    // Any cursor counts we added have to come back off, or the game is left
    // with a cursor it never asked for. See the tick.
    while (g_hud_cursor_added-- > 0) ShowCursor(FALSE);
    g_hud_cursor_added = 0;
    g_hud_was_over = false;

    // Break ownership before the window goes: an owned window is destroyed with
    // its owner, and leaving a cross-process owner set while we tear our own
    // window down invites the two processes to disagree about who destroys what.
    if (g_hud_owner && g_hud_hwnd && IsWindow(g_hud_hwnd))
        SetWindowLongPtrW(g_hud_hwnd, GWLP_HWNDPARENT, 0);
    g_hud_owner = nullptr;

    if (g_hud_composed) {
        hud_teardown_composed();
        g_ov_rects.clear();     // page-reported; the next page reports afresh
        g_ov_region.clear();
        g_ov_soft = false;
        g_hud_lastx = g_hud_lasty = g_hud_lastw = g_hud_lasth = -1;
        g_hud_shown = false;
        return;
    }

    if (!g_hud_wv) return;
    auto res = g_hud_wv->window();
    HWND h = res.ok() ? (HWND)res.value() : nullptr;
    // DestroyWindow rather than terminate(): terminate PostQuitMessages the
    // whole app, same as for the snippet window.
    if (h && IsWindow(h)) DestroyWindow(h);
    g_hud_wv.reset();
    g_hud_hwnd = nullptr;
    g_hud_lastx = g_hud_lasty = g_hud_lastw = g_hud_lasth = -1;
    g_hud_shown = false;
}

static void hud_create() {
    if (g_hud_wv || g_hud_composed || g_hud_creating) return;
    if (!overlay_wanted()) return;
    g_hud_creating = true;

    // Composition hosting first: the only mode whose badge corners are
    // anti-aliased, whose open/close can animate as a page-drawn fold, and
    // which can host a docked snippet panel at all. The wrapper+region path
    // below stays as the fallback for a machine where any piece of it
    // refuses -- console panel only.
    if (!g_overlay_refused && hud_create_composed()) {
        hud_install_hooks();
        g_hud_creating = false;
        return;
    }
    g_overlay_refused = true;
    if (!g_hud_enabled) {           // wanted only for a dock: no fallback fits
        g_hud_creating = false;
        return;
    }

    try {
        g_hud_wv.reset(new webview::webview(false, nullptr));
    } catch (...) {
        g_hud_wv.reset();
        g_hud_enabled = false;      // one failure is enough; do not retry per tick
        g_hud_creating = false;
        ec::logf("! in-game panel unavailable - WebView2 would not start\n");
        return;
    }

    g_hud_wv->set_title("Eclipse HUD");
    g_hud_wv->set_size(kHudPanel, kHudHeight, WEBVIEW_HINT_NONE);

    g_hud_wv->bind("hud_send", [](const std::string& req) -> std::string {
        hud_on_msg(extract_first_json_arg(req));   // shared with the composed path
        return "\"\"";
    });

    auto res = g_hud_wv->window();
    g_hud_hwnd = res.ok() ? (HWND)res.value() : nullptr;
    if (g_hud_hwnd) {
        // Before the first show. See the note at the top of this section.
        LONG_PTR ex = GetWindowLongPtrW(g_hud_hwnd, GWL_EXSTYLE);
        ex |= WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
        ex &= ~WS_EX_APPWINDOW;                   // keep it out of Alt-Tab
        SetWindowLongPtrW(g_hud_hwnd, GWL_EXSTYLE, ex);

        LONG_PTR st = GetWindowLongPtrW(g_hud_hwnd, GWL_STYLE);
        st &= ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
        st |= WS_POPUP;
        SetWindowLongPtrW(g_hud_hwnd, GWL_STYLE, st);

        SetWindowPos(g_hud_hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);

        // Rounded by the COMPOSITOR, not by CSS. A border-radius in the page
        // leaves the window's own corners square, and WebView2 paints its
        // default white wherever the page is transparent -- which is exactly
        // the white triangles at each corner. DWM clips the window itself, so
        // there is nothing left to show through. Same attributes the main
        // window uses; both are no-ops before Windows 11.
        // NO SHADOW, NO FRAME, NO DWM ROUNDING on this window.
        //
        // All three are drawn from the window RECT, and the window is
        // panel-sized even when only the badge shows -- a region clips the
        // content but none of these, so each one leaves a panel-sized ghost
        // hanging over the game while collapsed.
        //
        // The shadow comes from the window CLASS (CS_DROPSHADOW), which is
        // webview's, not ours -- DWMWA_NCRENDERING_POLICY does not remove it,
        // which is why it survived the first attempt. Clearing the class bit
        // does. The page draws its own borders and corners, so nothing is lost.
        SetClassLongPtrW(g_hud_hwnd, GCL_STYLE,
                         GetClassLongPtrW(g_hud_hwnd, GCL_STYLE) & ~CS_DROPSHADOW);
        enum { NCRENDERING_POLICY = 2, CORNER_PREFERENCE = 33, BORDER_COLOR = 34 };
        int policy = 2;                          // DWMNCRP_DISABLED
        DwmSetWindowAttribute(g_hud_hwnd, NCRENDERING_POLICY, &policy, sizeof policy);
        int corner = 1;                          // DWMWCP_DONOTROUND
        DwmSetWindowAttribute(g_hud_hwnd, CORNER_PREFERENCE, &corner, sizeof corner);
        COLORREF edge = 0xFFFFFFFE;              // DWMWA_COLOR_NONE
        DwmSetWindowAttribute(g_hud_hwnd, BORDER_COLOR, &edge, sizeof edge);
    }

    hud_install_hooks();

    // WebView2's default background is WHITE, and it shows wherever the surface
    // has not been painted yet -- which is precisely the strip exposed when the
    // panel widens, for the frame or two before the page lays out. Painting
    // that gap in the page's own background makes the expand read as movement
    // instead of as a flash.
    //
    // The IID is spelled out for the same reason as ICoreWebView2Settings9
    // above: MinGW only knows an interface's uuid when the header declared it
    // with __CRT_UUID_DECL, which WebView2.h does not do for these.
    if (auto ctrl = g_hud_wv->browser_controller(); ctrl.ok() && ctrl.value()) {
        static const GUID kController2 =
            { 0xc979903e, 0xd4ca, 0x4228, { 0x92, 0xeb, 0x47, 0xee, 0x3f, 0xa9, 0x6e, 0xab } };
        auto* c = static_cast<ICoreWebView2Controller*>(ctrl.value());
        ICoreWebView2Controller2* c2 = nullptr;
        if (SUCCEEDED(c->QueryInterface(kController2, (void**)&c2)) && c2) {
            COREWEBVIEW2_COLOR bg{ 255, 0x0d, 0x0d, 0x10 };   // --bg-1, opaque
            c2->put_DefaultBackgroundColor(bg);
            c2->Release();
        }
    }

    // From the first frame: a fresh window with no region is panel-shaped, and
    // would flash the whole panel before the page has drawn anything.
    hud_apply_region(g_hud_expanded);

    g_hud_wv->navigate("file:///" + ec::self_dir() + "/web/hud.html");
    // Nothing is pushed here. navigate() is asynchronous, so anything sent now
    // arrives before __onHudMsg exists and is simply lost -- which showed as a
    // panel stuck on "detached" with an empty snippet list. Worse once
    // unchanged snapshots started being dropped: the lost push was recorded as
    // already-sent, so the state that would have corrected it never went out
    // again. The page announces itself instead; see HUD_READY.
    g_hud_creating = false;
}

// Called from WM_TIMER on the UI thread, which is where every window call here
// has to run anyway.
void hud_tick() {
    if (g_hud_creating) return;

    const ec::State& s = ec::state();

    // App-mode choreography watchdogs: a page that never answers (overlay
    // rebuilt mid-flight, a message lost) must not strand the console hidden
    // behind a box that is not there. 1.5s is far beyond the 200ms travel
    // plus a compose; firing early only skips animation, never state.
    if (g_app_opening && GetTickCount() - g_app_phase_at > 1500) {
        zlog("APP open watchdog fired");
        app_open_finish();      // routes to app_open_uncover if the show already ran
    }
    if (g_app_closing && GetTickCount() - g_app_phase_at > 1500) {
        zlog("APP close watchdog fired");
        app_close_finish();
    }

    // The travel's pointer-following click-through (see ct_follow_apply), and
    // its own belt: g_travel_ct outlives the phase flags by design (the close
    // travel is visible after app_close_finish), so a page that never sends
    // APPIDLE would leave the rule running forever. 3s is 1.5s of watchdog
    // plus a full travel with room to spare.
    if (g_travel_ct) {
        ct_follow_apply();
        if (GetTickCount() - g_app_phase_at > 3000) {
            zlog("APP travel-ct belt fired");
            g_travel_ct = false;
            overlay_clickthrough(false);
        }
    } else if (g_ov_soft) {
        // A soft rect with no travel running: a passthrough docked panel. Same
        // pointer rule, no belt -- this one ends when the page stops reporting
        // the rect, which hud_set_rects handles.
        ct_follow_apply();
    }

    // OVERLAY MODE's lifecycle, both directions, in the one place that runs
    // whatever else happens. Detaching is not a mode change: with no client
    // there is nothing to sit over, so the desktop window comes back and the
    // preference simply waits. That is also the safety valve -- if the
    // overlay never comes up, the console is still reachable.
    // The desktop-window show/hide valves below own g_hwnd's visibility in
    // overlay mode -- EXCEPT while app mode is up or mid-hand-off, where
    // g_hwnd IS the app window shown over the game. Letting the hide valve
    // run then hides the app every tick (the game becomes foreground merely
    // because it is next in the stack -- it looks like Roblox reasserting,
    // measured, it is us). One gate for all three.
    bool app_owns_window = g_app_mode || g_app_opening || g_app_closing;

    // Gated on the first attach probe having COMPLETED: before it, "no pid"
    // only means "we have not looked yet", and acting on that is the
    // desktop-window flash at startup (shown at the first 16ms tick, hidden
    // again once the overlay came up). reveal_window carries the same gate.
    // ...and on the no-client state having HELD for a moment. "One probe has
    // finished and found nothing" is still not enough: at startup the first
    // probe completes before the attach it kicked off has landed, so there is
    // a ~250ms window where probed is true and pid is still 0 -- and showing
    // the window there is the startup flash, measured at 17 visible samples
    // across a 9s launch after the reveal_window gate alone was fixed. A
    // client that genuinely is not running stays not-running, so waiting
    // costs nothing; a client mid-attach resolves well inside this.
    static DWORD noclient_since = 0;
    if (s.pid) noclient_since = 0;
    else if (!noclient_since) noclient_since = GetTickCount();
    bool really_no_client = noclient_since &&
                            (GetTickCount() - noclient_since) > 1500;

    if (!app_owns_window && g_overlay_pref && !s.pid && attach_probed() &&
        really_no_client && ui_main_hwnd() && !IsWindowVisible(ui_main_hwnd()))
        ShowWindow(ui_main_hwnd(), SW_SHOWNOACTIVATE);

    // The band-lost safety valve (set by the heal below): neither the topmost
    // band nor the above-game slot held while the client was focused, so the
    // overlay may simply never be seen. In overlay mode that must not strand
    // the user with no UI at all -- the desktop window comes back, and stays
    // back until a heal actually SUCCEEDS (not merely until the game loses
    // focus: clearing it then would hide the window mid-alt-tab, exactly
    // while the user is trying to reach it).
    if (!app_owns_window && g_overlay_pref && g_hud_suppressed &&
        ui_main_hwnd() && !IsWindowVisible(ui_main_hwnd()))
        ShowWindow(ui_main_hwnd(), SW_SHOWNOACTIVATE);

    // A docked snippet panel keeps the window alive with the console panel
    // switched off; only "nothing wants it" tears it down.
    if (!s.pid || !overlay_wanted()) { hud_destroy(); return; }
    if (!g_hud_wv && !g_hud_composed) { hud_create(); return; }
    if (!g_hud_hwnd) return;

    // The desktop window goes away only once the overlay window actually
    // EXISTS (the check above guarantees it here). Hiding it on the strength
    // of the preference alone would leave a machine that cannot host the
    // overlay with no UI at all and no way to say so.
    if (!app_owns_window && g_overlay_pref && !g_hud_suppressed &&
        ui_main_hwnd() && IsWindowVisible(ui_main_hwnd()))
        ShowWindow(ui_main_hwnd(), SW_HIDE);

    HWND game = hud_client_cached(s.pid);
    // The desktop-safety tripwire: a client left TOPMOST by anything (see the
    // summon insert) is stripped within a tick, not left until a restart.
    client_topmost_guard(game, "tick");
    if (!game || IsIconic(game)) {
        if (g_hud_shown) { ShowWindow(g_hud_hwnd, SW_HIDE); g_hud_shown = false; }
        // Hand back any cursor top-ups too: the hidden panel cannot be
        // hovered, and holding the count while the game sits minimised
        // inflates this thread's ShowCursor baseline for as long as it stays
        // there -- anything else on the thread that hides the cursor and
        // expects it hidden is silently overruled until then.
        if (g_hud_cursor_added > 0) {
            while (g_hud_cursor_added-- > 0) ShowCursor(FALSE);
            g_hud_cursor_added = 0;
        }
        return;
    }

    RECT g{};
    // The cache re-checks IsWindow, but the client can die between that check
    // and here (crash, rejoin). Zeros would place the panel at the desktop's
    // top-left for up to a second until the cache notices; skipping the tick
    // leaves it where it was, which is right for a window about to vanish.
    if (!GetWindowRect(game, &g)) return;

    int x, y, w, h;
    if (g_hud_composed) {
        // The overlay: the client's whole rect. The panels inside are the
        // page's to place, the hit region is the page's to report; this tick
        // only glues the window to the client and keeps its band. The size
        // changes exactly when the game window's does -- never for a panel
        // animation, which all happens inside a constant-size page.
        x = g.left; y = g.top;
        w = g.right - g.left;
        h = g.bottom - g.top;
    } else {
        // Wrapper fallback: the old panel-sized window, right edge 12px
        // inside the game's so the badge ends up where the panel's tab was.
        int cy = g.top + (g.bottom - g.top) / 2;
        w = kHudPanel;
        h = kHudHeight;
        y = cy - h / 2;
        x = g.right - w - 12;
    }

    // NOTHING IS ANIMATED HERE any more, and that is the point.
    //
    // Interpolating the window rect meant a resize per frame, and every resize
    // relayouts WebView2 a frame or two behind -- which showed as the panel's
    // right and bottom edges trailing the window with a gap behind them, and as
    // the badge drifting vertically while the height changed.
    //
    // Both states share the window's RIGHT EDGE, so the badge sits on the same
    // screen rectangle either way. The window therefore changes size exactly
    // once per toggle, at a moment when what you are looking at is already
    // where it will end up: growing before the panel fades in, shrinking after
    // it has faded out. The motion is the page's cross-fade, which the
    // compositor runs properly.

    // Plain window behaviour: never hidden, just placed in the stack. Topmost
    // while the client is focused, and otherwise dropped out of that band and
    // slotted directly above the game, so whatever you tabbed to covers it and
    // the game does not.
    //
    // This USED TO blink when tabbing back to the game, and the blink could
    // not be removed from this design: Windows raises the game on activation
    // and the foreground event that tells us arrives afterwards. MEASURED (QPC
    // poll of the z-order relation over scripted alt-tabs): the panel is back
    // on top 0.5-1.4ms after the raise, so the blink is one composed frame and
    // only when DWM's compose lands inside that window -- most alt-tabs show
    // nothing at all. Hiding instead of re-ordering was tried and removes the
    // blink completely -- but then the panel vanishes whenever a window sits
    // over it, which is worse to use. Chosen knowingly; do not "fix" the
    // flicker by reintroducing the hide.
    //
    // The APP window removed its (far more visible) version of this blink by
    // taking the client as its OWNER, and since 2026-08-22 THIS window has
    // the same owner too -- second attempt; the first broke clicking another
    // app into focus and hud_swp's bracket is what fixed that. The text above
    // stands as the record of why the reactive paths could never win alone.
    //
    // Also considered and REJECTED: staying permanently topmost and clipping
    // the panel's window region against every window that sits above the game,
    // so it only DRAWS where it should. Removes the blink structurally, but
    // the clip is recomputed on our tick, so every move of an overlapping
    // window leaves a stale clip for a frame, and the "windows above the game"
    // enumeration has to get cloaked UWP windows, toasts, menus and the
    // taskbar right on every monitor -- each miss draws the panel over an app
    // persistently, which is far worse than a rare one-frame blink.
    //
    // THE OTHER TAB-IN BLINK IS NOT OURS AND IS NOT FIXABLE HERE. Alt-tab back
    // to the game also makes this window and a docked snippet window vanish
    // for a frame or two while WindowFromPoint still returns us and the
    // z-order is intact -- which looked like a dropped composed frame.
    // Measured (Aug 2026, pixel sampling + a ring-buffer screen capturer; the
    // caught frame is build/cap_f1_16ms_fg1.png): the deviating frame IS
    // Windows' alt-tab zoom animation -- the activated window's PREVIEW
    // scaling up to fullscreen, drawn by system UI above every topmost
    // window. The preview is built from the game's own surface, so every
    // overlay is absent from it by construction and pops back the frame the
    // animation lands. Three facts close the case: the deviations sample
    // 10-16ms after alt-up while the game only BECOMES foreground 43-178ms
    // after it, so none of the SetWindowPos paths here had even run; a
    // WebView2-free plain-DComp window driven through this exact
    // promote/demote dance never deviated in 45 tagged cycles; and the frame
    // capture shows the zoom preview mid-flight with the neighbouring frames
    // clean.
    //
    // "But only the DOCK drops" was measured (panel expanded, both windows in
    // the same alt-tab cycles: dock 5/14, panel 0/14) and is an INSTRUMENT
    // artefact, not a dock defect. The event is exactly one composed frame,
    // starting about one compose after alt-up; a sampler that grabs the two
    // patches SEQUENTIALLY gives each window a fixed ~8ms slot in its round,
    // and the dock's slot lands inside that frame while the panel's straddles
    // its edges -- every cycle. Re-measured with both patches in ONE readback
    // (a single GetDIBits of a strip): the deviating compose hits BOTH
    // windows at 576/576 of 576 pixels in the same sample. Two more traps for
    // anyone re-measuring: WindowFromPoint returns our window while the
    // animation is on glass (transition UI is not hit-testable), so "nothing
    // covered it" proves nothing; and continuous heavy screen readback
    // (620x600 at 130Hz) suppresses the phenomenon itself (1/75 cycles vs
    // ~20% with light sampling) -- the instrument perturbs DWM's animation
    // timing. The dock LOOKS worse to the eye because it is a dark card over
    // pale sky at a corner; the same absence of a collapsed badge is nearly
    // invisible. Windows' accessibility "animation effects" toggle
    // removes the animation system-wide, which is the user's call, not ours.
    // Do not re-chase this through z-order, dcomp rebinding or WebView2
    // visibility -- and do not conflate it with the separate, sub-2ms
    // raise-race blink documented above.
    // OWNERSHIP, second attempt, SHIPPED 2026-08-22. Windows keeps an owned
    // window above its owner AS PART OF the owner's activation, so with the
    // client as owner there is no interval for the tab-in blink to live in --
    // the rule the band code spent three attempts faking. The first attempt
    // was reverted the same day because clicking another app left the game
    // covering it; the mechanism was pinned by mimic probe afterwards and it
    // is NOT ownership itself: an owned window EXITING the topmost band
    // (NOTOPMOST, or a relative insert into the normal band) hoists the whole
    // owner group to the top of the normal band, above the app the user just
    // clicked -- and the demote path does exactly that on every switch to a
    // third app. TOPMOST entries, in-band moves and the owner-set itself
    // never hoist (all probed both ways, 2026-08-22). So the owner stays ON
    // and every non-TOPMOST move goes through hud_swp's bracket, which drops
    // the owner around the call. The band rule below is UNCHANGED and still
    // owns what ownership does not answer: whether we float above OTHER apps.
    //
    // The first attempt's other cost -- shared input queue: cursor bleed and
    // hitches -- is already paid: the cursor top-up below handles the bleed
    // (it ships and is measured), and the APP window has had the same owner
    // since the same day with no hitch reports.
    //
    // AND THE OWNER IS HELD ONLY WHILE DEMOTED -- set and cleared at the END
    // of this tick, once `fg` is known (search "OWNER ONLY WHILE DEMOTED").
    // Two reasons, both measured 2026-08-22:
    //   * the blink lives entirely in the transition FROM "third app
    //     foreground" INTO the game's activation, and the ride only needs
    //     the owner to exist when that activation starts -- i.e. while the
    //     overlay sits in the normal band directly above the game;
    //   * holding the owner while the GAME is foreground hides the cursor
    //     over the badge (CURSOR_SHOWING=0, handle=0 sampled over the badge
    //     with the game focused; the unowned baseline and the owned
    //     third-app-foreground state both sample visible) -- the shared
    //     input queue bleed the first attempt was turned off for, and the
    //     per-tick cursor top-up below does NOT win against it.
    // Dropping the owner is not a z-order operation, so the overlay stays
    // exactly where the promote put it.

    // Cursor visibility is per input queue, and ownership ATTACHED ours to the
    // client's -- so while the game hides the cursor for mouse-lock, ours is
    // hidden too, including over this panel. You cannot click what you cannot
    // point at.
    //
    // ShowCursor is a COUNTER, not a flag. Every TRUE we add has to be matched
    // by a FALSE on the way out or the game is left with a cursor it did not
    // ask for, so the additions are counted rather than assumed to be one.
    {
        POINT c{};
        bool over = false;
        if (GetCursorPos(&c)) {
            if (g_hud_composed) {
                // Over an INTERACTIVE piece, not over the client-sized
                // window: topping the cursor up across the whole game would
                // fight mouse-lock everywhere. The page's rects are window
                // coords; the cursor is screen coords.
                for (const RECT& pr : g_ov_rects) {
                    RECT sr{ x + pr.left, y + pr.top, x + pr.right, y + pr.bottom };
                    if (PtInRect(&sr, c)) { over = true; break; }
                }
                // The APP WINDOW counts as ours too. It is a real HWND, so it
                // is not in the page's rects -- and without this, opening the
                // app from the badge dropped every ShowCursor(TRUE) we had
                // added the moment the pointer stopped being over the badge,
                // and the cursor blinked out for a frame before Windows put it
                // back over the app. Same release rule as before, just with
                // the full set of our surfaces.
                if (!over && g_app_mode && ui_main_hwnd() && IsWindowVisible(ui_main_hwnd())) {
                    RECT ar{};
                    if (GetWindowRect(ui_main_hwnd(), &ar) && PtInRect(&ar, c)) over = true;
                }
            } else {
                RECT r{ x, y, x + w, y + h };
                over = PtInRect(&r, c) != FALSE;
            }
        }
        if (over) {
            // Topped up EVERY tick, not once on entry. The game re-hides the
            // cursor on its own frames while mouse-lock is on, so a single
            // correction is undone almost immediately -- which is exactly what
            // "it still hides sometimes" was.
            //
            // ShowCursor(TRUE) returns the count AFTER incrementing. Above zero
            // means it was already visible and this call was surplus, so it is
            // handed straight back; that keeps our tally to only the additions
            // that were actually needed.
            int guard = 0;
            while (guard++ < 8) {
                int now = ShowCursor(TRUE);
                if (now > 0) { ShowCursor(FALSE); break; }
                g_hud_cursor_added++;
                if (now == 0) break;              // visible; nothing more to add
            }
        } else if (!over && g_hud_cursor_added) {
            while (g_hud_cursor_added-- > 0) ShowCursor(FALSE);
            g_hud_cursor_added = 0;
        }

        // The hand-back itself now lives in overlay_handback, called from
        // overlay_tick: it serves a docked snippet window too, and must keep
        // working when the panel is switched off in Settings -- which tears
        // this tick's work down entirely. Only the cursor bookkeeping above
        // is the panel's own.

        g_hud_was_over = over;
    }

    // (owner maintenance is at the end of the tick -- it needs `fg`.)

    // The controller's bounds track the window: the overlay resizes when the
    // game window does (and only then -- panel motion is all in-page). A
    // resize shows the old surface stretched for a beat; unavoidable when the
    // user is dragging the game's frame, and invisible when nothing inside is
    // moving.
    bool resized = (w != g_hud_lastw || h != g_hud_lasth);
    if (g_hud_composed && resized && g_hudc.ctrl) {
        RECT b{ 0, 0, w, h };
        g_hudc.ctrl->put_Bounds(b);
    }

    // APP MODE: the real window follows the client the way the overlay does
    // -- re-glued by anchor when the client's rect changes. Never while the
    // user is mid-drag (the modal loop owns it; the anchor folds in at
    // WM_EXITSIZEMOVE) and never mid-choreography (APP_ATRECT places it).
    if (g_hud_composed && g_app_mode && !g_app_opening && !g_app_closing &&
        !ui_in_sizemove() && ui_main_hwnd() && IsWindowVisible(ui_main_hwnd()) &&
        (x != g_hud_lastx || y != g_hud_lasty || resized)) {
        int l, t, aw, ah;
        app_target_rect(w, h, &l, &t, &aw, &ah);
        SetWindowPos(ui_main_hwnd(), nullptr, x + l, y + t, aw, ah,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        app_placed(x + l, y + t, aw, ah);
    }
    // NO early-return while owned (the first ownership attempt had one:
    // "position only, ownership does the rest"). The band rule must keep
    // running -- it is what still decides whether the overlay floats above
    // OTHER apps, and skipping it also skipped the app window's owner and
    // band maintenance below.

    DWORD fgpid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &fgpid);
    // buried: the pid-foreground proxy failed its corroboration -- the
    // client's window is not what is visually on top at its own rect (see
    // hud_client_covered for the never-focused-launch state this catches).
    // A NARROWING of fg, unlike the two rejected widenings recorded below:
    // it can only ever demote, so it cannot reintroduce the panel floating
    // over apps it has no business being above. On a real tab-in the raise
    // happens as part of the activation, BEFORE our tick sees the foreground
    // change (measured in the app-band comment: our notice trails the raise
    // by 0.5-1.4ms), so by the first tick with fgpid == s.pid the centre
    // probe already reads the client and this gate does not delay the
    // promote.
    bool buried = hud_client_covered(game, s.pid);
    bool fg = (fgpid == s.pid) && !buried;
    // An activation of the client was seen coming a moment ago -- a click on
    // it, a corroborated foreground event, or our own hand-over -- and has
    // not landed yet. Holding the pre-emption across that gap is the whole
    // point of it. Every stamp is now a caller's explicit claim (see the
    // comment above hud_swp); this deliberately bypasses the covered gate
    // above, because the click stamp is made PRE-raise when the client
    // legitimately still reads covered.
    //
    // TRIED AND WRONG: skipping this bridge while foreground is NOBODY
    // (fgpid == 0). It changed nothing -- the cause was the owner/owned
    // activation redirection, see overlay_owner_drop.
    if (!fg && g_hud_preempt_at && GetTickCount() - g_hud_preempt_at < 400) fg = true;


    // Focus mode: WE are foreground, which this test reads as "game not
    // focused" and would demote the overlay out of the topmost band mid-type.
    // Treat it as focused. Safe, because the mode exits the moment anything
    // that is not the overlay takes foreground (hud_on_foreground).
    if (g_ov_focus) fg = true;
    // App mode with the CONSOLE foreground counts as focused the same way.
    // The band itself is the under-app glue's while the app is steady (it
    // takes the app's band), so what `fg` still decides here is the OWNER
    // (only while demoted -- end of the tick) and the mid-leg band, where the
    // entry gate does not bite (the foreground process is exempt, measured,
    // overlay-plan.md). Scoped to OUR process being foreground, not to app
    // mode as such: the app now stays OPEN across a switch to a third app
    // (the close rule closes only on a click into the client), and forcing
    // fg then would float the badge and the app over the window the user
    // just switched to -- the exact class of wrong the two rejected
    // widenings above were removed for.
    // WEBVIEW2'S BROWSER PID COUNTS AS OURS, like every other "is the
    // foreground ours" test in this file (see hud_on_foreground and the three
    // in the tick). Clicking a DOCKED SNIPPET PANEL parks Win32 focus on that
    // browser process, so without it a dock click in app mode read as "not
    // ours": the overlay demoted out of the band and the next foreground event
    // promoted it back. Measured with the app open and the dock clicked -- 643
    // samples over 10s, 2 order changes, a 16.4ms window with the app over the
    // overlay. That inversion is the flap. With the browser pid included, the
    // same probe over the same 643 samples: 0 order changes, worst hold 0.0ms.
    if (g_app_mode &&
        (fgpid == GetCurrentProcessId() ||
         (g_hudc.browser_pid && fgpid == g_hudc.browser_pid))) fg = true;

    // BAND-DIAG: which override decided fg, and did the result actually put
    // us over the window the user is looking at. Kept (under the zlog gate)
    // because "which band decision fired and what did it cover" recurs in
    // this file, and no capture or style-bit read can answer it after the
    // fact.
    //
    // It settled the never-focused-launch hunt (2026-08-28). The suspects it
    // ELIMINATED, so they are not re-investigated: the three fg overrides
    // (app foreground, pre-emption, focus mode -- all 0 in every bug line),
    // a multi-instance mix-up (one client, pid-filtered pick), and
    // hud_pick_window choosing the wrong window of the client (fgz == gamez
    // in every bug line means the foreground window IS the picked window;
    // a live enumeration of the staged state showed the client owning
    // exactly one visible unowned top-level window). What remained was the
    // proxy failure hud_client_covered documents, and `covered` here is that
    // gate's own reading -- a bug line under the fix reads raw=1 covered=1
    // fg=0.
    //
    // Edge-triggered on the whole tuple: steady state costs one comparison,
    // and the two z_rank walks only happen with ECLIPSE_ZLOG=1.
    if (zlog_on()) {
        DWORD upid = 0; int uz = -1;
        z_under(g_hud_hwnd, &upid, &uz);
        int zo = z_rank(g_hud_hwnd);
        // THE SYMPTOM ITSELF: we are covering a window that is neither the
        // client we are attached to nor our own. A second Roblox pid here is
        // the panel on the wrong game.
        int wrong = (upid && upid != s.pid && upid != GetCurrentProcessId()) ? 1 : 0;
        int raw   = (fgpid == s.pid) ? 1 : 0;
        int pre   = (g_hud_preempt_at && GetTickCount() - g_hud_preempt_at < 400) ? 1 : 0;
        static int  last = -1;
        static DWORD lastu = 0;
        int key = (fg?1:0) | (raw?2:0) | (pre?4:0) | (g_ov_focus?8:0) |
                  (g_app_mode?16:0) | (wrong?32:0);
        // covered= is THE VALUE THE GATE USED (`buried`, sampled once at the
        // top of the tick), never a re-sample: an earlier version called the
        // probe again here and logged covered=0 beside an fg the gate had
        // already demoted for covered=1 -- an instrument that can contradict
        // the decision it reports. wfp= is still a fresh sample, taken only
        // to NAME the covering pid; it can lawfully disagree with covered=.
        DWORD wpid = 0; HWND wroot = nullptr;
        RECT  gr{};
        if (game && GetWindowRect(game, &gr)) {
            POINT cpt{ (gr.left + gr.right) / 2, (gr.top + gr.bottom) / 2 };
            wroot = WindowFromPoint(cpt);
            if (wroot) {
                wroot = GetAncestor(wroot, GA_ROOT);
                GetWindowThreadProcessId(wroot, &wpid);
            }
        }
        int covered = buried ? 1 : 0;
        key |= covered ? 64 : 0;   // a covered-state flip must log too
        if (key != last || upid != lastu) {
            last = key; lastu = upid;
            // fgsame: is the window Windows calls foreground the SAME window
            // the panel is glued to. It read 1 in every observed bug line --
            // the foreground-but-buried state is one window, not a wrong
            // pick.
            HWND fgw = GetForegroundWindow();
            zlog("BAND-DIAG fg=%d raw=%d pre=%d focusmode=%d app=%d COVERING=%u"
                 "%s uz=%d ovz=%d gamez=%d fgz=%d fgsame=%d covered=%d wfp=%u"
                 " game=%p fgw=%p fgpid=%u attached=%u ours=%u",
                 (int)fg, raw, pre, (int)g_ov_focus, (int)g_app_mode,
                 (unsigned)upid, wrong ? " <== WRONG-WINDOW" : "",
                 uz, zo, z_rank(game), z_rank(fgw), (int)(fgw == game),
                 covered, (unsigned)wpid, (void*)game, (void*)fgw,
                 (unsigned)fgpid, (unsigned)s.pid,
                 (unsigned)GetCurrentProcessId());
        }
    }

    // Focus, and ONLY focus, decides the band. Two ways of widening that were
    // tried against the tab-in blink and both were removed, because each traded
    // the blink for the panel appearing above apps it had no business being
    // above:
    //
    //   * hovering the game counted as focused, to get topmost in place before
    //     a click could activate it. Killed the click-path blink completely,
    //     but parking the cursor over the game while another app was focused
    //     put the panel over that app.
    //   * staying topmost unless the focused window actually covered the panel.
    //     Same complaint, wider: it sat above everything it did not overlap,
    //     and alt-tab made it jump to the front.
    //
    // Kept here as a record so neither is re-invented as a fresh idea. The
    // blink on tab-in is the accepted cost; ownership above is the only thing
    // that removed it outright, and that has its own price.
    //
    // The `buried` gate above is the opposite move -- focus decides the band,
    // but only CORROBORATED focus. It narrows fg and can only demote, so it
    // is not a third entry in this list.

    bool moved = (x != g_hud_lastx || y != g_hud_lasty || resized);

    // APP MODE rides the SAME BAND RULE as the overlay: above the client
    // while the client is what you are looking at, an ordinary window the
    // moment you switch to something else. Not a decoration -- without it,
    // returning to the game from a third app left the console open and
    // BURIED (measured: app window z#22, client z#21), which is the worst of
    // both, since it is running, the user asked for it to stay, and they
    // cannot see it. A one-shot re-raise from the foreground handler was
    // tried first and loses the race: Windows raises the client as part of
    // the same activation, after our call.
    //
    // THE ONE OWNER of the app window's band, the way raise_overlays is the
    // one owner of the overlay's. The rule, in the user's own words
    // (2026-08-22): topmost exactly while THE CLIENT is the foreground app;
    // an ordinary window when anything else is -- another app, or the
    // console itself (a focused window is naturally on top of the normal
    // band and needs no bit). Evaluated continuously from the real
    // GWL_EXSTYLE, never latched at open time. The first cut used the
    // overlay's forced `fg`, which is true for the whole of app mode -- that
    // floated the console over whatever a user switched to, user-confirmed
    // wrong ("it is topmost even when i changed tab to other app").
    //
    // The client-click stamp joins the foreground test as pre-emption: the
    // LL hook sees a click on the client BEFORE the activation raise, so the
    // promote lands before (or within a tick of) the client rising -- this
    // is also what keeps the close-path content flash away (the ~30ms
    // game-glimpse between the client's raise and the close box composing,
    // caught by the spread capture): during the close gesture the app is
    // already topmost when the client comes up. A hook-time TOPMOST lift
    // with its own backstop was shipped for one build and removed: two
    // owners demoted/promoted each other sixty times a second (1067 fight
    // lines in one trace).
    //
    // Two details of HOW it moves between bands, both load-bearing:
    //   * promote and demote are both followed by raise_overlays(): a band
    //     change of the app leaves the overlay in the band the app just
    //     left, and the overlay's slot is DIRECTLY UNDER the app (since
    //     2026-09-04; overlay_glue_under_app), so it follows in the same
    //     tick -- the order is app > overlay > client in either band.
    //   * the demote's slot depends on who is foreground. Ours: plain
    //     HWND_NOTOPMOST -- top of the normal band is where a focused window
    //     lives anyway, and sinking it would shove the window the user is
    //     typing in under others. Anyone else's: the slot directly above the
    //     CLIENT (NOTOPMOST there would park the app ON TOP of the third app
    //     the user just switched to); inserting after a non-topmost window
    //     strips the bit, the same fact the overlay's demote glue relies on.
    //     If the window directly above the client is itself topmost (band
    //     boundary, nothing between), fall back to NOTOPMOST -- the next
    //     tick's read re-evaluates.
    // OWNERSHIP is what actually keeps the app above the game; the band rule
    // below only decides whether it also floats over OTHER apps.
    //
    // The blink this removes is the one the panel's comment above calls
    // unremovable-by-design, and for the panel it still is: Windows raises the
    // game as part of the activation, our foreground event arrives afterwards,
    // and 0.5-1.4ms of mis-order is the floor no faster reaction can beat.
    // Measured from the user's own alt-tab trace, the app's promote lands
    // 0.5-1.4ms after the event and the OVERLAY's 6-8ms after -- the window
    // that is out of the band LONGER is the one that barely blinks -- so the
    // difference was never the reaction: it is that the app is a 1180x780
    // window in the middle of the screen and the badge is 44x66 at the edge.
    // Same odds of catching a composed frame, wildly different visibility.
    //
    // An OWNED window is kept above its owner by USER32 as part of the
    // activation itself, so there is no interval to catch. MEASURED with a
    // two-window probe against the live client (identical windows, neither
    // topmost, neither moving itself, game then activated): owned sat at z=5
    // with the game at z=6, the unowned control at z=7 -- above and below
    // respectively, on every pass. The control is what makes that check able
    // to go red.
    //
    // Owner, NOT parent: the app stays a top-level window with its own focus,
    // input queue and lifetime. And the obvious hazard -- "owned windows are
    // destroyed with their owner", which would kill the console whenever
    // Roblox closes -- does NOT hold across processes: probed by killing an
    // owner process and watching the owned window in another process stay
    // alive and visible (owner_alive=0 self_alive=1) for the rest of the run.
    // That cleanup is within-process.
    //
    // Costs, accepted knowingly: the app minimises and restores WITH the
    // client, and can no longer be placed under it. Both are what "the
    // console lives in the game" already means.
    // THE OVERLAY NOW HAS THE SAME OWNER (see the ownership block above),
    // second attempt. The first was reverted on a user screenshot -- clicking
    // another app left the CLIENT covering it -- and the diagnosis written
    // here at the time ("raising ours drags the client up") was close but not
    // the mechanism. Mimic-probed 2026-08-22: the hoist happens exactly when
    // an owned window EXITS the topmost band, whatever slot it is sent to;
    // entries, in-band moves and the owner-set never hoist. hud_swp's bracket
    // makes every exit happen unowned, which is why the owner can stay on
    // full-time now. Why the APP window never needed a bracket: its demote
    // runs its own SetWindowPos, not hud_swp -- and its band exits measurably
    // do not hoist. What is different about them is NOT pinned (suspected:
    // they always run inside the activation window of a fresh real-input
    // foreground change, while the poison probes ran seconds later); if the
    // app is ever seen hoisting the game over a third app, bracket its two
    // demote calls the same way before suspecting anything else.
    //
    // CHAINING the overlay to the APP instead was tried too, and is out. It
    // should have been the safe version -- the drag-the-owner-up effect would
    // hoist our app, which belongs above the client anyway -- but MEASURED it
    // is worse than either alternative. Same probe, 10 verified client
    // activations with the app open, counting EXTERNAL overlay z-moves (a
    // reorder someone else performed, which we then correct, and those
    // corrections are what the flicker is made of):
    //
    //     no ownership at all      0
    //     app owned by client      0
    //     overlay owned by app    21
    //
    // So the app's owner costs the overlay nothing, and the chain costs it a
    // reorder per activation. Do not re-attempt either direction of overlay
    // ownership without beating those numbers.
    if (ui_main_hwnd()) {
        // During a CLOSE the owner follows the latched decision (foreground
        // at close time -- app_close_begin): a third-app close KEEPS it (the
        // carry is the only thing above the client's raise), a console-fg
        // close DROPPED it there and this holds that. Plus the return-hold-off.
        bool held = (g_app_owner_holdoff && GetTickCount() - g_app_owner_holdoff < 400) ||
                    (g_app_closing && !g_app_close_keep_owner);
        HWND want = (app_owns_window && game && IsWindow(game) && !held) ? game : nullptr;
        HWND cur  = (HWND)GetWindowLongPtrW(ui_main_hwnd(), GWLP_HWNDPARENT);
        if (cur != want) {
            SetWindowLongPtrW(ui_main_hwnd(), GWLP_HWNDPARENT, (LONG_PTR)want);
            zlog("APP owner -> %p", (void*)want);
        }
    }

    if (g_app_mode && !g_app_opening && !g_app_closing &&
        ui_main_hwnd() && IsWindowVisible(ui_main_hwnd())) {
        // Same corroboration as the overlay's fg above: pid-foreground alone
        // can be a buried never-activated window (hud_client_covered), and
        // the app must not float over the app that really covers the client.
        // In practice app mode's own window sits over the client's centre, so
        // buried is false whenever the mode is open and placed -- the gate
        // matters for the moment the state arises underneath it.
        // ...AND WHILE THE CONSOLE ITSELF IS FOREGROUND (2026-09-04). The rule
        // used to read "an ordinary window when the console is focused -- a
        // focused window is on top of the normal band and needs no bit". True
        // of the app; false of the overlay under it. With the app in the
        // normal band, ANY bring-to-top of the app -- a click on its title
        // bar (SC_MOVE's raise of an already-active owned window), a click
        // on its body from the dock -- restacks the owner group and the
        // CLIENT rises to the top of the normal band, over the overlay,
        // which is not in the group while the console is foreground. The
        // app does not move, so nothing tells us; only the tick caught it:
        // measured 13-44ms of badge/dock under the game per title-bar click
        // (0.24ms sampler), the user's "clicking the top bar flashes the
        // dock". In the topmost band the app's raise carries the client to
        // the top of the NORMAL band, still under the topmost overlay -- no
        // interval to catch. Ours-and-not-a-snippet-window (a focused radar
        // must stay above the app, unchanged), and the overlay's browser
        // (a dock click, which now closes the app anyway). Floating over
        // OTHER apps is unchanged: the moment a third app is foreground the
        // demote below runs, exactly as from the client-foreground state.
        HWND fgw0    = GetForegroundWindow();
        HWND fgroot0 = fgw0 ? GetAncestor(fgw0, GA_ROOT) : nullptr;
        bool console_fg = (fgpid == GetCurrentProcessId() && !snip_is_window(fgroot0)) ||
                          (g_hudc.browser_pid && fgpid == g_hudc.browser_pid);
        bool app_fg  = ((fgpid == s.pid) && !buried) ||
                       (GetTickCount() - g_game_click_at < 400) || console_fg;
        bool app_top = (GetWindowLongW(ui_main_hwnd(), GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        // BOTH steady states are no-ops. The demote below used to be
        // `else if (app_top)` -- which the steady state app_fg && app_top
        // FALLS INTO, so with the client foreground the rule could never
        // rest: promote on tick N, demote on tick N+1, promote on N+2 -- a
        // permanent 30Hz z-order war, two SetWindowPos round trips per
        // cycle, plus a raise_overlays per promote. Caught 2026-08-27 by the
        // ZWATCH stack trace (both call sites resolved into this block,
        // alternating every ~30ms, after=TOPMOST / after=above-game), after
        // hiding since it shipped: with only app, overlay and game in the
        // band, the promote slot and the demote slot LOOK identical, so the
        // war moved nothing anyone could see -- until a floating snippet
        // window sat in the normal band, and every promote crossed the app
        // over it while it held focus ("cannot focus the radar", and the
        // same trace's fg=radar buried/unburied flip). The war also explains
        // that symptom's intermittency: which half of the cycle a click
        // landed in decided whether the radar came up over the app or under.
        if (app_fg && !app_top) {
            zlog("APP band -> TOPMOST (fgpid=%lu%s)", fgpid, console_fg ? " console" : "");
            app_swp(HWND_TOPMOST);
            raise_overlays();          // the overlay follows it up
            // No position heal after the promote any more: whether the gate
            // grants the bit, swallows the move or refuses outright, the
            // OWNED app cannot rest below the client (app_ensure_above note
            // above -- USER32 restores a forced below-owner insert in
            // 0.24ms). The band decides floating over OTHER apps only.
        } else if (!app_fg && app_top) {
            zlog("APP band -> normal (fgpid=%lu)", fgpid);
            // "Ours is foreground" is NOT "the app is focused". A floating
            // snippet window (fx_window_*) is this process too, and it was
            // exactly the NOTOPMOST branch below that made it unfocusable in
            // app mode: NOTOPMOST is the TOP of the normal band, so the
            // demote landed the app ON TOP of the snippet window the user
            // had just clicked -- and, the app being owned by the client, a
            // NOTOPMOST exit HOISTS the owner group, putting the GAME over
            // it as well (both probed 2026-08-27: NOTOPMOST demote = APP
            // above RADAR; owned NOTOPMOST exit = GAME above RADAR too; the
            // relative slot below = RADAR APP GAME, owned or not). So a
            // focused snippet window takes the third-app branch: the app
            // slots directly above the client, under the window the user is
            // using. The close gesture and the arrive-stamp keep treating
            // snippet windows as ours -- clicking the game from one still
            // closes, clicking the app from one still summons nothing.
            HWND fgw    = GetForegroundWindow();
            HWND fgroot = fgw ? GetAncestor(fgw, GA_ROOT) : nullptr;
            // OVERLAY FIRST, THEN THE APP, on the way down. The overlay's
            // slot is directly under the app, so it is in the topmost band
            // with it; demoting the app first left the overlay above it
            // until the glue's second call -- measured 3-10ms between the
            // two SetWindowPos (2026-09-04, zlog), enough for a composed
            // frame of dock-over-console. Sending the overlay to the
            // destination's below-slot first and the app in above it keeps
            // app > overlay at every instant; the promote above is the
            // mirror (app up first, overlay follows), for the same reason.
            // (The console-foreground case no longer demotes at all -- see
            // console_fg above -- so what reaches here is a third app or a
            // focused snippet window: the slot directly above the client.)
            {
                (void)fgroot;
                // The overlay into the slot directly above the client (the
                // demote glue's own slot), skipping itself and the app
                // when they are what sits there; then the app directly
                // above the overlay.
                HWND above = game ? GetWindow(game, GW_HWNDPREV) : nullptr;
                while (above && (above == g_hud_hwnd || above == ui_main_hwnd()))
                    above = GetWindow(above, GW_HWNDPREV);
                bool above_top = above &&
                    (GetWindowLongW(above, GWL_EXSTYLE) & WS_EX_TOPMOST);
                hud_swp((above && !above_top) ? above : HWND_NOTOPMOST, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                HWND over = GetWindow(g_hud_hwnd, GW_HWNDPREV);
                bool over_top = over &&
                    (GetWindowLongW(over, GWL_EXSTYLE) & WS_EX_TOPMOST);
                app_swp((over && over != ui_main_hwnd() && !over_top)
                            ? over : HWND_NOTOPMOST);
            }
            raise_overlays();          // a no-op when the two landed as asked
        }
    }

    // What the window ACTUALLY is, not what we last asked for. The topmost bit
    // can be lost without us asking -- anything that puts a window into the
    // band above us, or places us relative to a non-topmost window, strips it
    // (the demote path below says so out loud) -- and a tick that only acts on
    // TRANSITIONS never notices, because its cached g_hud_was_fg still says
    // "already done". Measured: the overlay sat at z#26 with the game at z#16
    // and WS_EX_TOPMOST clear, while the trace showed our last request was
    // TOPMOST and nothing had asked otherwise. The badge was drawn, its hit
    // region was correct, and every click went to the game -- which reads as
    // "the overlay stopped working", not as a z-order problem. Reading the
    // real state makes the tick self-healing; when it agrees, nothing happens.
    bool is_top = (GetWindowLongW(g_hud_hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;

    if (app_in_front() && g_app_box_covering) {
        // OPEN LEG, box still covering: keep the overlay TOPMOST above the app
        // so the box hides the still-painting WebView2 surface (Fault A), and
        // keep following the client rect. Belt: if the app page never reports
        // its paint (APPPAINTED lost, a page rebuilt mid-open), run the
        // uncover after 250ms rather than sitting above the app forever.
        if (GetTickCount() - g_app_cover_at > 250) {
            zlog("APP paint witness belt fired");
            app_open_uncover();
            overlay_glue_under_app(game, x, y, w, h, moved || !g_hud_shown);
        } else {
            hud_swp(HWND_TOPMOST, x, y, w, h,
                    (moved || !g_hud_shown) ? (SWP_NOACTIVATE | SWP_SHOWWINDOW)
                                            : (SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
        }
    } else if (app_in_front()) {
        // THE APP IS ON GLASS: the overlay's slot is directly under it, in
        // whichever band the app's own rule put it (overlay_glue_under_app).
        // Neither the promote/heal nor the demote glue below applies -- the
        // app window is the thing in front, and its band rule above already
        // decided whether the pair floats over other apps. The band-lost
        // machinery is untouched here: it belongs to the client-foreground
        // state with nothing of ours in front, which this is not.
        overlay_glue_under_app(game, x, y, w, h, moved || !g_hud_shown);
    } else if (fg) {
        // Healing a lost topmost bit is right when something took the band
        // once. It is WRONG when the bit will not stick at all: every request
        // is undone immediately, and a tick that re-asks at 60 Hz just burns
        // a SetWindowPos per frame to lose the same race. Measured: 12
        // consecutive heals inside 300 ms with the style still reading
        // 0x8200080 afterwards.
        //
        // WHO undoes it is deliberately an open question -- see the globals
        // block for what the harnesses ELIMINATED (generic fullscreen, quiet
        // SetWindowLong strips). The fight was only ever observed at tick
        // granularity, so "bit clear after each call" never distinguished
        // "the call failed" (ret/err below now say) from "an external
        // SetWindowPos stripped it within 16ms" (g_hud_ext_zmoves now says).
        //
        // So: heal freely; once it has clearly stopped working, back off to
        // once a second, say why ONCE -- and do not go quietly invisible:
        // ride the normal band directly above the game (the demote path's
        // slot, self-healing per tick), and if even that will not hold,
        // bring the desktop window back rather than let the console believe
        // it is showing while nothing is (the tick's safety valve, top of
        // this function).
        bool heal = !is_top;
        if (heal && g_hud_band_fails >= 8) {
            DWORD now = GetTickCount();
            if (now - g_hud_band_retry < 1000) heal = false;
            else g_hud_band_retry = now;
        }
        if (moved || !g_hud_was_fg || !g_hud_shown || heal) {

            zlog("overlay -> TOPMOST%s", is_top ? "" : "  (bit was lost)");
            hud_swp(HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            bool now_top = (GetWindowLongW(g_hud_hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
            if (!is_top)
                zlog("   heal ret=%d err=%lu now_top=%d ext=%d",
                     (int)g_hud_swp_ret, g_hud_swp_err, (int)now_top,
                     g_hud_ext_zmoves);
            if (now_top) {
                g_hud_band_fails = 0; g_hud_band_told = false;
                g_hud_band_lost  = false;
                g_hud_suppressed = false;  // won outright; overlay owns the UI again
            } else if (++g_hud_band_fails == 8 && !g_hud_band_told) {
                g_hud_band_told = true;
                // The one diagnostic line this incident gets, carrying what
                // the first investigation needed and did not have: what the
                // call returned, whether an external actor has moved us, who
                // owns us, and the client window's actual shape (fullscreen
                // is readable from style + rect vs monitor).
                MONITORINFO mi; mi.cbSize = sizeof mi;
                GetMonitorInfoW(MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST), &mi);
                LONG gst = GetWindowLongW(game, GWL_STYLE);
                long mw = mi.rcMonitor.right - mi.rcMonitor.left;
                long mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
                // The fullscreen-optimizations SHAPE: borderless (no caption)
                // and the client rect exactly the monitor. MEASURED to be the
                // state that refuses WS_EX_TOPMOST while returning success
                // (ret=1, bit stays clear); a windowed client (rect != mon,
                // has caption) never reaches this branch -- topmost sticks
                // there, and attaching to its input thread does not change
                // that (build harness, 2026-08-22). This flag is the
                // actionable fact the first capture lacked.
                bool fso = (g.left == mi.rcMonitor.left && g.top == mi.rcMonitor.top &&
                            g.right - g.left == mw && g.bottom - g.top == mh &&
                            !(gst & WS_CAPTION));
                char diag[288];
                snprintf(diag, sizeof diag,
                         "overlay band diag: ret=%d err=%lu ext=%d pauses=%d fso=%d owner=%p "
                         "game st=%08lX ex=%08lX rect=%ld,%ld %ldx%ld mon=%ldx%ld",
                         (int)g_hud_swp_ret, g_hud_swp_err, g_hud_ext_zmoves,
                         g_hud_owner_pauses, (int)fso,
                         (void*)GetWindowLongPtrW(g_hud_hwnd, GWLP_HWNDPARENT),
                         (unsigned long)gst,
                         (unsigned long)GetWindowLongW(game, GWL_EXSTYLE),
                         g.left, g.top, g.right - g.left, g.bottom - g.top, mw, mh);
                // TRACE ONLY. Window styles, rects and a raw SetWindowPos
                // return are diagnostics for whoever is fixing this, not for
                // the person playing -- the WARN below is what THEY need, and
                // a wall of hex above it buries it. zlog writes
                // eclipse_z.log under ECLIPSE_ZLOG=1; ask for that when an
                // incident needs picking apart.
                zlog("%s", diag);
                // Name the real fix. The client IS borderless here, so the
                // old "switch to borderless" was useless advice; windowed
                // mode (or disabling fullscreen optimizations for the client
                // exe) is what lets an overlay sit above it.
                if (fso)
                    ui_log("WARN", "! Roblox is in borderless fullscreen - the overlay "
                                   "can't cover it; use Windowed mode, or turn off "
                                   "fullscreen optimizations for RobloxPlayerBeta.exe");
                else
                    // Not a client-shape problem: Windows is refusing the
                    // entry (see the globals block -- the gate opens on the
                    // next input-driven activation of the game), so the
                    // remedy really is one click.
                    ui_log("WARN", "! the overlay dropped behind Roblox - "
                                   "click the game once to bring it back");
            }
        }
        g_hud_band_lost = (g_hud_band_fails >= 8);
        if (g_hud_band_lost &&
            !(GetWindowLongW(g_hud_hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
            // Band confirmed lost. MEASURED (build harness + the user's live
            // diag, 2026-08-22): the client is borderless-fullscreen and the
            // above-game slot does NOT hold either -- the overlay sits below
            // a client that is not even topmost, WindowFromPoint at the badge
            // returns the game, and the badge cannot be clicked. A drawn-but-
            // unclickable badge is the worst outcome, so in overlay mode we
            // do not sit in it: the desktop window (reachable by Alt-Tab --
            // the client is borderless, not exclusive) comes back at once as
            // the way to the console. It clears the instant a heal succeeds
            // (top of the tick), i.e. the moment the client goes windowed.
            // Outside overlay mode the panel just rides the above-game slot
            // as before; there the desktop window is already the primary UI.
            // The desktop-window valve waits out the first storm (8 fails
            // arrive inside ~130ms of tick heals; 11 means the 1/s backoff
            // has also failed ~3 times). MEASURED 2026-08-22: the refusal's
            // common cause is Windows' entry gate after a programmatic
            // focus hand-back, which the user's next click on the game
            // heals through the LL hook -- popping the desktop window over
            // the game inside that window of time would flash it for every
            // ordinary use of the overlay from an unfocused game.
            if (g_overlay_pref && !g_hud_suppressed && g_hud_band_fails >= 11) {
                g_hud_suppressed = true;
                // App mode cannot be open here (our foreground exempts the
                // band), but a leg caught mid-flight is put back directly --
                // the overlay this valve is giving up on cannot animate it.
                app_abort_restore();
                ui_log("WARN", "! overlay is stuck behind Roblox - "
                               "desktop window restored; Alt-Tab to it");
            }
            HWND above = GetWindow(game, GW_HWNDPREV);
            if (above != g_hud_hwnd) {
                zlog("overlay -> above-game glue (band lost)");
                hud_swp(above ? above : HWND_TOP, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }
    } else {
        if (moved || g_hud_was_fg || !g_hud_shown || is_top) {
            zlog("overlay -> NOTOPMOST  (fg=%d was=%d moved=%d)", fg, g_hud_was_fg, moved);
            hud_swp(HWND_NOTOPMOST, x, y, w, h,
                    SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        // The slot directly above the game. One overlay window now, so the
        // old skip-past-the-dock dance (two ticks reshuffling two windows
        // through this slot forever) is gone with the second window.
        //
        // ...but MID-LEG of the app choreography (the steady state takes the
        // under-app branch above) the console's own window is what sits
        // directly above the client, and the travel box must compose OVER
        // it: the slot to take is then directly above the APP.
        HWND anchor = game;
        if (g_app_mode && ui_main_hwnd() && IsWindowVisible(ui_main_hwnd())) anchor = ui_main_hwnd();
        HWND above = GetWindow(anchor, GW_HWNDPREV);   // the window just above it
        if (above != g_hud_hwnd) {
            zlog("overlay -> insert after %p  (THIS STRIPS TOPMOST)", (void*)above);
            hud_swp(above ? above : HWND_TOP, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
    // OWNER ONLY WHILE DEMOTED. The client owns the overlay exactly while
    // the overlay rides the normal band (a third app is foreground) -- that
    // is the state a tab-in starts from, and USER32's owner enforcement is
    // what carries the overlay above the game through the activation with
    // no interval for the blink (measured: worst inversion 0.2ms owned vs
    // 8.8ms unowned with the app open -- more than a composed frame). Once
    // the game is foreground the promote above has already put the overlay
    // in the topmost band, the owner buys nothing, and holding it hides the
    // cursor over the badge (see the ownership block above). Setting the
    // owner here is safe in place: the owner-set never hoists the owner and
    // at most pulls the owned window DOWN to its group (mimic-probed).
    // Mode 1 keeps the owner unconditionally and unbracketed -- it is the
    // negative control that reproduces the coverage regression on demand.
    {
        int  m    = hud_owner_mode();
        // KEEP THE OVERLAY OWNED THROUGH A THIRD-APP CLOSE. Normally the
        // owner is dropped the moment the game is foreground (cursor bleed).
        // But a badge close FROM A THIRD APP hands the game foreground from
        // a state where the overlay is in the NORMAL band (the demote slot)
        // and the entry gate is shut -- so it cannot take topmost until the
        // reactive heal, ~11-15ms of badge-behind-game after the app is gone
        // (measured 2026-09-04; a console-fg close is clean because the
        // overlay was topmost throughout). Owned, the game's own activation
        // carries the overlay above the client with no interval. Scoped to
        // the close leg with the app NOT topmost -- the same third-app
        // condition app_close_begin keeps the APP owner for -- so the clean
        // console-fg close and steady play are untouched, and the cursor
        // bleed lasts only the ~50ms to close_finish.
        // Keep the overlay owned through a THIRD-APP close (its activation
        // carry is what keeps the badge above the client's raise); a console-
        // fg close leaves it dropped (the overlay is genuinely topmost there).
        bool close_carry = g_app_closing && g_app_close_keep_owner;
        // Owned while the user is in ANOTHER app -- that is what carries the
        // overlay above the game through the client's own activation with no
        // gap (0.2ms owned vs 8.8ms unowned). Dropping it here to fix the
        // activation bug cost exactly that: a visible badge blink on the way
        // back, with the console closed. The bug is fixed at the CLICK instead,
        // in hud_mouse_ll -- see overlay_owner_drop.
        bool want = m && game && IsWindow(game) && (m == 1 || !fg || close_carry);
        if (want && g_hud_owner != game) {
            SetWindowLongPtrW(g_hud_hwnd, GWLP_HWNDPARENT, (LONG_PTR)game);
            HWND now = (HWND)GetWindowLongPtrW(g_hud_hwnd, GWLP_HWNDPARENT);
            g_hud_owner = (now == game) ? game : nullptr;
            zlog("overlay owner -> %p (mode %d fg=%d)", (void*)g_hud_owner, m, (int)fg);
            if (!g_hud_owner)
                ui_log("LOG", "! in-game panel: the client would not take ownership "
                              "- falling back to z-order, which can blink on tab-in");
        } else if (!want && g_hud_owner) {
            SetWindowLongPtrW(g_hud_hwnd, GWLP_HWNDPARENT, 0);
            g_hud_owner = nullptr;
            zlog("overlay owner -> 0 (fg=%d)", (int)fg);
        }
    }

    g_hud_was_fg = fg;
    g_hud_shown  = true;
    g_hud_lastx = x; g_hud_lasty = y; g_hud_lastw = w; g_hud_lasth = h;
}

// Local health for the panel header.
//
// Polled at 4 Hz, not on the 60 Hz tick: it is four reads down a pointer chain
// and nobody reads a health number sixty times a second. fx_part memoises the
// Humanoid lookup, so the steady-state cost is the two float reads.
//
// Fails silent by design. In the menu, while dead, or with a stale conf there
// is no Humanoid to read, and the right answer is to show nothing rather than
// to show a confident 0 -- a health bar that reads empty because the console
// could not resolve a pointer is worse than no health bar.
void health_tick() {
    static DWORD next = 0;
    DWORD now = GetTickCount();
    if (now < next) return;
    next = now + 250;

    float hp = -1.0f, max = -1.0f;
    // NO PLACE, NO HEALTH. In the menu there is no character, but the cached
    // one from the game you just left can still be non-null and its memory can
    // still read 100/100 -- so this would report ok on the home screen. (It is
    // not what made the chip linger there; that was a CSS override in the page.
    // This gate is still right, and it is what makes ok=0 land within one tick
    // of leaving rather than whenever the character cache next turns over.)
    // A number you cannot stand behind is worse than no
    // number: the page already hides the chip when ok is false, and hiding is
    // the honest answer here.
    uint64_t chr = fx_place() ? fx_character() : 0;
    if (chr) {
        uint64_t hum = fx_part(chr, "Humanoid");
        uint64_t oh  = fx_off("Humanoid.Health");
        uint64_t om  = fx_off("Humanoid.MaxHealth");
        if (hum && oh != (uint64_t)-1 && om != (uint64_t)-1) {
            float a = 0.0f, b = 0.0f;
            if (fx_rdblk(hum + oh, &a, 4) == 4 && fx_rdblk(hum + om, &b, 4) == 4 &&
                b > 0.0f && b < 1e6f && a >= 0.0f && a <= b + 1.0f) {
                hp = a; max = b;
            }
        }
    }

    // DIAGNOSTIC, on the EDGE only -- one line per change of state, so it
    // costs nothing while the answer is steady.
    //
    // Kept because it is what ended the guessing. "HP lingers in the menu"
    // survived two plausible host-side fixes, and this line showed the host
    // had been right all along: ok=0 within 250ms of leaving, twice, while the
    // panel still rendered the number. The bug was in the PAGE -- .hp had an
    // explicit display, which overrides the hidden attribute. If a readout
    // ever disagrees with the host again, look here first and then at the CSS.
    {
        static int last_ok = -1;
        int ok = hp >= 0.0f ? 1 : 0;
        if (ok != last_ok) {
            last_ok = ok;
            zlog("HEALTH -> ok=%d hp=%.0f place=%llu chr=0x%llx dm=0x%llx",
                 ok, hp, (unsigned long long)fx_place(),
                 (unsigned long long)fx_character(),
                 (unsigned long long)ec::state().dm);
        }
    }

    char buf[96];
    if (hp < 0.0f) snprintf(buf, sizeof buf, "HEALTH:{\"ok\":false}");
    else snprintf(buf, sizeof buf, "HEALTH:{\"ok\":true,\"hp\":%.0f,\"max\":%.0f}", hp, max);
    hud_push(buf);
}


// Runs after the tick, and covers what it does not own.
//
// The hooks and the hand-back serve the overlay window whatever it is
// carrying -- console panel, docked snippet panel, or both. Tying them to the
// panel's SETTING once meant switching the panel off quietly degraded a
// docked window, which is a coupling nobody would think to look for; the
// window's existence is the honest signal, since it exists exactly while
// anything overlaid does.
void overlay_tick() {
    const ec::State& s = ec::state();
    bool any = s.pid && g_hud_hwnd;

    if (any) hud_install_hooks();
    else     overlay_release_hooks();

    if (any) overlay_handback(s.pid, hud_client_cached(s.pid));
}

// ---- app mode: the console's REAL WINDOW over the game ----------------------
//
// Re-architected 2026-08-22 (docs/overlay-plan.md step 3, second design). The
// app is g_hwnd itself -- the desktop window, WS_EX_TOOLWINDOW while over the
// game -- not an iframe inside the overlay. What the first design could not
// give, and this one gets for free:
//   * the drag is a real window move (the iframe drag ran OS -> host ->
//     SendMouseInput -> renderer and could not be made immediate);
//   * clicking the app takes foreground the ORDINARY way, so the topmost
//     band's entry gate -- which refused the overlay in a state that blocked
//     testing twice -- is simply not involved;
//   * the window is its own shape, so the region/pixels disagreement class
//     cannot touch it. Keyboard, IME and clipboard come with focus, no
//     MoveFocus, no focus mode.
// The overlay keeps ONLY the travel: an empty box (#apptrav -- background,
// rim, radius, no content) runs home-badge -> window rect and back, and the
// swap between box and window is choreographed so it is never composed
// against the bare game:
//
//   open:  host APPOPEN:rect -> page travels the box out -> APP_ATRECT ->
//          host shows the window under the still-held box (SWP_NOACTIVATE)
//          and asks the APP page for a painted frame (APPPAINT) ->
//          APPPAINTED -> host promotes the window above the box, glues the
//          overlay under it, takes foreground, APPSHOWN -> page drops the
//          box, beneath the window where it changes nothing on screen.
//   close: host APPCLOSE:rect -> page places the box over the window, waits a
//          composed frame -> APP_BOXUP -> host hides the window (and, badge/
//          hotkey paths, hands focus to the game), APPGO -> page travels the
//          box home.
//
// ORDER IS THE POINT: the window shows before the box hides, and hides only
// after the box is up. Either backwards is a one-frame flash of the game.
// hud_tick carries a watchdog for both legs -- a page that never answers
// (rebuild mid-flight, a lost message) must not strand a hidden console.
// (The leg flags live up near g_app_mode; the hook and the tick read them.)

static void app_apply_overlay_styles(bool on) {
    if (!ui_main_hwnd()) return;
    // TOOLWINDOW: off the taskbar and out of Alt-Tab while it lives in the
    // game -- the badge, the hotkey and the game itself are its switcher.
    // The frame is already frameless-with-edges, and DWM already rounds it
    // (DWMWCP_ROUND at startup) to match the overlay.
    LONG_PTR ex = GetWindowLongPtrW(ui_main_hwnd(), GWL_EXSTYLE);
    if (on) ex |= WS_EX_TOOLWINDOW;
    else    ex &= ~WS_EX_TOOLWINDOW;
    SetWindowLongPtrW(ui_main_hwnd(), GWL_EXSTYLE, ex);

    // AND NO MAXIMISE WHILE IT LIVES IN THE GAME. WS_MAXIMIZEBOX is what makes
    // Windows offer edge-snap and the Snap Layouts flyout, and a snap to the
    // top edge IS a maximise -- so the console came back zoomed to the monitor
    // with every SetWindowPos after it fighting Windows for the rect: the
    // drag-settle clamp into the client, then the tick's follow, every frame.
    // The window ends up drawn at the monitor rect with our geometry believing
    // something else, and a zoomed window cannot be edge-resized, which is the
    // "and it will not resize" half of the report.
    //
    // The DECISION was already made -- handle() refuses WIN:ZOOM and WIN:MIN
    // while overlay_app_mode() -- it just was not enforced anywhere the OS
    // could see it, so the snap path walked straight past the page-level
    // block. Nothing changes on the desktop: the capability goes back on the
    // way out.
    //
    // WS_MAXIMIZE (the STATE) goes with it, and comes back NEVER. A window
    // that was already maximised or snapped when it entered the game arrived
    // still zoomed and hit the identical bug from the other direction, because
    // the un-maximise this replaces did not work: it called SetWindowPlacement
    // with showCmd = SW_HIDE, which hides a window without restoring it, so
    // IsZoomed stayed true and the comment claiming otherwise was never
    // measured. Stripping the style is what actually clears it, and it is safe
    // while hidden -- the rect is set explicitly at APP_ATRECT either way.
    //
    // The state is deliberately NOT restored on the way out. Re-maximising
    // from a snapshot taken before the console entered the game is the same
    // mistake as restoring its rect from one: un-maximise inside the game and
    // it came back full-screen anyway. It is one window, and it keeps what it
    // is (docs/window-geometry.md).
    LONG st = GetWindowLongW(ui_main_hwnd(), GWL_STYLE);
    if (on) st &= ~(WS_MAXIMIZEBOX | WS_MAXIMIZE);
    else    st |=   WS_MAXIMIZEBOX;
    SetWindowLongW(ui_main_hwnd(), GWL_STYLE, st);
    // SetWindowLong's style change is not live until a SetWindowPos with
    // SWP_FRAMECHANGED tells the manager to re-read it. Free of side effects
    // here: all three callers (app_open_begin, app_close_finish,
    // app_abort_restore) have the window HIDDEN at this point, so the
    // WM_NCCALCSIZE it provokes cannot flash -- and ours returns the full rect
    // anyway, so the client area does not move.
    SetWindowPos(ui_main_hwnd(), nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                 SWP_FRAMECHANGED);
}

// The app's rect inside the client, page px (== device px; the overlay pins
// rasterization to 1.0). Same numbers the first design used: the desktop
// window's preferred 1180x780, clamped into the client with 12px margins,
// right-anchored at the remembered inset, vertically centred on the
// remembered centre. Clamping is re-run per call because the client's size is
// whatever it is NOW -- the anchor is only persisted by an actual drag.
static void app_target_rect(int cw, int ch, int* l, int* t, int* w, int* h) {
    // The size it carried in from the desktop (app_anchor_capture) wins over
    // the default fit; both are then clamped into the client, so a window
    // sized against a large client cannot strand itself when the game is
    // smaller next time.
    //
    // NO FILE IS READ HERE, and no settings flag is consulted. This runs on
    // every client move and resize (hud_tick), so anything it reads is read
    // hundreds of times a session -- and the version that re-derived the
    // anchor from a remembered screen position on every call is precisely
    // what stopped the window following the game. The anchor is captured
    // once, at the open, and edited only by a drag.
    int aw = g_app_w ? g_app_w : (cw - 24 < 1180 ? cw - 24 : 1180);
    int ah = g_app_h ? g_app_h : (ch - 24 < 780  ? ch - 24 : 780);
    // Still has to fit the client.
    if (aw > cw - 24) aw = cw - 24;
    if (ah > ch - 24) ah = ch - 24;
    if (aw > cw) aw = cw;   if (aw < 320) aw = 320;
    if (ah > ch) ah = ch;   if (ah < 240) ah = 240;

    // Never placed -> centre it, horizontally as well as vertically.
    int right = (g_app_right >= 0) ? g_app_right : (cw - aw) / 2;
    int maxr  = cw - aw; if (maxr < 0) maxr = 0;
    if (right > maxr) right = maxr;
    if (right < 0)    right = 0;
    int cy   = g_app_cy ? g_app_cy : ch / 2;
    int half = ah / 2;
    if (cy < half)      cy = half;
    if (cy > ch - half) cy = ch - half;
    *l = cw - right - aw;
    *t = cy - half;
    *w = aw;
    *h = ah;
}

// THE WINDOW RECT IS NOT WHAT DWM COMPOSES, and the travel box has to follow
// the second one. A window with WS_THICKFRAME carries invisible resize borders
// and GetWindowRect includes them. MEASURED on this build (2026-09-07, the app
// window at 1180x679): 7px on left, right and bottom, 0 on top -- the top
// border is drawn at the window edge. So a box drawn at the window rect is
// 14px wider and 7px taller than the app the user sees, overshooting on
// exactly those three sides, which is how it was reported.
//
// DWMWA_EXTENDED_FRAME_BOUNDS is the composed rect, and it answers for a
// HIDDEN window too (verified on this window while vis=FALSE), which is what
// the open leg needs -- there the window has not been placed yet, so the inset
// is applied to the COMPUTED target rather than read back off it.
//
// Fails to a zero inset: DWM declining, or absurd numbers, gives the old
// behaviour rather than a box in the wrong place.
static void app_frame_inset(int* il, int* it, int* ir, int* ib) {
    *il = *it = *ir = *ib = 0;
    HWND h = ui_main_hwnd();
    RECT wr{}, fb{};
    if (!h || !GetWindowRect(h, &wr)) return;
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &fb, sizeof fb)))
        return;
    if (fb.right <= fb.left || fb.bottom <= fb.top) return;
    int l = fb.left - wr.left, t = fb.top - wr.top;
    int r = wr.right - fb.right, b = wr.bottom - fb.bottom;
    if (l < 0 || t < 0 || r < 0 || b < 0) return;
    if (l > 32 || t > 32 || r > 32 || b > 32) return;   // not a resize border
    *il = l; *it = t; *ir = r; *ib = b;
}

static void app_open_begin() {
    // Same leg guard as app_close_begin -- the WM_HOTKEY comment always
    // claimed "mid-travel toggles are ignored by the begin functions'
    // guards", but only close_begin had one. An open landing mid-choreography
    // would emit APPOPEN into the page's close travel; the page recovers from
    // that today, but ordering violations in this choreography are exactly
    // the class that produced the permanent dead-rectangle wedge
    // (2026-09-02, docs/app-close-flicker.md), so every leg boundary is
    // enforced on both sides now.
    if (g_app_mode || g_app_opening || g_app_closing) return;
    if (g_app_mode || g_app_opening || g_app_closing) return;
    if (!ec::state().pid) {
        ui_log("WARN", "! attach to a client before opening the overlay");
        return;
    }
    if (!ensure_overlay()) {
        ui_log("ERROR", "x overlay unavailable - this machine cannot host it");
        return;
    }
    HWND game = hud_client_cached(ec::state().pid);
    RECT g{};
    if (!game || !GetWindowRect(game, &g)) {
        ui_log("WARN", "! no client window to open over");
        return;
    }
    // THE SYNC, and the whole of it: the window carries the rect it occupies
    // right now into the game, clamped to fit the client. Nothing is read
    // from disk and nothing is remembered separately, so the two modes cannot
    // hold different opinions -- hud mode's opening geometry IS app mode's
    // closing geometry. A maximised window folds to the clamped client fit,
    // which is what "as big as it was" means inside a smaller frame.
    //
    // ONLY IF THE WINDOW HAS MOVED SINCE WE LAST PLACED IT. Closing to the
    // badge leaves it hidden where it stood in the game; re-deriving from that
    // rect against a client that has since moved or changed monitor would drag
    // the console across the screen behind the user's back, so an untouched
    // window keeps the anchor it has and goes on following the game.
    //
    // The test cannot be "is it visible": by the time the badge is clicked the
    // overlay valve has already hidden the window (it hides on attach), so
    // visibility says no for a window the user had just dragged across the
    // desktop -- which is the reported bug, in both its forms (move while
    // detached then attach; move in app mode then switch app-as-HUD on).
    // A MAXIMISED WINDOW CARRIES ITS RESTORED RECT, not the monitor it is
    // filling. Maximised is a state, and the console cannot hold that state
    // inside a client rect -- so the honest reading of "carry what it is now"
    // is the size it had before it went full-screen, which is also the size it
    // gets back when it leaves the game. rcNormalPosition is in WORKSPACE
    // coordinates rather than screen ones; save_window_rect and
    // restore_window_rect have always treated the two as interchangeable and
    // the clamp into the client absorbs the work-area offset, so this does not
    // introduce an assumption the file did not already make.
    RECT wr{};
    bool have = false;
    if (IsZoomed(ui_main_hwnd())) {
        WINDOWPLACEMENT wp{};
        wp.length = sizeof wp;
        if (GetWindowPlacement(ui_main_hwnd(), &wp)) { wr = wp.rcNormalPosition; have = true; }
    } else {
        have = GetWindowRect(ui_main_hwnd(), &wr) != 0;
    }
    bool took = have && app_moved_since_placed(wr);
    if (took) app_anchor_capture(wr, g);
    zlog("APP carry %s from %d,%d %dx%d -> right=%d cy=%d",
         took ? "TAKEN" : "kept", wr.left, wr.top,
         wr.right - wr.left, wr.bottom - wr.top, g_app_right, g_app_cy);

    g_app_mode     = true;
    g_app_opening  = true;
    g_app_phase_at = GetTickCount();

    ShowWindow(ui_main_hwnd(), SW_HIDE);
    // A maximised or snapped desktop window must stop being one before it goes
    // into the game, or every SetWindowPos after this fights Windows for the
    // rect. app_apply_overlay_styles does it, while hidden -- see the note
    // there about why the SetWindowPlacement that used to sit here did not.
    app_apply_overlay_styles(true);
    ui_post("APPWIN:1");     // the page hides its desktop-only chrome

    int l, t, w, h;
    app_target_rect(g.right - g.left, g.bottom - g.top, &l, &t, &w, &h);
    char b[96];
    // The BOX gets the composed rect, the WINDOW still gets the full one at
    // APP_ATRECT -- they are different rectangles for the same app.
    int il, it, ir, ib;
    app_frame_inset(&il, &it, &ir, &ib);
    snprintf(b, sizeof b, "APPOPEN:%d,%d,%d,%d",
             l + il, t + it, w - il - ir, h - it - ib);
    // Click-through for the travel, but POINTER-FOLLOWING (travel_ct_apply):
    // over the badge the overlay stays opaque so the cursor stays ours and
    // visible; everywhere else the corridor is live to the game. An
    // unconditional click-through here hid the cursor for the whole travel
    // (the game's queue governs a point the overlay has declined); NO
    // click-through kept the cursor but left the corridor dead for ~200ms.
    // The per-point rule is both at once, and the same rule serves the close.
    g_travel_ct = true;
    ct_follow_apply();
    zlog("APP open -> %s", b);
    hud_push(b);
}

// The page's travel landed (APP_ATRECT), or the watchdog gave up waiting for
// it: show the real window under the box the overlay is still holding up.
static void app_open_finish() {
    if (!g_app_mode || !g_app_opening) return;
    // g_app_opening stays TRUE until APPSHOWN is pushed, at the bottom. It
    // used to clear here, which opened a window where a close gesture
    // accepted mid-function (the calls below synchronize cross-process and
    // could in principle dispatch the LL hook; probed same-process 0/75
    // nested, but the cross-process case is unproven) would emit APPCLOSE
    // BEFORE APPSHOWN -- and a stale APPSHOWN landing after a close begins
    // is the measured page wedge (permanent dead rectangle; see
    // docs/app-close-flicker.md 2026-09-02). While the flag holds, the hook
    // and the foreground handler refuse the gesture, the click still reaches
    // the game, and the user's intent resolves through the foreground
    // handler once the open settles. Re-entry protection is unchanged: the
    // guard above still refuses while the flag is up, and nothing posted
    // (watchdog, tick, page messages) can run mid-function.
    HWND game = hud_client_cached(ec::state().pid);
    RECT g{};
    if (!game || !GetWindowRect(game, &g)) { app_abort_restore(); return; }
    int l, t, w, h;
    app_target_rect(g.right - g.left, g.bottom - g.top, &l, &t, &w, &h);
    // Opening from a THIRD app brings the client up too -- the console is a
    // HUD over the game, so summoning it from a browser means "back to the
    // game, with the console". That work is NOT here: the hook marks it and
    // overlay_handback does it a tick later, which is the panel's own path
    // and the only one that works (see the comments at both ends). One
    // owner, and it covers the panel target for free.
    // Show, still inactive and without z games -- wherever it lands in the
    // stack, the topmost overlay's box covers the rect, so nothing shows yet.
    //
    // Tried and REVERTED 2026-08-22: showing into HWND_TOPMOST here (the
    // mirror of the close path's raise-before-handback) left the app sitting
    // above the client in cases where it should not, and the flicker it was
    // meant to prevent was never observed on this path anyway -- the box
    // covers the rect until APPSHOWN, and the band rule promotes within a
    // tick when the client is foreground. Place, then let focus and the band
    // rule decide; do not pre-empt them here.
    // A watchdog landing after the show already ran: finish the leg.
    if (g_app_box_covering) { app_open_uncover(); return; }
    SetWindowPos(ui_main_hwnd(), nullptr, g.left + l, g.top + t, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    app_placed(g.left + l, g.top + t, w, h);
    // KEEP THE BOX ABOVE THE APP until WebView2 has painted (Fault A): the
    // hold flag makes raise_overlays take its TOPMOST branch, and the tick's
    // covering branch keeps it there. Nothing else happens here -- no
    // promote, no foreground -- because either one crosses the app above
    // the box, and an unpainted app above the box is the game showing
    // through (measured ~34ms with DXGI duplication before the hold
    // existed). The witness is the APP page's own double-rAF after this
    // show (APPPAINT -> APPPAINTED, the same instrument the close leg's
    // APP_BOXUP uses for the hud page); the rest of the leg runs in
    // app_open_uncover when it lands, or from the tick's belt at 250ms.
    g_app_box_covering = true;
    g_app_cover_at     = GetTickCount();
    raise_overlays();
    zlog("APP shown, waiting for the page's paint");
    ui_post("APPPAINT");
}

// The app page reported a painted frame after the show (APPPAINTED), or the
// belt gave up waiting: put the window in front and drop the box under it.
void app_painted() {
    if (!g_app_opening || !g_app_box_covering) return;   // not waiting: a stray reply
    zlog("APP painted (+%lums)", (unsigned long)(GetTickCount() - g_app_cover_at));
    app_open_uncover();
}
static void app_open_uncover() {
    if (!g_app_mode || !g_app_opening || !g_app_box_covering) return;
    // One compose so the frame the page just committed is on glass before
    // the window crosses above the box (rAF fires on commit, not on
    // present; DwmFlush returns after DWM's next composition pass).
    DwmFlush();
    // FOREGROUND FIRST, WITH THE OVERLAY STILL TOPMOST ABOVE THE APP. The
    // first cut promoted first and glued the overlay under the app before
    // the hand-over, which is right when the promote TAKES -- but on the
    // COLD open (our process has never been foreground) Windows' topmost
    // entry gate swallows the app's TOPMOST (ret=1, no z-change, no ZWATCH
    // line), the glue then dropped the overlay into the NORMAL band under
    // a normal-band app, and force_foreground's activation raise of the
    // client-owned app hoisted the CLIENT over it: one presented frame of
    // the game through the app rect and the badge (DXGI duplication, 8 of
    // 8 cold opens, ~9ms GAH/AGH in the z-walk), and the tick's band rule
    // cleaning up 20ms later. Taking foreground under the hold instead
    // costs nothing visible -- the box covers the painted app, and the
    // owner-group hoist stays in the normal band beneath the topmost
    // overlay -- and makes the promote below run as the FOREGROUND
    // process, which the gate exempts (docs/overlay-plan.md, the topmost
    // fight). Permitted because the click that opened landed on our
    // overlay (or the hotkey was ours); force_foreground carries the
    // documented fallback.
    bool took = force_foreground(ui_main_hwnd());
    g_app_box_covering = false;
    // Into the band it will hold anyway (console_fg in the band rule) --
    // ONE crossing, from under the box to above it, painted -- then the
    // overlay into its slot directly under it. app_swp logs whether the
    // move actually took (bit AND position), so a refused promote is a
    // trace line rather than a guess.
    app_swp(HWND_TOPMOST);
    raise_overlays();
    g_travel_ct = false;           // travel over; the badge takes clicks again
    overlay_clickthrough(false);
    zlog("APP shown fg=%d", (int)took);
    // The box drops BENEATH the window now, so no compose has to be waited
    // for here: the drop changes nothing on screen. (The old DwmFlush pair
    // before this push was the one-frame margin for a drop ABOVE the app.)
    hud_push("APPSHOWN");    // page drops the box and reports the badge rects
    g_app_opening = false;   // cleared LAST -- see the note at the top
}

// CLICK-THROUGH for the duration of a travel. While the box is moving, the
// overlay's hit region is the UNION of the badge and the app's rect -- it has
// to be, because DWM clips composited pixels to the region and the box is
// drawn inside it. But the region is also what takes INPUT, so for the ~200ms
// of the animation a large rectangle sat over the game swallowing clicks
// (user report: "when closing animation i cant do stuff like right click").
// WS_EX_TRANSPARENT separates the two: the pixels stay, hit-testing skips the
// window entirely and everything lands on the game. Nothing in the overlay
// needs clicking mid-travel, and it is restored the moment the motion ends.
// ...and the APP WINDOW needs exactly the same treatment during a close. It
// cannot be hidden the instant the close starts -- the overlay's box has to be
// composed over it first, or the game shows through for a frame -- so for the
// tens of milliseconds between the click and the hide it is still on screen,
// still taking input. Clicking the client to close and immediately
// right-dragging the camera therefore did nothing: the right button landed on
// the app, which was on its way out. Transparent for the duration, so those
// clicks reach the game while the pixels finish their handover.
// WS_EX_LAYERED is NOT optional here. win32k's input dispatch only skips a
// WS_EX_TRANSPARENT window when it is ALSO layered; without the pair, the
// "transparent" overlay still received every button in the travel's union
// region and captured it for the browser -- the close-animation dead region,
// proven from outside: a synthetic right-drag mid-travel left Camera.Rotation
// byte-identical while the wndproc logged "overlay ATE msg=0204", and the
// same drag outside the region (and in the same spot after the travel) moved
// the camera. The hit-test APIs are the trap that hid this: CWP_SKIPTRANSPARENT
// skips a bare-TRANSPARENT window that real routing does not, so every
// ChildWindowFromPointEx probe said "the game owns the point" while the game
// heard nothing. Layered-ness costs the overlay nothing: with
// WS_EX_NOREDIRECTIONBITMAP there is no redirection surface for LAYERED to
// manage (no SetLayeredWindowAttributes needed), and the DComp visuals
// compose exactly as before -- verified by pixel-sampling the travel box
// mid-close against the settled scene.
//
// The APP window stays bare-TRANSPARENT on purpose. Making it layered would
// need SetLayeredWindowAttributes before it renders again, and a blanked
// frame there is exactly the game-flash the close choreography exists to
// prevent; its exposure is only the ~30ms between the close click and the
// hide (measured), which is below a human press-release-press. Every later
// press belongs to the overlay's travel, which the pair above fixes.
static void app_clickthrough(bool on) {
    if (!ui_main_hwnd()) return;
    LONG_PTR ex = GetWindowLongPtrW(ui_main_hwnd(), GWL_EXSTYLE);
    LONG_PTR want = on ? (ex | WS_EX_TRANSPARENT) : (ex & ~(LONG_PTR)WS_EX_TRANSPARENT);
    if (want != ex) SetWindowLongPtrW(ui_main_hwnd(), GWL_EXSTYLE, want);
}

static void overlay_clickthrough(bool on) {
    if (!g_hud_hwnd) return;
    const LONG_PTR both = WS_EX_TRANSPARENT | WS_EX_LAYERED;
    LONG_PTR ex = GetWindowLongPtrW(g_hud_hwnd, GWL_EXSTYLE);
    LONG_PTR want = on ? (ex | both) : (ex & ~both);
    if (want != ex) SetWindowLongPtrW(g_hud_hwnd, GWL_EXSTYLE, want);
}

void app_close_begin(bool hand_to_game) {
    if (!g_app_mode || g_app_opening || g_app_closing) return;
    g_app_box_covering = false;   // no open box to hold above the app any more
    g_app_closing    = true;
    g_app_close_hand = hand_to_game;
    g_app_phase_at   = GetTickCount();
    // No overlay page to animate (rebuild, teardown): skip the choreography
    // and put the world back directly.
    if (!g_hud_composed || !g_hud_hwnd) { app_abort_restore(); return; }
    HWND game = hud_client_cached(ec::state().pid);
    RECT g{}, r{};
    GetWindowRect(ui_main_hwnd(), &r);
    if (game) GetWindowRect(game, &g);
    char b[96];
    // Same correction as the open leg: the box stands over what DWM COMPOSES,
    // not over the window rect, or it is 14px wide and 7px tall of game either
    // side of the app for the whole close. See app_frame_inset.
    int il, it, ir, ib;
    app_frame_inset(&il, &it, &ir, &ib);
    snprintf(b, sizeof b, "APPCLOSE:%ld,%ld,%ld,%ld",
             r.left - g.left + il, r.top - g.top + it,
             (r.right - r.left) - il - ir, (r.bottom - r.top) - it - ib);
    zlog("APP close -> %s (hand=%d)", b, (int)hand_to_game);
    // Pointer-following, not unconditional -- see ct_follow_apply. The
    // corridor must not eat the game's clicks, but an unconditionally
    // transparent overlay hands the CURSOR to the game's queue too, and the
    // game's mouse-lock hid it for the whole visible close (measured
    // CURSOR_SHOWING clear + hCursor NULL, ~260ms). Cleared at APPIDLE.
    g_travel_ct = true;
    ct_follow_apply();
    app_clickthrough(true);       // nor may the app on its way out
    hud_push(b);
    // THE KEYBOARD GOES BACK NOW, not when the travel lands. Closing is a
    // decision already made -- the user pressed Insert, clicked the badge, or
    // clicked the client -- and the ~200ms of box-up, hide and travel that
    // follow are animation, not deliberation. Handing focus over at the END
    // left the game unresponsive for that whole stretch.
    //
    // RAISE OURS BEFORE HANDING FOCUS OVER. Focusing the client raises the
    // client, and on this path the app is NOT in the topmost band to begin
    // with: the band rule is "topmost iff the CLIENT is foreground", and when
    // the badge or Insert closes the app, WE are foreground. So the client's
    // raise crossed above a non-topmost app window and the game showed
    // through for a frame or two before the box covered it -- the flicker the
    // user sees on a badge close and never on a click-into-the-game close,
    // where the hook runs before Windows raises anything and no foreground is
    // forced at all.
    //
    // overlay_handback has carried this same line for the panel since the
    // beginning; app mode just never inherited it.
    //
    // The overlay goes back on top afterwards: the travel box lives there and
    // has to compose OVER the app it is covering. The band returns to normal
    // in app_close_finish, by which point the window is hidden and the demote
    // cannot be seen.
    // BOTH paths: the app into the topmost band and the overlay directly
    // under it (app_in_front keeps raise_overlays on the slot through the
    // leg). The client's activation raise -- forced below, or the click's own
    // on the hook path -- is a normal-band move and crosses neither. The
    // overlay is NOT put above the app: the box only has to be composed
    // before the hide (close_finish's DwmFlush pair), and putting the whole
    // overlay over the still-visible app for those ~50ms was the dock
    // flashing in front on every close (fault 4, 2026-09-04). app_swp is
    // bracketed, so the owned app's band entry restacks nothing.
    // THE OWNER decision, on FOREGROUND -- not the topmost bit, which the
    // entry gate GRANTS WITHOUT APPLYING THE MOVE from a third-app close
    // (measured 2026-09-04: WS_EX_TOPMOST set, no `refused` line, game still
    // over app+overlay ~15ms -- GAH, the user's "app and overlay blink behind
    // Roblox on a badge close from a third app"; the app_ensure_above state).
    //   * WE are foreground (console-fg close): the TOPMOST genuinely takes,
    //     the app holds itself up, and an owned app would only be re-placed by
    //     the client's activation (3-4ms overlay-over-app). DROP the owner --
    //     the console-fg close stays perfectly clean, as it was.
    //   * a THIRD app is foreground: our TOPMOST is swallowed, so the owner is
    //     the only carry that keeps app AND overlay above the client's raise.
    //     KEEP it. The decision is latched for the whole leg (the tick and the
    //     overlay owner block read g_app_close_keep_owner); close_finish drops
    //     it once the window is hidden.
    {
        HWND  fgw   = GetForegroundWindow();
        DWORD fgpid = 0; if (fgw) GetWindowThreadProcessId(fgw, &fgpid);
        bool  we_fg = (fgw == ui_main_hwnd()) ||
                      (fgpid == GetCurrentProcessId()) ||
                      (g_hudc.browser_pid && fgpid == g_hudc.browser_pid);
        g_app_close_keep_owner = !we_fg;
    }
    app_swp(HWND_TOPMOST);
    raise_overlays();
    if (!g_app_close_keep_owner) app_owner_drop();
    else zlog("APP close: third app fg -> keep owner to carry app+overlay above client");
    if (hand_to_game && game) {
        g_hud_preempt_at = GetTickCount();   // the next line IS the activation
        force_foreground(game);
    }
}

// The page's box is composed over the window (APP_BOXUP), or the watchdog
// gave up: hide the window, restore the desktop identity, start the travel.
static void app_close_finish() {
    if (!g_app_mode || !g_app_closing) return;
    g_app_mode    = false;
    g_app_closing = false;

    // NEVER HIDE WHILE WE STILL HOLD FOREGROUND. Windows hands the focus of a
    // window it is hiding to the next one in the Z-ORDER, and that is whatever
    // you were in before -- a browser, a terminal -- not the client underneath.
    //
    // The click-on-client close path passes hand_to_game=false on purpose: the
    // click is itself an activation, so handing over as well would be
    // redundant. But the hide happens later, when the page's box has composed,
    // and if the client's activation has not landed by then we are still the
    // foreground window and Windows picks the third app. That is the
    // intermittent "closing the app focused my terminal instead of Roblox" --
    // intermittent because it is a race with the click, and the click usually
    // wins.
    //
    // Costs nothing in the common case: if the click already moved foreground
    // to the client, this test is false and nothing happens.
    if (GetForegroundWindow() == ui_main_hwnd()) {
        HWND game = hud_client_cached(ec::state().pid);
        if (game) {
            zlog("APP close: still foreground at hide -> hand to client");
            force_foreground(game);
        }
    }

    // TWO COMPOSITION PASSES BEFORE THE HIDE, the open path's own pattern
    // (app_open_finish). APP_BOXUP proves the page's double-rAF ran, i.e. the
    // box frame was SUBMITTED to Chromium's compositor -- not that DWM has
    // PRESENTED it, and under GPU load those are tens of ms apart. Measured
    // 2026-09-02 in a mimic harness (composed WebView2 overlay, flip-model
    // fullscreen "game", loaded renderer+GPU, DXGI output duplication
    // stamping every presented frame): hiding on BOXUP alone put BARE GAME
    // on glass for up to ~35ms in 8/40 closes -- the intermittent close
    // flicker exactly, incl. its immunity to the double-rAF (that only moves
    // submission). With these two flushes: 0/40 under the same load. The
    // reproduction needed load AND fullscreen-sized surfaces; idle runs were
    // 0/30 either way, which is why the flash never showed on a bare desktop.
    // This is measured slack, not a presentation proof -- no public API can
    // prove a foreign process's commit is on glass.
    //
    // AND IT WAS EXCEEDED LIVE (2026-09-07): with the iGPU -- DWM's and the
    // WebView2 GPU process's adapter on this machine -- 90% busy, these two
    // passes spanned 31-78ms while the box frame reached glass ~140ms after
    // APP_BOXUP, and 4 of 9 closes presented bare game in the app rect
    // between the hide and the box (DXGI duplication, whole 7x4 grid). Six
    // passes fixed it at 141-234ms of blocking per close and blind to
    // Chromium; the page now sends APP_BOXUP off an Element Timing entry
    // for the box (hud.html startAppClose) -- Chromium's presentation
    // feedback, i.e. its COMMIT of the frame carrying the box, which covers
    // the renderer/GPU-process tail a rAF count cannot -- and these two stay
    // as the slack on DWM's side. Residual under that load 3/36: DWM keeps
    // composing the overlay's old surface until the new one's GPU work
    // completes, and nothing observable from here sees that fence. Do not
    // delete these; if the tail matters, the fix is structural (see the
    // page comment), not a longer wait.
    //
    // SIX, NOT TWO, AND THAT IS THE USER'S CALL (2026-09-07). Six passes were
    // measured at zero above and rejected here for costing 141-234ms of
    // blocking per close -- a judgement about FEEL, not about correctness. The
    // user overruled it, and their reason is better than mine: focus has
    // already gone to the game by this point (app_close_begin hands it over),
    // so the wait is spent on a window nobody is looking at or typing into.
    // A slower close nobody perceives beats a rare flash they do.
    //
    // This is also exactly what the note above prescribes -- widen the passes,
    // do not delete them. It does NOT close the 3/36 residual, which is a GPU
    // fence no wait can see; that is still the pre-composed cover in
    // docs/app-cover-precomposed.md. This just buys most of the tail cheaply.
    //
    // If the close ever feels sluggish, this number is the dial.
    for (int i = 0; i < 6; i++) DwmFlush();

    ShowWindow(ui_main_hwnd(), SW_HIDE);
    app_clickthrough(false);   // hidden now; it takes input again when reopened
    // Out of the topmost band the leg put it in (app_close_begin's
    // app_swp(TOPMOST), or the band rule's). Hidden now, so the demote is
    // invisible. (A hook-time TOPMOST "close-gesture cover" was once granted
    // in hud_mouse_ll and is gone -- the band rule made it redundant; the old
    // comment here still named it.)
    SetWindowPos(ui_main_hwnd(), HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    app_apply_overlay_styles(false);
    ui_post("APPWIN:0");
    // The desktop SHOW STATE back, and only that. In overlay mode the window
    // stays hidden -- the badge is the way back in; the tick's valves cover
    // every failure path. Otherwise it returns where it was: activated when
    // the user is staying in the console (the Settings switch-off path),
    // inactive when the game is about to take focus.
    UINT cmd = g_overlay_pref  ? SW_HIDE
             : g_app_close_hand ? SW_SHOWNOACTIVATE
                                : SW_SHOW;
    app_restore_show(cmd);
    // Focus already went to the game at close_begin -- see the note there.
    // Repeating it here would re-steal foreground from wherever the user has
    // moved in the meantime.
    zlog("APP closed");
    hud_push("APPGO");       // page travels the box home
}

// Every non-animated way out: overlay dying under an open app (detach, client
// exit, teardown), the band fight's desktop-window valve, WM_CLOSE. Restores
// the desktop identity without the page's help and shows the window unless
// overlay mode says the badge owns re-entry -- EXCEPT that with the overlay
// gone there is no badge, so the window always comes back here.
void app_abort_restore() {
    if (!g_app_mode && !g_app_opening && !g_app_closing) return;
    g_app_mode = false; g_app_opening = false; g_app_closing = false;
    g_app_box_covering = false;
    // DISOWN HERE, not on the next tick: this is the path where the client is
    // going away or the overlay is being torn down, so there may be no next
    // tick -- and a desktop window still owned by the game would minimise and
    // restore with it. The tick's rule agrees; this is only about being early.
    if (ui_main_hwnd()) SetWindowLongPtrW(ui_main_hwnd(), GWLP_HWNDPARENT, 0);
    // BOTH windows are owned by the CLIENT, never by each other (the app for
    // its whole visible life, the overlay while demoted or mid-close-carry --
    // the tick's two owner blocks, both to `game`). Corrected 2026-09-04: the
    // old comment here claimed the overlay was owned by the APP and that the
    // order mattered; it never was and it does not. Clear both anyway, now
    // rather than next tick, because this path (client gone, overlay torn
    // down) may have no next tick, and a window left owned by a client that
    // is going away minimises and restores with it. Order is immaterial.
    if (g_hud_hwnd) SetWindowLongPtrW(g_hud_hwnd, GWLP_HWNDPARENT, 0);
    // No travel will finish to clear it, so clear it here.
    g_travel_ct = false;
    overlay_clickthrough(false);
    app_clickthrough(false);
    if (!ui_main_hwnd()) return;
    ShowWindow(ui_main_hwnd(), SW_HIDE);
    // Out of the topmost band, whoever put it there. Same correction as
    // app_close_finish: this named a hook-time "close-gesture cover" granted
    // in hud_mouse_ll, and that has not existed since the band rule replaced
    // it. Hidden already, so the demote is invisible.
    SetWindowPos(ui_main_hwnd(), HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    app_apply_overlay_styles(false);
    ui_post("APPWIN:0");
    app_restore_show(SW_SHOWNOACTIVATE);
}

// The modal move/size loop ended on the app window: fold the new position
// into the anchor (right inset + centre Y, client-relative), clamp it back
// inside the client if the drag left it hanging out, and persist -- this is
// the one write the position memory gets, matching the first design's
// save-on-drag-end. A resize (the native edges still work) re-derives the
// anchor too; the SIZE is deliberately not remembered -- the next open
// recomputes the clamped 1180x780, as the first design always did.
void app_drag_settled() {
    if (!g_app_mode || g_app_opening || g_app_closing) return;
    HWND game = hud_client_cached(ec::state().pid);
    RECT g{}, r{};
    if (!game || !GetWindowRect(game, &g) || !GetWindowRect(ui_main_hwnd(), &r)) return;
    int cw = g.right - g.left;
    // Same converter as the open, so a drag and an open cannot disagree about
    // what a screen rect means. It clamps the size AND the anchor.
    app_anchor_capture(r, g);
    if (!g_app_w || !g_app_h) return;   // degenerate rect; capture declined it
    int w = g_app_w, h = g_app_h, half = h / 2;
    // Snap back only if the clamp actually moved the window (dragged half out
    // of the client, or resized past what the client can hold).
    int nx = g.left + (cw - g_app_right - w);
    int ny = g.top  + (g_app_cy - half);
    if (nx != r.left || ny != r.top || w != r.right - r.left || h != r.bottom - r.top) {
        SetWindowPos(ui_main_hwnd(), nullptr, nx, ny, w, h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        r = RECT{ nx, ny, nx + w, ny + h };
    }
    // The drag ends where the user left it, clamp included -- so the next open
    // sees an UNMOVED window and keeps this anchor.
    app_placed(r.left, r.top, w, h);
    // THE ONE STORE, from the rect the window actually ended at. Its position
    // here is a screen rectangle exactly as the desktop window's is -- same
    // window, same coordinates -- so there is nothing to convert and nothing
    // to keep in step.
    ui_geometry_changed(r.left, r.top, w, h);
    zlog("APP settled right=%d cy=%d rect=%d,%d %dx%d",
         g_app_right, g_app_cy, r.left, r.top, w, h);
}

// Feed. Always through dispatch: callers are on whichever thread logged or
// changed state, and WebView2 only accepts calls on the UI thread.
void hud_push(const std::string& msg) {
    if (!ui_page_live()) return;
    // The overlay serves a docked snippet panel, or app mode, with the console
    // panel switched off, so the gate is "anything overlaid", not the setting.
    if (!overlay_wanted()) return;

    // STATE, RUNSTATE and SNIPS are SNAPSHOTS, and the watcher re-sends them
    // every time it looks -- several times a second, almost always identical.
    // Each one re-rendered the list, which is why a group folded shut sprang
    // open again half a second later, and why the snippet view lost its scroll
    // position while you were reading it.
    //
    // Dropping the unchanged ones fixes that at the source. Deliberately NOT
    // applied to LOGS: two identical lines are two events, and collapsing them
    // would hide repetition that matters.
    size_t colon = msg.find(':');
    if (colon != std::string::npos) {
        std::string type = msg.substr(0, colon);
        if (type == "STATE" || type == "RUNSTATE" || type == "SNIPS" ||
            type == "HEALTH" || type == "DOCKP" || type == "HUDVIS") {
            // Caller's thread, by design (the dispatch below is the hop) --
            // so the dedup map needs its lock. See its declaration.
            std::lock_guard<std::mutex> lk(g_hud_last_mux);
            auto it = g_hud_last.find(type);
            if (it != g_hud_last.end() && it->second == msg) return;
            g_hud_last[type] = msg;
        }
    }
    std::string m = msg;
    ui_dispatch([m]() {
        std::string js = "window.__onHudMsg&&window.__onHudMsg(\"" + json_escape(m) + "\")";
        if (g_hud_composed && g_hudc.core)
            g_hudc.core->ExecuteScript(widen(js).c_str(), nullptr);
        else if (g_hud_wv)
            g_hud_wv->eval(js);
    });
}

// ---- host doors -------------------------------------------------------------
// The few places OUTSIDE this TU that read or flip overlay state: the WIN:
// handlers and the frameless-window plumbing branch on app mode,
// reveal_window's startup gate consults the standing preference, and the
// settings parser and WinMain hand their transitions over. Functions rather
// than setters on raw flags, so each transition keeps one owner (the same
// rule a previous round applied to g_snip_docked).

bool overlay_app_mode() { return g_app_mode; }
bool overlay_pref()     { return g_overlay_pref; }

// WinMain's startup load of the preference file (dpi.cpp owns the file; the
// value lives here).
void overlay_load_pref() { load_overlay_pref(g_overlay_pref); }

// Settings -> Console -> overlay mode. A standing preference, not an action:
// it says where the console LIVES while a client is attached. Turning it ON
// is left to the tick, which moves the UI in once the overlay exists.
// Turning it OFF must bring the desktop window back IMMEDIATELY -- the
// switch the user just clicked is inside the thing that is about to
// disappear, and a mode with no way out is not a mode.
void overlay_pref_set(bool want) {
    if (want != g_overlay_pref) {
        g_overlay_pref = want;
        save_overlay_pref(g_overlay_pref);
        push_overlay_pref();
        if (!want) {
            // hand_to_game=false: with the pref now off the
            // close lands the window back on the desktop,
            // shown and focused -- the user is mid-Settings.
            if (g_app_mode) app_close_begin(false);
            else if (ui_main_hwnd()) ShowWindow(ui_main_hwnd(), SW_SHOWNOACTIVATE);
        }
    }
}

// Settings -> Console -> in-game panel.
void overlay_hud_set(bool want) {
    if (want != g_hud_enabled) {
        g_hud_enabled = want;
        // Off takes effect now rather than at the next tick, so
        // the panel disappears when the switch is flipped
        // instead of a frame later. On is left to the tick,
        // which creates it in the right place already.
        //
        // With a docked snippet panel up, the WINDOW stays --
        // panel and dock are elements of ONE overlay window
        // now, and the dock still needs it -- so only the
        // console's element hides (HUDVIS) and the page drops
        // its rects from the hit region.
        push_hudvis();
        if (!want && !snip_docked()) hud_destroy();
    }
}
