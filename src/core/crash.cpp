// crash.cpp -- write a readable record when the process dies unexpectedly.
//
// WHY THIS EXISTS. The app has been dying with STATUS_HEAP_CORRUPTION every few
// days since at least 2026-08-23, and every investigation hit the same wall:
// WER records a fault bucket and an offset inside ntdll, which is where the
// heap manager NOTICED the damage, not where it was done. The dumps land in
// %ProgramData%\...\WER\Temp, get cleaned, and need elevation and a debugger
// nobody has installed. So a bug that fires every few days produced no
// evidence at all, and "tested and it did not crash" could never distinguish
// fixed from lucky.
//
// TWO RULES SHAPE EVERYTHING BELOW, and both come from what we are catching:
//
//   1. NO HEAP. Not new, not std::string, not printf -- the heap is precisely
//      what is suspect when this runs, and an allocation inside the handler
//      turns a diagnosable crash into a hang or a second crash with no output.
//      Fixed stack buffers, hand-rolled formatting, CreateFileA/WriteFile.
//   2. NO SYMBOLS. dbghelp's SymInitialize allocates and walks module lists.
//      Raw return addresses are written as "module+offset" instead, which
//      addr2line resolves offline against the exe with no risk here.
//
// WHAT IT WILL AND WILL NOT CATCH, stated plainly because the difference
// matters for the bug that prompted it:
//
//   CATCHES  access violations (0xC0000005) -- the libstdc++ cluster we saw --
//            stack overflow, illegal instruction, and anything else that
//            reaches the unhandled filter.
//   MAY MISS the heap corruption itself. The heap manager reports corruption
//            through RaiseFailFastException, and a fast-fail deliberately
//            bypasses vectored and unhandled exception filters so a corrupted
//            process cannot keep running its own code. When that path is taken
//            nothing here runs. It is still worth having: the same corruption
//            usually produces an ordinary AV first, and that we do catch.
//
// If the corruption ever has to be caught AT THE WRITE rather than afterwards,
// the tool for that is PageHeap (gflags / Application Verifier), which needs
// admin and the SDK -- not something the app can turn on for itself.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "eclipse_core.h"

namespace {

// ---- allocation-free formatting --------------------------------------------
// Each of these appends to a caller-owned buffer and never grows it.

void put(char* buf, size_t cap, size_t& n, const char* s) {
    while (s && *s && n + 1 < cap) buf[n++] = *s++;
}

void put_hex(char* buf, size_t cap, size_t& n, unsigned long long v, int digits) {
    static const char* kHex = "0123456789abcdef";
    char tmp[17];
    if (digits > 16) digits = 16;
    for (int i = digits - 1; i >= 0; i--) { tmp[i] = kHex[v & 0xF]; v >>= 4; }
    tmp[digits] = 0;
    put(buf, cap, n, tmp);
}

void put_dec(char* buf, size_t cap, size_t& n, unsigned long long v) {
    char tmp[24];
    int  i = 23;
    tmp[i--] = 0;
    if (!v) tmp[i--] = '0';
    while (v && i >= 0) { tmp[i--] = char('0' + (v % 10)); v /= 10; }
    put(buf, cap, n, tmp + i + 1);
}

const char* code_name(DWORD c) {
    switch (c) {
        case 0xC0000005: return "ACCESS_VIOLATION";
        case 0xC0000374: return "HEAP_CORRUPTION";
        case 0xC00000FD: return "STACK_OVERFLOW";
        case 0xC000001D: return "ILLEGAL_INSTRUCTION";
        case 0xC0000094: return "INTEGER_DIVIDE_BY_ZERO";
        case 0xC0000096: return "PRIVILEGED_INSTRUCTION";
        case 0xC0000409: return "STACK_BUFFER_OVERRUN/FASTFAIL";
        case 0xC0000602: return "FAIL_FAST_EXCEPTION";
        default:         return "";
    }
}

// %APPDATA%\EclipseConsole\crash.txt, or beside the exe if APPDATA is missing.
// Built into a caller buffer; GetEnvironmentVariableA writes into ours.
void crash_path(char* out, size_t cap) {
    out[0] = 0;
    char appdata[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
    size_t k = 0;
    if (n && n < MAX_PATH) {
        put(out, cap, k, appdata);
        put(out, cap, k, "\\EclipseConsole");
        CreateDirectoryA(out, nullptr);          // harmless if it exists
        put(out, cap, k, "\\crash.txt");
    } else {
        char exe[MAX_PATH];
        DWORD m = GetModuleFileNameA(nullptr, exe, MAX_PATH);
        while (m && exe[m - 1] != '\\') m--;
        exe[m] = 0;
        put(out, cap, k, exe);
        put(out, cap, k, "crash.txt");
    }
    out[k < cap ? k : cap - 1] = 0;
}

// The module a return address belongs to, as a bare file name, plus the offset
// into it. That pair is what makes the trace resolvable later:
//     addr2line -e eclipse_console.exe -f -C <offset>
void put_frame(char* buf, size_t cap, size_t& n, void* addr) {
    put(buf, cap, n, "    ");
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &mod) && mod) {
        char path[MAX_PATH];
        DWORD len = GetModuleFileNameA(mod, path, MAX_PATH);
        const char* base = path;
        for (DWORD i = 0; i < len; i++) if (path[i] == '\\') base = path + i + 1;
        put(buf, cap, n, base);
        put(buf, cap, n, "+0x");
        put_hex(buf, cap, n, (unsigned long long)((char*)addr - (char*)mod), 8);
        put(buf, cap, n, "   (abs 0x");
    } else {
        put(buf, cap, n, "?   (abs 0x");
    }
    put_hex(buf, cap, n, (unsigned long long)addr, 16);
    put(buf, cap, n, ")\r\n");
}

// NO preferred_base() HELPER HERE, and this is the second attempt: reading
// OptionalHeader.ImageBase out of the loaded module returns the RUNTIME base,
// not the linker's. Windows REWRITES that field in memory once it relocates an
// ASLR image, so the value the analyst needs cannot be recovered from the
// running process at all. It comes from the file instead:
//     objdump -p eclipse_console.exe | grep ImageBase
// which is 0x140000000 on this toolchain. The hint line below says so.

LONG WINAPI on_crash(EXCEPTION_POINTERS* ep) {
    // One fixed buffer, written once. A partial record is better than none, so
    // everything below silently stops appending rather than failing.
    static char buf[8192];
    size_t n = 0;

    SYSTEMTIME st;
    GetLocalTime(&st);

    put(buf, sizeof buf, n, "\r\n=== eclipse crash ");
    put_dec(buf, sizeof buf, n, st.wYear);   put(buf, sizeof buf, n, "-");
    put_dec(buf, sizeof buf, n, st.wMonth);  put(buf, sizeof buf, n, "-");
    put_dec(buf, sizeof buf, n, st.wDay);    put(buf, sizeof buf, n, " ");
    put_dec(buf, sizeof buf, n, st.wHour);   put(buf, sizeof buf, n, ":");
    put_dec(buf, sizeof buf, n, st.wMinute); put(buf, sizeof buf, n, ":");
    put_dec(buf, sizeof buf, n, st.wSecond);
    put(buf, sizeof buf, n, " ===\r\n");

    if (ep && ep->ExceptionRecord) {
        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        put(buf, sizeof buf, n, "code    0x");
        put_hex(buf, sizeof buf, n, er->ExceptionCode, 8);
        put(buf, sizeof buf, n, "  ");
        put(buf, sizeof buf, n, code_name(er->ExceptionCode));
        put(buf, sizeof buf, n, "\r\nat      0x");
        put_hex(buf, sizeof buf, n, (unsigned long long)er->ExceptionAddress, 16);
        put(buf, sizeof buf, n, "\r\n");
        // An AV says which address it touched and whether it was reading or
        // writing -- the single most useful line for a use-after-free.
        if (er->ExceptionCode == 0xC0000005 && er->NumberParameters >= 2) {
            put(buf, sizeof buf, n, "access  ");
            put(buf, sizeof buf, n, er->ExceptionInformation[0] == 1 ? "write" :
                                    er->ExceptionInformation[0] == 8 ? "execute" : "read");
            put(buf, sizeof buf, n, " at 0x");
            put_hex(buf, sizeof buf, n, er->ExceptionInformation[1], 16);
            put(buf, sizeof buf, n, "\r\n");
        }
    }

    put(buf, sizeof buf, n, "thread  ");
    put_dec(buf, sizeof buf, n, GetCurrentThreadId());
    put(buf, sizeof buf, n, "\r\nexe     0x");
    put_hex(buf, sizeof buf, n, (unsigned long long)GetModuleHandleA(nullptr), 16);
    // The recipe, in the file, because nobody will remember it in six months
    // and the RVAs below are useless without it.
    {
        HMODULE self = GetModuleHandleA(nullptr);
        char exe[MAX_PATH];
        DWORD len = GetModuleFileNameA(self, exe, MAX_PATH);
        const char* base = exe;
        for (DWORD i = 0; i < len; i++) if (exe[i] == '\\') base = exe + i + 1;
        put(buf, sizeof buf, n, "\r\nresolve  addr2line -e ");
        put(buf, sizeof buf, n, base);
        put(buf, sizeof buf, n, " -f -C 0x<ImageBase + the RVA below>\r\n");
        put(buf, sizeof buf, n, "         ImageBase: objdump -p <exe> | grep ImageBase   (0x140000000 here)\r\n");
    }
    put(buf, sizeof buf, n, "\r\nstack (this thread, innermost first)\r\n");

    // RtlCaptureStackBackTrace, not StackWalk64: no dbghelp, no symbol
    // handler, no allocation. The first frames are this handler's own and are
    // skipped.
    void* frames[48];
    USHORT got = RtlCaptureStackBackTrace(2, 48, frames, nullptr);
    for (USHORT i = 0; i < got; i++) put_frame(buf, sizeof buf, n, frames[i]);
    if (!got) put(buf, sizeof buf, n, "    (none captured)\r\n");

    char path[MAX_PATH * 2];
    crash_path(path, sizeof path);
    HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        WriteFile(f, buf, (DWORD)n, &wrote, nullptr);
        CloseHandle(f);
    }

    // Let the normal machinery run afterwards: WER still files its report and
    // the process still dies the way it did before. This is additive -- it
    // must not change what happens on a crash, only leave a note about it.
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

namespace ec {

void install_crash_handler() {
    SetUnhandledExceptionFilter(on_crash);
}

}  // namespace ec
