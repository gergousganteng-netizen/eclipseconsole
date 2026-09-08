// snippet_build.cpp -- compile a snippet (or a project folder) to a DLL, load
// it into OUR process, run its entry point, unload. Split out of
// eclipse_console.cpp. No memory reads live here; the run-lifecycle state
// (g_stop, the stop event, the extra flags, the run-phase hook) stays owned by
// eclipse_console.cpp and is reached through core_internal.h. The public API
// (run_snippet/run_project/stop_snippet/set_cli_stop_key) is declared in
// eclipse_core.h, so this TU needs no header of its own.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "eclipse_core.h"
#include "core_internal.h"
#include "run.h"         // the run registry: create/stop/current-id
#include "input.h"

namespace ec {

// ----------------------------- compile + run a snippet ----------------------

// ---- bundled toolchain -----------------------------------------------------
// A shipped build carries its own g++ in <exe dir>\toolchain, so a snippet
// compiles on a machine with nothing installed. Empty when there is no bundle
// and we fall back to whatever g++ is on PATH -- which is the dev setup.
//
// The bundle WINS over an installed compiler, and not only for reproducibility:
// the snippet DLL must use the same C runtime as the host, because the UI
// captures a snippet's printf by rebinding fd 1. A UCRT host loading a snippet
// built by an MSVCRT g++ gets two independent stdio states and the output
// silently vanishes. Verified: this bundle and the host both import the
// api-ms-win-crt-* apiset.
static std::string toolchain_bin() {
    std::string p = self_dir() + "\\toolchain\\bin";
    if (GetFileAttributesA((p + "\\g++.exe").c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    return "";
}

// How to spell the compiler on the command line.
static std::string compiler_cmd() {
    std::string bin = toolchain_bin();
    return bin.empty() ? std::string("g++") : ("\"" + bin + "\\g++.exe\"");
}

// The child's environment with `prefix` prepended to PATH.
//
// Invoking g++ by absolute path is NOT enough on its own. GCC finds cc1plus
// relative to the driver, but it looks for `as` and `ld` on PATH -- so without
// this the compile gets through the front end and then dies with no useful
// message at all. Measured, not assumed.
//
// Empty return means "inherit", which is what the dev build wants.
static std::string child_env_with_path(const std::string& prefix) {
    std::string out;
    if (prefix.empty()) return out;

    LPCH env = GetEnvironmentStrings();
    if (!env) return out;

    bool replaced = false;
    for (LPCH p = env; *p; ) {
        size_t len = strlen(p);
        // Case-insensitive: the variable is conventionally PATH but Windows
        // does not guarantee the casing, and a missed match would silently
        // append a SECOND Path= that the child would ignore.
        if (!replaced && len > 5 && _strnicmp(p, "PATH=", 5) == 0) {
            out += "PATH=" + prefix + ";" + std::string(p + 5, len - 5);
            out.push_back('\0');
            replaced = true;
        } else {
            out.append(p, len);
            out.push_back('\0');
        }
        p += len + 1;
    }
    FreeEnvironmentStringsA(env);

    if (!replaced) { out += "PATH=" + prefix; out.push_back('\0'); }
    out.push_back('\0');           // block is double-null terminated
    return out;
}

// Run a command with no console window and capture whatever it writes.
//
// `why` separates the ways this can fail, because the caller cannot tell them
// apart from an empty capture and guessed wrong for a long time -- an unopenable
// output file was reported to the user as a damaged toolchain.
enum class CmdFail { None, OutFile, Spawn, ExitCode };

// Echo g++ verbatim, indented. The Linux build shelled out to sed for this;
// there is no sed here and no reason to need one. Used by BOTH outcomes --
// a failed compile's errors and a successful one's warnings.
static void echo_compiler(const std::string& out) {
    size_t start = 0;
    while (start < out.size()) {
        size_t nl = out.find('\n', start);
        if (nl == std::string::npos) nl = out.size();
        std::string line = out.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) logf("    %s\n", line.c_str());
        start = nl + 1;
    }
}

static bool run_cmd(const std::string& cmd, std::string& captured,
                    CmdFail* why = nullptr) {
    if (why) *why = CmdFail::None;

    // Per-process, and that is not tidiness. This was a fixed path opened
    // CREATE_ALWAYS with only FILE_SHARE_READ, so two Eclipse processes
    // compiling at the same moment -- the GUI console and a CLI run, which is
    // an ordinary thing to do -- collided on it. The loser got a sharing
    // violation, returned with nothing captured, and was reported as
    // "bundled toolchain looks damaged".
    // Per-process AND per-call. Two Eclipse processes compiling at once was the
    // original collision (sharing violation, reported as a damaged toolchain);
    // multi-run adds two CONCURRENT compiles in ONE process, so the run sequence
    // number is on here too or the two clobber each other's captured output.
    char nm[80];
    snprintf(nm, sizeof nm, "\\eclipse_cmd_%u_%u.out",
             (unsigned)GetCurrentProcessId(), core_next_snip_seq());
    std::string outfile = core_temp_dir() + nm;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE h = CreateFileA(outfile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (why) *why = CmdFail::OutFile;
        return false;
    }

    STARTUPINFOA si{};
    si.cb = sizeof si;
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = h;
    si.hStdError  = h;
    si.hStdInput  = nullptr;

    PROCESS_INFORMATION pi{};
    std::string mutable_cmd = cmd;
    std::string envblock = child_env_with_path(toolchain_bin());
    BOOL ok = CreateProcessA(nullptr, &mutable_cmd[0], nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW,
                             envblock.empty() ? nullptr : (LPVOID)&envblock[0],
                             nullptr, &si, &pi);
    DWORD rc = 1;
    if (ok) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &rc);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    CloseHandle(h);

    std::ifstream f(outfile, std::ios::binary);
    captured.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (why && !ok)            *why = CmdFail::Spawn;
    else if (why && rc != 0)   *why = CmdFail::ExitCode;
    return ok && rc == 0;
}

// CLI only. Starts the keyboard hook and has it eat Ctrl+Shift+Q, so pressing
// the stop key does not also leave "^Q" echoed in the shell that launched us.
// Pointless in the UI, where the Stop button is a button.
void set_cli_stop_key(bool on) {
    kb_set_swallow_stopkey(on);
    if (on) kb_start();
    else    kb_stop();
}

// Stop EVERY run. The fx_sleep wake rides on each run's own stop event (set by
// core_run_stop_all); this adds the fx_wait_key wake -- without it a snippet
// parked there would sit until someone happened to press a key, and Stop would
// appear to hang. Wired to detach, client swap, offsets going stale and
// teardown, and is what the CLI and the app's teardown call.
void stop_all_runs() {
    core_run_stop_all();
    kb_wake_all();
}
void stop_snippet() { stop_all_runs(); }        // back-compat name

// Stop ONE run (the sidebar's per-snippet Stop). Same two wakes, aimed at a
// single run.
void stop_run(unsigned id) {
    core_run_stop(id);
    kb_wake_run(id);
}

// Blank out comments and string/char literals, keeping newlines so nothing
// shifts. Used only to decide whether a snippet defines its own entry point.
//
// RAW STRINGS are handled, and that is not a nicety. A snippet that hosts an
// HTML page keeps it in R"HTML(...)HTML", and that page is full of JavaScript
// using single quotes. Without this, every one of those apostrophes opened a
// char literal: with an odd number of them the scanner stayed "inside" one to
// the end of the file, blanked away the entry point that came after the page,
// and the app then wrapped the whole snippet in fx_run() -- producing a wall of
// "a function-definition is not allowed here" from a file that was correct.
// Diagnosed from exactly that, in a snippet that had compiled fine until one
// more apostrophe was added to its page.
static std::string strip_code(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    enum State { CODE, LINE, BLOCK, STR, CH, RAW } st = CODE;
    std::string raw_end;          // the ")delim\"" that closes the raw string

    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        char n = (i + 1 < s.size()) ? s[i + 1] : '\0';
        switch (st) {
            case CODE:
                // R"delim( ... )delim". The prefix may also be LR/uR/u8R/U R,
                // so the R is found by looking at what precedes the quote
                // rather than by matching the whole prefix.
                if (c == 'R' && n == '"') {
                    size_t open = s.find('(', i + 2);
                    if (open != std::string::npos && open - (i + 2) <= 16) {
                        raw_end = ")" + s.substr(i + 2, open - (i + 2)) + "\"";
                        st = RAW;
                        i = open;          // resume inside the body
                        break;
                    }
                }
                if      (c == '/' && n == '/') { st = LINE;  i++; }
                else if (c == '/' && n == '*') { st = BLOCK; i++; }
                else if (c == '"')             { st = STR;   }
                else if (c == '\'')            { st = CH;    }
                else out += c;
                break;
            case RAW:
                // Newlines kept so line numbers still line up for anything
                // that reads this output positionally.
                if (c == '\n') out += '\n';
                if (s.compare(i, raw_end.size(), raw_end) == 0) {
                    i += raw_end.size() - 1;
                    st = CODE;
                }
                break;
            case LINE:
                if (c == '\n') { st = CODE; out += '\n'; }
                break;
            case BLOCK:
                if      (c == '\n')            out += '\n';
                else if (c == '*' && n == '/') { st = CODE; i++; }
                break;
            case STR:
                if      (c == '\\') i++;
                else if (c == '"')  st = CODE;
                break;
            case CH:
                if      (c == '\\') i++;
                else if (c == '\'') st = CODE;
                break;
        }
    }
    return out;
}

static bool build_and_run(const std::vector<std::string>& sources,
                          const std::string& extra_flags);

// Refuse outright when the conf is for a different client. Not a warning worth
// overriding: with the wrong offsets every read is garbage, and an armed write
// goes to an address that was never right -- which corrupts the client rather
// than merely failing. Not using the app beats using it wrong.
static bool offsets_usable() {
    if (!state().offsets_stale) return true;
    logf("x refusing to run - offsets are for %s, client is %s\n",
         conf_version().c_str(), client_version().c_str());
    logf("  Update conf/offsets_windows.conf for this client version.\n");
    return false;
}

bool run_snippet(const std::string& body) {
    if (!core_attached()) { logf("x not attached\n"); return false; }
    if (!offsets_usable()) return false;

    // Same wrapping rule as the Linux original -- a full snippet defines its own
    // entry point, a bare body gets wrapped -- but scanning the raw text finds
    // "fx_run" inside comments and strings too. A snippet whose comment merely
    // MENTIONS fx_run was then left unwrapped, and its statements ended up at
    // file scope. Scan a copy with comments and literals blanked out.
    // The trailing "(" matters: "fx_run" alone is a substring of fx_running(),
    // so any loop polling fx_running() was mistaken for a snippet that defines
    // its own entry point.
    std::string scan = strip_code(body);
    bool full = scan.find("fx_run(")  != std::string::npos ||
                scan.find("int run(") != std::string::npos ||
                scan.find(" run(")    != std::string::npos;

    std::string src;
    if (full) {
        src = body;
    } else {
        // Lift #include lines out of the body before wrapping it.
        //
        // A bare body ends up inside fx_run(), and an include cannot go in a
        // function -- so `#include <cmath>` failed with a wall of errors from
        // inside math.h that never mentioned the real cause.
        //
        // The hoisted line is replaced by an EMPTY one rather than removed, so
        // the body keeps its own line numbering; #line 1 below then makes g++
        // report the line you actually typed instead of one offset by however
        // much the wrapper added.
        std::string includes, rest;
        size_t p = 0;
        while (p <= body.size()) {
            size_t nl = body.find('\n', p);
            std::string line = body.substr(p, nl == std::string::npos ? std::string::npos
                                                                      : nl - p);
            size_t a = line.find_first_not_of(" \t");
            bool is_include = a != std::string::npos &&
                              line.compare(a, 8, "#include") == 0;
            if (is_include) { includes += line + "\n"; rest += "\n"; }
            else            { rest     += line + "\n"; }
            if (nl == std::string::npos) break;
            p = nl + 1;
        }
        src = includes + "extern \"C\" void fx_run(){\n#line 1\n" + rest + "\n}\n";
    }

    // Per-process, for the same reason as the compiler's output file -- and
    // this one is worse. Two Eclipse processes compiling different snippets
    // through one fixed path can overwrite each other's SOURCE, so the one that
    // loses the race compiles and runs the other's code with no error anywhere.
    // Per-process AND per-call, for the same reason as the compiler output file
    // above: two concurrent run_snippet calls in one process must not share the
    // source path or the loser compiles and runs the other's code.
    char snipnm[80];
    snprintf(snipnm, sizeof snipnm, "\\eclipse_snip_%u_%u.cpp",
             (unsigned)GetCurrentProcessId(), core_next_snip_seq());
    std::string tmp  = core_temp_dir();
    std::string cpp  = tmp + snipnm;
    { std::ofstream o(cpp, std::ios::binary); o << src; }

    bool ok = build_and_run({ cpp }, "");
    DeleteFileA(cpp.c_str());   // per-call name, so it accumulates otherwise
    return ok;
}

// Compile `sources` into a DLL, load it into THIS process, call its entry
// point, unload. Shared by the one-file and whole-folder paths -- they differ
// only in how many files go on the command line.
static bool build_and_run(const std::vector<std::string>& sources,
                          const std::string& extra_flags) {
    std::string tmp = core_temp_dir();
    char dllname[64];
    snprintf(dllname, sizeof dllname, "\\eclipse_snip_%u_%u.dll",
             (unsigned)GetCurrentProcessId(), core_next_snip_seq());
    std::string dll = tmp + dllname;

    // -static-libgcc/-static-libstdc++ mirrors how the host is linked, so a
    // snippet loads without libstdc++-6.dll on PATH. Each module gets its own
    // libstdc++, which is safe because the fx_* boundary is pure C -- no C++
    // objects cross it. The UCRT stays shared, which is what lets the UI capture
    // a snippet's printf by rebinding fd 1.
    //
    // -include fx.h so every file gets the API whether or not it says so. A
    // single snippet never writes the include (the wrapper prepends it to the
    // temp file), but a project is compiled where it sits, so without this
    // pasting a working snippet into a project file fails for a reason nothing
    // explains. Doing it on the command line rather than by rewriting the source
    // keeps g++'s error line numbers pointing at the line you wrote.
    //
    // fx.h is #pragma once, so a file that includes it explicitly is unaffected
    // -- and writing it out is still worth doing for anything you intend to keep.
    std::string dir = self_dir();
    std::string cmd =
        // -Wformat: fx_logf carries a printf format attribute, and a wrong
        // conversion there is a crash at runtime rather than a wrong number.
        // Just that one warning, not -Wall -- a snippet is someone thinking
        // out loud, and burying a real diagnostic under unused-variable noise
        // is how warnings get ignored.
        compiler_cmd() + " -shared -O2 -std=c++17 -Wformat -static-libgcc -static-libstdc++ "
        "-I\"" + dir + "\" -include \"" + dir + "\\fx.h\" ";
    for (const std::string& s : sources) cmd += "\"" + s + "\" ";
    // Global first, project second. Later flags win on the g++ line, so a
    // project that sets -O0 still beats a global -O3 rather than fighting it.
    if (!core_extra_flags().empty()) cmd += core_extra_flags() + " ";
    if (!extra_flags.empty())   cmd += extra_flags + " ";
    // Link against THIS module's import library, not a hard-coded name.
    //
    // A snippet is a DLL that binds fx_* back into whichever exe loaded it, and
    // there are two of those now: eclipse_console.exe (the UI) and
    // eclipse_cli.exe (the terminal one). Each emits its own implib, so the
    // right -l is whatever we are called. Hard-coding it meant a snippet run
    // from the CLI would import from the GUI binary -- which is not the process
    // it is loaded into.
    char selfpath[MAX_PATH] = "";
    GetModuleFileNameA(nullptr, selfpath, MAX_PATH);
    std::string me = selfpath;
    size_t slash = me.find_last_of("\\/");
    if (slash != std::string::npos) me = me.substr(slash + 1);
    size_t dot = me.rfind('.');
    if (dot != std::string::npos) me = me.substr(0, dot);
    if (me.empty()) me = "eclipse_console";

    cmd += "-o \"" + dll + "\" -L\"" + dir + "\" -l" + me;

    std::string err;
    CmdFail why = CmdFail::None;
    if (!run_cmd(cmd, err, &why)) {
        // Could not even create the file the compiler's output goes to. This
        // used to fall into the branch below and be reported as a damaged
        // toolchain, which is a lie that costs an hour: the compiler is fine
        // and was never asked to run. The usual cause is a second Eclipse
        // process compiling at the same instant.
        if (why == CmdFail::OutFile) {
            logf("x could not open the compiler's output file\n");
            logf("    in %s - locked. Another Eclipse process compiling?\n",
                 core_temp_dir().c_str());
            return false;
        }
        // Nothing captured means CreateProcess never started a compiler, so
        // there is no diagnostic to echo. Left alone this prints "compile
        // failed:" followed by nothing -- and it is the failure a fresh install
        // is most likely to hit, reported as the one that explains itself
        // least. Name the compiler that was attempted instead.
        if (err.empty()) {
            std::string bin = toolchain_bin();
            logf("x could not start the compiler\n");
            if (bin.empty())
                logf("    No toolchain at %s\\toolchain\\bin and no g++ on PATH.\n",
                     self_dir().c_str());
            else
                logf("    %s\\g++.exe would not start (err %lu).\n",
                     bin.c_str(), GetLastError());
            return false;
        }
        logf("x compile failed:\n");
        echo_compiler(err);
        return false;
    }

    // A COMPILE THAT SUCCEEDS CAN STILL HAVE SOMETHING TO SAY, and this is the
    // only place it gets said. run_cmd fills `captured` whatever the exit code,
    // but only the failure branch above ever read it -- so every warning from a
    // build that worked was generated and thrown away.
    //
    // That is not a hypothetical: -Wformat is on the command line for exactly
    // one purpose (see the note there), and a snippet that passed a uint64_t to
    // "%.1f" built clean, printed 0.0 and gave no clue why. g++ had named the
    // line and the types; nobody was listening. WARN, not error: the DLL is
    // built and about to run, and the snippet may well be fine.
    if (core_show_warnings() &&
        err.find_first_not_of(" \t\r\n") != std::string::npos) {
        logf("! compiled with warnings:\n");
        echo_compiler(err);
    }

    HMODULE h = LoadLibraryA(dll.c_str());
    if (!h) {
        logf("x LoadLibrary failed (err %lu)\n", GetLastError());
        return false;
    }

    unsigned rid = core_run_current_id();   // this thread's run (0 on the CLI)
    core_clear_stop();              // fresh run; Stop applies to this one only
    // Manual-reset, so it must be cleared here or every fx_sleep in the new
    // run would return instantly on the PREVIOUS run's Stop. Per run this is
    // the run's own event (born reset); on the CLI it is the stop-all latch.
    core_reset_stop_event();
    kb_run_begin(rid);           // acquire this run's wake-sem slot; a key
                                 // pressed before it started is not one it sees
    // Built, loaded, about to run: the UI stops saying "building" here.
    core_run_phase_begin();
    bool ran = true;
    if (auto fn = (void(*)())GetProcAddress(h, "fx_run")) {
        fn();
    } else if (auto rn = (int(*)())GetProcAddress(h, "run")) {
        int rc = rn();
        logf("  run() returned %d\n", rc);
    } else {
        // For a project this is the likely mistake: several files, none of them
        // defining the entry point. A folder is not wrapped the way a bare
        // single snippet is, because there would be no way to say which file the
        // wrapper belongs in.
        logf("x no fx_run()/run() entrypoint - one file must define one\n");
        ran = false;
    }

    // The last collapsed run has nothing after it to close it, so without this
    // a snippet whose final act was a burst of writes would lose the count.
    core_dry_flush();

    // Give this run's keys back before anything else, release its wake-sem slot
    // and tear the hook down IF this was the last run. If the snippet claimed F6
    // and we returned without this, F6 would stay swallowed system-wide for the
    // rest of the session -- a broken keyboard with no visible cause. Only THIS
    // run's claims are dropped; a co-running snippet keeps its own.
    kb_run_end(rid);
    // Release this run's hold on the snippet window / overlay (and close a
    // window it left open), so a first-wins surface is not stranded.
    window_run_end(rid);
    // Settings writes are rate-limited, so the run's last set is usually still
    // only in memory here. Flush before the run is forgotten.
    config_flush();

    FreeLibrary(h);
    DeleteFileA(dll.c_str());     // unique per run, so this always succeeds
    return ran;
}

// Run a whole folder: every .cpp in it compiled and linked together, so the
// files can call into each other and share headers.
//
// Nothing is wrapped here, unlike a single snippet. A bare body only works when
// there is exactly one file to put the wrapper around; with several there is no
// way to say which, so a project must define fx_run() itself.
bool run_project(const std::string& dir) {
    if (!core_attached()) { logf("x not attached\n"); return false; }
    if (!offsets_usable()) return false;

    // ANSI, matching the rest of this file (LoadLibraryA, DeleteFileA); the
    // wide/narrow helpers live in eclipse_ui.cpp.
    std::vector<std::string> sources;
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((dir + "\\*.cpp").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            sources.push_back(dir + "\\" + fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (sources.empty()) { logf("x project has no .cpp files\n"); return false; }

    // Optional extra flags from the marker file. It may be empty -- its job is
    // to say "this folder is a project", and being able to carry settings is a
    // bonus rather than the reason it exists.
    std::string flags;
    {
        std::ifstream f(dir + "\\.project");
        std::string line;
        while (std::getline(f, line)) {
            size_t hash = line.find('#');
            if (hash != std::string::npos) line = line.substr(0, hash);
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            auto trim = [](std::string& s) {
                size_t a = s.find_first_not_of(" \t");
                size_t b = s.find_last_not_of(" \t\r\n");
                if (a == std::string::npos) { s.clear(); return; }
                s = s.substr(a, b - a + 1);
            };
            trim(k); trim(v);
            if (k == "flags") flags = v;
        }
    }

    logf("  building %zu file%s\n", sources.size(), sources.size() == 1 ? "" : "s");
    return build_and_run(sources, flags);
}

}  // namespace ec
