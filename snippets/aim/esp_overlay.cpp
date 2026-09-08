// esp_overlay.cpp â€” Live ESP through the console's overlay pipe.
//
// The console's WebView2 window transforms into a transparent, always-on-top
// full-monitor pane on fx_overlay_on(). This snippet feeds it: 10 Hz, walk
// Players, project each head to screen, hand the list to the page as JSON.
//
// Because rendering is HTML+SVG in the console window (not in Roblox's
// process), we sidestep the reactive-GUI wall that killed the previous
// esp.cpp attempts. Nothing in the game's memory is touched -- this is pure
// read + host-side render.
//
//   Insert  toggle overlay on / off. Enable also brings the console to the
//           foreground so it can grab topmost.
//   Stop    turn overlay off and end.

#include <cmath>
#include <cstring>
#include <cstdio>
#include <windows.h>

// The draw rate. This snippet is ESP only, so it IS the loop rate: every tick
// walks the players and projects them purely to redraw boxes.
const unsigned ESP_HZ = 60;

const float MAX_DIST = 800.0f;
const float BOX_MIN_H = 60.0f;
const float BOX_ASPECT = 0.6f;      // width = height * this

// ---- read helpers -----------------------------------------------------------

static float3 read_v3(uint64_t addr) {
    float3 v = { 0, 0, 0 };
    fx_rdblk(addr, &v, 12);
    return v;
}

static float read_f(uint64_t addr) {
    float v = 0;
    fx_rdblk(addr, &v, 4);
    return v;
}

// ---- camera + projection (same math as aimbot's view.cpp) ------------------

static uint64_t find_camera() {
    uint64_t ws = fx_service("Workspace");
    if (!ws) return 0;
    uint64_t n = fx_nchildren(ws);
    for (uint64_t i = 0; i < n; i++) {
        uint64_t c = fx_child(ws, (int)i);
        if (c && strcmp(fx_class(c), "Camera") == 0) return c;
    }
    return 0;
}

struct Cam { float R[9]; float3 pos; float fov; float vp[2]; bool ok; };

static Cam read_cam() {
    Cam c = {};
    uint64_t cam = find_camera();
    if (!cam) return c;
    uint64_t oR = fx_off("Camera.Rotation");
    uint64_t oP = fx_off("Camera.Position");
    uint64_t oF = fx_off("Camera.FieldOfView");
    uint64_t oV = fx_off("Camera.ViewportSize");
    if (oR == (uint64_t)-1 || oP == (uint64_t)-1 ||
        oF == (uint64_t)-1 || oV == (uint64_t)-1) return c;
    if (fx_rdblk(cam + oR, c.R, 36) != 36) return c;
    c.pos = read_v3(cam + oP);
    c.fov = read_f(cam + oF);
    if (fx_rdblk(cam + oV, c.vp, 8) != 8) return c;
    c.ok = (c.vp[0] >= 1.0f && c.vp[1] >= 1.0f);
    return c;
}

// Find the attached Roblox client's window RECT in monitor coordinates. This
// is critical: Camera.ViewportSize tells us the render surface dimensions,
// but NOT where that surface sits on the monitor. The overlay window is
// full-screen at monitor coordinates, so ignoring the Roblox window position
// draws every marker offset by the window's title bar / left margin -- a
// small constant that DISAPPEARS at close range (big characters) and
// DOMINATES at far range (tiny characters). Exactly the "unnoticeable close,
// very noticeable far" pattern the last version showed.
//
// Multi-instance-safe: EnumWindows filtered by fx_pid picks the specific
// Roblox we are attached to, not "any Roblox".
struct RbxWin { int x, y, w, h; };

static BOOL CALLBACK rbx_win_probe(HWND h, LPARAM lp) {
    struct Ctx { DWORD pid; HWND hwnd; int area; } *ctx = (Ctx*)lp;
    if (!IsWindowVisible(h)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != ctx->pid) return TRUE;
    RECT r;
    if (!GetWindowRect(h, &r)) return TRUE;
    int a = (r.right - r.left) * (r.bottom - r.top);
    if (a > ctx->area) { ctx->area = a; ctx->hwnd = h; }
    return TRUE;
}

static bool get_rbx_window(RbxWin* out) {
    DWORD pid = (DWORD)fx_pid();
    if (!pid) return false;
    struct { DWORD pid; HWND hwnd; int area; } ctx = { pid, nullptr, 0 };
    EnumWindows(rbx_win_probe, (LPARAM)&ctx);
    if (!ctx.hwnd) return false;
    RECT client;
    if (!GetClientRect(ctx.hwnd, &client)) return false;
    POINT tl = { client.left, client.top };
    if (!ClientToScreen(ctx.hwnd, &tl)) return false;
    out->x = tl.x;
    out->y = tl.y;
    out->w = client.right - client.left;
    out->h = client.bottom - client.top;
    return out->w > 0 && out->h > 0;
}

// The view matrix read and the perspective divide are fx_screen now. The reason
// they were here still holds and is worth keeping written down: Roblox's OWN
// world-to-clip matrix beats rebuilding a projection from Camera.Rotation and
// FOV, because a game doing anything unusual to its camera pipeline is already
// reflected in the matrix it actually renders with. A reconstructed projection
// was wrong for this game and put every marker in the wrong place.
//
// fx_screen returns CLIENT-AREA pixels and rejects anything behind the camera,
// clipped, or off the edge. The overlay wants MONITOR pixels, so the window
// origin is still added below.

// ---- target enumeration ----------------------------------------------------

struct Target {
    char  name[48];
    float sx, sy;     // top-left of the box in viewport pixels
    float w, h;
    float dist;
};

static bool part_pos(uint64_t part, float3* out) {
    uint64_t oPrim = fx_off("BasePart.Primitive");
    uint64_t oPos  = fx_off("Primitive.Position");
    if (oPrim == (uint64_t)-1 || oPos == (uint64_t)-1) return false;
    uint64_t prim = fx_rd(part + oPrim);
    if (prim < 0x10000 || prim > 0x7ff000000000ULL) return false;
    float3 p = read_v3(prim + oPos);
    if (!(p.x == p.x)) return false;
    *out = p;
    return true;
}

static int collect_targets(const Cam& c, Target* out, int max) {
    // fx_players: one walk over Players with each name and Character already
    // resolved and memoised, in place of the hand walk that used to be here.
    // It filters NOTHING -- unspawned players arrive with a zero model, and
    // every skip below is still ours to make.
    fx_player_info pl[64];
    int np = fx_players(pl, 64);
    if (np <= 0) return 0;

    // Roblox window rect, still needed for its ORIGIN: fx_screen gives
    // client-area pixels and the overlay draws in monitor pixels.
    RbxWin rw;
    if (!get_rbx_window(&rw)) return 0;

    int n = 0;
    for (int i = 0; i < np && n < max; i++) {
        if (pl[i].is_local) continue;
        uint64_t chr = pl[i].model;
        if (!chr) continue;

        // HRP is the anchor. The Python reference (player_lock.py) proves
        // this is the correct choice -- their aimbot / ESP both use HRP
        // with the same view-matrix projection and their markers land on
        // the visible character. All our prior "HRP is above the body"
        // observations were caused by using our reconstructed projection,
        // not by HRP being in the wrong place.
        //
        // pl[i].root IS fx_part(chr, "HumanoidRootPart") -- checked 15/15 on a
        // live server -- so the anchor and its fallbacks are unchanged.
        uint64_t hrp = pl[i].root;
        if (!hrp) hrp = fx_part(chr, "Torso");
        if (!hrp) hrp = fx_part(chr, "UpperTorso");
        if (!hrp) continue;

        float3 p;
        if (!part_pos(hrp, &p)) continue;
        float dx = p.x - c.pos.x, dy = p.y - c.pos.y, dz = p.z - c.pos.z;
        float dist = sqrtf(dx*dx + dy*dy + dz*dz);
        if (dist > MAX_DIST) continue;

        // HRP into client-area pixels.
        float sx, sy;
        if (fx_screen(p, &sx, &sy) != 1) continue;   // drawn only if visible

        // Same for size anchors 3 studs above and below HRP. These can fall
        // OUTSIDE the frame for someone standing at the edge while the anchor
        // itself is visible, and fx_screen rejects off-frame points -- so a
        // failure here means "cannot size the box", not "not a target". Skipping
        // the whole entry is what the old code did too, via the same early-out.
        float3 top = { p.x, p.y + 3.0f, p.z };
        float3 bot = { p.x, p.y - 3.0f, p.z };
        float top_sx, top_sy, bot_sx, bot_sy;
        if (fx_screen(top, &top_sx, &top_sy) == 0) continue;
        if (fx_screen(bot, &bot_sx, &bot_sy) == 0) continue;

        float h = fabsf(bot_sy - top_sy);
        if (h < 6.0f) h = 6.0f;
        float w = h * BOX_ASPECT;

        // Convert Roblox-window pixels to monitor pixels. Without this offset
        // the overlay draws every marker sitting on the monitor's top-left,
        // which is the "always above the character" bug.
        snprintf(out[n].name, sizeof out[n].name, "%s", pl[i].name);
        out[n].sx   = (sx - w * 0.5f) + rw.x;
        out[n].sy   = (sy - h * 0.5f) + rw.y;
        out[n].w    = w;
        out[n].h    = h;
        out[n].dist = dist;
        n++;
    }
    return n;
}

// ---- JSON serialisation ----------------------------------------------------
//
// The overlay pipe eats a JSON string; the page parses and renders. Keep the
// payload short (~50 chars per target) so a full lobby fits in a few KB.

static void json_escape(const char* in, char* out, size_t max) {
    size_t oi = 0;
    for (size_t i = 0; in[i] && oi + 2 < max; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') { out[oi++] = '\\'; out[oi++] = (char)c; }
        else if (c < 0x20)        { out[oi++] = '?'; }        // skip control chars
        else                       { out[oi++] = (char)c; }
    }
    out[oi] = 0;
}

static void serialise(const Target* t, int n, char* out, size_t max) {
    size_t o = 0;
    o += snprintf(out + o, max - o, "[");
    for (int i = 0; i < n && o + 128 < max; i++) {
        char safe[64];
        json_escape(t[i].name, safe, sizeof safe);
        o += snprintf(out + o, max - o,
                      "%s{\"n\":\"%s\",\"sx\":%.1f,\"sy\":%.1f,\"w\":%.1f,\"h\":%.1f,\"d\":%.1f}",
                      i ? "," : "", safe,
                      t[i].sx, t[i].sy, t[i].w, t[i].h, t[i].dist);
    }
    snprintf(out + o, max - o, "]");
}

extern "C" void fx_run() {
    fx_log("esp overlay: Insert toggles, Stop ends");
    // Claimed only while the gate is open -- see the loop. A claim is global,
    // so holding it unconditionally would swallow Insert in every other
    // application for as long as this snippet ran.
    bool claimed = false;

    bool overlay_on = false;
    // Starts true so the first frame after enabling draws without waiting for a
    // focus transition that already happened.
    bool was_focused = true;
    unsigned dm = fx_dm_gen();

    char json[8192];

    while (fx_running()) {
        // Before the early-outs below, or the claim would never be released
        // while the world was reloading or the overlay was hidden. Toggled only
        // on change: one call per transition, not one per frame.
        {
            bool want = fx_input_ok();
            if (want != claimed) { fx_claim_key(VK_INSERT, want); claimed = want; }
        }

        if (fx_dm_gen() != dm) {
            dm = fx_dm_gen();
            if (overlay_on) fx_overlay_post("[]");   // clear on world reload
            if (!fx_sleep(500)) break;
            continue;
        }

        // Same gate as the claim, so the key is acted on exactly when it is
        // swallowed. Without this the two disagree the moment focus is lost:
        // Insert would reach the app you alt-tabbed to AND toggle the overlay
        // behind it. fx_key_pressed first -- it consumes a press per call.
        if (fx_key_pressed(VK_INSERT) && fx_input_ok()) {
            overlay_on = !overlay_on;
            if (overlay_on) { fx_overlay_on();  fx_log("overlay: on"); }
            else            { fx_overlay_off(); fx_log("overlay: off"); }
        }

        if (overlay_on) {
            // Alt-tabbed away: draw nothing. The boxes are positioned in
            // MONITOR pixels, so with the game behind another window they would
            // otherwise keep floating over whatever is now in front of it.
            //
            // Posted once on the way out rather than every frame, and the whole
            // player walk is skipped while away -- that walk is the expensive
            // part of this loop and none of it is worth doing for a hidden
            // overlay. fx_overlay_off() is deliberately NOT used: that is the
            // mode toggle Insert owns, and flipping it here would fight it.
            if (!fx_focused()) {
                if (was_focused) { fx_overlay_post("[]"); was_focused = false; }
                if (!fx_sleep(100)) break;      // idle poll; nothing to draw
                continue;
            }
            was_focused = true;

            Cam c = read_cam();
            if (c.ok) {
                Target t[24];
                int n = collect_targets(c, t, 24);
                serialise(t, n, json, sizeof json);
                fx_overlay_post(json);
            }
            // fx_pace, not fx_sleep(16): collect_targets walks every player
            // and projects each one, which costs real milliseconds, and
            // sleeping 16 ON TOP of that ran this at ~47 Hz while the comment
            // claimed 60. Pacing takes the work out of the wait.
            //
            // 60, NOT 144. There is no aim in this snippet -- every tick
            // exists to redraw boxes, and nothing on screen benefits from
            // them being rebuilt faster than the display shows them. The
            // walk-and-project is the cost, so halving the rate halves the
            // reads too, not just the drawing. Measured against the same
            // work in cursor_aim: a full tick is milliseconds, so 144 Hz of
            // it was most of a core spent on frames nobody sees. The
            // standalone aimbot has always used 60 for its ESP for exactly
            // this reason.
            if (!fx_pace(ESP_HZ)) break;
        } else {
            if (!fx_wait_key(1)) break;
        }
    }

    if (overlay_on) fx_overlay_off();
    fx_log("esp overlay: stopped");
}