// Eclipse hub -- one docked panel, tabs down the side, tools inside it.
//
// The shell: it owns the window, the roster, our own rig, the message pump and
// the loop rate. Tools own their own behaviour and nothing else. See hub.h.

#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <string>

#include "hub.h"

const double ROSTER_MS  = 250.0;
const double PRIM_MS    = 200.0;
const int    PANEL_W    = 560;
const int    PANEL_H    = 430;

double hub_now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

void hub_eval(const std::string& js) { fx_window_eval(js.c_str()); }

void js_escape(const std::string& in, std::string& out) {
    for (char c : in) {
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c >= 32)   out += c;
    }
}

void move_body(const Ctx& c, float3 at) {
    if (!c.prim) return;
    float3 root{};
    if (fx_rdblk(c.prim + c.oPos, &root, 12) != 12) return;
    float3 d{ at.x - root.x, at.y - root.y, at.z - root.z };
    if (c.nbody <= 0) {
        float3 to{ root.x + d.x, root.y + d.y, root.z + d.z };
        fx_wrblk(c.prim + c.oPos, &to, 12);
        return;
    }
    for (int i = 0; i < c.nbody; i++) {
        uint64_t pr = fx_rd(c.body[i] + c.oPrim);
        if (pr < 0x10000 || pr > 0x7ff000000000ULL) continue;
        float3 cur{};
        if (fx_rdblk(pr + c.oPos, &cur, 12) != 12) continue;
        float3 to{ cur.x + d.x, cur.y + d.y, cur.z + d.z };
        fx_wrblk(pr + c.oPos, &to, 12);
    }
}

extern "C" void fx_run() {
    Ctx c;
    c.oPrim = fx_off("BasePart.Primitive");
    c.oPos  = fx_off("Primitive.Position");
    c.oVel  = fx_off("Primitive.AssemblyLinearVelocity");
    c.oSpin = fx_off("Primitive.AssemblyAngularVelocity");
    if (c.oPrim == (uint64_t)-1 || c.oPos == (uint64_t)-1 ||
        c.oVel  == (uint64_t)-1 || c.oSpin == (uint64_t)-1) {
        fx_log("x missing a Primitive offset");
        return;
    }

    int dock_dx = 14, dock_dy = 14;
    int panel_h = PANEL_H;        // tracks the collapse, for the clamp
    fx_window_open("Eclipse hub", PANEL_W, PANEL_H);
    fx_window_navigate((std::string(fx_snippets_dir()) + "\\hub\\ui.html").c_str());
    // TOP-left, not bottom: a resize grows and shrinks the rect away from the
    // docked corner, so the corner's edge is the one that holds still. With the
    // header at the top of the panel, that has to be the TOP edge -- docked
    // bottom-left, collapsing dragged the header down by the height it lost.
    fx_window_dock(1, FX_DOCK_TL, dock_dx, dock_dy);

    fx_log("hub: open.");
    fx_log(fx_armed() ? "  ARMED -- tools that write will actually write."
                      : "  disarmed -- writes are logged, not performed.");

    std::string tab = "players";
    bool     ready       = false;
    double   start_ms    = hub_now_ms();
    double   next_roster = 0;
    bool     push_now    = false;
    double   next_prim   = 0;
    double   drag_until  = 0;
    double   next_clamp  = 0;
    unsigned gen         = fx_gen() - 1;
    // DisplayName by UserId, read once each. It cannot change while they are in
    // the server, and re-reading a std::string out of the client four times a
    // second for every player is a lot of syscalls for a value that never moves.
    std::map<int64_t, std::string> disp;
    const uint64_t oDN = fx_off("Player.DisplayName");

    while (fx_running()) {
        c.now = hub_now_ms();

        // ---- the panel ------------------------------------------------------
        while (const char* m = fx_window_get_msg()) {
            std::string msg = m;
            if      (msg == "ready")            ready = true;
            else if (msg.rfind("tab:", 0) == 0) tab = msg.substr(4);
            // COLLAPSING HAS TO SHRINK THE RECT, not just the drawing. A docked
            // panel owns its whole rect as far as input is concerned, so a
            // collapsed page left at full size is a header you can see and 390
            // pixels of invisible click-blocker underneath it. Re-opening at a
            // new size resizes in place and leaves the DOM alone (snip_open).
            // 100 is the host's minimum height, so that is the floor.
            else if (msg == "size:min")  { panel_h = 36;     fx_window_open("Eclipse hub", PANEL_W, panel_h); }
            else if (msg == "size:full") { panel_h = PANEL_H; fx_window_open("Eclipse hub", PANEL_W, panel_h); }
            else if (msg.rfind("dbg:", 0) == 0) fx_logf("panel: %s", msg.substr(4).c_str());
            else if (msg.rfind("drag:", 0) == 0) {
                int ddx = 0, ddy = 0;
                drag_until = c.now + 250.0;
                if (sscanf(msg.c_str() + 5, "%d,%d", &ddx, &ddy) == 2) {
                    dock_dx += ddx; dock_dy += ddy;      // TL: y grows downwards
                    // CLAMPED SO THE WHOLE PANEL STAYS INSIDE. The bound is
                    // the far EDGE of the panel, not its origin -- bounding the
                    // origin still let 480 of its 560 pixels hang off the right.
                    // Uses the CURRENT height, which the collapse changes.
                    float vw = 0, vh = 0;
                    if (fx_viewport(&vw, &vh) && vw > 1 && vh > 1) {
                        int maxx = (int)vw - PANEL_W;
                        int maxy = (int)vh - panel_h;
                        if (maxx < 0) maxx = 0;     // panel wider than the game
                        if (maxy < 0) maxy = 0;
                        if (dock_dx > maxx) dock_dx = maxx;
                        if (dock_dy > maxy) dock_dy = maxy;
                    }
                    if (dock_dx < 0) dock_dx = 0;
                    if (dock_dy < 0) dock_dy = 0;
                    fx_window_dock(1, FX_DOCK_TL, dock_dx, dock_dy);
                }
            }
            // Tools see every other message. They ignore what is not theirs.
            else { tp_msg(msg, c); fling_msg(msg, c); mov_msg(msg, c); spec_msg(msg, c); fly_msg(msg, c); }
            // Anything we just acted on should be visible immediately. Waiting
            // for the next roster tick put up to 250ms between a click and the
            // button changing, which reads as the panel being slow.
            push_now = true;
        }
        // Nothing is sent before the page exists: fx_window_navigate is async,
        // so an eval in the first ~100ms lands in a document that has not
        // loaded ui.js and is dropped without a word. Fails OPEN after 3s.
        if (!ready && c.now - start_ms > 3000) {
            ready = true;
            fx_log("  ! panel never reported ready");
        }

        // ---- our own rig ----------------------------------------------------
        // On a clock as well as on the generation edge: the root part is
        // REPLACED ~100ms after a respawn with no fx_gen tick to say so, and a
        // latched pointer then writes into freed memory. Measured in fly.cpp.
        if (fx_gen() != gen || !c.prim || c.now >= next_prim) {
            bool edge = fx_gen() != gen;
            gen = fx_gen();
            next_prim = c.now + PRIM_MS;
            uint64_t found = 0;
            uint64_t chr = fx_character();
            uint64_t root = chr ? fx_part(chr, "HumanoidRootPart") : 0;
            if (!root && chr) root = fx_part(chr, "Torso");
            if (root) {
                uint64_t pr = fx_rd(root + c.oPrim);
                if (pr >= 0x10000 && pr <= 0x7ff000000000ULL) found = pr;
            }
            // A 0 is only believed on the generation edge, where it really does
            // mean dead or loading.
            if (found || edge) c.prim = found;
            c.hum   = chr ? fx_part(chr, "Humanoid") : 0;
            c.nbody = c.prim ? fx_parts(c.body, 32) : 0;
            c.me_ok = c.prim && fx_rdblk(c.prim + c.oPos, &c.me, 12) == 12;
        }

        // OUR OWN POSITION, EVERY TICK. One 12-byte read, and the tools steer
        // by it: on the roster's 250ms clock it was up to a quarter of a second
        // stale, which at fling speeds points the approach vector at where we
        // used to be. The roster MEMBERSHIP can be slow; where we are cannot.
        // (Target positions were already fresh -- the tools read those
        // themselves off the roster's part pointers each tick.)
        if (c.prim) c.me_ok = fx_rdblk(c.prim + c.oPos, &c.me, 12) == 12;

        // ---- keep it on screen when the CLIENT changes ----------------------
        // The dock follows the game window on its own -- corner plus insets --
        // but the insets were only ever clamped at drag time. Resize the client
        // smaller, or move it to a smaller monitor, and a panel that was inside
        // is suddenly hanging off the edge with nothing re-checking it. Cheap
        // enough to re-check on the roster clock.
        if (c.now >= next_clamp) {
            next_clamp = c.now + 500.0;
            float vw = 0, vh = 0;
            if (fx_viewport(&vw, &vh) && vw > 1 && vh > 1) {
                int maxx = (int)vw - PANEL_W;  if (maxx < 0) maxx = 0;
                int maxy = (int)vh - panel_h;  if (maxy < 0) maxy = 0;
                int nx = dock_dx > maxx ? maxx : dock_dx;
                int ny = dock_dy > maxy ? maxy : dock_dy;
                if (nx != dock_dx || ny != dock_dy) {
                    dock_dx = nx; dock_dy = ny;
                    fx_window_dock(1, FX_DOCK_TL, dock_dx, dock_dy);
                }
            }
        }

        // ---- the roster -----------------------------------------------------
        if (c.now >= next_roster || push_now) {
            push_now    = false;
            next_roster = c.now + ROSTER_MS;
            c.n = fx_players(c.p, 64);
            if (c.prim) c.me_ok = fx_rdblk(c.prim + c.oPos, &c.me, 12) == 12;

            if (ready) {
                std::string js = "S({\"tab\":\"" + tab + "\",\"players\":[";
                bool first = true;
                for (int i = 0; i < c.n; i++) {
                    if (c.p[i].is_local) continue;      // no root is fine: a
                    char b[192];                        // respawning player
                    float3 q{};                         // keeps its row
                    float  d = -1.0f;
                    if (c.me_ok && c.p[i].root) {
                        uint64_t pr = fx_rd(c.p[i].root + c.oPrim);
                        if (pr >= 0x10000) {
                            q = fx_rf3(pr + c.oPos);
                            float dx = q.x - c.me.x, dy = q.y - c.me.y, dz = q.z - c.me.z;
                            d = sqrtf(dx*dx + dy*dy + dz*dz);
                        }
                    }
                    if (!first) js += ",";
                    first = false;
                    snprintf(b, sizeof b, "{\"u\":%lld,\"d\":%.0f,\"hp\":%.0f,\"n\":\"",
                             (long long)c.p[i].userid, d, c.p[i].health);
                    js += b;
                    std::string nm = c.p[i].name;
                    js_escape(nm, js);
                    js += "\",\"dn\":\"";
                    // DisplayName is what people read in game; the username is
                    // what identifies them. Routinely different here, so both.
                    if (c.p[i].userid && oDN != (uint64_t)-1) {
                        auto it = disp.find(c.p[i].userid);
                        if (it == disp.end()) {
                            char dn[64] = "";
                            fx_str(c.p[i].player + oDN, dn, sizeof dn);
                            it = disp.emplace(c.p[i].userid, dn).first;
                        }
                        js_escape(it->second, js);
                    }
                    js += "\",\"a\":\"";
                    char u[256];
                    if (c.p[i].userid && fx_avatar_url(c.p[i].userid, u, sizeof u)) {
                        std::string url = u;
                        js_escape(url, js);
                    }
                    js += "\"}";
                }
                js += "],";
                tp_panel(js, c);            // each tool's own slice
                js += ",";
                fling_panel(js, c);
                js += ",";
                mov_panel(js, c);
                js += ",";
                spec_panel(js, c);
                js += ",";
                fly_panel(js, c);
                js += "})";
                hub_eval(js);
            }
        }

        // ---- tools ----------------------------------------------------------
        // EVERY tool ticks, whatever tab is showing. A tab is a view.
        tp_tick(c);
        fling_tick(c);
        mov_tick(c);
        spec_tick(c);
        fly_tick(c);

        // ---- how fast to go -------------------------------------------------
        // The loop runs at the rate the HOTTEST active tool needs. Anything
        // racing the physics solver has to spin -- a paced write loses -- but
        // spinning while nothing is happening burns a core for nothing.
        if      (tp_hot() || fling_hot() || mov_hot() || spec_hot() || fly_hot()) { if (!fx_sleep(0)) break; }
        else if (c.now < drag_until || mov_busy()) { if (!fx_sleep(4)) break; }
        else if (!fx_wait_key(60))    break;

        if (fx_key_pressed(fling_key()) && fx_input_ok()) fling_toggle(c);
        // NO INSERT HANDLER. Insert is the console's own hotkey -- it opens the
        // console over the game -- and having the hub answer it too meant one
        // press did two things. Collapse and expand is the header's button.
    }

    // The camera is the one thing here that outlives the snippet if it is left
    // pointed somewhere else -- a black screen with no obvious cause.
    spec_stop(c);
    // Gravity is GLOBAL: a map left floating outlives the snippet and affects
    // everyone. This is the other thing that must not be skipped on the way out.
    fly_stop();
    mov_stop(c);
    fx_window_close();
    fx_log("hub: stopped");
}
