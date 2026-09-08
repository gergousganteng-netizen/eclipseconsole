#pragma once
// Shared across the dex project. float3 and the fx_* API are already in scope --
// the host passes -include fx.h, which lands above everything.
#include <set>
#include <string>
#include <vector>

// One scan candidate.
//
// Stored as INSTANCE + OFFSET, never as an absolute address, and that is the
// whole reason this is worth building rather than deferring to Cheat Engine.
// CE finds a number in the heap and hands you a bare pointer; narrow it, then
// respawn, and it is meaningless. A hit here is "Humanoid +0x190", which is a
// fact about the game rather than about this session -- and it is already named
// before you have done anything.
struct Cand {
    uint64_t inst = 0;
    uint64_t off  = 0;
    double   last = 0;      // value at the previous pass, for changed/unchanged
};

// One entry in the remotes inventory. Flat on purpose: the tree view already
// exists, and what a remote list is FOR is seeing the whole surface at once.
struct Remote {
    uint64_t    inst = 0;
    std::string cls;
    std::string name;
    std::string path;
};

struct State {
    uint64_t           root = 0;
    std::set<uint64_t> expanded;      // which nodes are open
    uint64_t           sel  = 0;      // the inspected node
    std::string        find;          // last search text, "" when not searching
    // Hide services with no children. The DataModel has ~148 of them and most
    // are empty shells -- TimerService, VideoCaptureService -- so the first
    // screen is mostly noise without this.
    bool               tidy = true;
    // Last thing worth telling the user: a locate result, a scan count.
    std::string        note;

    // Scan candidates, and the type they were found at.
    std::vector<Cand>  cands;
    std::string        scan_type = "f32";
    // How much of the game the last scan actually covered. Reported because a
    // truncated index reads exactly like a clean empty result: the old cap of
    // 120000 silently missed 35% of a 184512-instance game, and every Player
    // object was in the missing part.
    int                scan_indexed = 0;
    bool               scan_partial = false;

    // Shape of the rows the page was last SENT. The tree is rebuilt on a timer
    // because its values are live, but resending 200 KB of unchanged rows twice
    // a second is what made clicking feel dead. When the shape matches, only the
    // values go over.
    std::string        last_shape;
    // Set when the row budget cut the tree. Without this the twisty you just
    // opened silently expands onto nothing.
    bool               rows_truncated = false;

    // Remotes inventory. Built on demand because it is a full-tree walk, and
    // kept until asked to refresh -- remotes are created at load and rarely
    // change, unlike the positions everything else here reads.
    bool                remotes_mode = false;
    std::vector<Remote> remotes;
    int                 remotes_ms = 0;
    bool                remotes_truncated = false;

    // Property names declared by a class, read out of Roblox's own reflection
    // data rather than the conf. Tied to one CLASS DESCRIPTOR at a time: the
    // walk takes about a second, so it happens when asked and the result stays
    // until you ask about a different class.
    uint64_t                 props_desc = 0;
    std::vector<std::string> props;
    int                      props_ms = 0;
};

// One visible line. The page renders these and nothing else -- it never walks
// the tree itself, which is what keeps the two sides from disagreeing.
struct Row {
    int         depth;
    uint64_t    addr;
    std::string name;
    std::string cls;
    bool        kids;                 // draw a twisty
    bool        open;
    bool        sel;
    // Decoded payload for a *Value child, shown inline. Browsing a folder of
    // BoolValues is useless without it -- Bad Business keeps its whole movement
    // state that way, and the names alone tell you nothing.
    std::string val;
};

// Decode a *Value instance's payload by its CLASS, never by guessing a width.
// Empty for anything that is not a Value.
std::string value_of(uint64_t inst, const char* cls);

// tree.cpp
std::vector<Row> build_rows(State& st);      // sets st.rows_truncated
std::string      detail_json(uint64_t inst);
std::string      path_of(const State& st, uint64_t inst);
// Bounded name/class search. Expands the path to each hit so they are visible.
int              run_search(State& st, const std::string& q);

// Which object does a raw address live inside? This is namer.cpp folded in, and
// it is what closes the loop: Cheat Engine finds an address, this says it is
// Workspace.Someone.Humanoid +0x190 and takes you there.
//
// Unlike the lazy tree it needs a FULL walk, so it is built on demand rather
// than kept -- pointers go stale and a stale index names the wrong object with
// complete confidence.
struct Hit {
    bool        found = false;
    uint64_t    inst  = 0;
    uint64_t    off   = 0;
    bool        prim  = false;
    std::string cls;
    std::string field;      // conf name for that offset, "" if none
};
Hit locate(uint64_t addr);

// ---- scanning INSIDE known objects --------------------------------------------
//
// Not a memory scanner, and the difference is the point. Cheat Engine sweeps the
// whole heap and is better at that than anything written here -- two attempts in
// this project proved it. This sweeps only the bytes of instances the tree
// already knows about: ~35k objects at 0x900 bytes is about 30 MB, versus
// gigabytes, so it finishes in well under a second.
//
// What it buys over CE, for this domain:
//   - every hit is ALREADY NAMED. "Humanoid +0x190" beats a bare pointer.
//   - hits are instance+offset, so they mean something after a respawn.
//   - a hit that lands in an object is by construction a property of that
//     object, not a coincidence in some allocator's scratch space.
//
// What it cannot do: find anything OUTSIDE an instance -- Luau locals, engine
// state, script tables. CE still wins there, and namer/locate() is how you carry
// a CE result back into this tool.
int scan_first(State& st, const std::string& type, const std::string& text);
// op: "eq" "ne" "gt" "lt" "ch" (changed) "un" (unchanged)
int scan_narrow(State& st, const std::string& op, const std::string& text);
std::string cands_json(const State& st);
// Walk the whole tree for remotes. Expensive (it is every instance), so it is
// called only when asked and the result is kept.
void        build_remotes(State& st);
std::string remotes_json(const State& st);
// Property names the selected instance's CLASS declares, from reflection.
void        build_props(State& st, uint64_t inst);
std::string props_json(const State& st, uint64_t inst);

// ui.cpp
// Non-const: it records the shape it just sent, so the next call can send only
// the values when nothing moved.
std::string build_js(State& st);
bool        handle_msg(State& st, const char* msg);   // true if a redraw is due
void        json_escape(const std::string& in, std::string& out);
