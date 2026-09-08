// Fly -- zero the world's gravity, then steer with velocity.
//
// Ported from snippets/movement/fly.cpp. WRITES World.Gravity and our own
// Primitive's AssemblyLinearVelocity, through fx_wrblk, so dry-run until armed.
//
// GRAVITY IS GLOBAL. The whole map floats while this is on, for everyone in the
// server, not just you. That is the trade and it is not subtle.
//
// Velocity alone was tried first and does not hold you up; zeroing gravity and
// then writing velocity does. With gravity off, writing a ZERO velocity IS
// holding still, so there is no altitude target and no gain to tune.
//
// THE RESTORE VALUE COMES FROM THE MIRROR, NOT THE LIVE FIELD.
// Workspace.ReadOnlyGravity is a read-only copy, so it still holds the true
// figure even when the live field has been left at 0 by a run that died without
// restoring. Reading the live field would capture that 0 as "the original" and
// write 0 back on exit -- and one bad exit then poisons every run after it.
// That has happened.

#include <cmath>
#include <string>

#include "hub.h"

const float SPEED = 90.0f;      // studs/second

namespace {

bool     g_on = false;
uint64_t g_grav_addr = 0;       // World + World.Gravity, THIS generation's
float    g_grav_value = 0.0f;   // what to put back
uint64_t g_ws = 0, g_cam = 0;
unsigned g_gen = 0;
bool     g_gen_init = false;
std::string g_status;

// EVERY POINTER HERE BELONGS TO ONE DATAMODEL. Workspace, the camera and the
// World are each resolved once and cached, and joining another game replaces
// all three -- but the old addresses are freed memory that usually still READS,
// so nothing fails loudly. The shell re-resolves its own rig on the fx_gen()
// edge and does not tell the tools, so each one watches the counter itself.
//
// This is exactly what "fly bugged after joining another game" was. Three
// separate faults from the one cause:
//   - gravity was still being zeroed at the OLD World's address, so the new
//     map kept its gravity and you fell;
//   - the camera was read off the old Workspace, so the steering basis was
//     whatever those bytes now hold -- W goes somewhere arbitrary;
//   - switching fly off wrote the previous map's gravity figure to that dead
//     address, which is an armed write to somewhere that was never right.
// Switching fly off and on again did not help, because find_gravity() keeps a
// non-zero g_ws rather than re-resolving it.
void check_gen() {
    unsigned g = fx_gen();
    if (g_gen_init && g == g_gen) return;
    g_gen_init = true;
    g_gen = g;
    g_ws = g_cam = 0;
    // DROPPED, NOT RESTORED. There is nothing to restore to: the address is
    // gone with the old DataModel and the value belonged to a map we have
    // left. Clearing it is also what makes set_gravity safe -- a non-zero
    // g_grav_addr now means "resolved in the generation we are in", so no
    // write below can reach a stale one.
    g_grav_addr = 0;
}

bool find_gravity() {
    g_grav_addr = 0;
    uint64_t oWorld = fx_off("Workspace.World");
    uint64_t oWG    = fx_off("World.Gravity");
    uint64_t oRO    = fx_off("Workspace.ReadOnlyGravity");
    if (oWorld == (uint64_t)-1 || oWG == (uint64_t)-1) {
        g_status = "conf has no World.Gravity";
        return false;
    }
    if (!g_ws) g_ws = fx_service("Workspace");
    if (!g_ws) { g_status = "no Workspace yet"; return false; }

    uint64_t world = fx_rd(g_ws + oWorld);
    if (world < 0x10000 || world > 0x7ff000000000ULL) {
        g_status = "Workspace.World reads wrong";
        return false;
    }
    g_grav_addr = world + oWG;

    float mirror = (oRO != (uint64_t)-1) ? fx_rf(g_ws + oRO) : 0.0f;
    float live   = fx_rf(g_grav_addr);
    g_grav_value = (mirror > 1.0f) ? mirror : live;
    if (mirror > 1.0f && fabsf(live - mirror) > 0.001f)
        fx_logf("  ! gravity reads %.2f, should be %.2f -- restoring to that",
                live, mirror);
    return true;
}

void set_gravity(float v) {
    if (g_grav_addr) fx_wrblk(g_grav_addr, &v, 4);
}

}  // namespace

// Hot while flying: the velocity write races the engine, which rewrites
// AssemblyLinearVelocity from its own solver state every step. Pacing loses.
bool fly_hot() { return g_on; }

void fly_stop() {
    if (!g_on) return;
    // check_gen first: if we hopped, g_grav_addr goes to 0 and set_gravity is a
    // no-op rather than a write to the old world. The map we zeroed is gone
    // anyway -- there is no longer anything there to put right.
    check_gen();
    set_gravity(g_grav_value);
    g_on = false;
    g_status = "";
}

void fly_msg(const std::string& msg, const Ctx& c) {
    if (msg != "fly") return;
    check_gen();
    if (g_on) {
        bool restored = g_grav_addr != 0;
        set_gravity(g_grav_value);
        if (c.prim) { float3 z{ 0, 0, 0 }; fx_wrblk(c.prim + c.oVel, &z, 12); }
        g_on = false;
        g_status = "";
        // Honest about which of the two happened. Claiming a restore that
        // set_gravity declined to make is the kind of log line that sends you
        // looking in the wrong place later.
        if (restored) fx_logf("  fly off (gravity restored to %.1f)", g_grav_value);
        else          fx_log("  fly off (different place now - nothing to restore)");
        return;
    }
    if (!c.prim) { g_status = "no character"; return; }
    if (!find_gravity()) return;
    set_gravity(0.0f);
    g_on = true;
    g_status = "WASD, space up, ctrl down";
    fx_logf("  fly on (gravity %.2f -> 0)", g_grav_value);
}

void fly_tick(const Ctx& c) {
    check_gen();
    if (!g_on) return;

    // A NEW PLACE, STILL FLYING. Re-resolve and re-zero rather than switching
    // off: the toggle is on, and it is meant to survive the things that replace
    // your character. Ahead of the c.prim check below because this needs only
    // Workspace -- which exists long before the map has loaded a character for
    // us, and that is most of the hop.
    //
    // Retried every tick while it fails, and it WILL fail for a while. No write
    // happens until it succeeds, so a slow load costs nothing but altitude.
    if (!g_grav_addr) {
        if (!find_gravity()) return;
        set_gravity(0.0f);
        g_status = "WASD, space up, ctrl down";
        fx_logf("  fly: new place, gravity %.2f -> 0", g_grav_value);
    }

    // Dead or respawning: hold the world at zero -- gravity belongs to the map,
    // not to your body -- and pick the flying up when the character returns.
    if (!c.prim) { set_gravity(0.0f); return; }

    uint64_t oCam = fx_off("Workspace.CurrentCamera");
    uint64_t oRot = fx_off("Camera.Rotation");
    if (oCam == (uint64_t)-1 || oRot == (uint64_t)-1) return;
    if (!g_ws)  g_ws  = fx_service("Workspace");
    if (!g_cam) g_cam = g_ws ? fx_rd(g_ws + oCam) : 0;

    // The rotation is stored row-major, so the COLUMNS are the axes: column 0
    // is right, and the look vector is column 2 NEGATED.
    float R[9] = { 0 };
    if (!g_cam || fx_rdblk(g_cam + oRot, R, 36) != 36) {
        g_ws = g_cam = 0;              // stale: re-resolve next tick
        return;
    }
    float fwdx = -R[2], fwdy = -R[5], fwdz = -R[8];
    float rgtx =  R[0], rgty =  R[3], rgtz =  R[6];

    // Gated on fx_input_ok, so typing in chat does not fly you into a wall.
    float mx = 0, my = 0, mz = 0;
    if (fx_input_ok()) {
        if (fx_key('W')) { mx += fwdx; my += fwdy; mz += fwdz; }
        if (fx_key('S')) { mx -= fwdx; my -= fwdy; mz -= fwdz; }
        if (fx_key('D')) { mx += rgtx; my += rgty; mz += rgtz; }
        if (fx_key('A')) { mx -= rgtx; my -= rgty; mz -= rgtz; }
        if (fx_key(VK_SPACE))    my += 1.0f;    // world up, not camera up
        if (fx_key(VK_LCONTROL)) my -= 1.0f;
    }

    // Normalised so diagonals are not faster. No keys held leaves a zero
    // vector, and with gravity off that is the hover.
    float len = sqrtf(mx*mx + my*my + mz*mz);
    float v[3] = { 0, 0, 0 };
    if (len > 1e-4f) {
        v[0] = mx / len * SPEED;
        v[1] = my / len * SPEED;
        v[2] = mz / len * SPEED;
    }
    fx_wrblk(c.prim + c.oVel, v, 12);
}

void fly_panel(std::string& out, const Ctx& c) {
    (void)c;
    out += "\"fly\":{\"on\":";
    out += (g_on ? "true" : "false");
    out += ",\"status\":\"";
    js_escape(g_status, out);
    out += "\"}";
}
