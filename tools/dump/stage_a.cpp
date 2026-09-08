// Stage A of a self-hosted offset dumper.
//
// Recovers the FakeDataModel static -- the one offset that changes on EVERY
// Roblox build and the reason a stale conf is fatal -- using nothing but the
// module base and the client's own memory. No conf offsets are read until the
// very end, and then only to GRADE the answer against what we already trust.
//
// The method is a pointer scan run backwards from a string we know must exist:
//
//   "DataModel" (a std::string, so size==9 && cap==15 confirms it is one)
//     <- +k   ClassDescriptor            (k recovered, conf says 0x8)
//     <- +j   Instance.ClassDescriptor   (j recovered, conf says 0x18)
//     <- +m   FakeDataModel.RealDataModel(m recovered, conf says 0x1d0)
//     <- a qword inside the module       => the static, as an RVA
//
// Each level is one sweep of committed memory collecting "addresses whose
// qword lands in the previous level's candidate set". Nothing here assumes a
// struct layout; the offsets fall out of the search as the k/j/m that worked.

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

static void L(const char* fmt, ...) {
    char b[512];
    va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    fx_log(b);
}

struct Range { uint64_t base, size; };

// ---- sweeping ---------------------------------------------------------------
// A failed read is a page that went away between the region query and now,
// which is normal in a live process. Fall back to page-sized reads so one bad
// page costs a page, not the megabyte around it.
template <class F>
static uint64_t sweep(const std::vector<Range>& rs, size_t overlap, F fn) {
    const size_t CHUNK = 1u << 20;
    std::vector<unsigned char> buf(CHUNK);
    uint64_t bytes = 0;

    for (const Range& r : rs) {
        uint64_t pos = r.base, end = r.base + r.size;
        while (pos < end) {
            if (!fx_running()) return bytes;
            size_t want = (size_t)((end - pos < CHUNK) ? (end - pos) : CHUNK);
            size_t got  = fx_rdblk(pos, buf.data(), want);
            if (got == 0) {
                for (uint64_t p = pos; p < pos + want; p += 4096) {
                    size_t g = fx_rdblk(p, buf.data(), 4096);
                    if (g) { fn(p, buf.data(), g); bytes += g; }
                }
            } else {
                fn(pos, buf.data(), got);
                bytes += got;
            }
            if (want <= overlap) break;
            pos += want - overlap;
        }
    }
    return bytes;
}

// Every 8-aligned qword in a chunk whose value is in `want`, reported as the
// ADDRESS of the qword. That address minus a small unknown offset is the object
// that holds it, which is the next level up.
static void collect_holders(const std::unordered_set<uint64_t>& want,
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

// Candidate object bases for a set of holder addresses: the holder minus every
// plausible field offset. This is where the struct offsets come from -- we do
// not know which one is right, so we carry them all and let the next level
// decide by finding a pointer to one of them.
static void expand(const std::vector<uint64_t>& holders, uint64_t maxoff,
                   std::unordered_set<uint64_t>& out,
                   std::unordered_map<uint64_t, uint64_t>& off_of) {
    for (uint64_t h : holders)
        for (uint64_t k = 0; k <= maxoff; k += 8) {
            uint64_t obj = h - k;
            if (out.insert(obj).second) off_of[obj] = k;
        }
}

extern "C" void fx_run() {
    uint64_t base = fx_base();
    if (!base) { L("no module base -- not attached"); return; }

    // SizeOfImage out of the PE header, so the module sweep covers the whole
    // image rather than a guess. e_lfanew at +0x3c; SizeOfImage is 0x50 into
    // the NT headers (4 signature + 20 file header + 0x38 optional header).
    uint32_t lfanew = fx_rd32(base + 0x3c);
    uint32_t img    = lfanew ? fx_rd32(base + lfanew + 0x50) : 0;
    if (!img || img > 0x40000000u) { L("bad PE header at base"); return; }
    L("module  %llx  size %.1f MB", (unsigned long long)base, img / 1048576.0);

    int n = fx_regions(nullptr, nullptr, 0);
    std::vector<uint64_t> rb(n), rs(n);
    fx_regions(rb.data(), rs.data(), n);
    std::vector<Range> heap;
    uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        heap.push_back({ rb[i], rs[i] });
        total += rs[i];
    }
    std::vector<Range> module_only{ { base, img } };
    L("%d writable regions, %.0f MB committed", n, total / 1048576.0);

    // ---- level 0: the string ------------------------------------------------
    const char* NEEDLE = "DataModel";
    const size_t NLEN  = 9;
    std::vector<uint64_t> strings;
    sweep(heap, 32, [&](uint64_t b, const unsigned char* p, size_t sz) {
        if (sz < 0x20) return;
        for (size_t i = 0; i + 0x20 <= sz; i += 8) {
            if (memcmp(p + i, NEEDLE, NLEN + 1) != 0) continue;   // + the NUL
            uint64_t len, cap;
            memcpy(&len, p + i + 0x10, 8);
            memcpy(&cap, p + i + 0x18, 8);
            if (len == NLEN && cap == 15) strings.push_back(b + i);
        }
    });
    L("L0  std::string \"%s\": %zu", NEEDLE, strings.size());
    if (strings.empty()) { L("stop: no such string -- nothing to anchor on"); return; }

    // ---- level 1: descriptors that hold it ----------------------------------
    std::unordered_set<uint64_t> descs;
    std::unordered_map<uint64_t, uint64_t> desc_k;
    for (uint64_t s : strings)
        for (uint64_t k = 0; k <= 0x40; k += 8)
            if (descs.insert(s - k).second) desc_k[s - k] = k;

    std::vector<uint64_t> h1;
    collect_holders(descs, heap, h1, 200000);
    L("L1  fields pointing at a descriptor: %zu", h1.size());
    if (h1.empty()) { L("stop: nothing points at the descriptor"); return; }

    // ---- level 2: instances that hold a descriptor --------------------------
    std::unordered_set<uint64_t> insts;
    std::unordered_map<uint64_t, uint64_t> inst_j;
    expand(h1, 0x40, insts, inst_j);

    std::vector<uint64_t> h2;
    collect_holders(insts, heap, h2, 400000);
    L("L2  fields pointing at an instance: %zu", h2.size());
    if (h2.empty()) { L("stop: nothing points at the instance"); return; }

    // ---- level 3: the object the static points at ---------------------------
    std::unordered_set<uint64_t> fakes;
    std::unordered_map<uint64_t, uint64_t> fake_m;
    expand(h2, 0x400, fakes, fake_m);
    L("L3  candidate objects: %zu", fakes.size());

    // ---- level 4: a qword inside the module pointing at one of them ---------
    std::vector<uint64_t> h3;
    collect_holders(fakes, module_only, h3, 4096);
    L("L4  module slots: %zu", h3.size());
    if (h3.empty()) { L("stop: no static reaches it"); return; }

    for (size_t i = 0; i < h3.size() && i < 12; i++) {
        uint64_t slot = h3[i];
        uint64_t fake = fx_rd(slot);
        L("    rva %llx  ->  %llx   RealDataModel +%llx",
          (unsigned long long)(slot - base), (unsigned long long)fake,
          (unsigned long long)fake_m[fake]);
    }

    // ---- grade --------------------------------------------------------------
    // Only now is the conf consulted, and only to score what the scan found.
    uint64_t want_rva = fx_off("FakeDataModel.Pointer");
    bool hit = false;
    for (uint64_t slot : h3) if (slot - base == want_rva) hit = true;
    L("---");
    L("conf FakeDataModel.Pointer = %llx", (unsigned long long)want_rva);
    L("scan %s it", hit ? "FOUND" : "MISSED");
}
