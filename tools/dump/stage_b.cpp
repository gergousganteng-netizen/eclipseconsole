// Stage B: pin the DataModel exactly, then keep only statics that reach it.
//
// Stage A ended with 297 candidate slots because every level widened by "minus
// each plausible field offset" and nothing ever narrowed. The fix is an
// invariant that cannot be satisfied by accident:
//
//     I is an Instance  <=>  it has a children container whose entries all
//                            point BACK at I through one common offset
//
// A wrong instance base has no such container, and a coincidence would have to
// hold for several entries at once. That single test pins Instance.ChildrenStart,
// ChildrenEnd, Parent and the instance base together -- four unknowns collapsing
// on one round trip -- and the static hunt then verifies FORWARD to that exact
// address, so nothing is accepted on shape alone.

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
        size_t lim = n & ~(size_t)7;
        for (size_t i = 0; i + 8 <= lim; i += 8) {
            uint64_t v;
            memcpy(&v, p + i, 8);
            if (v && want.count(v)) {
                out.push_back(base + i);
                if (out.size() >= cap) return;
            }
        }
    });
}

static uint64_t qw(const unsigned char* p, size_t off) {
    uint64_t v; memcpy(&v, p + off, 8); return v;
}

// The round trip. Returns true and fills the offsets when `inst` really is an
// Instance -- i.e. when a container hangs off it whose entries agree, all of
// them, on where their Parent pointer lives and that it points back here.
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
            if (count < 3 || count > 4000) continue;   // services: tens, not billions

            // Three entries have to agree. One could be luck; three sharing an
            // offset that points back to the same object is the structure.
            unsigned char ent[0x30];
            if (fx_rdblk(begin, ent, sizeof ent) != sizeof ent) continue;
            uint64_t c0 = qw(ent, 0x00), c1 = qw(ent, 0x10), c2 = qw(ent, 0x20);
            if (!fx_ptr_ok(c0) || !fx_ptr_ok(c1) || !fx_ptr_ok(c2)) continue;

            unsigned char b0[0x100], b1[0x100], b2[0x100];
            if (fx_rdblk(c0, b0, sizeof b0) != sizeof b0) continue;
            if (fx_rdblk(c1, b1, sizeof b1) != sizeof b1) continue;
            if (fx_rdblk(c2, b2, sizeof b2) != sizeof b2) continue;

            for (uint64_t pa = 0x40; pa <= 0xf8; pa += 8) {
                if (qw(b0, (size_t)pa) == inst &&
                    qw(b1, (size_t)pa) == inst &&
                    qw(b2, (size_t)pa) == inst) {
                    out = { cs, ce, pa, count };
                    return true;
                }
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
    std::vector<Range> heap;
    for (int i = 0; i < n; i++) heap.push_back({ rb[i], rs[i] });
    std::vector<Range> module_only{ { base, img } };

    // ---- the anchor, as in stage A -----------------------------------------
    std::vector<uint64_t> strings;
    sweep(heap, 32, [&](uint64_t b, const unsigned char* p, size_t sz) {
        for (size_t i = 0; i + 0x20 <= sz; i += 8)
            if (memcmp(p + i, "DataModel\0", 10) == 0 &&
                qw(p, i + 0x10) == 9 && qw(p, i + 0x18) == 15)
                strings.push_back(b + i);
    });
    if (strings.empty()) { L("no anchor string"); return; }

    std::unordered_set<uint64_t> descs;
    for (uint64_t s : strings)
        for (uint64_t k = 0; k <= 0x40; k += 8) descs.insert(s - k);

    std::vector<uint64_t> h1;
    holders_of(descs, heap, h1, 200000);
    L("descriptor holders: %zu", h1.size());

    // ---- pin the instance ---------------------------------------------------
    std::unordered_set<uint64_t> tried;
    std::vector<uint64_t> real;
    Shape shape{};
    uint64_t class_off = 0;

    for (uint64_t h : h1) {
        for (uint64_t j = 0; j <= 0x40; j += 8) {
            uint64_t inst = h - j;
            if (!tried.insert(inst).second) continue;
            Shape s{};
            if (!round_trip(inst, s)) continue;
            real.push_back(inst);
            shape = s;
            class_off = j;
            L("instance %llx  children +%llx  end +%llx  parent +%llx  (%llu children)",
              (unsigned long long)inst, (unsigned long long)s.children,
              (unsigned long long)s.cend, (unsigned long long)s.parent,
              (unsigned long long)s.count);
        }
    }
    L("round trip survivors: %zu", real.size());
    if (real.empty()) { L("stop: no candidate is shaped like an Instance"); return; }

    // The DataModel is the one with no parent -- it is the root of the tree.
    uint64_t dm = 0;
    for (uint64_t i : real) if (fx_rd(i + shape.parent) == 0) dm = i;
    if (!dm) dm = real[0];
    L("DataModel = %llx   Instance.ClassDescriptor +%llx",
      (unsigned long long)dm, (unsigned long long)class_off);

    // ---- statics that reach it, verified forwards ---------------------------
    std::unordered_set<uint64_t> want{ dm };
    std::vector<uint64_t> h2;
    holders_of(want, heap, h2, 400000);
    L("fields holding the DataModel: %zu", h2.size());

    std::unordered_set<uint64_t> objs;
    for (uint64_t h : h2)
        for (uint64_t m = 0; m <= 0x400; m += 8) objs.insert(h - m);

    std::vector<uint64_t> slots;
    holders_of(objs, module_only, slots, 8192);
    L("module slots reaching it: %zu", slots.size());

    // Forward check: follow the chain the way the console will, and keep only
    // what actually lands on the DataModel. This is what stage A never did.
    struct Hit { uint64_t rva, m; };
    std::vector<Hit> good;
    for (uint64_t s : slots) {
        uint64_t fake = fx_rd(s);
        if (!fx_ptr_ok(fake)) continue;
        unsigned char buf[0x408];
        if (fx_rdblk(fake, buf, sizeof buf) != sizeof buf) continue;
        for (uint64_t m = 0; m <= 0x400; m += 8)
            if (qw(buf, (size_t)m) == dm) good.push_back({ s - base, m });
    }
    L("---");
    L("verified chains: %zu", good.size());
    for (size_t i = 0; i < good.size() && i < 20; i++)
        L("   rva %llx  +%llx", (unsigned long long)good[i].rva,
          (unsigned long long)good[i].m);

    uint64_t want_rva = fx_off("FakeDataModel.Pointer");
    uint64_t want_m   = fx_off("FakeDataModel.RealDataModel");
    bool hit = false;
    for (const Hit& g : good) if (g.rva == want_rva && g.m == want_m) hit = true;
    L("conf says rva %llx +%llx -- scan %s",
      (unsigned long long)want_rva, (unsigned long long)want_m,
      hit ? "AGREES" : "does not contain it");
}
