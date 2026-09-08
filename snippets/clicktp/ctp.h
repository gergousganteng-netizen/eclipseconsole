#pragma once
// clicktp -- point at a spot in the world, verify it, go there.
//
// The geometry index and the ray test are NOT here. They live in the host as
// fx_geom / fx_raycast / fx_visible, because a per-place cache of every part is
// exactly the kind of thing that ends up copied into five snippets that then
// disagree -- the same argument that moved projection into fx_screen. This file
// is only the parts that are specific to teleporting.
//
// WHAT IT WRITES  Two fields on the character root's Primitive and nothing else,
//                 ever: Position, and AssemblyLinearVelocity zeroed so the
//                 momentum you arrived with does not drag you straight off the
//                 spot. Both go through fx_wrblk, so both are dry-run until the
//                 console is armed.
//
// WHY THERE IS A VERIFY STEP
//
// A teleport that fails puts you back where you started, and there are two
// different reasons for that which need different answers:
//
//   the destination was never valid -- midair, or inside a wall. Answered
//     BEFORE the write, by casting at real geometry and checking your body fits.
//   the destination was fine, it just did not stick -- answered AFTER, by
//     holding the position briefly and reading it back.
//
// Only the first is fixed by clicking somewhere else, which is why the report
// tells you which one happened.

#include <cmath>
#include <cstdint>
#include <string>

// ---- small vector helpers ---------------------------------------------------
// float3 is fx.h's, so these interop with fx_screen / fx_raycast / fx_wf3.
static inline float3 v_add(float3 a, float3 b) { return float3{ a.x+b.x, a.y+b.y, a.z+b.z }; }
static inline float3 v_sub(float3 a, float3 b) { return float3{ a.x-b.x, a.y-b.y, a.z-b.z }; }
static inline float3 v_mul(float3 a, float s)  { return float3{ a.x*s, a.y*s, a.z*s }; }
static inline float  v_dot(float3 a, float3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline float  v_len(float3 a)           { return sqrtf(v_dot(a, a)); }
static inline float3 v_norm(float3 a) {
    float l = v_len(a);
    return l > 1e-6f ? v_mul(a, 1.0f / l) : float3{ 0, 0, 0 };
}

struct Ptrs {
    uint64_t character = 0, humanoid = 0, hrp = 0, primitive = 0;
    bool     ok = false;
};

// Where the body sits relative to the ROOT part. The root is at your waist, so
// putting it on a floor buries you to the hips -- foot_drop is what fixes that.
struct Body {
    float3 half{};        // half extents of the whole character
    float3 centre_off{};  // body centre minus root position
    float  foot_drop = 0; // root Y minus the lowest point
    bool   ok = false;
};

struct Cfg {
    int   trigger_vk;     // side mouse button
    int   back_vk;        // return to where you were -- press twice to come back
    int   rebuild_vk;     // force a fresh geometry index
    float max_range;      // studs; refuse a click further out than this
    float step_studs;     // 0 = one jump; >0 = cover it in hops this big
    int   hold_ms;        // keep writing the destination for this long
    int   hold_hz;        // how often during the hold; 0 = SPIN (fx_sleep(0)),
                          // which measured strictly better -- see move.cpp
    float clearance_pad;  // studs of slack above the surface
    float stick_tol;      // studs; further than this from dest == it did not stick
    bool  require_clear;  // refuse when the body would not fit
    bool  carry_camera;   // drag the camera along instead of letting it lag

    // ---- freecam ----
    int   freecam_vk;     // held with SHIFT to toggle, like Roblox's own
    float fly_speed;      // studs/second
    float fly_boost;      // multiplier while Shift is held
    float fly_slow;       // multiplier while Ctrl is held
    bool  freeze_walk;    // zero Walkspeed while flying so WASD does not also
                          // drive your body around
    bool  first_person;   // camera AT the flying point instead of orbiting it.
                          // Roblox hides your character in first person, so turn
                          // this off if you would rather watch your own body
};
extern Cfg g;

// freecam.cpp
bool freecam_on(void);
// Toggle. Returns a line to log either way.
std::string freecam_toggle(const Ptrs& p);
void freecam_tick(const Ptrs& p);
// Put everything back. Safe to call when not flying, and MUST be called before
// the snippet exits or the map keeps a hole in it.
void freecam_stop(const Ptrs& p);

// world.cpp
bool refresh_ptrs(Ptrs* out);
bool cursor_px(float* x, float* y);
// The ray under a client-area pixel. Self-calibrating and self-CHECKING; it
// does not trust Camera.FieldOfView. See the definition.
bool camera_ray(float sx, float sy, float3* origin, float3* dir, std::string* why);
bool measure_body(const Ptrs& p, Body* out);
// Is the body's box at dest free of world geometry? Fills *blocker on a no.
bool room_at(const Body& b, float3 dest, float3* blocker);

// move.cpp
bool teleport(const Ptrs& p, float3 dest, std::string* report);
