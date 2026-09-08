// Stage C. Two corrections over B, both found by feeding the test a known-good
// input instead of reasoning about why it came back empty:
//
//   1. Class descriptors live in the MODULE, not the heap, and fx_regions is
//      writable-only by design. Sweeping only its regions could never see them.
//   2. ClassName is a POINTER to a std::string, not the string inline. The
//      console's read_str_field tries the pointer first; I read the pointer as
//      if it were the characters and got garbage.
//
// So the chain has one more hop than stage A assumed:
//
//   "DataModel" std::string  <-  a pointer to it  =  descriptor + ClassName
//                            <-  instance + ClassDescriptor
//                            <-  verified by the parent round trip
//                            <-  a field holding the instance = fake + Real
//                            <-  a module qword = the static
//
// The round trip is run against fx_dm() FIRST. A test that cannot go green on
// the answer we already have is not evidence when it goes red on a candidate.

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
    for (int i = 0; i < n; i++) { heap.push_back({ rb[i], rs[i] }); }
    all = heap;
    all.push_back({ base, img });          // the module: descriptors live here

    // ---- control: the test must pass on the answer we already trust ---------
    uint64_t known = fx_dm();
    Shape ks{};
    bool ok = round_trip(known, ks);
    L("control  round_trip(fx_dm() = %llx) -> %s", (unsigned long long)known,
      ok ? "Instance" : "NOT an Instance");
    if (ok)
        L("control  children +%llx  end +%llx  parent +%llx  (%llu children)",
          (unsigned long long)ks.children, (unsigned long long)ks.cend,
          (unsigned long long)ks.parent, (unsigned long long)ks.count);
    if (!ok) { L("stop: the test is broken, not the search"); return; }

    // A second control, the other way: garbage must NOT pass.
    Shape junk{};
    L("control  round_trip(dm + 0x40)  -> %s",
      round_trip(known + 0x40, junk) ? "Instance (BAD - test is too loose)"
                                     : "rejected");

    // ---- level 0: the class-name string, module included --------------------
    std::vector<uint64_t> strings;
    sweep(all, 32, [&](uint64_t b, const unsigned char* p, size_t sz) {
        for (size_t i = 0; i + 0x20 <= sz; i += 8)
            if (memcmp(p + i, "DataModel\0", 10) == 0 &&
                qw(p, i + 0x10) == 9 && qw(p, i + 0x18) == 15)
                strings.push_back(b + i);
    });
    L("L0  \"DataModel\" std::strings: %zu", strings.size());
    if (strings.empty()) { L("stop: no anchor"); return; }

    // ---- level 1: pointers to it = descriptor + ClassName -------------------
    std::unordered_set<uint64_t> sset(strings.begin(), strings.end());
    std::vector<uint64_t> nameptrs;
    holders_of(sset, all, nameptrs, 100000);
    L("L1  pointers to it: %zu", nameptrs.size());

    std::unordered_set<uint64_t> descs;
    for (uint64_t h : nameptrs)
        for (uint64_t k = 0; k <= 0x40; k += 8) descs.insert(h - k);

    // ---- level 2: instances holding a descriptor ----------------------------
    std::vector<uint64_t> cdfields;
    holders_of(descs, heap, cdfields, 200000);
    L("L2  fields holding a descriptor: %zu", cdfields.size());

    std::unordered_set<uint64_t> tried;
    std::vector<uint64_t> insts;
    Shape shape{};
    for (uint64_t h : cdfields) {
        for (uint64_t j = 0; j <= 0x40; j += 8) {
            uint64_t inst = h - j;
            if (!tried.insert(inst).second) continue;
            Shape s{};
            if (round_trip(inst, s)) { insts.push_back(inst); shape = s; }
        }
    }
    L("L2  survived the round trip: %zu", insts.size());
    for (size_t i = 0; i < insts.size() && i < 8; i++)
        L("      %llx  %s", (unsigned long long)insts[i],
          insts[i] == known ? "<- the DataModel the console uses" : "");
    if (insts.empty()) { L("stop"); return; }

    uint64_t dm = 0;
    for (uint64_t i : insts) if (fx_rd(i + shape.parent) == 0) dm = i;
    if (!dm) dm = insts[0];
    L("picked root: %llx  (%s)", (unsigned long long)dm,
      dm == known ? "matches the console" : "DIFFERENT from the console");

    // ---- level 3/4: statics reaching it, verified forwards ------------------
    std::unordered_set<uint64_t> want{ dm };
    std::vector<uint64_t> h2;
    holders_of(want, heap, h2, 400000);
    L("L3  fields holding it: %zu", h2.size());

    std::unordered_set<uint64_t> objs;
    for (uint64_t h : h2)
        for (uint64_t m = 0; m <= 0x400; m += 8) objs.insert(h - m);

    std::vector<uint64_t> slots;
    holders_of(objs, std::vector<Range>{ { base, img } }, slots, 8192);

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
    L("L4  module slots: %zu   verified chains: %zu", slots.size(), good.size());
    for (size_t i = 0; i < good.size() && i < 20; i++)
        L("      rva %llx  +%llx", (unsigned long long)good[i].rva,
          (unsigned long long)good[i].m);

    // ---- grade --------------------------------------------------------------
    uint64_t want_rva = fx_off("FakeDataModel.Pointer");
    uint64_t want_m   = fx_off("FakeDataModel.RealDataModel");
    uint64_t want_cs  = fx_off("Instance.ChildrenStart");
    uint64_t want_ce  = fx_off("Instance.ChildrenEnd");
    uint64_t want_pa  = fx_off("Instance.Parent");
    bool hit = false;
    for (const Hit& g : good) if (g.rva == want_rva && g.m == want_m) hit = true;
    L("---");
    L("derived  children +%llx  end +%llx  parent +%llx",
      (unsigned long long)shape.children, (unsigned long long)shape.cend,
      (unsigned long long)shape.parent);
    L("conf     children +%llx  end +%llx  parent +%llx   %s",
      (unsigned long long)want_cs, (unsigned long long)want_ce,
      (unsigned long long)want_pa,
      (shape.children == want_cs && shape.cend == want_ce && shape.parent == want_pa)
        ? "MATCH" : "differ");
    L("conf     FakeDataModel %llx +%llx   -- chain %s",
      (unsigned long long)want_rva, (unsigned long long)want_m,
      hit ? "REPRODUCED" : "not reproduced");
}
