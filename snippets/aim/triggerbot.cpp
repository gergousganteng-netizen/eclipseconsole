// Trigger bot -- TESTING BUILD. It decides, it does not click.
//
// The finished version fires when a target is already under your crosshair.
// This one does everything up to that point and LOGS what it would have done,
// so the decision can be checked against what you actually see before any
// input is synthesised. That split is the same one fx_wrblk uses: a dry run
// that reports the intended action is how you find out the decision is wrong
// without the action being taken.
//
// WHAT IT DECIDES, in order -- all four must hold:
//   1. the pointer is inside a player's ESP box   (same box esp_overlay draws,
//      so what you see is what this tests)
//   2. they are not a teammate
//   3. the line from the camera to them is CLEAR  (fx_visible)
//   4. they are within MAX_DIST
//
// The pointer IS the crosshair in both modes: locked (first person or shift
// lock) Roblox pins the cursor to the middle of the client area, which is
// where the crosshair is, so reading the pointer gives the right answer
// without asking which mode is in force. Same reasoning as cursor_aim.cpp.
//
// LOGS ARE EDGE-TRIGGERED. A state printed every tick at 60 Hz is not a log,
// it is a denial of service against the panel -- and the thing you want to see
// is the moment the decision changes, not that it is still true. One line when
// the pointer lands on someone, one when it leaves.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <windows.h>

// ---- dials ------------------------------------------------------------------

// Hold to arm the decision. 0 means "always on", which is what you want while
// watching the log; the firing version will want a real key so it cannot go
// off while you are walking around.
const int HOLD_KEY = 0;              // e.g. VK_XBUTTON2, or 0 for always

const bool  TEAM_CHECK = true;
const float MAX_DIST   = 800.0f;     // studs

// The wall check. Off, this becomes a pure "is the pointer on a player" test,
// which is worth trying once to see how often geometry is the thing rejecting.
const bool CHECK_LOS = true;

// Box shape, identical to esp_overlay.cpp and cursor_aim.cpp so all three
// agree about where a player IS on screen. A trigger bot that disagreed with
// the boxes you can see would be untestable by eye.
const float BOX_SPAN_STUDS = 3.0f;   // +-, vertically
const float BOX_ASPECT     = 0.6f;   // width = height * this
const float BOX_MIN_H      = 6.0f;

// Poll rate. 60 while only watching; the firing version wants more, since the
// window where a target is under the crosshair can be short.
const unsigned POLL_HZ = 60;

// ---- reads ------------------------------------------------------------------

static float3 read_v3(uint64_t addr) {
    float3 v = { 0, 0, 0 };
    fx_rdblk(addr, &v, 12);
    return v;
}

static bool part_pos(uint64_t part, float3* out) {
    uint64_t oPrim = fx_off("BasePart.Primitive");
    uint64_t oPos  = fx_off("Primitive.Position");
    if (oPrim == (uint64_t)-1 || oPos == (uint64_t)-1) return false;
    uint64_t prim = fx_rd(part + oPrim);
    // A part caught mid-rebuild hands back whatever was in those bytes. This
    // range check is what stops that becoming a target 47000 studs away.
    if (prim < 0x10000 || prim > 0x7ff000000000ULL) return false;
    float3 p = read_v3(prim + oPos);
    if (!(p.x == p.x)) return false;              // NaN
    *out = p;
    return true;
}

static bool project_world(float3 w, float* sx, float* sy) {
    return fx_screen(w, sx, sy) != 0;
}

// ---- the Roblox window ------------------------------------------------------
// Projection gives pixels inside the CLIENT AREA; the pointer is in MONITOR
// pixels. Without the client-area origin every box sits offset by the border
// and title bar -- an error that hides at close range and dominates at
// distance.

struct RbxWin { int x, y, w, h; };

static BOOL CALLBACK rbx_probe(HWND h, LPARAM lp) {
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

// Filtered by fx_pid, so with two clients open this finds the one we attached
// to rather than whichever Roblox window happens to be first.
static bool get_rbx_window(RbxWin* out) {
    DWORD pid = (DWORD)fx_pid();
    if (!pid) return false;
    struct { DWORD pid; HWND hwnd; int area; } ctx = { pid, nullptr, 0 };
    EnumWindows(rbx_probe, (LPARAM)&ctx);
    if (!ctx.hwnd) return false;
    RECT cl;
    if (!GetClientRect(ctx.hwnd, &cl)) return false;
    POINT tl = { cl.left, cl.top };
    if (!ClientToScreen(ctx.hwnd, &tl)) return false;
    out->x = tl.x; out->y = tl.y;
    out->w = cl.right - cl.left;
    out->h = cl.bottom - cl.top;
    return out->w > 0 && out->h > 0;
}

// ---- team check --------------------------------------------------------------
// Both Team pointers come from the roster now, so this reads nothing at all --
// it used to cost two reads per player per call.
//
// DISABLED when our own Team is null: in a game with no teams every Player.Team
// is 0, so a naive equality test makes everyone a teammate and nothing is ever
// a target.
static bool same_team(uint64_t mine, uint64_t theirs) {
    if (!TEAM_CHECK || !mine) return false;   // no teams in this game
    return theirs == mine;
}

// ---- the camera --------------------------------------------------------------

static float3 camera_eye() {
    float3 eye = { 0, 0, 0 };
    uint64_t ws = fx_service("Workspace");
    uint64_t cam = 0;
    if (ws) {
        uint64_t n = fx_nchildren(ws);
        for (uint64_t i = 0; i < n && !cam; i++) {
            uint64_t c = fx_child(ws, (int)i);
            if (c && strcmp(fx_class(c), "Camera") == 0) cam = c;
        }
    }
    uint64_t oP = fx_off("Camera.Position");
    if (cam && oP != (uint64_t)-1) eye = read_v3(cam + oP);
    return eye;
}

// ---- the decision ------------------------------------------------------------

struct Hit {
    uint64_t player;
    char  name[48];
    float dist;
    bool  clear;       // line of sight
};

// Who is under the pointer? Returns the NEAREST such player, since two boxes
// can overlap and the closer one is the one you are looking at.
static bool under_pointer(POINT cur, const RbxWin& rw, Hit* out) {
    // fx_players: one walk over Players with the name, Character and Team
    // already resolved and memoised. Filters nothing -- every skip below is
    // still ours.
    fx_player_info pl[64];
    int np = fx_players(pl, 64);
    if (np <= 0) return false;

    // Our own Team, taken from our own roster entry rather than re-resolving
    // LocalPlayer.
    uint64_t my_team = 0;
    for (int i = 0; i < np; i++) if (pl[i].is_local) my_team = pl[i].team;

    float3 eye = camera_eye();

    bool  found = false;
    float best  = 1e9f;

    for (int i = 0; i < np; i++) {
        if (pl[i].is_local) continue;
        if (same_team(my_team, pl[i].team)) continue;

        uint64_t chr = pl[i].model;
        if (!chr) continue;                       // not spawned

        // pl[i].root IS fx_part(chr, "HumanoidRootPart"), checked 15/15 live.
        uint64_t root = pl[i].root;
        if (!root) root = fx_part(chr, "Torso");
        if (!root) root = fx_part(chr, "UpperTorso");
        if (!root) continue;

        float3 rp;
        if (!part_pos(root, &rp)) continue;

        float dx = rp.x - eye.x, dy = rp.y - eye.y, dz = rp.z - eye.z;
        float dist = sqrtf(dx*dx + dy*dy + dz*dz);
        if (dist > MAX_DIST) continue;

        // The same three projections esp_overlay uses to size a box: centre,
        // and +-BOX_SPAN_STUDS vertically. Off-screen fails the whole target,
        // which is correct here -- something you cannot see is not under your
        // crosshair.
        float bsx, bsy, tsx, tsy, fsx, fsy;
        float3 top = { rp.x, rp.y + BOX_SPAN_STUDS, rp.z };
        float3 bot = { rp.x, rp.y - BOX_SPAN_STUDS, rp.z };
        if (!project_world(rp,  &bsx, &bsy)) continue;
        if (!project_world(top, &tsx, &tsy)) continue;
        if (!project_world(bot, &fsx, &fsy)) continue;

        float bh = fabsf(fsy - tsy);
        if (bh < BOX_MIN_H) bh = BOX_MIN_H;
        float bw = bh * BOX_ASPECT;
        float bx = bsx - bw * 0.5f + rw.x;
        float by = bsy - bh * 0.5f + rw.y;

        if ((float)cur.x < bx || (float)cur.x > bx + bw) continue;
        if ((float)cur.y < by || (float)cur.y > by + bh) continue;

        if (dist >= best) continue;               // a nearer one already won

        // The wall check LAST, because it is the expensive one and everything
        // above rejects far more targets than it does.
        //
        // fx_visible FAILS OPEN -- no geometry cache, no offsets or a
        // degenerate segment all answer "clear". For a trigger bot that is the
        // right way round: the alternative is a filter that silently never
        // fires and looks like the whole snippet is broken. Reported either
        // way below, so a run that is always "clear" is visible as suspicious
        // rather than passing quietly.
        bool clear = CHECK_LOS ? (fx_visible(eye, rp) != 0) : true;

        best = dist;
        found = true;
        out->player = pl[i].player;
        out->dist   = dist;
        out->clear  = clear;
        snprintf(out->name, sizeof out->name, "%s", pl[i].name);
        if (!out->name[0]) snprintf(out->name, sizeof out->name, "player");
    }
    return found;
}

// ---- run --------------------------------------------------------------------

extern "C" void fx_run() {
    fx_log("trigger bot (TESTING) - logs only, never clicks");
    if (HOLD_KEY) fx_log("  hold the key to arm the decision");
    else          fx_log("  no hold key: deciding whenever the client is focused");

    // The identity of what we are on, so the edge is per PLAYER and not per
    // tick: walking the pointer from one enemy to another is two events, and
    // re-testing the same one is none.
    uint64_t on = 0;
    bool     on_clear = false;
    unsigned dm = fx_dm_gen();

    while (fx_running()) {
        if (fx_dm_gen() != dm) {
            dm = fx_dm_gen();
            on = 0;
            fx_log("world replaced - holding off until it settles");
            if (!fx_sleep(500)) break;
            continue;
        }

        // Not focused: decide nothing. The pointer is over whatever is in
        // front now, and a "target" read off that is meaningless.
        if (!fx_focused()) {
            if (on) { fx_log("  off (alt-tabbed)"); on = 0; }
            if (!fx_sleep(100)) break;
            continue;
        }

        // fx_input_ok covers typing in chat: the same gate the aim uses, so a
        // message with the pointer over someone is not a trigger event.
        bool armed = fx_input_ok() && (!HOLD_KEY || fx_key(HOLD_KEY) != 0);
        if (!armed) {
            if (on) { fx_log("  off (key released)"); on = 0; }
            if (!fx_pace(POLL_HZ)) break;
            continue;
        }

        RbxWin rw;
        POINT  cur;
        if (!get_rbx_window(&rw) || !GetCursorPos(&cur)) {
            if (!fx_sleep(100)) break;
            continue;
        }

        Hit h{};
        bool now = under_pointer(cur, rw, &h);

        if (now && (h.player != on || h.clear != on_clear)) {
            // THE LINE THE FIRING VERSION REPLACES WITH A CLICK. Written as
            // what it WOULD do, so the log reads as a decision rather than as
            // an observation -- and so a wrong decision is obvious here
            // instead of after something has been sent.
            fx_logf(h.clear ? "  WOULD FIRE - %s at %.0f studs, line clear"
                            : "  holding - %s at %.0f studs, BLOCKED by geometry",
                    h.name, h.dist);
            on = h.player;
            on_clear = h.clear;
        } else if (!now && on) {
            fx_log("  off target");
            on = 0;
        }

        if (!fx_pace(POLL_HZ)) break;
    }

    fx_log("trigger bot stopped");
}
