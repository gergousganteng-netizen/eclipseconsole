#include "lock.h"

// Where on a character to aim. HumanoidRootPart first, NOT the head: a lock is
// held for seconds at a time, and the head is the part that moves most -- idle
// and run animations swing it around while the torso stays where the character
// actually is. Aiming at the head makes a stationary target look jittery.
static const char* kAimPart[] = { "HumanoidRootPart", "UpperTorso", "Torso", "Head" };

// Nothing moves faster than this on its own feet. A velocity above it is not a
// fast player, it is a read that landed on something else -- and unlike a bad
// position, which throws the view somewhere obvious, a bad velocity throws it
// somewhere obvious MULTIPLIED by the lead time.
static const float kMaxSpeed = 500.0f;

static bool sane3(float3 v, float limit) {
    if (!(v.x == v.x && v.y == v.y && v.z == v.z)) return false;   // NaN
    return !(v.x >  limit || v.x < -limit || v.y >  limit ||
             v.y < -limit || v.z >  limit || v.z < -limit);
}

// `vel` is optional. Both fields hang off the same Primitive, so reading them
// together costs one pointer resolution instead of two -- and, more to the
// point, guarantees the position and the velocity describe the same instant.
static bool part_motion(uint64_t part, float3* pos, float3* vel) {
    uint64_t oPrim = fx_off("BasePart.Primitive");
    uint64_t oPos  = fx_off("Primitive.Position");
    if (oPrim == (uint64_t)-1 || oPos == (uint64_t)-1) return false;
    uint64_t prim = fx_rd(part + oPrim);
    if (prim < 0x10000 || prim > 0x7ff000000000ULL) return false;

    // Reading a part mid-rebuild returns whatever was in those twelve bytes.
    // It matters more here than in the aimbot: this position is fed straight
    // into a rotation the camera is then set to, so garbage does not just show
    // a silly number, it throws the view somewhere random.
    float3 p = fx_rf3(prim + oPos);
    if (!sane3(p, 1.0e6f)) return false;
    *pos = p;

    if (vel) {
        *vel = float3{ 0, 0, 0 };
        uint64_t oVel = fx_off("Primitive.AssemblyLinearVelocity");
        if (oVel != (uint64_t)-1) {
            float3 v = fx_rf3(prim + oVel);
            // Zeroed rather than refused: a target whose velocity cannot be
            // trusted is still worth tracking, just without the lead.
            if (sane3(v, kMaxSpeed)) *vel = v;
        }
    }
    return true;
}

static bool part_position(uint64_t part, float3* out) {
    return part_motion(part, out, nullptr);
}

static uint64_t aim_part_of(uint64_t chr) {
    for (unsigned i = 0; i < sizeof kAimPart / sizeof *kAimPart; i++) {
        uint64_t p = fx_part(chr, kAimPart[i]);
        if (p) return p;
    }
    return 0;
}

// The locked target, re-resolved from scratch every tick.
//
// Nothing derived from the Player is cached between ticks, and that is the
// point: the Character pointer dies on every respawn. Caching it is how a lock
// ends up following a model that was freed -- which reads as the target
// standing frozen at the spot where they died, because the memory still holds
// their last position until something else claims it.
bool aim_point(uint64_t player, float3* pos, float3* vel) {
    uint64_t oChr = fx_off("Player.ModelInstance");
    if (oChr == (uint64_t)-1) return false;

    uint64_t chr = fx_rd(player + oChr);
    if (!chr) return false;                   // dead, or still loading in

    uint64_t part = aim_part_of(chr);
    if (!part) return false;
    return part_motion(part, pos, vel);
}

// OUR aim origin. fx_character() rather than walking Players -> LocalPlayer:
// the host caches it and re-resolves it on the same generation counter the
// main loop already watches, so the two cannot disagree.
bool local_origin(float3* out) {
    uint64_t chr = fx_character();
    if (!chr) return false;
    uint64_t part = fx_part(chr, "HumanoidRootPart");
    if (!part) return false;
    return part_position(part, out);
}

int collect_targets(Target* out, int max) {
    // fx_players is the host's roster: ONE walk over Players with each player's
    // name and Character already resolved and memoised against the generation
    // counter, in place of resolving LocalPlayer, comparing every child against
    // it and reading every name here. Measured 6.3x cheaper on a 15-player
    // server (0.20 ms against 1.25 ms).
    //
    // The roster filters NOTHING -- unspawned and dead players come back with
    // zeros in the fields that could not be read, and the skips below are still
    // ours. It also hands back .player directly, which is the identity this
    // file is careful to key the lock on (see the Target comment in lock.h).
    fx_player_info pl[64];
    int np = fx_players(pl, 64);
    if (np <= 0) return 0;

    // Distances are from the CAMERA here, because this list is only used for
    // PICKING, and picking is done by eye. Once locked, the aim is measured
    // from the character instead -- see aim_from().
    float R[9], vp[2], fov;
    float3 eye = { 0, 0, 0 };
    camera_basis(R, &eye, &fov, vp);

    int n = 0;
    for (int i = 0; i < np && n < max; i++) {
        if (pl[i].is_local) continue;

        uint64_t chr = pl[i].model;
        if (!chr) continue;                       // not spawned

        uint64_t part = aim_part_of(chr);
        if (!part) continue;

        float3 p;
        if (!part_position(part, &p)) continue;

        out[n].player = pl[i].player;
        snprintf(out[n].name, sizeof out[n].name, "%s", pl[i].name);
        out[n].world    = p;
        float3 d        = v_sub(p, eye);
        out[n].dist     = sqrtf(v_dot(d, d));
        out[n].onscreen = project_to_screen(p, &out[n].sx, &out[n].sy);
        n++;
    }
    return n;
}
