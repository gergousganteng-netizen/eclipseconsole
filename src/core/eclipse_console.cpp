// eclipse_console.cpp — Windows port of the Eclipse external-memory console.
//
// Ported from the Linux eclipse_console.cpp. PORT, not invent: the shape is the
// same (paste C++ -> compile to a shared object -> load it into OUR OWN process
// -> run it against the game's memory from the outside), only the OS primitives
// changed.
//
//   process_vm_readv/writev  ->  indirect NtReadVirtualMemory / NtWriteVirtualMemory
//                                syscall stubs -- see the NT stub layer below for
//                                why not the RPM/WPM wrappers
//   /proc scan               ->  Toolhelp32 snapshot
//   dlopen/dlsym/dlclose     ->  LoadLibrary / GetProcAddress / FreeLibrary
//   .so + -fPIC              ->  .dll  (PIC is implicit on Windows)
//   -rdynamic                ->  host exports fx_* + snippets link its import lib
//   /tmp                     ->  GetTempPath
//   /proc/self/exe           ->  GetModuleFileName
//
// BOUNDARY (load-bearing, unchanged): this writes DATA the engine reads. It
// never touches the Luau VM, never emits bytecode, never injects into the game
// process. fx_wrblk is the only write primitive and it is DRY-RUN until armed.
//
// Two things the Linux build didn't need and this one does:
//   * a module base — every static offset here is an RVA, and Windows has ASLR
//     on, so nothing is at a fixed address the way it was under flux.
//   * class names by descriptor rather than a vtable map: the Windows offsets
//     give Instance.ClassDescriptor -> ClassName, so /tmp/vtmap.tsv is obsolete.

#define WIN32_LEAN_AND_MEAN
// FX_HOST comes from the build (see CMakeLists.txt) so fx.h exports here and
// imports everywhere else. Defining it again locally would only mask a build
// that forgot it.
#ifndef FX_HOST
#  error "build this file with -DFX_HOST"
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cmath>        // health_check: sqrtf/fabsf for the geometry assertions
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <vector>

#include "fx.h"
#include "eclipse_core.h"
#include "core_internal.h"   // forwarders THIS file provides to the split TUs
#include "run.h"             // core_run_current_id / core_run_mark_running
#include "input.h"           // the keyboard/mouse hooks, now their own TU
#include "write.h"           // fx_wrblk, the arm gate and the dry-run recorder
#include "read.h"            // the memory read layer (rd64, class_of, read_name, ...)
#include "offsets.h"         // the offset table behind accessors (off_of, offset_set, ...)
#include "conf.h"            // load_offsets: parse the conf into the table
#include "stubs.h"           // init_stubs + handle acquisition (make_nt_stub stays private)
#include "cache.h"           // LocalPlayer/Character + part caches (own the mutex)

// ----------------------------- state ----------------------------------------

static const uint64_t BAD = (uint64_t)-1;

static HANDLE   g_proc   = nullptr;      // cached handle to the client
static ec::State g_st;
static unsigned  g_snip_seq = 0;         // unique .dll name per run; Windows locks loaded modules


// Fired once per run, when the compiled snippet is about to execute. Null
// in the CLI binary and before the UI wires it.
static ec::RunPhaseFn g_run_phase = nullptr;

namespace ec {


void set_run_phase(RunPhaseFn fn) { g_run_phase = fn; }


const State& state() { return g_st; }

// ---- the State's STRINGS: immutable snapshots, not members ------------------
//
// THIS IS A HEAP-CORRUPTION FIX, not tidying. `state()` hands out a reference to
// one global, and the four strings used to be members of it. So a writer on the
// attach path (`detect_client_version` assigning a 26-character version string,
// which is past the small-string buffer and therefore heap-allocated) freed a
// block while a reader on ANOTHER thread was walking it. push_state's reader is
// the clearest one: `place_title_remember` calls it from the PLACENAME FETCH
// THREAD -- an HTTPS callback that lands whenever the network says so -- holding
// no attach lock at all. Free on one thread, read on another, microseconds
// apart. That is corruption, not a stale value, and it is invisible in the
// crash record because heap corruption reports through a fast-fail that
// deliberately bypasses our unhandled filter (see crash.cpp).
//
// The rule was already written down for the identity strings next door in
// ui/client/attach.cpp -- "Two writers on a std::string or std::map is heap
// corruption, not a stale read" -- and guarded there with g_ident_mux. These
// never got the same treatment.
//
// SAME SHAPE AS offsets.cpp'S TABLE SWAP, and chosen for the same reason: the
// readers are on threads we do not control and cannot be made to take a lock we
// would then have to hold across a WebView2 round trip. Publish a new immutable
// string, swap the pointer, and NEVER FREE THE OLD ONE -- a reader may still be
// inside it, and the whole point is that there is no moment we can prove it is
// not. The leak is bounded by how often these change: a handful of attaches per
// session, a few dozen bytes each.
//
// The SCALARS stay plain members. A torn read of a pointer-sized field yields a
// wrong number, which the plausibility checks already reject; it cannot free
// anything.
static const std::string kNoStr;
static std::atomic<const std::string*> g_s_dm_class{&kNoStr};
static std::atomic<const std::string*> g_s_conf_path{&kNoStr};
static std::atomic<const std::string*> g_s_conf_version{&kNoStr};
static std::atomic<const std::string*> g_s_client_version{&kNoStr};

// Compared before publishing: these are re-derived on every attach and mostly
// do not change, and an unconditional publish would retire a block each time
// for no reason.
static void publish(std::atomic<const std::string*>& slot, const std::string& v) {
    const std::string* cur = slot.load(std::memory_order_acquire);
    if (cur && *cur == v) return;
    slot.store(new std::string(v), std::memory_order_release);
}

// Safe to return by reference BECAUSE the referent is immutable and immortal.
const std::string& dm_class()       { return *g_s_dm_class.load(std::memory_order_acquire); }
const std::string& conf_path()      { return *g_s_conf_path.load(std::memory_order_acquire); }
const std::string& conf_version()   { return *g_s_conf_version.load(std::memory_order_acquire); }
const std::string& client_version() { return *g_s_client_version.load(std::memory_order_acquire); }

void set_dm_class(const std::string& v)       { publish(g_s_dm_class, v); }
void set_conf_path(const std::string& v)      { publish(g_s_conf_path, v); }
void set_client_version(const std::string& v) { publish(g_s_client_version, v); }
void set_conf_version(const std::string& v)   { publish(g_s_conf_version, v); }

}  // namespace ec

using ec::logf;

// ----------------------------- exported primitives --------------------------
// These are what a snippet binds to. Same names and semantics as the Linux
// build; only the syscalls underneath changed.

extern "C" {



}  // extern "C"

// The write path -- fx_wrblk, its arm gate and the dry-run recorder -- lives
// in write.cpp now. It is the safety-critical part of this program and it is
// worth being able to read all of it at once; it was previously spread through
// the middle of this file. write.h says why, and states the invariant.


// Appended to every snippet's g++ line. A project's own .project flags are
// added AFTER these, so a project can override anything set globally.
static std::string g_extra_flags;
// Echo a successful compile's warnings. On by default: -Wformat is on the
// g++ line for a reason, and a warning nobody prints is a warning nobody has.
static bool g_show_warnings = true;

extern "C" {








// ---- input gating -----------------------------------------------------------
// The two questions every hotkey-driven snippet has to ask and, until now,
// could not. Without them a bound key fires while you are alt-tabbed into
// something else, and pressing 'e' to type "hey" in chat triggers whatever 'e'
// does. Both failures look like the snippet is broken.




FX_API uint64_t fx_dm(void)   { return g_st.dm; }
FX_API int      fx_pid(void)  { return (int)g_st.pid; }
FX_API uint64_t fx_base(void) { return g_st.base; }

// The PlaceId, or 0 when there is no place -- which is the ONLY reliable way to
// ask "am I actually in a game".
//
// fx_dm() does NOT answer that, and assuming it does is the trap this exists to
// close: the client holds a DataModel at the menu as well, and it is REPLACED on
// join and on leave. So a non-zero dm means "there is a DataModel", not "a place
// is loaded" -- user-confirmed, watching the console stay dm-verified while
// sitting in the menu.
//
// A VALUE, not a boolean. "In game" means different things to different callers
// -- place loaded, character spawned, world streamed in -- and baking one of
// them into an fx_ingame() would have every snippet that meant a different one
// checking the wrong thing while looking right. The id also lets a snippet
// behave per place, which is what the sibling aimbot does with its per-game rig
// table.
//
// Read as 64 bits deliberately. PlaceId is an int64, and reading a field at the
// wrong width is a documented trap here -- Misc.Value read as a float instead of
// a double returned 0.0 and looked exactly like a dead player.
FX_API uint64_t fx_place(void) {
    // RESOLVED LIVE, not read through the cached g_st.dm. That cache is
    // refreshed by the watcher once a SECOND, and leaving a place swaps
    // straight to the next DataModel rather than passing through zero -- so
    // for up to a second after you leave, g_st.dm points at a freed object
    // whose PlaceId bytes still read as the place you just left. Everything
    // gated on this then believed you were still in a game: the panel kept
    // showing a health bar on the home screen, read out of the same freed
    // character.
    //
    // Three reads instead of one, about 2us: the module slot, the real
    // DataModel behind it, then PlaceId. Deliberately WITHOUT resolve_dm's
    // class verification, which is a string read and the expensive half --
    // this is "is there a place", not "is this pointer trustworthy", and a
    // stale answer would defeat the whole point of the function.
    uint64_t po = off_of("FakeDataModel.Pointer");
    uint64_t ro = off_of("FakeDataModel.RealDataModel");
    uint64_t pio = off_of("DataModel.PlaceId");
    if (po == BAD || ro == BAD || pio == BAD || !g_st.base) return 0;

    uint64_t fake = rd64(g_st.base + po);
    if (!fake) return 0;
    uint64_t dm = rd64(fake + ro);
    if (!dm) return 0;
    return rd64(dm + pio);
}




// Is the write gate open?
//
// fx_wrblk already honours it, so a snippet that only writes memory never has
// to ask. This is for the ones whose effect is NOT a memory write -- moving the
// mouse, say -- where nothing else would hold them to the gate. Without it,
// running such a snippet disarmed would do its whole job while the UI said
// writes were being refused, which is the one thing the gate exists to prevent
// anyone believing.
//
// Read-only, and deliberately so: there is no fx_arm(). Arming stays something
// the person at the keyboard does.
FX_API int fx_armed(void) { return g_st.armed ? 1 : 0; }




}  // extern "C"

// ----------------------------- process discovery ----------------------------

static const wchar_t* kClientExe = L"RobloxPlayerBeta.exe";

static uint32_t find_pid() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    uint32_t pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, kClientExe) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

// The Linux build never needed this: under flux the binary sat at a fixed VA.
// Here every static offset in the conf is an RVA and must be based.
// Which client version we are actually attached to, from its own exe path:
//
//   %LOCALAPPDATA%\Roblox\Versions\version-<hash>\RobloxPlayerBeta.exe
//
// The parent directory IS the version. Deliberately strict about the shape --
// a Microsoft Store install lives under WindowsApps with no version directory
// at all, and half-matching some other path would produce a version string
// that is confidently wrong. Better to report nothing and skip the check.
//
// QueryFullProcessImageNameW needs PROCESS_QUERY_INFORMATION, which the handle
// is guaranteed to carry: both acquisition paths request it explicitly, and
// NtDuplicateObject fails outright when the source handle lacks it -- so if we
// hold a handle at all, this call can succeed.
static void detect_client_version(bool quiet) {
    ec::set_client_version("");
    g_st.offsets_stale = false;
    if (!g_proc) return;

    wchar_t buf[MAX_PATH];
    DWORD n = MAX_PATH;
    if (!QueryFullProcessImageNameW(g_proc, 0, buf, &n)) return;

    std::wstring path(buf, n);
    size_t last = path.find_last_of(L'\\');
    if (last == std::wstring::npos || last == 0) return;
    size_t prev = path.find_last_of(L'\\', last - 1);
    if (prev == std::wstring::npos) return;

    std::wstring dir = path.substr(prev + 1, last - prev - 1);
    if (dir.compare(0, 8, L"version-") != 0) return;      // not the usual layout
    // Everything after "version-" must be hex, or it is not one of these.
    for (size_t i = 8; i < dir.size(); i++)
        if (!iswxdigit(dir[i])) return;

    ec::set_client_version(std::string(dir.begin(), dir.end()));

    // Only meaningful when the conf says what it is FOR. A conf with no
    // version note cannot be judged, so it is not condemned either.
    if (!ec::conf_version().empty() && ec::conf_version() != ec::client_version()) {
        g_st.offsets_stale = true;
        // One line. It was seven, which buried the two version strings that
        // are the entire content -- and a fetch attempt follows immediately
        // and explains itself, so the reasoning does not need repeating here.
        //
        // NOT gated on `quiet`. The quiet path is the poll-driven attach the
        // watcher does when the client starts after the console, which is the
        // ordinary way of attaching rather than an edge case. Suppressing it
        // there left Run disabled with nothing on screen saying why.
        logf("! Mismatch Roblox version\n");
    }
}

static uint64_t module_base(uint32_t pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W me{};
    me.dwSize = sizeof me;
    uint64_t base = 0;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, kClientExe) == 0) {
                base = (uint64_t)me.modBaseAddr;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return base;
}

// ----------------------------- DataModel ------------------------------------
//
// Windows replaces the whole Linux resolution path. There is no
// /tmp/flux_game_dm cache and no fixed-pointer slot; instead the offsets give a
// module-relative chain:  base + FakeDataModel.Pointer -> fake -> +RealDataModel.
// `quiet` suppresses the diagnostics: the retry path below runs once a second
// while the client sits outside a place, and would otherwise fill the console
// with the same two warnings forever.
static uint64_t resolve_dm(bool quiet = false) {
    g_st.dm_verified = false;
    ec::set_dm_class("");

    uint64_t po = off_of("FakeDataModel.Pointer");
    uint64_t ro = off_of("FakeDataModel.RealDataModel");
    if (po == BAD || ro == BAD) {
        if (!quiet) logf("  ! conf is missing FakeDataModel.Pointer / .RealDataModel\n");
        return 0;
    }
    if (!g_st.base) return 0;

    uint64_t fake = rd64(g_st.base + po);
    if (!fake) {
        if (!quiet)
            logf("  ! FakeDataModel slot read 0 at base+0x%llx (client still loading?)\n",
                 (unsigned long long)po);
        return 0;
    }
    uint64_t dm = rd64(fake + ro);
    if (!dm) {
        if (!quiet) logf("  ! FakeDataModel+0x%llx read 0\n", (unsigned long long)ro);
        return 0;
    }

    ec::set_dm_class(class_of(dm));
    g_st.dm_verified = (ec::dm_class() == "DataModel");
    if (!g_st.dm_verified && !quiet) {
        // Surfaced rather than silently accepted: a wrong class here almost
        // always means the offsets are for a different client build.
        logf("  ! DM candidate 0x%llx classed as '%s', expected 'DataModel'\n",
             (unsigned long long)dm, ec::dm_class().c_str());
    }
    return dm;
}

namespace ec {

uint32_t find_client() { return find_pid(); }

bool alive() {
    if (!g_proc) return false;
    DWORD code = 0;
    // GetExitCodeProcess works with the PROCESS_QUERY_INFORMATION we already
    // hold; waiting on the handle would have needed SYNCHRONIZE as well.
    if (!GetExitCodeProcess(g_proc, &code)) return false;
    return code == STILL_ACTIVE;
}

bool refresh_dm() {
    if (!g_proc) return false;
    uint64_t before = g_st.dm;
    g_st.dm = resolve_dm(true);
    if (g_st.dm != before) {
        // Leaving a place destroys the DataModel and builds another. Everything
        // reachable from the old one -- services, the character, every cached
        // Instance -- is freed the moment this changes.
        dm_gen_bump();
        char_cache_reset();

        // DELIBERATE, not a side effect. Snippets are told fx_gen() alone is
        // enough to invalidate everything, and that promise has to be kept here
        // rather than inherited from char_cache_reset happening to bump it.
        // Someone deciding a DataModel swap should not clear the character cache
        // would otherwise break every snippet at once, silently, with nothing
        // failing to compile. The double increment is harmless: the contract is
        // "this number differs", never "it differs by one".
        char_cache_bump_gen();
    }
    return g_st.dm != before;
}

}  // namespace ec

// ----------------------------- self-locate ----------------------------------

namespace ec {

std::string self_dir() {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (!n) return ".";
    std::string p(buf, n);
    size_t slash = p.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}

}  // namespace ec

static std::string temp_dir() {
    char buf[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, buf);
    if (!n) return ".";
    std::string p(buf, n);
    while (!p.empty() && (p.back() == '\\' || p.back() == '/')) p.pop_back();
    return p;
}

// ----------------------------- attach ---------------------------------------

namespace ec {

// Whether the arm state survives a detach. FALSE by default -- the UI phrases
// this as "Disarm on detach", on, which is the same thing said the safe way
// round. Detaching and attaching again is how you move to a DIFFERENT client,
// and coming back already armed means the next session's first write happens
// without anyone deciding it should.
//
// This changes only the LIFECYCLE of the flag. Disarmed still refuses every
// write exactly as before; nothing here touches the gate itself. Note there is
// still no way to be armed at STARTUP, deliberately -- surviving a detach keeps
// you in the session you armed, surviving a launch would not.
static bool g_keep_armed = false;

void set_keep_armed(bool on) { g_keep_armed = on; }
bool keep_armed(void) { return g_keep_armed; }

void detach() {
    if (g_proc) { CloseHandle(g_proc); g_proc = nullptr; }
    // State{} resets every field, arm included -- so carry it across
    // explicitly when the setting says to, rather than by leaving a field out
    // of the reset where the next person to add one would not see it.
    bool was_armed = g_st.armed;
    g_st = State{};
    if (g_keep_armed) g_st.armed = was_armed;
    offset_clear();
    // Every cached instance pointer belonged to a process we no longer hold.
    char_cache_reset();
}

// Closes any run still collapsing, so the last dry-run line lands BEFORE the
// "ARMED" notice rather than after it -- those writes really did happen while
// it was still disarmed.
void set_armed(bool on) { dry_flush(); g_st.armed = on; }

void set_dry_window_ms(unsigned ms) { write_set_dry_window_ms(ms); }

void set_extra_flags(const std::string& flags) { g_extra_flags = flags; }

void set_show_warnings(bool on) { g_show_warnings = on; }

// Every RobloxPlayerBeta.exe currently running, with what identity we can
// read out of it. Multi-instance users pick from this list.
//
// Read strategy: for each pid we open a MINIMAL handle (VM_READ + QUERY only,
// no write bit -- less suspicious than a real attach). The existing rd/read
// helpers work off g_proc, so we swap it briefly per candidate and restore
// the caller's handle at the end. The g_attach_mux the UI holds around every
// attach call means no other thread is reading through g_proc at the same
// time.
void preload_offsets(const std::string& conf_path) {
    if (offset_size() != 0) return;   // already loaded by a prior attach or preload
    load_offsets(conf_path);
}

std::vector<Candidate> enumerate_candidates(bool explain) {
    std::vector<Candidate> out;
    // Which pids to inspect. Not find_pid() -- that's "first hit and stop".
    std::vector<uint32_t> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof pe;
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, kClientExe) == 0)
                    pids.push_back(pe.th32ProcessID);
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    if (pids.empty()) return out;

    // Stubs are needed for the reads. Building them if the caller has not
    // already attached anywhere is cheap and one-shot.
    init_stubs();

    HANDLE saved_proc = g_proc;
    std::vector<std::string> unnamed;   // held until we know it is worth saying

    for (uint32_t pid : pids) {
        Candidate c;
        c.pid = pid;

        // SAME ROUTE AS ATTACH: duplicate a handle out of RobloxCrashHandler
        // rather than opening the client ourselves.
        //
        // This used to be a plain OpenProcess, which quietly undid the point of
        // the whole design -- the console goes to that trouble precisely so it
        // never issues NtOpenProcess against the game, and the picker was doing
        // it against EVERY client it found, before the user had chosen one or
        // even seen the list.
        //
        // The OpenProcess fallback stays, because attach has the same one for
        // the same reason: with no crash handler running there is no handle to
        // borrow, and being detectable beats not working. See the BOUNDARY note
        // at the top of this file.
        //
        // Cost: one system-wide handle enumeration per candidate. Fine at the
        // one-to-three clients this ever sees, and it only runs when the picker
        // is being built.
        HANDLE h = handle_from_crash_handler(pid);
        bool via_crash = (h != nullptr);
        if (!h) h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
                                FALSE, pid);

        // WHY a candidate came back unnamed. The picker once showed two rows of
        // "(unknown)" with no place either, and there was no way to tell which
        // step had failed -- the guesses ranged from "still loading" to
        // "OpenProcess is blocked", and neither was ever confirmed. One string
        // costs nothing and turns the next occurrence into a fact.
        const char* why = "ok";
        if (!h)                 why = "no handle (crash handler and OpenProcess both failed)";
        else if (!read_stub_ok()) why = "syscall stubs not built";

        if (h && read_stub_ok()) {
            c.readable = true;
            g_proc = h;

            uint64_t base = module_base(pid);
            if (!base) why = "module base not found";
            if (base) {
                uint64_t po = off_of("FakeDataModel.Pointer");
                uint64_t ro = off_of("FakeDataModel.RealDataModel");
                uint64_t fake = (po != BAD)     ? rd64(base + po) : 0;
                uint64_t dm   = (fake && ro != BAD) ? rd64(fake + ro) : 0;
                // Distinguish "we have no offset for this" from "the offset is
                // right and the slot is genuinely empty". Conflating them is
                // what let a cleared offset table masquerade as a client that
                // had not finished loading.
                if (po == BAD || ro == BAD)
                                      why = "offsets not loaded (nothing to read through)";
                else if (!fake)       why = "FakeDataModel slot reads 0 (client still starting?)";
                else if (!dm)         why = "RealDataModel reads 0";
                else if (class_of(dm) != "DataModel") why = "DataModel slot is not a DataModel";
                if (dm && class_of(dm) == "DataModel") {
                    read_name(dm, c.place, 128);
                    // PlaceId. Non-zero when in a game -- the DataModel's
                    // Instance.Name only ever reports the class ("LuaApp",
                    // "Ugc"), so this is the actual per-place identifier.
                    uint64_t pio = off_of("DataModel.PlaceId");
                    if (pio != BAD) rd_raw(dm + pio, &c.place_id, 8);

                    // Walk DataModel's children looking for the Players
                    // service. Same layout fx_nchildren uses:
                    // Instance.ChildrenStart -> container, then [begin, end).
                    uint64_t cso = off_of("Instance.ChildrenStart");
                    uint64_t ceo = off_of("Instance.ChildrenEnd");
                    uint64_t cptr = (cso != BAD) ? rd64(dm + cso) : 0;
                    uint64_t begin = cptr ? rd64(cptr) : 0;
                    uint64_t end   = (cptr && ceo != BAD) ? rd64(cptr + ceo) : 0;
                    uint64_t players = 0;
                    // Bounded for the same reason, and on the UI thread here:
                    // a garbage count would freeze the window itself, not just
                    // a snippet.
                    if (begin && end > begin && (end - begin) / 0x10 <= FX_MAX_CHILDREN) {
                        uint64_t n = (end - begin) / 0x10;
                        for (uint64_t i = 0; i < n && !players; i++) {
                            uint64_t child = rd64(begin + i * 0x10);
                            if (child && class_of(child) == "Players")
                                players = child;
                        }
                    }
                    if (!players) why = "no Players service among the DataModel's children";
                    if (players) {
                        uint64_t lpo = off_of("Player.LocalPlayer");
                        uint64_t lp  = (lpo != BAD) ? rd64(players + lpo) : 0;
                        if (!lp) why = "LocalPlayer is null (not in a place yet)";
                        if (lp) {
                            read_name(lp, c.username, 64);
                            // Player.DisplayName is a direct field on Player,
                            // NOT inside the name container -- do not route it
                            // through read_name.
                            uint64_t dno = off_of("Player.DisplayName");
                            if (dno != BAD)
                                read_str_field(lp + dno, c.display_name, 64);
                            uint64_t uio = off_of("Player.UserId");
                            if (uio != BAD) {
                                uint64_t uid = 0;
                                rd_raw(lp + uio, &uid, 8);
                                c.user_id = uid;
                            }
                        }
                    }
                }
            }

            g_proc = saved_proc;
            CloseHandle(h);
        }

        // Held, not logged yet. The picker exists to tell two clients apart, so
        // on a single-instance attach nobody is ever shown this list and a
        // complaint that it could not name the one client is pure noise --
        // especially during launch, when "still starting" is the expected
        // answer for a second or two. Emitted after the loop, and only if there
        // was actually a choice to make.
        if (c.username[0] == '\0') {
            char line[192];
            snprintf(line, sizeof line, "  picker: pid %lu unnamed via %s - %s\n",
                     (unsigned long)pid,
                     via_crash ? "crash handler" : "OpenProcess", why);
            unnamed.push_back(line);
        }

        // Monitor index. Find the biggest visible window belonging to this
        // pid, then look up its display monitor.
        struct WinCtx { DWORD pid; HWND hwnd; int area; } wctx = { pid, nullptr, 0 };
        EnumWindows([](HWND w, LPARAM lp) -> BOOL {
            auto* ctx = (WinCtx*)lp;
            if (!IsWindowVisible(w)) return TRUE;
            DWORD wpid = 0;
            GetWindowThreadProcessId(w, &wpid);
            if (wpid != ctx->pid) return TRUE;
            RECT r;
            if (!GetWindowRect(w, &r)) return TRUE;
            int a = (r.right - r.left) * (r.bottom - r.top);
            if (a > ctx->area) { ctx->area = a; ctx->hwnd = w; }
            return TRUE;
        }, (LPARAM)&wctx);

        if (wctx.hwnd) {
            HMONITOR mon = MonitorFromWindow(wctx.hwnd, MONITOR_DEFAULTTONEAREST);
            struct MonCtx { HMONITOR wanted; int seen; int found; } mctx = { mon, 0, 0 };
            EnumDisplayMonitors(nullptr, nullptr,
                [](HMONITOR mm, HDC, LPRECT, LPARAM lp) -> BOOL {
                    auto* m = (MonCtx*)lp;
                    m->seen++;
                    if (mm == m->wanted) { m->found = m->seen; return FALSE; }
                    return TRUE;
                }, (LPARAM)&mctx);
            c.monitor = mctx.found;
        }

        out.push_back(std::move(c));
    }

    g_proc = saved_proc;   // belt-and-suspenders, always restored

    // Only when there was a choice AND the list is about to be shown. One
    // client means the picker never appears; a background refresh means nobody
    // is looking at it yet.
    if (explain && out.size() > 1)
        for (size_t i = 0; i < unnamed.size(); i++) logf("%s", unnamed[i].c_str());

    return out;
}

// THE WAIT STATE, AT FILE SCOPE SO EVERY EXIT FROM attach() CAN SEE IT.
//
// The "waiting for a crash-handler handle" line is deliberately NOT gated on
// `quiet` -- a wait that looks like a hang has to say so even on the poll-driven
// attach. But every line that ENDS that wait WAS gated, and the watchdog only
// ever attaches quietly. So a client that could never be attached produced that
// one line, forever, roughly every 22 seconds, with the explanation suppressed
// every time.
//
// Reported 2026-09-04: a crashed client killed by hand still appeared in the
// process list (the crash handler holds it open), so find_pid kept handing back
// a pid that OpenProcess then refused -- and the console said only that it was
// waiting, never that it had given up, or why, or that the client was gone.
//
// The rule this encodes: IF WE ANNOUNCED THE WAIT, WE OWE THE LOG ITS OUTCOME,
// quiet or not. An attach that never announced anything stays silent.
static uint64_t s_wait_since = 0;
static bool     s_wait_said  = false;

bool attach(const std::string& conf_path, bool quiet, uint32_t target_pid) {
    bool was_armed = g_st.armed;
    detach();
    g_st.armed = was_armed;

    ec::set_conf_path(conf_path);
    g_st.noffsets  = load_offsets(conf_path);
    if (!g_st.noffsets) {
        if (!quiet) logf("x no offsets loaded from %s\n", conf_path.c_str());
        return false;
    }

    // A caller that specified a pid gets exactly that one -- used by the
    // multi-instance picker. Zero means "any Roblox", the historical default.
    g_st.pid = target_pid ? target_pid : find_pid();
    if (!g_st.pid) {
        // Two different facts, and the wait state is the only thing that tells
        // them apart: a client that was never there, versus one that went away
        // while we were holding off on opening it directly.
        if (s_wait_said) logf("! the client exited while we waited for a crash-handler handle\n");
        else if (!quiet) logf("x RobloxPlayerBeta.exe is not running\n");
        s_wait_since = 0;
        s_wait_said  = false;
        return false;
    }

    // Build the indirect syscall stubs once; they stay valid across reattaches.
    // Fatal if it fails -- there is no RPM/WPM fallback, so without the stubs
    // there is no way to touch the process, and limping on the hooked Win32 APIs
    // is exactly what this avoids. It only fails if ntdll's prologue is hooked
    // past its own syscall instruction, which is not something to paper over.
    if (!init_stubs()) {
        if (!quiet)
            logf("x could not build syscall stubs - ntdll prologue looks hooked past the syscall instruction\n");
        g_st.pid = 0;
        return false;
    }

    // Prefer handle duplication from RobloxCrashHandler.exe: our process never
    // issues NtOpenProcess for the game PID, and the handle arrives with the
    // same access rights we would have requested anyway.
    // Falls back to direct OpenProcess if the crash handler is not running.
    // BOTH ROUTES ARE REPORTED, and neither is gated on `quiet`.
    //
    // The fallback used to log nothing, and the crash-handler line was
    // suppressed on a quiet attach -- which is what the watchdog's auto-attach
    // uses, i.e. the common case. The result was a log that could not answer
    // "did we just open the game process?", which is the one guarantee this
    // whole mechanism exists to keep. Attaching is rare enough that one line is
    // not noise.
    g_proc = handle_from_crash_handler(g_st.pid);

    // WAIT FOR THE CRASH HANDLER RATHER THAN OPENING THE GAME.
    //
    // Launching the client and attaching four seconds later always fell back,
    // because there is nothing to borrow a handle from that early. Declining to
    // attach for a few seconds costs nothing: the DataModel is not ready either
    // (~4s after launch, measured), and the watchdog retries every tick.
    //
    // WAITS FOR A USABLE HANDLE, not for the process to exist. The first
    // version waited only while no RobloxCrashHandler.exe was running at all,
    // which gave up after about three seconds: the process appears well before
    // it opens a handle to the client, so "it exists now" ended the wait while
    // there was still nothing to duplicate. Observed as a fallback three
    // seconds into a twenty second budget.
    //
    // Bounded, because a client with crash reporting disabled would otherwise
    // never be attachable at all.
    static const uint64_t CRASH_WAIT_MS = 20000;
    if (!g_proc) {
        uint64_t now = GetTickCount64();
        if (!s_wait_since) s_wait_since = now;
        // Only worth saying once the wait is long enough to look like a hang.
        else if (!s_wait_said && now - s_wait_since > 2000) {
            s_wait_said = true;
            logf("  waiting for a crash-handler handle so the client is not opened directly\n");
        }
        if (now - s_wait_since < CRASH_WAIT_MS) {
            g_st.pid = 0;                 // not attached; the watchdog retries
            return false;
        }
    }
    // Carried past the reset: the failure branches below are the OUTCOME of a
    // wait we announced, so they have to speak even on a quiet attach.
    const bool owed = s_wait_said;
    s_wait_since = 0;
    s_wait_said  = false;
    // NOTHING SAID ON SUCCESS. A wait that ends in an attach is already closed
    // off by the attach line itself, which reports the route -- so the ordinary
    // new-client sequence stays at two lines ("attaching...", the wait, then the
    // attach) and a good attach of a client that is already up stays at one.
    // Only the ways this can FAIL owe the log a line, because those are what
    // otherwise leave the wait hanging with no outcome at all.

    // Recorded rather than announced. The crash-handler route is the expected
    // outcome, so a line every attach saying so is noise; the attach line
    // reports the route instead, and the fallback still warns because that one
    // is the exception worth reading.
    g_st.via_crash_handler = (g_proc != nullptr);
    if (!g_proc) {
        // VM_OPERATION is needed alongside WRITE for pages that need protection
        // changed; QUERY_INFORMATION for GetExitCodeProcess and the module walk.
        g_proc = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION |
                                 PROCESS_QUERY_INFORMATION,
                             FALSE, g_st.pid);
        // A WARN, not an info line: this is the detectable path. Usually it
        // means the crash handler had not started yet -- launching the client
        // and attaching within a few seconds is the reliable way to hit it.
        if (g_proc)
            logf("! no crash-handler handle after %llus - opened the process directly (detectable)\n",
                 (unsigned long long)(CRASH_WAIT_MS / 1000));
    }
    if (!g_proc) {
        DWORD err = GetLastError();
        if (!quiet || owed) {
            // ERROR_INVALID_PARAMETER from OpenProcess means the pid no longer
            // names a process: it exited between find_pid() and here. That is
            // the ordinary shape of a crashed client being killed while we wait
            // -- not our failure, and not a permissions problem, so it must not
            // read as one or it sends you looking at elevation for nothing.
            if (err == ERROR_INVALID_PARAMETER)
                logf("! the client (pid %u) exited while we were attaching\n", g_st.pid);
            else
                logf("x OpenProcess(%u) failed (err %lu) - run as the same user, or elevate\n",
                     g_st.pid, err);
        }
        g_st.pid = 0;                    // roll back: not attached
        return false;
    }

    g_st.base = module_base(g_st.pid);
    if (!g_st.base) {
        // A client that has only just started exists as a process before its
        // module list is mapped. That is a retry, not a failure -- but the
        // state must be rolled back or a caller sees pid != 0 and believes it
        // is attached to something with a zero base.
        if (!quiet || owed) logf("x could not find the %ls module base yet\n", kClientExe);
        CloseHandle(g_proc);
        g_proc = nullptr;
        g_st.pid = 0;
        return false;
    }

    detect_client_version(quiet);

    g_st.dm = resolve_dm(quiet);
    return true;
}

}  // namespace ec

// ---- core-internal surface (for the split-out core TUs) --------------------
// Thin forwarders so input.cpp (and the TUs that follow) reach core-owned state
// through FUNCTIONS, never an extern global. Defined here at file scope, after
// every static they touch, so the untouched sections above -- raw memory,
// exported primitives, attach -- stay byte-for-byte unchanged. See
// core_internal.h.
bool        core_attached(void)          { return g_proc != nullptr; }
void*       core_proc(void)              { return (void*)g_proc; }
void        core_dry_flush(void)         { dry_flush(); }
std::string core_temp_dir(void)          { return temp_dir(); }
std::string core_extra_flags(void)       { return g_extra_flags; }
bool        core_show_warnings(void)     { return g_show_warnings; }
unsigned    core_next_snip_seq(void)     { return ++g_snip_seq; }
void        core_run_phase_begin(void)   {
    // On the run's own thread: mark THIS run executing, then tell the UI which
    // one, so a list of several building runs flips only the one that started.
    unsigned id = core_run_current_id();
    core_run_mark_running(id);
    if (g_run_phase) g_run_phase(id);
}
void core_set_conf_version(const std::string& v) { ec::set_conf_version(v); }
