#include "bd.h"
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
// WIN32_LEAN_AND_MEAN: windows.h is the most expensive header this project
// includes -- measured 2026-09-07 on the bundled g++, +2222ms over an empty
// TU at -O2, against +520 for <fstream>, +365 for <atomic>, +275 for
// <thread> and nothing at all for <string>/<chrono>/<cstdio> (fx.h already
// pulls those in). LEAN drops the parts nothing here uses -- OLE, RPC,
// winsock, the multimedia and shell APIs -- and measured 319-491ms back per
// file. Delete it only if something here starts needing one of those.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" void fx_run() {
    cfg_load();

    fx_log("backdash: settings are in the window, and saved to settings.ini");
    if (!fx_armed())
        fx_log("! disarmed - gates and UI run, nothing is written");

    // Claimed so the game never sees them, and re-claimed if you rebind.
    int claimed_dash = g.key_dash, claimed_fps = g.key_fps;
    fx_claim_key(claimed_dash, 1);
    fx_claim_key(claimed_fps, 1);

    fx_window_open("Backdash", 400, 620);
    fx_window_navigate(ui_page_path().c_str());

    Ptrs p;
    int    dashes  = 0;
    double last_ui = 0.0;
    // The burst runs here so the loop keeps polling keys, ticking the HP gate
    // and refreshing the window while it writes. Inline, it blocked all three
    // for hold_secs -- long enough to miss damage taken mid-dash.
    std::thread       worker;
    std::atomic<bool> busy{false};
    bool        claims_on = true;
    bool        push_now  = true;      // force the first frame out
    std::string status    = "ready";
    double      fps       = fx_getfps();
    unsigned    gen       = fx_gen();

    while (fx_running()) {
        // The panel: set:<key>=<value>, reset, ready, dbg:<msg>.
        while (const char* m = fx_window_get_msg()) {
            std::string s = m;
            if (s.rfind("set:", 0) == 0) {
                size_t eq = s.find('=', 4);
                if (eq != std::string::npos) {
                    std::string k = s.substr(4, eq - 4), v = s.substr(eq + 1);
                    if (cfg_set(k, v)) {
                        cfg_save();
                        status = k + " = " + v;
                        // A rebind has to move the claim with it, or the old key
                        // stays swallowed for the rest of the session and the
                        // new one is never taken.
                        if (k == "key_dash" || k == "key_fps") {
                            fx_claim_key(claimed_dash, 0);
                            fx_claim_key(claimed_fps, 0);
                            claimed_dash = g.key_dash;
                            claimed_fps  = g.key_fps;
                            claims_on = false;      // retaken below by the gate
                        }
                    } else {
                        status = "unknown setting " + k;
                    }
                    push_now = true;
                }
            } else if (s == "reset") {
                cfg_reset();
                cfg_save();
                fx_claim_key(claimed_dash, 0);
                fx_claim_key(claimed_fps, 0);
                claimed_dash = g.key_dash;
                claimed_fps  = g.key_fps;
                claims_on = false;
                status = "settings reset";
                push_now = true;
            } else if (s == "ready") {
                push_now = true;        // the page reloaded; it has nothing yet
            } else if (s.rfind("dbg:", 0) == 0) {
                fx_log(("  page> " + s.substr(4)).c_str());
            }
        }

        // Claims are global -- held only while the game is in front and no chat
        // box is open, or the bound keys would be swallowed everywhere.
        bool want = fx_input_ok();
        if (want != claims_on) {
            fx_claim_key(claimed_dash, want);
            fx_claim_key(claimed_fps,  want);
            claims_on = want;
        }

        if (fx_gen() != gen) { gen = fx_gen(); status = "character changed"; }

        refresh_ptrs(&p);
        gates_tick(p);

        if (fx_key_pressed(g.key_dash) && fx_input_ok()) {
            DashPlan plan;
            std::string why;
            if (dash_begin(p, &plan, &why)) {
                // Join before reusing the handle: assigning over a running
                // thread terminates the process. The cooldown makes it
                // unreachable in practice, which is not a thing to rely on.
                if (worker.joinable()) worker.join();
                busy = true;
                // Adopt this run on the worker. With several snippets able to
                // run at once, a thread the host never saw spawn defaults to
                // "stop only when EVERYTHING stops" -- so it would not see this
                // snippet's own Stop, and the join below would block until the
                // dash reached its wall-clock end instead of cutting mid-burst.
                unsigned rid = fx_run_id();
                worker = std::thread([plan, rid, &busy]() {
                    fx_adopt_run(rid);
                    dash_run(plan); busy = false;
                });
                dashes++;
            }
            status = why;
            fx_log(("  " + status).c_str());
            push_now = true;
        }

        if (fx_key_pressed(g.key_fps) && fx_input_ok()) {
            double cur  = fx_getfps();
            double next = (cur > (g.fps_low + g.fps_high) * 0.5) ? g.fps_low : g.fps_high;
            char b[64];
            if (fx_setfps(next)) snprintf(b, sizeof b, "fps cap %.0f", next);
            else                 snprintf(b, sizeof b, "fps cap %.0f refused (disarmed?)", next);
            status = b;
            fx_log(("  " + status).c_str());
            push_now = true;
        }

        // The window only needs the tick rate. Repainting on every key event
        // would push a full state object at whatever rate you happen to type.
        unsigned long long now = GetTickCount64();
        if (push_now || now - (unsigned long long)last_ui >= TICK_MS) {
            last_ui  = (double)now;
            push_now = false;
            fps = fx_getfps();
            fx_window_eval(ui_state_js(p, fps, dashes, busy.load(), status).c_str());
        }

        // Wakes on a key event, on Stop, or after TICK_MS -- whichever is
        // first. A plain sleep here WAS the input delay: the key is read once
        // per pass, so the dash waited up to a tenth of a second.
        if (!fx_wait_key(TICK_MS)) break;
    }

    // Before the window closes and before fx_run returns: this DLL is unloaded
    // on return, and a thread still inside dash_run would be executing freed
    // code. fx_running() is already false, so the burst exits on its own.
    if (worker.joinable()) worker.join();

    fx_window_close();
    fx_log("backdash: stopped");
}
