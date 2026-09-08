// input.h -- the host-owned low-level keyboard and mouse hooks, split out of
// eclipse_console.cpp. This TU OWNS the counter arrays, the wait semaphore, the
// claim state and the two hooks; it exposes the lifecycle the snippet run path
// drives. Functions only -- none of that state is published.
#pragma once

// Lifecycle around a run, called from the build/run path.
void kb_start(void);
void kb_stop(void);
// Per-run lifecycle. kb_run_begin acquires a wake-semaphore slot for the run
// (id 0 = CLI/un-adopted, which uses the shared fallback sem); kb_run_end
// releases the run's key claims and its slot, and tears the hook down when the
// LAST run ends. Shared key counters are NOT reset here (see input.cpp).
void kb_run_begin(unsigned id);
void kb_run_end(unsigned id);
void kb_release_claims_for(unsigned id);
void kb_wake_run(unsigned id);          // wake one run parked in fx_wait_key
void kb_wake_all(void);                 // wake every parked run (stop-all)
void kb_set_swallow_stopkey(bool on);   // CLI stop-chord swallow (Ctrl+Shift+Q)
