// stubs.cpp -- the NT syscall trampolines (make_nt_stub) and handle duplication
// (handle_from_crash_handler), split out of eclipse_console.cpp. See stubs.h
// for why the surface is deliberately tight: make_nt_stub stays PRIVATE to this
// file so no TU can mint a write trampoline and bypass the arm gate. This file
// builds both stubs itself and hands the builder to read.cpp and write.cpp; it
// keeps no read/write pointer of its own, so it too cannot reach the client's
// memory. The dense trampoline and picker-cost comments travel verbatim.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// NT function pointer types — resolved at runtime from ntdll so that no import
// table entry exists for them and no static linker dependency is created.
#ifndef NTSTATUS
typedef LONG NTSTATUS;
#endif
#ifndef NT_SUCCESS
#define NT_SUCCESS(st) (((NTSTATUS)(st)) >= 0)
#endif
typedef NTSTATUS (NTAPI *pNtReadVirtualMemory) (HANDLE,PVOID,PVOID,SIZE_T,PSIZE_T);
typedef NTSTATUS (NTAPI *pNtWriteVirtualMemory)(HANDLE,PVOID,PVOID,SIZE_T,PSIZE_T);
typedef NTSTATUS (NTAPI *pNtProtectVirtualMemory)(HANDLE,PVOID*,PSIZE_T,ULONG,PULONG);
typedef NTSTATUS (NTAPI *pNtQuerySystemInformation)(ULONG,PVOID,ULONG,PULONG);
typedef NTSTATUS (NTAPI *pNtDuplicateObject)(HANDLE,HANDLE,HANDLE,PHANDLE,
                                              ACCESS_MASK,ULONG,ULONG);

// SystemHandleInformation (class 16) record layout.
struct G_SYSTEM_HANDLE {
    ULONG       ProcessId;
    UCHAR       ObjectTypeNumber;
    UCHAR       Flags;
    USHORT      Handle;
    PVOID       Object;
    ACCESS_MASK GrantedAccess;
};
struct G_SYSTEM_HANDLE_INFORMATION {
    ULONG          HandleCount;
    G_SYSTEM_HANDLE Handles[1];
};

#include <cstdint>
#include <cstdlib>   // malloc/realloc/free for the process-list query
#include <cstring>   // memcpy in make_nt_stub
#include <mutex>
#include <unordered_map>
#include <vector>

#include "read.h"    // read_build_stub
#include "write.h"   // write_build_stub
#include "stubs.h"

// ----------------------------- NT stub layer --------------------------------
// Instead of calling ReadProcessMemory / WriteProcessMemory (Win32 wrappers that
// Byfron/Hyperion can hook at the IAT or ntdll prologue), we build a small
// trampoline for each syscall:
//
//   [copy] first 8 bytes of ntdll!NtReadVirtualMemory   <- syscall number (mov eax, N)
//   [jmp]  mov r11, <gadget>;  jmp r11                  <- jumps to the real
//                                                           "syscall; ret" inside ntdll
//
// The trampoline executes the syscall instruction from ntdll's own memory rather
// than going through the hooked function entry. Function-level hooks (inline or
// import-table) never see the call. The buffers are marked PAGE_EXECUTE_READ once
// and never touched again.

// Neither stub is here any more: the read stub's buffer and pointer are private
// to read.cpp and the write stub's to write.cpp, so this file -- which does a
// great deal besides reading and writing -- cannot reach the client's memory
// except through the functions those TUs publish (and write.cpp publishes no
// way to write at all). See read.h and write.h.
static bool g_stubs_init = false;
static bool g_rd_stub_ok  = false;   // read.cpp owns the stub; we own the verdict
static bool g_wr_stub_ok  = false;   // write.cpp owns the stub; we own the verdict

static void* make_nt_stub(const char* fn_name, uint8_t* buf) {
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) return nullptr;

    auto fn = (uintptr_t)GetProcAddress(ntdll, fn_name);
    if (!fn) return nullptr;

    // Find the "syscall; ret" gadget (0F 05) inside the ntdll function body.
    uintptr_t gadget = 0;
    for (size_t i = 0; i < 64; i++) {
        if (*(uint8_t*)(fn + i) == 0x0F && *(uint8_t*)(fn + i + 1) == 0x05) {
            gadget = fn + i;
            break;
        }
    }
    if (!gadget) return nullptr;    // prologue is hooked past the syscall instruction

    // Trampoline: 8-byte prologue (mov eax, syscall_num) + JMP to the gadget.
    uint8_t tail[13] = {
        0x49, 0xBB,
        0,0,0,0,0,0,0,0,   // imm64 = gadget address
        0x41, 0xFF, 0xE3    // jmp r11
    };
    *(uint64_t*)(&tail[2]) = gadget;
    memcpy(buf, (void*)fn, 8);
    memcpy(buf + 8, tail, sizeof tail);

    auto NtProtect = (pNtProtectVirtualMemory)
        GetProcAddress(ntdll, "NtProtectVirtualMemory");
    if (!NtProtect) return nullptr;
    PVOID base = buf;
    SIZE_T pgsz = 4096;
    ULONG old = 0;
    NtProtect(GetCurrentProcess(), &base, &pgsz, PAGE_EXECUTE_READ, &old);
    return buf;
}

// True once both stubs are built. Memoised, so the cached outcome is what a
// second call reports. There is no RPM/WPM fallback any more: if this fails the
// process has no read/write path at all, and attach() treats it as fatal rather
// than quietly limping on the very APIs the stubs exist to avoid.
bool init_stubs() {
    if (g_stubs_init) return g_rd_stub_ok && g_wr_stub_ok;
    g_stubs_init = true;
    // Handed the BUILDER, not asked for the result: read.cpp and write.cpp each
    // keep the stub they make, and this file keeps make_nt_stub. Neither
    // publishes its own.
    g_rd_stub_ok = read_build_stub(&make_nt_stub);
    g_wr_stub_ok = write_build_stub(&make_nt_stub);
    return g_rd_stub_ok && g_wr_stub_ok;
}

// ----------------------------- handle duplication ---------------------------
// Rather than calling OpenProcess(pid) directly — which leaves an obvious
// NtOpenProcess call in our process's syscall trace — we enumerate the handles
// that RobloxCrashHandler.exe already holds on RobloxPlayerBeta.exe and
// duplicate one into our process via NtDuplicateObject.  The handle arrives
// without our process ever issuing NtOpenProcess for the game PID.
//
// Returns nullptr if the crash handler is not found or the enumerate fails;
// the caller falls back to OpenProcess so the attach still succeeds.

static void find_pids_nt(const char* name, std::vector<DWORD>& out);

static DWORD find_pid_nt(const char* name) {
    std::vector<DWORD> v;
    find_pids_nt(name, v);
    return v.empty() ? 0 : v[0];
}

static void find_pids_nt(const char* name, std::vector<DWORD>& out) {
    static auto NtQSI = (pNtQuerySystemInformation)
        GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySystemInformation");
    if (!NtQSI) return;

    ULONG sz = 0x10000;
    auto* buf = (uint8_t*)malloc(sz);
    if (!buf) return;

    NTSTATUS st;
    while ((st = NtQSI(5, buf, sz, nullptr)) == (NTSTATUS)0xC0000004) {
        sz *= 2;
        auto* tmp = (uint8_t*)realloc(buf, sz);
        if (!tmp) { free(buf); return; }
        buf = tmp;
    }
    if (!NT_SUCCESS(st)) { free(buf); return; }

    // SYSTEM_PROCESS_INFORMATION layout (documented, stable).
    struct ProcEntry {
        ULONG  NextEntryOffset;
        ULONG  ThreadCount;
        BYTE   Reserved[48];
        struct { USHORT Length, MaxLen; PWSTR Buffer; } ImageName;
        LONG   BasePriority;
        HANDLE UniqueProcessId;
    };
    wchar_t wide[256]{};
    MultiByteToWideChar(CP_ACP, 0, name, -1, wide, 256);

    auto* e = (ProcEntry*)buf;
    for (;;) {
        if (e->ImageName.Buffer && _wcsicmp(e->ImageName.Buffer, wide) == 0)
            out.push_back((DWORD)(uintptr_t)e->UniqueProcessId);
        if (!e->NextEntryOffset) break;
        e = (ProcEntry*)((uint8_t*)e + e->NextEntryOffset);
    }
    free(buf);
}

HANDLE handle_from_crash_handler(DWORD target_pid) {
    static auto NtQSI = (pNtQuerySystemInformation)
        GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySystemInformation");
    static auto NtDup = (pNtDuplicateObject)
        GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtDuplicateObject");
    if (!NtQSI || !NtDup) return nullptr;

    // EVERY crash handler, not just the first. Each client spawns its own, and
    // one only holds a handle to the client it belongs to -- so with two clients
    // open, taking find_pid_nt's first hit meant the other one always fell back
    // to OpenProcess. Observed in the log as "pid NNNN unnamed via OpenProcess"
    // for exactly one of a pair.
    // Cached with the snapshot below. This is a SECOND system-wide query (the
    // process list), and it was running once per candidate -- so probing two
    // clients meant four full enumerations of the machine, not two.
    static std::vector<DWORD> s_crash_pids;
    static uint64_t s_crash_at = 0;
    {
        uint64_t now = GetTickCount64();
        if (s_crash_pids.empty() || now - s_crash_at > 1000) {
            s_crash_pids.clear();
            find_pids_nt("RobloxCrashHandler.exe", s_crash_pids);
            s_crash_at = now;
        }
    }
    const std::vector<DWORD>& crash_pids = s_crash_pids;
    if (crash_pids.empty()) return nullptr;

    // THE SYSTEM HANDLE TABLE IS CACHED for a moment, because it is the whole
    // cost of this function: it is a system-wide list, several megabytes on a
    // normal machine, and it was being fetched once PER CANDIDATE. With the
    // picker calling this for every client, opening the modal paid for it twice
    // over before anything appeared on screen.
    //
    // A second of staleness is harmless here. A handle the crash handler opened
    // a moment ago is still open, and if the snapshot misses one the caller
    // falls back exactly as it would have anyway -- and the next attempt, a tick
    // later, gets a fresh table.
    static std::mutex s_snap_mux;
    static std::vector<uint8_t> s_snap;
    static uint64_t s_snap_at = 0;

    std::lock_guard<std::mutex> snap_lk(s_snap_mux);
    uint64_t now_ms = GetTickCount64();
    if (s_snap.empty() || now_ms - s_snap_at > 1000) {
        ULONG sz = 0x10000;
        s_snap.resize(sz);
        NTSTATUS st;
        while ((st = NtQSI(16, s_snap.data(), sz, nullptr)) == (NTSTATUS)0xC0000004) {
            sz *= 2;
            if (sz > (64u << 20)) { s_snap.clear(); return nullptr; }   // sanity
            s_snap.resize(sz);
        }
        if (!NT_SUCCESS(st)) { s_snap.clear(); return nullptr; }
        s_snap_at = now_ms;
    }
    auto* info = (G_SYSTEM_HANDLE_INFORMATION*)s_snap.data();

    // TWO THINGS STOP THIS BEING THE WHOLE COST OF THE PICKER. Measured before
    // either: 90-140ms of a 110-160ms probe was spent here, against 2ms for the
    // module base and 0ms for the DataModel walk.
    //
    // 1. OBJECT TYPE. A crash handler holds hundreds of handles and almost none
    //    are processes; duplicating a file or a registry key just to ask
    //    GetProcessId about it is three syscalls thrown away. The type number is
    //    not contractual across Windows builds, so it is LEARNED -- the first
    //    duplication that turns out to be a process teaches it, and everything
    //    of another type is skipped for free from then on.
    //
    // 2. WHO OWNS WHAT, remembered for the life of the snapshot. The picker asks
    //    about each client in turn and the answers cannot change in between, so
    //    the second client is a lookup rather than another full sweep.
    static UCHAR s_proc_type = 0;                       // 0 = not learned yet
    static std::unordered_map<uint64_t, DWORD> s_owner; // (crash pid, handle) -> pid
    static uint64_t s_owner_for = 0;
    if (s_owner_for != s_snap_at) { s_owner.clear(); s_owner_for = s_snap_at; }

    HANDLE result = nullptr;
    for (size_t c = 0; c < crash_pids.size() && !result; c++) {
        DWORD crash_pid = crash_pids[c];
        HANDLE hCrash = OpenProcess(PROCESS_DUP_HANDLE, FALSE, crash_pid);
        if (!hCrash) continue;

        for (ULONG i = 0; i < info->HandleCount && !result; i++) {
            auto& h = info->Handles[i];
            if (h.ProcessId != (ULONG)crash_pid) continue;
            if (s_proc_type && h.ObjectTypeNumber != s_proc_type) continue;

            uint64_t key = ((uint64_t)crash_pid << 32) | (uint32_t)h.Handle;
            auto known = s_owner.find(key);
            if (known != s_owner.end() && known->second != target_pid) continue;

            HANDLE dup = nullptr;
            NTSTATUS ds = NtDup(
                hCrash, (HANDLE)(uintptr_t)h.Handle, GetCurrentProcess(), &dup,
                PROCESS_VM_READ | PROCESS_VM_WRITE |
                PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION,
                0, 0);
            if (!NT_SUCCESS(ds)) continue;

            DWORD owner = GetProcessId(dup);
            if (owner) {                       // it really is a process handle
                s_proc_type  = h.ObjectTypeNumber;
                s_owner[key] = owner;
            }
            if (owner == target_pid) result = dup;
            else CloseHandle(dup);
        }
        CloseHandle(hCrash);
    }

    return result;      // `info` points into the cache; not ours to free
}

// The candidate picker's readable-check reads this; g_rd_stub_ok stays private.
bool read_stub_ok() { return g_rd_stub_ok; }
