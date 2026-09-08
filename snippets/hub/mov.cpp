// Movement -- walkspeed and jump, held against the game.
//
// WRITES Humanoid fields on our own character and nothing else, through
// fx_wrblk, so dry-run until armed.
//
// HELD, NOT SET ONCE. The game rewrites these -- a value written once is gone
// the next time anything touches the Humanoid, which is why the standalone
// snippets read as "it worked for a second". Re-asserted on a slow clock: this
// is not racing the physics solver, so it does not need the spin loop, and
// 100ms is far below anything you would notice.
//
// BOTH FIELDS EACH TIME. Walkspeed has a check copy beside it and the game
// compares them; JumpPower and JumpHeight are two spellings of the same idea
// and which one is live depends on the rig, so both are written and whichever
// is not in use is harmless.

#include <string>

#include "hub.h"

// 60Hz. Some games rewrite these every frame, and at 100ms you get your value
// for one frame in six -- the same shape as the camera-subject fight. This is
// not a race with the physics solver, so it does not need the spin loop, but it
// does need to be at least as often as the game writes.
const double MOV_MS = 16.0;

namespace {

bool   g_ws_on = false, g_jp_on = false;
float  g_ws = 16.0f, g_jp = 50.0f;
// What the game had before we touched it, captured on the way ON and put back
// on the way OFF. REMEMBERED rather than reset to a stock 16/50: those are
// Roblox's defaults, not necessarily this game's, and handing someone 16 in a
// game that walks at 24 is its own bug.
float  g_ws0 = 0.0f, g_jp0 = 0.0f;
bool   g_ws_have0 = false, g_jp_have0 = false;
double g_next = 0;
bool   g_loaded = false;
unsigned g_gen = 0;
bool   g_gen_init = false;

// THE REMEMBERED ORIGINALS BELONG TO ONE PLACE. Unlike fly and spectate there
// are no stale POINTERS here -- the Humanoid comes fresh from the shell every
// tick -- but the captured values are just as place-specific, and restoring
// them into another game is its own wrong answer: switch walkspeed on in a game
// that walks at 24, join one that walks at 16, switch off, and you are handed
// 24 as "the original".
//
// So the capture is dropped on the fx_gen() edge and taken again from the new
// game, before our first write there overwrites what it had. The ON flags are
// deliberately KEPT: a held walkspeed is meant to survive the hop, and that is
// the whole reason the value is re-asserted rather than set once.
void check_gen() {
    unsigned g = fx_gen();
    if (g_gen_init && g == g_gen) return;
    g_gen_init = true;
    g_gen = g;
    g_ws_have0 = g_jp_have0 = false;
}

// NOTHING PERSISTS. Every run starts off, at the game's own values.
//
// Not just tidiness: a persisted ON flag was actively broken. The original
// walkspeed is captured when you switch it on, and a flag restored from the
// config skips that -- so the run would start forcing a value with nothing
// remembered to put back, and switching it off would do nothing at all. There
// is no correct value to restore for a toggle that was never toggled.
void load() {
    if (g_loaded) return;
    g_loaded = true;
}

void save() {}

// Captured on the way on, restored on the way off. Reading before our first
// write is the whole point -- once we are holding a value, the field no longer
// remembers what the game had.
void capture_ws(const Ctx& c) {
    uint64_t o = fx_off("Humanoid.Walkspeed");
    if (!c.hum || o == (uint64_t)-1 || g_ws_have0) return;
    if (fx_rdblk(c.hum + o, &g_ws0, 4) == 4 && g_ws0 > 0.0f) g_ws_have0 = true;
}
void capture_jp(const Ctx& c) {
    uint64_t o = fx_off("Humanoid.JumpPower");
    if (!c.hum || o == (uint64_t)-1 || g_jp_have0) return;
    if (fx_rdblk(c.hum + o, &g_jp0, 4) == 4 && g_jp0 > 0.0f) g_jp_have0 = true;
}
void restore_ws(const Ctx& c) {
    if (!c.hum || !g_ws_have0) return;
    uint64_t o  = fx_off("Humanoid.Walkspeed");
    uint64_t ck = fx_off("Humanoid.WalkspeedCheck");
    if (o  != (uint64_t)-1) fx_wrblk(c.hum + o,  &g_ws0, 4);
    if (ck != (uint64_t)-1) fx_wrblk(c.hum + ck, &g_ws0, 4);
    g_ws_have0 = false;
    fx_logf("  walkspeed back to %.0f", g_ws0);
}
void restore_jp(const Ctx& c) {
    if (!c.hum || !g_jp_have0) return;
    uint64_t p = fx_off("Humanoid.JumpPower");
    uint64_t h = fx_off("Humanoid.JumpHeight");
    if (p != (uint64_t)-1) fx_wrblk(c.hum + p, &g_jp0, 4);
    if (h != (uint64_t)-1) fx_wrblk(c.hum + h, &g_jp0, 4);
    g_jp_have0 = false;
    fx_logf("  jump back to %.0f", g_jp0);
}

}  // namespace

// Never HOT -- this is not racing the solver, so it must not pin the hub's loop
// at fx_sleep(0). But it is BUSY while something is held: the shell idles at
// 60ms, which would cap a 60Hz write clock at 16Hz.
bool mov_hot()  { return false; }
bool mov_busy() { load(); return g_ws_on || g_jp_on; }

// Put the game's own values back. Called on the way out, like spec_stop and
// fly_stop -- leaving someone at 500 walkspeed after the snippet has gone is a
// state they cannot undo from the panel that set it.
void mov_stop(const Ctx& c) {
    if (g_ws_on) { g_ws_on = false; restore_ws(c); }
    if (g_jp_on) { g_jp_on = false; restore_jp(c); }
}

void mov_msg(const std::string& msg, const Ctx& c) {
    load();
    if (msg == "mov:ws") {
        g_ws_on = !g_ws_on;
        if (g_ws_on) capture_ws(c);
        else         restore_ws(c);
    }
    else if (msg == "mov:jp") {
        g_jp_on = !g_jp_on;
        if (g_jp_on) capture_jp(c);
        else         restore_jp(c);
    }
    else if (msg.rfind("mov:wsv:", 0) == 0) g_ws = (float)atof(msg.c_str() + 8);
    else if (msg.rfind("mov:jpv:", 0) == 0) g_jp = (float)atof(msg.c_str() + 8);
    else return;
    if (g_ws < 0)    g_ws = 0;
    if (g_ws > 5000) g_ws = 5000;
    if (g_jp < 0)    g_jp = 0;
    if (g_jp > 5000) g_jp = 5000;
    g_next = 0;                 // apply now rather than up to MOV_MS late
    save();
}

void mov_tick(const Ctx& c) {
    load();
    check_gen();
    if (!c.hum || c.now < g_next) return;
    g_next = c.now + MOV_MS;

    if (g_ws_on) {
        // BEFORE the write, never after -- once we are holding a value the
        // field no longer remembers what the game had. No-ops unless the
        // capture was dropped, which is the first tick in a new place.
        capture_ws(c);
        uint64_t o  = fx_off("Humanoid.Walkspeed");
        uint64_t ck = fx_off("Humanoid.WalkspeedCheck");
        if (o  != (uint64_t)-1) fx_wrblk(c.hum + o,  &g_ws, 4);
        if (ck != (uint64_t)-1) fx_wrblk(c.hum + ck, &g_ws, 4);
    }
    if (g_jp_on) {
        capture_jp(c);
        uint64_t p = fx_off("Humanoid.JumpPower");
        uint64_t h = fx_off("Humanoid.JumpHeight");
        if (p != (uint64_t)-1) fx_wrblk(c.hum + p, &g_jp, 4);
        if (h != (uint64_t)-1) fx_wrblk(c.hum + h, &g_jp, 4);
    }
}

void mov_panel(std::string& out, const Ctx& c) {
    load();
    char b[192];
    snprintf(b, sizeof b,
             "\"mov\":{\"ws_on\":%s,\"jp_on\":%s,\"ws\":%.0f,\"jp\":%.0f,\"ok\":%s}",
             g_ws_on ? "true" : "false", g_jp_on ? "true" : "false",
             g_ws, g_jp, c.hum ? "true" : "false");
    out += b;
}
