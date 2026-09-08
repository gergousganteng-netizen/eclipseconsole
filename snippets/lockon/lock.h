#pragma once
// Shared across the project. float3 and the fx_* API are already in scope --
// the host passes -include fx.h, which lands above everything.
#include <cmath>

struct Target {
    // The lock's identity is the PLAYER instance, never the name and never the
    // Character. Names are not unique and can be changed; the Character is
    // freed and rebuilt on every respawn. The Player outlives both, for as long
    // as they are in the server.
    uint64_t player;
    char     name[48];
    float3   world;
    float    dist;      // studs from the camera
    bool     onscreen;
    float    sx, sy;    // pixels
};

// world.cpp
int  collect_targets(Target* out, int max);
// Re-resolve ONE player, live. `vel` may be null; when it is not, it comes back
// as studs/second, zeroed rather than left stale if it cannot be trusted.
bool aim_point(uint64_t player, float3* pos, float3* vel);
bool local_origin(float3* out);                 // our own HumanoidRootPart

// view.cpp
uint64_t find_camera();
bool camera_basis(float R[9], float3* pos, float* fov, float vp[2]);
bool project_to_screen(float3 world, float* sx, float* sy);
// Roblox window rect on the monitor. Add (x, y) to project_to_screen for
// MONITOR-space pixels (e.g. for the overlay pipe).
bool get_rbx_window(int* x, int* y, int* w, int* h);
// `pitchFrom` may be NULL, which means "leave the vertical angle exactly as it
// is". A pointer rather than a bool flag because the two cases genuinely differ
// in what they need: one wants an origin, the other wants nothing at all.
bool aim_from(float3 yawOrigin, const float3* pitchFrom, float3 world, float smoothing);

// Diagnostics: what the camera is ACTUALLY doing, read back from memory.
//   yawErrorDeg  how far the camera's facing is from the target, horizontally
//   sideOffset   studs the camera sits to the right of the character -- the
//                shift lock offset, measured rather than assumed
bool aim_report(float3 charPos, float3 world, float* yawErrorDeg, float* sideOffset);

// small vector helpers -- inline, so every file gets them without a .cpp
static inline float3 v_sub(float3 a, float3 b) { return float3{ a.x-b.x, a.y-b.y, a.z-b.z }; }
static inline float  v_dot(float3 a, float3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline float3 v_cross(float3 a, float3 b) {
    return float3{ a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
}
static inline float3 v_norm(float3 a) {
    float l = sqrtf(v_dot(a, a));
    if (l < 1e-6f) return float3{ 0, 0, 1 };
    return float3{ a.x/l, a.y/l, a.z/l };
}
