// stubs.h -- the NT syscall trampolines and handle duplication.
//
// THE SURFACE IS DELIBERATELY TIGHT, and that is the safety property, not a
// style choice. `make_nt_stub` -- the one function in the program that can mint
// a syscall trampoline from ntdll -- is NOT declared here and never leaves
// stubs.cpp. If any TU could build an NtWriteVirtualMemory trampoline it could
// write to the client without going through fx_wrblk, and the arm gate (the one
// safety-critical invariant in this program) would be bypassable. So the
// builder stays private and stubs.cpp does the two build calls itself, handing
// make_nt_stub to read.cpp and write.cpp exactly as before -- neither of them,
// nor anyone else, ever sees the builder.
//
// What this file exposes is therefore far less than what it holds: build the
// stubs, report whether the read stub is up, and acquire a client handle
// without issuing NtOpenProcess. FUNCTIONS ONLY. No extern globals.
//
// THE memory/ DIRECTORY INVARIANT (stated here because this is where the
// trampolines are built): everything that can touch the CLIENT'S memory lives
// in memory/ -- stubs.cpp mints the trampolines, read.cpp holds the read one,
// write.cpp holds the write one and the arm gate -- and nothing outside memory/
// can reach the client's memory at all, because the only paths to it are the
// functions those three files publish.
//
// READ AND WRITE ARE SEPARATE FILES ON PURPOSE, and must stay separate (asked
// about; the answer is no). If they shared a translation unit, g_nt_write would
// be in scope for the read code, and the property step 1 exists to protect --
// that the write trampoline is reachable ONLY through fx_wrblk's arm gate --
// would be gone. The directory is the grouping; the file boundary is the
// invariant.
#pragma once

#include <windows.h>   // HANDLE, DWORD

// Build BOTH syscall stubs (read and write). Memoised: the first call builds,
// later calls return the cached verdict. True only if BOTH succeeded. There is
// no RPM/WPM fallback -- attach() treats false as fatal.
bool init_stubs();

// Is the read trampoline built? The candidate picker asks this to decide
// whether a client it found is actually readable.
bool read_stub_ok();

// A handle to `target_pid`, duplicated out of RobloxCrashHandler.exe so our
// process never issues NtOpenProcess for the game PID. nullptr if there is no
// crash handler to borrow from (the caller then falls back to OpenProcess).
HANDLE handle_from_crash_handler(DWORD target_pid);
