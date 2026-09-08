// Stage G: fix the three offsets stage F got wrong.
//
// Stage F voted for the pointer field that leads to a CFrame and got 0x68 --
// Instance.Parent -- with a perfect 65/65, tied with the real 0x188. Both are
// "a pointer to something containing a rotation matrix", because an identity
// matrix satisfies orthonormality and parents are full of them. Counting votes
// could not separate them: every part HAS a parent, so the wrong answer is
// exactly as consistent as the right one.
//
// What separates them is that parts are in DIFFERENT PLACES. Sixty parts have
// sixty positions; their shared parents have one. So the discriminator is the
// number of DISTINCT positions a candidate produces, not how many parts vote
// for it. Fields we have already identified (Parent, ClassDescriptor) are
// excluded outright -- their meaning is known, so leaving them in the running
// is just letting a solved question be re-answered wrongly.

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

static bool rd_string(uint64_t at, std::string& out) {
    unsigned char raw[0x20];
    if (fx_rdblk(at, raw, sizeof raw) != sizeof raw) return false;
    uint64_t sz = qw(raw, 0x10), cap = qw(raw, 0x18);
    if (sz == 0 || sz > 128 || cap < sz) return false;
    char buf[129] = {};
    if (cap >= 16) {
        uint64_t p = qw(raw, 0);
        if (!fx_ptr_ok(p) || fx_rdblk(p, buf, (size_t)sz) != (size_t)sz) return false;
    } else {
        memcpy(buf, raw, (size_t)sz);
    }
    for (size_t i = 0; i < sz; i++) if (buf[i] < 32 || buf[i] > 126) return false;
    out.assign(buf, (size_t)sz);
    return true;
}

static bool is_rotation(const unsigned char* p, size_t o) {
    float m[9];
    for (int i = 0; i < 9; i++) {
        m[i] = fl(p, o + (size_t)i * 4);
        if (!std::isfinite(m[i]) || std::fabs(m[i]) > 1.001f) return false;
    }
    for (int r = 0; r < 3; r++) {
        float len = m[r*3]*m[r*3] + m[r*3+1]*m[r*3+1] + m[r*3+2]*m[r*3+2];
        if (std::fabs(len - 1.0f) > 1e-3f) return false;
    }
    if (std::fabs(m[0]*m[3] + m[1]*m[4] + m[2]*m[5]) > 1e-3f) return false;
    if (std::fabs(m[0]*m[6] + m[1]*m[7] + m[2]*m[8]) > 1e-3f) return false;
    if (std::fabs(m[3]*m[6] + m[4]*m[7] + m[5]*m[8]) > 1e-3f) return false;
    float det = m[0]*(m[4]*m[8] - m[5]*m[7])
              - m[1]*(m[3]*m[8] - m[5]*m[6])
              + m[2]*(m[3]*m[7] - m[4]*m[6]);
    return std::fabs(det - 1.0f) < 1e-3f;
}

static bool is_position(const unsigned char* p, size_t o) {
    for (int i = 0; i < 3; i++) {
        float v = fl(p, o + (size_t)i * 4);
        if (!std::isfinite(v) || std::fabs(v) > 100000.0f) return false;
    }
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

static void grade(const char* key, uint64_t got) {
    uint64_t want = fx_off(key);
    L("  %-26s derived %-8llx conf %-8llx  %s", key,
      (unsigned long long)got, (unsigned long long)want,
      got == want ? "MATCH" : "DIFFER");
}

// A candidate is a (pointer field, CFrame offset) pair. What matters is how
// many distinct positions it yields across the sample.
struct Cand {
    int votes = 0;
    std::set<uint64_t> distinct;      // position triples, packed
};

extern "C" void fx_run() {
    uint64_t dm = fx_dm();
    if (!dm) { L("not attached"); return; }

    // Shape, exactly as stage F derived it (all four MATCHed there).
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
    if (!c.cs) { L("stop: no shape"); return; }

    std::vector<uint64_t> svc, wsk;
    children(c, dm, svc);
    uint64_t ws = 0;
    for (uint64_t s : svc) if (class_of(c, s) == "Workspace") ws = s;
    if (!ws) { L("stop: no Workspace"); return; }
    children(c, ws, wsk);

    std::vector<uint64_t> parts;
    for (uint64_t k : wsk) {
        std::string cl = class_of(c, k);
        if (cl == "Part" || cl == "MeshPart" || cl == "UnionOperation" ||
            cl == "SpawnLocation" || cl == "Seat" || cl == "TrussPart" ||
            cl == "WedgePart" || cl == "CornerWedgePart") parts.push_back(k);
        if (cl == "Model" || cl == "Folder") {
            std::vector<uint64_t> gk;
            children(c, k, gk);
            for (uint64_t g : gk) {
                std::string gc = class_of(c, g);
                if (gc == "Part" || gc == "MeshPart" || gc == "UnionOperation" ||
                    gc == "WedgePart" || gc == "Seat") parts.push_back(g);
            }
        }
        if (parts.size() > 80) break;
    }
    L("parts sampled: %zu   (excluded fields: Parent +%llx, ClassDescriptor +%llx)",
      parts.size(), (unsigned long long)c.pa, (unsigned long long)c.cd);

    // ---- joint vote over (pointer field, CFrame offset) ---------------------
    std::map<std::pair<uint64_t, uint64_t>, Cand> cands;
    for (uint64_t p : parts) {
        unsigned char pb[0x300];
        if (fx_rdblk(p, pb, sizeof pb) != sizeof pb) continue;
        for (size_t o = 0; o + 8 <= sizeof pb; o += 8) {
            if (o == c.pa || o == c.cd) continue;         // already identified
            uint64_t t = qw(pb, o);
            if (!fx_ptr_ok(t)) continue;
            unsigned char tb[0x240];
            if (fx_rdblk(t, tb, sizeof tb) != sizeof tb) continue;
            for (size_t f = 0; f + 48 <= sizeof tb; f += 4) {
                if (!is_rotation(tb, f) || !is_position(tb, f + 36)) continue;
                Cand& cd = cands[{ o, f }];
                cd.votes++;
                uint64_t key = 0;
                for (int i = 0; i < 3; i++) {
                    uint32_t bits;
                    float v = fl(tb, f + 36 + (size_t)i * 4);
                    memcpy(&bits, &v, 4);
                    key = key * 1000003ull + bits;
                }
                cd.distinct.insert(key);
            }
        }
    }

    // Rank by distinct positions. Ties on votes are the whole problem; ties on
    // distinctness do not happen between a real primitive and a shared parent.
    std::vector<std::pair<std::pair<uint64_t, uint64_t>, Cand>> rank(cands.begin(), cands.end());
    for (size_t i = 0; i + 1 < rank.size(); i++)
        for (size_t j = i + 1; j < rank.size(); j++)
            if (rank[j].second.distinct.size() > rank[i].second.distinct.size())
                std::swap(rank[i], rank[j]);

    L("--- candidates, ranked by distinct positions ---");
    for (size_t i = 0; i < rank.size() && i < 8; i++)
        L("   ptr +%-5llx cframe +%-5llx  votes %-3d  distinct %zu",
          (unsigned long long)rank[i].first.first,
          (unsigned long long)rank[i].first.second,
          rank[i].second.votes, rank[i].second.distinct.size());
    if (rank.empty()) { L("stop: no candidates"); return; }

    uint64_t prim = rank[0].first.first;
    uint64_t prot = rank[0].first.second;

    // ---- Primitive.Size -----------------------------------------------------
    // Inside the same object: three positive floats that differ from part to
    // part. Position already claimed its slot, so it is excluded by offset.
    std::map<uint64_t, std::set<uint64_t>> size_cand;
    for (uint64_t p : parts) {
        uint64_t t = fx_rd(p + prim);
        if (!fx_ptr_ok(t)) continue;
        unsigned char tb[0x240];
        if (fx_rdblk(t, tb, sizeof tb) != sizeof tb) continue;
        for (size_t o = 0; o + 12 <= sizeof tb; o += 4) {
            if (o >= prot && o <= prot + 44) continue;         // the CFrame itself
            bool ok = true;
            uint64_t key = 0;
            for (int i = 0; i < 3; i++) {
                float v = fl(tb, o + (size_t)i * 4);
                if (!std::isfinite(v) || v <= 0.01f || v > 2100.0f) { ok = false; break; }
                uint32_t bits; memcpy(&bits, &v, 4);
                key = key * 1000003ull + bits;
            }
            if (ok) size_cand[o].insert(key);
        }
    }
    uint64_t size_off = (uint64_t)-1; size_t sbest = 0;
    for (const auto& kv : size_cand)
        if (kv.second.size() > sbest) { sbest = kv.second.size(); size_off = kv.first; }
    L("Primitive.Size candidate +%llx with %zu distinct triples",
      (unsigned long long)size_off, sbest);

    L("--- graded ---");
    grade("BasePart.Primitive", prim);
    grade("Primitive.Rotation", prot);
    grade("Primitive.Position", prot + 36);
    grade("Primitive.Size",     size_off);
}
