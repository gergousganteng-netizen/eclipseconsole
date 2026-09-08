#pragma once
#include <cstdint>
#include <string>

// Every dial lives in one struct so the panel can edit it and settings.ini can
// hold it. The values below are the DEFAULTS -- what you get before the file
// exists, and what Reset puts back.
struct Cfg {
    // ----------------------------- --SETTINGS-- -----------------------------

    // Keys. Virtual-key codes; a letter is just its uppercase character.
    int    key_fps  = 'E';        // toggle the frame cap
    int    key_dash = VK_F8;      // fire the dash

    // 0 back, 1 forward, 2 left, 3 right, 4 up, 5 down.
    int    dash_dir = 0;

    // Velocity written, scaled by remaining HP: full HP gets power_max.
    double power_min = 90.0;
    double power_max = 120.0;

    // How long the velocity is held, and how often it is rewritten while held.
    double hold_secs = 0.15;
    int    hold_hz   = 1000;

    // Jump first, then dash once airborne.
    bool   do_jump    = false;
    double jump_delay = 0.15;

    // Minimum seconds between dashes.
    double cooldown = 0.0;

    // Gates. Both refuse the dash rather than delay it.
    bool   use_sound_gate = true;    // only dash within gate_secs of the sound
    bool   use_hp_gate    = false;   // refuse for hp_gate_secs after damage
    // Matched as a SUBSTRING, so the bare id or the full rbxassetid:// form
    // both work, whichever way the game stores it.
    std::string gate_sound = "rbxassetid://10492011015";
    double gate_secs    = 0.35;      // how long the gate stays open once heard
    double hp_gate_secs = 0.75;

    // FPS toggle swaps between these two caps.
    double fps_low  = 30.0;
    double fps_high = 144.0;

    // --------------------------- end --SETTINGS-- ---------------------------
};

extern Cfg g;

// UI refresh. Not in Cfg: changing it from the panel that the tick redraws is
// a good way to make the panel unusable.
const unsigned TICK_MS = 100;

struct Ptrs {
    uint64_t character = 0, humanoid = 0, hrp = 0, primitive = 0, head = 0;
    bool ok = false;
};

// settings.cpp
std::string cfg_path();
void        cfg_load();
void        cfg_save();
bool        cfg_set(const std::string& key, const std::string& val);  // false = unknown key
void        cfg_reset();
std::string cfg_json();

// world.cpp
bool  refresh_ptrs(Ptrs* out);
float hp_ratio(uint64_t humanoid);
bool  read_rot(uint64_t primitive, float* rot9);
bool  dir_vector(int dir, const float* rot9, float* bx, float* bz, float* by, bool* vertical);

// gates.cpp
void   gates_tick(const Ptrs& p);     // every loop: tracks HP drops and the gate sound
bool   sound_gate_open();
double sound_gate_left();             // seconds left in the window, 0 if shut
bool   hp_gate_open();
double last_damage_age();             // seconds since the last HP drop, -1 if none

// dash.cpp
// Split in two so the burst can run off the main loop. dash_begin does the
// deciding -- gates, cooldown, direction, power -- and is fast; dash_run is the
// part that holds a thread for hold_secs.
struct DashPlan {
    // The generation the pointers below were resolved under. The burst runs on
    // a WORKER THREAD for hold_secs, so unlike the main loop it cannot just
    // re-resolve -- it holds `primitive` and `humanoid` for the whole burst.
    // Die mid-dash and both are freed while up to hold_secs * hold_hz writes
    // are still queued against them (150 at the defaults). fx_running() does
    // not cover this: it is Stop, not death. dash_run bails when this moves.
    unsigned gen = 0;
    uint64_t primitive = 0, humanoid = 0, vel_off = 0, jump_off = 0;
    float    bx = 0, bz = 0, by = 0;
    bool     vertical = false;
    double   power = 0;
    double   hold_secs = 0;   // copied, so editing a dial mid-burst cannot
    int      hold_hz = 0;     // change the burst that is already running
    bool     do_jump = false;
    double   jump_delay = 0;
};

bool   dash_begin(const Ptrs& p, DashPlan* plan, std::string* why);
void   dash_run(const DashPlan& plan);
double dash_cooldown_left();

// ui.cpp
std::string ui_page_path();           // the .html this window loads
std::string ui_state_js(const Ptrs& p, double fps, int dashes, bool busy,
                        const std::string& status);
