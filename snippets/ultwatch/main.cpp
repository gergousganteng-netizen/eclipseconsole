// Ult watch -- toasts when someone goes Saitama, and while Death Counter is up.
//
// READ ONLY. It watches instances and draws a panel; nothing here writes to the
// client, so the arm gate never comes into it.
//
// ---------------------------------------------------------------------------
// TWO EVENTS, TWO DIFFERENT SIGNALS, and the second one is not where you would
// first look.
//
// 1. THE ULT is a SET change in the BACKPACK. Going Saitama replaces the whole
//    moveset, so the four Tools become exactly:
//
//        Death Counter | Table Flip | Serious Punch | Omni Directional Punch
//
//    and revert when it ends. "The set became the Saitama set" is the ult
//    starting -- no per-move guessing, and it fires when they pop it rather
//    than when they first swing. Tools genuinely replicate, so this works for
//    every player.
//
// 2. DEATH COUNTER is the `Counter` ACCESSORY on the CHARACTER, not anything in
//    the backpack. TSB tags character state with Accessories -- `M1ing`,
//    `NoBlock`, `NoJump`, `StopRunning`, `Freeze`, `M1CD`, `Counter` -- and
//    those replicate to everyone.
//
//    DO NOT go back to detecting a move by watching the Tool reorder to the
//    bottom of the backpack. That was the first implementation and it is only
//    ever right for YOURSELF. Measured, both players in one 3ms loop, each
//    pressing Death Counter:
//
//        LOCAL   [13940] Table Flip | Serious Punch | Omni | Death Counter
//                [14013] (empty)                <- reorder held for 73ms
//                [14117] Normal Punch | ...
//        REMOTE  [16500] (empty)                <- NO reorder. ever.
//                [16611] Normal Punch | ...
//
//    Backpack child ORDER does not replicate. The remote goes straight from the
//    Saitama set to empty; the reorder that carries "a move was used" simply
//    does not exist on our client. That is why Death Counter appeared to work
//    and then not -- it was only ever working on the local player.
//
//    The accessory is a LEVEL, not an edge, and its lifetime is exactly 10s:
//        local  6605 -> 16605  = 10000ms
//        remote 12307 -> 22304 =  9997ms
//    So the countdown below is real remaining time, and the toast is cut short
//    if the accessory clears early (they died, the round ended).

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

// The Saitama moveset. The set is the ult; matching is exact and order-free.
static const char* SAITAMA[] = {
    "Death Counter",
    "Table Flip",
    "Serious Punch",
    "Omni Directional Punch",
};

// The character tag that means Death Counter is armed. Measured at 10.000s,
// but the toast follows the accessory rather than trusting this number -- it
// is only how long the bar is drawn for.
static const char* COUNTER_TAG = "Counter";
const double   DC_SECONDS = 10.0;

const unsigned POLL_MS   = 15;
const unsigned ROSTER_MS = 1000;
// The tag scan walks every child of every character, so it gets its own slower
// clock -- 50ms is nothing against a 10s window, and it keeps that walk out of
// the 15ms loop the backpack needs.
const unsigned TAG_MS    = 50;

struct Watch {
    uint64_t                 player   = 0;
    uint64_t                 backpack = 0;
    uint64_t                 model    = 0;
    int64_t                  userid   = 0;
    bool                     av_sent  = false;   // headshot handed to the page
    std::string              name;      // username -- the stable identity
    std::string              display;   // DisplayName -- what people call them
    bool                     local    = false;
    bool                     primed   = false;
    bool                     saitama  = false;   // was the ult up, confirmed
    bool                     counter  = false;   // was Death Counter armed
    // Debounce. A moveset is assembled tool by tool, so mid-swap the backpack
    // is briefly a PARTIAL list -- measured as "Death Counter | Table Flip"
    // for 11ms before the other two arrived, and as an empty list for ~100ms
    // between movesets. Acting on that reads as losing the ult and instantly
    // regaining it, so every single ult would emit a spurious lost/gained
    // pair. A set must hold still before it counts.
    std::vector<std::string> pend;
    int                      pend_n = 0;
};

// Consecutive polls a set must survive before it is believed. 3 x POLL_MS is
// ~45ms -- four times the measured 11ms rebuild, and far under any human
// re-ult.
const int STABLE_POLLS = 3;

static double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

static uint64_t find_backpack(uint64_t player) {
    uint64_t n = fx_nchildren(player);
    for (uint64_t i = 0; i < n; i++) {
        uint64_t c = fx_child(player, (int)i);
        // By CLASS, not by name: a game may rename it, it cannot change what
        // it is.
        if (c && strcmp(fx_class(c), "Backpack") == 0) return c;
    }
    return 0;
}

// Is the character carrying a named state tag? TSB implements these as
// Accessories parented to the character.
static bool has_tag(uint64_t model, const char* tag) {
    if (!model) return false;
    uint64_t n = fx_nchildren(model);
    for (uint64_t i = 0; i < n && i < 64; i++) {
        uint64_t c = fx_child(model, (int)i);
        if (!c) continue;
        // Name first: it is the cheaper read and almost always rejects.
        if (strcmp(fx_name(c), tag) == 0 &&
            strcmp(fx_class(c), "Accessory") == 0) return true;
    }
    return false;
}

static std::vector<std::string> snapshot(uint64_t bp) {
    std::vector<std::string> v;
    uint64_t n = fx_nchildren(bp);
    for (uint64_t i = 0; i < n && i < 16; i++) {
        uint64_t t = fx_child(bp, (int)i);
        if (!t) continue;
        const char* nm = fx_name(t);
        v.push_back(nm ? nm : "");
    }
    return v;
}

static bool same_set(std::vector<std::string> a, std::vector<std::string> b) {
    if (a.size() != b.size()) return false;
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    return a == b;
}

static bool is_saitama(const std::vector<std::string>& v) {
    std::vector<std::string> s;
    for (const char* n : SAITAMA) s.push_back(n);
    return same_set(v, s);
}

static void js_escape(const std::string& in, std::string& out) {
    for (char c : in) {
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c >= 32)   out += c;
    }
}

extern "C" void fx_run() {
    fx_window_open("Ult watch", 430, 320);
    fx_window_navigate((std::string(fx_snippets_dir()) + "\\ultwatch\\ui.html").c_str());
    fx_window_dock(1, FX_DOCK_BR, 14, 14);
    // Nothing here is clickable, and the panel is most of a corner of the
    // screen -- without this it would be a 430x320 hole in the game's input.
    fx_window_passthrough(1);

    fx_log("ult watch: watching for Saitama + Death Counter. Stop to end.");

    // T(display, username, kind, seconds, isLocal)
    //   kind 0 = has the ult, 1 = Death Counter armed, 2 = lost the ult
    auto toast = [&](const Watch& x, int kind, double secs) {
        char b[48];
        std::string js = "T(\"";
        js_escape(x.display, js);
        js += "\",\"";
        js_escape(x.name, js);
        snprintf(b, sizeof b, "\",%d,%.0f,%d)", kind, secs, x.local ? 1 : 0);
        js += b;
        fx_window_eval(js.c_str());
    };
    // The player's headshot URL, once fx_avatar_url has it. Sent as it lands
    // rather than when a toast fires: the page keys them by username, so by the
    // time someone ults the picture is already there and the card renders
    // complete instead of popping an image in a beat late.
    auto avatar = [&](const Watch& x, const char* url) {
        std::string js = "A(\"";
        js_escape(x.name, js);
        js += "\",\"";
        js_escape(url, js);
        js += "\")";
        fx_window_eval(js.c_str());
    };
    // The counter cleared. Ends that player's countdown early rather than
    // letting it run down to a time that is no longer true.
    auto dc_over = [&](const Watch& x) {
        std::string js = "D(\"";
        js_escape(x.name, js);
        js += "\")";
        fx_window_eval(js.c_str());
    };

    std::vector<Watch> w;
    unsigned gen = fx_gen();
    // NOTHING IS SENT BEFORE THE PAGE SAYS IT IS THERE. fx_window_navigate is
    // asynchronous, so the page takes ~100ms to exist, while the first roster
    // pass runs at t=0 and a CACHED avatar comes back from disk instantly. Every
    // one of those evals landed in a document that had not loaded ui.js yet and
    // was silently dropped -- and since av_sent was already true it was never
    // re-sent, so the players most certainly cached (you, above all) were
    // exactly the ones that never got a picture. It reads as "avatars are
    // broken" while the uncached ones, resolving a second later, work fine.
    bool   ready    = false;
    double start_ms = now_ms();
    // FAILS OPEN. A handshake that can never complete is a worse bug than the
    // one it fixes -- it would leave the snippet silent forever with nothing
    // logged. If the page has not spoken in 3s, carry on without it: the
    // avatars may be missing, but the detection still runs.
    const double READY_WAIT_MS = 3000;
    double   next_roster = 0;
    double   next_tag    = 0;

    while (fx_running()) {
        // Pumped FIRST, so `ready` is acted on the tick it arrives.
        while (const char* m = fx_window_get_msg()) {
            std::string s = m;
            if      (s == "ready")             { if (!ready) ready = true; }
            else if (s.rfind("dbg:", 0) == 0)  fx_logf("panel: %s", s.substr(4).c_str());
        }
        if (!ready && now_ms() - start_ms < READY_WAIT_MS) {
            if (!fx_sleep(POLL_MS)) break;
            continue;
        }
        if (!ready) {
            ready = true;
            fx_log("ult watch: panel never reported ready - avatars may not show");
        }

        // The world changed underneath us: every cached pointer is suspect,
        // so drop the lot and rebuild. fx_gen is the superset -- respawn,
        // leaving the place, joining another, detaching -- so this one check
        // covers both a reset and a server hop.
        if (fx_gen() != gen) {
            gen = fx_gen();
            w.clear();
            next_roster = 0;
            next_tag    = 0;
        }

        // Rebuilt occasionally rather than every tick: people join at human
        // speed, and the backpack walk under this is not free.
        if (now_ms() >= next_roster) {
            next_roster = now_ms() + ROSTER_MS;
            fx_player_info p[64];
            int n = fx_players(p, 64);
            for (int i = 0; i < n; i++) {
                Watch* found = nullptr;
                for (Watch& x : w) if (x.player == p[i].player) { found = &x; break; }
                if (found) {
                    // The MODEL is refreshed every pass, not just on join: it
                    // is replaced on every respawn, and a stale one reads tags
                    // off a character that no longer exists.
                    found->model = p[i].model;
                    // Same for the id: a Player object has a name before it
                    // has a UserId, so one seen at the menu joins with 0.
                    if (!found->userid) found->userid = p[i].userid;
                    continue;
                }
                Watch x;
                x.player = p[i].player;
                x.model  = p[i].model;
                x.userid = p[i].userid;
                x.name   = p[i].name;
                x.local  = p[i].is_local != 0;
                // DisplayName is what everyone actually reads; the username is
                // what identifies them. Show both, since in this game they are
                // routinely different.
                uint64_t oDN = fx_off("Player.DisplayName");
                if (oDN != (uint64_t)-1) {
                    char b[64] = "";
                    fx_str(x.player + oDN, b, sizeof b);
                    x.display = b;
                }
                if (x.display.empty()) x.display = x.name;
                w.push_back(x);
            }
            for (size_t i = 0; i < w.size(); ) {
                bool still = false;
                for (int k = 0; k < n; k++) if (p[k].player == w[i].player) { still = true; break; }
                if (still) i++;
                else       w.erase(w.begin() + i);
            }
        }

        for (Watch& x : w) {
            // Re-resolved until it ANSWERS, not once on an edge: after a
            // respawn the backpack does not exist yet, and resolving a single
            // time stores that 0 forever (fx.h says this about fx_gen; it is
            // the same trap one level down).
            if (!x.backpack) {
                x.backpack = find_backpack(x.player);
                if (!x.backpack) continue;
            }

            std::vector<std::string> cur = snapshot(x.backpack);
            if (cur.empty()) {
                // AN EMPTY READ MEANS THE POINTER IS DEAD, not that the player
                // is carrying nothing -- a live Backpack in this game always
                // holds a moveset. This is on top of fx_gen, not instead of
                // it: fx_gen tracks YOUR character, so ANOTHER player's
                // respawn moves nothing. Measured -- one player's backpack was
                // freed 50ms before fx_gen ticked for an unrelated reason, and
                // the stale pointer went on returning a full four-item list.
                x.backpack = 0;
                continue;
            }

            // ---- debounce the SET ------------------------------------------
            if (!same_set(cur, x.pend)) { x.pend = cur; x.pend_n = 1; }
            else                        { x.pend_n++; }
            bool set_stable = x.pend_n >= STABLE_POLLS;

            bool sait = is_saitama(cur);

            if (!x.primed) {
                // First sight reports the STATE, it does not stay silent.
                //
                // Firing only on the transition INTO the set was a bug: an alt
                // that is already Saitama when the snippet starts never
                // transitions, so it never reported and the whole thing looked
                // broken. What you want to know is who HAS it, which includes
                // whoever had it before you were watching.
                x.saitama = sait;
                x.primed  = true;
                if (sait) {
                    toast(x, 0, 0);
                    fx_logf("%s has Saitama ult", x.name.c_str());
                }
                continue;
            }

            // ---- gained the ult --------------------------------------------
            // Gated on the set having held still, so a half-built moveset
            // cannot announce anything.
            if (set_stable && sait && !x.saitama) {
                toast(x, 0, 0);
                fx_logf("%s has Saitama ult", x.name.c_str());
            }
            // ---- lost it ----------------------------------------------------
            // Worth saying out loud: knowing the threat is OVER is as useful as
            // knowing it started, and without this the last toast you saw stays
            // the last thing you believe.
            if (set_stable && !sait && x.saitama) {
                toast(x, 2, 0);
                fx_logf("%s lost Saitama ult", x.name.c_str());
            }

            if (set_stable) x.saitama = sait;

        }

        // ---- Death Counter, off the character tag --------------------------
        // Its own pass over every player, NOT folded into the backpack loop
        // above and NOT gated on the ult being up, however tempting that is as
        // an optimisation. USING Death Counter TEARS THE ULT DOWN: measured,
        // the backpack reverts to the normal moveset 73ms after the press,
        // while the Counter accessory still has a further 10s to run. Gating on
        // the ult stops the scan the instant it starts mattering, and the toast
        // reports "over" immediately. The slower TAG_MS clock pays for the walk
        // instead.
        if (now_ms() >= next_tag) {
            next_tag = now_ms() + TAG_MS;
            // Headshots, on this clock rather than the roster's: it is a map
            // lookup behind a mutex until the id resolves, so asking often is
            // free, and a player who joins and ults immediately still gets a
            // picture. The page backfills anything that fired before it landed.
            for (Watch& x : w) {
                if (!x.av_sent && x.userid) {
                    char u[256];
                    if (fx_avatar_url(x.userid, u, sizeof u)) {
                        avatar(x, u); x.av_sent = true;
                    }
                }
            }
            for (Watch& x : w) {
                bool counter = has_tag(x.model, COUNTER_TAG);
                if (counter && !x.counter) {
                    toast(x, 1, DC_SECONDS);
                    fx_logf("%s: Death Counter active", x.name.c_str());
                } else if (!counter && x.counter) {
                    dc_over(x);
                }
                x.counter = counter;
            }
        }

        if (!fx_sleep(POLL_MS)) break;
    }

    fx_window_close();
    fx_log("ult watch: stopped");
}
