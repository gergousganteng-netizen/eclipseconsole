#include "dex.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Hard ceilings. Every one of these bounds a walk over a count that comes from
// the game, and a game-derived count can be garbage -- an unbounded loop over
// one is a hang, not a wrong number.
// MAX_ROWS is a PAYLOAD budget, not a display one: every visible row is pushed
// to the page as JSON, and that is where the previous explorer's "very slow"
// came from.
//
// It was 1200, which was too tight and failed INVISIBLY. The budget is spent
// depth-first from the top, so expanding something large near the root ate it
// all and every twisty below the cut flipped open onto nothing -- the row
// counter still read "1200 rows" and gave no hint anything had been dropped.
// Measured: a few services open wants 2088 rows, so 1200 was cutting a normal
// browsing state almost in half.
//
// Affordable now because build_js only sends the rows when the tree's SHAPE
// changes; a periodic value refresh sends a few KB instead of the whole list.
// At this ceiling a full send measured 65 ms / 203 KB, once per click.
// Truncation is reported rather than silent -- see rows_truncated.
static const int MAX_ROWS      = 4000;
static const int MAX_KIDS      = 4096;   // children drawn under one node
static const int SEARCH_NODES  = 60000;  // nodes a search may visit
// Each hit expands every ancestor, so hits multiply rows. 81 hits on "Humanoid"
// opened 3853 rows -- past useful and past affordable.
static const int SEARCH_HITS   = 40;
// The tree bottoms out at 25 levels in Bad Business, so 24 clipped the last
// one. Kept well clear of the measurement rather than exactly at it.
static const int MAX_DEPTH     = 32;

// ---- value decoding -------------------------------------------------------------

// The payload lives at the generic ValueBase slot, but the TYPE there varies and
// the CLASS is the only thing that says which. Guessing the width is how this
// project once read Bad Business's health -- a double -- as a float, got 0.0,
// and filtered every living player out of the game as dead.
std::string value_of(uint64_t inst, const char* cls) {
    if (!cls || !strstr(cls, "Value")) return "";
    uint64_t vo = fx_off("Misc.Value");
    if (vo == (uint64_t)-1) return "";
    uint64_t at = inst + vo;
    char b[160];

    if (!strcmp(cls, "BoolValue")) {
        unsigned char v = 0; fx_rdblk(at, &v, 1);
        return v ? "true" : "false";
    }
    if (!strcmp(cls, "NumberValue")) {
        double d = 0; fx_rdblk(at, &d, 8);
        snprintf(b, sizeof b, "%.4g", d); return b;
    }
    if (!strcmp(cls, "IntValue")) {
        long long v = 0; fx_rdblk(at, &v, 8);
        snprintf(b, sizeof b, "%lld", v); return b;
    }
    if (!strcmp(cls, "StringValue")) {
        char s[96] = {0};
        fx_str(at, s, sizeof s);
        snprintf(b, sizeof b, "\"%s\"", s); return b;
    }
    if (!strcmp(cls, "ObjectValue")) {
        uint64_t p = fx_rd(at);
        if (p < 0x10000 || p > 0x7fffffffffffULL) return "nil";
        const char* nm = fx_name(p);
        snprintf(b, sizeof b, "-> %s (%s)", (nm && *nm) ? nm : "?", fx_class(p));
        return b;
    }
    if (!strcmp(cls, "Color3Value")) {
        float3 c = fx_rf3(at);
        snprintf(b, sizeof b, "%.2f %.2f %.2f", c.x, c.y, c.z); return b;
    }
    if (!strcmp(cls, "Vector3Value")) {
        float3 v = fx_rf3(at);
        snprintf(b, sizeof b, "%.1f %.1f %.1f", v.x, v.y, v.z); return b;
    }
    // A Value class we have no decoder for. Say so rather than print a number
    // read at a width we have not established -- an unlabelled wrong value is
    // worse than a blank.
    return "?";
}

static bool is_basepart(const char* c) {
    static const char* ok[] = { "Part", "MeshPart", "UnionOperation", "TrussPart",
                                "WedgePart", "CornerWedgePart", "SpawnLocation",
                                "Seat", "VehicleSeat", "BasePart", "Terrain" };
    for (const char* k : ok) if (strcmp(c, k) == 0) return true;
    return false;
}

// ---- rows ---------------------------------------------------------------------

static void walk(const State& st, uint64_t inst, int depth, std::vector<Row>& out) {
    if ((int)out.size() >= MAX_ROWS || depth > MAX_DEPTH || !fx_running()) return;

    uint64_t n = fx_nchildren(inst);
    if (n > MAX_KIDS) n = MAX_KIDS;

    for (uint64_t i = 0; i < n; i++) {
        if ((int)out.size() >= MAX_ROWS) return;
        uint64_t c = fx_child(inst, (int)i);
        if (!c) continue;

        // Tidy mode, at the ROOT only: the DataModel's direct children are ~148
        // services and most are empty shells. Hiding the childless ones makes
        // the first screen the game rather than the engine. Never applied
        // deeper, where an empty node is usually the thing you are looking for.
        if (st.tidy && depth == 1 && fx_nchildren(c) == 0) continue;

        Row r;
        r.depth = depth;
        r.addr  = c;
        r.cls   = fx_class(c);
        const char* nm = fx_name(c);
        r.name  = (nm && *nm) ? nm : "";
        // One read to know whether to draw a twisty -- the whole reason a big
        // tree stays cheap. Never a walk just to ask "does this have children".
        r.kids  = fx_nchildren(c) > 0;
        r.open  = st.expanded.count(c) != 0;
        r.sel   = (c == st.sel);
        r.val   = value_of(c, r.cls.c_str());
        out.push_back(r);

        if (r.open && r.kids) walk(st, c, depth + 1, out);
    }
}

std::vector<Row> build_rows(State& st) {
    std::vector<Row> rows;
    st.rows_truncated = false;
    if (!st.root) return rows;

    Row r;
    r.depth = 0;
    r.addr  = st.root;
    r.cls   = fx_class(st.root);
    const char* nm = fx_name(st.root);
    r.name  = (nm && *nm) ? nm : "game";
    r.kids  = fx_nchildren(st.root) > 0;
    r.open  = st.expanded.count(st.root) != 0;
    r.sel   = (st.root == st.sel);
    rows.push_back(r);

    if (r.open) walk(st, st.root, 1, rows);
    // Landing exactly on the ceiling means the walk stopped early -- there is no
    // cheap way to know how much was left, and "some was" is the part that
    // matters. Reported, because a silent cut looks like an empty folder.
    st.rows_truncated = ((int)rows.size() >= MAX_ROWS);
    return rows;
}

// ---- path ----------------------------------------------------------------------
// Walked UPWARD via Instance.Parent rather than threaded down through the
// recursion: the row list is rebuilt constantly and carrying a string per row
// costs more than resolving one path for the single selected node.
std::string path_of(const State& st, uint64_t inst) {
    uint64_t oPar = fx_off("Instance.Parent");
    if (oPar == (uint64_t)-1 || !inst) return "";

    std::string parts[MAX_DEPTH];
    int n = 0;
    uint64_t cur = inst;
    while (cur && n < MAX_DEPTH) {
        const char* nm = fx_name(cur);
        parts[n++] = (nm && *nm) ? nm : fx_class(cur);
        if (cur == st.root) break;
        uint64_t par = fx_rd(cur + oPar);
        if (par < 0x10000 || par > 0x7fffffffffffULL) break;
        cur = par;
    }
    std::string out;
    for (int i = n - 1; i >= 0; i--) { out += parts[i]; if (i) out += "."; }
    return out;
}

// ---- detail --------------------------------------------------------------------

// Which conf prefixes apply to an instance of this class.
//
// The conf is keyed by BASE class -- GuiObject.Text, BasePart.Transparency,
// Instance.Name -- but an instance's class is the concrete one: TextLabel,
// Part, Humanoid. Matching only the exact name meant a TextLabel showed NO
// fields at all and a Part hid every BasePart property, which made the detail
// pane look broken on most of the tree.
//
// Roblox's real inheritance graph is not available to us, so this is the short
// hand-written version covering the bases the conf actually uses. A class we do
// not recognise still gets its own name and Instance, which is never wrong --
// only incomplete.
static void bases_for(const std::string& cls, std::vector<std::string>& out) {
    out.push_back(cls);
    if (cls != "Instance") out.push_back("Instance");

    if (is_basepart(cls.c_str())) out.push_back("BasePart");

    static const char* gui[] = {
        "TextLabel", "TextButton", "TextBox", "ImageLabel", "ImageButton",
        "Frame", "ScrollingFrame", "ViewportFrame", "VideoFrame", "CanvasGroup",
        "BillboardGui", "ScreenGui", "SurfaceGui", "GuiObject",
    };
    for (const char* g : gui)
        if (cls == g) { out.push_back("GuiObject"); out.push_back("GuiBase2D"); break; }
}

// Every conf key for this class, with the live bytes at each offset.
//
// This is the view the whole tool exists for. Both widths are always shown
// because reading a double as a float returns 0.0, which is indistinguishable
// from a real zero and has cost this project hours more than once -- BB's
// health is a double and read as a float it looked like a corpse.
std::string detail_json(uint64_t inst) {
    std::string s = "{";
    char b[512];

    // COPIED, not borrowed. fx_class returns a pointer into one thread_local
    // buffer that every later fx_class call overwrites -- and this function
    // calls it again for each pointer field. Holding the raw pointer meant that
    // after the first resolved pointer, `cls` silently became the TARGET's
    // class, so no conf key matched and the field list came back empty. It
    // looked like the conf knowing nothing about Player.
    const std::string cls = fx_class(inst);
    const std::string nm  = fx_name(inst);
    std::vector<std::string> bases;
    bases_for(cls, bases);

    snprintf(b, sizeof b, "\"addr\":\"0x%llx\",\"cls\":\"%s\",\"nkids\":%llu,",
             (unsigned long long)inst, cls.c_str(),
             (unsigned long long)fx_nchildren(inst));
    s += b;
    s += "\"name\":\"";
    std::string esc; json_escape(nm, esc);
    s += esc;
    s += "\",\"fields\":[";

    int emitted = 0;
    for (int i = 0, n = fx_off_count(); i < n && fx_running(); i++) {
        const char* k = fx_off_key(i);
        if (!k) continue;
        const char* dot = strchr(k, '.');
        if (!dot) continue;
        size_t clen = (size_t)(dot - k);

        bool mine = false;
        for (const std::string& base : bases) {
            if (base.size() == clen && strncmp(k, base.c_str(), clen) == 0) { mine = true; break; }
        }
        // A BasePart's physics fields live on its Primitive, a separate
        // allocation, so show those too rather than making you chase the
        // pointer by hand.
        bool prim = is_basepart(cls.c_str()) && clen == 9 &&
                    strncmp(k, "Primitive", 9) == 0;
        if (!mine && !prim) continue;

        uint64_t off = fx_off(k);
        if (off == (uint64_t)-1) continue;

        uint64_t base = inst;
        if (prim) {
            uint64_t op = fx_off("BasePart.Primitive");
            if (op == (uint64_t)-1) continue;
            base = fx_rd(inst + op);
            if (base < 0x10000 || base > 0x7fffffffffffULL) continue;
        }

        uint64_t at = base + off;
        float  f32 = fx_rf(at);
        double f64 = 0;  fx_rdblk(at, &f64, 8);
        int    i32 = 0;  fx_rdblk(at, &i32, 4);
        uint64_t p = fx_rd(at);

        // A pointer is only worth showing as one if it resolves to something
        // with a class -- otherwise every float looks like an address.
        const char* pcls = "";
        if (p >= 0x10000 && p <= 0x7fffffffffffULL) pcls = fx_class(p);

        // Does this field hold TEXT?
        //
        // The conf stores an offset and no type, so a std::string field --
        // GuiObject.Text is the one that matters -- reads as nonsense in the
        // numeric columns. Which is a real gap: a game's ammo, score and timer
        // usually live in a HUD TextLabel long before they live anywhere we can
        // name, and that label is often the only readable copy.
        //
        // No type to consult, so this is a guess that has to be conservative:
        // decode it, and only believe it if every byte is printable. Anything
        // else stays blank rather than showing mojibake next to real numbers.
        std::string text;
        {
            char sb[96] = {0};
            size_t n2 = fx_str(at, sb, sizeof sb);
            if (n2 > 0 && n2 < sizeof sb) {
                bool printable = true;
                for (size_t z = 0; z < n2; z++) {
                    unsigned char ch = (unsigned char)sb[z];
                    if (ch < 0x20 || ch > 0x7e) { printable = false; break; }
                }
                if (printable) { std::string e2; json_escape(sb, e2); text = e2; }
            }
        }

        if (emitted++) s += ",";
        // "tgt" is the pointer's target, so the page can offer to jump there.
        // Only emitted when it resolved to something with a class -- otherwise
        // every float that happens to look like an address becomes a link.
        snprintf(b, sizeof b,
                 "{\"k\":\"%s\",\"off\":\"0x%llx\",\"prim\":%d,\"addr\":\"%llx\","
                 "\"f32\":%.4f,\"f64\":%.4f,\"i32\":%d,\"ptr\":\"%s\",\"tgt\":\"\",\"str\":\"",
                 dot + 1, (unsigned long long)off, prim ? 1 : 0,
                 (unsigned long long)at, f32, f64, i32,
                 (pcls && *pcls) ? pcls : "");
        s += b;
        s += text;
        s += "\"}";
        // Patch the target in separately: snprintf above would need a second
        // %llx conditional on pcls, and a wrong link is worse than none.
        if (pcls && *pcls) {
            char t[48];
            snprintf(t, sizeof t, "%llx", (unsigned long long)p);
            size_t q = s.rfind("\"tgt\":\"");
            if (q != std::string::npos) s.insert(q + 7, t);
        }
    }
    s += "]}";
    return s;
}

// ---- address -> object ------------------------------------------------------------

// How far into an object an address may sit and still belong to it. Too small
// misses fields at the tail; too large attributes a hit to the object before it.
static const uint64_t WINDOW = 0x900;
// Measured 184512 instances in a full Bad Business round, walked in 1.58s. The
// cap is headroom over that, not a guess -- and it is now reported when hit.
static const int      INDEX_CAP = 400000;

Hit locate(uint64_t addr) {
    Hit h;
    uint64_t dm = fx_dm();
    if (!dm || !addr) return h;

    struct Ent { uint64_t base; bool prim; uint64_t inst; };
    std::vector<Ent> idx;
    idx.reserve(40000);

    struct F { uint64_t inst; int depth; };
    std::vector<F> stack;
    stack.push_back({ dm, 0 });
    uint64_t oPrim = fx_off("BasePart.Primitive");
    int seen = 0;

    while (!stack.empty() && seen < INDEX_CAP && fx_running()) {
        F f = stack.back();
        stack.pop_back();
        uint64_t n = fx_nchildren(f.inst);
        if (n > MAX_KIDS) n = MAX_KIDS;
        for (uint64_t i = 0; i < n && seen < INDEX_CAP; i++) {
            uint64_t c = fx_child(f.inst, (int)i);
            if (!c) continue;
            seen++;
            idx.push_back({ c, false, c });

            // The physics object is a separate allocation and is where a scan
            // for a coordinate actually lands. Only for real BaseParts: reading
            // Primitive on anything else returns something pointer-shaped and
            // produced an index entry based at 0x800 the first time this was
            // written.
            if (oPrim != (uint64_t)-1 && is_basepart(fx_class(c))) {
                uint64_t p = fx_rd(c + oPrim);
                if (p >= 0x10000 && p <= 0x7fffffffffffULL)
                    idx.push_back({ p, true, c });
            }
            if (f.depth + 1 < MAX_DEPTH) stack.push_back({ c, f.depth + 1 });
        }
    }
    if (idx.empty()) return h;

    std::sort(idx.begin(), idx.end(),
              [](const Ent& a, const Ent& b) { return a.base < b.base; });

    size_t lo = 0, hi = idx.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (idx[mid].base <= addr) lo = mid + 1; else hi = mid;
    }
    if (lo == 0) return h;
    const Ent& e = idx[lo - 1];
    uint64_t off = addr - e.base;
    if (off >= WINDOW) return h;

    h.found = true;
    h.inst  = e.inst;
    h.off   = off;
    h.prim  = e.prim;
    h.cls   = e.prim ? "Primitive" : fx_class(e.inst);

    // Name the offset from the conf rather than a hand-written table.
    for (int i = 0, n = fx_off_count(); i < n; i++) {
        const char* k = fx_off_key(i);
        if (!k) continue;
        const char* dot = strchr(k, '.');
        if (!dot) continue;
        size_t clen = (size_t)(dot - k);
        if (clen != h.cls.size() || strncmp(k, h.cls.c_str(), clen) != 0) continue;
        if (fx_off(k) == off) { h.field = dot + 1; break; }
    }
    return h;
}

// ---- scanning inside known objects ------------------------------------------------

static const int    MAX_CANDS   = 40000;
static const size_t OBJ_BYTES   = 0x900;   // how much of an object to sweep

// Read one slot at the scan type. Returns false for a width we do not scan.
static bool slot(const unsigned char* buf, size_t off, const std::string& t, double* out) {
    if (t == "f32") { float  v; memcpy(&v, buf + off, 4); if (v != v) return false; *out = v; return true; }
    if (t == "f64") { double v; memcpy(&v, buf + off, 8); if (v != v) return false; *out = v; return true; }
    if (t == "i32") { int    v; memcpy(&v, buf + off, 4); *out = (double)v; return true; }
    // HOW TO KNOW A ptr SCAN IS WORKING, before believing an empty result:
    // every child holds its parent at Instance.Parent, so "who points here" on
    // any object with N children MUST come back with at least N hits at +0x68,
    // all named Instance.Parent. If that control comes back empty the scan is
    // broken, and "nothing references this object" means nothing.
    //
    // ptr: an 8-byte value compared exactly. A double carries this losslessly --
    // user-space addresses here top out at 0x7fffffffffff (47 bits), well under
    // the 53 a double holds exactly -- so the comparison below is not fuzzy.
    if (t == "ptr") { uint64_t v; memcpy(&v, buf + off, 8); *out = (double)v; return true; }
    return false;
}
static size_t slot_size(const std::string& t) {
    return (t == "f64" || t == "ptr") ? 8 : 4;
}

// Every instance base in the tree. Shared by the scanner; the lazy row walk
// deliberately never builds this, so it is done on demand and thrown away.
//
// BREADTH-FIRST, and that is not a style preference. Depth-first with a cap
// spends the whole budget on whichever subtree it entered first: measured in
// Bad Business it indexed 16 of the DataModel's 145 services, and Players was
// not among them -- so a "who points here" scan on the Players service found
// its own self-pointer and none of the seven children that point at it. The
// empty result looked like an answer about the game. Breadth-first spends the
// budget evenly, so a truncated index loses the deepest leaves instead.
//
// Returns false if the cap cut the walk short. Callers must say so; silent
// truncation is indistinguishable from a clean empty result.
static bool all_instances(std::vector<uint64_t>& out) {
    uint64_t dm = fx_dm();
    if (!dm) return true;
    std::vector<uint64_t> cur, next;
    cur.push_back(dm);
    for (int depth = 0; depth < MAX_DEPTH && !cur.empty() && fx_running(); depth++) {
        next.clear();
        for (uint64_t p : cur) {
            uint64_t n = fx_nchildren(p);
            if (n > MAX_KIDS) n = MAX_KIDS;
            for (uint64_t i = 0; i < n; i++) {
                uint64_t c = fx_child(p, (int)i);
                if (!c) continue;
                if ((int)out.size() >= INDEX_CAP) return false;
                out.push_back(c);
                next.push_back(c);
            }
        }
        cur.swap(next);
    }
    return true;
}

int scan_first(State& st, const std::string& type, const std::string& text) {
    st.cands.clear();
    st.scan_type = type;
    double want = atof(text.c_str());
    // A pointer is written in hex, with or without the 0x -- nobody types an
    // instance address in decimal.
    if (type == "ptr") {
        const char* p = text.c_str();
        while (*p == ' ') p++;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        want = (double)strtoull(p, nullptr, 16);
    }
    size_t step = slot_size(type);
    if (!step) return 0;
    // Pointers are 8-aligned. Sweeping them on the 4-byte grid would both halve
    // the speed and manufacture hits straddling two unrelated fields.
    size_t grid = (type == "ptr") ? 8 : 4;

    std::vector<uint64_t> objs;
    st.scan_partial = !all_instances(objs);
    st.scan_indexed = (int)objs.size();

    // CLAMP EACH WINDOW TO THE NEXT OBJECT.
    //
    // OBJ_BYTES cannot just be made smaller: real fields live past it either way
    // (GuiObject.Text is at 0xdf8). But 88% of adjacent instance bases are less
    // than 0x900 apart, so a fixed window reads the NEXT object's bytes and
    // credits them to this one. That is how a single stored pointer came back as
    // 14 hits at 14 different offsets in objects of the same class -- 14 windows
    // overlapping one address, and not one of them a real field.
    //
    // The next instance base is an upper bound on this object's size (a
    // non-instance allocation may sit between), so this never drops a hit that
    // was genuinely inside the object -- it only stops crediting one elsewhere.
    //
    // It does not eliminate the problem, and results must be read knowing that:
    // ~12% of objects have no instance within 0x900, so those still sweep into
    // whatever follows them. A hit is only a FIELD if the same class and offset
    // repeat across objects; a single hit at an odd offset is very likely the
    // neighbour's data wearing this object's name. That mistake produced a
    // convincing but entirely false character-to-player mapping in Bad Business.
    std::vector<uint64_t> sorted = objs;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());

    std::vector<unsigned char> buf(OBJ_BYTES);
    for (uint64_t inst : objs) {
        if (!fx_running() || (int)st.cands.size() >= MAX_CANDS) break;

        size_t win = OBJ_BYTES;
        auto nx = std::upper_bound(sorted.begin(), sorted.end(), inst);
        if (nx != sorted.end() && *nx - inst < win) win = (size_t)(*nx - inst);
        if (win < step) continue;

        // ONE read per object rather than one per slot. Per-slot would be nine
        // million reads for the whole game.
        size_t got = fx_rdblk(inst, buf.data(), win);
        if (got < step) continue;
        for (size_t o = 0; o + step <= got; o += grid) {
            double v;
            if (!slot(buf.data(), o, type, &v)) continue;
            // Exact on integers, tolerant on floats: a float typed as "100" is
            // never bit-identical to what the game computed.
            bool hit = (type == "i32" || type == "ptr") ? (v == want)
                                       : (v > want - 0.001 && v < want + 0.001);
            if (!hit) continue;
            Cand c; c.inst = inst; c.off = o; c.last = v;
            st.cands.push_back(c);
            if ((int)st.cands.size() >= MAX_CANDS) break;
        }
    }
    return (int)st.cands.size();
}

int scan_narrow(State& st, const std::string& op, const std::string& text) {
    double want = atof(text.c_str());
    if (st.scan_type == "ptr") {
        const char* p = text.c_str();
        while (*p == ' ') p++;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        want = (double)strtoull(p, nullptr, 16);
    }
    size_t step = slot_size(st.scan_type);
    std::vector<Cand> keep;
    keep.reserve(st.cands.size());

    unsigned char b[8];
    for (Cand& c : st.cands) {
        if (!fx_running()) break;
        if (fx_rdblk(c.inst + c.off, b, step) != step) continue;   // object gone
        double v;
        if (!slot(b, 0, st.scan_type, &v)) continue;

        bool ok = false;
        if      (op == "eq") ok = (st.scan_type == "i32" || st.scan_type == "ptr")
                                    ? (v == want)
                                    : (v > want - 0.001 && v < want + 0.001);
        else if (op == "ne") ok = !(v > want - 0.001 && v < want + 0.001);
        else if (op == "gt") ok = v > c.last;
        else if (op == "lt") ok = v < c.last;
        else if (op == "ch") ok = v != c.last;
        else if (op == "un") ok = v == c.last;

        if (ok) { c.last = v; keep.push_back(c); }
    }
    st.cands.swap(keep);
    return (int)st.cands.size();
}

// The conf name for a class+offset, "" if it does not know one. Matched against
// the base classes too, for the same reason detail_json does: the conf says
// Instance.Parent and BasePart.Transparency, never Part.Transparency, so a
// concrete-class-only match names almost nothing.
static std::string conf_field(const std::string& cls, uint64_t off) {
    std::vector<std::string> bases;
    bases_for(cls, bases);
    for (const std::string& b : bases) {
        for (int k = 0, n = fx_off_count(); k < n; k++) {
            const char* key = fx_off_key(k);
            const char* dot = key ? strchr(key, '.') : nullptr;
            if (!dot) continue;
            if ((size_t)(dot - key) != b.size()) continue;
            if (strncmp(key, b.c_str(), b.size()) != 0) continue;
            if (fx_off(key) == off) return dot + 1;
        }
    }
    return "";
}

std::string cands_json(const State& st) {
    const size_t SHOW = 60;

    // NAMED HITS FIRST, and this is not cosmetic.
    //
    // Scanning for health found 398 candidates and the real Humanoid.Health was
    // among them -- but not in the first 60, so the results list showed a wall
    // of anonymous offsets and none of the answer. A hit that lands exactly on
    // a field the conf already knows is far more likely to be what you were
    // looking for than one at a random offset in some GUI object, so it goes to
    // the top.
    struct Ent { size_t i; std::string cls, field, name; double v; };
    std::vector<Ent> named, rest;
    size_t step = slot_size(st.scan_type);

    for (size_t i = 0; i < st.cands.size(); i++) {
        if (named.size() + rest.size() >= SHOW * 4) break;   // enough to choose from
        const Cand& c = st.cands[i];
        unsigned char raw[8] = {0};
        double v = 0;
        if (fx_rdblk(c.inst + c.off, raw, step) == step) slot(raw, 0, st.scan_type, &v);

        Ent e;
        e.i     = i;
        e.cls   = fx_class(c.inst);
        e.name  = fx_name(c.inst);
        e.field = conf_field(e.cls, c.off);
        e.v     = v;
        (e.field.empty() ? rest : named).push_back(e);
    }
    // Named entries can be anywhere in the list, so a cap that stops early
    // would still miss them. Sweep the rest of the candidates for named ones
    // only -- cheap, since it is a conf lookup and no extra reads.
    if (named.size() < SHOW) {
        for (size_t i = SHOW * 4; i < st.cands.size() && named.size() < SHOW; i++) {
            const Cand& c = st.cands[i];
            std::string cls = fx_class(c.inst);
            std::string f   = conf_field(cls, c.off);
            if (f.empty()) continue;
            unsigned char raw[8] = {0};
            double v = 0;
            if (fx_rdblk(c.inst + c.off, raw, step) == step) slot(raw, 0, st.scan_type, &v);
            Ent e; e.i = i; e.cls = cls; e.name = fx_name(c.inst); e.field = f; e.v = v;
            named.push_back(e);
        }
    }

    std::string s = "[";
    char b[420];
    size_t out = 0;
    for (int pass = 0; pass < 2; pass++) {
        const std::vector<Ent>& src = pass ? rest : named;
        for (const Ent& e : src) {
            if (out >= SHOW) break;
            if (out++) s += ",";
            const Cand& c = st.cands[e.i];
            std::string nm; json_escape(e.name, nm);
            snprintf(b, sizeof b,
                     "{\"a\":\"%llx\",\"o\":\"%llx\",\"v\":%.4f,\"c\":\"%s\",\"f\":\"%s\",\"n\":\"",
                     (unsigned long long)c.inst, (unsigned long long)c.off,
                     e.v, e.cls.c_str(), e.field.c_str());
            s += b; s += nm; s += "\"}";
        }
    }
    s += "]";
    return s;
}

// ---- remotes ----------------------------------------------------------------
//
// READ ONLY, and that is the whole shape of this feature. It lists what exists;
// it cannot fire one and it cannot show you the arguments of a call. Both of
// those need code running inside the client -- FireServer is a Luau method, and
// capturing arguments means a hook on the call path -- which is the one thing
// this project does not do. See the BOUNDARY note in eclipse_console.cpp.
//
// The find box already matches class names, so "RemoteEvent" finds them; this
// exists because that search stops at SEARCH_HITS and expands every ancestor,
// which buries a 60-remote game in opened folders. An inventory wants to be flat
// and complete.
static const char* kRemoteClasses[] = {
    "RemoteEvent", "RemoteFunction", "UnreliableRemoteEvent",
    "BindableEvent", "BindableFunction",
};

void build_remotes(State& st) {
    st.remotes.clear();
    st.remotes_truncated = false;
    st.remotes_ms = 0;
    uint64_t dm = fx_dm();
    if (!dm) return;

    auto t0 = std::chrono::steady_clock::now();

    // Class by DESCRIPTOR pointer, not by name. fx_class costs a descriptor read
    // plus a string read on every instance, and this walks all of them; every
    // instance of a class shares one descriptor, so the string is read once per
    // class and the rest is a pointer compare.
    uint64_t oDesc = fx_off("Instance.ClassDescriptor");
    std::map<uint64_t, int> seen;          // descriptor -> index into kRemoteClasses, -1 none

    std::vector<uint64_t> cur, next;
    cur.push_back(dm);
    int visited = 0;
    const int NREM = (int)(sizeof kRemoteClasses / sizeof kRemoteClasses[0]);

    for (int depth = 0; depth < MAX_DEPTH && !cur.empty() && fx_running(); depth++) {
        next.clear();
        for (uint64_t p : cur) {
            uint64_t n = fx_nchildren(p);
            if (n > MAX_KIDS) n = MAX_KIDS;
            for (uint64_t i = 0; i < n; i++) {
                uint64_t c = fx_child(p, (int)i);
                if (!c) continue;
                if (++visited >= INDEX_CAP) { st.remotes_truncated = true; break; }
                next.push_back(c);

                int which = -1;
                if (oDesc != (uint64_t)-1) {
                    uint64_t d = fx_rd(c + oDesc);
                    auto it = seen.find(d);
                    if (it != seen.end()) which = it->second;
                    else {
                        const char* cl = fx_class(c);
                        for (int k = 0; k < NREM && cl; k++)
                            if (strcmp(cl, kRemoteClasses[k]) == 0) { which = k; break; }
                        if (d) seen[d] = which;
                    }
                } else {
                    const char* cl = fx_class(c);
                    for (int k = 0; k < NREM && cl; k++)
                        if (strcmp(cl, kRemoteClasses[k]) == 0) { which = k; break; }
                }
                if (which < 0) continue;

                Remote r;
                r.inst = c;
                r.cls  = kRemoteClasses[which];
                const char* nm = fx_name(c);          // borrowed -- copy now
                r.name = (nm && *nm) ? nm : "";
                r.path = path_of(st, c);
                st.remotes.push_back(r);
            }
            if (st.remotes_truncated) break;
        }
        if (st.remotes_truncated) break;
        cur.swap(next);
    }

    // Grouped by container, then by name: a game's remotes come in folders and
    // reading them folder by folder is how you make sense of the surface.
    std::sort(st.remotes.begin(), st.remotes.end(),
              [](const Remote& a, const Remote& b) {
                  if (a.path != b.path) return a.path < b.path;
                  return a.name < b.name;
              });
    st.remotes_ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
}

std::string remotes_json(const State& st) {
    std::string s = "[";
    char b[64];
    for (size_t i = 0; i < st.remotes.size(); i++) {
        const Remote& r = st.remotes[i];
        if (i) s += ",";
        std::string nm, cl, pa;
        json_escape(r.name, nm);
        json_escape(r.cls,  cl);
        json_escape(r.path, pa);
        snprintf(b, sizeof b, "{\"a\":\"%llx\",\"n\":\"", (unsigned long long)r.inst);
        s += b; s += nm;
        s += "\",\"c\":\""; s += cl;
        s += "\",\"p\":\""; s += pa;
        s += "\"}";
    }
    s += "]";
    return s;
}

// ---- declared properties, from Roblox's own reflection ----------------------
//
// WHAT THIS IS AND IS NOT. Every instance points at a ClassDescriptor, and the
// property NAMES its class declares are reachable from there -- verified against
// a live client, where all eight known Humanoid fields (WalkSpeed, Health,
// MaxHealth, JumpPower, HipHeight, DisplayName, RigType, PlatformStand) came
// back from a blind three-level pointer walk, alongside 251 identifiers total.
//
// It gives you names, NOT offsets. Two things were measured and both failed:
// the offset is not stored beside the name (0 of 6 known values appeared within
// +-0x120 of the name slot), and a sweep for getter thunks near the name found
// only more strings -- the nodes reached this way are a string table, not the
// property descriptors. Finding those needs a real layout hypothesis and is not
// attempted here.
//
// So the value is knowing what EXISTS to go hunting for. A name listed here with
// no conf entry is a field this client has and our offsets do not.
static bool ident_str(uint64_t a, std::string* out) {
    char b[96] = {0};
    size_t n = fx_str(a, b, sizeof b - 1);
    if (n < 3 || n > 64) return false;
    if (!((b[0] >= 'A' && b[0] <= 'Z') || (b[0] >= 'a' && b[0] <= 'z'))) return false;
    for (size_t i = 0; i < n; i++) {
        char c = b[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    *out = b;
    return true;
}

void build_props(State& st, uint64_t inst) {
    st.props.clear();
    st.props_desc = 0;
    st.props_ms = 0;
    if (!inst) return;

    uint64_t oDesc = fx_off("Instance.ClassDescriptor");
    if (oDesc == (uint64_t)-1) return;
    uint64_t desc = fx_rd(inst + oDesc);
    if (desc < 0x10000 || desc > 0x7fffffffffffULL) return;

    auto t0 = std::chrono::steady_clock::now();

    const size_t SPAN = 0x300;
    std::set<uint64_t> visited;
    std::set<std::string> names;
    std::vector<uint64_t> cur{ desc }, next;
    int reads = 0;

    for (int level = 0; level < 3 && !cur.empty() && fx_running(); level++) {
        next.clear();
        for (uint64_t b : cur) {
            if (!visited.insert(b).second) continue;
            if (++reads > 8000) break;                 // bound: this is a blind walk
            std::vector<unsigned char> buf(SPAN);
            size_t got = fx_rdblk(b, buf.data(), SPAN);
            if (got < 16) continue;
            for (size_t o = 0; o + 8 <= got; o += 8) {
                uint64_t p;
                memcpy(&p, buf.data() + o, 8);
                if (p < 0x10000 || p > 0x7fffffffffffULL) continue;
                std::string s;
                if (ident_str(p, &s)) names.insert(s);
                if (level + 1 < 3) next.push_back(p);
            }
        }
        cur.swap(next);
        if (cur.size() > 4000) cur.resize(4000);
    }

    st.props.assign(names.begin(), names.end());
    st.props_desc = desc;
    st.props_ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
}

std::string props_json(const State& st, uint64_t inst) {
    if (!inst || !st.props_desc) return "null";
    uint64_t oDesc = fx_off("Instance.ClassDescriptor");
    if (oDesc == (uint64_t)-1 || fx_rd(inst + oDesc) != st.props_desc) return "null";

    // Which of these the conf can already locate. Matched case-INSENSITIVELY and
    // across base classes, because the conf spells one of them "Walkspeed" while
    // reflection says "WalkSpeed", and keys BasePart.* for a Part.
    std::vector<std::string> bases;
    bases_for(fx_class(inst), bases);
    std::set<std::string> known;
    for (int k = 0, n = fx_off_count(); k < n; k++) {
        const char* key = fx_off_key(k);
        const char* dot = key ? strchr(key, '.') : nullptr;
        if (!dot) continue;
        std::string cls(key, dot - key), fld(dot + 1);
        bool mine = false;
        for (const std::string& b : bases) if (b == cls) mine = true;
        if (!mine) continue;
        for (char& c : fld) c = (char)tolower((unsigned char)c);
        known.insert(fld);
    }

    std::string s = "[";
    for (size_t i = 0; i < st.props.size(); i++) {
        if (i) s += ",";
        std::string lower = st.props[i];
        for (char& c : lower) c = (char)tolower((unsigned char)c);
        std::string nm;
        json_escape(st.props[i], nm);
        s += "{\"n\":\"" + nm + "\",\"k\":" + (known.count(lower) ? "1" : "0") + "}";
    }
    s += "]";
    return s;
}

// ---- search ---------------------------------------------------------------------

static bool icontains(const char* hay, const std::string& needle) {
    if (!hay || needle.empty()) return false;
    size_t hl = strlen(hay), nl = needle.size();
    if (nl > hl) return false;
    for (size_t i = 0; i + nl <= hl; i++) {
        size_t j = 0;
        while (j < nl) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
            j++;
        }
        if (j == nl) return true;
    }
    return false;
}

// Depth-first with an explicit stack and a hard node cap. Each hit has its
// ancestors expanded, so the results are visible in the tree rather than being
// a separate list the tree disagrees with.
int run_search(State& st, const std::string& q) {
    st.find = q;
    if (q.empty()) return 0;

    uint64_t oPar = fx_off("Instance.Parent");
    struct F { uint64_t inst; int depth; };
    std::vector<F> stack;
    stack.push_back({ st.root, 0 });

    int seen = 0, hits = 0;
    while (!stack.empty() && seen < SEARCH_NODES && hits < SEARCH_HITS && fx_running()) {
        F f = stack.back();
        stack.pop_back();
        if (f.depth >= MAX_DEPTH) continue;

        uint64_t n = fx_nchildren(f.inst);
        if (n > MAX_KIDS) n = MAX_KIDS;
        for (uint64_t i = 0; i < n; i++) {
            uint64_t c = fx_child(f.inst, (int)i);
            if (!c) continue;
            seen++;

            if (icontains(fx_name(c), q) || icontains(fx_class(c), q)) {
                hits++;
                // Open every ancestor so the hit is on screen.
                if (oPar != (uint64_t)-1) {
                    uint64_t up = c;
                    for (int d = 0; d < MAX_DEPTH; d++) {
                        uint64_t par = fx_rd(up + oPar);
                        if (par < 0x10000 || par > 0x7fffffffffffULL) break;
                        st.expanded.insert(par);
                        if (par == st.root) break;
                        up = par;
                    }
                }
                if (hits == 1) st.sel = c;
            }
            stack.push_back({ c, f.depth + 1 });
        }
    }
    return hits;
}
