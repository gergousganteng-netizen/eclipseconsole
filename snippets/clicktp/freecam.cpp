#include "ctp.h"
#include <chrono>
#include <cstdio>
#include <cstring>
// WIN32_LEAN_AND_MEAN: windows.h is the most expensive header this project
// includes -- measured 2026-09-07 on the bundled g++, +2222ms over an empty
// TU at -O2, against +520 for <fstream>, +365 for <atomic>, +275 for
// <thread> and nothing at all for <string>/<chrono>/<cstdio> (fx.h already
// pulls those in). LEAN drops the parts nothing here uses -- OLE, RPC,
// winsock, the multimedia and shell APIs -- and measured 319-491ms back per
// file. Delete it only if something here starts needing one of those.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vector>
#include <algorithm>
#include <map>

// FREECAM, BY MOVING WHAT THE CAMERA FOLLOWS.
//
// The obvious approach -- write Camera.Position every tick -- does not work. The
// camera module rewrites it every frame and wins: measured 4 of 187 passes held,
// and setting CameraType to Scriptable did not change that (7 of 187). Same
// shape as the airborne teleport; the engine's own state reasserts and ours is
// only ever visible between frames.
//
// So do not fight it. The module follows Camera.CameraSubject, so point that at
// a part and fly the PART. Measured: retargeting moved the view 204 studs, and a
// static world part held 187 of 187 passes. Everything the module does for a
// living -- mouse look, smoothing, FOV, collision softening -- keeps working,
// which is the half that would have been miserable to reimplement.
//
// The cost is that it BORROWS a real part out of the map. It is hidden with
// Transparency while flying and put back on exit, but that is why the exit path
// is defensive: leaving early with a part parked in the sky is the one way this
// can leave a mess. freecam_stop() is called on toggle, on respawn, on a place
// change and at snippet shutdown.

namespace {

struct Cam {
    bool     on = false;
    uint64_t cam = 0;
    uint64_t part = 0, prim = 0;      // the borrowed part
    uint64_t subj0 = 0;               // what the camera followed before
    float3   part_home{};             // where the part belongs
    float    transp0 = 0;
    float    walk0 = -1;              // character walkspeed to restore
    int      rot0  = -1;              // AutoRotate to restore
    uint64_t hum0 = 0;
    uint64_t plr  = 0;                // local Player, for the camera mode
    float    zmin0 = -1, zmax0 = -1;  // zoom distances to restore
    float3   pos{};                   // where we are flying
    double   last_ms = 0;
};

Cam c;

// KEYS ARE NOT CLAIMED WHILE FLYING, and this was tried and reverted.
//
// fx_claim_key swallows a key with a low-level keyboard hook. Two things went
// wrong at once. Our own reads broke -- fx_key was GetAsyncKeyState, and a key
// the hook eats never reaches the async state, so claiming WASD stopped the
// camera moving and claiming Shift made the Shift+P that exits impossible to
// press. That half is fixed in the host now, so fx_key reads the hook's state
// for claimed keys.
//
// The other half is not fixable from here: Roblox still got the keystrokes.
// Shiftlock kept toggling with Shift claimed, so the client reads the keyboard
// by a route a WH_KEYBOARD_LL hook does not sit on. Swallowing therefore costs
// us our own input and buys nothing against this client.
//
// What actually stops WASD walking your body is freeze_walk zeroing Walkspeed,
// which works because it is a memory write rather than an input trick. Shiftlock
// firing while you fly is a cosmetic annoyance with no fix on this side of the
// boundary.
void claim_keys(int) {}

// Turn the wheel for real. Clamping Min/MaxZoomDistance only bites when the
// camera module next RECOMPUTES the zoom, and it does that on scroll input --
// which is why setting them changed nothing until you happened to scroll.
// Nudging the wheel ourselves is what makes the clamp apply immediately.
//
// Player.CameraMode = LockFirstPerson was tried instead and is INERT: a raw
// write does not fire the property-changed path the module listens on, so the
// cursor never locked and scrolling still worked. Same reason Camera.CameraType
// did nothing for the earlier attempt. Do not re-add it.
//
// Synthetic input rather than a memory write, which is the same category as the
// cursor aim moving the mouse -- and gated on the client being focused, so it
// cannot go and scroll whatever else you have in front.
void wheel(int notches) {
    if (!fx_focused()) return;
    int n = notches < 0 ? -notches : notches;
    for (int i = 0; i < n && fx_running(); i++) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_WHEEL;
        in.mi.mouseData = (DWORD)(notches > 0 ? WHEEL_DELTA : -WHEEL_DELTA);
        SendInput(1, &in, sizeof in);
        fx_sleep(4);
    }
}

double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// Players.LocalPlayer. The conf spells this Player.LocalPlayer, but it is read
// off the Players SERVICE -- confirmed by a pointer scan that found our own
// Player object at Players+0x130.
uint64_t local_player() {
    // fx_player() is this exact walk, done by the host and cached: it reads
    // Player.LocalPlayer off the Players service and re-resolves only when the
    // generation moves. The pointer check stays -- the host caches whatever it
    // read, and this function's contract is "a usable pointer or 0".
    uint64_t me = fx_player();
    return fx_ptr_ok(me) ? me : 0;
}

// Resolved live rather than trusted from Ptrs. refresh_ptrs runs ONCE when
// fx_gen moves, and a character that is not fully built at that instant leaves
// p.humanoid at 0 for the rest of that life -- which is why walking stayed
// enabled after a respawn.
uint64_t cur_humanoid() {
    uint64_t chr = fx_character();
    return chr ? fx_part(chr, "Humanoid") : 0;
}

// Re-applied EVERY TICK, not once on entry. Two independent reasons: a game
// commonly sets WalkSpeed itself a moment after you spawn, which would undo a
// one-shot write, and the humanoid may not have existed when we entered.
// Originals are captured the first time we actually see the field, so exit
// still restores what the game had rather than a zero we wrote.
void enforce_freeze(Cam& s) {
    if (!g.freeze_walk) return;
    if (!s.hum0) s.hum0 = cur_humanoid();
    if (!s.hum0) return;

    uint64_t oWs = fx_off("Humanoid.Walkspeed");
    uint64_t oAr = fx_off("Humanoid.AutoRotate");
    if (oWs != (uint64_t)-1) {
        float now = fx_rf(s.hum0 + oWs);
        if (s.walk0 < 0 && now > 0) s.walk0 = now;    // first real value seen
        if (now != 0.0f) { float z = 0.0f; fx_wrblk(s.hum0 + oWs, &z, 4); }
    }
    if (oAr != (uint64_t)-1) {
        int now = fx_rd8(s.hum0 + oAr);
        if (s.rot0 < 0) s.rot0 = now;
        if (now) { uint8_t off = 0; fx_wrblk(s.hum0 + oAr, &off, 1); }
    }
}

uint64_t find_cam() {
    uint64_t ws = fx_service("Workspace");
    if (!ws) return 0;
    uint64_t n = fx_nchildren(ws);
    if (n > 4096) n = 4096;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t k = fx_child(ws, (int)i);
        if (k && strcmp(fx_class(k), "Camera") == 0) return k;
    }
    return 0;
}

// The camera's basis. Columns are right/up/back, so forward is -column2 -- the
// same layout camera_ray and lockon rely on.
bool cam_basis(uint64_t cam, float3* right, float3* up, float3* fwd) {
    uint64_t oRot = fx_off("Camera.Rotation");
    if (oRot == (uint64_t)-1) return false;
    float R[9];
    if (fx_rdblk(cam + oRot, R, 36) != 36) return false;
    *right = float3{  R[0],  R[3],  R[6] };
    *up    = float3{  R[1],  R[4],  R[7] };
    *fwd   = float3{ -R[2], -R[5], -R[8] };
    return true;
}

// A part worth borrowing.
//
// THE FIRST VERSION KILLED PEOPLE. It cast a ring of downward rays and kept the
// smallest thing they hit -- but when a map's floor is one big part, every ray
// hits that same floor and it is the only candidate. Borrowing it took the
// ground out from under the player, who then fell to their death. The gate test
// picked "Baseplate" and that should have been the warning.
//
// So: rays are gone. Walk Workspace for parts that are SMALL and FAR, which a
// floor can be neither of, and then PROVE the choice by test-moving it before
// committing -- a part welded into somebody's character gets dragged straight
// back, and that would leave the camera stuck to something that will not fly.
static const float BORROW_MAX_EXTENT = 8.0f;    // a floor is never this small
static const float BORROW_MIN_DIST   = 120.0f;  // and never this far from you

// Does this part stay where it is put? Static scenery does; a limb welded to a
// character does not. Measured 187/187 versus 1/187 when this was first tried.
bool holds_still(uint64_t prim, float3 home) {
    uint64_t oPos = fx_off("Primitive.Position");
    uint64_t oVel = fx_off("Primitive.AssemblyLinearVelocity");
    if (oPos == (uint64_t)-1) return false;
    float3 test{ home.x, home.y + 4.0f, home.z }, zero{ 0, 0, 0 };
    for (int i = 0; i < 4; i++) {
        if (oVel != (uint64_t)-1) fx_wrblk(prim + oVel, &zero, 12);
        fx_wrblk(prim + oPos, &test, 12);
        if (!fx_sleep(8)) break;
    }
    float3 got{};
    bool ok = fx_rdblk(prim + oPos, &got, 12) == 12 && v_len(v_sub(got, test)) < 1.5f;
    // Put it back either way before anyone notices.
    for (int i = 0; i < 4; i++) {
        if (oVel != (uint64_t)-1) fx_wrblk(prim + oVel, &zero, 12);
        fx_wrblk(prim + oPos, &home, 12);
        if (!fx_sleep(4)) break;
    }
    return ok;
}

// Is this part inside somebody's character?
//
// Walks UP the ancestors, not just the immediate parent. Checking one level
// missed a head -- a rig that nests its parts one Model deeper than expected
// slips straight through, and the result was borrowing another player's head.
//
// Memoised per ancestor, because only the handful of parents that produced a
// candidate ever get asked; doing this for all ~18000 instances would cost a
// class read each.
bool in_character(uint64_t part, std::map<uint64_t, bool>& memo) {
    uint64_t oPar = fx_off("Instance.Parent");
    if (oPar == (uint64_t)-1) return false;

    uint64_t up = part;
    for (int level = 0; level < 6; level++) {
        uint64_t par = fx_rd(up + oPar);
        if (!fx_ptr_ok(par)) return false;

        auto it = memo.find(par);
        if (it != memo.end()) { if (it->second) return true; }
        else {
            bool is_char = false;
            uint64_t n = fx_nchildren(par);
            if (n > 512) n = 512;
            for (uint64_t i = 0; i < n; i++) {
                uint64_t k = fx_child(par, (int)i);
                if (k && strcmp(fx_class(k), "Humanoid") == 0) { is_char = true; break; }
            }
            memo[par] = is_char;
            if (is_char) return true;
        }
        up = par;
    }
    return false;
}

// Does it sit still on its own? Scenery does. A player, a projectile, a moving
// platform does not -- and none of those should be borrowed regardless of what
// their ancestors look like. This catches by BEHAVIOUR what the class check
// catches by structure, which is why both are used: the head that got through
// was moving the whole time.
bool is_static(uint64_t part) {
    float3 a{}, b2{};
    if (!fx_pos(part, &a)) return false;
    if (!fx_sleep(90)) return false;
    if (!fx_pos(part, &b2)) return false;
    return v_len(v_sub(a, b2)) < 0.05f;
}

uint64_t pick_part(float3 from, float3* home_out, uint64_t* prim_out) {
    uint64_t oPrim = fx_off("BasePart.Primitive"), oSize = fx_off("Primitive.Size");
    uint64_t ws = fx_service("Workspace");
    if (!ws || oPrim == (uint64_t)-1 || oSize == (uint64_t)-1) return 0;

    struct Cand { uint64_t part, prim; float3 home; float vol; };
    std::vector<Cand> cands;

    std::vector<uint64_t> cur, next;
    cur.push_back(ws);
    int seen = 0;
    for (int depth = 0; depth < 10 && !cur.empty() && seen < 40000 && fx_running(); depth++) {
        next.clear();
        for (uint64_t par : cur) {
            uint64_t n = fx_nchildren(par);
            if (n > 2048) n = 2048;
            for (uint64_t i = 0; i < n && seen < 40000; i++) {
                uint64_t k = fx_child(par, (int)i);
                if (!k) continue;
                seen++;
                next.push_back(k);

                float3 pp{};
                if (!fx_pos(k, &pp)) continue;              // class-checked: not a part
                if (v_len(v_sub(pp, from)) < BORROW_MIN_DIST) continue;

                uint64_t prim = fx_rd(k + oPrim);
                if (prim < 0x10000 || prim > 0x7ff000000000ULL) continue;
                float3 sz{};
                if (fx_rdblk(prim + oSize, &sz, 12) != 12) continue;
                if (!(sz.x > 0.05f && sz.y > 0.05f && sz.z > 0.05f)) continue;
                float m = sz.x; if (sz.y > m) m = sz.y; if (sz.z > m) m = sz.z;
                if (m > BORROW_MAX_EXTENT) continue;        // floors and walls out

                cands.push_back(Cand{ k, prim, pp, sz.x * sz.y * sz.z });
            }
        }
        cur.swap(next);
        if (cands.size() >= 600) break;
    }

    // SMALLEST FIRST WAS A TRAP. The smallest parts in any Roblox map are
    // character limbs -- a hand is 0.7 studs -- so sorting by volume handed the
    // hold test twelve pieces of one player's body, all of them welded, all
    // dragged back, while 5000 usable props went unexamined. Characters are
    // excluded outright now, and among what is left FARTHEST wins: the further
    // the thing we steal, the less chance anyone is standing on it.
    std::map<uint64_t, bool> memo;
    std::vector<Cand> clean;
    for (const Cand& c2 : cands) {
        if (clean.size() >= 40) break;
        if (in_character(c2.part, memo)) continue;
        clean.push_back(c2);
    }
    std::sort(clean.begin(), clean.end(),
              [&](const Cand& a, const Cand& b) {
                  return v_len(v_sub(a.home, from)) > v_len(v_sub(b.home, from));
              });

    // Three gates, cheapest first, and only the survivors pay for the next one.
    // is_static costs 90ms and holds_still about 50, so this tries a bounded
    // number rather than the whole shortlist.
    int tried = 0;
    for (const Cand& c2 : clean) {
        if (!fx_running() || tried >= 8) break;
        tried++;
        if (!is_static(c2.part)) continue;                  // a player, or moving
        float3 home{};
        if (!fx_pos(c2.part, &home)) continue;              // re-read: it waited 90ms
        if (!holds_still(c2.prim, home)) continue;          // welded to something
        *home_out = home;
        *prim_out = c2.prim;
        return c2.part;
    }
    return 0;
}

}  // namespace

bool freecam_on(void) { return c.on; }

void freecam_stop(const Ptrs& p) {
    if (!c.on) return;
    claim_keys(0);          // hand the keyboard back before anything else
    uint64_t oSubj = fx_off("Camera.CameraSubject");
    uint64_t oPos  = fx_off("Primitive.Position");
    uint64_t oVel  = fx_off("Primitive.AssemblyLinearVelocity");
    uint64_t oTr   = fx_off("BasePart.Transparency");
    uint64_t oWs   = fx_off("Humanoid.Walkspeed");

    // Subject first: put the view back on the player before the part moves, so
    // the borrowed part is never on screen while it flies home.
    if (c.cam && oSubj != (uint64_t)-1 && c.subj0) fx_wrblk(c.cam + oSubj, &c.subj0, 8);

    if (c.prim && oPos != (uint64_t)-1) {
        float3 zero{ 0, 0, 0 };
        for (int i = 0; i < 12; i++) {          // a few passes; one can be lost
            if (oVel != (uint64_t)-1) fx_wrblk(c.prim + oVel, &zero, 12);
            fx_wrblk(c.prim + oPos, &c.part_home, 12);
            if (!fx_sleep(8)) break;
        }
    }
    if (c.part && oTr != (uint64_t)-1) fx_wrblk(c.part + oTr, &c.transp0, 4);
    if (c.hum0 && oWs != (uint64_t)-1 && c.walk0 >= 0) fx_wrblk(c.hum0 + oWs, &c.walk0, 4);
    {
        uint64_t oAr = fx_off("Humanoid.AutoRotate");
        if (c.hum0 && c.rot0 >= 0 && oAr != (uint64_t)-1) {
            uint8_t v = (uint8_t)c.rot0;
            fx_wrblk(c.hum0 + oAr, &v, 1);
        }
    }

    // Zoom back out. Restored even if it was never clamped, because zmin0 stays
    // negative in that case and the guard below skips it.
    {
        uint64_t oMin = fx_off("Player.MinZoomDistance");
        uint64_t oMax = fx_off("Player.MaxZoomDistance");
        if (c.plr && c.zmin0 >= 0 && oMin != (uint64_t)-1) fx_wrblk(c.plr + oMin, &c.zmin0, 4);
        if (c.plr && c.zmax0 >= 0 && oMax != (uint64_t)-1) fx_wrblk(c.plr + oMax, &c.zmax0, 4);
        // Distances back FIRST, then scroll out -- the other order would ask the
        // module to zoom out while the ceiling is still zero, which is the trap
        // that left you stuck in first person until you exited freecam.
        if (c.plr && c.zmax0 > 0) wheel(12);
    }

    c = Cam{};
}

std::string freecam_toggle(const Ptrs& p) {
    if (c.on) { freecam_stop(p); return "freecam off"; }
    if (!fx_armed()) return "freecam needs the console ARMED";

    Cam n;
    n.cam = find_cam();
    if (!n.cam) return "no camera";

    uint64_t oSubj = fx_off("Camera.CameraSubject");
    uint64_t oCpos = fx_off("Camera.Position");
    uint64_t oPrim = fx_off("BasePart.Primitive");
    uint64_t oTr   = fx_off("BasePart.Transparency");
    uint64_t oWs   = fx_off("Humanoid.Walkspeed");
    if (oSubj == (uint64_t)-1 || oCpos == (uint64_t)-1) return "missing camera offsets";

    if (fx_rdblk(n.cam + oCpos, &n.pos, 12) != 12) return "cannot read the camera";
    n.subj0 = fx_rd(n.cam + oSubj);

    n.part = pick_part(n.pos, &n.part_home, &n.prim);
    if (!n.part) return "no small, distant, movable part to borrow here";
    (void)oPrim;

    // Hide it, take it, and seed it where the camera already is so entering
    // freecam does not jump the view.
    if (oTr != (uint64_t)-1) {
        n.transp0 = fx_rf(n.part + oTr);
        float one = 1.0f;
        fx_wrblk(n.part + oTr, &one, 4);
    }
    fx_wrblk(n.cam + oSubj, &n.part, 8);

    // FIRST PERSON, so the camera is AT the flying point rather than orbiting
    // it. In third person the module keeps the camera on a sphere around its
    // subject, so moving the subject swings the view around a point instead of
    // flying it.
    //
    // ALL OF THIS IS SKIPPED when first_person is off, and that is the point of
    // the guard. Clamping the zoom while not intending first person was strictly
    // harmful: nothing changed on entry, and then the first accidental scroll
    // collapsed into [0,0] and stranded the view there until you left freecam.
    // A knob set to "no" should leave the thing alone entirely, not arm a trap.
    //
    // Two steps, and both are needed. Clamping Min/Max to zero only BOUNDS the
    // zoom, applied whenever the module next recomputes it -- which it does on
    // scroll input, hence the wheel nudge to make it happen now instead of
    // whenever you happen to touch the wheel. Player.CameraMode =
    // LockFirstPerson was tried in place of this and is inert; see wheel().
    n.plr = local_player();
    if (g.first_person) {
        uint64_t oMin = fx_off("Player.MinZoomDistance");
        uint64_t oMax = fx_off("Player.MaxZoomDistance");
        if (n.plr && oMin != (uint64_t)-1 && oMax != (uint64_t)-1) {
            n.zmin0 = fx_rf(n.plr + oMin);
            n.zmax0 = fx_rf(n.plr + oMax);
            float z = 0.0f;
            fx_wrblk(n.plr + oMin, &z, 4);
            fx_wrblk(n.plr + oMax, &z, 4);
            wheel(-12);                          // scroll in, applying the clamp
        }
    }

    // Stop the humanoid driving the body while we fly. Two separate things:
    //
    //   Walkspeed   so WASD does not also march you around.
    //   AutoRotate  so SHIFTLOCK does not spin you. In shiftlock the humanoid
    //               turns the character to face wherever the camera looks, and
    //               in freecam that target is a camera somewhere else entirely
    //               and moving -- so the rotation controller churns every frame,
    //               keeps the assembly awake, and fights a teleport write the
    //               same way the physics solver does mid-fall. Confirmed: it is
    //               what made teleporting inconsistent in freecam + shiftlock.
    //
    // Applied here AND on every tick, because one write does not survive a game
    // that sets WalkSpeed itself after a respawn.
    n.hum0 = p.humanoid ? p.humanoid : cur_humanoid();
    enforce_freeze(n);

    claim_keys(1);
    n.on = true;
    n.last_ms = now_ms();
    c = n;

    char b[160];
    snprintf(b, sizeof b, "freecam ON -- borrowed '%s'. WASD, Q/E, Shift fast, Ctrl slow",
             fx_name(c.part));
    return b;
}

void freecam_tick(const Ptrs& p) {
    if (!c.on) return;

    double t = now_ms();
    float dt = (float)((t - c.last_ms) / 1000.0);
    c.last_ms = t;
    // A stall -- a long compile, a hitch -- must not fling the camera across the
    // map on the next tick.
    if (dt > 0.1f) dt = 0.1f;
    if (dt <= 0.0f) return;

    float3 right{}, up{}, fwd{};
    if (!cam_basis(c.cam, &right, &up, &fwd)) return;

    // Read input only when the keystrokes are actually meant for the game.
    // Alt-tabbed, or typing in chat, the camera HOLDS -- it does not drift, and
    // it does not fly off because a message happened to contain a W. The
    // position write below still runs, which is what keeps it parked.
    if (fx_input_ok()) {
        // Movement is relative to where you are LOOKING, which is what makes
        // this feel like a camera rather than a vehicle.
        float3 d{ 0, 0, 0 };
        if (fx_key('W')) d = v_add(d, fwd);
        if (fx_key('S')) d = v_sub(d, fwd);
        if (fx_key('D')) d = v_add(d, right);
        if (fx_key('A')) d = v_sub(d, right);
        if (fx_key('E')) d = v_add(d, float3{ 0, 1, 0 });
        if (fx_key('Q')) d = v_sub(d, float3{ 0, 1, 0 });

        float speed = g.fly_speed;
        if (fx_key(VK_SHIFT))   speed *= g.fly_boost;
        if (fx_key(VK_CONTROL)) speed *= g.fly_slow;

        if (v_len(d) > 0.001f) c.pos = v_add(c.pos, v_mul(v_norm(d), speed * dt));
    }

    enforce_freeze(c);

    // Hold the part there. Velocity zeroed first for the same reason the
    // teleport does it: otherwise one physics step carries it off the spot.
    uint64_t oPos = fx_off("Primitive.Position");
    uint64_t oVel = fx_off("Primitive.AssemblyLinearVelocity");
    if (oPos == (uint64_t)-1) return;
    float3 zero{ 0, 0, 0 };
    if (oVel != (uint64_t)-1) fx_wrblk(c.prim + oVel, &zero, 12);
    fx_wrblk(c.prim + oPos, &c.pos, 12);
}
