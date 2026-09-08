// Stage H: Primitive.Size, and breaking the two-CFrame tie.
//
// Stage G's size filter was "three positive floats under 2100", which is an
// exact description of a rotation matrix row. It picked +130, inside the second
// CFrame. Two properties separate a real Size from float noise, and neither
// needs a known value:
//
//   * A part's Size does not change while the part moves or turns. A rotation
//     entry does. Sample twice with the world running and drop anything that
//     moved -- but only judge it on parts that actually MOVED, or "unchanged"
//     means nothing.
//   * Roblox sizes are authored numbers: whole studs, halves, tenths. Rotation
//     entries are arbitrary reals. Scoring "is a multiple of 0.05" separates
//     authored data from computed data across a large sample.
//
// The two-CFrame tie gets the same treatment: the CFrame the game renders from
// is the one that tracks the CAMERA when you look at your own character, so the
// tie-break is which candidate moves when the world moves.

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

static void L(const char* fmt, ...) {
    char b[512];
    va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    fx_log(b);
}

static uint64_t qw(const unsigned char* p, size_t o) {
    uint64_t v; memcpy(&v, p + o, 8); return v;
}
static float fl(const unsigned char* p, size_t o) {
    float v; memcpy(&v, p + o, 4); return v;
}
static bool nice(float v) {                       // authored, not computed
    float t = v * 20.0f;                          // 0.05 grid
    return std::fabs(t - std::floor(t + 0.5f)) < 1e-3f;
}

static bool rd_string(uint64_t at, std::string& out) {
    unsigned char raw[0x20];
    if (fx_rdblk(at, raw, sizeof raw) != sizeof raw) return false;
    uint64_t sz = qw(raw, 0x10), cap = qw(raw, 0x18);
    if (sz == 0 || sz > 128 || cap < sz) return false;
    char buf[129] = {};
    if (cap >= 16) {
        uint64_t p = qw(raw, 0);
        if (!fx_ptr_ok(p) || fx_rdblk(p, buf, (size_t)sz) != (size_t)sz) return false;
    } else memcpy(buf, raw, (size_t)sz);
    for (size_t i = 0; i < sz; i++) if (buf[i] < 32 || buf[i] > 126) return false;
    out.assign(buf, (size_t)sz);
    return true;
}

struct Ctx { uint64_t cd, cn, cs, ce, pa; };

static std::string class_of(const Ctx& c, uint64_t inst) {
    uint64_t d = fx_rd(inst + c.cd);
    if (!d) return "";
    std::string s;
    uint64_t p = fx_rd(d + c.cn);
    if (fx_ptr_ok(p) && rd_string(p, s)) return s;
    if (rd_string(d + c.cn, s)) return s;
    return "";
}

static void children(const Ctx& c, uint64_t inst, std::vector<uint64_t>& out) {
    uint64_t cont = fx_rd(inst + c.cs);
    if (!fx_ptr_ok(cont)) return;
    uint64_t b = fx_rd(cont), e = fx_rd(cont + c.ce);
    if (!fx_ptr_ok(b) || e <= b || (e - b) % 0x10) return;
    uint64_t n = (e - b) / 0x10;
    if (n > 4000) n = 4000;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t ch = fx_rd(b + i * 0x10);
        if (fx_ptr_ok(ch)) out.push_back(ch);
    }
}

extern "C" void fx_run() {
    uint64_t dm = fx_dm();
    Ctx c{};
    unsigned char obj[0x200];
    fx_rdblk(dm, obj, sizeof obj);
    for (uint64_t j = 0; j <= 0x40 && !c.cd; j += 8) {
        uint64_t d = qw(obj, (size_t)j);
        if (!fx_ptr_ok(d)) continue;
        for (uint64_t k = 0; k <= 0x40; k += 8) {
            std::string s;
            uint64_t p = fx_rd(d + k);
            if ((fx_ptr_ok(p) && rd_string(p, s) && s == "DataModel") ||
                (rd_string(d + k, s) && s == "DataModel")) { c.cd = j; c.cn = k; break; }
        }
    }
    for (uint64_t cs = 0x40; cs <= 0x100 && !c.cs; cs += 8) {
        uint64_t cont = qw(obj, (size_t)cs);
        if (!fx_ptr_ok(cont)) continue;
        unsigned char cb[0x40];
        if (fx_rdblk(cont, cb, sizeof cb) != sizeof cb) continue;
        uint64_t b = qw(cb, 0);
        if (!fx_ptr_ok(b)) continue;
        for (uint64_t ce = 8; ce <= 0x38; ce += 8) {
            uint64_t e = qw(cb, (size_t)ce);
            if (e <= b || (e - b) % 0x10) continue;
            uint64_t n = (e - b) / 0x10;
            if (n < 3 || n > 4000) continue;
            uint64_t c0 = fx_rd(b), c1 = fx_rd(b + 0x10);
            for (uint64_t pa = 0x40; pa <= 0xf8; pa += 8)
                if (fx_rd(c0 + pa) == dm && fx_rd(c1 + pa) == dm) {
                    c.cs = cs; c.ce = ce; c.pa = pa; break;
                }
            if (c.cs) break;
        }
    }

    std::vector<uint64_t> svc, wsk;
    children(c, dm, svc);
    uint64_t ws = 0;
    for (uint64_t s : svc) if (class_of(c, s) == "Workspace") ws = s;
    if (!ws) { L("stop: no Workspace"); return; }
    children(c, ws, wsk);

    // Prefer parts that MOVE: a character's limbs never stop, so they are the
    // sample that makes "unchanged" mean something. Fall back to world parts.
    std::vector<uint64_t> parts, movers;
    for (uint64_t k : wsk) {
        std::string cl = class_of(c, k);
        if (cl == "Part" || cl == "MeshPart" || cl == "UnionOperation" ||
            cl == "SpawnLocation" || cl == "Seat" || cl == "WedgePart")
            parts.push_back(k);
        if (cl == "Model") {
            std::vector<uint64_t> gk;
            children(c, k, gk);
            bool humanoid = false;
            for (uint64_t g : gk) if (class_of(c, g) == "Humanoid") humanoid = true;
            for (uint64_t g : gk) {
                std::string gc = class_of(c, g);
                if (gc == "Part" || gc == "MeshPart" || gc == "UnionOperation" ||
                    gc == "WedgePart") {
                    parts.push_back(g);
                    if (humanoid) movers.push_back(g);
                }
            }
        }
        if (parts.size() > 120) break;
    }
    const uint64_t PRIM = 0x188;      // stage G, agreed 95/95 and graded MATCH
    L("parts %zu   character parts %zu", parts.size(), movers.size());

    // ---- which CFrame is the live one --------------------------------------
    // Sample the movers twice. The CFrame the game drives changes; a cached or
    // lagging copy may not. If BOTH change, say so rather than picking one.
    const uint64_t CF[2] = { 0xc8, 0x110 };
    int changed[2] = { 0, 0 };
    std::vector<unsigned char> a(0x240), b(0x240);
    std::vector<uint64_t> prims;
    for (uint64_t p : movers) {
        uint64_t t = fx_rd(p + PRIM);
        if (fx_ptr_ok(t)) prims.push_back(t);
    }
    if (prims.empty()) L("no character parts found -- move somewhere with players");
    for (uint64_t t : prims) {
        if (fx_rdblk(t, a.data(), a.size()) != a.size()) continue;
        fx_sleep(120);
        if (fx_rdblk(t, b.data(), b.size()) != b.size()) continue;
        for (int i = 0; i < 2; i++)
            if (memcmp(a.data() + CF[i], b.data() + CF[i], 48) != 0) changed[i]++;
    }
    L("CFrame +c8 moved on %d/%zu parts, +110 moved on %d/%zu",
      changed[0], prims.size(), changed[1], prims.size());

    // ---- Size ---------------------------------------------------------------
    // Two scores per offset: how many distinct triples (a real field varies
    // between parts) and what fraction are on the 0.05 grid (authored, not
    // computed). Both must be high; either alone is what fooled stage G.
    struct S { std::set<uint64_t> distinct; int total = 0, on_grid = 0; };
    std::map<uint64_t, S> cand;
    for (uint64_t p : parts) {
        uint64_t t = fx_rd(p + PRIM);
        if (!fx_ptr_ok(t)) continue;
        unsigned char tb[0x240];
        if (fx_rdblk(t, tb, sizeof tb) != sizeof tb) continue;
        for (size_t o = 0; o + 12 <= sizeof tb; o += 4) {
            bool ok = true, grid = true;
            uint64_t key = 0;
            for (int i = 0; i < 3; i++) {
                float v = fl(tb, o + (size_t)i * 4);
                if (!std::isfinite(v) || v < 0.05f || v > 2100.0f) { ok = false; break; }
                if (!nice(v)) grid = false;
                uint32_t bits; memcpy(&bits, &v, 4);
                key = key * 1000003ull + bits;
            }
            if (!ok) continue;
            S& s = cand[o];
            s.distinct.insert(key);
            s.total++;
            if (grid) s.on_grid++;
        }
    }
    std::vector<std::pair<uint64_t, S>> rank(cand.begin(), cand.end());
    for (size_t i = 0; i + 1 < rank.size(); i++)
        for (size_t j = i + 1; j < rank.size(); j++) {
            double si = rank[i].second.distinct.size() *
                        (double)rank[i].second.on_grid / (rank[i].second.total + 1e-9);
            double sj = rank[j].second.distinct.size() *
                        (double)rank[j].second.on_grid / (rank[j].second.total + 1e-9);
            if (sj > si) std::swap(rank[i], rank[j]);
        }
    L("--- size candidates: distinct x on-grid ---");
    for (size_t i = 0; i < rank.size() && i < 8; i++)
        L("   +%-6llx  distinct %-4zu  on-grid %d/%d%s",
          (unsigned long long)rank[i].first, rank[i].second.distinct.size(),
          rank[i].second.on_grid, rank[i].second.total,
          rank[i].first == fx_off("Primitive.Size") ? "   <- the conf's" : "");

    uint64_t got = rank.empty() ? (uint64_t)-1 : rank[0].first;
    uint64_t want = fx_off("Primitive.Size");
    L("Primitive.Size  derived %llx  conf %llx  %s",
      (unsigned long long)got, (unsigned long long)want,
      got == want ? "MATCH" : "DIFFER");
}
