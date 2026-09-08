// Stage J -- hunting the ATTRIBUTE container offset on Instance.
//
// Every other stage here recovered something the conf already had, so the
// answer was checkable. This one is different: NOTHING in either conf names an
// attribute offset, so there is no key to grade against. That makes the
// controls the whole story -- a stage that cannot be graded has to prove its
// instruments instead of its result.
//
// THE PROBLEM. Attributes are the one place a game's OWN data lives on the
// engine side of the line rather than in a Luau table, so reaching them is
// worth a stage. But the usual anchor is missing: stage_a could sweep for
// "DataModel" because the class name must exist in memory. Here we do not know
// a single attribute NAME that exists in this place, so there is nothing to
// sweep backwards from.
//
// THE SUBSTITUTE ANCHOR is the SHAPE, not a value. Whatever the container is,
// it maps names to values, and a name is an MSVC std::string -- 16 bytes of
// buffer, then size, then capacity, with capacity == 15 for the short form.
// That is the same test stage_a used to prove the "DataModel" bytes were a
// string and not a coincidence, and it is strong: three fields have to agree.
//
// THE SEARCH, in two passes so it stays affordable:
//   1. read the first 0x400 bytes of every Instance we can reach ONCE, and for
//      each 8-aligned slot count how many instances hold a plausible heap
//      pointer there. No extra reads at all.
//   2. for the slots with the right PROFILE -- non-null on some instances but
//      not most, because most instances have no attributes -- follow the
//      pointer on a sample and look for string-shaped bytes behind it.
//
// WHAT WOULD MAKE THIS WRONG, and is checked below: Instance.Name already
// points at a string through NameContainer, so a "leads to a string" test on
// its own would happily nominate the name. Known offsets are excluded by name
// and reported separately, so a hit on one is visible rather than silent.

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <string>
#include <vector>

static void L(const char* fmt, ...) {
    char b[700];
    va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    fx_log(b);
}

static const size_t   SCAN_BYTES = 0x400;          // how far into an Instance to look
static const size_t   SLOTS      = SCAN_BYTES / 8;
static const int      MAX_INST   = 4000;
static const int      SAMPLE     = 40;             // instances followed per slot

// Heap, not module: class descriptors live at 0x7ff6xxxxxxxx and would pass a
// naive pointer test. The conf's own note about this cost a whole stage once.
static bool heapish(uint64_t p) {
    return p >= 0x10000ull && p < 0x7ff000000000ull;
}

// ---- the instrument: an MSVC std::string decoder ---------------------------
// char buf[16]; size_t size; size_t cap.  cap == 15 means the chars are inline;
// cap >= 16 means buf[0..7] is a pointer to them.
static bool decode_string(const unsigned char* b, std::string& out) {
    uint64_t size, cap;
    memcpy(&size, b + 16, 8);
    memcpy(&cap,  b + 24, 8);

    if (cap == 15) {
        if (size == 0 || size > 15) return false;
        for (uint64_t i = 0; i < size; i++)
            if (b[i] < 0x20 || b[i] > 0x7e) return false;
        if (b[size] != 0) return false;            // NUL terminated in place
        out.assign((const char*)b, (size_t)size);
        return true;
    }
    if (cap >= 16 && cap < 0x100000 && size > 0 && size <= cap) {
        uint64_t ptr;
        memcpy(&ptr, b, 8);
        if (!heapish(ptr)) return false;
        char tmp[256];
        uint64_t n = size < 200 ? size : 200;
        if (fx_rdblk(ptr, tmp, (size_t)n) != n) return false;
        for (uint64_t i = 0; i < n; i++)
            if (tmp[i] < 0x20 || tmp[i] > 0x7e) return false;
        out.assign(tmp, (size_t)n);
        return true;
    }
    return false;
}

// An attribute NAME is authored by a developer, so it looks like an
// identifier. This is what separates a real key from the engine telemetry
// strings that litter the heap -- "LogServiceJob" survives it, but
// "Mouse Move.....:", "Failed to load animation" and "Depth....:" do not,
// and those were most of the first run's output.
static bool identifier_like(const std::string& s) {
    if (s.size() < 2 || s.size() > 48) return false;
    if (!isalpha((unsigned char)s[0]) && s[0] != '_') return false;
    for (char c : s)
        if (!isalnum((unsigned char)c) && c != '_') return false;
    return true;
}

// Where in the pointed-to block a decodable identifier sits, or -1.
//
// The SUB-OFFSET is the point. A real container puts its key at a fixed place,
// so the same sub-offset recurs across unrelated instances; a pointer that
// merely lands near heap text gives a different one every time. Scoring the
// most common sub-offset is what turns "there is a string somewhere near here"
// -- which is true of most of the heap -- into evidence.
//
// TWO HOPS, because the container may be a handle rather than the storage: if
// nothing decodes directly, follow the first qword once and look again.
static int string_suboffset(uint64_t addr, std::string* first, int depth = 0) {
    unsigned char buf[256];
    if (fx_rdblk(addr, buf, sizeof buf) != sizeof buf) return -1;
    for (size_t o = 0; o + 32 <= sizeof buf; o += 8) {
        std::string s;
        if (decode_string(buf + o, s) && identifier_like(s)) {
            if (first) *first = s;
            return (int)o;
        }
    }
    if (depth == 0) {
        uint64_t nxt;
        memcpy(&nxt, buf, 8);
        if (heapish(nxt)) {
            int r = string_suboffset(nxt, first, 1);
            if (r >= 0) return 0x1000 + r;      // flagged as "via one hop"
        }
    }
    return -1;
}

// ---- the tree ---------------------------------------------------------------
static void collect(uint64_t root, std::vector<uint64_t>& out) {
    std::vector<uint64_t> frontier{ root };
    while (!frontier.empty() && (int)out.size() < MAX_INST) {
        std::vector<uint64_t> next;
        for (uint64_t inst : frontier) {
            if (!fx_running()) return;
            uint64_t n = fx_nchildren(inst);
            for (uint64_t i = 0; i < n && (int)out.size() < MAX_INST; i++) {
                uint64_t c = fx_child(inst, (int)i);
                if (!c || !heapish(c)) continue;
                out.push_back(c);
                next.push_back(c);
            }
        }
        frontier.swap(next);
    }
}

extern "C" void fx_run() {
    uint64_t dm = fx_dm();
    if (!dm) { L("no DataModel -- attach and join a place first"); return; }

    // ---- CONTROL 1: the string decoder, against a known-good input ----------
    // Instance.Name is reached through NameContainer. fx_name() already does
    // that correctly, so decoding the same bytes ourselves and comparing is a
    // test that can go red: if the decoder is wrong, this disagrees.
    uint64_t oNC = fx_off("Instance.NameContainer"), oNm = fx_off("Instance.Name");
    int dec_ok = 0, dec_tried = 0;
    std::vector<uint64_t> insts;
    collect(dm, insts);
    L("collected %d instances from the live tree", (int)insts.size());
    if (insts.empty()) { L("nothing to scan"); return; }

    for (int i = 0; i < (int)insts.size() && dec_tried < 200; i++) {
        const char* nm = fx_name(insts[i]);
        if (!nm || !*nm) continue;
        dec_tried++;
        unsigned char b[32];
        uint64_t nc = fx_rd(insts[i] + oNC);
        if (!heapish(nc)) continue;
        if (fx_rdblk(nc + oNm, b, 32) != 32) continue;
        std::string s;
        if (decode_string(b, s) && s == nm) dec_ok++;
    }
    L("CONTROL 1  decoder agrees with fx_name on %d/%d  %s",
      dec_ok, dec_tried, dec_ok > dec_tried / 2 ? "-> decoder TRUSTED" : "-> DECODER BROKEN, stop here");
    if (dec_ok <= dec_tried / 2) {
        L("  the instrument is wrong, so nothing below would mean anything.");
        return;
    }

    // ---- CONTROL 2: it must REJECT things that are not strings -------------
    int false_pos = 0;
    for (int i = 0; i < 200 && i < (int)insts.size(); i++) {
        unsigned char b[32];
        // The Instance header itself: vtable and pointers, never a string.
        if (fx_rdblk(insts[i], b, 32) != 32) continue;
        std::string s;
        if (decode_string(b, s)) false_pos++;
    }
    L("CONTROL 2  decoder rejects Instance headers: %d false positives %s",
      false_pos, false_pos == 0 ? "-> good" : "-> too loose, results are suspect");

    // ---- PASS 1: which slots hold a pointer, and how often ------------------
    std::vector<int> nonnull(SLOTS, 0), readable(SLOTS, 0);
    std::vector<std::vector<uint64_t>> examples(SLOTS);
    std::vector<unsigned char> buf(SCAN_BYTES);
    int scanned = 0;

    for (uint64_t inst : insts) {
        if (!fx_running()) break;
        if (fx_rdblk(inst, buf.data(), SCAN_BYTES) != SCAN_BYTES) continue;
        scanned++;
        for (size_t k = 0; k < SLOTS; k++) {
            uint64_t p;
            memcpy(&p, buf.data() + k * 8, 8);
            if (p) nonnull[k]++;
            if (heapish(p)) {
                readable[k]++;
                if (examples[k].size() < (size_t)SAMPLE) examples[k].push_back(p);
            }
        }
    }
    L("PASS 1  scanned %d instances, %d slots each", scanned, (int)SLOTS);
    if (!scanned) { L("no instance could be read whole"); return; }

    // ---- PASS 2: of the plausible slots, which lead to STRINGS? ------------
    // The profile that matters: present on SOME instances but not nearly all.
    // A field every instance has is structural (Parent, children, descriptor);
    // attributes are the exception rather than the rule, so a slot that is set
    // on 1-50% of instances is the interesting shape.
    struct Hit { size_t k; int ptr_n; int agree; int tried; int sub; std::string sample; };
    std::vector<Hit> hits;

    for (size_t k = 0; k < SLOTS; k++) {
        double frac = (double)readable[k] / scanned;
        if (readable[k] < 3) continue;
        if (frac > 0.60) continue;                 // too common to be attributes

        // Tally the sub-offset each sample's identifier was found at.
        std::vector<std::pair<int,int>> tally;     // (sub-offset, count)
        std::string sample;
        int tried = 0;
        for (uint64_t p : examples[k]) {
            if (!fx_running()) break;
            tried++;
            std::string s;
            int sub = string_suboffset(p, &s);
            if (sub < 0) continue;
            bool seen = false;
            for (auto& t : tally) if (t.first == sub) { t.second++; seen = true; break; }
            if (!seen) tally.push_back({ sub, 1 });
            if (sample.empty()) sample = s;
        }
        if (tally.empty() || !tried) continue;

        // The winner is the most common sub-offset; how OFTEN it wins is the
        // score. Scattered sub-offsets mean coincidence, not structure.
        int best_sub = -1, best_n = 0;
        for (auto& t : tally) if (t.second > best_n) { best_n = t.second; best_sub = t.first; }
        if (best_n < 3) continue;
        hits.push_back(Hit{ k, readable[k], best_n, tried, best_sub, sample });
    }

    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        double fa = (double)a.agree / a.tried, fb = (double)b.agree / b.tried;
        if (fa != fb) return fa > fb;
        return a.agree > b.agree;
    });

    // Known offsets, so a hit on one is named rather than mistaken for a find.
    struct Known { const char* key; uint64_t off; };
    Known known[] = {
        { "Instance.ClassDescriptor", fx_off("Instance.ClassDescriptor") },
        { "Instance.Parent",          fx_off("Instance.Parent") },
        { "Instance.ChildrenStart",   fx_off("Instance.ChildrenStart") },
        { "Instance.NameContainer",   fx_off("Instance.NameContainer") },
        { "Instance.ClassBase",       fx_off("Instance.ClassBase") },
    };

    L(" ");
    L("PASS 2  slots pointing at an identifier held at a CONSISTENT sub-offset:");
    L("%-8s %-7s %-9s %-12s %-24s %s",
      "offset", "on", "agree", "sub-offset", "example identifier", "known as");
    int shown = 0;
    for (const Hit& h : hits) {
        if (shown++ >= 20) break;
        uint64_t off = h.k * 8;
        const char* as = "";
        for (const Known& kn : known)
            if (kn.off != (uint64_t)-1 && kn.off == off) as = kn.key;
        char pct[16], agree[24], sub[24];
        snprintf(pct, sizeof pct, "%d%%", (int)(100.0 * h.ptr_n / scanned));
        snprintf(agree, sizeof agree, "%d/%d", h.agree, h.tried);
        if (h.sub >= 0x1000) snprintf(sub, sizeof sub, "+0x%x hop", h.sub - 0x1000);
        else                 snprintf(sub, sizeof sub, "+0x%x", h.sub);
        L("+0x%-5llx %-7s %-9s %-12s %-24s %s",
          (unsigned long long)off, pct, agree, sub,
          h.sample.substr(0, 23).c_str(), as);
    }
    if (!shown) L("  none -- no slot in the first 0x%llx bytes matched the profile",
                  (unsigned long long)SCAN_BYTES);

    L(" ");
    L("READ THIS BEFORE BELIEVING ANY ROW ABOVE:");
    L("  A row only says 'this slot points at memory containing a string'.");
    L("  That is NOT 'this is the attribute map' -- it is the shortlist. The");
    L("  next step is to confirm ONE candidate holds name/value PAIRS, and");
    L("  that the same offset works on instances from an unrelated place.");
}
