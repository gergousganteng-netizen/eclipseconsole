// runtime.cpp -- the snippet RUN layer: the stop flag and its event, the
// high-resolution sleep/pace clock, and the log sink. Split out of
// eclipse_console.cpp. It OWNS this state (nothing outside sees the run
// registry, the stop events, or the log function pointer) and exposes it through functions --
// fx_running/fx_sleep/fx_pace/fx_log/fx_logf for snippets, the run-state
// forwarders for snippet_build.cpp and input.cpp, ec::set_log/ec::logf for the
// host. Comments travel verbatim.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "fx.h"
#include "eclipse_core.h"    // ec::LogFn, ec::logf decl, ec::self_dir
#include "core_internal.h"   // the run-state forwarders declared for the other TUs
#include "run.h"             // the multi-run registry seam (owned here)
#include "write.h"           // dry_flush (logf closes an open dry-run collapse)

using ec::logf;   // logf is declared in eclipse_core.h and defined below

static ec::LogFn         g_logfn         = nullptr;

// ======================= the multi-run registry ============================
// One Run per execution. runtime.cpp owns them because a Run's core identity --
// its stop flag and its own stop event -- is exactly the run-layer state this
// TU has always owned; multi-run just makes it N of them instead of one. The
// registry is reached only through the functions in run.h; nothing here is
// published. See docs/multi-run.md.
struct Run {
    unsigned           id;
    std::string        label;
    std::atomic<bool>  stop{false};
    std::atomic<bool>  building{true};          // compiling, not yet executing
    std::atomic<DWORD> stop_at{0};              // GetTickCount()|1 when Stop asked
    HANDLE             stop_event = nullptr;    // its own; wakes fx_sleep on this run only
};

static std::mutex                        g_runs_mux;
static std::vector<std::unique_ptr<Run>> g_runs;          // GUARDED by g_runs_mux
static std::atomic<unsigned>             g_next_id{1};

// The CALLING thread's run. Set by spawn_run on the run thread (and by
// fx_adopt_run on a snippet-spawned worker). Null on an un-adopted thread and
// on the CLI, where the stop-all latch governs instead. thread_local, so every
// fx_* export resolves its own run for free with no parameter and no change to
// the C boundary.
static thread_local Run* t_run = nullptr;

// The un-adopted / CLI stop signal. A thread with no t_run stops only when
// EVERYTHING stops -- detach, client swap, offsets going stale, teardown. This
// is also the whole of the CLI's stop state: the CLI never registers a Run, so
// t_run stays null there and this latch plays the old single g_stop's role.
static std::atomic<bool> g_stop_all{false};
static HANDLE            g_all_event = nullptr;    // the un-adopted fallback wait target

// THE CONSOLE'S OWN THREADS ARE NOT A RUN, AND NEVER WERE.
//
// fx_running() answers "is MY run stopping". With t_run null it falls back to
// the stop-all latch, which is right for a snippet's un-adopted worker (it
// should die when its snippet does) and WRONG for the host's ticks, which have
// no run to stop. That fallback took the in-game panel dark for whole
// sessions: stop_all_runs() latches g_stop_all at client exit, client swap,
// offsets going stale, detach and teardown, and only the START OF THE NEXT RUN
// clears it. Until then every fx_service()/fx_find() walk on a host thread
// returned 0 at its first child -- so the character cache could never
// re-resolve Players after a rejoin, and HP, the username and the offset
// health check all read empty until the user happened to run a snippet.
//
// Marked per thread rather than inferred: "no run" cannot distinguish the two
// cases, and guessing wrong in the other direction would leave a snippet's
// worker unstoppable. Set by ec::mark_host_thread at each host thread's entry;
// no fx_* export can reach it, so a snippet cannot mark itself exempt.
static thread_local bool t_host = false;

static HANDLE all_event(void) {
    // Created once for the session and never closed, same reasoning the single
    // stop event always had: closing it would race a stop arriving from another
    // thread just as a run returns.
    if (!g_all_event) g_all_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    return g_all_event;
}

// find under the lock. The Run lives until core_run_destroy, which only runs
// after the run's body() has returned on its own thread, so a pointer taken
// under the lock and used briefly is safe.
static Run* find_run(unsigned id) {
    for (auto& r : g_runs) if (r->id == id) return r.get();
    return nullptr;
}

// The one question fx_running/fx_sleep/fx_pace/fx_wait_key all reduce to: is
// the CALLING thread's run stopping?
static bool cur_stopping(void) {
    Run* r = t_run;
    if (r)      return r->stop.load();
    if (t_host) return false;          // see t_host above
    return g_stop_all.load();
}
static HANDLE cur_stop_event(void) {
    Run* r = t_run;
    return r ? r->stop_event : all_event();
}

FX_API int fx_running(void) { return cur_stopping() ? 0 : 1; }

// ---- additive fx_* exports for snippet-spawned threads ---------------------
// A snippet that starts its own std::thread leaves t_run null on it. Un-adopted
// threads fall back to the stop-all latch -- safe but coarse: they stop only
// when everything does, never on their own snippet's Stop. A worker that must
// cut on its OWN Stop adopts the run:
//
//   unsigned rid = fx_run_id();                 // on the run thread
//   worker = std::thread([rid]{ fx_adopt_run(rid); dash_run(plan); });
//
// backdash is the live counter-example the doc names: the join at the end of
// its fx_run DEPENDS on the worker observing fx_running() == false after Stop.
// POD in, void out -- the fx_* boundary stays C, and both exports are purely
// additive (nothing existing changes shape).
FX_API unsigned fx_run_id(void)          { return core_run_current_id(); }
FX_API void     fx_adopt_run(unsigned id){ core_run_set_current(id); }

// NOT Sleep(). Its resolution is the scheduler tick, ~15.6ms, so fx_sleep(16)
// -- the "60 Hz" every overlay loop in here asks for -- returned after 30ms
// measured. Nothing ran at the rate it claimed.
//
// NOT timeBeginPeriod either, which is the older answer: it raises the timer
// resolution for the whole machine and costs battery in every other process
// on it. A high-resolution waitable timer gets the same accuracy with no
// side effect outside this one.
//
// CREATE_WAITABLE_TIMER_HIGH_RESOLUTION is Win10 1803+. Older systems fall
// back to an ordinary timer -- still better than Sleep, because the wait ends
// on Stop as well as on the timer.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#  define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// One timer per thread, closed when that thread ends. Shared between threads
// it could not be: SetWaitableTimer on a handle another thread is waiting on
// would retarget that wait.
struct SleepTimer {
    HANDLE h = nullptr;
    ~SleepTimer() { if (h) CloseHandle(h); }
};
static thread_local SleepTimer g_sleep_timer;

static HANDLE sleep_timer(void) {
    if (!g_sleep_timer.h) {
        g_sleep_timer.h = CreateWaitableTimerExW(
            nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!g_sleep_timer.h)
            g_sleep_timer.h = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    return g_sleep_timer.h;
}

// How much of the tail to take back from the scheduler by busy-waiting, in
// milliseconds. 0.0 disables the spin and leaves a pure waitable-timer sleep.
//
// A yielding wait cannot beat ~0.35ms: that is how long Windows takes to put
// the thread back on a core once the timer signals. It is a CONSTANT, not a
// fraction, so it is 1% of a 33ms sleep and 50% of a 1ms one -- only the
// short end suffers, and only the short end pays to fix it.
//
// The margin is capped at a QUARTER of the sleep. Without that cap a fast
// loop spins most of its period; with it, cost peaks around 2ms (~13% of a
// core) and falls off in BOTH directions.
//
// Measured, sustained: 1ms 1.10ms/4.7%   16ms 16.02ms/1.6%   50ms 50.02/1.0%.
// At 0.0 those read 1.53ms, 16.34ms, 50.4ms for no CPU at all.
static const double SPIN_MS = 0.5;

// The one wait both fx_sleep and fx_pace go through. 100ns units because that
// is the timer's own resolution -- fx_pace has a sub-millisecond remainder to
// wait out, and rounding it to whole ms would put the drift straight back.

static int wait_100ns(LONGLONG hns) {
    if (cur_stopping())   return 0;
    if (hns <= 0) return 1;

    // Only bother with the counter if there is a spin to pace against.
    LARGE_INTEGER f, now;
    bool spin = SPIN_MS > 0.0 &&
                QueryPerformanceFrequency(&f) && QueryPerformanceCounter(&now);

    LONGLONG deadline = 0, timerHns = hns;
    if (spin) {
        double ticksPerHns = (double)f.QuadPart / 10000000.0;
        deadline = now.QuadPart + (LONGLONG)((double)hns * ticksPerHns);

        double marginMs = SPIN_MS;
        double wantMs   = (double)hns / 10000.0;
        if (marginMs > wantMs / 4.0) marginMs = wantMs / 4.0;   // the cap

        timerHns = hns - (LONGLONG)(marginMs * 10000.0);
        if (timerHns < 0) timerHns = 0;
    }

    HANDLE t = sleep_timer(), s = cur_stop_event();
    bool waited = false;
    if (t && s && timerHns > 0) {
        LARGE_INTEGER due;
        due.QuadPart = -timerHns;                    // negative == relative
        if (SetWaitableTimer(t, &due, 0, nullptr, nullptr, FALSE)) {
            HANDLE objs[2] = { t, s };
            DWORD r = WaitForMultipleObjects(2, objs, FALSE, INFINITE);
            CancelWaitableTimer(t);
            // Index 1 is Stop. Any failure falls through to the cur_stopping() read,
            // which is the real answer either way.
            if (r == WAIT_OBJECT_0 + 1) return 0;
            if (cur_stopping()) return 0;
            waited = true;
        }
    }

    if (spin) {
        // Tail only -- bounded by the margin above, never the whole wait.
        for (;;) {
            if (cur_stopping()) return 0;
            QueryPerformanceCounter(&now);
            if (now.QuadPart >= deadline) break;
            YieldProcessor();
        }
        return cur_stopping() ? 0 : 1;
    }
    if (waited) return cur_stopping() ? 0 : 1;

    // No timer at all. The old sliced Sleep, kept as the floor.
    unsigned ms = (unsigned)(hns / 10000);
    const unsigned step = 25;
    while (ms && !cur_stopping()) {
        unsigned n = ms < step ? ms : step;
        Sleep(n);
        ms -= n;
    }
    return cur_stopping() ? 0 : 1;
}

FX_API int fx_sleep(unsigned ms) {
    if (cur_stopping()) return 0;
    if (!ms)    return 1;
    return wait_100ns((LONGLONG)ms * 10000LL);
}

// ---- fx_pace: hold a rate, rather than wait a duration ---------------------
// fx_sleep(16) does not give 60 Hz. The body's own runtime adds to every
// period, so a loop spending 5ms reading players then sleeping 16 runs at 47
// Hz -- an error fifteen times the sleep's own, and no amount of sleep
// accuracy touches it.
//
// So this waits until the next PERIOD BOUNDARY instead of for a fixed span,
// and the body's runtime comes out of the wait.
//
//   while (fx_running()) { work(); if (!fx_pace(60)) break; }
//
// The deadline advances from the PREVIOUS DEADLINE, not from now, which is
// what makes the error self-cancelling: a wait that overshoots leaves the
// next one correspondingly shorter. `next = now + period` would compound
// instead, and drift without bound.
//
// Per-thread state, so two threads in one snippet can each hold their own
// rate without interfering.
struct Pacer {
    long long next   = 0;      // QPC ticks; the deadline being aimed at
    long long period = 0;      // QPC ticks
};
static thread_local Pacer g_pacer;

FX_API int fx_pace(unsigned hz) {
    if (cur_stopping()) return 0;
    if (!hz)    return 1;

    LARGE_INTEGER f, now;
    if (!QueryPerformanceFrequency(&f) || !QueryPerformanceCounter(&now))
        return fx_sleep(1000 / hz);

    long long period = f.QuadPart / (long long)hz;
    if (period <= 0) return 1;                    // hz finer than the counter

    // First call, a changed rate, or a deadline left over from a previous run
    // on this thread: start the cadence here rather than trying to honour a
    // boundary that belongs to a different loop.
    if (g_pacer.period != period || g_pacer.next <= now.QuadPart - period) {
        g_pacer.period = period;
        g_pacer.next   = now.QuadPart + period;
    }

    int alive = 1;
    long long remain = g_pacer.next - now.QuadPart;
    if (remain > 0) {
        // Ticks -> 100ns. Scaled before dividing so a high-frequency counter
        // does not floor the whole thing to zero.
        alive = wait_100ns(remain * 10000000LL / f.QuadPart);
    }

    g_pacer.next += period;

    // Overran the period -- the body took longer than the whole budget. Do
    // NOT leave the deadline in the past: it would stay behind for several
    // iterations and the loop would run flat out "catching up", which is a
    // burst of work at exactly the moment it is already too slow. Give up the
    // lost time and re-aim from now.
    QueryPerformanceCounter(&now);
    if (g_pacer.next <= now.QuadPart) g_pacer.next = now.QuadPart + period;

    return (alive && !cur_stopping()) ? 1 : 0;
}

// ---- the log sink -----------------------------------------------------------
// fx_log/fx_logf run on the snippet's OWN thread, so t_run is right there and
// attribution is free and exact. Tag the line with the run label ONLY when more
// than one run is live -- a lone run needs no prefix, and the single-run output
// stays byte-for-byte what it was. (Captured printf cannot be attributed this
// way: fd 1 is per-process, so it is marked unattributed in fx.h instead.)
static void log_tagged(const char* body) {
    if (core_run_count() > 1) {
        std::string label = core_run_label(core_run_current_id());
        logf("  [%s] %s\n", label.empty() ? "?" : label.c_str(), body);
    } else {
        logf("  %s\n", body);
    }
}

FX_API void     fx_log(const char* m) { log_tagged(m ? m : ""); }

// The formatting sibling. Added because everyone reaches for fx_log("x=%d", v)
// first, and because printf -- which does work -- only works while the snippet
// and the host share a C runtime: rebinding fd 1 gives two independent stdio
// states otherwise and the output vanishes with no error at all (see the
// toolchain note in CLAUDE.md). This crosses the fx_* boundary instead, so it
// reports whatever the snippet was built with.
//
// fx_log itself is deliberately NOT made variadic: snippets pass runtime
// strings to it, and any of those containing a '%' would start being read as
// a format the day it changed.
FX_API void     fx_logf(const char* fmt, ...) {
    if (!fmt) return;
    char b[1024];
    va_list a;
    va_start(a, fmt);
    // Truncation over refusal: a log line is diagnostics, and half of one
    // still says more than nothing.
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    log_tagged(b);
}

// Where a snippet's own files live: %APPDATA%\EclipseConsole\snippets.
//
// Same rule the UI uses, computed here rather than read from it so the CLI --
// which has no snippets folder of its own -- gives the same answer.
FX_API const char* fx_snippets_dir(void) {
    static std::string dir;
    if (!dir.empty()) return dir.c_str();
    char appdata[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
    dir = (n && n < MAX_PATH) ? std::string(appdata) + "\\EclipseConsole\\snippets"
                              : ec::self_dir() + "\\snippets";
    return dir.c_str();
}

namespace ec {

// Called once at the entry of each console-owned thread. See t_host.
void mark_host_thread(void) { t_host = true; }

void set_log(LogFn fn) { g_logfn = fn; }

// logf closes an open dry-run collapse before printing anything else, or the
// ×N line lands after output it actually preceded. dry_flush comes from
// write.h, included above with the other project headers.
void logf(const char* fmt, ...) {
    // Any other output ends the run being collapsed. dry_flush clears the
    // pending state before it logs, so the nested call back into here is a
    // no-op rather than infinite recursion.
    dry_flush();

    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_logfn) g_logfn(buf);
    else { fputs(buf, stdout); fflush(stdout); }
}

}  // namespace ec

// ---- run-state forwarders --------------------------------------------------
// Now resolve PER RUN through t_run. The CLI/un-adopted path (t_run null) falls
// back to the stop-all latch, which is exactly the old single-cur_stopping() behaviour.
bool core_stop_requested(void) { return cur_stopping(); }

// The CLI stop chord (Ctrl+Shift+Q), fired from inside the WH_KEYBOARD_LL proc.
// Lock-free and allocation-free ON PURPOSE -- it runs in the hook. The CLI is
// single-run and registers no Run, so setting the latch and signalling the
// fallback event is the whole job; there is no registry to walk here.
void core_request_stop(void) {
    g_stop_all = true;
    if (g_all_event) SetEvent(g_all_event);
}

// Called by build_and_run at the start of a run, on the RUN thread (t_run set)
// or on the CLI main thread (t_run null). Per run it is a no-op in practice
// (the Run is born un-stopped with a reset event); on the CLI it clears the
// latch and re-arms the fallback event for the fresh run.
void core_clear_stop(void) {
    if (t_run) t_run->stop = false;
    else       g_stop_all = false;
}
void core_signal_stop_event(void) { HANDLE e = cur_stop_event(); if (e) SetEvent(e); }
void core_reset_stop_event(void)  { HANDLE e = cur_stop_event(); if (e) ResetEvent(e); }

// ---- the registry itself (run.h) -------------------------------------------
unsigned core_run_create(const std::string& label) {
    std::lock_guard<std::mutex> lk(g_runs_mux);
    if ((int)g_runs.size() >= CORE_RUN_CAP) return 0;      // cap; caller names the holders
    // The first run of a fresh batch clears the stop-all latch a previous
    // detach / teardown / stale event left set, so un-adopted threads in the
    // new run are not born stopped.
    if (g_runs.empty()) { g_stop_all = false; if (g_all_event) ResetEvent(g_all_event); }
    auto r = std::make_unique<Run>();
    r->id         = g_next_id.fetch_add(1);
    r->label      = label;
    r->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // manual-reset, non-signalled
    unsigned id   = r->id;
    g_runs.push_back(std::move(r));
    return id;
}

void core_run_set_current(unsigned id) {
    std::lock_guard<std::mutex> lk(g_runs_mux);
    t_run = find_run(id);
}
void core_run_clear_current(void) { t_run = nullptr; }

unsigned core_run_current_id(void) { Run* r = t_run; return r ? r->id : 0; }

void core_run_destroy(unsigned id) {
    HANDLE ev = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_runs_mux);
        for (size_t i = 0; i < g_runs.size(); i++) {
            if (g_runs[i]->id == id) {
                ev = g_runs[i]->stop_event;
                if (t_run == g_runs[i].get()) t_run = nullptr;
                g_runs.erase(g_runs.begin() + i);
                break;
            }
        }
    }
    if (ev) CloseHandle(ev);
}

void core_run_stop(unsigned id) {
    std::lock_guard<std::mutex> lk(g_runs_mux);
    Run* r = find_run(id);
    if (!r) return;
    r->stop = true;
    DWORD z = 0; r->stop_at.compare_exchange_strong(z, GetTickCount() | 1u);  // first Stop only
    if (r->stop_event) SetEvent(r->stop_event);
}

void core_run_stop_all(void) {
    std::lock_guard<std::mutex> lk(g_runs_mux);
    g_stop_all = true;
    if (g_all_event) SetEvent(g_all_event);
    DWORD now = GetTickCount() | 1u;
    for (auto& r : g_runs) {
        r->stop = true;
        DWORD z = 0; r->stop_at.compare_exchange_strong(z, now);
        if (r->stop_event) SetEvent(r->stop_event);
    }
}

bool core_run_stop_all_active(void) { return g_stop_all.load(); }

void core_run_mark_running(unsigned id) {
    std::lock_guard<std::mutex> lk(g_runs_mux);
    if (Run* r = find_run(id)) r->building = false;
}

std::vector<CoreRunInfo> core_run_list(void) {
    std::vector<CoreRunInfo> out;
    std::lock_guard<std::mutex> lk(g_runs_mux);
    DWORD now = GetTickCount();
    for (auto& r : g_runs) {
        CoreRunInfo ci;
        ci.id       = r->id;
        ci.label    = r->label;
        ci.building = r->building.load();
        DWORD at    = r->stop_at.load();
        ci.stopping = at != 0;
        ci.stop_ms  = at ? (now - at) : 0;      // |1 in the stamp costs at most 1ms
        out.push_back(std::move(ci));
    }
    return out;
}

int core_run_count(void) {
    std::lock_guard<std::mutex> lk(g_runs_mux);
    return (int)g_runs.size();
}

std::string core_run_label(unsigned id) {
    std::lock_guard<std::mutex> lk(g_runs_mux);
    Run* r = find_run(id);
    return r ? r->label : std::string();
}
