// Auto parry -- Blade Ball (PlaceId 13772394625).
//
// READ ONLY: it watches the ball and taps F. Nothing writes to the client, so
// the arm gate never comes into it.
//
// THE HIGHLIGHT SAYS WHETHER, GEOMETRY SAYS WHEN.
// The targeted player's character gets a Highlight child; that is the game's own
// answer and it is required before anything fires. Geometry then times the tap:
//     closing = dot(v_ball - v_us, to_me) / |to_me|
//     eta     = |to_me| / closing
//
// THINGS THAT WERE TRIED AND ARE WRONG -- do not re-add without a measurement:
//
//  * An acceleration term (d = c*t + a*t*t/2). The ball really does speed up and
//    the straight-line eta really does overstate the time left, but when another
//    player parries, the closing speed jumps in one window, `a` reads enormous,
//    eta collapses to zero and it fires at a slow distant ball. The bias is a
//    smooth few tenths; the spike is a whole approach's error in one tick.
//  * Velocity by differencing positions, in case the ball were tweened. It is
//    not: the field is exact, the difference is noise (237 vs 290/166/334 in the
//    same instant) because our clock and the game's are unrelated.
//  * A geometry fallback when no highlight is seen. Stand in front of the player
//    the ball is going to and geometry sees a fine approach -- it is closing,
//    just not on you. Killed the user in a public server.
//  * A 150ms cooldown. `armed`, the deflection detector and the highlight
//    already give one parry per approach; a timer only swallows fast returns.
//
// THE GAME'S TARGET FLAG IS NOT READABLE. Searched 2026-09-05: the ball holds no
// character pointer (0x200 bytes plus all children, roster including parts), and
// TargetCharacter's value is not reachable flat or one indirection deep -- it is
// a ref-counted handle behind a property container. Two traps on the way, both
// plausible-looking: Misc.Value (0xb8) is not the value field for these classes
// and returns a BALL pointer, and "+0x68" is Instance.Parent. The target is
// probably a Roblox Attribute, which no pointer scan can see.

#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

// ---- the dials -------------------------------------------------------------
// PARRY_ETA is the one to tune: seconds before impact, absorbing your ping and
// the game's window. AROUND 0.44 IS RIGHT, TUNED IN PLAY -- do not lower it
// towards the 0.14 noted below on the strength of the highlight timing. That was
// tried and does not parry.
float PARRY_ETA = 0.50f;
float MISS_STUDS = 15.0f;   // a ball passing this close is preferred
float MIN_SPEED = 10.0f;   // a resting or drifting ball is not an attack
const int    PARRY_VK    = 'F';
// A SECOND KEY, pressed with the first. Some setups bind the parry to Z as
// well, and a game that ignores one will take the other.
//
// NOT ON A THREAD, deliberately. fx_send_key is a SendInput call costing
// microseconds, so two of them back to back land in the same input frame -- the
// game cannot tell that apart from simultaneous. A thread would buy nothing and
// would need its own answer for the arm gate and for stopping cleanly.
//
// Set to 0 to send only PARRY_VK.
const int    PARRY_VK2   = 'Z';
const int    HOLD_MS     = 0;       // 0: release next tick, never blocks the loop
const int    TOGGLE_VK   = VK_F6;

// PRE-EMPT. A ball handed over by somebody standing next to you arrives with no
// flight to measure: the highlight lands on us and it is already here. Press at
// once instead of waiting for CONFIRM_TICKS and an eta that has already gone
// past. ONE TAP; this is a trigger, not a mode.
//
// TWO WAYS IN, because distance alone cannot tell the cases apart: 90 studs at
// 200 studs/s is 0.45s and the normal path handles it comfortably, while 90
// studs at 700 studs/s is 0.13s and the window has already closed.
//   * eta <= PARRY_ETA -- already inside the firing window, so there is nothing
//     left to time. This is the velocity-aware one, since eta is distance over
//     closing speed.
//   * distance < PREEMPT_STUDS -- point blank, whatever the numbers say.
//
// The rising edge normally happens with the ball 204-270 studs out and its
// closing speed NEGATIVE (the highlight arrives while the ball is still going
// elsewhere), so eta is not even defined then and neither test trips. It only
// fires when the handover really is late.
float PREEMPT_STUDS = 25.0f;

// SPAM. A separate mechanism with a separate reason, and not speculative:
// mashing F is expensive here, because a parry that does not register costs the
// block cooldown and therefore the NEXT ball as well.
//
// TRIGGERED BY HIGHLIGHT CHURN, not by a missing flash. The flash is evidence
// AFTER the fact -- we press, and some tens of ms later the game either
// confirms or does not -- so building on its absence means waiting out that
// delay before reacting, and inheriting whatever unreliability it has. If the
// flash is ever missed we would both mis-verdict AND fail to spam.
//
// The highlight returning to us again and again is the same situation seen
// BEFORE the tap: several targetings in a short window is two players volleying
// it back and forth, each approach too short to time and the next arriving
// before the last has settled. Measured off our own rising edges, which we
// already track, so it costs nothing.
//
// The window grows mildly with ball speed: poll jitter and ping are the same
// milliseconds whatever the ball does, but a much larger share of a fast
// arrival. Small slope, hard ceiling.
// A turn sooner than this after our tap CANNOT be our tap -- the press has to
// reach the server and the result come back. Without the floor a ball already
// wheeling about at point-blank range closed the verdict as turned within a
// millisecond or two, which both invented a result and shut the door on the
// spam trigger before it could look.
const double   TURN_MIN_MS   = 40.0;
// SPAM_GAP is the whole trigger: the highlight returning to us sooner than
// this means the ball was hit straight back. Distance and eta were both tried
// and both cried wolf -- they describe any approach, not a frantic one.
//
// A FLAT BURST LENGTH. It used to grow with ball speed, on the reasoning that
// jitter is a larger share of a fast arrival. That is wrong twice over: the
// parry window is a fixed TIME (the highlight clears at eta 0.107-0.151 across
// ball speeds from 466 to 698 studs/s), so jitter is the same share of it
// whatever the ball does -- and a faster ball arrives sooner, so if anything
// there is LESS to cover, not more. The slope was pointing the wrong way.
double SPAM_GAP  = 700.0;   // re-targeted sooner than this = spam
// ...OR the exchange is simply short-range. The ball's distance AT THE RISING
// EDGE stands in for how far away the player who just hit it is, and it is far
// cheaper than finding who holds the highlight -- that means walking every
// living character's children on the scan clock.
//
// 30, NOT 110. A cross-map handover measures 204-270 studs, so I first set this
// well under that and called it close. Wrong end of the scale: in play, someone
// standing next to you is 20-30 studs, and at 110 it fired on an approach that
// the careful path then parried cleanly at eta 0.444 -- "ball 85 studs" with
// nearly eight seconds since the last targeting, which is not a rally at all.
double SPAM_MS   = 250.0;   // how long to keep tapping
const unsigned SPAM_EVERY = 20;   // ticks between taps -- 1 = every poll

// Consecutive ticks of falling eta before we believe it. Guards against a SINGLE
// BAD READ: these come from another process's memory mid-update, and one torn
// position is one tiny eta and one wasted parry. Costs 4ms of a 440ms window.
// (Added for a flicker the highlight has since taken over.)
int CONFIRM_TICKS = 3;

// How far back the turn is measured, and what counts as a deflection rather than
// homing. COUPLED: a smaller window makes the test more selective, since homing
// scales down with it while a deflection appears in full in whichever window
// contains it.
//
// 8 was wrong. Sampled at 2ms a curving or orbiting ball turns 8-24deg by itself
// (13deg, 19deg, both at constant distance); real reversals are 100-173deg. At 8
// every wobble counted as a hit and scored LANDED for parries that never landed.
const double TURN_WINDOW_MS = 5.0;
const float  HIT_TURN_DEG   = 60.0f;
// Sustained turning at this rate is a ball under an ability rather than an
// approach: well under HIT_TURN_DEG, since a deflection is one big step while a
// spin is a steady grind. CHAOS_WINDOWS is how many in a row make it a spin
// rather than a deflection -- the whirl lasts around 600ms, so a fifth of that
// is plenty and still far more than any single hard window.
float CHAOS_TURN_DEG = 30.0f;
int   CHAOS_WINDOWS  = 24;

// ---- did the parry land? ---------------------------------------------------
// Without this every dial is tuned by feel. The verdict has to be able to report
// FAILURE, which is the part that took three attempts:
//   * the flash (below) is the game confirming a parry -- the strong evidence;
//   * a turn alone is not, because a ball that HITS you turns just as hard. On
//     that basis alone it went 22 for 22, which is a check that cannot go red;
//   * health dropping, or dying, is the half that can fail. A hit here replaces
//     the character rather than shaving health, so the generation tick IS the
//     death notice -- and a death shortly after a tap is blamed on that tap even
//     once the verdict slot has closed.
const double VERDICT_MS     = 600.0;
const double DEATH_BLAME_MS = 1500.0;

// ---- the highlight ---------------------------------------------------------
// Walking the character's children is the expensive part of this snippet, so it
// runs on its own clock and skips the walk entirely when the child count is
// unchanged -- adding a Highlight changes that count, so an unchanged count is
// an unchanged answer for one read instead of a hundred. A full walk is forced
// periodically anyway, because one child added and another removed leaves the
// count identical.
const double TARGET_SCAN_MS = 5.0;
const double FORCE_WALK_MS  = 250.0;
// Only feeds the highlight/geometry label in the verdict log, now that the
// highlight is required.
const double HL_TRUST_MS    = 15000.0;

// ---- settings, remembered across runs ---------------------------------------
// The values above are the DEFAULTS -- what a fresh install parries with, and
// what the panel resets to. fx_cfg overrides them per machine, so tuning in the
// panel survives a rebuild.
struct Dial { const char* key; void* p; char kind; };   // 'f' float, 'd' double, 'i' int
static Dial g_dials[] = {
    { "parry.eta",        &PARRY_ETA,     'f' },
    { "parry.missStuds",  &MISS_STUDS,    'f' },
    { "parry.minSpeed",   &MIN_SPEED,     'f' },
    { "parry.preempt",    &PREEMPT_STUDS, 'f' },
    { "parry.spamGap",    &SPAM_GAP,      'd' },
    { "parry.spamMs",     &SPAM_MS,       'd' },
    { "parry.confirm",    &CONFIRM_TICKS, 'i' },
    { "parry.chaosWins",  &CHAOS_WINDOWS,  'i' },
    { "parry.chaosTurn",  &CHAOS_TURN_DEG, 'f' },
};
static const int N_DIALS = (int)(sizeof g_dials / sizeof g_dials[0]);

static double dial_get(const Dial& d) {
    if (d.kind == 'f') return *(float*)d.p;
    if (d.kind == 'i') return *(int*)d.p;
    return *(double*)d.p;
}
static void dial_set(const Dial& d, double v) {
    if      (d.kind == 'f') *(float*)d.p  = (float)v;
    else if (d.kind == 'i') *(int*)d.p    = (int)v;
    else                    *(double*)d.p = v;
}
static void dials_load() {
    for (int i = 0; i < N_DIALS; i++)
        dial_set(g_dials[i], fx_cfg_num(g_dials[i].key, dial_get(g_dials[i])));
}
static bool dial_apply(const std::string& key, double v) {
    for (int i = 0; i < N_DIALS; i++) {
        if (key != g_dials[i].key) continue;
        dial_set(g_dials[i], v);
        fx_cfg_set_num(g_dials[i].key, v);
        return true;
    }
    return false;
}

// Both keys, one after the other.
static int send_parry() {
    int ok = fx_send_key(PARRY_VK, HOLD_MS);
    if (PARRY_VK2) fx_send_key(PARRY_VK2, HOLD_MS);
    return ok;
}

static double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// THE PARRY FLASH: "ParryHighlight", a ~9ms flash at the instant a parry
// registers -- a different, much briefer object than the target Highlight.
// NO CACHE, deliberately: it can appear and vanish between two child-count
// checks, and missing it reports failure for a parry that worked. Only called
// while a verdict is open, which bounds the cost.
static bool parry_flash(uint64_t chr) {
    if (!chr) return false;
    uint64_t n = fx_nchildren(chr);
    if (n > 64) n = 64;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t c = fx_child(chr, (int)i);
        if (!c) continue;
        const char* nm = fx_name(c);
        if (nm && strcmp(nm, "ParryHighlight") == 0) return true;
    }
    return false;
}

// The target marker, matched BY CLASS. Matching the name "ParryHighlight" was
// tried and stopped the parry working: a probe watching by class printed the
// name of what it found, and it is plainly "Highlight" every time. Two different
// objects; class finds the right one without tracking names.
//
// What it marks, at a 2ms poll over a dozen rallies: ON at 204-270 studs with
// closing speed NEGATIVE (it fires as we become the target, while the ball is
// still going elsewhere), OFF at 66-93 studs, eta clustered on ~0.14. So it is
// an early warning, and that 0.14 is the window CLOSING, not the moment to
// press.

static uint64_t highlight_on(uint64_t chr) {
    static uint64_t s_chr = 0, s_n = 0, s_hl = 0;
    static double   s_walked = 0;
    if (!chr) { s_chr = 0; s_n = 0; s_hl = 0; return 0; }

    uint64_t n = fx_nchildren(chr);
    double   t = now_ms();
    if (chr == s_chr && n == s_n && (t - s_walked) < FORCE_WALK_MS) return s_hl;

    // NAME AND CLASS BOTH. Class alone was too loose: any Highlight the game
    // parents to you counts, and it parents them for other reasons -- ability
    // effects, mode markers -- so we read "targeted" while the ball was going
    // somewhere else entirely.
    //
    // The name to match is plainly "Highlight". That is measured, not assumed:
    // a probe watched by CLASS and printed the name of what it found, and it
    // came back "Highlight" on every approach. (ParryHighlight is a different,
    // 9ms object -- the parry flash -- and matching THAT name broke parrying
    // outright, which is how the two came to be told apart.)
    uint64_t hl = 0;
    uint64_t lim = n > 64 ? 64 : n;           // bound a game-derived count
    for (uint64_t i = 0; i < lim; i++) {
        uint64_t c = fx_child(chr, (int)i);
        if (!c) continue;
        const char* nm = fx_name(c);
        if (!nm || strcmp(nm, "Highlight") != 0) continue;
        const char* cl = fx_class(c);
        if (cl && strcmp(cl, "Highlight") == 0) { hl = c; break; }
    }
    s_chr = chr; s_n = n; s_hl = hl; s_walked = t;
    return hl;
}

// Per-ball history, so the turn is measured against the SAME ball rather than
// whatever was first in the folder last tick. A fifth ball is simply untracked.
struct BallHist {
    uint64_t inst  = 0;
    double   t_ref = 0;
    float3   v_ref{};         // unit velocity at t_ref
    float    c_ref = 0;       // closing speed at t_ref, RELATIVE to us
    float    turn  = 0;       // degrees turned in the last window
    int      spin_n = 0;      // consecutive hard-turning windows
    bool     spun  = false;   // being whirled about (Tornado and friends)
    bool     hit   = false;   // stopped coming at us since t_ref
};
static BallHist g_hist[4];

static BallHist* hist_for(uint64_t inst) {
    for (BallHist& h : g_hist) if (h.inst == inst) return &h;
    for (BallHist& h : g_hist) if (!h.inst) { h = BallHist{}; h.inst = inst; return &h; }
    // All slots taken by balls that have gone: recycle the stalest.
    BallHist* old = &g_hist[0];
    for (BallHist& h : g_hist) if (h.t_ref < old->t_ref) old = &h;
    *old = BallHist{}; old->inst = inst;
    return old;
}

// Read-only loop, so there is no reason to spin at fx_sleep(0).
const unsigned POLL_MS = 1;

static float3 sub(float3 a, float3 b) { return float3{ a.x-b.x, a.y-b.y, a.z-b.z }; }
static float  len(float3 v) { return sqrtf(v.x*v.x + v.y*v.y + v.z*v.z); }
static float  dot(float3 a, float3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static float3 unit(float3 v) { float l = len(v); return l > 1e-6f ? float3{v.x/l, v.y/l, v.z/l} : float3{0,0,0}; }
static float  ang_deg(float3 a, float3 b) {
    float d = dot(a, b);
    if (d >  1.0f) d =  1.0f;
    if (d < -1.0f) d = -1.0f;
    return acosf(d) * 57.29578f;
}

// ---- the panel -------------------------------------------------------------
// The hub's width (560) so the two docked panels read as the same app, but not
// its height: this one has far less in it. Sized to the DIALS tab, which is the
// tall one -- seven rows plus the reset button. It was 430, from back when
// there were ten dials, and three have since been deleted; the leftover was a
// third of the panel of empty translucent grey sitting over the game.
static const int PANEL_W = 560, PANEL_H = 330;

static void js_escape(const std::string& in, std::string& out) {
    for (char c : in) {
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c >= 32)   out += c;
    }
}

// One JSON push per refresh. The dial list travels WITH the state, so the page
// builds its rows from what actually exists in C++: add an entry to g_dials and
// it appears, with no matching edit here or in the page.
static void push_state(bool on, bool targeted, bool spamming,
                       float eta, float dist, float spd,
                       long ok, long turn, long bad, long unk,
                       const std::string& last) {
    std::string j = "S({";
    char b[256];
    snprintf(b, sizeof b,
             "\"on\":%s,\"targeted\":%s,\"spam\":%s,"
             "\"eta\":%.3f,\"dist\":%.0f,\"spd\":%.0f,"
             "\"ok\":%ld,\"turn\":%ld,\"bad\":%ld,\"unk\":%ld,\"last\":\"",
             on ? "true" : "false", targeted ? "true" : "false",
             spamming ? "true" : "false",
             eta < 1e8f ? eta : -1.0f, dist < 1e8f ? dist : 0.0f, spd,
             ok, turn, bad, unk);
    j += b;
    js_escape(last, j);
    j += "\",\"dials\":[";
    for (int i = 0; i < N_DIALS; i++) {
        // A step sized off the value itself, so eta nudges by 0.01 and a
        // distance by 10 without keeping a table of steps in step with g_dials.
        double v  = dial_get(g_dials[i]);
        double st = v >= 100 ? 10 : v >= 10 ? 1 : v >= 1 ? 0.5 : 0.01;
        snprintf(b, sizeof b, "%s{\"k\":\"%s\",\"n\":\"%s\",\"v\":%g,\"s\":%g}",
                 i ? "," : "", g_dials[i].key,
                 g_dials[i].key + 6,          // drop the "parry." prefix
                 v, st);
        j += b;
    }
    j += "]})";
    fx_window_eval(j.c_str());
}

extern "C" void fx_run() {
    if (!fx_pid()) { fx_log("not attached - start Roblox first"); return; }

    const uint64_t oPrim = fx_off("BasePart.Primitive");
    const uint64_t oPos  = fx_off("Primitive.Position");
    const uint64_t oVel  = fx_off("Primitive.AssemblyLinearVelocity");
    const uint64_t oHP   = fx_off("Humanoid.Health");
    if (oPrim == (uint64_t)-1 || oPos == (uint64_t)-1 || oVel == (uint64_t)-1) {
        fx_log("x conf is missing Primitive.Position / AssemblyLinearVelocity");
        return;
    }

    dials_load();
    fx_window_open("Auto parry", PANEL_W, PANEL_H);
    fx_window_navigate((std::string(fx_snippets_dir()) + "\\autoparry\\ui.html").c_str());
    int    dock_dx = (int)fx_cfg_num("parry.dockx", 16);
    int    dock_dy = (int)fx_cfg_num("parry.docky", 16);
    int    panel_h = PANEL_H;
    double next_push = 0;
    std::string last_line;
    // What the panel is shown. Kept across ticks because the refresh happens
    // ABOVE the loop's early-outs, where this tick's figures do not exist yet.
    float ui_eta = -1.0f, ui_dist = 0.0f, ui_spd = 0.0f;
    double next_clamp = 0;
    fx_window_dock(1, FX_DOCK_TL, dock_dx, dock_dy);

    bool     on      = true;
    unsigned gen     = fx_gen() - 1;
    uint64_t balls   = 0;
    uint64_t alive   = 0;
    uint64_t root_pr = 0;
    bool     armed   = true;   // one shot per approach; re-armed when eta grows
    long     count   = 0;
    // FOUR OUTCOMES, COUNTED SEPARATELY, because lumping them flatters the
    // result. A flash is the game confirming a parry; a turn is an inference,
    // and a ball that HITS you turns just as hard. They must not share a
    // column, or the weak evidence hides behind the strong.
    long     landed  = 0;      // ParryHighlight flashed: confirmed
    long     turned  = 0;      // only the ball turned: probably, not certainly
    long     missed  = 0;      // took damage, or died
    long     unknown = 0;      // never resolved -- see the overwrite case
    double   last_fire = -1e9;
    uint64_t pend_ball = 0;    // the ball a parry is awaiting a verdict on
    double   pend_at   = 0;
    float    pend_eta  = 0;
    bool     pend_hl   = false;   // was it the highlight that let this fire?
    bool     pend_flash = false;  // ParryHighlight seen since we fired
    float    pend_hp   = 0;       // our health when we fired
    uint64_t hum       = 0;       // our Humanoid, re-resolved with the rig
    int      confirm = 0;      // consecutive ticks the ball has looked inbound
    float    prev_eta = 1e9f;
    double   next_scan = 0;
    bool     targeted  = false;   // the game says the ball is for us
    // FAILS OPEN. Until we have been highlighted once we cannot know the game
    // marks targets at all -- a Blade Ball update could drop it, and a parry
    // that silently stops working is worse than one that fires early. So the
    // geometry alone decides until the first highlight proves the mechanism is
    // there, and from then on the highlight is authoritative.
    bool     hl_works  = false;
    double   hl_seen_at = 0;      // when the target Highlight was last on us
    double   start_ms   = now_ms();
    bool     warned_no_hl = false;
    bool     was_targeted = false;
    double   spam_until   = 0;    // pressing every tick until this moment
    bool     spam_counted = false;   // this burst has a verdict open
    double   last_target_at = 0;  // when the highlight last landed on us
    double   next_spam    = 0;

    fx_logf("auto parry: ON. F6 toggles, Stop ends. eta<=%.2fs, miss<=%.0f studs",
            PARRY_ETA, MISS_STUDS);

    while (fx_running()) {
        if (fx_key_pressed(TOGGLE_VK) && fx_input_ok()) {
            on = !on;
            fx_logf("auto parry: %s", on ? "ON" : "off");
        }

        // 8Hz, and ABOVE the early-outs below. At the foot of the loop it
        // almost never ran: the loop skips everything while we are not
        // targeted, which is most of a match, so the panel sat on its
        // placeholder waiting for a push that only arrived mid-approach.
        //
        // A JS eval is a blocking round trip to the browser, so it stays on its
        // own slow clock and out of the 1ms tick.
        if (now_ms() >= next_push) {
            next_push = now_ms() + 125.0;
            push_state(on, targeted, spam_until > 0 && now_ms() < spam_until,
                       ui_eta, ui_dist, ui_spd,
                       landed, turned, missed, unknown, last_line);
        }

        // ---- keep it on screen ---------------------------------------------
        // The dock is a corner plus insets, and the insets were only clamped
        // when dragged. Drag it near an edge, or resize the client, and the
        // panel hangs off with nothing re-checking it. Ported from the hub,
        // which learned the same thing.
        if (now_ms() >= next_clamp) {
            next_clamp = now_ms() + 500.0;
            float vw = 0, vh = 0;
            if (fx_viewport(&vw, &vh) && vw > 1 && vh > 1) {
                int maxx = (int)vw - PANEL_W;  if (maxx < 0) maxx = 0;
                int maxy = (int)vh - panel_h;  if (maxy < 0) maxy = 0;
                int nx = dock_dx > maxx ? maxx : dock_dx;
                int ny = dock_dy > maxy ? maxy : dock_dy;
                if (nx != dock_dx || ny != dock_dy) {
                    dock_dx = nx; dock_dy = ny;
                    fx_window_dock(1, FX_DOCK_TL, dock_dx, dock_dy);
                    fx_cfg_set_num("parry.dockx", dock_dx);
                    fx_cfg_set_num("parry.docky", dock_dy);
                }
            }
        }

        while (const char* raw = fx_window_get_msg()) {
            std::string m(raw);
            if (m == "ready")            next_push = 0;   // page is up: paint it
            else if (m == "toggle")      on = !on;
            else if (m == "size:min")  { panel_h = 36;      fx_window_open("Auto parry", PANEL_W, panel_h); }
            else if (m == "size:full") { panel_h = PANEL_H; fx_window_open("Auto parry", PANEL_W, panel_h); }
            else if (m == "reset") {
                // Back to the figures compiled in. The config keys are left
                // behind deliberately -- writing the defaults would make them
                // indistinguishable from a value someone chose.
                for (int i = 0; i < N_DIALS; i++) fx_cfg_set_num(g_dials[i].key, dial_get(g_dials[i]));
                fx_log("  dials: reset");
            }
            else if (m.rfind("drag:", 0) == 0) {
                size_t comma = m.find(',', 5);
                if (comma != std::string::npos) {
                    dock_dx += atoi(m.c_str() + 5);
                    dock_dy += atoi(m.c_str() + comma + 1);
                    if (dock_dx < 0) dock_dx = 0;
                    if (dock_dy < 0) dock_dy = 0;
                    float vw = 0, vh = 0;
                    if (fx_viewport(&vw, &vh) && vw > 1 && vh > 1) {
                        int maxx = (int)vw - PANEL_W;  if (maxx < 0) maxx = 0;
                        int maxy = (int)vh - panel_h;  if (maxy < 0) maxy = 0;
                        if (dock_dx > maxx) dock_dx = maxx;
                        if (dock_dy > maxy) dock_dy = maxy;
                    }
                    fx_window_dock(1, FX_DOCK_TL, dock_dx, dock_dy);
                    fx_cfg_set_num("parry.dockx", dock_dx);
                    fx_cfg_set_num("parry.docky", dock_dy);
                }
            }
            else if (m.rfind("set:", 0) == 0) {
                size_t colon = m.rfind(':');
                if (colon != std::string::npos && colon > 4) {
                    std::string key = m.substr(4, colon - 4);
                    if (dial_apply(key, atof(m.c_str() + colon + 1)))
                        fx_logf("  %s = %g", key.c_str(), fx_cfg_num(key.c_str(), 0));
                }
            }
            else if (m.rfind("dbg:", 0) == 0) fx_log(m.c_str());
        }

        // A place change or a respawn replaces every pointer below. Re-resolved
        // on the generation edge rather than trusted, exactly as the other
        // snippets do -- a latched root is how you end up reading freed memory.
        if (fx_gen() != gen) {
            gen = fx_gen();
            balls = alive = root_pr = 0;
            hum   = 0;
            armed = true;
            // THE OTHER WAY A PARRY FAILS, and probably the usual one. A hit in
            // this game does not shave health off, it replaces your character --
            // so the health comparison never gets to run, because the Humanoid
            // it was reading is freed. The generation tick IS the death notice.
            // Counted as a miss: we were mid-verdict and are now respawning,
            // which is not what a parry that worked looks like.
            if (pend_ball) {
                missed++;
                fx_logf("parry #%ld DIED     eta %.3fs %-10s  %ld/%ld",
                        count, pend_eta < 1e8f ? pend_eta : 0.0f, pend_hl ? "highlight" : "geometry",
                        landed, landed + turned + missed + unknown);
                pend_ball = 0;
            } else if (count > 0 && now_ms() - last_fire >= DEATH_BLAME_MS) {
                // Not blamed on a parry -- too long since the last one -- but
                // still worth saying. A death that appears nowhere in the log
                // reads as "it just stopped working".
                fx_logf("died (%.1fs after the last parry)",
                        (now_ms() - last_fire) / 1000.0);
            } else if (count > 0) {
                // The verdict had already closed -- almost certainly as a TURN,
                // which is what a ball hitting you also looks like. Correct it.
                if (turned > 0) turned--;
                missed++;
                fx_logf("parry #%ld DIED     %.0fms after firing (verdict was wrong)  %ld/%ld",
                        count, now_ms() - last_fire,
                        landed, landed + turned + missed + unknown);
            }
        }
        if (!balls || !alive) {
            uint64_t ws = fx_service("Workspace");
            if (!balls) balls = ws ? fx_find(ws, "Balls") : 0;
            if (!alive) alive = ws ? fx_find(ws, "Alive") : 0;
        }
        if (!root_pr) {
            uint64_t chr  = fx_character();
            uint64_t root = chr ? fx_part(chr, "HumanoidRootPart") : 0;
            uint64_t pr   = root ? fx_rd(root + oPrim) : 0;
            if (pr >= 0x10000 && pr <= 0x7ff000000000ULL) root_pr = pr;
            hum = chr ? fx_part(chr, "Humanoid") : 0;
        }
        if (!on || !balls || !root_pr) { fx_sleep(POLL_MS); continue; }

        if (now_ms() >= next_scan) {
            next_scan = now_ms() + TARGET_SCAN_MS;
            // PRESENCE, not a latch. The latch existed because the NAMED
            // highlight was a 20ms flash; the one matched by class is a level
            // that stands for the whole approach, so asking whether it is there
            // right now is the right question again.
            uint64_t me_chr = fx_character();
            targeted = highlight_on(me_chr) != 0;
            if (targeted) hl_seen_at = now_ms();

            // Still decays, as the one safety worth keeping from that round: if
            // highlights stop being visible for any reason, this falls back to
            // geometry instead of blocking every parry forever.
            hl_works = hl_seen_at > 0 && (now_ms() - hl_seen_at) < HL_TRUST_MS;
        }

        // SKIPPED WHEN IT CANNOT MATTER. Firing requires the highlight, so
        // while untargeted no ball maths can produce a parry -- and in a
        // public server that is most of the match. An open verdict still
        // needs the deflection watch, hence a condition and not an early-out.
        if (!targeted && !pend_ball) {
            confirm  = 0;
            prev_eta = 1e9f;
            ui_eta   = -1.0f;          // nothing incoming: the panel says so
            // Cleared here too, or the rising edge below fires only once:
            // skipping the loop skips where it is normally updated.
            // THE APPROACH IS OVER, SO RE-ARM. This is the fix for the latch
            // wedging shut: `armed` used to come back only when the eta grew
            // past 3x PARRY_ETA, and with balls always flying about it can sit
            // between "too far to fire at" and "far enough to re-arm" for
            // minutes. One parry landed, then nothing for 18.8 seconds and a
            // death, with the snippet apparently working perfectly.
            //
            // Losing the highlight is the game telling us the approach ended --
            // an event, not a threshold, and it cannot get stuck.
            armed = true;
            was_targeted = false;
            fx_sleep(POLL_MS);
            continue;
        }

        float3 me   = fx_rf3(root_pr + oPos);
        // OUR velocity, for the relative-motion lead below. One extra 12-byte
        // read per tick against a root pointer we already hold.
        float3 mev  = fx_rf3(root_pr + oVel);

        // The soonest arrival across every ball in play. Blade Ball is usually
        // one, but a second one must not be ignored just because it is later in
        // the folder.
        float best_eta  = 1e9f;
        uint64_t best_ball = 0;   // the ball that eta belongs to, for the verdict
        float alt_eta   = 1e9f;   // least-bad candidate if none passes the miss test
        uint64_t alt_ball  = 0;
        float best_turn = 0.0f;       // how hard the chosen ball is turning
        bool  best_spun = false;      // ...and whether it is mid-spin
        float near_dist = 1e9f;   // closest CLOSING ball
        float near_spd  = 0.0f;   // ...and its speed
        float    any_dist    = 1e9f;  // closest ball of any kind, for the pre-empt
        float    any_closing = 0.0f;  // ...and whether it is actually coming
        uint64_t any_ball    = 0;
        bool  any_hit  = false;   // ANY ball deflected this tick, filtered or not
        bool  pend_turned = false;
        uint64_t n = fx_nchildren(balls);
        if (n > 16) n = 16;                       // bound a game-derived count
        for (uint64_t i = 0; i < n; i++) {
            uint64_t b = fx_child(balls, (int)i);
            if (!b) continue;
            uint64_t pr = fx_rd(b + oPrim);
            if (pr < 0x10000 || pr > 0x7ff000000000ULL) continue;

            float3 bp = fx_rf3(pr + oPos);
            float3 bv = fx_rf3(pr + oVel);
            float  spd  = len(bv);
            float3 to   = sub(me, bp);
            float  dist = len(to);
            if (dist < 0.01f) continue;

            // THE NEAREST BALL, WHATEVER IT IS DOING. Taken before the speed
            // and closing filters, which is the point: a ball sitting right
            // next to you at walking pace fails both, so every distance we
            // recorded below was of a ball already qualified as an attack --
            // and pre-empt could never see the one case it most needs to.
            // Tracked with its RELATIVE closing speed, because "right next to
            // me" is not the same as "about to hit me". A slow ball you are
            // walking away from is close and harmless, and pre-empting at it
            // spends a parry -- and the block cooldown -- on nothing.
            if (dist < any_dist) {
                any_dist = dist; any_ball = b;
                any_closing = dot(sub(bv, mev), to) / dist;
            }

            if (spd < MIN_SPEED) continue;

            // LEAD: what matters is how fast the GAP closes, and you are
            // dodging the whole time. Relative velocity is built from two
            // INSTANTANEOUS readings, so unlike a turn rate or an
            // acceleration there is nothing here for a deflection to spike.
            float3 rel = sub(bv, mev);
            float  closing = dot(rel, to) / dist;

            // HISTORY FIRST, BEFORE ANY FILTER. A ball that has just been
            // parried is travelling AWAY, so its closing speed is negative and
            // the test below drops it -- if the turn were measured after that
            // filter the one event we most want to see, the deflection itself,
            // would be the one event never reached.
            BallHist* h = hist_for(b);
            float3 vn = unit(bv);
            double tn = now_ms();
            if (h->t_ref == 0) {                       // first sight of this ball
                h->t_ref = tn; h->v_ref = vn; h->c_ref = closing;
            } else if (tn - h->t_ref >= TURN_WINDOW_MS) {
                // A REVERSAL, NOT A TURN. The angle test cannot tell a parry
                // from an orbit, and the Tornado ability spins the ball in a
                // rising circle -- far more than HIT_TURN_DEG per window, for
                // seconds, with nobody having touched it. Every one of those
                // scored a "turned" verdict, re-armed the latch and closed a
                // parry that never happened.
                //
                // What a parry actually does is stop the ball coming AT US:
                // the closing speed crosses from positive to negative. An orbit
                // never does that -- it circles at a roughly constant closing
                // speed. The angle stays as a second condition so a slow drift
                // through zero does not read as a strike.
                h->turn  = ang_deg(h->v_ref, vn);
                // SUSTAINED, not one window. Tornado whirls the ball in a
                // small rising circle for the better part of a second; an
                // ordinary deflection is a single hard window and a homing ball
                // curves gently. Requiring a run of hard windows is what tells
                // the three apart -- keyed on one window, a normal exchange
                // read as a spin and the parry was suppressed.
                if (h->turn > CHAOS_TURN_DEG) { if (h->spin_n < 99) h->spin_n++; }
                else                            h->spin_n = 0;
                h->spun = (h->spin_n >= CHAOS_WINDOWS);
                bool reversed = (h->c_ref > MIN_SPEED && closing < -MIN_SPEED);
                h->hit   = reversed && h->turn > HIT_TURN_DEG;
                h->t_ref = tn; h->v_ref = vn; h->c_ref = closing;
            }
            if (h->hit) any_hit = true;
            // The ball we last fired at, turning: that is the parry landing.
            if (h->hit && b == pend_ball) pend_turned = true;

            if (closing < MIN_SPEED) continue;    // going away, or merely drifting

            // ALWAYS APPLIED. Switching it off when highlighted killed the
            // user in a public server: the highlight marks YOU as a target,
            // it cannot say WHICH ball, and one aimed at somebody behind you
            // flies through your position with an excellent closing speed.
            // Same relative frame as the eta, or the two disagree when we move.
            float rs = len(rel);
            if (rs < 1e-3f) continue;
            float  t  = dot(to, rel) / (rs * rs);          // time to closest point
            float3 at{ bp.x + rel.x*t, bp.y + rel.y*t, bp.z + rel.z*t };
            float  miss = len(sub(me, at));

            float eta = dist / closing;
            if (dist < near_dist) { near_dist = dist; near_spd = spd; }

            // IT CHOOSES, IT DOES NOT REJECT. As a reject filter it killed
            // us the other way: a homing ball 260 studs out is not pointed
            // at us yet, gets rejected, confirm never builds, and we die
            // while glowing. Close-passing is preferred, wide is kept as
            // the fallback candidate.
            if (miss <= MISS_STUDS) {
                if (eta < best_eta) {
                best_eta = eta; best_ball = b; best_turn = h->turn;
                best_spun = h->spun;
            }
            } else if (eta < alt_eta) {
                alt_eta = eta; alt_ball = b;
            }
        }

        // THE HIGHLIGHT JUST LANDED ON US.
        bool preempt = false;
        if (targeted && !was_targeted) {
            double gap = last_target_at > 0 ? now_ms() - last_target_at : 1e9;
            last_target_at = now_ms();

            // Either it is already inside the firing window, or it is simply
            // ON us -- and the second uses the UNFILTERED distance, so a slow
            // ball at arm's length still counts. At that range its speed tells
            // you nothing: it arrives whatever it is doing.
            preempt = (best_eta <= PARRY_ETA) ||
                      (any_dist < PREEMPT_STUDS && any_closing > 0.0f);
            if (preempt)
                // best_eta is its 1e9 sentinel whenever nothing qualified as
                // closing -- which is the usual case for a pre-empt, since that
                // is the whole reason this path exists. Printing it raw put
                // "eta 1000000000.000" in the log.
                if (best_eta < 1e8f)
                    fx_logf("pre-empt: eta %.3fs, %.0f studs", best_eta, any_dist);
                else
                    fx_logf("pre-empt: %.0f studs, closing %.0f", any_dist, any_closing);

            // THE HIGHLIGHT SWITCHING FAST IS THE WHOLE TRIGGER.
            //
            // Nothing about distance or eta. Both were tried and both produce
            // false positives, because neither describes the situation spam is
            // for: a ball 17 studs away at 60 studs/s is closing at 24 and is
            // nearly a second out, and an eta short enough to be urgent is
            // reached on every ordinary approach the careful path handles fine.
            //
            // The highlight coming straight back to us is different in kind. It
            // means the ball was returned immediately -- a frantic exchange at
            // range, where the approach is too short to time and the next one
            // arrives before this one has settled.
            // CHURN ONLY. There was a second trigger on how close the player
            // who just hit it was, and it is gone: a close HANDOVER is already
            // pre-empt's case, and pre-empt answers it with a single tap, which
            // is right. Spamming the same situation only adds presses that can
            // trip the block cooldown.
            //
            // It also fired on things that were not exchanges at all --
            // "121982ms since last, they were 21 studs" is somebody standing
            // near you two minutes later, not a rally.
            if (gap < SPAM_GAP) {
                spam_until   = now_ms() + SPAM_MS;
                next_spam    = 0;
                spam_counted = false;
                fx_logf("  spam: highlight back after %.0fms", gap);
            }
        }
        if (!targeted && was_targeted) armed = true;   // as above: approach over
        was_targeted = targeted;

        // SETTLED BEFORE ANYTHING READS IT. These ran after the pre-empt
        // decision, so pre-empt judged one value and the verdict recorded
        // another: "pre-empt: 28 studs, not closing yet" followed by "parry
        // #472 LANDED eta 2.224s" -- the eta of the fallback candidate, a ball
        // two seconds away, while the pre-empt had fired at something 28 studs
        // from our face. One answer per tick, decided once.
        // Nothing passed the miss test: fall back to the closest-passing ball
        // we saw. Only ever reached while a ball is genuinely closing on us.
        if (!best_ball && alt_ball) { best_eta = alt_eta; best_ball = alt_ball; }
        // Still nothing, but something is within arm's reach: a pre-empt is
        // about to fire at it, so the verdict needs a ball to watch.
        if (!best_ball && any_ball && any_dist < PREEMPT_STUDS) best_ball = any_ball;

        ui_eta  = best_eta < 1e8f ? best_eta : -1.0f;
        ui_dist = near_dist < 1e8f ? near_dist : 0.0f;
        ui_spd  = near_spd;


        // Re-arm once the danger has passed, so the next approach gets its own
        // shot. Hysteresis on purpose: re-arming at the firing threshold would
        // let one ball trigger twice as eta jitters across it.
        if (best_eta > PARRY_ETA * 3.0f) armed = true;

        // A deflection re-arms immediately: it is the real end of an
        // approach, where the hysteresis above waits for the eta to grow back.
        if (any_hit) { armed = true; confirm = 0; }
        // The highlight sighting is NOT cleared here -- doing so made the
        // snippet parry once and then stop.

        // A real approach CLOSES. Requiring eta to be falling as well as small
        // is what separates it from a ball loitering at the edge of the tests:
        // the flicker case never produces a run of shrinking etas, and a thrown
        // ball produces nothing else.
        // FALLING, not merely low. Dropping this was tried and is WORSE: a
        // circling ball's eta dips below the threshold every time the circle
        // swings towards us, without the ball ever arriving, so we fired on a
        // momentary reading and took the hit at full health ("parry #33 TOOK
        // HIT eta 0.450, hp 100 -> 0"). A wasted parry costs the block cooldown
        // as well, making it the more expensive of the two failures.
        //
        // A ball that cannot satisfy this is one whose eta means nothing. That
        // is handled by the chaos path below, not by relaxing this.
        if (best_eta < 1e8f && best_eta <= prev_eta + 0.002f) {
            if (confirm < CONFIRM_TICKS) confirm++;
        } else {
            confirm = 0;
        }
        prev_eta = best_eta;

        // SPAM: only once the tap has visibly failed. Opened further down,
        // by the code that knows whether the flash arrived.
        // No `targeted` here either, for the same reason: the burst is short,
        // bounded, and was opened on evidence. Requiring the highlight to still
        // be up would stop it firing over the final tenth of a second.
        bool spamming = (spam_until > 0 && now_ms() < spam_until);
        if (spamming && now_ms() >= next_spam && fx_input_ok()) {
            next_spam = now_ms() + (double)SPAM_EVERY * POLL_MS;
            send_parry();

            // COUNTED AND LOGGED, once per burst. Pressing the key without
            // telling the accounting is the third time this hole has appeared
            // in a new place -- it looks exactly like "it tried to parry and
            // nothing was logged", and it quietly falsifies the tally as well.
            // The later taps of a burst are the same attempt, not new ones.
            if (!spam_counted) {
                spam_counted = true;
                if (pend_ball) {
                    unknown++;
                    fx_logf("parry #%ld unresolved (a new parry started)", count);
                }
                ++count;
                last_fire  = now_ms();
                armed      = false;
                pend_ball  = best_ball;
                pend_at    = now_ms();
                pend_eta   = best_eta;
                pend_hl    = true;      // spam only ever runs while targeted
                pend_flash = false;
                pend_hp    = (hum && oHP != (uint64_t)-1) ? fx_rf(hum + oHP) : -1.0f;
            }
        }

        // THE HIGHLIGHT IS REQUIRED -- no geometry fallback. A missed parry
        // costs one death; a parry thrown at somebody else's ball costs a
        // death AND the block cooldown, so the next one gets through too.
        // Silence is the cheaper failure, and the warning below makes it
        // visible.
        //
        // Pre-empt bypasses armed/confirm/eta: all three decide WHEN during a
        // flight, and a handover from arm's length has no flight to decide in.
        // NOT WHILE IT IS BEING SPUN. Mid-whirl the ball is not coming for
        // anyone, and a tap now is the tap we will not have when it is thrown.
        if (((armed && confirm >= CONFIRM_TICKS && best_eta <= PARRY_ETA) || preempt) &&
            targeted && !best_spun && fx_input_ok()) {
            if (send_parry()) {
                armed = false;
                // A verdict still open means the previous parry never resolved.
                // Silently reusing the slot is what made the count say 3 while
                // the tally said 2 -- so it is closed and counted, not dropped.
                if (pend_ball) {
                    unknown++;
                    fx_logf("parry #%ld unresolved (a new parry started)", count);
                }
                ++count;
                last_fire = now_ms();
                // Opened here, closed by the verdict block below.
                pend_ball = best_ball;
                pend_at   = now_ms();
                pend_eta  = best_eta;
                pend_hl   = targeted && hl_works;
                pend_flash = false;
                pend_hp   = (hum && oHP != (uint64_t)-1) ? fx_rf(hum + oHP) : -1.0f;
            }
        }
        // Silence has to explain itself. If no highlight has EVER been seen,
        // nothing will ever fire, and without this the snippet would just sit
        // there looking broken.
        if (!warned_no_hl && hl_seen_at == 0 && now_ms() - start_ms > 30000.0) {
            warned_no_hl = true;
            fx_log("! no highlight seen in 30s - nothing will fire. Is this Blade Ball?");
        }

        // ---- the verdict on the last parry ---------------------------------
        // Checked every tick while open, not on the scan clock: the flash is
        // 9ms and the scan is slower than that.
        if (pend_ball && !pend_flash && parry_flash(fx_character())) pend_flash = true;

        // ENDS ON CONFIRMATION, OR ON LOSING THE HIGHLIGHT -- not on the
        // pending slot being empty. That was the test, and it wiped the window
        // on the very tick the rally set it: the rally triggers on the RISING
        // EDGE, before anything has been fired, so no verdict is open yet and
        // the spam lasted exactly one tap.
        // NO SPAM FOR THE RELEASE. It was added and is gone again: once the
        // ball is let go it DESCENDS TOWARDS US, which is an ordinary approach
        // with an eta that means what it says. The careful path handles it, and
        // handles it better -- spamming there fires early, misses, and puts the
        // block on cooldown for the arrival itself.
        //
        // All the spin needs from us is to be ignored while it lasts. See the
        // firing gate below.

        // ONLY A CONFIRMED PARRY STOPS IT EARLY.
        //
        // `!targeted` used to end it too, and that was backwards: the highlight
        // CLEARS BEFORE IMPACT -- measured at 66-93 studs, eta 0.107-0.151 --
        // so the burst was being cut off at the exact moment the ball arrives,
        // which is the moment it exists to cover. It now runs its SPAM_MS out.
        //
        // pend_flash only counts while a verdict is open: it stays true after
        // one closes, and testing it bare cleared the window every tick while
        // the trigger re-opened it every tick, filling the log with one line
        // sixty times a second.
        if (pend_ball && pend_flash) spam_until = 0;

        if (pend_ball) {
            // HEALTH FIRST. Losing health means the ball got through, whatever
            // it did afterwards -- and it is the only branch here that can
            // report a failure, so it must be tested before the turn.
            float hp_now = (hum && oHP != (uint64_t)-1) ? fx_rf(hum + oHP) : -1.0f;
            bool  hurt   = (pend_hp > 0 && hp_now >= 0 && hp_now < pend_hp - 0.5f);
            if (hurt) {
                missed++;
                fx_logf("parry #%ld TOOK HIT eta %.3fs %-10s  hp %.0f -> %.0f  %ld/%ld",
                        count, pend_eta < 1e8f ? pend_eta : 0.0f, pend_hl ? "highlight" : "geometry",
                        pend_hp, hp_now, landed, landed + turned + missed + unknown);
                pend_ball = 0;
                last_line = "took a hit";
            } else if (pend_flash) {
                // THE DIRECT ANSWER. The game flashed ParryHighlight, which is
                // what it does when a parry registers -- no inference from what
                // the ball did afterwards, and a ball that HITS you turns just
                // as hard as one you parried.
                landed++;
                last_line = "landed";
                fx_logf("parry #%ld LANDED   eta %.3fs %-10s  flash   %ld/%ld",
                        count, pend_eta < 1e8f ? pend_eta : 0.0f, pend_hl ? "highlight" : "geometry",
                        landed, landed + turned + missed + unknown);
                pend_ball = 0;
            } else if (pend_turned && now_ms() - pend_at > TURN_MIN_MS) {
                turned++;
                last_line = "turned only";
                fx_logf("parry #%ld turned   eta %.3fs %-10s  %ld/%ld",
                        count, pend_eta < 1e8f ? pend_eta : 0.0f, pend_hl ? "highlight" : "geometry",
                        landed, landed + turned + missed + unknown);
                pend_ball = 0;
            } else if (now_ms() - pend_at > VERDICT_MS) {
                missed++;
                fx_logf("parry #%ld no hit   eta %.3fs %-10s  %ld/%ld",
                        count, pend_eta < 1e8f ? pend_eta : 0.0f, pend_hl ? "highlight" : "geometry",
                        landed, landed + turned + missed + unknown);
                pend_ball = 0;
            }
        }

        fx_sleep(POLL_MS);
    }

    // Back to a normal window before closing, as the other docked snippets do.
    fx_window_dock(0, 0, 0, 0);
    fx_window_close();

    if (count)
        fx_logf("auto parry: stopped. %ld parries: %ld confirmed, %ld turned-only, "
                "%ld failed, %ld unresolved",
                count, landed, turned, missed, unknown);
    else
        fx_log("auto parry: stopped, nothing fired");
}
