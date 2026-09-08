// read.cpp -- the memory READ layer: the NtReadVirtualMemory stub and the
// helpers that turn a client address into a value, a std::string, an Instance
// name or a class name. Split out of eclipse_console.cpp's `raw memory`
// section. See read.h for why this API is deliberately published (unlike the
// write path, which is deliberately sealed). The dense comments below -- the
// boxed-string layout, the NameContainer indirection and how its failure
// presents -- travel verbatim with the code.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "fx.h"
#include "read.h"
#include "core_internal.h"   // core_proc (the client handle)
#include "offsets.h"         // off_of (the offset table lives in offsets.cpp)

// The read stub, private to this TU on purpose -- the mirror of write.cpp's
// write stub. eclipse_console.cpp hands over the finished trampoline (built by
// its make_nt_stub, which it keeps) and holds no copy of the pointer.
#ifndef NTSTATUS
typedef LONG NTSTATUS;
#endif
typedef NTSTATUS (NTAPI *pNtReadVirtualMemory)(HANDLE,PVOID,PVOID,SIZE_T,PSIZE_T);

alignas(4096) static uint8_t g_rd_stub_buf[4096];
static pNtReadVirtualMemory  g_nt_read = nullptr;

bool read_build_stub(make_stub_fn make) {
    if (!make) return false;
    g_nt_read = (pNtReadVirtualMemory)make("NtReadVirtualMemory", g_rd_stub_buf);
    return g_nt_read != nullptr;
}

// The offset "not found" sentinel, value-identical to eclipse_console.cpp's --
// off_of() returns it for an absent key.
static const uint64_t BAD = (uint64_t)-1;

bool rd_raw(uint64_t a, void* buf, size_t n) {
    // g_proc is only ever set after init_stubs() succeeded, so a non-null handle
    // guarantees a valid g_nt_read -- no fallback path and no need to test it.
    HANDLE proc = (HANDLE)core_proc();
    if (!proc || !a) return false;
    SIZE_T got = 0;
    return g_nt_read(proc, (PVOID)a, buf, (SIZE_T)n, &got) == 0 && got == n;
}

uint64_t rd64(uint64_t a) {
    uint64_t v = 0;
    return rd_raw(a, &v, 8) ? v : 0;
}

// MSVC std::string: 16-byte SSO buffer at +0, size at +0x10, capacity at +0x18.
// When capacity >= 16 the buffer is heap and +0 holds the pointer.
bool read_std_string(uint64_t addr, std::string& out, size_t limit) {
    unsigned char raw[0x20];
    if (!rd_raw(addr, raw, sizeof raw)) return false;

    uint64_t sz, cap;
    memcpy(&sz,  raw + 0x10, 8);
    memcpy(&cap, raw + 0x18, 8);
    if (sz > limit || cap > 0xFFFFFFull) return false;
    if (sz == 0) { out.clear(); return true; }

    std::string tmp;
    tmp.resize((size_t)sz);
    if (cap >= 16) {
        uint64_t ptr;
        memcpy(&ptr, raw, 8);
        if (!ptr || !rd_raw(ptr, &tmp[0], (size_t)sz)) return false;
    } else {
        memcpy(&tmp[0], raw, (size_t)sz);
    }
    out.swap(tmp);
    return true;
}

// Partial-count read backing fx_rdblk (read up to n, return however many bytes
// came back). Kept here with the stub rather than in eclipse_console.cpp,
// because g_nt_read is private to this file now.
size_t read_block(uint64_t a, void* buf, size_t n) {
    HANDLE proc = (HANDLE)core_proc();
    if (!proc || !a) return 0;
    SIZE_T got = 0;
    return g_nt_read(proc, (PVOID)a, buf, (SIZE_T)n, &got) == 0 ? (size_t)got : 0;
}

static bool printable(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) return false;
    return true;
}

// Instance.Name and ClassDescriptor.ClassName are POINTERS to a std::string on
// the Windows client, not inline strings -- verified against the live process,
// where reading them inline returns empty every time while one dereference
// gives "DataModel", "Workspace", "RunService" and so on. The Linux original
// had no equivalent (it resolved classes from a vtable map), so this is a
// genuine layout difference rather than a translation.
//
// The inline read is kept as a fallback for a build that stops boxing them; the
// printable check is what makes trying both safe, since a wrong interpretation
// yields bytes that fail it.
bool read_str_field(uint64_t at, std::string& out, size_t limit) {
    uint64_t p = rd64(at);
    if (p && read_std_string(p, out, limit) && printable(out)) return true;
    if (read_std_string(at, out, limit) && printable(out)) return true;
    out.clear();
    return false;
}

// Client version-d584fb6c717a43d9 moved an Instance's name behind a container
// pointer. The name now lives at
//     *(inst + Instance.NameContainer) + Instance.Name
// where it used to sit at inst + Instance.Name directly. Confs predating the
// change carry no NameContainer key, so the deref is skipped and the old
// one-step read stands -- an old conf still works against an old client.
//
// Worth knowing how this failure presents, because it looks like something
// else: only NAME reads broke. class_of goes through ClassDescriptor and was
// untouched, so DataModel still verifies and the status line stays green,
// while every name-driven lookup (fx_find, fx_service, the picker's
// place/username) silently returns nothing and offset-only paths like the FPS
// cap keep working perfectly.
bool read_name(uint64_t inst, std::string& out, size_t limit) {
    out.clear();
    uint64_t no = off_of("Instance.Name");
    if (no == BAD || !inst) return false;
    uint64_t nc = off_of("Instance.NameContainer");
    if (nc != BAD) {
        inst = rd64(inst + nc);
        if (!inst) return false;
    }
    return read_str_field(inst + no, out, limit);
}

std::string class_of(uint64_t obj) {
    uint64_t cd = off_of("Instance.ClassDescriptor");
    uint64_t cn = off_of("Instance.ClassName");
    if (cd == BAD || cn == BAD || !obj) return "";
    uint64_t desc = rd64(obj + cd);
    if (!desc) return "";
    std::string s;
    read_str_field(desc + cn, s, 128);
    return s;
}

// ---- snippet-facing read exports (thin wrappers over the helpers above) ------
// fx.h gives these C linkage; they call this file's internal read helpers now.
FX_API uint64_t fx_rd(uint64_t a) { return rd64(a); }

FX_API size_t fx_rdblk(uint64_t a, void* buf, size_t n) { return read_block(a, buf, n); }

FX_API const char* fx_class(uint64_t obj) {
    static thread_local std::string s;
    s = class_of(obj);
    return s.c_str();
}

FX_API const char* fx_name(uint64_t inst) {
    static thread_local std::string s;
    s.clear();
    read_name(inst, s, 256);
    return s.c_str();
}

FX_API size_t fx_str(uint64_t addr, char* buf, size_t n) {
    if (!buf || n == 0) return 0;
    std::string s;
    if (!read_std_string(addr, s)) { buf[0] = 0; return 0; }
    size_t k = s.size() < n - 1 ? s.size() : n - 1;
    memcpy(buf, s.data(), k);
    buf[k] = 0;
    return k;
}

// Committed, writable regions of the client's address space.
//
// The one primitive a value SCANNER needs and could not build for itself: to
// search for a number you first have to know where there is memory to search,
// and nothing else here answers that. fx_rdblk can then read each region.
//
// Writable only, and that is a filter rather than a nicety. Read-only pages are
// code and constants -- gigabytes of them, none of which can hold a value that
// changes as you play, so scanning them is pure cost. PAGE_GUARD is skipped
// because touching a guard page is how a process notices you did.
//
// VirtualQueryEx, not a syscall stub. The stubs exist because RPM/WPM on a game
// process are what an anti-cheat watches for; asking the kernel to describe its
// own bookkeeping is not in that class, and there is no read of the target's
// memory here at all.
//
// Returns the count written, or the count that WOULD have been written when
// `max` is 0 -- so a caller can size its own array first.
FX_API int fx_regions(uint64_t* base, uint64_t* size, int max) {
    HANDLE proc = (HANDLE)core_proc();
    if (!proc) return 0;

    MEMORY_BASIC_INFORMATION mbi{};
    uint64_t addr = 0;
    int n = 0;
    // Stop at the end of the 47-bit user half. Walking past it into kernel
    // space returns nothing and costs a query per step forever.
    const uint64_t kUserEnd = 0x7ffffffe0000ULL;

    while (addr < kUserEnd &&
           VirtualQueryEx(proc, (LPCVOID)addr, &mbi, sizeof mbi) == sizeof mbi) {
        uint64_t rgn  = (uint64_t)mbi.BaseAddress;
        uint64_t len  = (uint64_t)mbi.RegionSize;
        if (!len) break;                          // no progress; bail rather than spin

        bool writable = (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                                        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        if (mbi.State == MEM_COMMIT && writable && !(mbi.Protect & PAGE_GUARD)) {
            if (base && size && n < max) { base[n] = rgn; size[n] = len; }
            n++;
        }
        addr = rgn + len;
    }
    return n;
}
