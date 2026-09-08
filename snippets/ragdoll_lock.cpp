// ragdoll_lock.cpp -- lock on, and let the body come along for the ride.
//
// TAP the key to lock on, tap again to let go. Whoever is nearest your crosshair
// at the moment you switch it on is the target, and nothing takes them off it --
// not a closer player, not them leaving your view, not a tick where they could
// not be read. You chose by pointing; the lock should not second-guess that.
//
// While it is on your CHARACTER is held facing wherever the camera faces, which
// the game otherwise only does when it feels like it. VERTICAL = 1 hands the
// body the camera's PITCH as well, so looking down face-plants you and looking
// up leans you back; at the default of 0 you stay upright and only turn.
//
// A second key does the body on its own, with no target and no lock.
//
// The targeting is lifted from the lockon/ project rather than reinvented, and
// the three things it does differently all turned out to matter:
//
//   1. It aims at the ROOT PART, not the head. A lock is held for seconds, and
//      the head is the part that moves most -- idle and run animations swing it
//      around while the torso stays where the character actually is. Aiming at
//      the head makes a standing-still target look jittery.
//   2. It picks by SCREEN PIXELS from the crosshair, not by angle in 3D. Those
//      are not the same ordering once FOV and aspect are involved, and the
//      pixel one is what you actually see.
//   3. It caches NOTHING derived from the Player. The Character pointer dies on
//      every respawn, and a cached one reads as the target frozen at the spot
//      where they died -- because the freed memory still holds their last
//      position until something else claims it.
//
// It also aims FROM YOUR CHARACTER, not from the camera: the yaw is measured
// character-to-target so your body genuinely faces them, instead of past them
// by the shift lock offset. The target then sits slightly left of the
// crosshair, which is correct rather than a bug -- see the comment at the turn.
//
// Like lockon it is horizontal by default and leaves your pitch alone. Unlike
// lockon, VERTICAL = 1 will drive the pitch too -- there because the pitch is
// what tips the body over, not because a lock has any business holding it.
//
// Both writes are DATA writes to fields the engine rewrites every frame -- a
// camera basis and a part's rotation. Nothing here is code, a hook, or anything
// the client interprets as instructions, and fx_wrblk is dry-run until the
// console is armed, so a disarmed run logs what it WOULD write and does
// nothing.
//
// Why the body normally stays upright, measured on a live client rather than
// assumed:
//
//     Camera.Rotation            Primitive.Rotation (root part)
//     [-0.383 -0.239  0.892]     [-0.383  0.000  0.924]
//     [ 0.000  0.966  0.259]     [ 0.000  1.000  0.000]
//     [-0.924  0.099 -0.370]     [-0.924  0.000 -0.383]
//
// Same yaw, same 9-float column-major layout, both orthonormal -- the root part
// simply throws the pitch away. Handing it the camera basis verbatim is the
// entire trick.

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

// ---- dials -------------------------------------------------------------------
// A TAP toggles, so this wants a key you do not otherwise hold down.
// VK_RBUTTON would fight aim-down-sights; mouse5 is free in most games.
// 0x45 is E, 0x46 is F, VK_XBUTTON1 is mouse4.
const int   TOGGLE_KEY  = 'E';    // lock on / let go
const int   RAGDOLL_KEY = 'R';    // body-follow on its own, no target needed
const float SMOOTHING  = 0.55f;   // 0..1 of the way to the target per tick
const float MAX_DIST   = 500.0f;  // studs. Filters ACQUISITION only -- once
                                  // locked they can run as far as they like.
// Milliseconds to sleep at the end of every tick.
//
// fx_sleep, not fx_pace, by request. The difference is worth knowing: fx_pace
// waits until the next period BOUNDARY, so the loop body's runtime comes out of
// the wait and the rate you ask for is the rate you get. fx_sleep waits a fixed
// span AFTER the body, so the real period is body + SLEEP_MS and the rate is
// always lower than 1000/SLEEP_MS -- by however long the reads took, which
// varies with how many players are in the server.
//
// 1 ms is the FLOOR, and it is a hard one: fx_sleep takes whole milliseconds,
// so anything below 1 truncates to 0 and does not sleep at all. Measured what
// that costs -- 6.55 CPU seconds over 8 seconds of wall clock, ~82% of a core,
// for a loop waiting on a game that renders at a fraction of the rate.
//
// If you want faster than 1000 Hz, fx_sleep is the wrong tool and no value here
// will do it. fx_pace(2000) is how you get half-millisecond periods, and it is
// more accurate besides.
const unsigned SLEEP_MS = 0;

// Turns the truncation above into a compile error rather than a spinning core.
// A fractional literal here is the easy mistake -- 0.5 looks like it should
// mean something and quietly becomes 0.

// Unreadable ticks before the lock gives up and re-acquires -- about a second,
// derived from the sleep so the timeout stays a second if SLEEP_MS changes.
const int   LOST_TICKS = 1000 / (SLEEP_MS ? SLEEP_MS : 1);

// Horizontal, or horizontal and vertical. ONE dial, covering both the camera
// and the body -- they were briefly separate and that was a mistake: the camera
// keeps your own pitch even when it is not aiming with it, and the body copies
// the camera, so turning the lock's pitch off did not keep the character
// upright. Splitting them meant two dials that both had to be right.
//
//   0  YAW ONLY. The lock holds the yaw on the target and leaves your up/down
//      angle entirely yours; the body turns to face where you look and stays
//      UPRIGHT. Still not normal behaviour -- the game only turns the body when
//      it feels like it, and this holds it on your view constantly.
//   1  YAW AND PITCH. The lock pitches at the target as well, and the body
//      takes the full basis, so looking down face-plants you and looking up
//      leans you back. The original gag.
const int   VERTICAL = 1;

// Does locking on ALSO tip the body, or is that only ever the ragdoll key?
// 1 keeps the two together the way they started; 0 separates them completely,
// so the lock is a plain lock and RAGDOLL_KEY is the whole comedy budget.
const int   BODY_WITH_LOCK = 1;

// Measure the yaw from your CHARACTER rather than from the camera, so your
// character genuinely faces the target instead of past it by the shift lock
// offset. See the long comment where it is used. False aims the camera itself,
// which centres them on screen and leaves the body pointing wide.
const bool  AIM_FROM_CHARACTER = true;

// How far off the wanted basis has to be before a camera write is worth it.
// Not zero: every write competes with the engine's own camera update, so one
// that changes nothing is pure interference -- and disarmed, fx_wrblk logs each
// write it refuses, which at this rate buries the console.
//
// The BODY write is deliberately not gated by this. The engine rights the
// character every frame, so that one has to keep going out even when the
// camera has settled.
const float DEADZONE   = 0.0008f;

// Where to aim on a character, in order of preference. Root part first: see the
// header. Head is last precisely because it is the worst of the four.
static const char* kAimPart[] = { "HumanoidRootPart", "UpperTorso", "Torso", "Head" };

// ---- output ------------------------------------------------------------------
// fx_log, never printf: the GUI console has no stdout, so printf output goes
// nowhere at all while looking perfectly fine under eclipse_cli.exe.
static void lg(const char* fmt, ...) {
    char b[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    fx_log(b);
}

// ---- small vector helpers ----------------------------------------------------
static float3 sub(float3 a, float3 b) { return float3{ a.x-b.x, a.y-b.y, a.z-b.z }; }
static float  dot(float3 a, float3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static float3 cross(float3 a, float3 b) {
    return float3{ a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
}
static float3 norm(float3 a) {
    float l = sqrtf(dot(a, a));
    if (l < 1e-6f) return float3{ 0, 0, 1 };
    return float3{ a.x/l, a.y/l, a.z/l };
}

// A read that landed on something else, rather than a real position. It matters
// more here than in an ESP: this feeds a rotation the camera is then SET to, so
// garbage does not show a silly number, it throws the view somewhere random.
static bool sane3(float3 v, float limit) {
    if (!(v.x == v.x && v.y == v.y && v.z == v.z)) return false;      // NaN
    return !(v.x >  limit || v.x < -limit || v.y >  limit ||
             v.y < -limit || v.z >  limit || v.z < -limit);
}

// ---- instances ---------------------------------------------------------------

// Reading BasePart.Primitive on something that is not a BasePart returns
// whatever sits at that offset and it looks pointer-shaped -- a Script gives
// back a module address. Writing through one corrupts the client, so the class
// is checked before the pointer is ever used.
static bool is_part(const char* cls) {
    static const char* ok[] = { "Part", "MeshPart", "UnionOperation", "TrussPart",
                                "WedgePart", "CornerWedgePart", "Seat", "VehicleSeat" };
    for (const char* k : ok) if (strcmp(cls, k) == 0) return true;
    return false;
}

static uint64_t primitive_of(uint64_t part) {
    if (!part || !is_part(fx_class(part))) return 0;
    uint64_t o = fx_off("BasePart.Primitive");
    if (o == (uint64_t)-1) return 0;
    uint64_t p = fx_rd(part + o);
    return (p >= 0x10000 && p <= 0x7fffffffffffULL) ? p : 0;
}

static bool part_pos(uint64_t part, float3* out) {
    uint64_t prim = primitive_of(part);
    uint64_t o = fx_off("Primitive.Position");
    if (!prim || o == (uint64_t)-1) return false;
    float3 p = fx_rf3(prim + o);
    if (!sane3(p, 1.0e6f)) return false;
    *out = p;
    return true;
}

// fx_part, not fx_find: this runs once per candidate per tick during
// acquisition and once per tick on the locked target, and fx_find re-reads
// every sibling's name each time to answer a question that cannot change while
// the model lives. Measured 375x on a nine-rig server.
static uint64_t aim_part_of(uint64_t chr) {
    for (const char* nm : kAimPart)
        if (uint64_t p = fx_part(chr, nm)) return p;
    return 0;
}

// The locked target, re-resolved from scratch. Nothing derived from the Player
// is kept between ticks -- that is the point, not an oversight.
static bool aim_point(uint64_t player, float3* pos) {
    uint64_t oChr = fx_off("Player.ModelInstance");
    if (oChr == (uint64_t)-1) return false;
    uint64_t chr = fx_rd(player + oChr);
    if (!chr) return false;                    // dead, or still loading in
    uint64_t part = aim_part_of(chr);
    return part && part_pos(part, pos);
}

static uint64_t find_camera(void) {
    uint64_t ws = fx_service("Workspace");
    if (!ws) return 0;
    // Not cached across generations: the Camera instance is replaced on respawn
    // in some games, and a stale pointer here writes into freed memory.
    for (uint64_t i = 0, n = fx_nchildren(ws); i < n; i++) {
        uint64_t c = fx_child(ws, (int)i);
        if (c && strcmp(fx_class(c), "Camera") == 0) return c;
    }
    return 0;
}

// ---- projection ---------------------------------------------------------------
// fx_screen now. It reads Roblox's OWN world-to-clip matrix, same as the copy
// that used to live here, and it also rejects points outside the frame -- which
// this copy did NOT. That was a real bug: acquisition compares pixel distance
// to the crosshair, and a target off to the side passed the depth test and
// could win the pick while not being on screen at all.

// A rotation that is not orthonormal is not a wrong angle, it is a malformed
// CFrame -- the physics engine is entitled to do anything with it. Checked
// before every write rather than trusted.
static bool orthonormal(const float R[9]) {
    for (int c = 0; c < 3; c++) {
        float l = sqrtf(R[0*3+c]*R[0*3+c] + R[1*3+c]*R[1*3+c] + R[2*3+c]*R[2*3+c]);
        if (fabsf(l - 1.0f) > 0.02f) return false;
    }
    float det = R[0]*(R[4]*R[8]-R[5]*R[7])
              - R[1]*(R[3]*R[8]-R[5]*R[6])
              + R[2]*(R[3]*R[7]-R[4]*R[6]);
    return fabsf(det - 1.0f) <= 0.02f;
}

// ---- run ---------------------------------------------------------------------

extern "C" void fx_run() {
    uint64_t oCamR  = fx_off("Camera.Rotation");
    uint64_t oCamP  = fx_off("Camera.Position");
    uint64_t oCamVp = fx_off("Camera.ViewportSize");
    uint64_t oPrimR = fx_off("Primitive.Rotation");
    uint64_t oModel = fx_off("Player.ModelInstance");
    uint64_t oLP    = fx_off("Player.LocalPlayer");
    if (oCamR == (uint64_t)-1 || oCamP == (uint64_t)-1 || oCamVp == (uint64_t)-1 ||
        oPrimR == (uint64_t)-1 || oModel == (uint64_t)-1 || oLP == (uint64_t)-1) {
        lg("missing an offset: Camera.Rotation/Position/ViewportSize,");
        lg("Primitive.Rotation, Player.ModelInstance, Player.LocalPlayer");
        return;
    }

    lg("ragdoll lock: '%c' locks on, '%c' tips the body on its own.",
       (char)TOGGLE_KEY, (char)RAGDOLL_KEY);
    lg("  lock drives %s; body takes %s.",
       VERTICAL  ? "yaw and pitch" : "yaw only",
       VERTICAL ? "yaw and pitch" : "yaw only (stays upright)");
    lg("  %s", fx_armed() ? "ARMED -- this will actually write."
                          : "disarmed -- writes are logged, not performed.");

    unsigned gen = fx_gen() - 1;      // never compare fx_gen against 0
    uint64_t cam = 0;

    // The lock holds a PLAYER, not a character or a part. Characters die on
    // every respawn and parts are re-created with them, so anything else as the
    // identity silently becomes a different person -- or a freed pointer. The
    // Player outlives both, for as long as they are in the server.
    uint64_t locked = 0;
    bool     on = false, ragdoll = false;
    unsigned last_lock = fx_key_seq(TOGGLE_KEY);
    unsigned last_rag  = fx_key_seq(RAGDOLL_KEY);
    int      unreadable = 0;

    // fx_key_seq is a COUNTER, not a level, so a tap landing entirely between
    // two polls still registers -- fx_key would miss it. Parity of the
    // difference, so two taps inside one tick are a no-op rather than one.
    //
    // A press while alt-tabbed or typing is CONSUMED but does not toggle:
    // otherwise typing the bound letter in chat turns it on and you find out by
    // falling over.
    auto tapped = [](unsigned& last, int vk) -> bool {
        unsigned seq = fx_key_seq(vk);
        if (seq == last) return false;
        bool odd = ((seq - last) & 1u) != 0;
        last = seq;
        return odd && fx_focused() && !fx_typing();
    };

    while (fx_running()) {
        if (fx_gen() != gen) {        // respawn, teleport, rejoin: re-resolve all
            gen = fx_gen();
            // Players and LocalPlayer used to be re-resolved here. The roster
            // does that itself, against the host's own generation counter, so
            // only the camera is left to re-find.
            cam     = find_camera();
            // `locked` deliberately survives: a Player instance is not what
            // fx_gen invalidates, and dropping the target every time the
            // target respawns is exactly the behaviour being avoided.
        }

        if (tapped(last_lock, TOGGLE_KEY)) {
            on = !on;
            locked = 0;
            unreadable = 0;
            if (!on) lg("  lock off");
        }
        if (tapped(last_rag, RAGDOLL_KEY)) {
            ragdoll = !ragdoll;
            lg("  ragdoll %s", ragdoll ? "on" : "off");
        }

        bool body_now = ragdoll || (on && BODY_WITH_LOCK);
        if (!on && !body_now) { fx_sleep(SLEEP_MS); continue; }

        if (!cam) { cam = find_camera(); if (!cam) { fx_sleep(SLEEP_MS); continue; } }

        float R[9];
        if (fx_rdblk(cam + oCamR, R, 36) != 36 || !orthonormal(R)) {
            fx_sleep(SLEEP_MS); continue;
        }
        float3 eye = fx_rf3(cam + oCamP);

        // What the body will be given. Defaults to the camera as it stands, so
        // the ragdoll key works with no target at all -- and so a lock that
        // cannot read its target this tick still holds the body where it is
        // rather than dropping it for a frame.
        float body[9];
        memcpy(body, R, sizeof body);

        // Everything from here to the camera write is the LOCK, and is skipped
        // entirely when only the ragdoll key is on. do/while(0) so that every
        // failure below is a `break` that still reaches the body write and the
        // single fx_sleep at the bottom -- an early `continue` would skip both,
        // and sleeping twice in one tick would quietly halve the rate.
        if (on) do {

        // --- acquire, once, on the tick the lock came on -----------------------
        // Nearest to the CROSSHAIR in pixels, not nearest in the world: the one
        // you are already roughly pointing at is the one you meant. Scanning
        // every player happens HERE and nowhere else -- once locked the loop
        // resolves exactly one character per tick, which is what makes a
        // four-figure tick rate affordable.
        if (!locked) {
            // The crosshair is the middle of the viewport fx_screen divides by,
            // read from the same source so the centre and the projections
            // cannot disagree about the size of the picture.
            float vw = 0, vh = 0;
            if (!fx_viewport(&vw, &vh)) break;
            float cx = vw * 0.5f, cy = vh * 0.5f;

            float best_d2 = 1e18f;
            // The roster hands back .player directly, which is the identity
            // this lock is careful to hold (see the comment above `locked`).
            fx_player_info roster[64];
            int nr = fx_players(roster, 64);
            for (int i = 0; i < nr && fx_running(); i++) {
                if (roster[i].is_local) continue;
                uint64_t pl = roster[i].player;

                float3 p;
                if (!aim_point(pl, &p)) continue;

                float3 d = sub(p, eye);
                float  dist = sqrtf(dot(d, d));
                if (dist < 1.0f || dist > MAX_DIST) continue;

                float sx, sy;
                // == 1: acquisition picks by pixel distance to the crosshair,
                // so a target off the edge must not be allowed to win it.
                if (fx_screen(p, &sx, &sy) != 1) continue;
                float dx = sx - cx, dy = sy - cy;
                float d2 = dx*dx + dy*dy;
                if (d2 < best_d2) { best_d2 = d2; locked = pl; }
            }
            if (!locked) {
                on = false;
                lg("  nothing on screen to lock");
                break;
            }
            lg("  locked '%s', %.0f px from the crosshair", fx_name(locked),
               sqrtf(best_d2));
            unreadable = 0;
        }

        // --- follow whoever is locked, wherever they went ----------------------
        // No re-pick, no screen test, no nearest-player check. They leave your
        // view, you keep turning; they respawn, the character pointer changes
        // and aim_point finds the new one through the same Player.
        float3 tp{};
        if (!aim_point(locked, &tp)) {
            // A tick that could not be read is NOT a reason to drop the lock --
            // dead, mid-respawn and behind-a-wall all read identically here.
            // Only a sustained silence means they are really gone.
            if (++unreadable > LOST_TICKS) {   // about a second of nothing
                lg("  lost '%s' -- re-acquiring", fx_name(locked));
                locked = 0;
                unreadable = 0;
            }
            break;
        }
        unreadable = 0;

        // --- turn the camera onto them -----------------------------------------
        // YAW from one origin, PITCH from another, and splitting them is the
        // whole shift lock correction.
        //
        // Yaw, from the CHARACTER: under shift lock the camera sits about 1.75
        // studs to the RIGHT, and the character's facing follows the camera's
        // yaw -- so a camera aimed exactly at the target leaves the CHARACTER
        // aimed past it, by atan(1.75 / distance). That is 5 degrees at 20
        // studs and worse as you close. Measuring yaw from the character
        // cancels it with no magic 1.75 anywhere: whatever the offset actually
        // is -- shift lock, a custom Humanoid.CameraOffset, first person, a
        // game that moves its own camera -- it is already the difference
        // between two positions that are both read live.
        //
        // Pitch never comes from the character. Our root sits at hip height and
        // so does theirs, so a look vector drawn between them is nearly flat;
        // taking pitch from there would yank the view to horizontal however
        // good the yaw is.
        //
        // What is left over is that the target sits slightly LEFT of the
        // crosshair. That is not an error to fix: it is what being correctly
        // aimed from a point that is not your eye looks like.
        float3 yaw_origin = eye;
        if (AIM_FROM_CHARACTER) {
            float3 self;
            uint64_t mine0 = fx_character();
            uint64_t root0 = mine0 ? aim_part_of(mine0) : 0;
            if (root0 && part_pos(root0, &self)) yaw_origin = self;
        }

        float3 h = sub(tp, yaw_origin);
        h.y = 0.0f;
        if (dot(h, h) < 1e-6f) break;          // directly above or below us
        h = norm(h);

        float tan_pitch;
        if (VERTICAL) {
            float3 rel  = sub(tp, eye);
            float  flat = sqrtf(rel.x*rel.x + rel.z*rel.z);
            tan_pitch = (flat > 1e-3f) ? (rel.y / flat) : 0.0f;
        } else {
            // Read the pitch back off the CURRENT basis and put it straight
            // back, so the write contributes nothing at all to the vertical
            // axis and looking up or down stays entirely yours.
            float3 fwd  = float3{ -R[2], -R[5], -R[8] };
            float  flat = sqrtf(fwd.x*fwd.x + fwd.z*fwd.z);
            // Straight up or down: no horizontal component left to hold a yaw
            // against. Refuse rather than substitute a pitch of our own --
            // flattening the view to the horizon is exactly what this branch
            // exists to avoid, and it would do it at the worst moment.
            if (flat <= 1e-4f) break;
            tan_pitch = fwd.y / flat;
        }

        // h is a unit HORIZONTAL vector, so raising it by tan(pitch) and
        // renormalising gives exactly that pitch without a trig call.
        float3 look  = norm(float3{ h.x, tan_pitch, h.z });
        float3 back  = float3{ -look.x, -look.y, -look.z };
        float3 up0   = float3{ 0, 1, 0 };
        float3 right = cross(up0, back);
        if (dot(right, right) < 1e-6f) break;   // straight up or down
        right = norm(right);
        float3 up = cross(back, right);

        float want[9];
        want[0] = right.x; want[3] = right.y; want[6] = right.z;
        want[1] = up.x;    want[4] = up.y;    want[7] = up.z;
        want[2] = back.x;  want[5] = back.y;  want[8] = back.z;

        if (SMOOTHING < 1.0f) {
            for (int i = 0; i < 9; i++) want[i] = R[i] + (want[i] - R[i]) * SMOOTHING;
            // Blending two rotations componentwise does NOT stay orthonormal, so
            // the other two axes are re-derived from the blended back vector.
            // Skipping this shears the view as it turns.
            float3 b2 = norm(float3{ want[2], want[5], want[8] });
            float3 r2 = cross(up0, b2);
            if (dot(r2, r2) < 1e-6f) break;
            r2 = norm(r2);
            float3 u2 = cross(b2, r2);
            want[0] = r2.x; want[3] = r2.y; want[6] = r2.z;
            want[1] = u2.x; want[4] = u2.y; want[7] = u2.z;
            want[2] = b2.x; want[5] = b2.y; want[8] = b2.z;
        }
        if (!orthonormal(want)) break;

        bool moved = false;
        for (int i = 0; i < 9 && !moved; i++)
            if (fabsf(want[i] - R[i]) > DEADZONE) moved = true;
        if (moved) fx_wrblk(cam + oCamR, want, 36);

        // The body follows where the camera is being PUT, not where it was, so
        // the two cannot disagree by a tick.
        memcpy(body, want, sizeof body);
        } while (0);

        // --- and take the body with it -----------------------------------------
        // Written every tick regardless of the camera's deadzone: the engine
        // rights the character continuously, so this is holding a position
        // rather than reaching one, and skipping it when the camera settles
        // would just stand you back up.
        //
        // Resolved fresh each tick for the same reason the target is: our own
        // character dies on respawn too.
        //
        // Re-evaluated rather than reusing body_now from the top of the tick:
        // a failed acquisition clears `on` inside the block above, and the
        // stale flag would tip the body for exactly one frame on a lock that
        // never actually happened.
        if (ragdoll || (on && BODY_WITH_LOCK)) {
            // Flatten to the yaw the camera has, keeping the character upright.
            //
            // Done by rebuilding the basis from the back vector with its Y
            // dropped, NOT by zeroing components in place: a matrix with parts
            // of a rotation removed is not a rotation, and the physics engine
            // is entitled to do anything with a malformed CFrame. This is
            // exactly the shape a root part normally holds -- compare the
            // measured pair at the top of this file.
            if (!VERTICAL) {
                float3 b = float3{ body[2], body[5], body[8] };
                b.y = 0.0f;
                if (dot(b, b) < 1e-6f) {
                    // Looking straight up or down: no yaw left to keep. Leave
                    // the body alone this tick rather than inventing a facing.
                    fx_sleep(SLEEP_MS);
                    continue;
                }
                b = norm(b);
                float3 up0 = float3{ 0, 1, 0 };
                float3 r = norm(cross(up0, b));
                float3 u = cross(b, r);            // exactly world up, by construction
                body[0] = r.x; body[3] = r.y; body[6] = r.z;
                body[1] = u.x; body[4] = u.y; body[7] = u.z;
                body[2] = b.x; body[5] = b.y; body[8] = b.z;
            }

            if (orthonormal(body)) {
                uint64_t mine = fx_character();
                uint64_t root = mine ? aim_part_of(mine) : 0;
                uint64_t prim = primitive_of(root);
                if (prim) fx_wrblk(prim + oPrimR, body, 36);
            }
        }

        fx_sleep(SLEEP_MS);
    }

    lg("ragdoll lock: stopped.");
}
