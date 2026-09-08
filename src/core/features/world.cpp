// world.cpp -- the geometry layer: world->screen projection, a part's world
// position (fx_pos), the BasePart classification (kPartClasses/in_list/fx_parts),
// and the hitbox/ray machinery (fx_geom/fx_visible/fx_raycast). Split out of
// eclipse_console.cpp once the read layer, the offset table and the caches were
// behind functions -- with those published, this section reads memory only
// through fx_rd/rd64/class_of and needs no core-private state at all. It owns
// its own view-matrix cache (g_view_mux, formerly sharing the part cache's
// mutex) and its own hitbox cache (g_geom_mux). Comments travel verbatim.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>       // sqrtf/fabsf for the ray/box geometry
#include <cstdint>
#include <cstring>     // strcmp, memcpy
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "fx.h"
#include "eclipse_core.h"
#include "read.h"       // rd64, class_of
#include "offsets.h"    // off_of

// The offset "not found" sentinel, value-identical to off_of's (matches the
// other TUs, which each keep their own copy).
static const uint64_t BAD = (uint64_t)-1;

// ---- world -> screen ----------------------------------------------------------
//
// One correct projection instead of the copy every snippet grew its own version
// of -- and they had already drifted: some bounds-checked the result against
// the viewport, some only checked the depth range, so an off-screen target
// passed in one and failed in the other. That difference was a real bug: an
// acquisition could lock onto someone who was not on screen.
//
// Uses Roblox's OWN world-to-clip matrix rather than rebuilding one from
// Camera.Rotation and FieldOfView. Authoritative: whatever the engine does to
// its projection -- a scripted FOV, a custom camera -- is already in it.
//
// Coordinates are CLIENT-AREA pixels, the same space the projection naturally
// produces. Add the Roblox window origin for monitor pixels; the overlay wants
// monitor pixels, so a snippet feeding fx_overlay_post has to add it.
//
// The matrix is read FRESH on every call, and that is not an oversight.
//
// It was cached for a millisecond at first, on the reasoning that a snippet
// projecting hundreds of points would otherwise pay a 64-byte read for each
// one. That reasoning was an estimate, never a measurement, and it broke the
// caller that matters most: a cursor aim is a CLOSED LOOP. It measures the
// error between the crosshair and the target, pushes the mouse, and measures
// again. Feed it a camera pose from up to a millisecond ago -- inconsistently,
// some ticks fresh and some not -- and the error it measures is wrong by
// however far the view turned in between. The correction overshoots, the next
// one overcorrects, and the aim visibly flickers. An ESP never notices; an aim
// loop cannot survive it.
//
// So: correctness first, and the cost is measured rather than assumed. If a
// caller ever genuinely needs hundreds of projections a tick, the answer is a
// batch call, not a stale matrix.
namespace {

float    g_vm[16] = {0};
float    g_vdim[2] = {0, 0};
bool     g_vm_ok  = false;
// world->screen's OWN mutex for the view-matrix cache above. It used to share
// the part cache's g_fxcache_mux; that mutex moved to cache.cpp with the part
// cache, so this disjoint state got its own lock (behaviour is unchanged --
// the two caches never touched each other under the shared lock).
std::mutex g_view_mux;

// Refresh under the caller's lock.
void view_refresh(DWORD) {
    g_vm_ok = false;

    uint64_t po = fx_off("VisualEngine.Pointer");
    uint64_t mo = fx_off("VisualEngine.ViewMatrix");
    uint64_t dp = fx_off("VisualEngine.Dimensions");
    if (po == (uint64_t)-1 || mo == (uint64_t)-1 || dp == (uint64_t)-1) return;

    uint64_t ve = fx_rd(fx_base() + po);
    if (!ve) return;
    if (fx_rdblk(ve + mo, g_vm, 64) != 64) return;
    if (fx_rdblk(ve + dp, g_vdim, 8) != 8) return;
    if (!(g_vdim[0] >= 1.0f && g_vdim[1] >= 1.0f)) return;
    g_vm_ok = true;
}

}  // namespace

FX_API int fx_screen(float3 w, float* sx, float* sy) {
    if (!sx || !sy) return 0;
    DWORD now = GetTickCount();

    float m[16], dx, dy;
    {
        std::lock_guard<std::mutex> lk(g_view_mux);
        view_refresh(now);
        if (!g_vm_ok) return 0;
        memcpy(m, g_vm, sizeof m);
        dx = g_vdim[0];
        dy = g_vdim[1];
    }

    float cx = m[0]*w.x  + m[1]*w.y  + m[2]*w.z  + m[3];
    float cy = m[4]*w.x  + m[5]*w.y  + m[6]*w.z  + m[7];
    float cz = m[8]*w.x  + m[9]*w.y  + m[10]*w.z + m[11];
    float cw = m[12]*w.x + m[13]*w.y + m[14]*w.z + m[15];
    if (cw <= 1e-4f) return 0;                       // behind the camera

    float inv = 1.0f / cw;
    float ndz = cz * inv;
    if (ndz < 0.0f || ndz > 1.0f) return 0;          // outside the depth range

    float x = (cx * inv + 1.0f) * 0.5f * dx;
    float y = (1.0f - cy * inv) * 0.5f * dy;
    *sx = x;
    *sy = y;

    // Three outcomes, not two, because the callers genuinely want different
    // things and the copies of this that drifted apart drifted exactly here.
    //
    // An ESP wants "is it visible" and must reject the off-frame ones. A cursor
    // aim wants "where is it" and must NOT: in a mouse-locked game a target
    // just off the edge is still reachable by turning, and dropping it there is
    // how a lock gets lost mid-turn. Returning a bare false for both cases
    // forces one of them to be wrong.
    return (x < 0.0f || y < 0.0f || x > dx || y > dy) ? -1 : 1;
}

// The viewport fx_screen divides by, for a caller that needs to know where the
// centre is or how big the picture is. Same source as the projection, so the
// two cannot disagree.
FX_API int fx_viewport(float* w, float* h) {
    if (!w || !h) return 0;
    std::lock_guard<std::mutex> lk(g_view_mux);
    view_refresh(GetTickCount());
    if (!g_vm_ok) return 0;
    *w = g_vdim[0];
    *h = g_vdim[1];
    return 1;
}

// What counts as a BasePart. This list is the whole point of fx_parts: reading
// BasePart.Primitive on anything else returns whatever happens to sit at that
// offset, and it looks pointer-shaped -- a probe found Script handing back a
// module address, Shirt handing back the float 1.0, and Humanoid handing back
// plain garbage. Writing through one of those corrupts the client. Nobody should
// have to rediscover that.
static const char* kPartClasses[] = {
    "Part", "MeshPart", "UnionOperation", "TrussPart", "WedgePart",
    "CornerWedgePart", "Seat", "VehicleSeat", "SpawnLocation", "NegateOperation"
};
// A hat or tool welded to you collides too, so step one level into these.
static const char* kContainerClasses[] = { "Accessory", "Model", "Tool", "Folder" };

static bool in_list(const char* s, const char* const* list, size_t n) {
    for (size_t i = 0; i < n; i++) if (strcmp(s, list[i]) == 0) return true;
    return false;
}

FX_API int fx_parts(uint64_t* out, int max) {
    if (!out || max <= 0) return 0;
    uint64_t chr = fx_character();
    if (!chr) return 0;

    const size_t NP = sizeof kPartClasses / sizeof kPartClasses[0];
    const size_t NC = sizeof kContainerClasses / sizeof kContainerClasses[0];

    int k = 0;
    uint64_t n = fx_nchildren(chr);
    for (uint64_t i = 0; i < n && k < max; i++) {
        uint64_t c = fx_child(chr, (int)i);
        if (!c) continue;

        // fx_class hands back a pointer into a buffer it reuses on the next
        // call, so take a copy before descending.
        std::string cls = fx_class(c);

        if (in_list(cls.c_str(), kPartClasses, NP)) { out[k++] = c; continue; }

        if (in_list(cls.c_str(), kContainerClasses, NC)) {
            uint64_t m = fx_nchildren(c);
            for (uint64_t j = 0; j < m && k < max; j++) {
                uint64_t g = fx_child(c, (int)j);
                if (!g) continue;
                std::string gcls = fx_class(g);
                if (in_list(gcls.c_str(), kPartClasses, NP)) out[k++] = g;
            }
        }
    }
    return k;
}

// ---- fx_pos: a part's world position, class-checked -------------------------
//
// The same three lines nine snippets had each written for themselves, and four
// of those had dropped the class check -- including noclip.cpp, which WRITES
// through the primitive pointer it derives. Reading BasePart.Primitive off
// something that is not a BasePart hands back a pointer-shaped value (see
// kPartClasses above), so the ones that only read got a wrong number and the
// one that writes was one bad instance away from corrupting the client.
//
// Returns 0 rather than a zero vector when it cannot answer, because "the part
// is at the origin" and "I could not read it" must not look the same -- a
// filter that confuses them is how a whole lobby gets discarded as dead.
// Is this pointer a BasePart? MEMOISED, because the answer cannot change for a
// live pointer and the question is expensive: class_of hops to the class
// descriptor and then reads the class NAME STRING, and it dominated this
// function -- measured against a live client, fx_pos was 5.98us of which
// fx_class was 3.59us (60%). An aim or ESP loop asks it about the same parts
// every tick, so nearly all of that was re-reading a string that had not
// changed. With this it lands near the two-read floor.
//
// Keyed on fx_gen, exactly like fx_part's cache: the generation moves whenever
// anything cached may have been freed, and it never resets, so a stale entry
// cannot match by accident. thread_local rather than mutexed -- a snippet's
// loop runs on its own thread, and the point of this is to be cheaper than the
// read it replaces, which a lock contended with the host would not be.
//
// Bounded and cleared wholesale on a generation change, for the same reason
// fx_part's is: liveness tracking for parts that come and go every round costs
// more than one slow tick.
static bool is_basepart_cached(uint64_t part) {
    struct Ent { uint64_t part; bool ok; };
    static thread_local Ent  tab[512];
    static thread_local unsigned tab_gen = 0;

    unsigned gen = fx_gen();
    if (gen != tab_gen) { tab_gen = gen; memset(tab, 0, sizeof tab); }

    Ent& e = tab[(part >> 4) & 511];      // parts are 16-byte aligned
    if (e.part == part) return e.ok;

    const char* cls = fx_class(part);
    bool ok = cls && in_list(cls, kPartClasses,
                             sizeof kPartClasses / sizeof kPartClasses[0]);
    e.part = part;
    e.ok   = ok;
    return ok;
}

FX_API int fx_pos(uint64_t part, float3* out) {
    if (!part || !out) return 0;
    if (!is_basepart_cached(part)) return 0;

    uint64_t op = fx_off("BasePart.Primitive"), opos = fx_off("Primitive.Position");
    if (op == BAD || opos == BAD) return 0;

    uint64_t prim = rd64(part + op);
    if (prim < 0x10000 || prim > 0x7ff000000000ULL) return 0;

    float3 v{};
    if (fx_rdblk(prim + opos, &v, 12) != 12) return 0;
    if (!(v.x == v.x && v.y == v.y && v.z == v.z)) return 0;   // NaN
    *out = v;
    return 1;
}

// ---- world geometry, and casting a ray at it --------------------------------
//
// WHAT THE CACHE HOLDS: every BasePart reachable from Workspace, as an oriented
// box, EXCLUDING characters -- any Model with a Humanoid in it is skipped whole.
// That definition is deliberate and it is what both callers want. Occlusion
// means "is a wall in the way", not "is another player's elbow in the way", and
// a teleport destination on top of somebody's head is not a destination.
//
// WHEN IT IS BUILT: lazily, on first use, and then only when the DataModel
// changes or a caller asks. NOT on fx_gen -- that ticks every time you respawn,
// and rebuilding a 60k-part index on every death would cost more than every
// query it serves. The consequence is honest and worth knowing: parts that MOVE
// go stale. Static map geometry, which is what occlusion and teleport care
// about, does not.
//
// NO BROADPHASE, on purpose, for now. A flat test over the whole cache is
// simple and obviously correct; a grid is the kind of machinery this codebase
// has been bitten by adding before it was measured. fx_geom() reports the part
// count and the last build time so the decision can be made on a number. If a
// caller ever wants many rays per tick and the flat test is too slow, that
// measurement is the argument for adding one.
namespace {

struct GBox {
    uint64_t part;
    float3   pos;
    float3   half;
    float    rot[9];      // columns are the box's local axes, as CFrame stores them
};

std::mutex         g_geom_mux;
std::vector<GBox>  g_geom;
uint64_t           g_geom_dm    = 0;
bool               g_geom_ok    = false;
bool               g_geom_trunc = false;
DWORD              g_geom_ms    = 0;

const int GEOM_MAX_PARTS = 150000;
const int GEOM_MAX_DEPTH = 32;

// Class lookup keyed on the CLASS DESCRIPTOR pointer rather than the name.
// fx_class costs a descriptor read plus a string read per instance; over ~185k
// instances that is the difference between an index that takes seconds and one
// that takes tens of them. Every instance of a class shares one descriptor, so
// the string is read once per class and the rest is a pointer compare.
struct ClsInfo { bool part; bool humanoid; };

bool geom_class(uint64_t inst, uint64_t oDesc,
                std::unordered_map<uint64_t, ClsInfo>& seen, ClsInfo* out) {
    uint64_t d = rd64(inst + oDesc);
    if (!d) return false;
    auto it = seen.find(d);
    if (it != seen.end()) { *out = it->second; return true; }

    const char* cls = fx_class(inst);
    ClsInfo ci{ false, false };
    if (cls) {
        ci.part = in_list(cls, kPartClasses,
                          sizeof kPartClasses / sizeof kPartClasses[0]);
        ci.humanoid = (strcmp(cls, "Humanoid") == 0);
    }
    seen[d] = ci;
    *out = ci;
    return true;
}

// Build under the caller's lock.
void geom_build_locked() {
    DWORD t0 = GetTickCount();
    g_geom.clear();
    g_geom_ok = false;
    g_geom_trunc = false;

    uint64_t ws = fx_service("Workspace");
    if (!ws) return;

    uint64_t oDesc = fx_off("Instance.ClassDescriptor");
    uint64_t oCS   = fx_off("Instance.ChildrenStart");
    uint64_t oCE   = fx_off("Instance.ChildrenEnd");
    uint64_t oPrim = fx_off("BasePart.Primitive");
    uint64_t oRot  = fx_off("Primitive.Rotation");
    uint64_t oPos  = fx_off("Primitive.Position");
    uint64_t oSize = fx_off("Primitive.Size");
    if (oDesc == BAD || oCS == BAD || oCE == BAD || oPrim == BAD ||
        oRot == BAD || oPos == BAD || oSize == BAD) return;

    // One read per part covering rotation, position and size together instead of
    // three. They are one CFrame plus a size in the engine and sit within a few
    // hundred bytes of each other, so the span is small.
    uint64_t lo = oRot < oPos ? oRot : oPos;  if (oSize < lo) lo = oSize;
    uint64_t hi = oRot + 36;
    if (oPos  + 12 > hi) hi = oPos + 12;
    if (oSize + 12 > hi) hi = oSize + 12;
    size_t span = (size_t)(hi - lo);
    if (span > 0x800) return;                    // conf is not shaped as expected

    std::unordered_map<uint64_t, ClsInfo> seen;
    std::vector<uint64_t> cur, next, kids;
    std::vector<unsigned char> raw, blk(span);
    cur.push_back(ws);

    for (int depth = 0; depth < GEOM_MAX_DEPTH && !cur.empty(); depth++) {
        next.clear();
        for (uint64_t p : cur) {
            // Children in ONE read. fx_child costs a read each, and at ~185k
            // instances that alone dominated the build.
            uint64_t cc = rd64(p + oCS);
            if (!cc) continue;
            uint64_t begin = rd64(cc), end = rd64(cc + oCE);
            if (!begin || end <= begin) continue;
            uint64_t n = (end - begin) / 0x10;
            if (n > 4096) n = 4096;              // game-derived count; bound it
            raw.resize((size_t)n * 0x10);
            if (fx_rdblk(begin, raw.data(), raw.size()) != raw.size()) continue;

            kids.clear();
            bool is_character = false;
            for (uint64_t i = 0; i < n; i++) {
                uint64_t c = 0;
                memcpy(&c, raw.data() + (size_t)i * 0x10, 8);
                if (c < 0x10000 || c > 0x7fffffffffffULL) continue;
                kids.push_back(c);
            }

            // Classify first, act second: a Humanoid anywhere in this child list
            // means p is somebody's character, and none of its parts belong in a
            // cache of world geometry.
            std::vector<ClsInfo> info(kids.size());
            for (size_t i = 0; i < kids.size(); i++) {
                ClsInfo ci{ false, false };
                geom_class(kids[i], oDesc, seen, &ci);
                info[i] = ci;
                if (ci.humanoid) is_character = true;
            }
            if (is_character) continue;          // skip the whole model

            for (size_t i = 0; i < kids.size(); i++) {
                if (depth + 1 < GEOM_MAX_DEPTH) next.push_back(kids[i]);
                if (!info[i].part) continue;
                if ((int)g_geom.size() >= GEOM_MAX_PARTS) { g_geom_trunc = true; break; }

                uint64_t prim = rd64(kids[i] + oPrim);
                if (prim < 0x10000 || prim > 0x7ff000000000ULL) continue;
                if (fx_rdblk(prim + lo, blk.data(), span) != span) continue;

                GBox b;
                b.part = kids[i];
                memcpy(b.rot,   blk.data() + (oRot  - lo), 36);
                memcpy(&b.pos,  blk.data() + (oPos  - lo), 12);
                float3 sz{};
                memcpy(&sz,     blk.data() + (oSize - lo), 12);
                // Size is full extents; the box test wants half. Reject the
                // degenerate and the unreadable rather than cast against them.
                if (!(sz.x > 0.001f && sz.y > 0.001f && sz.z > 0.001f)) continue;
                if (!(sz.x < 1e6f && sz.y < 1e6f && sz.z < 1e6f)) continue;
                if (!(b.pos.x == b.pos.x && b.pos.y == b.pos.y && b.pos.z == b.pos.z)) continue;
                bool rot_ok = true;
                for (int k = 0; k < 9; k++) if (!(b.rot[k] == b.rot[k])) rot_ok = false;
                if (!rot_ok) continue;
                b.half = float3{ sz.x * 0.5f, sz.y * 0.5f, sz.z * 0.5f };
                g_geom.push_back(b);
            }
            if (g_geom_trunc) break;
        }
        if (g_geom_trunc) break;
        cur.swap(next);
    }

    g_geom_dm = fx_dm();
    g_geom_ms = GetTickCount() - t0;

    // AN EMPTY BUILD DOES NOT COUNT AS BUILT. Rejoining changes the DataModel
    // before the world has streamed in, so a build that lands in that window
    // walks a Workspace with nothing in it -- and latching that left the cache
    // "built" with zero parts until the place changed AGAIN. Every later
    // fx_geom returned 0, and fx_visible answered "clear" for everything,
    // because an empty cache has nothing to block with. User-reported: two
    // DataModel changes on rejoin, both re-indexing to "0 parts", and then
    // nothing found under the cursor for the rest of the session.
    //
    // Leaving it unbuilt is what makes the retry automatic: the next call
    // walks again, and the walk over an empty Workspace is cheap. That is
    // better than a fixed settle delay in the caller, which cannot know how
    // long streaming takes and is either too short or wasted time -- detect
    // the world arriving, do not guess when it will.
    g_geom_ok = !g_geom.empty();
}

// Rebuild if the place changed or nothing has been built. Caller holds the lock.
void geom_ensure_locked() {
    if (g_geom_ok && g_geom_dm == fx_dm() && g_geom_dm) return;
    geom_build_locked();
}

// Slab test against one oriented box. Returns the near distance, or -1.
float box_hit(const GBox& b, float3 o, float3 d, float maxd) {
    // Into the box's own frame. The columns of a CFrame rotation are its local
    // axes -- same layout Camera.Rotation uses, right/up/back.
    float3 rel{ o.x - b.pos.x, o.y - b.pos.y, o.z - b.pos.z };
    const float ax[3][3] = {
        { b.rot[0], b.rot[3], b.rot[6] },
        { b.rot[1], b.rot[4], b.rot[7] },
        { b.rot[2], b.rot[5], b.rot[8] },
    };
    const float h[3] = { b.half.x, b.half.y, b.half.z };

    float tmin = 0.0f, tmax = maxd;
    for (int i = 0; i < 3; i++) {
        float oi = rel.x * ax[i][0] + rel.y * ax[i][1] + rel.z * ax[i][2];
        float di = d.x  * ax[i][0] + d.y  * ax[i][1] + d.z  * ax[i][2];
        if (fabsf(di) < 1e-8f) {
            if (fabsf(oi) > h[i]) return -1.0f;   // parallel and outside
            continue;
        }
        float t1 = (-h[i] - oi) / di, t2 = (h[i] - oi) / di;
        if (t1 > t2) { float t = t1; t1 = t2; t2 = t; }
        if (t1 > tmin) tmin = t1;
        if (t2 < tmax) tmax = t2;
        if (tmin > tmax) return -1.0f;
    }
    return tmin;
}

}  // namespace

// Part count in the geometry cache, building it first if needed. Pass a
// non-zero argument to force a rebuild -- the only way to pick up a map that
// changed without leaving the place.
FX_API int fx_geom(int refresh) {
    std::lock_guard<std::mutex> lk(g_geom_mux);
    if (refresh) geom_build_locked();
    else         geom_ensure_locked();
    return g_geom_ok ? (int)g_geom.size() : 0;
}

// Milliseconds the last geometry build took, and whether it was cut short.
// Diagnostic, so the broadphase question can be settled with a number.
FX_API int fx_geom_ms(void) {
    std::lock_guard<std::mutex> lk(g_geom_mux);
    return g_geom_trunc ? -(int)g_geom_ms : (int)g_geom_ms;
}

FX_API int fx_raycast(float3 origin, float3 dir, float maxdist,
                      float3* hit, uint64_t* part) {
    float len = sqrtf(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
    if (!(len > 1e-6f) || !(maxdist > 0.0f)) return 0;
    float3 d{ dir.x / len, dir.y / len, dir.z / len };

    std::lock_guard<std::mutex> lk(g_geom_mux);
    geom_ensure_locked();
    if (!g_geom_ok) return 0;

    float best = maxdist;
    const GBox* which = nullptr;
    for (const GBox& b : g_geom) {
        float t = box_hit(b, origin, d, best);
        if (t >= 0.0f && t < best) { best = t; which = &b; }
    }
    if (!which) return 0;

    if (hit)  *hit = float3{ origin.x + d.x*best, origin.y + d.y*best, origin.z + d.z*best };
    if (part) *part = which->part;
    return 1;
}

// Is the straight line from A to B clear of world geometry?
//
// FAILS OPEN. With no cache, no offsets, or a degenerate segment this returns 1
// (visible). A visibility filter that fails closed hides everything, and this
// codebase has already paid for that once -- a health filter that read an
// undecodable value as "dead" removed every player in a normal game. If it
// cannot be corroborated, include the target and move on.
FX_API int fx_visible(float3 from, float3 to) {
    float3 d{ to.x - from.x, to.y - from.y, to.z - from.z };
    float len = sqrtf(d.x*d.x + d.y*d.y + d.z*d.z);
    if (!(len > 1e-4f)) return 1;

    // Pull both ends in slightly. A target standing ON a floor, or an eye point
    // just inside a wall's skin, otherwise reports itself occluded.
    const float EPS = 0.15f;
    if (len <= 2.0f * EPS) return 1;
    float3 u{ d.x/len, d.y/len, d.z/len };
    float3 a{ from.x + u.x*EPS, from.y + u.y*EPS, from.z + u.z*EPS };
    float  span = len - 2.0f * EPS;

    std::lock_guard<std::mutex> lk(g_geom_mux);
    geom_ensure_locked();
    if (!g_geom_ok) return 1;                     // no answer -> not a filter

    for (const GBox& b : g_geom)
        if (box_hit(b, a, u, span) >= 0.0f) return 0;
    return 1;
}
