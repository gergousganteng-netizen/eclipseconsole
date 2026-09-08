// Stage E: tell the live DataModel from the dead one.
//
// After a rejoin two instances pass the round trip: the current tree, and the
// previous server's, still held alive by a reference somewhere. "Parent == 0"
// does not separate them -- both are roots. Stage D picked wrong on that rule
// and then measured every survival test against the corpse, which is why it
// reported the conf's own static as stale.
//
// What actually separates them is that one of them is RUNNING. Sample the
// object twice a second apart: the live DataModel ticks (counters, timers, job
// state), the dead one is frozen. No offsets needed -- just "did any byte
// change", which is the differential technique in its simplest form.
//
// Controls, both directions: the live pick must equal fx_dm(), and a known-dead
// region (the module's own header) must report zero churn.

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

// How many bytes of an object changed over `ms`. The whole point is that this
// needs no idea what any of the fields mean.
static int churn(uint64_t addr, size_t len, int ms) {
    std::vector<unsigned char> a(len), b(len);
    if (fx_rdblk(addr, a.data(), len) != len) return -1;
    fx_sleep(ms);
    if (fx_rdblk(addr, b.data(), len) != len) return -1;
    int d = 0;
    for (size_t i = 0; i < len; i++) if (a[i] != b[i]) d++;
    return d;
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

    // ---- candidates, exactly as stage C ------------------------------------
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
    L("round-trip survivors: %zu", insts.size());

    // ---- control: something known-dead must read as dead --------------------
    int ctl = churn(base, 0x600, 700);
    L("control  module header churn = %d bytes  (expect 0)", ctl);

    // ---- which one is alive -------------------------------------------------
    uint64_t live = 0; int best = -1;
    for (uint64_t i : insts) {
        Shape s{}; round_trip(i, s);
        int c = churn(i, 0x600, 700);
        L("  %llx  parent %llx  %llu children  churn %d bytes",
          (unsigned long long)i, (unsigned long long)fx_rd(i + shape.parent),
          (unsigned long long)s.count, c);
        if (c > best) { best = c; live = i; }
    }
    if (best <= 0) { L("stop: nothing is ticking -- cannot tell them apart"); return; }

    uint64_t known = fx_dm();
    L("live pick = %llx   console = %llx   %s", (unsigned long long)live,
      (unsigned long long)known, live == known ? "MATCH" : "MISMATCH");

    // ---- now the survival test, against the RIGHT target --------------------
    struct Old { uint64_t rva; const char* note; };
    Old olds[] = {
        { 0x8354490, "stage C candidate 1" },
        { 0x8354498, "stage C candidate 2" },
        { 0x83544b0, "stage C candidate 3" },
        { 0x8338218, "stage D (dead-DM) 1" },
        { 0x86919c8, "stage D (dead-DM) 2" },
        { 0x8693e18, "stage D (dead-DM) 3" },
        { 0x8bb86e0, "stage D (dead-DM) 4" },
        { 0x8bb86e8, "stage D (dead-DM) 5" },
        { 0x8b79b58, "the conf's FakeDataModel" },
    };
    L("--- slots re-followed against the live DataModel ---");
    for (const Old& o : olds) {
        uint64_t fake = fx_rd(base + o.rva);
        unsigned char buf[0x408];
        if (!fx_ptr_ok(fake) || fx_rdblk(fake, buf, sizeof buf) != sizeof buf) {
            L("  rva %-9llx  unreadable                    (%s)",
              (unsigned long long)o.rva, o.note);
            continue;
        }
        uint64_t first = 0; int hits = 0;
        for (uint64_t m = 0; m <= 0x400; m += 8)
            if (qw(buf, (size_t)m) == live) { if (!hits) first = m; hits++; }
        if (hits) L("  rva %-9llx  LIVE  -> +%llx  (%d)          (%s)",
                    (unsigned long long)o.rva, (unsigned long long)first, hits, o.note);
        else      L("  rva %-9llx  does not reach it            (%s)",
                    (unsigned long long)o.rva, o.note);
    }

    // ---- and the fresh list for the live one --------------------------------
    std::unordered_set<uint64_t> want{ live };
    std::vector<uint64_t> h2;
    holders_of(want, heap, h2, 400000);
    std::unordered_set<uint64_t> objs;
    for (uint64_t h : h2)
        for (uint64_t m = 0; m <= 0x400; m += 8) objs.insert(h - m);
    std::vector<uint64_t> slots;
    holders_of(objs, module_only, slots, 8192);
    std::unordered_set<uint64_t> uniq;
    for (uint64_t s : slots) uniq.insert(s - base);
    std::vector<uint64_t> sorted(uniq.begin(), uniq.end());
    for (size_t i = 0; i + 1 < sorted.size(); i++)
        for (size_t j = i + 1; j < sorted.size(); j++)
            if (sorted[j] < sorted[i]) { uint64_t t = sorted[i]; sorted[i] = sorted[j]; sorted[j] = t; }
    L("--- statics reaching the live DataModel: %zu ---", sorted.size());
    for (uint64_t r : sorted)
        L("      rva %llx%s", (unsigned long long)r,
          r == fx_off("FakeDataModel.Pointer") ? "   <- the conf's" : "");
}
