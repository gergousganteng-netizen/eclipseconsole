// Stage D: which of the candidate statics survive a DataModel being replaced?
//
// A rejoin builds a new DataModel at a new address. A slot that only LOOKED
// static -- a transient object that happened to hold a pointer to the old one
// -- cannot follow. The real one can, because the game reaches its DataModel
// through it every frame. So: re-derive from scratch in the new server, then
// ask which of the pre-rejoin slots still land on the new DataModel.
//
// The four RVAs below are what stage C printed BEFORE the rejoin (the first
// three of its sixteen, plus the one the conf ships). They are module-relative,
// so they mean the same thing across sessions -- that is what makes this test
// possible at all.

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

static void L(const char* fmt, ...) {
    char b[512];
    va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    fx_log(b);
}

struct Range { uint64_t base, size; };
static uint64_t qw(const unsigned char* p, size_t off) {
    uint64_t v; memcpy(&v, p + off, 8); return v;
}

template <class F>
static void sweep(const std::vector<Range>& rs, size_t overlap, F fn) {
    const size_t CHUNK = 1u << 20;
    std::vector<unsigned char> buf(CHUNK);
    for (const Range& r : rs) {
        uint64_t pos = r.base, end = r.base + r.size;
        while (pos < end) {
            if (!fx_running()) return;
            size_t want = (size_t)((end - pos < CHUNK) ? (end - pos) : CHUNK);
            size_t got  = fx_rdblk(pos, buf.data(), want);
            if (got == 0) {
                for (uint64_t p = pos; p < pos + want; p += 4096) {
                    size_t g = fx_rdblk(p, buf.data(), 4096);
                    if (g) fn(p, buf.data(), g);
                }
            } else {
                fn(pos, buf.data(), got);
            }
            if (want <= overlap) break;
            pos += want - overlap;
        }
    }
}

static void holders_of(const std::unordered_set<uint64_t>& want,
                       const std::vector<Range>& rs,
                       std::vector<uint64_t>& out, size_t cap) {
    sweep(rs, 8, [&](uint64_t base, const unsigned char* p, size_t n) {
        if (out.size() >= cap) return;
        for (size_t i = 0; i + 8 <= n; i += 8) {
            uint64_t v; memcpy(&v, p + i, 8);
            if (v && want.count(v)) {
                out.push_back(base + i);
                if (out.size() >= cap) return;
            }
        }
    });
}

struct Shape { uint64_t children, cend, parent, count; };

static bool round_trip(uint64_t inst, Shape& out) {
    unsigned char obj[0x200];
    if (fx_rdblk(inst, obj, sizeof obj) != sizeof obj) return false;
    for (uint64_t cs = 0x40; cs <= 0x100; cs += 8) {
        uint64_t cont = qw(obj, (size_t)cs);
        if (!fx_ptr_ok(cont)) continue;
        unsigned char cbuf[0x40];
        if (fx_rdblk(cont, cbuf, sizeof cbuf) != sizeof cbuf) continue;
        uint64_t begin = qw(cbuf, 0);
        if (!fx_ptr_ok(begin)) continue;
        for (uint64_t ce = 8; ce <= 0x38; ce += 8) {
            uint64_t end = qw(cbuf, (size_t)ce);
            if (end <= begin || (end - begin) % 0x10) continue;
            uint64_t count = (end - begin) / 0x10;
            if (count < 3 || count > 4000) continue;
            unsigned char ent[0x30];
            if (fx_rdblk(begin, ent, sizeof ent) != sizeof ent) continue;
            uint64_t c0 = qw(ent, 0x00), c1 = qw(ent, 0x10), c2 = qw(ent, 0x20);
            if (!fx_ptr_ok(c0) || !fx_ptr_ok(c1) || !fx_ptr_ok(c2)) continue;
            unsigned char b0[0x100], b1[0x100], b2[0x100];
            if (fx_rdblk(c0, b0, sizeof b0) != sizeof b0) continue;
            if (fx_rdblk(c1, b1, sizeof b1) != sizeof b1) continue;
            if (fx_rdblk(c2, b2, sizeof b2) != sizeof b2) continue;
            for (uint64_t pa = 0x40; pa <= 0xf8; pa += 8)
                if (qw(b0, (size_t)pa) == inst && qw(b1, (size_t)pa) == inst &&
                    qw(b2, (size_t)pa) == inst) {
                    out = { cs, ce, pa, count };
                    return true;
                }
        }
    }
    return false;
}

extern "C" void fx_run() {
    uint64_t base = fx_base();
    uint32_t lfanew = fx_rd32(base + 0x3c);
    uint32_t img    = fx_rd32(base + lfanew + 0x50);

    int n = fx_regions(nullptr, nullptr, 0);
    std::vector<uint64_t> rb(n), rs(n);
    fx_regions(rb.data(), rs.data(), n);
    std::vector<Range> heap, all;
    for (int i = 0; i < n; i++) heap.push_back({ rb[i], rs[i] });
    all = heap;
    all.push_back({ base, img });
    std::vector<Range> module_only{ { base, img } };

    // ---- re-derive the DataModel, from scratch, in this server -------------
    std::vector<uint64_t> strings;
    sweep(all, 32, [&](uint64_t b, const unsigned char* p, size_t sz) {
        for (size_t i = 0; i + 0x20 <= sz; i += 8)
            if (memcmp(p + i, "DataModel\0", 10) == 0 &&
                qw(p, i + 0x10) == 9 && qw(p, i + 0x18) == 15)
                strings.push_back(b + i);
    });
    std::unordered_set<uint64_t> sset(strings.begin(), strings.end());
    std::vector<uint64_t> nameptrs;
    holders_of(sset, all, nameptrs, 100000);
    std::unordered_set<uint64_t> descs;
    for (uint64_t h : nameptrs)
        for (uint64_t k = 0; k <= 0x40; k += 8) descs.insert(h - k);
    std::vector<uint64_t> cdfields;
    holders_of(descs, heap, cdfields, 200000);

    std::unordered_set<uint64_t> tried;
    std::vector<uint64_t> insts;
    Shape shape{};
    for (uint64_t h : cdfields)
        for (uint64_t j = 0; j <= 0x40; j += 8) {
            uint64_t inst = h - j;
            if (!tried.insert(inst).second) continue;
            Shape s{};
            if (round_trip(inst, s)) { insts.push_back(inst); shape = s; }
        }

    uint64_t dm = 0;
    for (uint64_t i : insts) if (fx_rd(i + shape.parent) == 0) dm = i;
    if (!dm && !insts.empty()) dm = insts[0];
    uint64_t known = fx_dm();
    L("new DataModel = %llx   console says %llx   %s",
      (unsigned long long)dm, (unsigned long long)known,
      dm == known ? "match" : "MISMATCH");
    if (!dm) { L("stop: no DataModel"); return; }

    // ---- the actual test ----------------------------------------------------
    // Does a pre-rejoin slot still reach a DataModel? Nothing about the slot is
    // assumed -- follow it and see where it lands now.
    struct Old { uint64_t rva; const char* note; };
    Old olds[] = {
        { 0x8354490, "stage C candidate 1" },
        { 0x8354498, "stage C candidate 2" },
        { 0x83544b0, "stage C candidate 3" },
        { 0x8b79b58, "the conf's FakeDataModel" },
    };
    L("--- pre-rejoin slots, re-followed ---");
    for (const Old& o : olds) {
        uint64_t fake = fx_rd(base + o.rva);
        if (!fx_ptr_ok(fake)) { L("  rva %llx  dead pointer            (%s)",
                                  (unsigned long long)o.rva, o.note); continue; }
        unsigned char buf[0x408];
        if (fx_rdblk(fake, buf, sizeof buf) != sizeof buf) {
            L("  rva %llx  unreadable object       (%s)",
              (unsigned long long)o.rva, o.note); continue;
        }
        int hits = 0; uint64_t first = 0;
        for (uint64_t m = 0; m <= 0x400; m += 8)
            if (qw(buf, (size_t)m) == dm) { if (!hits) first = m; hits++; }
        if (hits) L("  rva %llx  SURVIVED  -> +%llx (%d slots)  (%s)",
                    (unsigned long long)o.rva, (unsigned long long)first, hits, o.note);
        else      L("  rva %llx  stale, no longer reaches the DataModel  (%s)",
                    (unsigned long long)o.rva, o.note);
    }

    // ---- and re-derive the whole candidate list for the NEXT rejoin ---------
    std::unordered_set<uint64_t> want{ dm };
    std::vector<uint64_t> h2;
    holders_of(want, heap, h2, 400000);
    std::unordered_set<uint64_t> objs;
    for (uint64_t h : h2)
        for (uint64_t m = 0; m <= 0x400; m += 8) objs.insert(h - m);
    std::vector<uint64_t> slots;
    holders_of(objs, module_only, slots, 8192);

    std::unordered_set<uint64_t> uniq;
    for (uint64_t s : slots) uniq.insert(s - base);
    L("--- this server's candidates: %zu slots ---", uniq.size());
    std::vector<uint64_t> sorted(uniq.begin(), uniq.end());
    for (size_t i = 0; i + 1 < sorted.size(); i++)
        for (size_t j = i + 1; j < sorted.size(); j++)
            if (sorted[j] < sorted[i]) { uint64_t t = sorted[i]; sorted[i] = sorted[j]; sorted[j] = t; }
    for (uint64_t r : sorted) L("      rva %llx%s", (unsigned long long)r,
                                r == 0x8b79b58 ? "   <- the conf's" : "");

    FILE* f = fopen("build/dump/slots.txt", "w");
    if (f) {
        for (uint64_t r : sorted) fprintf(f, "%llx\n", (unsigned long long)r);
        fclose(f);
        L("written to build/dump/slots.txt");
    }
}
