// Fly -- zero the world's gravity, then steer with velocity.
//
//   Disarmed -> logs the writes it would make, changes nothing.
//   ARMED    -> actually writes them.
//
//   F toggles.  WASD moves relative to where you are LOOKING.
//   E climbs, Q drops.  Release everything and you stop where you are.
//
// GRAVITY IS GLOBAL -- it is the whole map's, not yours. Every player and every
// unanchored part floats while this is on. It is restored on toggle-off, on
// respawn, on a world change and on Stop; that restore is the most important
// code in this file, because leaving it at zero leaves the place floating with
// nothing running to put it back.
//
// The hover is free rather than tuned: with gravity off nothing accelerates
// you, so writing a zero velocity IS holding still. No altitude target, no gain.
//
// The velocity write RACES the engine, which rewrites AssemblyLinearVelocity
// every physics step -- ours only counts if it lands before the next
// integration. So the loop never paces itself: SLEEP_MS is 0, meaning yield and
// go again. A pacer sleeps out the remainder of its interval, which is exactly
// the wrong behaviour here. Do not put one back in.
//
// Gravity is read straight from the conf: Workspace.World -> World.Gravity.
// The log line prints the value it resolved before writing, so a wrong offset
// shows up there as a number that is not a gravity. docs/fly.md has the rest.

#include <chrono>
#include <cmath>
#include <string>
#include <vector>

// A lambda, not a function: this file is a BARE-BODY snippet -- the console
// wraps the whole thing in fx_run(), so a free function definition would close
// that body early and everything after it fails to parse.
auto now_ms = []() {
    using namespace std::chrono;
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
};

const int   TOGGLE_KEY  = 'X';
const float SPEED       = 90.0f;   // studs/second
const unsigned SLEEP_MS = 0;       // 0 = spin; see the note above
// How often the Primitive is re-derived even when we already have one. See the
// re-resolve block: fx_gen is not enough on its own.
const double PRIM_RECHECK_MS = 200.0;

uint64_t oVel   = fx_off("Primitive.AssemblyLinearVelocity");
uint64_t oPrim  = fx_off("BasePart.Primitive");
uint64_t oCam   = fx_off("Workspace.CurrentCamera");
uint64_t oRot   = fx_off("Camera.Rotation");
if (oVel == (uint64_t)-1 || oPrim == (uint64_t)-1 || oCam == (uint64_t)-1 ||
    oRot == (uint64_t)-1) {
    fx_log("x missing an offset: Primitive.AssemblyLinearVelocity, "
           "BasePart.Primitive or the Camera bits");
    return;
}

// Workspace and the camera, CACHED.
//
// fx_service walks every service in the DataModel comparing class names, and
// fx.h says outright it is far too expensive for a loop -- this one calls it
// with SLEEP_MS = 0, so it was paying that walk thousands of times a second to
// re-answer a question whose answer only changes when the world does.
//
// Dropped on a DataModel change (both are freed with it) and whenever the
// rotation read fails, which covers the camera being swapped without a world
// change.
uint64_t ws  = 0;
uint64_t cam = 0;

// The world's gravity: Workspace.World -> World.Gravity, straight from the conf.
uint64_t grav_addr  = 0;
float    grav_value = 0.0f;
std::string grav_why;

auto find_gravity = [&]() {
    grav_addr = 0;
    grav_why.clear();

    uint64_t oWorld = fx_off("Workspace.World");
    uint64_t oWG    = fx_off("World.Gravity");
    if (oWorld == (uint64_t)-1 || oWG == (uint64_t)-1) {
        grav_why = "conf has no Workspace.World / World.Gravity";
        return false;
    }

    if (!ws) ws = fx_service("Workspace");
    if (!ws) { grav_why = "no Workspace service yet"; return false; }

    uint64_t world = fx_rd(ws + oWorld);
    if (world < 0x10000 || world > 0x7ff000000000ULL) {
        char m[96];
        snprintf(m, sizeof m, "Workspace.World reads 0x%llx", (unsigned long long)world);
        grav_why = m;
        return false;
    }

    grav_addr = world + oWG;

    // THE RESTORE VALUE COMES FROM THE MIRROR, NOT FROM THE LIVE FIELD.
    //
    // Reading the live field looks obvious and is a trap: if a previous run
    // died without restoring -- killed, crashed, a timeout -- gravity is
    // already 0, so we capture 0 as "the original" and put 0 back on exit.
    // One bad exit then poisons every run after it, and the log says
    // "gravity 0.00 -> 0" while nothing flies. That happened.
    //
    // Workspace.ReadOnlyGravity is a MIRROR: writing it does nothing, so it
    // still holds the true value even when the live field has been zeroed.
    // That is what makes a stuck world recoverable.
    uint64_t oRO = fx_off("Workspace.ReadOnlyGravity");
    float mirror = (oRO != (uint64_t)-1) ? fx_rf(ws + oRO) : 0.0f;
    float live   = fx_rf(grav_addr);
    grav_value = (mirror > 1.0f) ? mirror : live;

    if (mirror > 1.0f && fabsf(live - mirror) > 0.001f)
        fx_logf("  ! gravity reads %.2f but should be %.2f -- a previous run left"
                " it that way; restoring to %.2f on exit", live, mirror, mirror);
    return true;
};

// ONE address, and which one took a while to pin down.
//
// TWO adjacent floats in the world object both read 196.2, and only one of them
// is gravity -- they are indistinguishable to a value scan, which is what made
// this confusing. Isolated by testing each alone: zeroing the first (+0x228)
// leaves you rising while thrusting and sinking back afterwards; zeroing the
// second (+0x22c) flies correctly. So +0x22c is gravity, +0x228 is a different
// field that happens to hold the same number, and the conf points at the right
// one.
//
// Do NOT add a second write at grav_addr + 4. Zeroing an unidentified float
// next to gravity is not free -- writing zeros into every float that merely
// matched the value is what crashed the client once already.
auto set_gravity = [&](float v) {
    if (grav_addr) fx_wrblk(grav_addr, &v, 4);
};

fx_logf("fly: '%c' toggles. WASD by camera, E up, Q down.", (char)TOGGLE_KEY);
fx_log("  ! gravity is GLOBAL - the whole map floats while this is on");
fx_log(fx_armed() ? "  ARMED -- this will actually write."
                  : "  disarmed -- writes are logged, not performed.");

// TWO counters, because two different things go stale and they are NOT the
// same event. Treating them as one is why flying used to end at your first
// death:
//
//   fx_gen()     your CHARACTER changed -- died, respawned, rig rebuilt. Only
//                `prim` is stale. The World object is untouched, so the gravity
//                addresses are still good and there is no reason to stop
//                flying: re-resolve the body and carry on.
//
//   fx_dm_gen()  the DATAMODEL changed -- left, joined, teleported. The World
//                object is freed with everything else, so the gravity addresses
//                point at nothing. Restore through them while they may still be
//                valid, then drop the lot.
//
// Both re-resolve on `|| !prim` as well, not just on the edge: the generation
// ticks before the new character exists, so resolving once when it moves would
// latch a 0 forever. See fx_gen in fx.h.
unsigned gen   = fx_gen() - 1;      // never compare fx_gen against 0
unsigned dmgen = fx_dm_gen();
uint64_t prim  = 0;
double   prim_next = 0;
bool flying = false;

while (fx_running()) {
    if (fx_dm_gen() != dmgen) {
        dmgen = fx_dm_gen();
        if (flying) {
            set_gravity(grav_value);
            flying = false;
            fx_log("  fly off - world changed, gravity restored");
        }
        grav_addr = 0;              // it belonged to the old World
        prim = 0;
        ws = cam = 0;               // freed with the DataModel
    }

    // RE-DERIVED ON A CLOCK, not only on the generation edge -- and that is not
    // belt-and-braces, it is the whole fix for "velocity stops working after I
    // die". Measured across three deaths, logging every pointer change:
    //
    //   [18627] fx_gen -> 4   latched prim=2920d9de680  root=2916fc56000
    //   [18727] chr=2920db13c00 root=2916fc57c00 live=2920d9d8000  *** STALE ***
    //
    // THE ROOT PART IS REPLACED ~100ms AFTER THE RESPAWN, with the CHARACTER
    // pointer unchanged and NO second fx_gen tick. Resolve once on the edge and
    // you latch the transient root's Primitive, it is freed underneath you, and
    // every velocity write after that lands in freed memory and does nothing --
    // silently, with fly still "on" and gravity still held at zero. It is
    // intermittent because it only bites when the resolve lands inside that
    // window: the other two deaths had root=0 at the tick, waited, and got the
    // real one.
    //
    // fx_gen cannot cover this. It ticks for the respawn, once, before the rig
    // has settled; nothing ticks for the swap that follows.
    if (fx_gen() != gen || !prim || now_ms() >= prim_next) {
        bool edge = fx_gen() != gen;
        gen       = fx_gen();
        prim_next = now_ms() + PRIM_RECHECK_MS;

        uint64_t found = 0;
        uint64_t chr = fx_character();
        if (chr) {
            uint64_t root = fx_part(chr, "HumanoidRootPart");
            if (!root) root = fx_part(chr, "Torso");
            if (root) {
                uint64_t p = fx_rd(root + oPrim);
                if (p >= 0x10000 && p <= 0x7ff000000000ULL) found = p;
            }
        }
        // A 0 is only believed on the generation edge, where it genuinely means
        // "dead or still loading". On the periodic re-check it means the walk
        // lost a race, and dropping a working Primitive for that would put a
        // stutter into every 200ms of flight.
        if (found || edge) {
            if (found && prim && found != prim)
                fx_logf("  primitive moved %llx -> %llx (rig rebuilt)",
                        (unsigned long long)prim, (unsigned long long)found);
            prim = found;
        }
    }

    if (fx_key_pressed(TOGGLE_KEY) && fx_input_ok()) {
        if (!prim) {
            fx_log("  no character - are you spawned in?");
        } else if (!flying) {
            if (!find_gravity()) {
                fx_logf("x cannot find the world's gravity: %s", grav_why.c_str());
            } else {
                set_gravity(0.0f);
                flying = true;
                fx_logf("  fly on  (gravity %.2f -> 0 at 0x%llx)",
                        grav_value, (unsigned long long)grav_addr);
            }
        } else {
            set_gravity(grav_value);
            float3 zero{ 0, 0, 0 };
            fx_wrblk(prim + oVel, &zero, 12);
            flying = false;
            fx_logf("  fly off  (gravity restored to %.1f)", grav_value);
        }
    }

    if (!flying) { if (!fx_wait_key(60)) break; continue; }

    // DEAD, or respawning. Fly stays ON: gravity is still ours to hold at zero
    // (it belongs to the world, not to your body), and the moment the new
    // character resolves above, flight resumes with nothing to press. Only the
    // velocity write has to wait, since there is no body to write it to.
    if (!prim) {
        set_gravity(0.0f);
        if (!fx_sleep(50)) break;
        continue;
    }

    // Camera basis. The rotation is stored row-major, so the COLUMNS are the
    // axes: column 0 is right, and the look vector is column 2 NEGATED.
    if (!ws)  ws  = fx_service("Workspace");
    if (!cam) cam = ws ? fx_rd(ws + oCam) : 0;
    float R[9] = {0};
    if (!cam || fx_rdblk(cam + oRot, R, 36) != 36) {
        ws = cam = 0;               // stale: re-resolve next tick
        if (!fx_sleep(SLEEP_MS)) break;
        continue;
    }
    float fwdx = -R[2], fwdy = -R[5], fwdz = -R[8];
    float rgtx =  R[0], rgty =  R[3], rgtz =  R[6];

    // Gated on fx_input_ok so typing in chat does not fly you into a wall.
    float mx = 0, my = 0, mz = 0;
    if (fx_input_ok()) {
        if (fx_key('W')) { mx += fwdx; my += fwdy; mz += fwdz; }
        if (fx_key('S')) { mx -= fwdx; my -= fwdy; mz -= fwdz; }
        if (fx_key('D')) { mx += rgtx; my += rgty; mz += rgtz; }
        if (fx_key('A')) { mx -= rgtx; my -= rgty; mz -= rgtz; }
        if (fx_key(VK_SPACE)) my += 1.0f;      // world up, not camera up
        if (fx_key(VK_LCONTROL)) my -= 1.0f;
    }

    // Normalised so diagonals are not faster, then scaled to SPEED. No keys
    // held leaves this a zero vector, and with gravity off that is the hover.
    float len = sqrtf(mx * mx + my * my + mz * mz);
    float v[3] = { 0.0f, 0.0f, 0.0f };
    if (len > 1e-4f) {
        v[0] = mx / len * SPEED;
        v[1] = my / len * SPEED;
        v[2] = mz / len * SPEED;
    }
    fx_wrblk(prim + oVel, v, 12);

    // Re-asserted every tick, not set once: a place that writes its own gravity
    // would otherwise put it back mid-flight and drop you.
    set_gravity(0.0f);

    if (!fx_sleep(SLEEP_MS)) break;
}

// Stop, an error, a Ctrl+Break -- gravity goes back either way.
if (flying) {
    set_gravity(grav_value);
    fx_logf("gravity restored to %.1f on exit", grav_value);
}
fx_log("fly: stopped");
