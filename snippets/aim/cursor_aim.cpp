// Cursor aim -- nudges the MOUSE onto the player nearest your aim point.
//
// The other aimbot writes Camera.Rotation. This one never writes to the game at
// all: it reads player positions, projects them to screen, and sends RELATIVE
// mouse movement. Everything it does goes through the OS.
//
// Relative movement is the whole trick, and it covers both ways Roblox can be
// set up without needing to know which is in force:
//
//   LOCKED (first person, shift lock, or while right mouse is held). The
//   pointer is pinned and the client reads raw MOVEMENT, so relative movement
//   turns the camera -- exactly what your hand does there.
//
//   FREE CURSOR (third person, shift lock off). Relative movement moves the
//   pointer -- exactly what your hand does there.
//
// Either way the loop closes on the same error: locked, the camera turns and
// the target slides toward the crosshair; free, the pointer slides toward the
// target. Same measurement, same correction.
//
// SetCursorPos was tried first and is a dead end. It is exact in the free case
// and useless in the locked one, where the client re-centres the pointer on its
// own frame and undoes it. Detecting which mode was active -- via
// GetCursorInfo, then via reading the pointer back -- failed twice: Roblox
// blanks the cursor IMAGE rather than hiding it, so Windows still reports one
// as showing, and reading the pointer straight back races the client's
// re-centre. Relative movement needs none of that. Do not reintroduce it.
//
//   hold HOLD_KEY  pull toward the nearest player inside the ring
//   F7             report what it can see
//   Stop           ends it
//
// The ring is the whole idea. Only players inside it are candidates -- so you
// pick the target by pointing roughly at them and this closes the last few
// degrees. Widen RADIUS and it grabs across the screen; narrow it and it only
// helps when you are nearly there.
//
// Inside the ring the nearest wins, but it does NOT re-pick every frame: the
// current target is held until a challenger is clearly the one you meant. See
// SWITCH_FRACTION.
//
// ARM GATE. Mouse movement is not a memory write, so fx_wrblk's gate does not
// cover it -- this checks fx_armed() itself. Disarmed it draws the ring, picks
// a target, says what it would send, and leaves your mouse alone. "Disarmed"
// has to mean nothing happens, or it means nothing.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <windows.h>

// ---- dials ------------------------------------------------------------------

// What to hold.
//
// VK_RBUTTON is fine here, and was not with the old absolute version: holding
// right mouse makes Roblox capture the pointer, which broke that outright. With
// relative movement that capture is simply the locked case, and turning the
// camera is what you wanted anyway.
//
// So choose by preference: RMB combines aiming with camera-pan, a side button
// (VK_XBUTTON1/2) keeps them separate. Avoid VK_LBUTTON -- it is your fire
// button, and fx_claim_key refuses it.
const int HOLD_KEY = VK_RBUTTON;

// Ring radius in SCREEN PIXELS from the aim point. Also what gets drawn.
const float RADIUS = 160.0f;

// Does the ring LIMIT anything, or is it just a circle?
//
// False is Eclipse Aimbot's current setting and a real off: the radius stops
// gating the pick entirely and the nearest target ON SCREEN wins however far
// from the aim point it is. MAX_DIST still applies, so it is unbounded in
// screen space only. The circle is not drawn either -- a boundary that limits
// nothing misstates where the aim can reach.
const bool RING_ENABLED = false;

// ---- matched to aimbot/main.cpp ---------------------------------------------
// The four below are the aimbot's own dials, at its own values, so the two
// behave the same where they can. Keep them in step if you retune either.

// 0..1, the fraction of the remaining error corrected each tick. In the aimbot
// this scales a rotation matrix directly, so it means exactly what it says.
// Here it is multiplied by GAIN to get from pixels to mouse counts, so the two
// dials interact: raising one is the same as raising the other.
const float SMOOTHING = 0.55f;

// Aim this far ahead along the target's own velocity. This is the fix for
// trailing a moving player, and smoothing is NOT: smoothing is a low-pass
// filter, and a low-pass filter is itself a lag. A player strafing at 16
// studs/s at 20 studs sweeps ~45 deg/s, so even 30 ms of round trip sits a
// degree and a half behind permanently.
const float LEAD_SECONDS = 0.07f;

// Ceiling on the lead, as a fraction of the range to the target.
//
// LEAD_SECONDS is a time, so the offset it produces is a fixed world distance
// -- but the ANGLE that subtends goes as 1/distance. Without a cap the same
// lead that is invisible at 100 studs throws the aim ~18 degrees off someone
// at three, and velocity noise is magnified identically. 0.05 caps the lead at
// about 3 degrees whatever the range -- tighter than the 0.12 this used to
// carry, matching Eclipse Aimbot, which pairs it with a LONGER lead time.
const float LEAD_MAX_FRACTION = 0.05f;

// Hold the target you acquired for as long as the key is down, and let nothing
// take it: not a closer player, not the target leaving the ring, not a tick
// where they could not be read.
//
// You picked them by pointing at them when you pressed the key. Anything that
// reassigns the target after that is the aim overruling that choice, and every
// version of "it let go when I didn't want it to" traced back to one of those
// rules firing.
//
// False restores the camera aimbot's behaviour: a challenger clearly closer to
// the crosshair may take over after a short dwell. Better when sweeping across
// a crowd, worse when you have chosen someone specific.
const bool HARD_LOCK = false;

// Only consulted when HARD_LOCK is false. A challenger must be closer by this
// fraction AND hold that lead for the dwell before it takes the target.
const float  SWITCH_FRACTION = 0.8f;
const double SWITCH_DWELL_MS = 25.0;

// Below this error, leave it alone. Movement is sent in whole mouse counts, so
// the fractional part of every step is lost; without a deadzone a target half a
// count away twitches back and forth forever.
const float DEADZONE_PX = 2.0f;

// Screen pixels of aim movement per mouse count depends on your in-game
// sensitivity, which this cannot read. The aimbot has no equivalent problem --
// it turns the camera in world space, so there is no conversion at all.
//
// GAIN is that conversion, set by feel. There WAS an auto-calibrator that
// measured it -- it watched how far the error moved per count sent and divided
// the correction by the result. It is gone, for the reason Eclipse Aimbot
// dropped it: the measurement needs the error to close cleanly along the push,
// and at higher smoothing the aim reaches the target before a clean sample
// exists, so it converged on a wrong ratio and aimed off. A fixed number is
// both simpler and more predictable.
const float GAIN = 0.30f;

// Never AIM at your own team. They are still drawn, in TEAM_RGB, because
// knowing where a teammate is matters as much as knowing where an enemy is
// -- you just do not want the aim grabbing them. Ignored in games without
// teams; see same_team().
const bool TEAM_CHECK = true;
const int  TEAM_RGB   = 0x6EE7A8;   // green for friendly, white for the rest

const float MAX_DIST = 800.0f;    // studs; ignore anything further

// Head is the smaller target and the one worth hitting; the root part is
// steadier because it moves less as animations play.
const bool AIM_AT_HEAD = true;

// TWO RATES, because there are two jobs. collect() walks every player and
// projects each one, so a tick is not free -- and running the full scan at the
// aiming rate the whole time is a core spent keeping an overlay fresher than
// any monitor can show. Measured in Rivals: one rate at 360 Hz cost ~40 FPS
// against the standalone aimbot, which has always used this split.
//
// AIM_HZ IS DELIBERATELY UNCHANGED at the value it has always run. SMOOTHING
// corrects a FRACTION OF THE CURRENT ERROR PER TICK, so the tick rate is part
// of how the aim feels -- lowering it while the key is held would slow the
// pull and quietly retune the dials below. Only the idle rate drops.
const unsigned AIM_HZ  = 360;   // while HOLD_KEY is down: unchanged, do not "optimise"
const unsigned IDLE_HZ = 60;    // ESP only: nothing on screen needs more

// And the DRAWING is throttled separately from the loop, so holding the key
// does not also drag the overlay to 360 Hz. Boxes at 60 are already fresher
// than the monitor shows them; building the JSON and posting it six times per
// frame is work with no visible result.
const unsigned ESP_HZ = 60;


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

// Studs/second for a part. Zeroed rather than failed when it cannot be
// trusted, so the caller can always add lead * vel without a branch.
//
// The speed cap matters: a rag-dolling or mid-teleport part reads absurd
// values, and leading by one of those throws the aim clean off the map.
static bool part_vel(uint64_t part, float3* out) {
    *out = float3{ 0, 0, 0 };
    uint64_t oPrim = fx_off("BasePart.Primitive");
    uint64_t oVel  = fx_off("Primitive.AssemblyLinearVelocity");
    if (oPrim == (uint64_t)-1 || oVel == (uint64_t)-1) return false;
    uint64_t prim = fx_rd(part + oPrim);
    if (prim < 0x10000 || prim > 0x7ff000000000ULL) return false;
    float3 v = read_v3(prim + oVel);
    if (!(v.x == v.x) || !(v.y == v.y) || !(v.z == v.z)) return false;
    if (v.x*v.x + v.y*v.y + v.z*v.z > 500.0f * 500.0f) return false;
    *out = v;
    return true;
}

// Roblox's own world-to-clip matrix, via fx_screen. Authoritative: whatever the
// engine does to its projection -- scripted FOV, a custom camera -- it reflects
// it, because it is the matrix it renders with. Rebuilding projection from
// Camera.Rotation and FieldOfView was tried and is wrong for exactly that
// reason.
//
// `!= 0` here, NOT `== 1`, and the difference is load-bearing. fx_screen
// distinguishes "off the edge of the frame" (-1) from "no answer at all" (0),
// and this aim wants the former to keep working: it turns the view by relative
// mouse movement, so a target just past the edge is still reachable, and
// dropping it there is exactly how the lock used to be lost mid-turn. The ESP
// snippets test `== 1` because they must not draw what is not visible.
static bool project_world(float3 w, float* sx, float* sy) {
    return fx_screen(w, sx, sy) != 0;
}

// ---- the Roblox window ------------------------------------------------------
// Projection gives pixels inside the CLIENT AREA. The pointer and the overlay
// are both in MONITOR pixels. Without the client-area origin every target sits
// offset by the window's border and title bar -- a constant error that hides at
// close range, where characters are large, and dominates at distance.
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
// Player.Team points at a Team instance, so "same team" is a pointer compare.
//
// DISABLED when our own Team is null, and that is the important part: in a game
// with no teams every Player.Team is 0, so a naive equality test makes everyone
// a teammate and the aim finds nothing at all. No team means no way to tell
// friend from foe, which means everyone is a target.
// Both Team pointers come from the roster now, so this reads nothing at all.
static bool same_team(uint64_t mine, uint64_t theirs) {
    if (!TEAM_CHECK || !mine) return false;   // no teams in this game
    return theirs == mine;
}

// ---- targets ----------------------------------------------------------------

struct Cand {
    // The Player instance. THIS is the identity, not the name: fx_name reads a
    // string out of the process and can transiently return "" -- one bad read
    // was enough to lose the lock and re-acquire somebody else, which is what
    // the drop-and-regain looked like from the outside. Names are also not
    // unique, so two players with the same one made the match ambiguous.
    uint64_t player;
    char  name[48];    // display only
    float mx, my;      // monitor pixels -- directly comparable with the pointer
    float bx, by, bw, bh;
    float dist;        // studs
    float aim_d;       // pixels from the aim point; what the pick is made on
    bool  team;        // drawn, never aimed at
};

// Is this Player still in the server at all?
//
// A target drops out of the candidate list for several innocent reasons -- it
// went behind the camera and cannot be projected, a part was caught
// mid-rebuild, it stepped past MAX_DIST. None of those mean the player left,
// and under a hard lock none of them should cost you the target. This is what
// separates "cannot see them this instant" from "they are gone", so the lock
// can wait out the first and release on the second.
static bool player_present(uint64_t pl) {
    if (!pl) return false;
    // The roster IS the membership list, and it is class-checked -- a child of
    // Players that is not a Player never appears in it, so this cannot match
    // one by accident the way the raw child walk could.
    fx_player_info r[64];
    int n = fx_players(r, 64);
    for (int i = 0; i < n; i++) if (r[i].player == pl) return true;
    return false;
}

static int collect(Cand* out, int max, POINT origin, const RbxWin& rw) {
    // fx_players: one walk over Players, with each name, Character and Team
    // already resolved and memoised. Filters nothing -- unspawned players
    // arrive with a zero model and every skip below is still ours.
    fx_player_info pl[64];
    int np = fx_players(pl, 64);
    if (np <= 0) return 0;

    // Our own Team, from our own roster entry.
    uint64_t my_team = 0;
    for (int i = 0; i < np; i++) if (pl[i].is_local) my_team = pl[i].team;

    // Camera position, for the distance label and the range filter.
    float3 eye = { 0, 0, 0 };
    {
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
    }

    int n = 0;
    for (int i = 0; i < np && n < max; i++) {
        if (pl[i].is_local) continue;
        bool mate = same_team(my_team, pl[i].team);
        uint64_t chr = pl[i].model;
        if (!chr) continue;                       // not spawned

        // Aim point and box anchor are different questions, read separately --
        // the same split the other aimbot's ESP needed.
        uint64_t aim = AIM_AT_HEAD ? fx_part(chr, "Head") : 0;
        if (!aim) aim = fx_part(chr, "HumanoidRootPart");
        if (!aim) aim = fx_part(chr, "Torso");
        if (!aim) continue;

        float3 ap;
        if (!part_pos(aim, &ap)) continue;

        float dx = ap.x - eye.x, dy = ap.y - eye.y, dz = ap.z - eye.z;
        float dist = sqrtf(dx*dx + dy*dy + dz*dz);
        if (dist > MAX_DIST) continue;

        // Lead the AIM point only. The box below is built from the root's real
        // position, or every box would sit ahead of its player.
        float3 lead = ap;
        if (LEAD_SECONDS > 0.0f) {
            float3 v;
            if (part_vel(aim, &v)) {
                float lx = v.x * LEAD_SECONDS;
                float ly = v.y * LEAD_SECONDS;
                float lz = v.z * LEAD_SECONDS;

                // Cap the lead as a FRACTION OF RANGE. The offset is a fixed
                // distance in the world, but the angle it subtends grows as
                // 1/distance -- one stud of lead is half a degree at 100 studs
                // and about 18 at three. Uncapped, that is the aim flinging
                // itself off a close target every time they strafe, while
                // looking perfectly calm at range. Which is exactly how it
                // behaved.
                //
                // Velocity noise rides the same curve, so this bounds the
                // jitter too. 0.12 is roughly a 7 degree ceiling.
                float len = sqrtf(lx*lx + ly*ly + lz*lz);
                float cap = LEAD_MAX_FRACTION * dist;
                if (len > cap && len > 1e-4f) {
                    float k = cap / len;
                    lx *= k; ly *= k; lz *= k;
                }
                lead.x += lx; lead.y += ly; lead.z += lz;
            }
        }

        // Fall back to the unled point if the led one projects off screen, so
        // a fast target does not blink out of the candidate list.
        float sx, sy;
        if (!project_world(lead, &sx, &sy) &&
            !project_world(ap, &sx, &sy)) continue;

        Cand& c = out[n];
        c.player = pl[i].player;
        c.team = mate;
        snprintf(c.name, sizeof c.name, "%s", pl[i].name);
        if (!c.name[0]) snprintf(c.name, sizeof c.name, "player");
        c.mx = sx + rw.x;
        c.my = sy + rw.y;
        c.dist = dist;
        float ex = c.mx - (float)origin.x, ey = c.my - (float)origin.y;
        c.aim_d = sqrtf(ex*ex + ey*ey);

        // Box around the root part from a +-3 stud span -- identical to
        // esp_overlay.cpp, so the two draw the same shape at the same distance.
        // pl[i].root IS fx_part(chr, "HumanoidRootPart"), checked 15/15 live.
        uint64_t root = pl[i].root;
        if (!root) root = fx_part(chr, "Torso");
        if (!root) root = fx_part(chr, "UpperTorso");
        float3 rp = ap;
        if (root) { float3 t; if (part_pos(root, &t)) rp = t; }

        float bsx, bsy, tsx, tsy, fsx, fsy;
        float3 top = { rp.x, rp.y + 3.0f, rp.z };
        float3 bot = { rp.x, rp.y - 3.0f, rp.z };
        if (project_world(rp, &bsx, &bsy) &&
            project_world(top, &tsx, &tsy) &&
            project_world(bot, &fsx, &fsy)) {
            float bh = fabsf(fsy - tsy);
            if (bh < 6.0f) bh = 6.0f;
            float bw = bh * 0.6f;
            c.bx = bsx - bw * 0.5f + rw.x;
            c.by = bsy - bh * 0.5f + rw.y;
            c.bw = bw; c.bh = bh;
        } else {
            c.bw = c.bh = 0.0f;                   // no box; still a candidate
        }
        n++;
    }
    return n;
}

// ---- run --------------------------------------------------------------------

extern "C" void fx_run() {
    char b[256];

    fx_log("cursor aim - hold the aim key, F7 to report, Stop to end");
    if (!fx_armed())
        fx_log("! disarmed: it will draw and pick, but send no mouse movement");
    fx_overlay_on();

    Cand cands[64];
    char json[8192];
    unsigned dm = fx_dm_gen();
    bool  ovl_shown = true;
    bool  warned_disarmed = false;
    float last_err = -1.0f;

    // Stickiness. Player POINTERS, not names -- see the note on Cand::player.
    uint64_t sticky = 0;
    uint64_t chall  = 0;
    double   chall_since = 0.0;

    // How many consecutive ticks a held target may be missing from the
    // candidate list before the lock is released. At 144 Hz this is about
    // 70 ms, long enough to ride out a part caught mid-rebuild or a frame
    // where the projection failed, short enough that a genuinely dead or
    // departed target does not hold the aim hostage.
    const int LOST_GRACE_TICKS = 10;
    int misses = 0;

    // Sub-count remainder, so a demand smaller than one mouse count is spent
    // over several ticks rather than rounded away. See the send site.
    float carry_x = 0, carry_y = 0;

    // Last step and last error, kept only for the F7 report's direction check.
    // They used to feed the pixels-per-count measurement as well; that is gone.
    float last_sx = 0, last_sy = 0, last_ex = 0, last_ey = 0;
    char  last_name[48] = "";

    while (fx_running()) {
        if (fx_dm_gen() != dm) {
            dm = fx_dm_gen();
            fx_overlay_post("[]");
            fx_log("world replaced - holding off until it settles");
            if (!fx_sleep(500)) break;
            continue;
        }

        // Alt-tabbed: draw nothing, send nothing. The ring is in monitor
        // pixels, so it would otherwise sit over whatever is now in front.
        if (!fx_focused()) {
            if (ovl_shown) { fx_overlay_post("[]"); ovl_shown = false; }
            if (!fx_sleep(100)) break;
            continue;
        }
        ovl_shown = true;

        RbxWin rw;
        POINT  cur;
        if (!get_rbx_window(&rw) || !GetCursorPos(&cur)) {
            if (!fx_sleep(100)) break;
            continue;
        }

        // The aim point is the POINTER, in both modes. Locked, the game pins it
        // to the middle -- which is where the crosshair is -- so reading it
        // gives the right answer without asking which mode is in force.
        POINT origin = cur;

        int n = collect(cands, 64, origin, rw);

        // Nearest to the aim point, and only inside the ring -- that is what
        // makes the ring mean something rather than being decoration over a
        // screen-wide aimbot. Unless RING_ENABLED is false, in which case a
        // screen-wide aimbot is exactly what was asked for.
        const float ring = RING_ENABLED ? RADIUS : 1e9f;

        int nearest = -1;
        float nd = ring;
        for (int i = 0; i < n; i++)
            if (!cands[i].team && cands[i].aim_d < nd) { nd = cands[i].aim_d; nearest = i; }

        // Two SEPARATE questions, and conflating them is what kept dropping the
        // lock for no visible reason.
        //
        //   key_down  governs how long the lock LIVES
        //   may_send  governs whether movement may be SENT right now
        //
        // fx_input_ok() is fx_focused() && !fx_typing(), and fx_typing chains
        // several pointer reads to reach UserInputService.WindowInputState.
        // CurrentTextBox. One transient bad read there used to make `pulling`
        // false for a single tick, which released the target outright -- an
        // unrelated subsystem hiccup losing your aim. Alt-tabbing should stop
        // the mouse moving, not forget who you were aiming at.
        bool key_down = fx_key(HOLD_KEY) != 0;
        bool may_send = fx_input_ok();
        bool pulling  = key_down;

        // Stickiness, same rule as the aimbot: hold the current target and
        // hand off only when a challenger is clearly the one you mean.
        //
        // The ring gates ACQUISITION, not retention. It used to gate both, and
        // that is what made the aim let go mid-pull: the moment a target
        // drifted past RADIUS -- which it does while the aim is still closing,
        // or when they strafe -- the lock dropped and re-acquired someone else.
        // Once held, a target is kept until the key is released or it is gone.
        int best = -1;
        {
            int held = -1;
            for (int i = 0; i < n; i++)
                if (!cands[i].team && cands[i].player == sticky) { held = i; break; }

            if (!pulling) {
                // Releasing drops the lock, so every press picks fresh. That is
                // what keeps the ring meaningful: it decides what this press
                // may grab, rather than being bypassed by a stale hold.
                sticky = 0; chall = 0; misses = 0;
                best = -1;
            } else if (held < 0 && sticky) {
                // Held target is not in the list THIS tick. Usually transient:
                // behind the camera so it cannot be projected, a part caught
                // mid-rebuild, a read that missed. Never hand the lock to
                // somebody else over that.
                misses++;
                if (HARD_LOCK && player_present(sticky)) {
                    // Still in the server, just not visible right now. Wait as
                    // long as it takes -- a target you turned away from is
                    // exactly the one you are turning back toward.
                    best = -1;
                } else if (misses <= LOST_GRACE_TICKS) {
                    best = -1;
                } else {
                    // Genuinely gone: left, died, despawned.
                    sticky = 0; chall = 0; misses = 0;
                    best = nearest;
                    if (nearest >= 0) sticky = cands[nearest].player;
                }
            } else if (held < 0) {
                // Acquire -- and only from inside the ring.
                misses = 0;
                sticky = nearest >= 0 ? cands[nearest].player : 0;
                chall = 0;
                best = nearest;
            } else if (HARD_LOCK) {
                // Whoever was acquired on the key press is the target for as
                // long as the key is down. Nothing takes it away: not somebody
                // closer to the crosshair, not the target leaving the ring, not
                // a tick where they could not be read.
                //
                // This is the whole point of a hold-to-aim key -- you chose
                // them by pointing, and the aim should not second-guess that.
                // Set HARD_LOCK false for the aimbot's behaviour instead, where
                // a clearly-closer challenger can take over after a dwell.
                misses = 0;
                chall = 0;
                best = held;
            } else if (nearest >= 0 && nearest != held &&
                       cands[nearest].aim_d < cands[held].aim_d * SWITCH_FRACTION) {
                misses = 0;
                double now = (double)GetTickCount64();
                if (chall != cands[nearest].player) {
                    chall = cands[nearest].player;
                    chall_since = now;
                }
                if (now - chall_since >= SWITCH_DWELL_MS) {
                    sticky = chall;
                    chall = 0;
                    best = nearest;
                } else {
                    // Keep pulling the CURRENT target while a challenger is
                    // timed. Aiming at neither was the old behaviour and it is
                    // what made the pull stutter every time somebody crossed
                    // the ring: 25 ms of dead time, repeatedly. The handoff
                    // still completes, it just does not pause first.
                    best = held;
                }
            } else {
                misses = 0;
                chall = 0;
                best = held;
            }
        }

        if (pulling && best >= 0) {
            float ex = cands[best].mx - (float)origin.x;
            float ey = cands[best].my - (float)origin.y;
            last_err = sqrtf(ex*ex + ey*ey);

            // Kept for the F7 report only. It used to gate the pixels-per-count
            // measurement, which had to ignore the tick after a handoff: the
            // error jumps by the gap between two players, which says nothing
            // about what our counts achieved.
            snprintf(last_name, sizeof last_name, "%s", cands[best].name);

            // NO in-flight ledger. There was one -- it tracked counts sent but
            // not yet reflected in the view matrix, to stop stacked corrections
            // overshooting. It made things worse in the way that matters: after
            // one large step it booked a big pending figure, which suppressed
            // the next several corrections, so the aim snapped once and then
            // sat there while the target walked away. Clamping it to stop the
            // sign flipping only turned "wrong direction" into "no movement".
            //
            // The aimbot has no such thing: every tick it corrects SMOOTHING of
            // the error as it is RIGHT NOW, and that self-corrects whatever the
            // latency is. Same here. Simple beats clever.

            // Movement is sent in whole mouse counts, so there is a QUANTISATION
            // FLOOR: one count moves the aim a fixed number of pixels -- 3.169
            // measured in a real session -- and the aim cannot be placed closer
            // than half of that to anything. This is the one respect in which a
            // mouse aim cannot match the camera write, which sets a float
            // rotation and has no floor at all. Lower your in-game sensitivity
            // to shrink it.
            //
            // That floor used to be MEASURED and folded in here. The measurement
            // is gone -- it needed the error to close cleanly along the push,
            // and at higher smoothing the aim arrives before a clean sample
            // exists, so it converged on a wrong ratio and aimed off. DEADZONE_PX
            // is the whole of it now, same as Eclipse Aimbot.
            const float dz = DEADZONE_PX;

            if (fabsf(ex) > dz || fabsf(ey) > dz) {
                // Close SMOOTHING of the current error. GAIN is the conversion
                // from pixels to counts, set by feel rather than measured.
                float cx = ex * SMOOTHING * GAIN;
                float cy = ey * SMOOTHING * GAIN;

                // Ceiling on one tick, so a large error cannot turn into a step
                // big enough to spin the view. Scaled together so the direction
                // survives the clamp rather than each axis being clipped
                // independently.
                const float MAX_COUNTS = 220.0f;
                float mag = sqrtf(cx*cx + cy*cy);
                if (mag > MAX_COUNTS) { float k = MAX_COUNTS / mag; cx *= k; cy *= k; }

                // Carry the fraction instead of rounding it away.
                //
                // A demand of 0.4 counts rounds to zero every tick, so the aim
                // simply never closes the last bit of error; 0.6 rounds to a
                // whole count and overshoots. Both are the same bug: throwing
                // away the sub-count remainder. Accumulating it means four
                // ticks of 0.4 spend a real count on the fourth, so the AVERAGE
                // rate is right even though each individual step is an integer.
                //
                // This is what recovers precision below the quantisation floor.
                // Not perfectly -- the aim still lands on the lattice at any
                // instant -- but it stops the error being permanently stranded
                // between two counts.
                float wx = cx + carry_x;
                float wy = cy + carry_y;
                LONG dx = (LONG)truncf(wx);
                LONG dy = (LONG)truncf(wy);
                carry_x = wx - (float)dx;
                carry_y = wy - (float)dy;
                // Bounded so a long run of suppressed ticks cannot save up a
                // jump. It can never legitimately exceed one count.
                if (carry_x >  1.0f) carry_x =  1.0f;
                if (carry_x < -1.0f) carry_x = -1.0f;
                if (carry_y >  1.0f) carry_y =  1.0f;
                if (carry_y < -1.0f) carry_y = -1.0f;

                if (!fx_armed()) {
                    if (!warned_disarmed) {
                        snprintf(b, sizeof b,
                                 "would send %+ld,%+ld toward %s - disarmed, nothing sent",
                                 dx, dy, cands[best].name);
                        fx_log(b);
                        warned_disarmed = true;
                    }
                } else if (!may_send) {
                    // Alt-tabbed, or typing in chat. The lock is KEPT -- only
                    // the movement is withheld, so tabbing out and back does
                    // not cost you the target.
                } else if (dx || dy) {
                    INPUT in;
                    memset(&in, 0, sizeof in);
                    in.type = INPUT_MOUSE;
                    in.mi.dwFlags = MOUSEEVENTF_MOVE;      // RELATIVE, not absolute
                    in.mi.dx = dx;
                    in.mi.dy = dy;
                    SendInput(1, &in, sizeof in);

                    // The input half of the measurement the next tick finishes.
                    last_sx = (float)dx; last_sy = (float)dy;
                    last_ex = ex;        last_ey = ey;
                } else {
                }
            }
        } else {
            last_err = -1.0f;
        }
        if (!pulling) {
            warned_disarmed = false;
            // Releasing breaks the measurement chain: pairing a fresh error
            // remainder goes with it -- a fraction saved up for one target
            // must not be spent on the next.
            carry_x = carry_y = 0.0f;
            chall = 0;
        }

        // THE DRAW IS ON ITS OWN CLOCK. The loop may be running at AIM_HZ, but
        // nothing on screen benefits from boxes rebuilt faster than ESP_HZ --
        // and the JSON build plus the post is the most expensive thing in this
        // tick that is not a memory read. Skipped, not shortened: the aim above
        // has already run at full rate, which is the half that matters.
        static double last_draw_ms = 0.0;
        double now_ms = (double)GetTickCount64();
        if (now_ms - last_draw_ms >= 1000.0 / (double)ESP_HZ) {
        last_draw_ms = now_ms;

        // Ring first so the boxes draw over it. sx/sy is the CENTRE for a
        // circle -- the one entry in this format where it is not a corner.
        // Drawn only when it limits something: a circle the pull ignores would
        // misstate where the aim can reach.
        int o = RING_ENABLED
                  ? snprintf(json, sizeof json,
                             "[{\"sx\":%ld,\"sy\":%ld,\"r\":%.0f}",
                             origin.x, origin.y, RADIUS)
                  : snprintf(json, sizeof json, "[{\"sx\":0,\"sy\":0,\"r\":0}");
        for (int i = 0; i < n && o + 160 < (int)sizeof json; i++) {
            // Every player on screen, not just the ones in the ring. The ring
            // decides what the PULL may grab; it does not decide what you see.
            if (cands[i].bw <= 0.0f) continue;
            o += snprintf(json + o, sizeof json - o,
                          ",{\"n\":\"%s\",\"sx\":%.1f,\"sy\":%.1f,"
                          "\"w\":%.1f,\"h\":%.1f,\"d\":%.1f,\"c\":%d}",
                          cands[i].name, cands[i].bx, cands[i].by,
                          cands[i].bw, cands[i].bh, cands[i].dist,
                          cands[i].team ? TEAM_RGB : 0xFFFFFF);
        }
        snprintf(json + o, sizeof json - o, "]");
        fx_overlay_post(json);
        }   // ESP_HZ gate

        if (fx_key_pressed(VK_F7) && fx_input_ok()) {
            int inring = 0;
            for (int i = 0; i < n; i++) if (!cands[i].team && cands[i].aim_d < ring) inring++;
            snprintf(b, sizeof b,
                     "%d player%s in range, %d in the ring | aim point %ld,%ld | %s",
                     n, n == 1 ? "" : "s", inring, origin.x, origin.y,
                     fx_armed() ? "armed" : "disarmed");
            fx_log(b);
            if (best >= 0) {
                snprintf(b, sizeof b, "  nearest: %s at %.0f px, %.0f studs",
                         cands[best].name, cands[best].aim_d, cands[best].dist);
                fx_log(b);
            }
            if (last_err >= 0.0f) {
                snprintf(b, sizeof b, "  pull error %.0f px | gain %.2f",
                         last_err, GAIN);
                fx_log(b);

                // Did the last step actually point AT the target? If these
                // disagree in sign the aim is being driven the wrong way.
                bool agree_x = (last_sx == 0) || ((last_sx < 0) == (last_ex < 0));
                bool agree_y = (last_sy == 0) || ((last_sy < 0) == (last_ey < 0));
                snprintf(b, sizeof b,
                         "  last step %+.0f,%+.0f counts for error %+.0f,%+.0f px  -> %s",
                         last_sx, last_sy, last_ex, last_ey,
                         (agree_x && agree_y) ? "correct direction"
                                              : "WRONG DIRECTION");
                fx_log(b);
            }
            if (sticky) {
                // Retention: a held target outside the ring is KEPT rather than
                // dropped, and a briefly-missing one is waited on. Report both,
                // since they are the two behaviours that used to look like the
                // aim randomly letting go.
                float hd = -1.0f;
                const char* hn = "(not visible this tick)";
                for (int i = 0; i < n; i++)
                    if (cands[i].player == sticky) { hd = cands[i].aim_d; hn = cands[i].name; break; }

                if (hd < 0.0f)
                    snprintf(b, sizeof b, "  holding %s - missing %d/%d ticks, waiting",
                             hn, misses, LOST_GRACE_TICKS);
                else
                    snprintf(b, sizeof b, "  holding %s at %.0f px%s", hn, hd,
                             (RING_ENABLED && hd >= RADIUS) ? "  (outside the ring - retained)" : "");
                fx_log(b);
                if (chall) fx_log("  a challenger is being timed");
            }
        }

        // Paced, not slept: collect() walks every player and projects each one,
        // so a fixed 16 ms sleep on top of that lands well under 60 Hz.
        //
        // TWO RATES. The scan only has to keep up with the AIM, and the aim
        // only runs while the key is down -- the rest of the time this loop
        // exists to feed the overlay, which is capped at ESP_HZ anyway. One
        // rate at AIM_HZ meant walking and projecting every player 360 times a
        // second to draw boxes 60 times a second.
        if (!fx_pace(pulling ? AIM_HZ : IDLE_HZ)) break;
    }

    fx_overlay_off();
    fx_log("cursor aim stopped");
}
