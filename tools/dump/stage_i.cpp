// Stage I: the intersection. Third server, same question.
//
// Server 2 left eight module slots that all reached its DataModel. Only the
// ones that reach THIS server's DataModel too are actually static; the rest
// were transient objects that happened to hold a pointer. The DataModel is
// re-derived from scratch here (anchor -> round trip -> churn), so nothing in
// this test comes from the conf until the final grade.

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
static uint64_t qw(const unsigned char* p, size_t o) {
    uint64_t v; memcpy(&v, p + o, 8); return v;
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
            } else fn(pos, buf.data(), got);
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
                    qw(b2, (size_t)pa) == inst) { out = { cs, ce, pa, count }; return true; }
        }
    }
    return false;
}

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
    for (uint64_t h : cdfields)
        for (uint64_t j = 0; j <= 0x40; j += 8) {
            uint64_t inst = h - j;
            if (!tried.insert(inst).second) continue;
            Shape s{};
            if (round_trip(inst, s)) insts.push_back(inst);
        }

    int ctl = churn(base, 0x600, 700);
    uint64_t live = 0; int best = -1;
    for (uint64_t i : insts) {
        int c = churn(i, 0x600, 700);
        L("  candidate %llx  churn %d", (unsigned long long)i, c);
        if (c > best) { best = c; live = i; }
    }
    L("control (module header) churn %d   live = %llx   console = %llx  %s",
      ctl, (unsigned long long)live, (unsigned long long)fx_dm(),
      live == fx_dm() ? "match" : "MISMATCH");
    if (!live || best <= 0) { L("stop"); return; }

    // ---- the eight from the previous server --------------------------------
    uint64_t prev[] = { 0x7a98230, 0x8354750, 0x86a56e8, 0x8b79b58,
                        0x8b79b60, 0x8b7ae60, 0x8b7ae68, 0x8ba3628 };
    L("--- server 2's eight, re-followed here ---");
    std::vector<uint64_t> survivors;
    for (uint64_t r : prev) {
        uint64_t fake = fx_rd(base + r);
        unsigned char buf[0x408];
        if (!fx_ptr_ok(fake) || fx_rdblk(fake, buf, sizeof buf) != sizeof buf) {
            L("   rva %-9llx  unreadable", (unsigned long long)r);
            continue;
        }
        uint64_t first = 0; int hits = 0;
        for (uint64_t m = 0; m <= 0x400; m += 8)
            if (qw(buf, (size_t)m) == live) { if (!hits) first = m; hits++; }
        if (hits) {
            survivors.push_back(r);
            L("   rva %-9llx  SURVIVES  -> +%llx", (unsigned long long)r,
              (unsigned long long)first);
        } else {
            L("   rva %-9llx  dead", (unsigned long long)r);
        }
    }

    L("--- intersection: %zu slot(s) ---", survivors.size());
    uint64_t want = fx_off("FakeDataModel.Pointer");
    for (uint64_t r : survivors)
        L("   rva %llx%s", (unsigned long long)r, r == want ? "   <- the conf's" : "");
    L("conf FakeDataModel.Pointer = %llx", (unsigned long long)want);
}
