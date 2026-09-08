#include "aim.h"

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

    // Reading a part mid-rebuild returns whatever was in those twelve bytes --
    // it showed up once as a player "47627 studs away" in a crowd standing 80.
    float3 p = fx_rf3(prim + oPos);
    if (!sane3(p, 1.0e6f)) return false;
    *pos = p;

    if (vel) {
        *vel = float3{ 0, 0, 0 };
        uint64_t oVel = fx_off("Primitive.AssemblyLinearVelocity");
        if (oVel != (uint64_t)-1) {
            float3 v = fx_rf3(prim + oVel);
            // Zeroed rather than refused: a target whose velocity cannot be
            // trusted is still worth aiming at, just without the lead.
            if (sane3(v, kMaxSpeed)) *vel = v;
        }
    }
    return true;
}

// OUR aim origin, for the shift lock correction in aim_from(). fx_character()
// rather than walking Players -> LocalPlayer: the host caches it and
// re-resolves it on its own generation counter.
bool local_origin(float3* out) {
    uint64_t chr = fx_character();
    if (!chr) return false;
    uint64_t part = fx_part(chr, "HumanoidRootPart");
    if (!part) return false;
    return part_motion(part, out, nullptr);
}

// ---- team check --------------------------------------------------------------
// Player.Team points at a Team instance, so "same team" is a pointer compare.
// Both pointers come from the roster now, so this reads nothing at all.
//
// DISABLED when our own Team is null, and that is the important part: in a game
// with no teams every Player.Team is 0, so a naive equality test makes everyone
// a teammate and the aim finds nothing at all. No team means no way to tell
// friend from foe, which means everyone is a target.
static bool same_team(uint64_t mine, uint64_t theirs) {
    if (!TEAM_CHECK || !mine) return false;   // no teams in this game
    return theirs == mine;
}

int collect_targets(Target* out, int max) {
    // fx_players is the host's roster: ONE walk over Players, with each
    // player's name, Character and Team already resolved and memoised against
    // the generation counter. It replaces the hand walk that used to live here
    // -- resolve LocalPlayer, compare every child against it, read every name,
    // read Player.Team twice per player -- and measured 6.3x cheaper on a
    // 15-player server (0.20 ms against 1.25 ms).
    //
    // NOTHING IS FILTERED OUT BY THE ROSTER. Dead players, unspawned ones and
    // ones whose Humanoid could not be read all come back with zeros, and the
    // skips below are still ours to make. That is the point: a roster that
    // quietly dropped what it could not decode would be indistinguishable from
    // an emptier server.
    fx_player_info pl[64];
    int np = fx_players(pl, 64);
    if (np <= 0) return 0;

    // Our own Team, for the friend check. Taken from our own roster entry
    // rather than re-resolving LocalPlayer.
    uint64_t my_team = 0;
    for (int i = 0; i < np; i++) if (pl[i].is_local) my_team = pl[i].team;

    // Distances are from the CAMERA, not from our character: the camera is what
    // is doing the aiming, and the two are metres apart in third person.
    float R[9], vp[2], fov;
    float3 eye = { 0, 0, 0 };
    camera_basis(R, &eye, &fov, vp);

    int n = 0;
    for (int i = 0; i < np && n < max; i++) {
        if (pl[i].is_local) continue;
        bool mate = same_team(my_team, pl[i].team);

        uint64_t chr = pl[i].model;
        if (!chr) continue;                       // not spawned

        uint64_t part = fx_part(chr, "Head");
        if (!part) part = fx_part(chr, "HumanoidRootPart");
        if (!part) continue;

        float3 p, v;
        if (!part_motion(part, &p, &v)) continue;

        // The ESP box is anchored on the ROOT PART, not the head, so it matches
        // esp_overlay.cpp exactly -- same part, same fallback order. The aim
        // still uses the head above; these are two different questions and
        // sharing one point was what made the two ESPs disagree.
        float3 rootp = p;                                  // fall back to head
        // pl[i].root IS fx_part(chr, "HumanoidRootPart") -- checked against it
        // 15/15 on a live server -- so the fallback chain below is unchanged,
        // just starting from a value already in hand.
        uint64_t root = pl[i].root;
        if (!root) root = fx_part(chr, "Torso");
        if (!root) root = fx_part(chr, "UpperTorso");
        if (root) {
            float3 rp, rv;
            if (part_motion(root, &rp, &rv)) rootp = rp;
        }

        out[n].team = mate;
        snprintf(out[n].name, sizeof out[n].name, "%s", pl[i].name);
        out[n].world    = p;
        out[n].hrp      = rootp;
        out[n].vel      = v;
        float3 d        = v_sub(p, eye);
        out[n].dist     = sqrtf(v_dot(d, d));
        out[n].onscreen = project_to_screen(p, &out[n].sx, &out[n].sy);
        n++;
    }
    return n;
}
