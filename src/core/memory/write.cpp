// write.cpp -- fx_wrblk, its arm gate, and the dry-run recorder that stands in
// for the write while disarmed. See write.h for why this is a file of its own.
//
// THE INVARIANT THIS FILE EXISTS TO PROTECT, restated where it is enforced:
// fx_wrblk MUST log-and-return-n when disarmed, and must not touch the process.
// If a change would let a write reach the client in dry-run mode it is wrong
// regardless of what else it enables.

#include <windows.h>

#ifndef NTSTATUS
typedef LONG NTSTATUS;
#endif
#ifndef NT_SUCCESS
#define NT_SUCCESS(st) (((NTSTATUS)(st)) >= 0)
#endif
typedef NTSTATUS (NTAPI *pNtWriteVirtualMemory)(HANDLE,PVOID,PVOID,SIZE_T,PSIZE_T);

#include <cstdio>
#include <cstring>
#include <mutex>

#include "fx.h"
#include "eclipse_core.h"
#include "core_internal.h"
#include "run.h"          // core_run_current_id / CORE_RUN_CAP (per-run collapse key)
#include "write.h"

// ----------------------------- the write stub --------------------------------
// The buffer and the pointer are private to this TU on purpose: they are the
// only means in the program of writing to the client, and fx_wrblk below is the
// only thing that touches them. eclipse_console.cpp hands over the finished
// trampoline and keeps no copy.
alignas(4096) static uint8_t g_wr_stub_buf[4096];
static pNtWriteVirtualMemory g_nt_write = nullptr;

bool write_build_stub(make_stub_fn make) {
    if (!make) return false;
    g_nt_write = (pNtWriteVirtualMemory)make("NtWriteVirtualMemory", g_wr_stub_buf);
    return g_nt_write != nullptr;
}

// ----------------------------- dry-run collapsing ----------------------------
// Settable from the UI (Settings -> Console). Not const any more, but still
// read only from inside the dry-run machinery under its own mutex.
static unsigned DRY_WINDOW_MS = 250;

// A consecutive run of intended writes to the SAME destination collapses into
// one ×N line.
//
// Collapsing on the destination rather than on the whole line is the point: an
// aim or lock snippet rewrites the same camera field every tick with a slightly
// different value, so byte-identical runs would essentially never occur and
// nothing would collapse. What repeats is the address, and that is also the
// thing worth reading -- "this field is being hammered 500 times a second" is
// the useful fact, not five hundred near-identical hex dumps. Whether the bytes
// held still is reported rather than assumed.
//
// Collapsing is not filtering: every write is still counted, and the count is
// printed. Silently dropping writes would be lying about exactly the thing a
// dry run exists to show.
// Keyed on (RUN, destination), not destination alone. With several snippets
// disarmed and writing at once, keying on the address only would let each one's
// writes close the OTHER's open collapse on every tick -- so nothing would ever
// collapse and a disarmed pair would flood the log at full rate, defeating the
// machinery that exists to keep a dry run from taking the app down. So there is
// one open collapse PER RUN: a run's writes collapse among themselves and are
// closed only by that run's own other output, arming, or the run ending.
// (docs/multi-run.md.)
//
// SEVERAL SLOTS PER RUN, not one, and that is the difference between this
// working and not. One-per-run meant a loop touching TWO fields -- fly writes
// a velocity and a gravity every tick -- found the open collapse pointing at
// the other address on every single write, so every write counted as a switch,
// flushed, and printed. The machinery reported nothing collapsed and flooded
// the log at full rate, which is exactly what it exists to prevent.
//
// DRY_PER_RUN is small on purpose: a snippet hammering a handful of fields
// collapses cleanly, and one walking hundreds of addresses still gets a line
// each, which is the honest answer for a walk. When a run wants more slots
// than it has, the OLDEST is flushed to make room -- never dropped, because
// collapsing is not filtering.
struct DrySlot {
    bool          have   = false;
    unsigned      run    = 0;        // owning run id while `have`; 0 = CLI/anon
    uint64_t      addr   = 0;
    size_t        size   = 0;
    unsigned      count  = 0;
    bool          varied = false;
    unsigned char bytes[16];
    size_t        nbytes = 0;
    uint64_t      since  = 0;
};
// THE OVERFLOW BUCKET. A run gets DRY_PER_RUN slots; a write to a FIFTH
// destination used to evict the oldest and print it, which is fine when a
// snippet cycles a few fields and catastrophic when it does not.
//
// MEASURED 2026-09-07: hub's fling writes velocity+spin for the root and for
// every body part -- ~30 destinations -- on a loop spinning at fx_sleep(0).
// Disarmed, that is an evict-and-print per write, millions of lines a second,
// and the log funnel drowns: the snippet appears frozen. Armed it is fine,
// because armed emits no dry-run lines at all. fly does not hit it because it
// writes ONE destination, which collapses to nothing.
//
// So the fifth destination onward is COUNTED, not printed, and the count is
// reported once per window. This is still a COLLAPSE, never a filter: every
// refused write is counted and every count is printed. Nothing is dropped and
// nothing reaches the process -- the arm gate is untouched by any of this.
struct DryOver {
    bool     have  = false;
    unsigned run   = 0;
    unsigned long long count = 0;
    uint64_t last  = 0;      // the most recent destination, for the line
    uint64_t since = 0;
};
static std::mutex g_dry_mux;
static const int  DRY_PER_RUN = 4;
static const int  DRY_MAX = (CORE_RUN_CAP + 1) * DRY_PER_RUN;
static DrySlot    g_dry[DRY_MAX];
static DryOver    g_over[CORE_RUN_CAP + 1];

// Caller holds no lock. Emits and clears this run's overflow, if it has one.
static void over_flush(unsigned me) {
    DryOver copy;
    {
        std::lock_guard<std::mutex> lk(g_dry_mux);
        DryOver* o = nullptr;
        for (int i = 0; i <= CORE_RUN_CAP; i++)
            if (g_over[i].have && g_over[i].run == me) { o = &g_over[i]; break; }
        if (!o) return;
        copy = *o;
        o->have = false;          // cleared BEFORE logging, as dry_flush does,
        o->count = 0;             // so logf -> dry_flush -> here cannot recurse
    }
    ec::logf("  [dry-run] would write %llu more times -> other addresses"
             " (latest 0x%llx)  (arm to write)\n",
             (unsigned long long)copy.count, (unsigned long long)copy.last);
}

static void dry_emit(const DrySlot& s) {
    char line[640];
    int k = snprintf(line, sizeof line,
                     "  [dry-run] would write %zu bytes -> 0x%llx  [",
                     s.size, (unsigned long long)s.addr);
    for (size_t i = 0; i < s.nbytes && k < (int)sizeof line - 8; i++)
        k += snprintf(line + k, sizeof line - k, "%02x ", s.bytes[i]);
    k += snprintf(line + k, sizeof line - k, "%s]", s.size > 16 ? "..." : "");
    if (s.count > 1)
        k += snprintf(line + k, sizeof line - k, "  ×%u%s", s.count,
                      s.varied ? " (value varying, last shown)" : "");
    snprintf(line + k, sizeof line - k, "  (arm to write)\n");
    ec::logf("%s", line);
}

// Flush the CALLING RUN's open collapse, if any. ec::logf calls this (a run's
// other output ends ITS collapse, not another run's), and so does run end. It
// resolves the run from the thread, so it only ever closes the caller's.
void dry_flush() {
    unsigned me = core_run_current_id();
    DrySlot copies[DRY_PER_RUN];
    int n = 0;
    {
        std::lock_guard<std::mutex> lk(g_dry_mux);
        for (int i = 0; i < DRY_MAX && n < DRY_PER_RUN; i++) {
            if (!g_dry[i].have || g_dry[i].run != me) continue;
            copies[n++] = g_dry[i];
            // Cleared before logging, which is what stops
            // logf -> dry_flush -> logf from recursing (the nested dry_flush
            // finds no open slot for this run).
            g_dry[i].have = false;
        }
    }
    // The overflow last: it is the tail of everything the slots could not
    // hold, so it reads correctly after them.
    over_flush(me);
    // Oldest first, so the log reads in the order the writes happened.
    for (int i = 0; i < n; i++) {
        int oldest = i;
        for (int j = i + 1; j < n; j++)
            if (copies[j].since < copies[oldest].since) oldest = j;
        DrySlot t = copies[i]; copies[i] = copies[oldest]; copies[oldest] = t;
        dry_emit(copies[i]);
    }
}

// Emit and clear ONE slot -- used when a single collapse hits its window while
// the run's others are still open and must stay open.
static void dry_flush_one(unsigned me, uint64_t addr, size_t size) {
    DrySlot copy;
    {
        std::lock_guard<std::mutex> lk(g_dry_mux);
        DrySlot* s = nullptr;
        for (int i = 0; i < DRY_MAX; i++)
            if (g_dry[i].have && g_dry[i].run == me &&
                g_dry[i].addr == addr && g_dry[i].size == size) { s = &g_dry[i]; break; }
        if (!s) return;
        copy = *s;
        s->have = false;
    }
    dry_emit(copy);
}

void write_set_dry_window_ms(unsigned ms) {
    // Flushed first (the caller's run, if any): a collapse open under the old
    // window must not have the new one applied retroactively.
    dry_flush();
    DRY_WINDOW_MS = ms;
}

static void dry_note(uint64_t a, size_t n, const void* buf) {
    const unsigned char* b = (const unsigned char*)buf;
    size_t keep = n < sizeof g_dry[0].bytes ? n : sizeof g_dry[0].bytes;
    unsigned me = core_run_current_id();

    enum { MERGED, EXPIRED, SWITCHED } what;
    {
        std::lock_guard<std::mutex> lk(g_dry_mux);
        // Matched on the DESTINATION now, not merely on the run: a run may
        // have several collapses open at once, one per field it is writing.
        DrySlot* mine = nullptr;
        for (int i = 0; i < DRY_MAX; i++)
            if (g_dry[i].have && g_dry[i].run == me &&
                g_dry[i].addr == a && g_dry[i].size == n) { mine = &g_dry[i]; break; }

        if (mine) {
            mine->count++;
            if (mine->nbytes != keep || memcmp(mine->bytes, b, keep) != 0) {
                mine->varied = true;
                memcpy(mine->bytes, b, keep);
                mine->nbytes = keep;
            }
            // Time-bounded so a tight loop on one field reports a running count
            // a few times a second rather than printing nothing until Stop.
            what = (GetTickCount64() - mine->since >= DRY_WINDOW_MS) ? EXPIRED : MERGED;
        } else {
            what = SWITCHED;
        }
    }
    if (what == MERGED) return;

    if (what == EXPIRED) {
        // Only THIS destination's collapse is closed; the run's others are
        // still accumulating and closing them here would undo the fix.
        // The write that tripped the window is already counted in the line just
        // emitted, so the next one opens from scratch.
        dry_flush_one(me, a, n);
        return;
    }

    // SWITCHED: a destination this run has no collapse open for. Take a free
    // slot; if the run already holds all of its slots, the write goes to the
    // OVERFLOW BUCKET and is counted rather than printed. See DryOver.
    bool     emit_over = false;
    DryOver  over_copy{};
    {
        std::lock_guard<std::mutex> lk(g_dry_mux);
        DrySlot* slot = nullptr;
        int mine_n = 0;
        for (int i = 0; i < DRY_MAX; i++) {
            if (!g_dry[i].have) { if (!slot) slot = &g_dry[i]; continue; }
            if (g_dry[i].run != me) continue;
            mine_n++;
        }

        if (mine_n >= DRY_PER_RUN) {
            DryOver* o = nullptr;
            for (int i = 0; i <= CORE_RUN_CAP; i++)
                if (g_over[i].have && g_over[i].run == me) { o = &g_over[i]; break; }
            if (!o) {
                for (int i = 0; i <= CORE_RUN_CAP; i++)
                    if (!g_over[i].have) { o = &g_over[i]; break; }
                if (o) { *o = DryOver{}; o->have = true; o->run = me;
                         o->since = GetTickCount64(); }
            }
            // No bucket free cannot happen within the run cap, and if it did
            // the write is still refused -- only its LINE would be lost, never
            // the refusal.
            if (!o) return;
            o->count++;
            o->last = a;
            if (GetTickCount64() - o->since >= DRY_WINDOW_MS) {
                over_copy   = *o;
                o->count    = 0;
                o->since    = GetTickCount64();
                emit_over   = true;
            }
        } else if (slot) {
            slot->have   = true;
            slot->run    = me;
            slot->addr   = a;
            slot->size   = n;
            slot->count  = 1;
            slot->varied = false;
            slot->nbytes = keep;
            memcpy(slot->bytes, b, keep);
            slot->since  = GetTickCount64();
        }
    }
    // OUTSIDE THE LOCK. ec::logf calls dry_flush, which takes g_dry_mux, so
    // emitting under it would deadlock -- the same reason dry_flush clears a
    // slot before it logs.
    if (emit_over)
        ec::logf("  [dry-run] would write %llu more times -> other addresses"
                 " (latest 0x%llx)  (arm to write)\n",
                 (unsigned long long)over_copy.count,
                 (unsigned long long)over_copy.last);
}

// ----------------------------- the only write --------------------------------

extern "C" {

FX_API size_t fx_wrblk(uint64_t a, const void* buf, size_t n) {
    if (!ec::state().armed) {
        // disarmed: RECORD the intended write and do NOT touch the process.
        dry_note(a, n, buf);
        return n;
    }
    HANDLE proc = (HANDLE)core_proc();
    if (!proc || !a) return 0;
    SIZE_T put = 0;
    NTSTATUS wst = g_nt_write(proc, (PVOID)a,
                              const_cast<void*>(buf), (SIZE_T)n, &put);
    if (!NT_SUCCESS(wst)) {
        ec::logf("  x write failed @0x%llx (NTSTATUS 0x%lx)\n",
                 (unsigned long long)a, (unsigned long)wst);
        return 0;
    }
    return (size_t)put;
}

}  // extern "C"
