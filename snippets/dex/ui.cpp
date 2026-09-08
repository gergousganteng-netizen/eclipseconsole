#include "dex.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

void json_escape(const std::string& in, std::string& out) {
    for (char c : in) {
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c >= 32)   out += c;
        // Anything below 32 is dropped rather than escaped: instance names are
        // game-supplied, and a stray control byte in a name would otherwise
        // break the whole JS payload and blank the page.
    }
}

static uint64_t parse_hex(const char* s) {
    return (uint64_t)strtoull(s, nullptr, 16);
}

// Page -> snippet. Deliberately tiny: every message is one letter, a colon and
// a value. The page holds no state to keep in sync, so there is nothing more to
// say than "the user clicked this".
bool handle_msg(State& st, const char* msg) {
    if (!msg || !*msg) return false;

    if (msg[0] == 't' && msg[1] == ':') {           // toggle expand
        uint64_t a = parse_hex(msg + 2);
        if (!a) return false;
        if (st.expanded.count(a)) st.expanded.erase(a);
        else                      st.expanded.insert(a);
        return true;
    }
    if (msg[0] == 's' && msg[1] == ':') {           // select
        uint64_t a = parse_hex(msg + 2);
        if (a) st.sel = a;
        return true;
    }
    if (msg[0] == 'f' && msg[1] == ':') {           // find
        run_search(st, msg + 2);
        return true;
    }
    if (msg[0] == 'r') {                            // collapse to root
        st.expanded.clear();
        st.expanded.insert(st.root);
        st.sel = st.root;
        st.find.clear();
        return true;
    }
    if (msg[0] == 'y') { st.tidy = !st.tidy; return true; }   // tidy toggle

    // Follow a pointer field to whatever it points at: select that instance and
    // open the path down to it. Turns the detail pane from a readout into
    // navigation -- Player.ModelInstance takes you to the character.
    if (msg[0] == 'g' && msg[1] == ':') {
        uint64_t a = parse_hex(msg + 2);
        if (a < 0x10000 || a > 0x7fffffffffffULL) return false;
        uint64_t oPar = fx_off("Instance.Parent");
        if (oPar != (uint64_t)-1) {
            uint64_t up = a;
            for (int d = 0; d < 24; d++) {
                uint64_t par = fx_rd(up + oPar);
                if (par < 0x10000 || par > 0x7fffffffffffULL) break;
                st.expanded.insert(par);
                if (par == st.root) break;
                up = par;
            }
        }
        st.sel = a;
        return true;
    }
    // Paste an address from Cheat Engine: find which object it lives inside,
    // select it and open the path down to it. This is namer.cpp folded in, and
    // it is the step that turns a hex number into somewhere you can stand.
    if (msg[0] == 'n' && msg[1] == ':') {
        const char* p = msg + 2;
        while (*p == ' ') p++;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        uint64_t a = parse_hex(p);
        if (!a) { st.note = "not a hex address"; return true; }

        Hit h = locate(a);
        if (!h.found) {
            char b[96];
            snprintf(b, sizeof b, "0x%llx is not inside any indexed object",
                     (unsigned long long)a);
            st.note = b;
            return true;
        }
        char b[220];
        snprintf(b, sizeof b, "0x%llx = %s%s +0x%llx%s%s",
                 (unsigned long long)a, h.cls.c_str(), h.prim ? " (Primitive)" : "",
                 (unsigned long long)h.off,
                 h.field.empty() ? "" : "  = ", h.field.c_str());
        st.note = b;

        // Open the path so the object is actually visible, not just named.
        uint64_t oPar = fx_off("Instance.Parent");
        if (oPar != (uint64_t)-1) {
            uint64_t up = h.inst;
            for (int d = 0; d < 24; d++) {
                uint64_t par = fx_rd(up + oPar);
                if (par < 0x10000 || par > 0x7fffffffffffULL) break;
                st.expanded.insert(par);
                if (par == st.root) break;
                up = par;
            }
        }
        st.sel = h.inst;
        return true;
    }

    // S:<type>:<value>   first scan
    if (msg[0] == 'S' && msg[1] == ':') {
        const char* c1 = strchr(msg + 2, ':');
        if (!c1) return false;
        std::string type(msg + 2, c1 - (msg + 2));
        int n = scan_first(st, type, c1 + 1);
        char b[160];
        // The indexed count is always shown, not only on truncation: an empty
        // result is only evidence if you know how much was actually swept.
        snprintf(b, sizeof b, "%d candidate%s in %d objects%s", n, n == 1 ? "" : "s",
                 st.scan_indexed,
                 st.scan_partial ? "  -- INDEX TRUNCATED, tree is bigger than the cap" : "");
        st.note = b;
        if (n >= 40000) st.note += "  (capped -- narrow it)";
        return true;
    }
    // N:<op>[:<value>]   narrow
    if (msg[0] == 'N' && msg[1] == ':') {
        const char* c1 = strchr(msg + 2, ':');
        std::string op = c1 ? std::string(msg + 2, c1 - (msg + 2)) : std::string(msg + 2);
        int n = scan_narrow(st, op, c1 ? c1 + 1 : "");
        char b[96];
        snprintf(b, sizeof b, "%d left", n);
        st.note = b;
        return true;
    }
    if (msg[0] == 'X') { st.cands.clear(); st.note = "scan cleared"; return true; }

    // C:<inst>:<off>  -- take a candidate: select its object and open the path
    // down to it, so a scan hit lands you on the instance it belongs to. The
    // offset is the page's business; nothing here needs it.
    if (msg[0] == 'C' && msg[1] == ':') {
        const char* c1 = strchr(msg + 2, ':');
        if (!c1) return false;
        uint64_t inst = parse_hex(std::string(msg + 2, c1 - (msg + 2)).c_str());
        if (!inst) return false;
        uint64_t oPar = fx_off("Instance.Parent");
        if (oPar != (uint64_t)-1) {
            uint64_t up = inst;
            for (int d = 0; d < 24; d++) {
                uint64_t par = fx_rd(up + oPar);
                if (par < 0x10000 || par > 0x7fffffffffffULL) break;
                st.expanded.insert(par);
                if (par == st.root) break;
                up = par;
            }
        }
        st.sel = inst;
        return true;
    }

    // M  toggle the remotes inventory. M! rebuilds it.
    //
    // Built lazily and kept, because it is a walk over every instance in the
    // game -- unlike everything else here, which is a handful of reads. Remotes
    // are created at load and effectively static, so a stale list is the right
    // default and a refresh is one keypress.
    if (msg[0] == 'M') {
        bool force = (msg[1] == '!');
        st.remotes_mode = force ? true : !st.remotes_mode;
        if (st.remotes_mode && (force || st.remotes.empty())) {
            build_remotes(st);
            char b[160];
            snprintf(b, sizeof b, "%d remotes in %d ms%s", (int)st.remotes.size(),
                     st.remotes_ms,
                     st.remotes_truncated ? "  -- TREE TRUNCATED, list is partial" : "");
            st.note = b;
        }
        return true;
    }

    // P  list the properties the selected instance's CLASS declares.
    //
    // Explicit rather than automatic: the walk is about a second, and doing it
    // on every selection would turn clicking around the tree into a stall. Tied
    // to one class at a time -- select a different class and the pane offers the
    // link again rather than silently showing the previous class's list.
    if (msg[0] == 'P') {
        if (!st.sel) { st.note = "nothing selected"; return true; }
        uint64_t oDesc = fx_off("Instance.ClassDescriptor");
        uint64_t d = (oDesc == (uint64_t)-1) ? 0 : fx_rd(st.sel + oDesc);
        if (st.props_desc && d == st.props_desc) {   // same class again: close it
            st.props.clear();
            st.props_desc = 0;
            return true;
        }
        build_props(st, st.sel);
        char b[160];
        if (st.props.empty())
            snprintf(b, sizeof b, "no reflection data reachable for %s", fx_class(st.sel));
        else
            snprintf(b, sizeof b, "%d names declared by %s (%d ms)",
                     (int)st.props.size(), fx_class(st.sel), st.props_ms);
        st.note = b;
        return true;
    }

    if (msg[0] == 'u') return true;                 // just redraw
    return false;
}

// Snippet -> page. One call, one complete picture: the rows to draw and the
// detail for the selection. The page never asks for anything.
std::string build_js(State& st) {
    std::vector<Row> rows = build_rows(st);

    // ROWS GO OVER ONLY WHEN THE TREE'S SHAPE CHANGED.
    //
    // The tree is rebuilt on a timer because the values in it are live, but the
    // rows themselves only move when you click. Resending all of them anyway was
    // ~200 KB of JS to parse twice a second, which is what made a click feel
    // like it had been ignored. Shape covers exactly what the page draws
    // structurally -- which rows, at what depth, open, selected.
    std::string shape;
    {
        char sb[48];
        // The remotes list rides on the same "full" send as the rows, so the
        // mode and the list's identity have to be part of the shape -- toggling
        // it does not move a single row, and without this the page would be
        // told to show a list it was never given.
        snprintf(sb, sizeof sb, "R%d.%d.%d;", st.remotes_mode ? 1 : 0,
                 (int)st.remotes.size(), st.remotes_ms);
        shape += sb;
        for (const Row& r : rows) {
            snprintf(sb, sizeof sb, "%llx.%d%c%c;", (unsigned long long)r.addr,
                     r.depth, r.open ? 'o' : '-', r.sel ? 's' : '-');
            shape += sb;
        }
    }
    bool full = (shape != st.last_shape);
    st.last_shape = shape;

    char b[512];
    std::string s = "U({\"full\":";
    s += full ? "1," : "0,";

    if (full) {
        s += "\"rows\":[";
        for (size_t i = 0; i < rows.size(); i++) {
            const Row& r = rows[i];
            if (i) s += ",";
            std::string nm, cl, vl;
            json_escape(r.name, nm);
            json_escape(r.cls,  cl);
            json_escape(r.val,  vl);
            snprintf(b, sizeof b,
                     "{\"d\":%d,\"a\":\"%llx\",\"k\":%d,\"o\":%d,\"s\":%d,\"n\":\"",
                     r.depth, (unsigned long long)r.addr,
                     r.kids ? 1 : 0, r.open ? 1 : 0, r.sel ? 1 : 0);
            s += b;
            s += nm;
            s += "\",\"c\":\"";
            s += cl;
            s += "\",\"v\":\"";
            s += vl;
            s += "\"}";
        }
        s += "],";
    } else {
        // Same rows, new numbers. Values only, in row order.
        s += "\"vals\":[";
        for (size_t i = 0; i < rows.size(); i++) {
            if (i) s += ",";
            std::string vl;
            json_escape(rows[i].val, vl);
            s += "\"" + vl + "\"";
        }
        s += "],";
    }

    snprintf(b, sizeof b, "\"trunc\":%d,\"rmode\":%d,\"rtrunc\":%d,",
             st.rows_truncated ? 1 : 0, st.remotes_mode ? 1 : 0,
             st.remotes_truncated ? 1 : 0);
    s += b;

    // Only when the list is on screen -- it is static and can be hundreds of
    // entries, so there is no reason to push it on every value refresh.
    s += "\"remotes\":";
    s += (st.remotes_mode && full) ? remotes_json(st) : std::string("null");
    s += ",";

    std::string p;
    json_escape(path_of(st, st.sel), p);
    s += "\"path\":\"" + p + "\",";

    std::string f;
    json_escape(st.find, f);
    s += "\"find\":\"" + f + "\",";

    std::string nt;
    json_escape(st.note, nt);
    s += "\"note\":\"" + nt + "\",";

    // The selected node's payload, decoded by its CLASS. A display value and
    // nothing more -- there is no editor behind it, so the page never has to
    // ask whether this class is one it may write.
    {
        std::string cls = st.sel ? fx_class(st.sel) : "";
        std::string v   = st.sel ? value_of(st.sel, cls.c_str()) : "";
        std::string ev;
        json_escape(v, ev);
        s += "\"val\":\"" + ev + "\",";
    }

    // Scan candidates. cands_json caps what it emits -- a 40k-candidate first
    // scan must not become a 40k-row payload pushed twice a second.
    snprintf(b, sizeof b, "\"ncand\":%d,\"stype\":\"%s\",\"cands\":",
             (int)st.cands.size(), st.scan_type.c_str());
    s += b;
    s += cands_json(st);
    s += ",";

    // Declared property names, only for the class they were built for.
    s += "\"props\":";
    s += st.sel ? props_json(st, st.sel) : std::string("null");
    s += ",";

    snprintf(b, sizeof b, "\"count\":%d,\"tidy\":%d,\"sel\":",
             (int)rows.size(), st.tidy ? 1 : 0);
    s += b;
    s += st.sel ? detail_json(st.sel) : std::string("null");
    s += "})";
    return s;
}
