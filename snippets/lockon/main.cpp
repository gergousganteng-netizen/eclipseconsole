// Lock-on. Press the toggle once to lock the target nearest your crosshair,
// press it again to let go. The difference from the aimbot is that this picks a
// target ONCE and then follows that one character -- through cover, off screen,
// behind you -- until you drop it, it leaves, or the world changes underneath.
//
// It turns you HORIZONTALLY only. The up/down angle stays yours the whole time.
//
//   E      lock on / let go
//   F7     report what it can see, and what it is holding
//   F8     MEASURE a held lock: where the camera actually ended up, and the
//          shift lock offset it is correcting for. Start here when the aim
//          looks wrong -- it separates bad arithmetic from a lost rewrite race.
//   Stop   ends it
//
// This WRITES Camera.Rotation, so it obeys the arm gate like everything else:
// disarmed it logs the write and the camera does not move.
//
// Run it disarmed FIRST, but expect the log to fill while a lock is held -- the
// gate reports every write it refuses. F7 is the quiet way to check that it is
// picking and holding the right person before you let it touch anything.
#include "lock.h"
#include <chrono>

const int   TOGGLE_KEY = 'E';    // any key: 'Q', VK_F8, VK_XBUTTON2, ...
const bool  CLAIM_TOGGLE = true; // swallow it, so the game never sees the press

// 0..1 per tick. 1.0 snaps.
//
// Left at 1.0 on purpose. Smoothing is a low-pass filter on the aim, and a
// low-pass filter is exactly a lag -- against a STANDING target that lag costs
// nothing and looks better, but against a moving one it is the difference
// between tracking them and following them. Lower it for feel once the tracking
// is right, not before.
const float SMOOTHING = 1.0f;

// How far ahead of the target to aim, in seconds of their own velocity.
//
// This is the fix for "it lags behind them when they move", and smoothing is
// not: no amount of snapping harder helps, because the error is not that the
// camera is slow to reach where they WERE. Between reading their position and
// the client drawing the frame, they keep moving -- a player strafing at 16
// studs/s at 20 studs range sweeps about 45 degrees a second, so even 30 ms of
// round trip is a degree and a half behind, permanently.
//
// Aiming at position + velocity * LEAD cancels that. Too small and it still
// trails; too large and the crosshair sits in front of them and swings wide
// whenever they change direction. 0.06 is a starting point -- F8 reports their
// speed and how far the lead is actually displacing the aim, which is the
// honest way to tune it.
const float LEAD_SECONDS = 0.06f;

const float MAX_DIST = 400.0f;   // studs. Filters ACQUISITION only -- once
                                 // locked, the target can run as far as it likes.

// Aim from the character, not the camera. This is the shift lock correction:
// see the long comment on aim_from(). Set false to aim the camera itself at the
// target, which centres them on screen but leaves your character facing past
// them by the width of the shift lock offset.
const bool AIM_FROM_CHARACTER = true;

// Turn only. The lock holds the yaw and never touches the vertical angle, which
// stays entirely yours -- look up, look down, look at the floor, and the lock
// keeps facing them the whole time without arguing about it. Yaw is also the
// only axis the character's facing follows, so the pitch was never part of what
// a lock is for.
//
// Set false to have it pitch at the target as well, which centres them
// vertically but takes the up/down axis off you while the lock is held.
const bool HORIZONTAL_ONLY = true;

// The camera has to be rewritten faster than the client rewrites it, or the
// write is simply reverted before it is drawn. The client recomputes the camera
// EVERY frame from its own stored angles -- our write does not update those, so
// this is a competition rather than a replacement. At 16 ms against a 144 fps
// client we were writing once per two frames and losing both of the others,
// which looks like the camera twitching toward the target and springing back.
//
// Raise it and the fight becomes visible again; that is the trade.
const unsigned TICK_MS = 0.75;

// Long enough to ride out a single hitched read, short enough that a respawn
// releases rather than hangs on.
//
// Measured as TIME, not as a count of missed iterations. The old form was
//
//     MAX_MISSES = GRACE_MS / (TICK_MS ? TICK_MS : 1)
//
// which quietly took the `: 1` branch, because TICK_MS is 0 -- so the grace
// was 150 ITERATIONS. This loop has no fixed rate; with no sleep in it, 150
// iterations elapse in microseconds, and a single hitched read was enough to
// drop the lock. A timestamp is correct whatever the tick does, and stays
// correct if the tick ever changes.
const unsigned GRACE_MS = 150;

extern "C" void fx_run() {
    char b[192];
    Target t[64];

    uint64_t locked = 0;
    char     lockedName[48] = "";
    // When the target first went unreadable, or `missing` false if it is
    // readable right now. Cleared on every good read, so the grace applies to
    // a CONTINUOUS outage rather than accumulating across unrelated hitches.
    bool missing = false;
    std::chrono::steady_clock::time_point missSince{};

    // Claimed only while the gate is open -- see the loop. Starts unclaimed so
    // the first pass through decides, rather than swallowing the key for one
    // iteration before anything has checked whether it should.
    bool claimed = false;

    fx_log("lock-on running - press the toggle to lock, F7 to report, Stop to end");

    float R[9], vp[2], fov;
    float3 eye;
    if (!camera_basis(R, &eye, &fov, vp)) {
        fx_log("x cannot read the camera - are you in a place?");
        return;
    }

    unsigned gen = fx_gen();
    unsigned dm  = fx_dm_gen();

    while (fx_running()) {
        // Hold the claim ONLY while the gate is open.
        //
        // A claim is global, not per-window: while it is on, that key reaches
        // nothing at all. With TOGGLE_KEY bound to a letter that means you
        // could not type the letter in chat, and could not type it in another
        // application while alt-tabbed either -- the snippet would look like a
        // broken keyboard. Toggled only on change, so this is one call per
        // focus/chat transition rather than one per frame.
        if (CLAIM_TOGGLE) {
            bool want = fx_input_ok();
            if (want != claimed) { fx_claim_key(TOGGLE_KEY, want); claimed = want; }
        }

        // The DataModel was replaced, which frees the camera this writes to.
        // Worth checking here more than anywhere else in the project: this is
        // the one loop that WRITES, and writing through a freed pointer is the
        // difference between a stale read and a corrupted client.
        if (fx_dm_gen() != dm) {
            dm = fx_dm_gen();
            gen = fx_gen();
            if (locked) { locked = 0; fx_log("lock dropped - world replaced"); }
            fx_log("holding off until it settles");
            if (!fx_sleep(500)) break;
            continue;
        }

        // Anything cached may have been freed -- our respawn, a rejoin. The
        // locked Player is exactly such a cached pointer, so it goes too. It
        // costs one re-press after each of your own deaths, which is the right
        // side to err on: the alternative is aiming through a dangling pointer.
        if (fx_gen() != gen) {
            gen = fx_gen();
            if (locked) {
                locked = 0;
                fx_log("lock dropped - the world changed under it, press again");
            }
        }

        // fx_key_pressed FIRST, then the gate: it consumes one press per call,
        // so testing the gate first would swallow presses made while a chat box
        // was open or the game was in the background, then lose the next real
        // one.
        //
        // Only the action key is gated. F7/F8 below are report keys -- you
        // cannot type them into a chat box by accident, and blocking them would
        // just make the thing harder to diagnose exactly when you need to.
        if (fx_key_pressed(TOGGLE_KEY) && fx_input_ok()) {
            if (locked) {
                snprintf(b, sizeof b, "released %s", lockedName);
                fx_log(b);
                locked = 0;
            } else {
                int n = collect_targets(t, 64);
                if (!camera_basis(R, &eye, &fov, vp)) n = 0;

                // Nearest to the CROSSHAIR, not nearest in the world: the one
                // you are already roughly pointing at is the one you meant.
                float cx = vp[0] * 0.5f, cy = vp[1] * 0.5f;
                int best = -1;
                float bestd2 = 1e18f;
                for (int i = 0; i < n; i++) {
                    if (!t[i].onscreen || t[i].dist > MAX_DIST) continue;
                    float dx = t[i].sx - cx, dy = t[i].sy - cy;
                    float d2 = dx*dx + dy*dy;
                    if (d2 < bestd2) { bestd2 = d2; best = i; }
                }
                if (best < 0) {
                    fx_log("nothing on screen to lock");
                } else {
                    locked = t[best].player;
                    snprintf(lockedName, sizeof lockedName, "%s", t[best].name);
                    missing = false;
                    snprintf(b, sizeof b, "locked %s  (%.0f studs)",
                             lockedName, t[best].dist);
                    fx_log(b);
                }
            }
        }

        // Measure, do not assume. Reports where the camera actually ended up
        // after the client had its say, which is the one thing the aim maths
        // cannot tell you about itself:
        //
        //   yaw err  small and steady -> the aim is landing; anything left is
        //            the shift lock parallax, which is meant to be there
        //   yaw err  large or jumping -> the client is winning the rewrite, and
        //            the fix is a shorter TICK_MS, not different arithmetic
        //   side     the measured shift lock offset. ~1.75 with it on, ~0 off.
        //            If this reads 0 while you are shift locked, the correction
        //            has nothing to correct and something else is wrong.
        if (fx_key_pressed(VK_F8)) {
            float3 tp, tv, me;
            if (!locked) {
                fx_log("nothing locked - F8 measures a held lock");
            } else if (!aim_point(locked, &tp, &tv) || !local_origin(&me)) {
                fx_log("cannot read the target or our own character right now");
            } else {
                float err = 0, side = 0;
                if (aim_report(me, tp, &err, &side)) {
                    float3 d = v_sub(tp, me);
                    float dist  = sqrtf(v_dot(d, d));
                    float speed = sqrtf(v_dot(tv, tv));
                    snprintf(b, sizeof b,
                             "%s: yaw err %.2f deg | %.0f studs | speed %.1f | lead %.2f studs | cam %.2f right",
                             lockedName, err, dist, speed, speed * LEAD_SECONDS, side);
                    fx_log(b);
                }
            }
        }

        if (fx_key_pressed(VK_F7)) {
            int n = collect_targets(t, 64);
            int vis = 0;
            for (int i = 0; i < n; i++) if (t[i].onscreen) vis++;
            if (locked) {
                float3 tp;
                if (aim_point(locked, &tp, nullptr)) {
                    float3 me;
                    float3 from = local_origin(&me) ? me : eye;
                    float3 d = v_sub(tp, from);
                    snprintf(b, sizeof b, "%d players, %d on screen - holding %s at %.0f studs",
                             n, vis, lockedName, sqrtf(v_dot(d, d)));
                } else {
                    snprintf(b, sizeof b, "%d players, %d on screen - holding %s (out of reach)",
                             n, vis, lockedName);
                }
            } else {
                snprintf(b, sizeof b, "%d players, %d on screen - nothing locked", n, vis);
            }
            fx_log(b);
        }

        if (locked) {
            float3 tp, tv, me;
            bool haveT = aim_point(locked, &tp, &tv);
            bool haveM = local_origin(&me);
            if (!camera_basis(R, &eye, &fov, vp)) { if (!fx_sleep(TICK_MS)) break; continue; }

            if (haveT && (haveM || !AIM_FROM_CHARACTER)) {
                missing = false;
                // Where they are GOING, not where they are. See LEAD_SECONDS.
                float3 lead = float3{ tp.x + tv.x * LEAD_SECONDS,
                                      tp.y + tv.y * LEAD_SECONDS,
                                      tp.z + tv.z * LEAD_SECONDS };
                // Falling back to the camera for yaw is worse aim, not no aim:
                // it is off by the shift lock offset, which is the whole point
                // of the other branch. Better than dropping the lock while our
                // OWN character is mid-respawn.
                float3 yawFrom = (AIM_FROM_CHARACTER && haveM) ? me : eye;
                aim_from(yawFrom, HORIZONTAL_ONLY ? nullptr : &eye, lead, SMOOTHING);
            } else {
                // Start the clock on the first bad read, and only release once
                // the outage has lasted GRACE_MS.
                auto now = std::chrono::steady_clock::now();
                if (!missing) { missing = true; missSince = now; }
                else if (std::chrono::duration_cast<std::chrono::milliseconds>(
                             now - missSince).count() > (long long)GRACE_MS) {
                    // Their character is gone -- killed, respawning, or streamed
                    // out. Releasing is the wanted behaviour: a lock that survived
                    // a death would silently re-attach to whatever model came back,
                    // which is a different person's worth of surprise.
                    snprintf(b, sizeof b, "released %s - they respawned or left", lockedName);
                    fx_log(b);
                    locked = 0;
                    missing = false;
                }
            }
            if (!fx_sleep(TICK_MS)) break;
        } else {
            // Idle costs nothing: the wake is the keystroke itself, not the end
            // of a sleep, so the toggle stays instant without a spin loop.
            if (!fx_wait_key(200)) break;
        }
    }

    if (CLAIM_TOGGLE) fx_claim_key(TOGGLE_KEY, 0);
    fx_log("lock-on stopped");
}
