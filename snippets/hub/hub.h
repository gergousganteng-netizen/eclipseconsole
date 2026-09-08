// hub.h -- the seam between the shell and the tools.
//
// ONE DOCKED PANEL, SEVERAL TOOLS. The dock is singular (snipwin.cpp: "DOCKING
// STAYS SINGULAR ... a second claimant refused by name"), so two snippets that
// both want an in-game panel cannot both have one. A hub is the shape that fits
// that: one window, tabs down the side.
//
// A TAB IS A VIEW, NOT A SWITCH. Every enabled tool ticks whether or not its
// tab is showing -- switching to Teleport must not quietly turn off whatever
// else was running. The tab only decides what the panel is told about.
#pragma once

#include <string>

// What every tool is handed each tick. Resolved once by the shell rather than
// each tool walking the roster and the rig for itself.
struct Ctx {
    fx_player_info p[64];
    int      n     = 0;
    float3   me{};                 // our root's position
    bool     me_ok = false;
    uint64_t prim  = 0;            // our root Primitive; 0 while dead
    uint64_t hum   = 0;            // our Humanoid; 0 while dead
    uint64_t body[32];             // our BaseParts
    int      nbody = 0;
    uint64_t oPrim = 0, oPos = 0, oVel = 0, oSpin = 0;
    double   now   = 0;            // hub_now_ms(), sampled once per tick
};

double hub_now_ms();
void   hub_eval(const std::string& js);
void   js_escape(const std::string& in, std::string& out);

// Move the WHOLE assembly so the root lands at `at`, keeping the rig's shape.
// Writing only the root leaves the other parts where the solver put them and
// the body tears -- see clicktp's place() and fling's move_body, both of which
// learned this separately.
void   move_body(const Ctx& c, float3 at);

// ---- tools ------------------------------------------------------------------
// Three calls each: a message from the panel, a tick, and "does this tool need
// the fast loop right now". Adding a tool is adding those three to main.cpp.
void tp_msg (const std::string& msg, const Ctx& c);
void tp_tick(const Ctx& c);
bool tp_hot();                 // true while it needs fx_sleep(0)
void tp_panel(std::string& out, const Ctx& c);   // its slice of the panel JSON

void fling_msg (const std::string& msg, const Ctx& c);
void fling_tick(const Ctx& c);
bool fling_hot();
void fling_panel(std::string& out, const Ctx& c);
int  fling_key();                      // its hotkey, polled by the shell
void fling_toggle(const Ctx& c);

void mov_msg  (const std::string& msg, const Ctx& c);
void mov_tick (const Ctx& c);
bool mov_hot();
bool mov_busy();          // wants a faster loop than idle, but not a spin
void mov_stop(const Ctx& c);   // restore the game's own values; MUST run on exit
void mov_panel(std::string& out, const Ctx& c);

void    spec_msg  (const std::string& msg, const Ctx& c);
void    spec_tick (const Ctx& c);
bool    spec_hot();
void    spec_panel(std::string& out, const Ctx& c);
void    spec_stop (const Ctx& c);      // restore the camera; MUST run on exit
int64_t spec_who();

void fly_msg  (const std::string& msg, const Ctx& c);
void fly_tick (const Ctx& c);
bool fly_hot();
void fly_panel(std::string& out, const Ctx& c);
void fly_stop();                       // restore gravity; MUST run on exit
