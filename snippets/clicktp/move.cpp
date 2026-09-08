#include "ctp.h"
#include <chrono>
#include <cstdio>
#include <vector>
#include <cstring>

static double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// MOVE THE WHOLE BODY, not just the root, and this is measured rather than
// assumed.
//
// Writing only HumanoidRootPart leaves every other part where it was, still
// welded to it, and the solver spends the whole hold dragging the root back onto
// the limbs. Measured against the live client, root-to-head distance during the
// hold (normally 1.2 studs):
//
//   standing still, 12-stud move   root-only 10.8    whole-body 1.2
//   WALKING, 60-stud move          root-only 60.0    whole-body 1.3
//
// Walking is what makes it fail: 60.0 means the root made the entire trip and
// the body did not follow at all. Of six trials while moving, the only
// SNAPPED BACK was root-only; whole-body stuck 3 for 3. Standing still, both
// variants stuck at every distance out to 400 studs -- which is why this was
// invisible until it was tested under movement.
//
// Offsets are recomputed from CURRENT positions every pass rather than snapshot
// once, so an animation playing during the hold keeps animating instead of being
// frozen into the pose it had when you clicked.
//
// Velocity is zeroed BEFORE the position. The other order leaves one physics
// step where you are at the new spot carrying the old speed, which is enough to
// be flung straight back off it.
struct Piece { uint64_t prim; };

// The camera. Roblox re-derives it from its subject every frame, so this only
// has to cover the gap DURING a move -- but that gap is very visible with
// step_studs small, because the walk lasts half a second.
//
// Measured, resting camera-to-root 82 studs: left alone it stretched to 149,
// 133, 110, 109 across trials and snapped back at the end. Carried, it stayed at
// 82, 84, 103, 100, 107. It does not change whether the move sticks -- 3 of 5
// either way -- it only stops the camera being dragged behind.
static uint64_t find_camera() {
    uint64_t ws = fx_service("Workspace");
    if (!ws) return 0;
    uint64_t n = fx_nchildren(ws);
    if (n > 4096) n = 4096;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t c = fx_child(ws, (int)i);
        if (c && strcmp(fx_class(c), "Camera") == 0) return c;
    }
    return 0;
}

static void place(const std::vector<Piece>& body, uint64_t root_prim,
                  uint64_t oPos, uint64_t oVel, uint64_t cam, uint64_t oCam,
                  float3 at) {
    float3 root{};
    if (fx_rdblk(root_prim + oPos, &root, 12) != 12) return;
    float3 d = v_sub(at, root);
    float3 zero{ 0, 0, 0 };

    for (const Piece& pc : body) {
        float3 cur{};
        if (fx_rdblk(pc.prim + oPos, &cur, 12) != 12) continue;
        float3 to = v_add(cur, d);
        if (oVel != (uint64_t)-1) fx_wrblk(pc.prim + oVel, &zero, 12);
        fx_wrblk(pc.prim + oPos, &to, 12);
    }

    if (cam && oCam != (uint64_t)-1) {
        float3 c{};
        if (fx_rdblk(cam + oCam, &c, 12) == 12) {
            float3 nc = v_add(c, d);
            fx_wrblk(cam + oCam, &nc, 12);
        }
    }
}

// The character's primitives, root first. Rebuilt per teleport because a
// respawn or an accessory change replaces them.
static void collect(const Ptrs& p, std::vector<Piece>* out, uint64_t* root_prim) {
    out->clear();
    *root_prim = 0;
    uint64_t oPrim = fx_off("BasePart.Primitive");
    if (oPrim == (uint64_t)-1) return;

    uint64_t parts[64];
    int n = fx_parts(parts, 64);
    bool have_root = false;
    for (int i = 0; i < n; i++) {
        uint64_t pr = fx_rd(parts[i] + oPrim);
        if (pr < 0x10000 || pr > 0x7ff000000000ULL) continue;
        out->push_back(Piece{ pr });
        if (parts[i] == p.hrp) have_root = true;
    }
    *root_prim = p.primitive;
    // fx_parts should list the root, but the delta is measured from it, so a
    // rig that names it something unexpected must not silently leave it behind.
    if (!have_root && p.primitive) out->push_back(Piece{ p.primitive });
}

// Move there and say what happened.
//
// THE HOLD is the part that makes this stick. A single write lands between two
// physics steps and the next one puts you back, which is the whole "it teleports
// me and then teleports me back" complaint. Rewriting the destination for a
// short window outlasts that step. It is not a loop that fights the server
// forever -- it ends, and then the position is READ BACK so the report can say
// whether it actually took.
//
// STEPPING exists for the other failure: a single large jump is what a server
// notices and reverts. Covering the distance in hops keeps every individual move
// small. g.step_studs == 0 turns it off.
bool teleport(const Ptrs& p, float3 dest, std::string* report) {
    if (!p.ok) { *report = "no character"; return false; }

    uint64_t oPos = fx_off("Primitive.Position");
    uint64_t oVel = fx_off("Primitive.AssemblyLinearVelocity");
    if (oPos == (uint64_t)-1) { *report = "no Primitive.Position offset"; return false; }

    float3 from{};
    if (!fx_pos(p.hrp, &from)) { *report = "cannot read current position"; return false; }
    float total = v_len(v_sub(dest, from));

    // Not while flying: the camera is following the borrowed part, and nudging
    // Camera.Position would only fight the module for no visible gain.
    uint64_t cam = (g.carry_camera && !freecam_on()) ? find_camera() : 0;
    uint64_t oCam = fx_off("Camera.Position");

    std::vector<Piece> body;
    uint64_t root_prim = 0;
    collect(p, &body, &root_prim);
    if (body.empty() || !root_prim) { *report = "cannot enumerate the body"; return false; }

    // Disarmed: ONE root write, so the dry-run log is a single line rather than
    // one per part per pass. The report says what an armed run would really do.
    if (!fx_armed()) {
        float3 zero{ 0, 0, 0 };
        if (oVel != (uint64_t)-1) fx_wrblk(root_prim + oVel, &zero, 12);
        fx_wrblk(root_prim + oPos, &dest, 12);
        char b[200];
        snprintf(b, sizeof b, "DISARMED -- would move %d parts %.0f studs to (%.0f, %.0f, %.0f)",
                 (int)body.size(), total, dest.x, dest.y, dest.z);
        *report = b;
        return false;
    }

    int hops = 1;
    if (g.step_studs > 0.5f && total > g.step_studs)
        hops = (int)(total / g.step_studs) + 1;
    if (hops > 64) hops = 64;               // bound it; a click is not a journey

    for (int i = 1; i <= hops && fx_running(); i++) {
        float3 at = v_add(from, v_mul(v_sub(dest, from), (float)i / (float)hops));
        place(body, root_prim, oPos, oVel, cam, oCam, at);
        if (i < hops && !fx_sleep(8)) return false;
    }

    // Hold the last position for a window, then stop and look.
    //
    // hold_hz = 0 SPINS on fx_sleep(0) rather than pacing, and that is now the
    // default because it measured strictly better. Writing Primitive.Position
    // 25 studs up and re-asserting it on fx_sleep(0) put the character exactly
    // there on the FIRST write and held it dead-on for 1.5s across 434,252
    // writes, with zero drift. A paced hold leaves a gap between writes for the
    // solver to step into; a spin does not, because the engine cannot step
    // faster than we rewrite. The dial is kept for when CPU matters more than
    // grip.
    double t_end = now_ms() + (double)g.hold_ms;
    unsigned period = g.hold_hz > 0 ? (unsigned)(1000 / g.hold_hz) : 0;
    if (g.hold_hz > 0 && period < 1) period = 1;
    while (fx_running() && now_ms() < t_end) {
        place(body, root_prim, oPos, oVel, cam, oCam, dest);
        if (!fx_sleep(period)) break;
    }

    // Settle, then read it back. Without the pause this measures the value we
    // just wrote rather than the one the game kept, which would make the check
    // impossible to fail.
    if (!fx_sleep(120)) return false;

    // AIRBORNE IS A KNOWN DEAD END. There is no check for it here on purpose --
    // this is used from the ground, and a branch nobody reaches is worse than a
    // comment. The findings stay because the alternative is measuring them twice.
    //
    // While the assembly is in the air the solver keeps its own copy of the
    // transform, integrates the original fall from it, and overwrites
    // Primitive.Position every step. Measured: the write LANDS -- reading the
    // root straight back gives exactly the destination, 29 times out of 29 --
    // and the moment the writes stop you are back on the old trajectory, having
    // fallen the whole time. Grounded, the same write is authoritative, which is
    // why walking works and falling does not.
    //
    // Ruled out, all measured, all identical failures:
    //   Humanoid.EvaluateStateMachine = 0    no effect at all
    //   Humanoid.PlatformStand = 1           holds it, but only while held;
    //                                        releasing restores the fall
    //   holding until FloorMaterial reports ground, then releasing
    //   keeping PlatformStand on after the writes stop
    //   keeping the writes going after PlatformStand is released
    // Whatever owns the transform for an awake assembly is somewhere else. Do
    // not retry these; find that first.
    float3 landed{};
    if (!fx_pos(p.hrp, &landed)) { *report = "moved, but position unreadable"; return false; }
    float off = v_len(v_sub(landed, dest));
    float back = v_len(v_sub(landed, from));

    char b[220];
    if (off <= g.stick_tol) {
        snprintf(b, sizeof b, "there (%.0f studs, off by %.1f)", total, off);
        *report = b;
        return true;
    }
    if (back <= g.stick_tol * 2.0f) {
        // Body tearing is fixed, so on the GROUND this means the server rejected
        // the move rather than the solver undoing it. Stepping is the remedy for
        // that one. In mid-air it means the dead end above instead.
        snprintf(b, sizeof b,
                 "SNAPPED BACK -- the spot was fine, the server rejected the move "
                 "(set step_studs below %.0f)",
                 g.step_studs > 0.5f ? g.step_studs : total);
    } else {
        snprintf(b, sizeof b, "drifted %.0f studs off target (landed %.0f from start)",
                 off, back);
    }
    *report = b;
    return false;
}
