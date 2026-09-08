#pragma once
// Shared across the project. float3 and the fx_* API are already in scope --
// the host passes -include fx.h, which lands above everything.
#include <cmath>

// Never AIM at your own team. They are still drawn, in TEAM_RGB -- knowing
// where a teammate is matters too, you just do not want the aim taking
// them. Ignored in games without teams; see same_team() in world.cpp.
// Here rather than in main.cpp because world.cpp walks the player list.
// TUNABLE AT RUNTIME. These are no longer const because the settings panel
// edits them live; their values, and the comments explaining each one, still
// live where they always did in main.cpp. cfg.cpp only moves them in and out of
// %APPDATA%\EclipseConsole\settings.conf.
extern bool TEAM_CHECK;
const int  TEAM_RGB   = 0x6EE7A8;   // green for friendly, white for the rest

extern float    SMOOTHING;
extern float    MAX_DIST;
extern float    LEAD_SECONDS;
extern float    SWITCH_FRACTION;
extern double   SWITCH_DWELL_MS;
extern unsigned AIM_HZ;
extern unsigned ESP_HZ;
extern int      TARGET_PRIORITY;
extern float    PICK_RADIUS;

// cfg.cpp -- settings, persisted through fx_cfg
#include <string>
void        cfg_load();
void        cfg_save();
bool        cfg_set(const std::string& key, double v);   // false = unknown key
void        cfg_reset();
std::string cfg_json();

// ui.cpp -- the settings panel
std::string ui_page_path();
std::string ui_state_js(const char* target, int nTargets, bool aiming);

struct Target {
    char   name[48];
    float3 world;      // head position -- what the AIM uses
    float3 hrp;        // HumanoidRootPart -- what the ESP BOX is built around
    float3 vel;        // studs/second, zeroed if it could not be trusted
    float  dist;       // studs from the camera
    bool   onscreen;
    float  sx, sy;     // pixels
    bool   team;       // drawn, never aimed at
};

// world.cpp
int  collect_targets(Target* out, int max);
bool local_origin(float3* out);                   // OUR HumanoidRootPart

// view.cpp
uint64_t find_camera();
bool     camera_basis(float R[9], float3* pos, float* fov, float vp[2]);
bool     project_to_screen(float3 world, float* sx, float* sy);
// Roblox window rect on the monitor. Add (x, y) to a project_to_screen
// result to get MONITOR-space pixels for the overlay pipe.
bool     get_rbx_window(int* x, int* y, int* w, int* h);
// The only write in the project. Yaw and pitch are measured from DIFFERENT
// origins; see the comment on the definition.
bool     aim_from(float3 yawOrigin, float3 pitchOrigin, float3 world, float smoothing);
// Pure read: where the camera actually ended up, and the shift lock offset,
// measured rather than assumed.
bool     aim_report(float3 charPos, float3 world, float* yawErrorDeg, float* sideOffset);

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
