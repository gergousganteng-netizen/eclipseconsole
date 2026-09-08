// cache.cpp -- the LocalPlayer/Character cache and the memoised child lookup,
// split out of eclipse_console.cpp. It OWNS its caches and the part-cache mutex
// (g_fxcache_mux moved here with g_fxparts); world->screen, which used to share
// that one mutex incidentally for its own disjoint view-matrix cache, was given
// its own mutex in the process so this file's lock could become private. The
// dense invalidation reasoning travels verbatim.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "fx.h"
#include "eclipse_core.h"   // ec::state().dm
#include "offsets.h"        // off_of

// The offset "not found" sentinel, value-identical to off_of's (matches the
// other TUs, which keep their own copy).
static const uint64_t BAD = (uint64_t)-1;

// ---- LocalPlayer / Character cache -----------------------------------------
// Five snippets were each hand-walking Players -> LocalPlayer -> Character, and
// each one got to rediscover the same two traps: fx_service walks every service
// in the DataModel comparing class names, so calling it in a hot loop is very
// expensive; and a Character pointer goes stale on every respawn, taking any
// pointer derived from it with it.
//
// So the walk lives here once. The expensive half (fx_service) happens on the
// first call and only again if the DataModel changes underneath us. The cheap
// half is one read, throttled to roughly a frame, which is what makes it safe to
// call from a 10ms loop.
static uint64_t g_c_dm      = 0;   // the DataModel this cache was built against
static uint64_t g_c_players = 0;
static uint64_t g_c_lp      = 0;
static uint64_t g_c_char    = 0;
static unsigned g_c_gen     = 0;   // ticks whenever the Character changes
static DWORD    g_c_last    = 0;
static std::atomic<unsigned> g_dm_gen{0};   // ticks whenever the DataModel is replaced

void char_cache_reset(void) {
    g_c_dm = g_c_players = g_c_lp = g_c_char = 0;
    g_c_last = 0;
    // The generation deliberately keeps counting rather than resetting to 0. A
    // snippet holding an old value must see a DIFFERENT number, and zeroing here
    // could hand it a coincidental match -- which is precisely the "my pointers
    // are still good" bug this exists to prevent.
    g_c_gen++;
}

static void char_refresh(void) {
    uint64_t dm = ec::state().dm;
    if (!dm) {
        if (g_c_char) { g_c_char = 0; g_c_gen++; }
        return;
    }
    if (dm != g_c_dm) {                 // rejoined: everything below is stale
        g_c_dm = dm;
        g_c_players = g_c_lp = 0;
    }

    DWORD now = GetTickCount();
    if (g_c_last && (now - g_c_last) < 16) return;    // ~one frame
    g_c_last = now;

    if (!g_c_players) g_c_players = fx_service("Players");
    if (!g_c_players) return;

    if (!g_c_lp) {
        uint64_t o = off_of("Player.LocalPlayer");
        if (o == BAD) return;
        g_c_lp = fx_rd(g_c_players + o);
        if (!g_c_lp) return;
    }

    uint64_t o = off_of("Player.ModelInstance");
    if (o == BAD) return;
    uint64_t c = fx_rd(g_c_lp + o);
    if (c != g_c_char) { g_c_char = c; g_c_gen++; }

    // No character for a moment is normal (dead, or still loading). But if the
    // Player object itself was rebuilt, re-reading it costs one read, so drop it
    // and let the next call find out.
    if (!c) g_c_lp = 0;
}

// Ticks whenever the DataModel is replaced -- joining a place, leaving one,
// teleporting between them. A level above fx_gen: a respawn changes the
// character, but leaving invalidates EVERYTHING, the character cache included.
//
//     static unsigned dm = 0;
//     if (fx_dm_gen() != dm) { dm = fx_dm_gen(); /* drop every cached pointer */ }
//
// Like fx_gen it never resets, so a stale holder cannot match by accident.
FX_API unsigned fx_dm_gen(void) { return g_dm_gen; }

FX_API uint64_t fx_player(void)    { char_refresh(); return g_c_lp; }
FX_API uint64_t fx_character(void) { char_refresh(); return g_c_char; }
FX_API unsigned fx_gen(void)  { char_refresh(); return g_c_gen; }

// ---- memoised child lookup ---------------------------------------------------
//
// fx_find with a memory. Same answer, same failure mode; the walk happens once
// per model instead of once per call.
//
// Worth an export rather than an inline because of what fx_find costs: it is a
// linear scan that reads every sibling's NAME, and a name is a deref, the
// NameContainer hop, then the string. Measured at ~125 us against a 20-part
// rig, so a snippet asking for two parts on twenty players spends about 5 ms a
// tick re-answering a question whose answer cannot change -- a part's
// name-to-pointer mapping is fixed for the life of the model. Caching it
// measured 57x on the repeat lookups and took one app's tick body from 4.0 ms
// to 0.28 ms.
//
// The cache lives HERE because correct invalidation needs fx_gen, and only the
// host maintains that. Two triggers:
//   - the generation moves (respawn, teleport, rejoin): all of it is stale
//   - the model's child count changes: it was rebuilt, and an address CAN be
//     handed to a different model afterwards
//
// The count check is rate-limited to once per millisecond per model rather than
// run per lookup. Per lookup it would be two extra reads on every call, which
// at a few hundred calls a tick costs more than it saves.
//
// MISSES ARE CACHED TOO, and that is not an optimisation detail: a probe that
// asks "is there a part called X" against a model that does not have one is the
// most expensive kind of call, because it walks every child before failing.
namespace {

struct FxPartCache {
    int      nchildren = -1;
    DWORD    checked   = 0;
    unsigned gen       = 0;
    std::unordered_map<std::string, uint64_t> byName;
};

std::mutex g_fxcache_mux;
std::unordered_map<uint64_t, FxPartCache> g_fxparts;

}  // namespace

FX_API uint64_t fx_part(uint64_t model, const char* name) {
    if (!model || !name || !*name) return 0;

    unsigned gen = fx_gen();
    DWORD    now = GetTickCount();

    std::lock_guard<std::mutex> lk(g_fxcache_mux);

    // Bounded rather than reference-counted. Models come and go every round,
    // and one slow tick beats tracking liveness forever.
    if (g_fxparts.size() > 256) g_fxparts.clear();

    FxPartCache& mp = g_fxparts[model];
    if (mp.gen != gen) { mp.byName.clear(); mp.nchildren = -1; mp.gen = gen; }
    if (mp.checked != now) {
        mp.checked = now;
        int n = (int)fx_nchildren(model);
        if (n != mp.nchildren) { mp.byName.clear(); mp.nchildren = n; }
    }

    auto it = mp.byName.find(name);
    if (it != mp.byName.end()) {
        uint64_t hit = it->second;
        // Confirm the part still belongs to this model before handing it back.
        //
        // The two invalidations above are not enough on their own, and the gap
        // is easy to miss: fx_gen tracks OUR character, not anybody else's.
        // When another player respawns their old model is freed, a new one may
        // land on the same address, and a rebuilt character usually has the
        // SAME child count -- so both checks pass and the cache serves pointers
        // to freed parts. Reads through those fail or return the position they
        // died at, which is indistinguishable from a target standing still.
        //
        // One extra read per hit closes it, against the ~60 a name walk costs.
        // Misses are exempt: a cached 0 has no pointer to validate.
        if (!hit) return 0;
        uint64_t po = fx_off("Instance.Parent");
        if (po == (uint64_t)-1 || fx_rd(hit + po) == model) return hit;
        mp.byName.erase(it);          // stale: fall through and re-resolve
    }

    uint64_t found = fx_find(model, name);
    mp.byName[name] = found;
    return found;
}

// ---- fx_players: the roster --------------------------------------------------
//
// Lives here for the same reason fx_part does: it is a cache, and correct
// invalidation needs the generations this TU owns.
//
// WHAT IS CACHED AND WHAT IS NOT, which is the whole design:
//   - the Players service pointer. fx_service walks every service comparing
//     class names, so it must not run per call. Dropped on a DataModel change.
//   - a player's NAME and UserId. Both fixed for the life of the Player
//     object, and a name is three dereferences (NameContainer, then the
//     string), so it is by far the most expensive field here.
//   - the Humanoid, keyed on the model it was found under. A respawn hands the
//     Player a new model, and comparing against the remembered one is how that
//     is noticed without a class walk per tick.
// Everything volatile -- model, health, root, team -- is read FRESH every
// call. The point of the roster is to be safe to poll, and a cached health is
// a wrong health.
//
// The per-player map is cleared wholesale on a DataModel change and bounded,
// same argument as g_fxparts: players come and go, and one slow tick beats
// tracking liveness forever.
namespace {

struct RosterEntry {
    std::string name;
    int64_t     userid   = 0;
    uint64_t    model    = 0;   // the model `humanoid` was resolved against
    uint64_t    humanoid = 0;
};

std::mutex g_roster_mux;
std::unordered_map<uint64_t, RosterEntry> g_roster;
uint64_t g_roster_players = 0;    // the Players service, per DataModel
unsigned g_roster_dm      = 0;    // the fx_dm_gen it was resolved under

// The Humanoid under a model. Named lookup first because it is memoised and
// stock rigs all call it "Humanoid" -- but the CLASS is what decides, since a
// game is free to name any instance that, and free to rename the real one.
uint64_t humanoid_of(uint64_t model) {
    if (!model) return 0;
    uint64_t h = fx_part(model, "Humanoid");
    if (h && strcmp(fx_class(h), "Humanoid") == 0) return h;
    uint64_t n = fx_nchildren(model);
    for (uint64_t i = 0; i < n; i++) {
        uint64_t c = fx_child(model, (int)i);
        if (c && strcmp(fx_class(c), "Humanoid") == 0) return c;
    }
    return 0;
}

}  // namespace

FX_API int fx_players(fx_player_info* out, int max) {
    if (max < 0) max = 0;
    if (!ec::state().dm) return 0;

    // Hoisted: every one of these is a string lookup in the offset table, and
    // the loop below would otherwise repeat all six per player.
    const uint64_t oModel = off_of("Player.ModelInstance");
    const uint64_t oUser  = off_of("Player.UserId");
    const uint64_t oTeam  = off_of("Player.Team");
    const uint64_t oHp    = off_of("Humanoid.Health");
    const uint64_t oMaxHp = off_of("Humanoid.MaxHealth");
    const uint64_t oRoot  = off_of("Humanoid.HumanoidRootPart");

    const uint64_t me = fx_player();

    std::lock_guard<std::mutex> lk(g_roster_mux);

    unsigned dgen = fx_dm_gen();
    if (dgen != g_roster_dm) {
        g_roster.clear();
        g_roster_players = 0;
        g_roster_dm = dgen;
    }
    if (g_roster.size() > 256) g_roster.clear();
    if (!g_roster_players) g_roster_players = fx_service("Players");
    if (!g_roster_players) return 0;

    int found = 0;
    uint64_t n = fx_nchildren(g_roster_players);
    for (uint64_t i = 0; i < n; i++) {
        uint64_t p = fx_child(g_roster_players, (int)i);
        if (!p) continue;
        // A game can parent anything under Players, so position is not proof.
        if (strcmp(fx_class(p), "Player") != 0) continue;

        int slot = found++;
        if (slot >= max || !out) continue;   // counting pass, or past the buffer

        RosterEntry& r = g_roster[p];
        if (r.name.empty()) {
            const char* nm = fx_name(p);
            r.name = nm ? nm : "";
        }
        // MEMOISED SEPARATELY FROM THE NAME, because they are separate reads
        // that fail separately. Latched together, a UserId that read 0 beside
        // a perfectly good name stayed 0 for the whole run -- the read is
        // never retried once the name is in, and nothing looks wrong: the
        // player is present and correctly named, just permanently id-less.
        // Retried while it is still 0 instead; one read, and only until it
        // lands. (Found by fx_avatar_url resolving nothing at all.)
        if (r.userid == 0 && oUser != BAD) r.userid = (int64_t)fx_rd(p + oUser);

        fx_player_info e{};
        e.player = p;
        e.userid = r.userid;
        // Truncate rather than refuse: a name longer than the buffer is still
        // more useful identified by its first 63 characters than dropped.
        size_t cn = r.name.size() < sizeof e.name - 1 ? r.name.size() : sizeof e.name - 1;
        memcpy(e.name, r.name.data(), cn);
        e.name[cn] = 0;

        e.model = oModel != BAD ? fx_rd(p + oModel) : 0;
        if (e.model != r.model) {           // respawned, or spawned for the first time
            r.model    = e.model;
            r.humanoid = humanoid_of(e.model);
        }
        e.humanoid = r.humanoid;

        if (e.humanoid) {
            if (oHp    != BAD) e.health    = fx_rf(e.humanoid + oHp);
            if (oMaxHp != BAD) e.maxhealth = fx_rf(e.humanoid + oMaxHp);
            if (oRoot  != BAD) e.root      = fx_rd(e.humanoid + oRoot);
        }
        // Humanoid.HumanoidRootPart is empty on some rigs; the named lookup is
        // memoised, so the fallback costs one map hit after the first miss.
        if (!fx_ptr_ok(e.root) && e.model) e.root = fx_part(e.model, "HumanoidRootPart");
        if (!fx_ptr_ok(e.root)) e.root = 0;

        e.team     = oTeam != BAD ? fx_rd(p + oTeam) : 0;
        e.is_local = (p == me) ? 1 : 0;

        out[slot] = e;
    }
    return found;
}

// ---- host-side write entry points (see cache.h) ----------------------------
// New, not moved: the two deliberate bumps refresh_dm makes now cross a TU
// boundary, so they go through functions rather than touching the counters.
void char_cache_bump_gen(void) { g_c_gen++; }
void dm_gen_bump(void)         { g_dm_gen++; }
