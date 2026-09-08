// run.h -- the multi-run REGISTRY seam. runtime.cpp owns the Run objects, the
// id allocator, the per-thread "current run" pointer and the stop-all latch;
// everything else reaches them through the functions here. FUNCTIONS ONLY, no
// extern globals -- the same rule core_internal.h and the ui host.h follow.
//
// Why a registry at all: the console used to serialise runs (one g_stop, one
// stop event, one loaded DLL). Multi-run gives each execution its own Run, so
// Stop, fx_sleep and fx_wait_key resolve PER RUN. The context comes from the
// THREAD, not from a parameter -- that is what keeps fx.h a pure-C boundary:
// every fx_* export resolves its own run through t_run for free, and no
// snippet source changes.
//
// See docs/multi-run.md for the design and the two places the obvious approach
// is wrong (snippet-spawned threads, and the per-run semaphore).
#pragma once

#include <string>
#include <vector>

// Concurrency cap. Threads are cheap; the cap exists for the WEDGED snippet --
// one that never polls fx_running() cannot be stopped and holds its thread,
// DLL, stop event and semaphore forever. A wedged run still counts toward this
// (a cap that excused the leaked ones would measure the wrong thing).
#define CORE_RUN_CAP 4

// ---- lifecycle (driven by spawn_run in the UI, and by the CLI path) --------
//
// core_run_create: register a new Run, allocate its id (1..N, never reused),
// create its own stop event (manual-reset, non-signalled). Returns the id, or
// 0 if the cap is already full -- the caller names what holds the slots. Does
// NOT set the current run; the spawning thread does that next.
unsigned core_run_create(const std::string& label);

// Bind this THREAD to a run. spawn_run calls it on the run thread before
// running the snippet; fx_adopt_run calls it on a snippet-spawned worker so
// fx_running() there means "MY run" rather than the stop-all fallback.
void core_run_set_current(unsigned id);
void core_run_clear_current(void);          // back to un-adopted (t_run = null)
unsigned core_run_current_id(void);         // 0 when the thread has no run

// Free a run: close its stop event and unregister it. Called after the run's
// body() returns (the DLL has already been unloaded on this same thread by
// build_and_run, so nothing is executing freed code).
void core_run_destroy(unsigned id);

// ---- stopping --------------------------------------------------------------
// Cooperative by necessity: a looping snippet polls fx_running() and returns.
// These set the flag, signal the run's stop event (wakes fx_sleep) and stamp
// the stop time (drives the "stopping..." UI and the wedge watchdog). Waking a
// snippet parked in fx_wait_key is the keyboard layer's job -- the run path
// calls kb_wake_run / kb_wake_all after these.
void core_run_stop(unsigned id);            // one run
void core_run_stop_all(void);               // every run + the stop-all latch

// The un-adopted-thread fallback: a snippet-spawned thread that never called
// fx_adopt_run has no t_run, so it stops only when EVERYTHING stops. This is
// the latch core_run_stop_all sets and a fresh first run clears.
bool core_run_stop_all_active(void);

// ---- introspection (the UI list, the cap refusal, the watchdog) ------------
struct CoreRunInfo {
    unsigned    id;
    std::string label;
    bool        building;      // compiling, not yet executing
    bool        stopping;      // Stop asked, run still present (maybe wedged)
    unsigned    stop_ms;       // ms since Stop was asked, 0 if not stopping
};
std::vector<CoreRunInfo> core_run_list(void);
int         core_run_count(void);
std::string core_run_label(unsigned id);    // "" if no such run

// Set by core_run_phase_begin(): a run stops being "building" the instant its
// snippet starts executing. Lives here so the flag travels with the Run.
void core_run_mark_running(unsigned id);
