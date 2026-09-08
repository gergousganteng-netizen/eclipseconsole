// Spectate -- point the camera at somebody else.
//
// WRITES ONE FIELD: Camera.CameraSubject, on our own client's camera. Nothing
// touches the player being watched. Through fx_wrblk, so dry-run until armed.
//
// THE MECHANISM IS FREECAM'S, MINUS THE BORROWED PART. Writing Camera.Position
// every frame does not work -- the camera module rewrites it and wins (measured
// 4 of 187 passes held, and CameraType = Scriptable did not change that). What
// DOES work is retargeting what the module follows: CameraSubject held 187 of
// 187. Spectating is the easy case of that, because the thing to follow already
// exists and does not need borrowing or hiding.
//
// RESTORE IS THE PART THAT MATTERS. A camera left pointing at someone who has
// left the server is a black screen with no obvious cause, so the subject is put
// back on toggle, when the target dies or leaves, on our own respawn, and at
// shutdown -- and it is restored to our CURRENT Humanoid rather than the pointer
// saved on entry, which is dead if we respawned while watching.

#include <string>

#include "hub.h"

const double REASSERT_MS = 100.0;

namespace {

int64_t  g_who   = 0;          // UserId being watched, 0 = off
uint64_t g_cam   = 0;
uint64_t g_subj0 = 0;          // what the camera followed before we started
double   g_next  = 0;
// SOME GAMES FIGHT BACK. Measured in one of them: a single write held for
// exactly 1 sample out of 399 -- the game's own camera script reassigns
// CameraSubject every frame. A 100ms re-assert against that gives you the
// target for one frame in six, which is unwatchable.
//
// Out-writing it wins outright (1,366,073 ours vs 384 theirs, 99.97%), but
// spinning permanently would pin the hub's whole loop at fx_sleep(0) in the
// games that never needed it. So it starts cheap and escalates only when it
// actually sees the subject put back.
bool     g_fighting = false;
std::string g_status;
unsigned g_gen = 0;
bool     g_gen_init = false;

uint64_t off_subject() { return fx_off("Camera.CameraSubject"); }

// THE CAMERA BELONGS TO ONE DATAMODEL. g_cam and g_subj0 are both pointers into
// the place we were in; joining another game frees them, and freed memory here
// still reads, so nothing announces it. The shell re-resolves its own rig on
// the fx_gen() edge and does not tell the tools -- so watch the counter here.
//
// Without this the hop path was: the target is not in the new roster, so
// spec_tick calls restore(), which writes our NEW Humanoid pointer to the OLD
// camera's address. An armed 8-byte write to somewhere that was never right.
// Cleared rather than restored -- the new place gives us a fresh camera already
// following our own character, so there is nothing left to put back.
void check_gen() {
    unsigned g = fx_gen();
    if (g_gen_init && g == g_gen) return;
    g_gen_init = true;
    g_gen = g;
    g_cam = g_subj0 = 0;
    if (g_who) {
        g_who      = 0;
        g_fighting = false;
        g_status   = "";
        fx_log("  spectate off - different place now");
    }
}

uint64_t find_cam() {
    uint64_t ws = fx_service("Workspace");
    if (!ws) return 0;
    uint64_t o = fx_off("Workspace.CurrentCamera");
    if (o != (uint64_t)-1) {
        uint64_t cam = fx_rd(ws + o);
        if (cam >= 0x10000 && cam <= 0x7ff000000000ULL) return cam;
    }
    // The conf key has been wrong before; the walk cannot be.
    uint64_t n = fx_nchildren(ws);
    if (n > 4096) n = 4096;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t k = fx_child(ws, (int)i);
        if (k && strcmp(fx_class(k), "Camera") == 0) return k;
    }
    return 0;
}

// What to point the camera at for a player: their Humanoid is what the module
// expects, and it is what our own subject normally is.
uint64_t subject_of(const fx_player_info& p) {
    if (p.humanoid) return p.humanoid;
    if (p.model) {
        uint64_t h = fx_part(p.model, "Head");
        if (h) return h;
    }
    return p.root;
}

void restore(const Ctx& c) {
    uint64_t o = off_subject();
    if (!g_cam || o == (uint64_t)-1) return;
    // Our CURRENT Humanoid first: g_subj0 is stale if we respawned while
    // watching, and writing a freed pointer back is how you get a black screen.
    uint64_t back = c.hum ? c.hum : g_subj0;
    if (back) fx_wrblk(g_cam + o, &back, 8);
}

}  // namespace

// Hot only in the games that reassign the subject -- see g_fighting. Elsewhere
// a 100ms re-assert is plenty and the loop can stay idle.
bool spec_hot() { return g_who != 0 && g_fighting; }

int64_t spec_who() { return g_who; }

void spec_msg(const std::string& msg, const Ctx& c) {
    check_gen();
    if (msg == "spec:off") {
        if (!g_who) return;
        restore(c);
        g_who      = 0;
        g_fighting = false;
        g_status   = "";
        fx_log("  spectate off");
        return;
    }
    if (msg.rfind("spec:", 0) != 0) return;
    int64_t id = _atoi64(msg.c_str() + 5);
    if (!id) return;
    if (id == g_who) { spec_msg("spec:off", c); return; }   // toggle

    uint64_t o = off_subject();
    if (o == (uint64_t)-1) { g_status = "no Camera.CameraSubject offset"; return; }
    if (!g_cam) g_cam = find_cam();
    if (!g_cam) { g_status = "no camera"; return; }

    for (int i = 0; i < c.n; i++) {
        if (c.p[i].userid != id) continue;
        uint64_t s = subject_of(c.p[i]);
        if (!s) { g_status = "they have no character"; return; }
        if (!g_who) g_subj0 = fx_rd(g_cam + o);   // only the FIRST time in
        fx_wrblk(g_cam + o, &s, 8);
        g_who      = id;
        g_next     = 0;
        g_fighting = false;      // re-learn per target; games differ, not people
        g_status = std::string("watching ") + c.p[i].name;
        fx_logf("  spectating %s", c.p[i].name);
        return;
    }
    g_status = "they are not here";
}

void spec_tick(const Ctx& c) {
    check_gen();
    if (!g_who) return;
    // Once we know the game reassigns it, every tick. Otherwise the slow clock.
    if (!g_fighting && c.now < g_next) return;
    g_next = c.now + REASSERT_MS;

    uint64_t o = off_subject();
    if (o == (uint64_t)-1 || !g_cam) return;

    // Follow them across their respawns: the Humanoid is replaced, and the old
    // pointer is what a frozen or black view looks like.
    for (int i = 0; i < c.n; i++) {
        if (c.p[i].userid != g_who) continue;
        uint64_t s = subject_of(c.p[i]);
        if (!s) return;                       // mid-respawn: hold, do not drop
        uint64_t cur = fx_rd(g_cam + o);
        if (cur != s) {
            // It was ours and is not any more: the game put it back. Escalate
            // to writing every tick, which measurement says we win.
            if (!g_fighting) {
                g_fighting = true;
                fx_log("  spectate: this game reassigns the camera - holding it");
            }
            fx_wrblk(g_cam + o, &s, 8);
        }
        return;
    }
    // They left the server. Nothing will ever point this camera back but us.
    restore(c);
    g_who      = 0;
    g_fighting = false;
    g_status   = "they left";
    fx_log("  spectate off - they left");
}

// Called when the snippet stops, and on a place change: see the header.
// check_gen first: after a place change it has already cleared g_who, so this
// returns without writing our new Humanoid into the old camera's address.
void spec_stop(const Ctx& c) {
    check_gen();
    if (!g_who) return;
    restore(c);
    g_who      = 0;
    g_fighting = false;
}

void spec_panel(std::string& out, const Ctx& c) {
    (void)c;
    char b[48];
    snprintf(b, sizeof b, "\"spec\":{\"who\":%lld,\"status\":\"", (long long)g_who);
    out += b;
    js_escape(g_status, out);
    out += "\"}";
}
