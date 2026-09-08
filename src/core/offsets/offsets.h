// offsets.h -- the offset table: "Class.Field" -> RVA, parsed from the conf.
//
// This is the OWNER of the map. It was shared mutable state -- conf and preload
// wrote it, the health check and the whole read layer read it -- reached
// directly as a file-static from everywhere. That is exactly what blocked
// splitting conf/health out. Behind these functions the map is private: nothing
// outside this TU touches it, so conf and health can now live in their own TUs
// and still load and query offsets.
//
// FUNCTIONS ONLY, like read.h / write.h / core_internal.h. No extern globals.
#pragma once

#include <cstddef>
#include <cstdint>

// The offset for a key, or BAD ((uint64_t)-1) if the conf has no such key.
uint64_t off_of(const char* key);

// Writers: only conf (parsing) and preload/attach (reset) use these.
//
// THE SEQUENCE IS clear -> set xN -> commit, and commit is not optional. The
// table is published as an immutable snapshot (see offsets.cpp for why: this
// is read by every snippet thread at enormous rate and was corrupting the heap
// when a reload rebuilt it underneath them). set() fills a table nobody can
// see yet; commit() is the single atomic store that makes it live. A load that
// forgets to commit leaves the table empty, which reads as "no offsets" -- loud
// and safe, rather than half of one conf and half of another.
void   offset_set(const char* key, uint64_t val);
void   offset_clear(void);
void   offset_commit(void);
size_t offset_size(void);

// The indexed walk (backs fx_off_count / fx_off_key), so a snippet can enumerate
// the whole conf rather than only the keys its author typed out.
int         offset_count(void);
const char* offset_key(int i);
