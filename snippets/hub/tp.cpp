// Teleport to a player.
//
// The mechanism is clicktp's, because it is the one that was measured: move the
// WHOLE assembly, then HOLD the destination for a window. A single write lands
// between two physics steps and the next one puts you back -- that is the
// "it teleports me and then teleports me back" complaint. Holding outlasts the
// step; the hold spins on fx_sleep(0) rather than pacing, since a paced loop
// leaves a gap for the solver to step into.
//
// Then it stops, waits, and READS BACK, so the report says what actually
// happened rather than what we asked for.

#include <cmath>
#include <string>

#include "hub.h"

// How long to keep re-asserting the destination, and how far off is still
// "there". A big jump is what a server notices and reverts, so the report
// distinguishes "arrived" from "snapped back".
const double HOLD_MS   = 300.0;
const double SETTLE_MS = 150.0;
const float  STICK_TOL = 6.0f;
// Land beside them rather than inside: writing into their assembly resolves as
// a separation impulse aimed at whichever of you the solver picks.
const float  OFFSET    = 4.0f;

namespace {

int64_t  g_want    = 0;        // UserId asked for, 0 = idle
float3   g_dest{};
bool     g_have    = false;
double   g_until   = 0;        // holding until
double   g_check   = 0;        // read back at
std::string g_status = "pick a player";

}  // namespace

bool tp_hot() { return g_have && (g_until > 0 || g_check > 0); }

void tp_msg(const std::string& msg, const Ctx& c) {
    if (msg.rfind("tp:", 0) != 0) return;
    int64_t id = _atoi64(msg.c_str() + 3);
    if (!id) return;
    if (!c.prim) { g_status = "you have no character"; return; }

    for (int i = 0; i < c.n; i++) {
        if (c.p[i].userid != id || !c.p[i].root) continue;
        uint64_t pr = fx_rd(c.p[i].root + c.oPrim);
        if (pr < 0x10000 || pr > 0x7ff000000000ULL) break;
        float3 q = fx_rf3(pr + c.oPos);
        // Offset along the line from them to us, so we arrive on the side we
        // came from rather than through them.
        float dx = c.me.x - q.x, dz = c.me.z - q.z;
        float h  = sqrtf(dx*dx + dz*dz);
        if (h > 0.001f) { dx /= h; dz /= h; } else { dx = 1.0f; dz = 0.0f; }

        g_dest  = float3{ q.x + dx * OFFSET, q.y + 1.0f, q.z + dz * OFFSET };
        g_want  = id;
        g_have  = true;
        g_until = c.now + HOLD_MS;
        g_check = 0;
        g_status = std::string("going to ") + c.p[i].name;
        fx_logf("  teleport -> %s", c.p[i].name);
        return;
    }
    g_status = "they have no character right now";
}

void tp_tick(const Ctx& c) {
    if (!g_have || !c.prim) return;

    if (g_until > 0) {
        // Velocity zeroed alongside, or the solver integrates us away from the
        // spot we keep asking for.
        float3 z{ 0, 0, 0 };
        fx_wrblk(c.prim + c.oVel,  &z, 12);
        fx_wrblk(c.prim + c.oSpin, &z, 12);
        move_body(c, g_dest);
        if (c.now >= g_until) { g_until = 0; g_check = c.now + SETTLE_MS; }
        return;
    }

    if (g_check > 0 && c.now >= g_check) {
        // Settled first, THEN read: reading straight after a write returns the
        // value we just wrote, which makes the check impossible to fail.
        g_check = 0;
        g_have  = false;
        float3 at = fx_rf3(c.prim + c.oPos);
        float  dx = at.x - g_dest.x, dy = at.y - g_dest.y, dz = at.z - g_dest.z;
        float  off = sqrtf(dx*dx + dy*dy + dz*dz);
        if (off <= STICK_TOL) {
            g_status = "there";
        } else {
            char b[96];
            snprintf(b, sizeof b, "snapped back (%.0f studs off)", off);
            g_status = b;
        }
        fx_logf("  %s", g_status.c_str());
    }
}

void tp_panel(std::string& out, const Ctx& c) {
    (void)c;
    out += "\"tp\":{\"status\":\"";
    js_escape(g_status, out);
    out += "\",\"busy\":";
    out += (g_have ? "true" : "false");
    out += "}";
}
