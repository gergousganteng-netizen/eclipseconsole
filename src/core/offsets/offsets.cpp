// offsets.cpp -- the offset table. See offsets.h for why the map is private and
// reached only through these functions.
//
// PUBLISHED AS AN IMMUTABLE SNAPSHOT, and that is not an optimisation -- it is
// what stops this table corrupting the heap.
//
// The table used to be a bare `static std::unordered_map` with no lock of any
// kind, and it is the busiest shared structure in the process: fx_off is two
// calls deep inside fx_nchildren and fx_child, so every snippet thread reads it
// tens of thousands of times a second. Meanwhile attach, detach and preload
// DESTROY it -- offset_clear() frees every key string and every bucket, then
// 389 offset_set() calls rehash it repeatedly -- from the UI thread. A reader
// inside find() across that is walking freed buckets and following dangling
// pointers. The visible result is STATUS_HEAP_CORRUPTION (0xc0000374) at a
// random later allocation, or an access violation inside libstdc++ where the
// hashtable code lives; both were recurring in this app.
//
// A mutex would work and is the obvious answer, but it puts a lock/unlock pair
// on the hottest path in the system, twice per child walk. So instead: writers
// build a NEW table off to the side and publish it with one atomic store, and
// readers take one atomic load and then touch memory nothing will ever modify.
// Uncontended, the read is a plain load.
//
// RETIRED TABLES ARE NEVER FREED, deliberately. A reader may be holding the old
// pointer (or a c_str() into it, which is what offset_key hands out) at the
// instant of the swap, and there is no cheap way to know when the last one has
// let go. A table is ~30KB and is replaced only on attach, detach or a client
// version change -- a handful of times in a session. Leaking that is the price
// of a lock-free read, and it is the right trade here.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "fx.h"
#include "offsets.h"

static const uint64_t BAD = (uint64_t)-1;

namespace {

struct Table {
    std::unordered_map<std::string, uint64_t> map;
    // A stable index over the map, so a snippet can walk the whole conf rather
    // than only the keys it happens to name. Sorted: an unordered_map has no
    // order worth exposing, and "offset 37" must mean the same thing twice in
    // a row. Built once at commit rather than on demand -- lazily rebuilding it
    // was a second unsynchronised write from whichever thread asked first.
    std::vector<std::string> index;
};

const Table  g_empty;                                  // published before any load
std::atomic<const Table*> g_tab{ &g_empty };

// Writers only. Serialises the build; readers never take it.
std::mutex   g_write_mux;
Table*       g_build = nullptr;

inline const Table& live() { return *g_tab.load(std::memory_order_acquire); }

}  // namespace

uint64_t off_of(const char* key) {
    const Table& t = live();
    auto it = t.map.find(key);
    return it == t.map.end() ? BAD : it->second;
}

// ---- writers ---------------------------------------------------------------
// offset_clear -> offset_set xN -> offset_commit. Between the clear and the
// commit readers see an EMPTY table, which is the same thing they saw
// mid-rebuild before and is honest: BAD for every key, rather than a mixture of
// the old conf and the new one.

void offset_clear(void) {
    std::lock_guard<std::mutex> lk(g_write_mux);
    g_build = new Table();
    // Published immediately: detach clears and never commits, and it must take
    // effect at once -- the picker probing every client through a stale table
    // is a bug this file has already had.
    g_tab.store(&g_empty, std::memory_order_release);
}

void offset_set(const char* key, uint64_t val) {
    std::lock_guard<std::mutex> lk(g_write_mux);
    if (!g_build) g_build = new Table();
    g_build->map[key] = val;
}

void offset_commit(void) {
    std::lock_guard<std::mutex> lk(g_write_mux);
    if (!g_build) return;
    g_build->index.reserve(g_build->map.size());
    for (const auto& kv : g_build->map) g_build->index.push_back(kv.first);
    std::sort(g_build->index.begin(), g_build->index.end());
    // The one instant the world changes. Everything above touched only the
    // unpublished build, so no reader has seen any of it until this line.
    g_tab.store(g_build, std::memory_order_release);
    g_build = nullptr;              // ownership handed to the published table
}

size_t offset_size(void) { return live().map.size(); }

int offset_count(void) { return (int)live().index.size(); }

const char* offset_key(int i) {
    const Table& t = live();
    if (i < 0 || (size_t)i >= t.index.size()) return "";
    // Safe to hand out because published tables are never freed -- see the
    // header comment. With the old in-place rebuild this pointer could be
    // freed by the next conf load while the caller still held it.
    return t.index[(size_t)i].c_str();
}

// ---- snippet-facing offset exports (thin wrappers over the accessors above) --
FX_API uint64_t fx_off(const char* key) { return off_of(key); }

// Walk the conf. Without these a snippet can only check keys its author typed
// out, which is exactly the set least likely to have broken.
FX_API int fx_off_count(void) { return offset_count(); }

FX_API const char* fx_off_key(int i) { return offset_key(i); }
