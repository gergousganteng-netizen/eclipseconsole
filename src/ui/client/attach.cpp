// ---- attach / identity ----------------------------------------------------
// The attach flags, the serialising mutex, the identity fields and the
// place-title cache, split out of eclipse_ui.cpp. This TU owns that state
// outright; eclipse_ui.cpp drives it through attach.h and reports through
// push_state / ui_post / ui_log.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "eclipse_core.h"

#include "attach.h"
#include "host.h"
#include "log.h"
#include "offsets_fetch.h"
#include "placename.h"


// Who we are attached to, for the in-game panel's header. A pid is the right
// answer for a status bar you read while debugging and the wrong one for a
// glance mid-game: it says the console is attached to SOMETHING, not that it is
// attached to YOUR account -- which is the question worth answering when two
// clients are running.
//
// Resolved by the same enumeration the picker uses, and only when the pid or
// the DataModel changes: that walks every process, so it belongs behind a
// change check rather than in a push that fires on a timer.
// GUARDED by g_ident_mux: written by the watcher's tick, by do_attach (which
// runs on the UI thread or the watcher depending on who asked), and by the
// placename fetch thread (place_title_remember); copied out by ident_user /
// ident_place from whatever thread called push_state. Two writers on a
// std::string or std::map is heap corruption, not a stale read. Leaf lock:
// never held across push_state (which reads back through the accessors) or
// across the candidate walk (~100ms). NOT g_attach_mux -- push_state runs
// under that lock in do_attach, and the accessors would deadlock.
static std::mutex g_ident_mux;
static std::string g_ident_user;
static std::string g_ident_place;
static uint64_t    g_ident_placeid = 0;
static uint32_t    g_ident_pid = 0;
static uint64_t    g_ident_dm  = 0;
static std::map<uint64_t, std::string> g_place_titles;

void refresh_identity() {
    const ec::State& s = ec::state();

    // THIS IS THE PLACE/VERSION PATH NOW, not the name path. The name comes
    // from identity_tick, which reads LocalPlayer directly twice a second --
    // live, so a logout/login that changes the account without changing the
    // pid or the DataModel is seen, which this function structurally cannot
    // see. What is left here needs the candidate walk: the place id and the
    // title cache.
    //
    // Throttled rather than run every tick: enumerate_candidates walks every
    // process on the machine and reads through each one. The retry exists
    // because this used to read once, at the instant of joining, when nothing
    // is resolvable yet -- same shape as the geometry cache latching an empty
    // build: a lookup that runs at the worst possible moment and never asks
    // again.
    static DWORD s_retry = 0;
    {
        std::lock_guard<std::mutex> lk(g_ident_mux);
        if (s.pid == g_ident_pid && s.dm == g_ident_dm) {
            if (!g_ident_user.empty()) return;
            DWORD now = GetTickCount();
            if (now - s_retry < 250) return;
            s_retry = now;
        }
        g_ident_pid = s.pid;
        g_ident_dm  = s.dm;
        // THE NAME IS NOT CLEARED HERE, and that is the point. Rejoining
        // changes the DataModel, which lands in this function while
        // LocalPlayer does not exist yet -- so clearing first and refilling
        // from the walk left the header blank for the seconds in between. The
        // account did not change; our ability to read it did. Keep the last
        // verified name and let it be REPLACED when a new one is actually
        // read, here or in identity_tick.
        //
        // Detaching does clear it below: with no client there is no account,
        // and holding the name of a process that is gone would be a
        // different lie.
        g_ident_place.clear();
        g_ident_placeid = 0;
        if (!s.pid) { g_ident_user.clear(); return; }
    }
    // The walk runs OUTSIDE the lock: it reads through every process and can
    // take ~100ms, and it retries 4x a second while a name is unresolved.
    //
    // want != 0 means "this place has no title cached, ask the network" --
    // kicked below, after the lock is dropped. See the comment at the miss.
    uint64_t want = 0;
    for (const ec::Candidate& c : ec::enumerate_candidates(false)) {
        if (c.pid != s.pid) continue;
        // THE NAME IS NOT WRITTEN HERE ANY MORE. identity_tick owns it, and
        // two writers with different ideas of "the name" is what made the
        // header FLASH on leaving a game: this path wrote the display name,
        // and the tick overwrote it with Instance.Name up to 500ms later.
        // Whichever is right, it has to be one of them deciding.
        std::lock_guard<std::mutex> lk(g_ident_mux);
        g_ident_placeid = c.place_id;
        auto it = g_place_titles.find(c.place_id);
        if (it != g_place_titles.end()) g_ident_place = it->second;
        // The DataModel's own name is "LuaApp" in the menu and "Ugc" in a
        // game, so it is only useful as "are you actually in a place".
        //
        // A MISS HAS TO ASK. g_place_titles was filled by exactly one thing --
        // the page's candidate picker, which only renders when 2+ clients are
        // running -- so with a single client nothing ever requested a title,
        // the map stayed empty, and the panel's place chip was permanently
        // blank. The panel was a passenger on a multi-instance code path.
        // s_kicked bounds this to one request per place transition: the
        // enclosing gate re-walks 4x a second while the name is unresolved,
        // and without it every one of those would be a fresh HTTP pair.
        else if (c.place_id) {
            g_ident_place = "";
            static uint64_t s_kicked = 0;
            if (c.place_id != s_kicked) { s_kicked = c.place_id; want = c.place_id; }
        }
        break;
    }
    // OUTSIDE the lock on purpose: the fetch thread lands in
    // place_title_remember, which takes g_ident_mux.
    if (want) lookup_place_name_async(want);
}

// Serialises every attach/detach transition. Two paths ask for the initial
// attach at startup -- the page's READY handler and the liveness watcher, which
// is already running before the page loads -- and the watcher also detaches and
// refreshes on its own thread. Without one lock they interleave.
static std::mutex g_attach_mux;

// Multi-instance state.
//
//   g_locked_pid   the pid we are (or intend to be) bound to. Non-zero once
//                  the user or the auto-attach path has picked one. Cleared on
//                  detach so the app doesn't silently jump to a different
//                  Roblox after the one you were on exits.
//   g_pick_pending true if a multi-instance state is active. Enables the
//                  watcher's picker refresh so the modal's rows update as
//                  still-loading clients finish (usernames read as
//                  "(unknown)" until Players.LocalPlayer resolves).
//   g_multi_logged one-shot: whether the "multiple Roblox instances" warning
//                  has already been emitted this multi-instance session.
//                  Separate from g_pick_pending because the WATCHER can set
//                  the pending flag quietly before the page's READY handler
//                  ever runs -- and we still want the READY log to fire.
static uint32_t g_locked_pid    = 0;
// Whether "attaching..." has been said since we were last attached. Stops the
// watchdog's per-tick retries from filling the console with it.
static bool     g_announced_attaching = false;
// The user pressed Detach. Without this the watchdog below would re-attach on
// its very next tick, because "no pid and a client is running" is exactly the
// condition it exists to fix -- so a manual detach would undo itself within a
// second and look like the button did nothing. Cleared by any deliberate
// attach: pressing Attach, or picking a client from the candidates modal.
static std::atomic<bool> g_user_detached{ false };
static bool     g_pick_pending  = false;
static bool     g_multi_logged  = false;
// One full attach look has happened (found a client or found none). Until it
// has, "no client attached" means "we have not looked yet", and neither
// reveal_window's overlay-mode skip nor hud_tick's no-client valve may act on
// it -- acting early is exactly the desktop-window flash at startup the user
// reported (the tick showed the window ~16ms in, long before the first probe).
static std::atomic<bool> g_attach_probed{ false };
// Is there a Roblox client running AT ALL, attached or not? The sidebar
// readout distinguishes "nothing to attach to" from "something is there and we
// are not on it", and those are different things to a user: the first wants
// Launch, the second wants Attach. Only the detached branch of attach_poll
// writes it, because that branch already calls find_client() every tick -- so
// this costs nothing new, and while we ARE attached the answer is trivially
// yes (client_running reads the pid first).
static std::atomic<bool> g_client_seen{ false };

static void send_candidates(const std::vector<ec::Candidate>& cs) {
    std::ostringstream j;
    j << "[";
    for (size_t i = 0; i < cs.size(); i++) {
        const auto& c = cs[i];
        if (i) j << ",";
        j << "{\"pid\":" << c.pid
          << ",\"user\":\""  << json_escape(c.username)     << "\""
          << ",\"disp\":\""  << json_escape(c.display_name) << "\""
          << ",\"place\":\"" << json_escape(c.place)        << "\""
          << ",\"placeId\":" << c.place_id
          << ",\"uid\":"     << c.user_id
          << ",\"mon\":"     << c.monitor
          << ",\"ok\":"      << (c.readable ? "true" : "false")
          << "}";
    }
    j << "]";
    ui_post("CANDIDATES:" + j.str());
}

// The clients panel asked for a list. Unconditional -- every client, whatever
// the count -- where send_candidates' other callers only fire at 2+.
//
// ON ITS OWN THREAD, and not negotiable: enumerating costs ~100ms with two
// clients (nearly all of it duplicating handles out of the crash handlers) and
// it takes g_attach_mux, so doing it inline would stall the message pump for
// the length of the walk AND for however long an attach in flight holds the
// lock. The panel opens on the click and fills in when this lands.
//
// g_list_busy collapses a double-click into one walk. Not correctness -- two
// walks would agree -- but two 100ms lock holds back to back delay whatever
// else wants the lock, for a second answer nobody asked for.
static std::atomic<bool> g_list_busy{ false };

void list_clients_async() {
    if (g_list_busy.exchange(true)) return;
    std::thread([] {
        ec::mark_host_thread();   // enumerate_candidates reads every client
        {
            std::lock_guard<std::mutex> lk(g_attach_mux);
            // OFFSETS FIRST -- the same reason do_attach does it below, and the
            // same bug if it is left out: ec::detach() calls offset_clear(), so
            // after a Detach every read in the walk comes back BAD and every
            // client is reported as unreadable. Idempotent when they are
            // already loaded.
            ec::preload_offsets(ui_conf());
            send_candidates(ec::enumerate_candidates());
        }
        g_list_busy = false;
    }).detach();
}

void do_attach(bool quiet, bool force, uint32_t target_pid) {
    std::lock_guard<std::mutex> lk(g_attach_mux);
    g_attach_probed = true;   // an attach attempt is a full look, whatever it finds

    // OFFSETS FIRST, because enumerate_candidates below reads through them and
    // ec::attach -- which loads them -- does not run until after it.
    //
    // ec::detach() clears the table. That was invisible while the only way to
    // detach was the client exiting, but the Detach button makes it reachable
    // on purpose: attach, detach, press Attach, and the picker probed every
    // client with an empty offset table. Every field came back BAD, which the
    // probe honestly reported as "FakeDataModel slot reads 0 (client still
    // starting?)" -- about a client that had been in a game for ten minutes.
    //
    // Idempotent, so this costs nothing on the path where they are already in.
    ec::preload_offsets(ui_conf());

    // Already on the right live client? Whichever path got here first attached
    // it. Re-attaching tears down the handle and doubles the log. Just refresh
    // the UI. `force` is the manual Attach button; specific target_pid always
    // gets its own re-attach, since it means "switch to this one".
    if (!force && !target_pid && ec::state().pid && ec::alive()) {
        push_state();
        return;
    }

    // OPEN THE PICKER FIRST, PROBE AFTER.
    //
    // Enumerating costs ~100ms with two clients (measured: almost all of it in
    // duplicating handles out of the crash handlers), and doing that before
    // posting OPEN_PICKER meant the modal visibly lagged the click. The page
    // already holds a candidate list -- the watchdog refreshes it every tick
    // while a multi-instance state is known -- so when the user presses Attach
    // and we ALREADY know there are several clients, show the list now and let
    // the next refresh correct it.
    //
    // Only on that path. A first-ever detection still probes synchronously,
    // because there is nothing cached to show and guessing would be worse than
    // a pause.
    if (force && !target_pid && g_pick_pending) {
        ui_post("OPEN_PICKER:");

        // Then probe anyway, on its own thread. Returning here without one made
        // the modal instant but left it showing whatever the last background
        // refresh happened to hold, and silenced the per-candidate reasons
        // entirely -- g_pick_pending is always already set by the time anyone
        // presses Attach, so the explaining path had become unreachable.
        //
        // Takes g_attach_mux, so it simply waits for this call to return.
        std::thread([] {
            ec::mark_host_thread();   // enumerate_candidates reads every client
            std::lock_guard<std::mutex> lk(g_attach_mux);
            auto cs = ec::enumerate_candidates(true);
            if (cs.size() >= 2) send_candidates(cs);
        }).detach();
        return;
    }

    // No specific pid asked for: decide by how many Roblox instances exist.
    if (!target_pid) {
        // `force` is the user pressing Attach, which is the only case where
        // the modal opens -- so it is also the only case worth explaining.
        auto cs = ec::enumerate_candidates(force);
        if (cs.size() >= 2) {
            // Multi-instance. Cache the list on the page (never auto-opens),
            // and only actually show the modal when the user explicitly asked
            // (Attach button or status-bar chip). Startup or the watcher's
            // background refresh sends CANDIDATES silently.
            send_candidates(cs);
            if (force) ui_post("OPEN_PICKER:");
            g_pick_pending = true;
            // Log the first VISIBLE detection. force = user knows already
            // (modal opens); quiet = poll-driven retry; g_multi_logged =
            // already said it. All three suppress.
            if (!g_multi_logged && !quiet && !force) {
                g_multi_logged = true;
                ui_log("STATUS", "! multiple Roblox instances - Attach to pick one");
            }
            return;
        }
        if (cs.size() == 1) target_pid = cs[0].pid;
        // If empty, fall through to attach() -- it will log "not running"
        // in the non-quiet case.
    }

    // Said on the automatic path as well, but only ONCE per detached spell.
    // The watchdog retries every tick while a client starts, so announcing each
    // attempt would be a column of "attaching..." -- and staying silent left a
    // gap where the app looked idle for the seconds it spends waiting.
    if (!quiet) ui_log("STATUS", "attaching...");
    else if (!g_announced_attaching) {
        g_announced_attaching = true;
        ui_log("STATUS", "attaching...");
    }
    // A real (re)attach past this point -- the "already on the right client"
    // early return above handled the no-op case. If we were attached, any
    // running snippet holds pointers into THAT client; stop them all before the
    // handle, base and offsets change underneath them (the client-swap case in
    // docs/multi-run.md's lifecycle).
    if (ec::state().pid) ec::stop_all_runs();

    // Report on the RETURN VALUE, not on pid: a partly-completed attach can
    // leave pid set while base is still 0x0, which is not an attach worth
    // announcing.
    bool ok = ec::attach(ui_conf(), quiet, target_pid);
    if (ok) {
        g_announced_attaching = false;      // next detached spell announces again
        g_pick_pending = false;
        g_multi_logged = false;

        // The fetch happens BEFORE anything is reported about the client.
        // attach() has already resolved the DataModel using whatever conf was
        // on disk, so on a genuine mismatch that result came through the wrong
        // offsets -- announcing "DataModel verified" from it would be stating
        // a fact we are about to invalidate and re-derive. Fetch, re-attach,
        // then report once from the state that is actually true.
        //
        // Tried ONCE per version: a version with no published dump would
        // otherwise mean a failed request on every re-attach and every watcher
        // tick, which is a request loop against someone else's server for an
        // answer that will not change.
        static std::string tried;
        {
            const ec::State& s0 = ec::state();
            if (s0.offsets_stale && !ec::client_version().empty() &&
                tried != ec::client_version()) {
                tried = ec::client_version();
                std::string want = ec::client_version();
                uint32_t    pid  = s0.pid;
                int n = fetch_offsets_for(want);
                if (n) {
                    apply_fetched_offsets(want, pid, n, true, ui_conf());
                } else {
                    ui_log("ERROR", "x no offsets published for this client - running stays blocked");
                }
            }
        }

        const ec::State& s = ec::state();
        g_locked_pid = s.pid;
        char b[192];
        // The handle route rides on this line rather than getting one of its
        // own. It is worth knowing on every attach, and not worth a whole entry
        // saying the expected thing happened.
        snprintf(b, sizeof b, "+ attached to pid %u via %s, base 0x%llx, %zu offsets",
                 s.pid, s.via_crash_handler ? "crash handler" : "OpenProcess",
                 (unsigned long long)s.base, s.noffsets);
        ui_log("STATUS", b);
        if (s.offsets_stale) {
            // Offsets going stale is the most important stop of all: this gate
            // is what stands between a stale conf and an armed write to an
            // address that was never right, and it must reach EVERY run, not
            // just the front one (docs/multi-run.md lifecycle).
            ec::stop_all_runs();
            ui_log("ERROR",  "x running is blocked - offsets do not match this client");
        }
        else if (s.dm && s.dm_verified) ui_log("STATUS", "+ DataModel verified");
        else if (s.dm)                  ui_log("STATUS", "! DataModel present but unverified");
        // No "re-attach" advice needed: the watcher retries once a second and
        // picks it up on its own within a few seconds of the client being ready.
        else                            ui_log("STATUS", "! DataModel not ready yet - watching for it");
    }
    push_state();
}

// Manual "Refresh offsets" from Settings.
//
// On its own thread, and not because the fetch is slow on a good day: http_get
// sets no WinHTTP timeouts, so it inherits the 60-second connect default. This
// arrives on the UI thread, where doing it inline would freeze the window for
// a minute against an unreachable server -- with no way to tell that from a
// hang. The watcher already fetches from a background thread under
// g_attach_mux; this follows it rather than inventing a second discipline.
//
// Deliberately ignores the once-per-version gate in do_attach. That gate stops
// a request loop against someone else's server for an answer that will not
// change; a person clicking a button IS the new information that it might have.
static std::atomic<bool> g_refresh_busy{false};

void refresh_offsets_async() {
    if (g_refresh_busy.exchange(true)) {
        ui_log("WARN", "! an offset refresh is already running");
        return;
    }
    std::thread([]() {
        ec::mark_host_thread();   // re-verifies offsets against the live client
        {
            std::lock_guard<std::mutex> lk(g_attach_mux);
            const ec::State& s = ec::state();
            const std::string want = ec::client_version();
            const uint32_t    pid  = s.pid;
            const bool  was_stale  = s.offsets_stale;

            if (want.empty()) {
                // No client means no version to ask for. The conf is per-build,
                // so there is no sensible "latest" to fall back on.
                ui_log("ERROR", "x not attached - there is no client version to fetch for");
            } else {
                // fetch_offsets_for reports its own failures, and each one says
                // something different about why; no generic line here.
                int n = fetch_offsets_for(want);
                if (n) apply_fetched_offsets(want, pid, n, was_stale, ui_conf());
            }
        }
        push_state();
        g_refresh_busy = false;
    }).detach();
}

// One tick of the watcher's attach work. Split from watch_client so the
// attach state stays owned here; the watcher's loop, the stop watchdog and
// the pacing stay in eclipse_ui.cpp.
void attach_poll() {
    uint32_t pid = ec::state().pid;

    if (!pid) {
        // Detached: attach by ourselves once a client turns up. Retried
        // quietly every tick because a freshly-launched client needs a few
        // seconds before its module list is readable -- the first attempts
        // are expected to fail and should not fill the console.
        // do_attach() already logs the success line, so nothing extra here.
        // It takes g_attach_mux itself, so it is NOT wrapped below.
        //
        // One find_client() call, two readers: the auto-attach below and the
        // readout flag. Calling it twice would double the process walk for the
        // same answer, and the two could disagree across the gap.
        bool seen = ec::find_client() != 0;
        bool was  = g_client_seen.exchange(seen);
        if (!g_user_detached.load() && seen) do_attach(true);
        // A client appearing or exiting while detached changes nothing else
        // the page can see, so without this push the readout would sit on its
        // old answer until some unrelated event happened to push state.
        else if (was != seen) push_state();
        // The look is complete either way: a client was found (do_attach
        // ran, pid set or not) or there is none. The overlay-mode window
        // gates may act on "no client" from here on.
        g_attach_probed = true;
    } else {
        // Same lock do_attach uses, so a manual Attach can't run while this
        // is mid-detach or mid-refresh.
        std::lock_guard<std::mutex> lk(g_attach_mux);
        if (!ec::alive()) {
            // The client is gone: every run is reading freed memory. Stop them
            // before dropping the handle (docs/multi-run.md lifecycle: detach).
            ec::stop_all_runs();
            ec::detach();
            // Clear the lock so a re-attach can re-enumerate. Also clear
            // any pending pick -- if the modal was up because the OTHER
            // client was also running, the user probably wants a fresh
            // list now that the state has changed.
            g_locked_pid   = 0;
            g_pick_pending = false;
            g_multi_logged = false;
            ui_log("STATUS", "! client exited - detached");
            push_state();
        } else {
            // Re-resolve EVERY tick, not only when the pointer is missing.
            //
            // Leaving a place destroys the DataModel and builds a new one,
            // and the old pointer stays non-zero the whole time -- so a check
            // for "is it zero" never fires and every snippet keeps reading
            // freed memory. Resolving is one dereference plus a class check,
            // which is nothing once a second.
            uint64_t before = ec::state().dm;
            if (ec::refresh_dm()) {
                const ec::State& s = ec::state();
                char b[160];
                // Rarely seen in practice: leaving a place swaps straight to
                // the next DataModel rather than passing through zero, so the
                // "changed" branch below is the one that actually fires. Kept
                // for the case where the client really does tear down without
                // building another.
                if (!s.dm)
                    snprintf(b, sizeof b, "! DataModel gone");
                else if (!before)
                    snprintf(b, sizeof b, "+ DataModel resolved: 0x%llx%s",
                             (unsigned long long)s.dm, s.dm_verified ? "" : " (unverified)");
                else
                    snprintf(b, sizeof b, "! DataModel changed: 0x%llx -> 0x%llx%s",
                             (unsigned long long)before, (unsigned long long)s.dm,
                             s.dm_verified ? "" : " (unverified)");
                ui_log("STATUS", b);
                push_state();
            }

            // While the picker is active (a multi-instance state has been
            // detected somewhere), refresh the row cache every tick so
            // "(unknown)" fills in as the still-loading client resolves
            // Players.LocalPlayer. If the count drops to <2, the multi
            // session is over -- clear the flag so a future one logs its
            // own opening line. No idle probe: the currently-attached
            // client is still attached whether or not a new one appears,
            // so unrequested detection is just noise.
            if (g_pick_pending) {
                auto cs = ec::enumerate_candidates();
                if (cs.size() >= 2) {
                    send_candidates(cs);
                } else {
                    g_pick_pending = false;
                    g_multi_logged = false;
                }
            }
        }
    }

    // THE RETRY MUST HAVE A CALLER. refresh_identity()'s empty-name retry
    // rate-limits itself, but it only ever ran inside push_state(), and
    // push_state() fires on EVENTS (run end, detach, the arm toggle, a page
    // load) -- so a name that was unreadable at the moment the panel loaded
    // (LocalPlayer commonly is not resolvable yet just after joining) stayed
    // "pid N" for the whole session unless the user happened to run a
    // snippet. Measured live: fx_player() returned the name while the panel
    // still showed the pid. Third instance this week of the same failure --
    // sample once at a transition, treat a not-yet answer as the answer,
    // never ask again (the geometry cache latching an empty build and a
    // snippet indexing an empty world on rejoin are the others). This is the
    // 1 Hz caller: while an attach is live and the identity unresolved, ask
    // again, and push only when it resolves so the views are not spammed.
    identity_tick();
}

// The retry itself, so both callers share one copy. refresh_identity throttles
// while the name is missing and returns at the top once it is not, so this is
// free to call from a fast tick -- which is the point: at the attach poll's
// 1 Hz alone the header sat visibly on "pid N" after a join.
// The snippet-facing reads, which this binary links anyway. LocalPlayer's name
// is the live truth about who is logged in, and it costs two reads.
extern "C" {
    uint64_t    fx_off(const char* key);
    uint64_t    fx_rd(uint64_t addr);
    size_t      fx_str(uint64_t addr, char* buf, size_t n);
    const char* fx_class(uint64_t inst);
    const char* fx_name(uint64_t inst);
}

// The Players service, memoised on the DataModel pointer.
//
// fx_service and the child helpers are `static inline` in fx.h -- the snippet
// surface, which the host deliberately does not include -- so the walk is here
// in terms of the primitives that ARE exported. It is the same walk
// enumerate_candidates does.
//
// Keyed on the DataModel because that is exactly what logging out changes and
// logging in does not: on logout the pointer moves and this re-walks, and on
// login the service object is the same one, so the memo stays valid and the
// LocalPlayer slot is re-read live from it. That is the whole point -- the
// thing that changes on login is the Player, not the service.
static uint64_t players_service() {
    static uint64_t s_dm = 0, s_players = 0;
    const uint64_t BAD = (uint64_t)-1;

    uint64_t dm = ec::state().dm;
    if (!dm) { s_dm = 0; s_players = 0; return 0; }
    if (dm == s_dm && s_players) return s_players;

    s_dm = dm;
    s_players = 0;
    uint64_t cso = fx_off("Instance.ChildrenStart");
    uint64_t ceo = fx_off("Instance.ChildrenEnd");
    if (cso == BAD || ceo == BAD) return 0;

    uint64_t cptr = fx_rd(dm + cso);
    if (!cptr) return 0;
    uint64_t begin = fx_rd(cptr), end = fx_rd(cptr + ceo);
    // Bounded like every other walk over a game-derived count: a stale
    // instance yields two unrelated pointers and the difference comes back in
    // the billions, which is a hang rather than a wrong answer.
    if (!begin || end <= begin || (end - begin) / 0x10 > 65536) return 0;

    uint64_t n = (end - begin) / 0x10;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t child = fx_rd(begin + i * 0x10);
        const char* cls = child ? fx_class(child) : nullptr;
        if (cls && strcmp(cls, "Players") == 0) { s_players = child; break; }
    }
    return s_players;
}

void identity_tick() {
    if (!ec::state().pid) return;

    // READ THE LIVE NAME, do not re-derive from a latched enumeration.
    //
    // refresh_identity() caches on (pid, DataModel) and only re-reads when one
    // of those changes, so anything that changed the NAME without changing
    // either was invisible: log out and the panel keeps your account, log back
    // in and it keeps "NOT_A_REAL_PLAYER" -- the client updates the Player
    // under Players and the panel never looks again. An empty-name retry did
    // not help, because a stale name is not an empty one.
    //
    // Players holds a Player even in the menu (LuaApp's DataModel has it), so
    // this resolves nearly always -- which is what lets the page drop its
    // "pid N" fallback and simply wait.
    //
    // NOT fx_player(): that is a CACHE, and it is keyed on the generation
    // counter -- which does not move when you log out and back in. The client
    // swaps the Player under Players, the cache keeps handing back the old
    // object, and its name still reads NOT_A_REAL_PLAYER long after the
    // account is back. Reported with a screenshot: the panel showed the
    // logged-out placeholder while Roblox showed the real account.
    //
    // So read LocalPlayer through the service, live, every time. Same lesson
    // as fx_place: a cache is only as fresh as the signal it is keyed on, and
    // "the account changed" fires none of ours.
    //
    // Cheap enough for a tick: fx_service memoises the Players pointer per
    // generation, so this is one read for the slot plus fx_name's descriptor
    // hop and string read -- about 6us, twice a second.
    static DWORD s_next = 0;
    DWORD now = GetTickCount();
    if (now < s_next) return;
    s_next = now + 500;

    uint64_t players = players_service();
    uint64_t lpo     = players ? fx_off("Player.LocalPlayer") : (uint64_t)-1;
    uint64_t lp      = (lpo != (uint64_t)-1) ? fx_rd(players + lpo) : 0;
    if (!lp) return;

    // DISPLAY NAME FIRST, username as the fallback -- the same preference the
    // candidate walk used to apply, and the reason it has to be applied HERE
    // now: with the walk writing display name and this writing Instance.Name,
    // leaving a game showed the display name for a beat and then flipped to
    // the username as this tick caught up. Same fact, two spellings, two
    // writers -- the flash was them disagreeing.
    //
    // DisplayName is a direct field on Player, NOT inside the name container,
    // so it is a plain string read rather than fx_name's descriptor route.
    char dn[64] = {0};
    uint64_t dno = fx_off("Player.DisplayName");
    if (dno != (uint64_t)-1) fx_str(lp + dno, dn, sizeof dn);

    const char* nm = dn[0] ? dn : fx_name(lp);
    if (!nm || !*nm) return;              // unreadable this tick: keep what we have

    // "Player" IS THE PLACEHOLDER, NOT AN ANSWER. While a place is loading --
    // and on a slow-loading client that window is seconds, not a frame -- the
    // Player under Players exists with its default Instance.Name of "Player"
    // and no DisplayName yet. Latching it put "Player" in the header on the
    // way out of a game, which is the same not-yet-answer-as-the-answer bug as
    // the display-name flash, just with a different spelling of the wrong
    // name. Keeping the last verified name is the honest reading: the account
    // did not change, only our ability to read it.
    //
    // IT TAKES BOTH HALVES, and each was measured on the way out of a place
    // (2026-09-06, probe sampling LocalPlayer at 4 Hz across the transition):
    //
    //   dm='Ugc'     Name='vl1eny'  DisplayName='vl1eny'  UserId=11612051674
    //   dm='LuaApp'  Name='Player'  DisplayName='Player'  UserId=0   <- placeholder
    //   dm='LuaApp'  Name='vl1eny'  DisplayName='vl1eny'  UserId=0   <- resolved
    //
    // The old test was `!dn[0] && name == "Player"`, on the theory that a real
    // account always has a DisplayName so only the nameless placeholder could
    // match. Line 2 says otherwise: THE PLACEHOLDER CARRIES A DISPLAY NAME OF
    // ITS OWN. dn[0] was 'P', the guard never fired, and "Player" reached the
    // header exactly as before.
    //
    // And UserId ALONE cannot be the test either, which is the half that is
    // easy to get wrong in the other direction: line 3 is the REAL account,
    // resolved, still reading UserId 0 because the menu's DataModel does not
    // carry one. Gating on uid > 0 would reject the true name for as long as
    // the user stayed out of a game.
    //
    // So: zero UserId AND the literal placeholder name. A genuine account
    // called "Player" is unaffected in a game (its UserId is real) and keeps
    // whatever was latched there while in the menu -- which is "Player", the
    // right answer, by a different route. (NOT_A_REAL_PLAYER is deliberately
    // NOT filtered here -- after a logout there IS no account, and the live
    // re-read above is what carries that case.)
    uint64_t uido = fx_off("Player.UserId");
    int64_t  uid  = (uido != (uint64_t)-1) ? (int64_t)fx_rd(lp + uido) : 0;
    if (uid == 0 && std::strcmp(nm, "Player") == 0) return;

    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(g_ident_mux);
        if (g_ident_user != nm) { g_ident_user = nm; changed = true; }
    }
    if (changed) push_state();   // outside the lock: push_state reads back in
}

// The state halves of the page's attach messages. Telling the page (push_state
// and the log lines) stays with the handlers in eclipse_ui.cpp.

// Deliberate teardown. The handle is closed and every cached pointer is
// dropped by ec::detach(), so a snippet still polling reads nothing rather
// than reading something stale -- failures, not wrong answers.
void attach_user_detach() {
    {
        std::lock_guard<std::mutex> lk(g_attach_mux);
        // Stop every run first -- a snippet still polling would read a closed
        // handle. Deliberate detach, so this is the user's choice, not a swap.
        ec::stop_all_runs();
        ec::detach();
        g_locked_pid   = 0;
        g_pick_pending = false;
        g_multi_logged = false;
    }
    g_user_detached = true;
}

void attach_reattach() {
    g_user_detached = false;
    // Manual re-attach: forget the lock so a fresh enumeration can happen.
    // g_pick_pending is DELIBERATELY not cleared -- it acts as the "already
    // logged multi-instance" gate. Clearing it would re-log the WARN on
    // every chip click. It gets cleared when we successfully attach or
    // when the client exits.
    g_locked_pid = 0;
    do_attach(false, true);
}

void attach_to_pid(uint32_t pid) {
    // The page picked one from the CANDIDATES modal.
    g_user_detached = false;
    if (pid) do_attach(false, true, pid);
}

bool attach_probed() { return g_attach_probed.load(); }

// Attached implies running, so the pid answers first and the flag only carries
// the detached case -- which is the only case the readout asks about.
bool client_running() { return ec::state().pid != 0 || g_client_seen.load(); }

std::string ident_user()  { std::lock_guard<std::mutex> lk(g_ident_mux); return g_ident_user; }
std::string ident_place() { std::lock_guard<std::mutex> lk(g_ident_mux); return g_ident_place; }

// Kept host-side too: the page caches these for the picker, and the panel
// needs the same answer without asking the network again. Arrives on the
// placename fetch thread, hence the lock.
void place_title_remember(uint64_t place_id, const std::string& name) {
    bool ours = false;
    {
        std::lock_guard<std::mutex> lk(g_ident_mux);
        g_place_titles[place_id] = name;
        if (place_id == g_ident_placeid) { g_ident_place = name; ours = true; }
    }
    if (ours) push_state();   // outside the lock: push_state reads back in
}
