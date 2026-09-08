// read.h -- the memory READ layer, split out of eclipse_console.cpp.
//
// This file PUBLISHES a widely-used API, and that is a deliberate decision
// rather than a tidy-up. write.h REMOVED a capability (the write syscall is now
// reachable from nowhere else); this one does the opposite -- it hands rd64,
// class_of, read_name and the rest to any TU that includes it. That is
// acceptable because READS ARE NOT GATED: nothing bad happens if another part
// of the program reads the client's memory, so there is no invariant to protect
// by keeping them private, only call-site noise to avoid by not.
//
// What must NOT happen is this becoming a way to hand-roll a write. The read
// stub is NtReadVirtualMemory and stays that; the buffer and pointer are
// private to read.cpp exactly as the write stub is private to write.cpp, and
// nothing here exposes a means of building or reaching a write stub. The one
// builder that can mint a syscall trampoline (make_nt_stub) stays in
// eclipse_console.cpp and is handed in, never published.
//
// FUNCTIONS ONLY, like write.h / core_internal.h / ui_host.h. No extern globals.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Build the read trampoline. Handed the BUILDER (make_nt_stub) exactly as
// write_build_stub is: eclipse_console.cpp keeps the builder, read.cpp keeps the
// finished NtReadVirtualMemory stub, and neither publishes its own. Called once
// from init_stubs; a false return is fatal to attach() (there is no RPM
// fallback).
typedef void* (*make_stub_fn)(const char* fn_name, uint8_t* buf);
bool read_build_stub(make_stub_fn make);

// All-or-nothing read: true only if all n bytes came back.
bool rd_raw(uint64_t a, void* buf, size_t n);
// Partial read: returns however many bytes were actually read (0 on failure).
// Backs fx_rdblk, whose contract is "read up to n", unlike rd_raw's "all or none".
size_t read_block(uint64_t a, void* buf, size_t n);
// One 8-byte pointer/word; 0 on failure.
uint64_t rd64(uint64_t a);
// An MSVC std::string living in the client at `addr`.
bool read_std_string(uint64_t addr, std::string& out, size_t limit = 4096);
// A std::string field that may be boxed behind a pointer (Name, ClassName).
bool read_str_field(uint64_t at, std::string& out, size_t limit);
// An Instance's Name, following the NameContainer indirection when present.
bool read_name(uint64_t inst, std::string& out, size_t limit);
// An Instance's class, via ClassDescriptor -> ClassName.
std::string class_of(uint64_t obj);
