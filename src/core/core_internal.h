// core_internal.h -- services the split-out core TUs (input.cpp,
// snippet_build.cpp, ...) call back into eclipse_console.cpp. FUNCTIONS ONLY:
// g_proc, g_st, g_off, g_stop and the read primitives stay OWNED by
// eclipse_console.cpp; nothing here is ever an `extern` global. This is the
// ui_host.h pattern, applied core-side.
#pragma once

// The run-stop flag (g_stop). Owned by eclipse_console.cpp because the
// untouched exported primitives fx_running()/fx_sleep() read it directly. The
// hooks in input.cpp set it (the CLI stop chord) and read it (fx_wait_key).
bool core_stop_requested(void);
void core_request_stop(void);

// ---- for snippet_build.cpp (the compile/run procedure) ---------------------
// Run-lifecycle state stays owned here (g_stop lives in exported primitives,
// g_extra_flags/g_snip_seq/g_run_phase in sections snippet_build must not own
// or that snippet_build accesses read-only). snippet_build.cpp reaches them
// through these, never directly.
#include <string>
bool        core_attached(void);            // g_proc != nullptr
// The handle itself, for write.cpp: it has to pass it to its syscall. That
// alone opens no write path -- the write trampoline is private to write.cpp
// and fx_wrblk, which checks the arm state, is the only caller.
void*       core_proc(void);                // g_proc, as an opaque HANDLE
void        core_dry_flush(void);           // close an open dry-run collapse
void        core_clear_stop(void);          // g_stop = false (a fresh run)
void        core_signal_stop_event(void);   // wake fx_sleep if the event exists
void        core_reset_stop_event(void);    // arm the event for a new run
std::string core_temp_dir(void);            // GetTempPath-based scratch dir
std::string core_extra_flags(void);         // the global extra g++ flags
bool        core_show_warnings(void);       // echo a clean build's warnings
unsigned    core_next_snip_seq(void);       // ++g_snip_seq, unique .dll name
void        core_run_phase_begin(void);     // fire the BUILDING->running hand-off

// ---- for window.cpp (snippet window / overlay, single-instance) ------------
// Release a run's claim on the snippet window and overlay at run end, tidying
// up anything it left open. Owned by window.cpp; called by build_and_run.
void        window_run_end(unsigned id);

// ---- for config.cpp (persisted snippet settings) ---------------------------
// Write settings.conf out if anything is unsaved. Sets are coalesced to at most
// one file write a second, so the LAST value a run stored is usually still only
// in memory when the run ends -- this is what stops it being the one that gets
// lost. Owned by config.cpp; called by build_and_run.
void        config_flush(void);

// ---- for read.cpp -----------------------------------------------------------
// read.cpp owns the read stub; the client HANDLE it needs is core_proc, already
// declared above for write.cpp. The offset lookup it needs (off_of) is owned by
// offsets.cpp and comes from offsets.h, not from here.

// ---- for conf.cpp -----------------------------------------------------------
// The conf's declared client version is stored in the attach State (g_st), read
// by the staleness check. conf.cpp writes it through this; it reads it back via
// the public ec::state().version.
void core_set_conf_version(const std::string& v);
