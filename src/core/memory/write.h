// write.h -- the seam to the isolated WRITE PATH.
//
// Why this file exists at all: `fx_wrblk` is the only write primitive in the
// program and it is DRY-RUN UNTIL ARMED. That gate is the safety-critical
// invariant in this codebase (see the BOUNDARY note at the top of
// eclipse_console.cpp), and it used to sit in the middle of a 2,600-line file
// alongside reads, offsets, attach and the object-graph helpers. It now has a
// file of its own, small enough to audit in one screen: the gate, the single
// call that can reach the process, and the dry-run recorder that stands in for
// it while disarmed. Nothing else belongs in write.cpp.
//
// The stronger property this buys, and the reason it is worth touching
// safety-critical code to get: THE WRITE SYSCALL IS NOT REACHABLE FROM
// ANYWHERE ELSE. The trampoline pointer is private to write.cpp, so no other
// translation unit can perform a write even by accident -- it would have to go
// through fx_wrblk, which checks the arm state first. Before the split the
// pointer was file-static in a file that does a great deal besides writing.
//
// FUNCTIONS ONLY, like core_internal.h and ui_host.h. No `extern` globals.
#pragma once

#include <stdint.h>

// The trampoline BUILDER (make_nt_stub) stays private to eclipse_console.cpp,
// and the finished write stub stays private to write.cpp. Passing the builder
// in means neither side has to publish its own: eclipse_console.cpp never sees
// the write pointer, and write.cpp never gains the ability to mint another.
typedef void* (*make_stub_fn)(const char* fn_name, uint8_t* buf);

// Build the write trampoline. Called once, from init_stubs, and its result is
// half of "the stubs are ready" -- attach() treats failure as fatal, because
// there is no RPM/WPM fallback to limp on.
bool write_build_stub(make_stub_fn make);

// Close an open dry-run collapse. Called by logf (any other output ends the
// run being collapsed) and by set_armed / set_dry_window_ms.
void dry_flush(void);

// Settings -> Console. Applied through a function so the flush-first rule
// lives with the machinery it protects rather than at the call site.
void write_set_dry_window_ms(unsigned ms);
