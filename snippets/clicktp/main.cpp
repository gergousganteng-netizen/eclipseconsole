#include "ctp.h"
#include <cstdarg>
#include <cstdio>
// WIN32_LEAN_AND_MEAN: windows.h is the most expensive header this project
// includes -- measured 2026-09-07 on the bundled g++, +2222ms over an empty
// TU at -O2, against +520 for <fstream>, +365 for <atomic>, +275 for
// <thread> and nothing at all for <string>/<chrono>/<cstdio> (fx.h already
// pulls those in). LEAN drops the parts nothing here uses -- OLE, RPC,
// winsock, the multimedia and shell APIs -- and measured 319-491ms back per
// file. Delete it only if something here starts needing one of those.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>   // GetTickCount64, for the settling burst below

// ---- dials ------------------------------------------------------------------
// Yours to change. Nothing here is written back, so editing the file and
// re-running is the whole workflow.
Cfg g = {
    /* trigger_vk    */ VK_XBUTTON2,   // side mouse button. VK_XBUTTON1 is the other one
    /* back_vk       */ VK_XBUTTON1,   // go back. It SWAPS, so pressing it again returns
    /* rebuild_vk    */ 'R',           // re-index the map after it changes
    /* max_range     */ 1500.0f,        // studs
    /* step_studs    */ 0.0f,          // 0 = one jump. Try 40 if the server pulls you back
    /* hold_ms       */ 250,
    /* hold_hz       */ 0,      // 0 = spin on fx_sleep(0); see move.cpp
    /* clearance_pad */ 0.5f,          // slack between your feet and the surface
    /* stick_tol     */ 6.0f,          // studs
    /* require_clear */ true,
    /* carry_camera  */ true,          // false to leave the camera to catch up
    /* freecam_vk    */ 'P',           // with SHIFT, like Roblox's own freecam
    /* fly_speed     */ 60.0f,         // studs/second
    /* fly_boost     */ 4.0f,          // while Shift is held
    /* fly_slow      */ 0.25f,         // while Ctrl is held
    /* freeze_walk   */ true,          // stop WASD also walking your body
    /* first_person  */ false,          // false = third person, but you can see yourself
};

static void lg(const char* f, ...) {
    char b[300];
    va_list a; va_start(a, f); vsnprintf(b, sizeof b, f, a); va_end(a);
    fx_log(b);
}

extern "C" void fx_run(void) {
    lg("clicktp: aim and press the side mouse button. The other one goes back.");
    lg("  Shift+P flies. R re-indexes the map.");
    if (!fx_armed()) lg("  disarmed -- it will report the move instead of making it.");

    unsigned gen = fx_gen();
    uint64_t dm = fx_dm();
    // 0 in the menu. Started from there it WAITS rather than exiting: the loop
    // below already handles a place arriving, so quitting would have been the
    // one state it could not recover from -- start it, tab in, and find it
    // dead.
    uint64_t place = fx_place();

    // The settling burst and the cheap watch that follows it. settle_step is
    // how many of the burst's rebuilds have run; ws_kids is the last Workspace
    // child count seen, -1 meaning "not looked yet" so the first read cannot
    // masquerade as a change.
    uint64_t settle_at = 0, watch_at = GetTickCount64(), last_build = GetTickCount64();
    int      settle_step = 3;
    int      ws_kids = -1;

    if (!place) {
        lg("  not in a game yet -- waiting for a place.");
    } else {
        // The index is the expensive part, so build it up front rather than
        // making the first click pay for it.
        int n = fx_geom(1);
        lg("  %d parts indexed in %d ms.", n, fx_geom_ms());
        if (n <= 0) lg("  WARN no geometry -- every click will refuse.");
        ws_kids = (int)fx_nchildren(fx_service("Workspace"));
    }
    Ptrs p; refresh_ptrs(&p);
    Body body{}; measure_body(p, &body);

    // Where you were before the last move. Kept across a respawn -- the
    // coordinates are still a real place you can stand -- but dropped when the
    // PLACE changes, because the same numbers in a different map are somewhere
    // arbitrary, quite possibly inside terrain.
    bool   have_back = false;
    float3 back_pos{};

    while (fx_running()) {
        // Respawn: the character is a new set of pointers and a new body.
        if (fx_gen() != gen) {
            gen = fx_gen();
            // Land first: the borrowed part and the old Humanoid's walkspeed
            // both belong to the world we are leaving behind.
            freecam_stop(p);
            refresh_ptrs(&p);
            measure_body(p, &body);
        }
        // A new place needs a new map. fx_geom rebuilds itself on a DataModel
        // change, but the body has to be re-measured here.
        //
        // INDEXING IMMEDIATELY IS TOO EARLY. The DataModel changes before the
        // world has streamed in, so a rebuild fired here walks a Workspace
        // that is still filling and reports "0 parts" -- which is exactly what
        // a rejoin used to log, twice, and then nothing was ever found under
        // the cursor again. So the change only ARMS a settling burst; the
        // rebuilds happen below.
        // AND THE MENU IS NOT A PLACE. Leaving a game changes the DataModel
        // exactly like joining one does, so keying the burst on that alone had
        // it index the MENU -- three rebuilds of an empty Workspace and a
        // warning telling you to press R, while you sat on the home screen.
        // fx_place is the difference: 0 there, the id in a place.
        if (fx_dm() != dm) {
            freecam_stop(p);
            dm = fx_dm();
            have_back = false;
            place = fx_place();
            if (!place) {
                settle_step = 3;                  // nothing to index; stop asking
                lg("  left the game -- waiting for a place.");
            } else {
                settle_at = GetTickCount64();
                settle_step = 0;
                lg("  new place -- indexing as it loads.");
            }
        } else if (!place && fx_place()) {
            // Joined from the menu without the DataModel pointer changing
            // again -- arm the burst on the place appearing.
            place = fx_place();
            settle_at = GetTickCount64();
            settle_step = 0;
            lg("  in a place -- indexing as it loads.");
        }

        // THE SETTLING BURST, then a cheap watch. Both halves matter:
        //
        //   * A FIXED delay cannot be right. Streaming takes as long as it
        //     takes, so one timer is either too early (empty map) or time
        //     spent waiting for nothing. Three tries spread over the first
        //     ten seconds cover the range without guessing which is correct.
        //   * A BLANKET TIMER afterwards is worse than nothing. A rebuild is
        //     31-37 ms on a real map -- two dropped frames -- so re-indexing
        //     "every few seconds" forever buys a periodic stutter to catch a
        //     change that almost never comes.
        //
        // So: burst while the world is arriving, then watch a signal that is
        // four orders of magnitude cheaper than the thing it decides. The
        // Workspace child count is one read; streaming adds and removes whole
        // regions, so it moves when the map does. It cannot see a part added
        // deep inside an existing model -- R still re-indexes by hand, and
        // that is what it is for.
        if (fx_place()) {
            const unsigned kSettle[3] = { 1000, 3000, 8000 };   // ms after the change
            uint64_t now = GetTickCount64();

            if (settle_step < 3 && now - settle_at >= kSettle[settle_step]) {
                settle_step++;
                int n = fx_geom(1);
                ws_kids = (int)fx_nchildren(fx_service("Workspace"));
                lg("  %d parts.", n);
                // Nothing yet: leave the burst running rather than settling on
                // an empty map. An empty build no longer sticks host-side, but
                // saying so here is what makes a slow load readable.
                if (n <= 0 && settle_step >= 3)
                    lg("  WARN still nothing -- press R once the map is in.");
            } else if (settle_step >= 3 && now - watch_at >= 1000) {
                watch_at = now;
                int kids = (int)fx_nchildren(fx_service("Workspace"));

                // THE SIGNAL IS NOISY, and treating it as exact makes this
                // worse than the blanket timer it replaces. Measured on a live
                // server: the count oscillates by one continuously -- dropped
                // items, projectiles, sounds, anything parented to Workspace
                // for a moment -- so "changed since last poll" fired every
                // second and rebuilt 37 ms each time. That was the first
                // version, and its own smoke test caught it.
                //
                // Two guards, both needed. A THRESHOLD, because a map actually
                // changing moves the count by more than the churn does; and a
                // MINIMUM INTERVAL, because a rebuild is expensive enough that
                // it should be rare whatever the signal says. Compared against
                // the count at the last REBUILD, not the last poll, so slow
                // drift accumulates into a trigger instead of hiding under the
                // threshold one part at a time.
                int  delta = kids > ws_kids ? kids - ws_kids : ws_kids - kids;
                int  need  = ws_kids / 10;  if (need < 4) need = 4;
                bool cooled = now - last_build >= 15000;

                if (ws_kids >= 0 && delta >= need && cooled) {
                    ws_kids = kids;
                    last_build = now;
                    lg("  map changed (%d top-level) -- re-indexed: %d parts.",
                       kids, fx_geom(1));
                }
            }
        }

        // Shift+P, the binding Roblox's own freecam uses. The key is tested
        // BEFORE the modifier because fx_key_pressed consumes one press per
        // call -- checking Shift first would eat the P on any frame you were
        // not holding Shift.
        if (fx_key_pressed(g.freecam_vk) && fx_key(VK_SHIFT) && fx_input_ok()) {
            lg("  %s", freecam_toggle(p).c_str());
            goto tick;
        }
        // Fly, but do NOT skip the rest of the loop. Two reasons, and the
        // second was a real bug: teleporting while flying is worth having, and
        // fx_key_pressed counts presses SINCE THE LAST CALL -- so skipping the
        // hotkey checks while flying buffered them, and the first click after
        // leaving freecam fired a teleport you asked for a minute ago.
        if (freecam_on()) freecam_tick(p);

        // Back. A SWAP rather than a one-shot: it remembers where you were when
        // you pressed it, so the second press returns you. Two spots, one key,
        // no history to reason about.
        //
        // No foot_drop and no surface here -- back_pos is a ROOT position that
        // you were already standing at, not a point on geometry, so adjusting it
        // the way a click does would lift you half a body every trip.
        if (fx_key_pressed(g.back_vk) && fx_input_ok()) {
            if (!have_back) { lg("x nowhere to go back to yet."); goto tick; }
            if (!p.ok && !refresh_ptrs(&p)) { lg("x no character."); goto tick; }

            float3 here{};
            bool got_here = fx_pos(p.hrp, &here) != 0;

            std::string report;
            bool ok = teleport(p, back_pos, &report);
            lg("%s back: %s", ok ? " " : "!", report.c_str());
            if (ok && got_here) back_pos = here;      // only swap if we moved
            goto tick;
        }

        if (fx_key_pressed(g.rebuild_vk) && fx_input_ok())
            lg("  re-indexed: %d parts in %d ms.", fx_geom(1), fx_geom_ms());

        if (fx_key_pressed(g.trigger_vk) && fx_input_ok()) {
            if (!p.ok && !refresh_ptrs(&p)) { lg("x no character."); goto tick; }
            if (!body.ok) measure_body(p, &body);

            float cx = 0, cy = 0;
            if (!cursor_px(&cx, &cy)) { lg("x cannot read the cursor."); goto tick; }

            float3 eye{}, dir{};
            std::string why;
            if (!camera_ray(cx, cy, &eye, &dir, &why)) { lg("x %s.", why.c_str()); goto tick; }

            float3 hit{};
            uint64_t part = 0;
            if (!fx_raycast(eye, dir, g.max_range, &hit, &part)) {
                lg("x nothing within %.0f studs under the cursor.", g.max_range);
                goto tick;
            }

            // Stand ON the surface: the root sits foot_drop above your feet.
            float3 dest = hit;
            dest.y += body.foot_drop + g.clearance_pad;

            float3 blocker{};
            if (g.require_clear && !room_at(body, dest, &blocker)) {
                lg("x no room there -- blocked at (%.0f, %.0f, %.0f).",
                   blocker.x, blocker.y, blocker.z);
                goto tick;
            }

            // Snapshot BEFORE the move, and keep it whether or not the move
            // stuck: if it drifted you are somewhere unintended and going back
            // is exactly what you want. Only a refusal above leaves it alone,
            // because then nothing was written.
            float3 before{};
            bool got_before = fx_pos(p.hrp, &before) != 0;

            std::string report;
            bool ok = teleport(p, dest, &report);
            lg("%s %s", ok ? " " : "!", report.c_str());
            if (got_before && fx_armed()) { back_pos = before; have_back = true; }
        }

    tick:
        if (!fx_sleep(16)) break;
    }
    // Never exit still flying: that would leave a part of the map parked in
    // the sky and the character unable to walk.
    freecam_stop(p);
    lg("clicktp: stopped.");
}
