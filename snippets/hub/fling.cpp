// Fling -- cross through someone carrying momentum until the solver hands it on.
//
// Ported from snippets/fling, dials and findings intact. WRITES our own
// Primitive's Position, AssemblyLinearVelocity and AssemblyAngularVelocity and
// nothing else; no write ever targets another player. All through fx_wrblk, so
// dry-run until the console is armed.
//
// THE MECHANISM IS TWO WRITES THAT ONLY WORK TOGETHER: velocity toward them,
// then a teleport onto them ALTERNATING a stud and a half either side of their
// centre. Crossing back and forth is a fresh contact every step, where holding
// one spot is an overlap the solver resolves once and ignores; the velocity is
// what that contact hands over. Do not reduce it to one of the two -- it was
// taken apart and rebuilt several times, and every failure was one half missing.
//
// A position write HELD STILL does not replicate (verified with two clients
// side by side: ours had us locked 2 studs away, theirs did not have us near
// them at all). The alternating write is a different case and does work.
//
// The Lua original pairs a 9e7 velocity with a BodyVelocity pinning it at zero,
// so the number is only ever read during contact resolution. Creating an
// Instance is not open to an external writer, so APPROACH stays a speed the
// assembly can survive; 9e7 unpinned put this character 3.5 billion studs out.

#include <cmath>
#include <set>
#include <string>

#include "hub.h"

const int    TOGGLE_KEY = 'F';
const float  APPROACH   = 100000.0f;   // studs/second, closing
const float  SPIN       = 50000.0f;    // radians/second
const float  MAX_RANGE  = 40000000.0f;  // our limit, and the only one

const float  OSC_STUDS  = 1.5f;
const double OSC_MS     = 90.0;

// Lead wants to be generous: their position is what THEIR device last reported,
// so it is already behind by the round trip, while ours is live.
const float  LEAD_SECONDS   = 0.45f;
const float  MAX_SPEED      = 500.0f;  // above this is a bad read, not a runner
const float  HOME_MAX_SPEED = 50.0f;   // only remember a spot we were still in


const bool   RETURN_ON_RELEASE = true;
// Long enough to actually keep trying. One assert+check cycle is 370ms, so
// 1000ms only ever bought three attempts -- and the failures were all "ran out
// of cycles", not "the write lost". This is a CAP, not a cost: a return that
// sticks on the first cycle still finishes in 370ms.
const double RETURN_MS         = 4000.0;
const float  RETURN_CLOSE      = 3.0f;
// One assert/quiet cycle. The quiet half has to outlast a physics step or the
// engine never gets its say; 120ms is clicktp's figure and it works.
const double ASSERT_MS         = 250.0;
const double CHECK_MS          = 120.0;
// How many quiet readings in a row must agree before it is called home. ONE
// reading proves we arrived; it does not prove we STAYED, and the failure mode
// here is being yanked back a moment later. Each extra confirmation costs only
// another quiet window, not another assert.
const int    CONFIRM_N         = 3;
const double SETTLE_MS         = 1000.0;

// Seconds on one target, then release, return, re-engage. A fling that has not
// landed in two seconds will not, and letting go restarts from a clean state.
const double BURST_MS     = 2000.0;
const double BURST_GAP_MS = 100.0;
// FLING ALL has TWO clocks. Each person gets a turn, and the reset happens on
// the burst clock as usual -- so at 0.5s a turn and a 2s burst, four people are
// covered between resets instead of going home after every one. The reset is
// still worth having; doing it per person just spent more time travelling than
// flinging.
const double ALL_TURN_MS = 500.0;

namespace {

bool     g_on       = false;
int64_t  g_pick     = 0;        // 0 = nearest; else the locked UserId
uint64_t g_target   = 0;
std::set<int64_t> g_never;
bool     g_never_loaded = false;

bool     g_osc      = false;
double   g_osc_next = 0;

float3   g_home{};
bool     g_have_home = false;
bool     g_returning = false;
bool     g_reported  = false;
int      g_close_n   = 0;
double   g_sample_next = 0;
double   g_return_until = 0;
double   g_started_at   = 0;
bool     g_quiet        = false;   // in the write-nothing phase
double   g_phase_until  = 0;
int      g_confirm      = 0;   // consecutive good quiet readings
double   g_settle_until = 0;

bool     g_all       = false;   // cycle everyone instead of one target
int64_t  g_all_cur   = 0;       // whose turn it is
double   g_turn_until = 0;      // when this person's turn ends
bool     g_resting    = false;
double   g_burst_until = 0;
double   g_rest_until  = 0;

std::string g_status = "off";

// THE WHITELIST: people this never flings. Stored under the old "never" key on
// purpose -- renaming it would silently drop a list the user has already built.
void never_load() {
    if (g_never_loaded) return;
    g_never_loaded = true;
    char buf[1024] = "";
    fx_cfg_str("hub.fling.never", buf, sizeof buf, "");
    const char* q = buf;
    while (*q) {
        int64_t v = _atoi64(q);
        if (v) g_never.insert(v);
        while (*q && *q != ',') q++;
        if (*q == ',') q++;
    }
}

void never_save() {
    std::string out;
    for (int64_t v : g_never) {
        char b[24]; snprintf(b, sizeof b, "%lld", (long long)v);
        if (!out.empty()) out += ",";
        out += b;
    }
    fx_cfg_set_str("hub.fling.never", out.c_str());
}

bool sane3(float3 v, float limit) {
    if (!(v.x == v.x && v.y == v.y && v.z == v.z)) return false;
    return !(v.x >  limit || v.x < -limit || v.y >  limit ||
             v.y < -limit || v.z >  limit || v.z < -limit);
}

float len3(float3 v) { return sqrtf(v.x*v.x + v.y*v.y + v.z*v.z); }

// Position AND velocity off the SAME Primitive in one resolution, so they
// describe the same instant -- the point of multiplying one by a time.
bool target_motion(uint64_t part, const Ctx& c, float3* pos, float3* vel) {
    uint64_t prim = fx_rd(part + c.oPrim);
    if (prim < 0x10000 || prim > 0x7ff000000000ULL) return false;
    float3 p = fx_rf3(prim + c.oPos);
    if (!sane3(p, 1.0e6f)) return false;
    *pos = p;
    *vel = float3{ 0, 0, 0 };
    float3 v = fx_rf3(prim + c.oVel);
    if (sane3(v, MAX_SPEED)) *vel = v;      // zeroed, not refused: still worth
    return true;                            // flinging, just without the lead
}

// Round-robin by UserId, which is a STABLE order -- rotating by distance would
// re-sort under us every tick and keep handing the turn back to whoever is
// nearest. Wraps to the lowest id once past the highest.
int64_t next_in_line(const Ctx& c, int64_t after) {
    int64_t best = 0, first = 0;
    for (int i = 0; i < c.n; i++) {
        if (c.p[i].is_local || !c.p[i].root) continue;
        if (g_never.count(c.p[i].userid)) continue;
        if (c.p[i].humanoid && c.p[i].health <= 0.0f) continue;
        int64_t id = c.p[i].userid;
        if (!id) continue;
        if (!first || id < first) first = id;
        if (id > after && (!best || id < best)) best = id;
    }
    return best ? best : first;
}

void begin_return(const Ctx& c) {
    g_settle_until = c.now + SETTLE_MS;
    // SILENCE WAS THE BUG ONCE ALREADY: with no home remembered this returned
    // without a word, which is indistinguishable from a return that ran and
    // missed. Say which it is.
    if (!RETURN_ON_RELEASE) { fx_log("  return: disabled"); return; }
    if (!g_have_home)       { fx_log("  return: NO HOME REMEMBERED - never stood still on the ground"); return; }
    if (c.oPos == (uint64_t)-1) { fx_log("  return: no Primitive.Position offset"); return; }
    {
        g_returning    = true;
        g_reported     = false;
        g_close_n      = 0;
        g_sample_next  = c.now + 20.0;
        g_return_until = c.now + RETURN_MS;
        g_started_at   = c.now;
        g_quiet        = false;
        g_phase_until  = c.now + ASSERT_MS;
        g_confirm      = 0;
        float3 at = c.prim ? fx_rf3(c.prim + c.oPos) : float3{0,0,0};
        float3 d{ at.x - g_home.x, at.y - g_home.y, at.z - g_home.z };
        fx_logf("  return: home (%.0f %.0f %.0f), %.0f studs away",
                g_home.x, g_home.y, g_home.z, len3(d));
    }
}

// The post-release hold, shared by a real release and the gap between bursts.
// Returns true if it is still working.
//
// THREE PHASES, and the middle one is the whole point:
//
//   ASSERT   write the position, and zero the velocities, for ASSERT_MS
//   QUIET    stop writing the POSITION for CHECK_MS -- but keep the velocities
//            at zero, because the question is whether the position STUCK, not
//            whether we drift when nothing holds us
//   JUDGE    read where we actually are
//
// The quiet phase is not politeness, it is the only way the check can fail.
// Two earlier versions read the position while still writing it -- once after
// the write and once just before -- and both reported "home in 60ms" while the
// character was 26,000 studs away, because at fx_sleep(0) we write thousands of
// times a second and the engine never gets a step in between. The reading was
// always our own value. clicktp learned this first: "without the pause this
// measures the value we just wrote, which would make the check impossible to
// fail."
//
// Fails the judgement and there is time left? Assert again. That is the retry,
// and it is bounded by RETURN_MS.
// Zero every part's linear and angular velocity. A fling leaves the limbs
// carrying their own momentum, and one loose arm drags the assembly off.
void quiet_velocities(const Ctx& c) {
    if (!c.prim) return;
    float3 z{ 0, 0, 0 };
    fx_wrblk(c.prim + c.oVel,  &z, 12);
    fx_wrblk(c.prim + c.oSpin, &z, 12);
    for (int k = 0; k < c.nbody; k++) {
        uint64_t pr = fx_rd(c.body[k] + c.oPrim);
        if (pr < 0x10000 || pr > 0x7ff000000000ULL) continue;
        fx_wrblk(pr + c.oVel,  &z, 12);
        fx_wrblk(pr + c.oSpin, &z, 12);
    }
}

// Our root Primitive, resolved RIGHT NOW rather than taken from the tick's
// cache. The judgement below has to read a live one: if the fling killed us,
// the cached pointer is freed memory that nothing but us ever writes, so the
// read gives back exactly what we wrote and the check reports a perfect
// "off by 0.0" for a return that never happened.
uint64_t live_prim(const Ctx& c) {
    uint64_t chr = fx_character();
    if (!chr) return 0;
    uint64_t root = fx_part(chr, "HumanoidRootPart");
    if (!root) root = fx_part(chr, "Torso");
    if (!root) return 0;
    uint64_t pr = fx_rd(root + c.oPrim);
    return (pr >= 0x10000 && pr <= 0x7ff000000000ULL) ? pr : 0;
}

bool hold_home(const Ctx& c) {
    if (!g_returning || !c.prim) {
        // Velocity-only settle, for the window after a release where there is
        // nothing to return to.
        if (c.prim && c.now < g_settle_until) {
            float3 z{ 0, 0, 0 };
            fx_wrblk(c.prim + c.oVel,  &z, 12);
            fx_wrblk(c.prim + c.oSpin, &z, 12);
            return true;
        }
        return false;
    }

    if (g_quiet) {
        // Velocity stays pinned through the quiet phase. Letting it go was what
        // made this look hopeless: at fling speeds, 120ms of free flight is
        // thousands of studs, so the check moved us away and then failed us for
        // having moved. Only the POSITION write is what is being tested.
        quiet_velocities(c);
        if (c.now < g_phase_until) return true;

        uint64_t pr = live_prim(c);
        if (!pr) {
            // No character to be home in. Say so rather than judging a corpse.
            g_returning = false;
            g_status    = "return: died";
            fx_log("  return: no character - died on the way");
            return false;
        }
        if (pr != c.prim) {
            // The rig was rebuilt mid-return, so everything asserted so far
            // went into a body that no longer exists. Start the return again
            // against the new one rather than reporting on the old.
            fx_log("  return: rig changed mid-return - re-asserting");
            g_quiet       = false;
            g_phase_until = c.now + ASSERT_MS;
            return true;
        }
        float3 at = fx_rf3(pr + c.oPos);
        float3 d{ at.x - g_home.x, at.y - g_home.y, at.z - g_home.z };
        float  off = len3(d);
        if (off < RETURN_CLOSE) {
            // Arrived. Now prove it STUCK: stay quiet for another window and
            // look again. Being pulled back a moment after landing is exactly
            // the failure this misses otherwise, and a second quiet window is
            // cheap -- no re-assert, just another CHECK_MS of not writing the
            // position.
            if (++g_confirm < CONFIRM_N) {
                g_phase_until = c.now + CHECK_MS;
                return true;
            }
            g_returning    = false;
            g_settle_until = 0;
            g_status       = "back home";
            fx_logf("  return: home in %.0fms (off by %.1f, %d checks)",
                    c.now - g_started_at, off, g_confirm);
            return false;
        }
        g_confirm = 0;                      // moved: the streak is broken
        if (c.now >= g_return_until) {
            g_returning = false;
            g_status    = "return failed";
            fx_logf("  return FAILED after %.0fms - still %.0f studs from home",
                    c.now - g_started_at, off);
            return false;
        }
        // Did not stick. Go round again with whatever time is left.
        g_quiet       = false;
        g_phase_until = c.now + ASSERT_MS;
        return true;
    }

    if (c.now >= g_phase_until) { g_quiet = true; g_phase_until = c.now + CHECK_MS; }

    quiet_velocities(c);
    move_body(c, g_home);
    return true;
}

void set_on(bool want, const Ctx& c) {
    if (want == g_on) return;
    g_on = want;
    if (!g_on && c.prim) {
        float3 z{ 0, 0, 0 };
        fx_wrblk(c.prim + c.oVel,  &z, 12);
        fx_wrblk(c.prim + c.oSpin, &z, 12);
    }
    // home is NOT captured here -- the idle path keeps it current, and only
    // while standing still. Capturing on the toggle is how a fling started from
    // mid-flight "returned" you to wherever the last one dropped you.
    if (g_on) { g_resting = false; g_burst_until = c.now + BURST_MS; }
    else      begin_return(c);
    g_status  = g_on ? "flinging" : "off";
    g_target  = 0;
}

}  // namespace

bool fling_hot() {
    return g_on || g_returning || g_settle_until > 0;
}

void fling_msg(const std::string& msg, const Ctx& c) {
    never_load();
    if      (msg == "fling:all") {
        g_all = !g_all;
        if (g_all) {
            g_pick    = 0;                  // a lock and "everyone" are exclusive
            g_all_cur    = next_in_line(c, 0);
            set_on(true, c);
            g_turn_until = c.now + ALL_TURN_MS;
        } else {
            // Turning ALL off stops entirely rather than falling back to
            // whoever is nearest -- carrying on flinging somebody after what
            // looks like a stop is the wrong way round to be surprising.
            // set_on runs the usual return home.
            set_on(false, c);
        }
        g_status = g_all ? "all" : (g_on ? "flinging" : "off");
    }
    else if (msg == "fling:on")  set_on(true,  c);
    else if (msg == "fling:off") set_on(false, c);
    else if (msg.rfind("fling:sel:", 0) == 0) {
        g_pick   = _atoi64(msg.c_str() + 10);
        if (g_pick) g_all = false;          // picking one leaves all-mode
        g_target = 0;
    }
    else if (msg.rfind("fling:never:", 0) == 0) {
        int64_t id = _atoi64(msg.c_str() + 12);
        if (!id) return;
        bool added = !g_never.count(id);
        if (added) g_never.insert(id);
        else       g_never.erase(id);
        fx_logf(added ? "  whitelisted %lld" : "  removed %lld from the whitelist",
                (long long)id);
        never_save();
        if (g_pick == id) g_pick = 0;   // or the drive chases someone it has
        g_target = 0;                   // been told to leave alone
    }
}

void fling_tick(const Ctx& c) {
    never_load();

    if (!g_on) {
        if (hold_home(c)) return;
        if (g_settle_until > 0 && c.now >= g_settle_until) g_settle_until = 0;
        // Remember where we are, but ONLY while standing still. Anything faster
        // than a walk means we are still being carried by the last fling, and
        // remembering THAT spot sends the return back to the victim.
        if (c.prim) {
            float3 v = fx_rf3(c.prim + c.oVel);
            if (sane3(v, 1.0e6f) && len3(v) < HOME_MAX_SPEED) {
                g_home = fx_rf3(c.prim + c.oPos);
                g_have_home = true;
            }
        }
        return;
    }
    if (!c.prim || !c.me_ok) return;

    // ---- burst cycle: on a target, let go, return, breathe, re-engage -------
    // The TURN clock, in all-mode only: hand over to the next person without
    // going home. The burst clock below still fires on its own schedule, so a
    // reset lands every BURST_MS regardless of how many people that covered.
    if (g_all && !g_resting && c.now >= g_turn_until) {
        g_all_cur    = next_in_line(c, g_all_cur);
        g_turn_until = c.now + ALL_TURN_MS;
        g_target     = 0;
    }

    if (!g_resting && c.now >= g_burst_until) {
        g_resting    = true;
        // The gap is measured from when the return FINISHES (set below), not
        // from a fixed RETURN_MS -- otherwise raising the retry budget would
        // stall every rotation by that long even when the return was instant.
        g_rest_until = c.now + BURST_GAP_MS;
        begin_return(c);
        g_target = 0;                    // next burst re-picks, so it rotates
        g_status = "resetting";
    }
    if (g_resting) {
        if (hold_home(c)) { g_rest_until = c.now + BURST_GAP_MS; return; }
        if (c.now < g_rest_until) return;
        g_resting     = false;
        g_burst_until = c.now + BURST_MS;
        // Resume the rotation where it left off rather than restarting it --
        // g_all_cur is kept, so a reset costs a pause, not your place in the
        // list.
        if (g_all) g_turn_until = c.now + ALL_TURN_MS;
        g_status      = "flinging";
    }

    // ---- pick, by locked UserId or by distance ------------------------------
    uint64_t best = 0;
    float3   bestpos{}, bestvel{};
    float    bestd = MAX_RANGE;
    for (int i = 0; i < c.n; i++) {
        if (c.p[i].is_local || !c.p[i].root) continue;
        if (g_never.count(c.p[i].userid)) continue;
        if (c.p[i].humanoid && c.p[i].health <= 0.0f) continue;
        float3 q{}, qv{};
        if (!target_motion(c.p[i].root, c, &q, &qv)) continue;
        float3 dd{ q.x - c.me.x, q.y - c.me.y, q.z - c.me.z };
        float  d = len3(dd);
        int64_t want = g_all ? g_all_cur : g_pick;
        if (want) {
            if (c.p[i].userid == want && d < MAX_RANGE) {
                bestd = d; best = c.p[i].player; bestpos = q; bestvel = qv;
            }
            continue;
        }
        if (d < bestd) { bestd = d; best = c.p[i].player; bestpos = q; bestvel = qv; }
    }

    if (!best) {
        float3 z{ 0, 0, 0 };
        fx_wrblk(c.prim + c.oSpin, &z, 12);
        g_target = 0;
        // In all-mode a missing target is somebody who died or left mid-turn,
        // not a reason to stall: take the next one now rather than waiting out
        // the rest of the turn on nobody.
        if (g_all) { g_all_cur = next_in_line(c, g_all_cur); g_turn_until = c.now + ALL_TURN_MS; }
        g_status = g_all ? "finding the next one" : "no target";
        return;
    }
    g_target = best;
    if (g_all) {
        for (int i = 0; i < c.n; i++)
            if (c.p[i].player == best) { g_status = std::string("all: ") + c.p[i].name; break; }
    }

    // ---- drive --------------------------------------------------------------
    // Lead first, then cross ALONG THEIR TRAVEL so the crossing keeps meeting
    // the body instead of being left behind by it. Horizontal only: their
    // vertical speed is jumps and knockback, not a direction they are going.
    if (c.now >= g_osc_next) { g_osc_next = c.now + OSC_MS; g_osc = !g_osc; }

    float3 base{ bestpos.x + bestvel.x * LEAD_SECONDS,
                 bestpos.y + bestvel.y * LEAD_SECONDS,
                 bestpos.z + bestvel.z * LEAD_SECONDS };
    float3 ax{ 1, 0, 0 };
    float  hs = sqrtf(bestvel.x * bestvel.x + bestvel.z * bestvel.z);
    if (hs > 1.0f) { ax.x = bestvel.x / hs; ax.z = bestvel.z / hs; }
    float  k = g_osc ? OSC_STUDS : -OSC_STUDS;
    float3 aim{ base.x + ax.x * k, base.y, base.z + ax.z * k };

    // VELOCITY FIRST, THEN THE TELEPORT. The engine can step between our two
    // writes: this order leaves the worst case as momentum with a stale
    // position, rather than a new position carrying nothing.
    float3 d{ aim.x - c.me.x, aim.y - c.me.y, aim.z - c.me.z };
    float  dist = len3(d);
    if (dist < 0.001f) dist = 0.001f;
    float3 dir{ d.x / dist, d.y / dist, d.z / dist };

    float3 v{ dir.x * APPROACH, dir.y * APPROACH, dir.z * APPROACH };
    float3 w{ SPIN, SPIN, SPIN };
    fx_wrblk(c.prim + c.oVel,  &v, 12);
    fx_wrblk(c.prim + c.oSpin, &w, 12);

    move_body(c, aim);
}

void fling_panel(std::string& out, const Ctx& c) {
    (void)c;
    char b[64];
    out += "\"fling\":{\"on\":";
    out += (g_on ? "true" : "false");
    out += ",\"all\":";
    out += (g_all ? "true" : "false");
    snprintf(b, sizeof b, ",\"pick\":%lld,\"never\":[", (long long)g_pick);
    out += b;
    bool first = true;
    for (int64_t v : g_never) {
        snprintf(b, sizeof b, "%s%lld", first ? "" : ",", (long long)v);
        out += b;
        first = false;
    }
    out += "],\"status\":\"";
    js_escape(g_status, out);
    out += "\"}";
}

int fling_key() { return TOGGLE_KEY; }
void fling_toggle(const Ctx& c) { set_on(!g_on, c); }
