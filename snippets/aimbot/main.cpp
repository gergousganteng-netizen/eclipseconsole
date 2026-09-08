// Aim assist. Hold RIGHT MOUSE to turn the camera toward an on-screen player.
//
//   hold RMB   acquire the target nearest your crosshair, then STAY on it --
//              it hands off only when you clearly aim at someone else, so a
//              player crossing your aim cannot steal it
//   F7         report what it can see: player counts, the current target, its
//              distance and speed, the lead being applied, and the yaw error
//   Stop       ends it
//
// This WRITES Camera.Rotation, so it obeys the arm gate like everything else:
// disarmed it logs the write and the camera does not move. Run it disarmed
// first -- that is the cheapest way to see whether it picks sensible targets
// before it is allowed to touch anything.
//
// Expect it to fight the client. The engine recomputes the camera every frame
// from mouse input, so a write is in competition with that rather than
// replacing it; SMOOTHING below trades snap for stability.
#include "aim.h"
#include <chrono>
#include <cstdio>

// 0..1 per tick. 1.0 snaps. Lower is calmer, but smoothing is a low-pass filter
// on the aim and a low-pass filter IS a lag -- against a moving target it is the
// difference between tracking them and following them. Lower it for feel once
// the tracking is right, not to fix tracking.
// TWO RATES, one per job. The aim branch used to fx_sleep(1) -- about 1000 Hz
// of walking every player and projecting each one, to drive a camera write
// nothing downstream consumes that fast. 144 is what the standalone aimbot
// has always used for the same work.
//
// NOTE IF THE PULL FEELS DIFFERENT: SMOOTHING below is a fraction of the
// CURRENT ERROR APPLIED PER TICK, so the tick rate is part of how the aim
// feels. Going from ~1000 Hz to 144 means fewer corrections per second, and
// the fix is to raise SMOOTHING -- not to put the rate back.
unsigned AIM_HZ = 144;   // while RMB is held
unsigned ESP_HZ = 60;    // idle: boxes only

float SMOOTHING = 0.50f;
float MAX_DIST  = 800.0f;  // studs; ignore anything further

// Defined here rather than in aim.h now that it is mutable.
bool  TEAM_CHECK = true;

// How far ahead of the target to aim, in seconds of their own velocity.
//
// This is the fix for "it lags behind them when they move", and smoothing is
// not: the error is not that the camera is slow to reach where they WERE.
// Between reading their position and the client drawing the frame they keep
// moving -- a player strafing at 16 studs/s at 20 studs range sweeps about 45
// degrees a second, so even 30 ms of round trip is a degree and a half behind,
// permanently. Aiming at position + velocity * LEAD cancels that.
//
// Too small and it still trails; too large and the crosshair sits in front of
// them and swings wide whenever they cut. F7 reports the speed and the lead it
// is actually applying, which is the honest way to tune it.
float LEAD_SECONDS = 0.06f;

// Target stickiness. Rather than snap to whoever is momentarily nearest the
// crosshair -- which lets anyone crossing your aim steal it -- the aim holds its
// current target and hands off only when a challenger is clearly the one you
// mean: closer to the crosshair by a margin AND held there long enough that a
// brief pass cannot trigger it.
//
//   SWITCH_FRACTION  a challenger's crosshair distance must be under this
//                    fraction of the current target's before it counts at all.
//                    0.70 = it has to be about a third closer. Lower = stickier.
//   SWITCH_DWELL_MS  and it must stay the better pick this long before the
//                    switch commits. Higher = stickier, harder to switch.
//
// While a challenger is being timed the aim STOPS writing, so it is not fighting
// you back onto the old target while you move onto the new one -- taking over is
// just "aim at them and hold for a moment". If the challenger turns out to be a
// passer-by, the timer resets and the aim resumes on the current target.
//
// A lost target -- dead, off screen, or beyond MAX_DIST -- is dropped at once
// and the nearest takes over. Stickiness guards against STEALS; it never leaves
// you aiming at nothing.
float  SWITCH_FRACTION = 0.8f;
double SWITCH_DWELL_MS = 25.0;

// 0 = nearest to the crosshair, 1 = nearest in the world. See target_score.
int   TARGET_PRIORITY = 1;
// Pixels from the crosshair a target must be within to be eligible. 0 = no
// limit. Defaults off so the behaviour is unchanged until you ask for it.
float PICK_RADIUS     = 0.0f;

// ---- who to aim at ----------------------------------------------------------
//
// ONE scorer for every decision -- acquiring, the challenger check, the
// fallback when a target is lost, and the F7 report. They used to disagree on
// purpose: acquisition took the nearest in the WORLD while the challenger check
// used crosshair PIXELS, which meant "aim at them to take over" behaved
// differently from "press to acquire". Now the priority decides, and it decides
// everywhere.
//
// TARGET_PRIORITY  0 = crosshair, nearest in screen pixels. Points where you
//                      point; a player crossing your aim is a real challenger.
//                  1 = distance, nearest in studs. The immediate threat wins
//                      even if your reticle is elsewhere.
//
// PICK_RADIUS      pixels from the crosshair a target must be within to be
//                  eligible AT ALL, whatever the priority. 0 is a real off,
//                  not merely invisible -- the pick stops being limited and
//                  the best target on screen wins however far out it is.
//                  MAX_DIST still applies, so 0 is unbounded in screen space
//                  only. Ported from the sibling's ring, which learned that
//                  hiding the boundary while it still gated the pick was the
//                  confusing half-measure.
//
// The two together are how you get "closest in world, but only near my
// crosshair": priority 1 with a radius set.
static float target_score(const Target& g, const float vp[2]) {
    if (g.team || !g.onscreen || g.dist > MAX_DIST) return 1e18f;
    float dx = g.sx - vp[0] * 0.5f, dy = g.sy - vp[1] * 0.5f;
    float px = sqrtf(dx * dx + dy * dy);
    if (PICK_RADIUS > 0.0f && px > PICK_RADIUS) return 1e18f;
    // Not squared, unlike the old crosshair test: SWITCH_FRACTION compares two
    // of these as a ratio, and a ratio of squares is not the same threshold.
    return TARGET_PRIORITY >= 1 ? g.dist : px;
}

static int pick_best(const Target* t, int n, const float vp[2]) {
    int best = -1;
    float bs = 1e18f;
    for (int i = 0; i < n; i++) {
        float s = target_score(t[i], vp);
        if (s < bs) { bs = s; best = i; }
    }
    return best;
}

// The target with this name in the current scan, or -1. Names are the identity
// that survives a re-scan; indices do not.
static int index_of(const Target* t, int n, const char* name) {
    if (!name[0]) return -1;
    for (int i = 0; i < n; i++) if (strcmp(t[i].name, name) == 0) return i;
    return -1;
}

extern "C" void fx_run() {
    char b[192];
    Target t[64];

    // Saved dials BEFORE anything reads them. cfg_load captures whatever the
    // source currently says as the defaults, so a missing file leaves the aim
    // exactly as it was tuned here rather than reverting it to something else.
    cfg_load();

    fx_log("aim assist running - hold RMB to aim, F7 to report, Stop to end");
    fx_overlay_on();       // ESP: show markers on every player while running

    // The settings panel. Its own window rather than the overlay: you tune this
    // with a mouse, and the overlay is click-through by design.
    fx_window_open("Aim assist", 420, 560);
    fx_window_navigate(ui_page_path().c_str());
    std::string ui_status;
    auto ui_last = std::chrono::steady_clock::now();
    // Last target count seen. Each branch below scopes its own `n`, and
    // collect_targets is far too expensive to call again just to label a
    // panel, so the count is carried out rather than re-derived.
    int ui_targets = 0;
    // Tracks whether anything is currently drawn, so the clear is posted once
    // on losing focus rather than every frame while away.
    bool ovl_shown = true;

    float R[9], vp[2], fov;
    float3 eye;
    if (!camera_basis(R, &eye, &fov, vp)) {
        fx_log("x cannot read the camera - are you in a place?");
        fx_overlay_off();
        return;
    }

    unsigned dm = fx_dm_gen();

    // The target we are on now, and the challenger we are timing. curName sticks
    // across ticks and changes only on a committed switch or a lost target;
    // challName + challSince are the "has it held the lead long enough?" clock.
    char curName[48]   = "";
    char challName[48] = "";
    auto challSince    = std::chrono::steady_clock::now();
    bool wasAiming     = false;

    while (fx_running()) {
        if (fx_dm_gen() != dm) {
            dm = fx_dm_gen();
            curName[0] = '\0';
            challName[0] = '\0';
            fx_log("world replaced - holding off until it settles");
            if (!fx_sleep(500)) break;
            continue;
        }

        // Gated on the game being in front and no chat box open. fx_key is a
        // LEVEL read rather than an edge one, so unlike fx_key_pressed it
        // consumes nothing and the order here does not matter.
        //
        // Releasing the gate mid-aim reads as letting go of RMB, which is the
        // behaviour you want: alt-tab and the camera stops being driven instead
        // of continuing to track someone you can no longer see.
        bool aiming = fx_key(VK_RBUTTON) != 0 && fx_input_ok();

        // RMB just pressed -- acquire the CLOSEST player in the world (studs),
        // not the one your crosshair is over. The idea: whoever is nearest is
        // the immediate threat, so the aimbot picks them up even if you were
        // pointing elsewhere. To override (deliberately switch to someone
        // else), keep RMB held and aim at them -- the challenger check below
        // still uses crosshair distance and will hand off.
        if (aiming && !wasAiming) {
            curName[0] = '\0';
            challName[0] = '\0';
            int n = collect_targets(t, 64); ui_targets = n;
            if (camera_basis(R, &eye, &fov, vp)) {
                int best = pick_best(t, n, vp);
                if (best >= 0) {
                    snprintf(curName, sizeof curName, "%s", t[best].name);
                    snprintf(b, sizeof b, "target %s (%.0f studs)", curName, t[best].dist);
                    fx_log(b);
                }
            }
        }

        if (!aiming) { curName[0] = '\0'; challName[0] = '\0'; }
        wasAiming = aiming;

        if (fx_key_pressed(VK_F7)) {
            int n = collect_targets(t, 64); ui_targets = n;
            int vis = 0;
            for (int i = 0; i < n; i++) if (t[i].onscreen) vis++;

            int k = -1;
            if (camera_basis(R, &eye, &fov, vp)) {
                if (curName[0]) k = index_of(t, n, curName);
                if (k < 0)      k = pick_best(t, n, vp);
            }
            if (k >= 0) {
                float err = 0, side = 0;
                float speed = sqrtf(v_dot(t[k].vel, t[k].vel));
                if (aim_report(eye, t[k].world, &err, &side))
                    snprintf(b, sizeof b,
                             "%d players, %d on screen | %s at %.0f studs, speed %.1f, "
                             "lead %.2f | yaw err %.2f deg",
                             n, vis, t[k].name, t[k].dist, speed,
                             speed * LEAD_SECONDS, err);
                else
                    snprintf(b, sizeof b, "%d players, %d on screen", n, vis);
            } else {
                snprintf(b, sizeof b, "%d players, %d on screen - none in front of you", n, vis);
            }
            fx_log(b);
        }

        if (aiming && curName[0]) {
            int n = collect_targets(t, 64); ui_targets = n;
            if (camera_basis(R, &eye, &fov, vp)) {
                int cur  = index_of(t, n, curName);
                int best = pick_best(t, n, vp);
                bool curValid = (cur >= 0) && t[cur].onscreen && t[cur].dist <= MAX_DIST;

                if (!curValid) {
                    // Current target gone / off screen / out of range: fall
                    // back to the nearest player in the world, matching how
                    // acquisition works. If nobody qualifies, drop the lock.
                    int fallback = pick_best(t, n, vp);
                    cur = fallback;
                    if (fallback >= 0) snprintf(curName, sizeof curName, "%s", t[fallback].name);
                    else               curName[0] = '\0';
                    challName[0] = '\0';
                } else if (best >= 0 && best != cur &&
                           target_score(t[best], vp) <
                               SWITCH_FRACTION * target_score(t[cur], vp)) {
                    // A clearly-closer challenger. Start, or keep, timing it;
                    // commit only once it has held the lead for the dwell.
                    auto now = std::chrono::steady_clock::now();
                    if (strcmp(challName, t[best].name) != 0) {
                        snprintf(challName, sizeof challName, "%s", t[best].name);
                        challSince = now;
                    } else if (std::chrono::duration<double, std::milli>(now - challSince).count()
                               >= SWITCH_DWELL_MS) {
                        snprintf(curName, sizeof curName, "%s", t[best].name);
                        cur = best;
                        challName[0] = '\0';
                        snprintf(b, sizeof b, "switched to %s (%.0f studs)", curName, t[cur].dist);
                        fx_log(b);
                    }
                } else {
                    // No qualifying challenger this tick -- reset the clock.
                    challName[0] = '\0';
                }

                // Aim at the current target -- but NOT while a challenger is
                // being timed, or the pull would fight you back onto the old one
                // and you could never establish the new target. Holding still
                // during those few ms is invisible; committing resumes the pull
                // on the new target immediately.
                if (cur >= 0 && challName[0] == '\0') {
                    const Target& g = t[cur];
                    float3 lead = { g.world.x + g.vel.x * LEAD_SECONDS,
                                    g.world.y + g.vel.y * LEAD_SECONDS,
                                    g.world.z + g.vel.z * LEAD_SECONDS };
                    aim_from(eye, eye, lead, SMOOTHING);
                }
            }
        }

        // ---- ESP overlay update -----------------------------------------
        // Throttled to ~60 Hz so we don't spam the overlay pipe when aiming
        // is polling at 1 kHz. When NOT aiming we still tick every 16 ms so
        // markers stay live even without RMB held.
        static auto last_ovl = std::chrono::steady_clock::now();
        auto now_ovl = std::chrono::steady_clock::now();
        double since = std::chrono::duration<double, std::milli>(
                           now_ovl - last_ovl).count();
        if (since >= 16.0) {
            last_ovl = now_ovl;
            // Fresh target list if the aim path didn't just build one.
            if (!aiming || !curName[0]) {
                (void)collect_targets(t, 64);
            }
            // Same rule as esp_overlay.cpp: boxes are in MONITOR pixels, so
            // while the game is behind another window they would float over
            // whatever is now in front of it. Cleared once on the way out
            // rather than every frame.
            if (!fx_focused()) {
                if (ovl_shown) { fx_overlay_post("[]"); ovl_shown = false; }
            } else {
            ovl_shown = true;
            int rbx_x = 0, rbx_y = 0, rbx_w = 0, rbx_h = 0;
            if (get_rbx_window(&rbx_x, &rbx_y, &rbx_w, &rbx_h)) {
                int n = collect_targets(t, 64); ui_targets = n;
                char json[8192];
                int o = snprintf(json, sizeof json, "[");
                bool first = true;
                for (int i = 0; i < n && o + 160 < (int)sizeof json; i++) {
                    if (!t[i].onscreen || t[i].dist > MAX_DIST) continue;

                    // Identical to esp_overlay.cpp, deliberately: same anchor
                    // (root part), same +-3 stud size anchors, same centring.
                    // The previous version hung the box off the HEAD and ran it
                    // downward, which put it lower and sized it differently at
                    // the same distance -- the two ESPs never lined up.
                    //
                    // The root part is projected fresh rather than reusing
                    // t[i].sx/sy: those are the HEAD's pixels, computed for the
                    // aim path.
                    float bsx, bsy;
                    if (!project_to_screen(t[i].hrp, &bsx, &bsy)) continue;

                    float3 top = { t[i].hrp.x, t[i].hrp.y + 3.0f, t[i].hrp.z };
                    float3 bot = { t[i].hrp.x, t[i].hrp.y - 3.0f, t[i].hrp.z };
                    float tsx, tsy, fsx, fsy;
                    if (!project_to_screen(top, &tsx, &tsy)) continue;
                    if (!project_to_screen(bot, &fsx, &fsy)) continue;

                    float box_h = fabsf(fsy - tsy);
                    if (box_h < 6.0f) box_h = 6.0f;
                    float box_w = box_h * 0.6f;

                    o += snprintf(json + o, sizeof json - o,
                                  "%s{\"n\":\"%s\",\"sx\":%.1f,\"sy\":%.1f,"
                                  "\"w\":%.1f,\"h\":%.1f,\"d\":%.1f,\"c\":%d}",
                                  first ? "" : ",", t[i].name,
                                  bsx - box_w * 0.5f + rbx_x,
                                  bsy - box_h * 0.5f + rbx_y,
                                  box_w, box_h, t[i].dist,
                                  t[i].team ? TEAM_RGB : 0xFFFFFF);
                    first = false;
                }
                snprintf(json + o, sizeof json - o, "]");
                fx_overlay_post(json);
            }
            }   // focused
        }

        // Aiming polls fast (1 ms) for camera-write responsiveness. Idle paces
        // to 60 Hz instead of sleeping 16 ms.
        //
        // The difference is not cosmetic: collect_targets walks every player
        // and projects each one, which costs real milliseconds, and sleeping 16
        // ON TOP of that put the idle loop well under the 60 Hz the old comment
        // claimed -- esp_overlay.cpp measured ~47 Hz before the same change.
        // fx_pace waits until the next period BOUNDARY, so the work comes out
        // of the wait rather than being added to it.
        //
        // Safe to alternate with fx_sleep: fx_pace resyncs when its deadline is
        // more than a period stale, which it always is coming back from a burst
        // of aiming.
        // Both branches PACE now. The aiming branch used to sleep 1 ms, which
        // is not a rate at all: it added 1 ms to however long the tick took,
        // so the loop ran as fast as the reads allowed. Pacing puts a ceiling
        // on it and takes the work out of the wait, the same way the idle
        // branch already did.
        // ---- settings panel -------------------------------------------------
        // set:<key>=<value>, reset, ready, dbg:<msg>. Drained every tick so a
        // slider drag feels immediate; the state push below is throttled
        // because nothing on the panel needs 144 Hz.
        while (const char* m = fx_window_get_msg()) {
            std::string msg = m;
            if (msg.rfind("set:", 0) == 0) {
                size_t eq = msg.find('=', 4);
                if (eq != std::string::npos) {
                    std::string k = msg.substr(4, eq - 4);
                    double v = atof(msg.c_str() + eq + 1);
                    ui_status = cfg_set(k, v) ? (k + " = " + msg.substr(eq + 1))
                                              : ("unknown setting " + k);
                }
            } else if (msg == "reset") {
                cfg_reset();
                ui_status = "settings reset";
            } else if (msg == "ready") {
                ui_status.clear();
            } else if (msg.rfind("dbg:", 0) == 0) {
                fx_log(("panel: " + msg.substr(4)).c_str());
            }
            // Answer immediately rather than waiting for the throttle, so the
            // panel reflects a clamp the moment it happens.
            fx_window_eval(ui_state_js(curName, ui_targets, aiming).c_str());
            if (!ui_status.empty()) {
                std::string js = "S(\"";
                for (char c : ui_status) {
                    if (c == '"' || c == '\\') js += '\\';
                    js += c;
                }
                fx_window_eval((js + "\")").c_str());
            }
        }

        auto now_ui = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now_ui - ui_last).count() >= 100) {
            ui_last = now_ui;
            fx_window_eval(ui_state_js(curName, ui_targets, aiming).c_str());
        }

        if (!fx_pace(aiming && curName[0] ? AIM_HZ : ESP_HZ)) break;
    }
    // Settings are written through on every change, but the host coalesces file
    // writes -- so flush the last one before the run ends.
    cfg_save();
    fx_window_close();
    fx_overlay_off();
    fx_log("aim assist stopped");
}
