// eclipse_ui.cpp — WebView2 front end over the Eclipse UI kit, plus the CLI.
//
// Two entry paths share one core:
//   --run FILE   headless, prints to the parent console and/or --log
//   (no args)    the window: web/ rendered in WebView2, wired to ec::
//
// The UI kit was drawn for a Luau executor. This backend is not one and must
// not pretend to be: nothing here executes script inside the game. The kit's
// components are mapped onto what the console genuinely does — the editor holds
// C++, the palette searches the offset table, the "arm" control is the write
// gate, and the status cluster shows the real pid/base/DataModel.

#define WIN32_LEAN_AND_MEAN
// Same API-level bump as overlay.cpp (the comment there says why: the
// overlay's composition hosting needs Windows 8+ declarations and MinGW
// still defaults to 0x0601). Kept here too so the two halves of the UI
// compile against one API surface.
#undef  _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#undef  WINVER
#define WINVER       0x0A00
#include <windows.h>
#include <windowsx.h>      // GET_X_LPARAM
#include <shellapi.h>
#include <dwmapi.h>
#include <dcomp.h>         // the in-game panel's composition visual tree
#include <mmsystem.h>      // timeBeginPeriod: the animation's frame clock
#include <winhttp.h>
#include <commdlg.h>       // bundle export/import file dialogs

// MinGW's shellapi.h predates this one; it's been in the API since Vista.
#ifndef ABM_GETAUTOHIDEBAREX
#  define ABM_GETAUTOHIDEBAREX 0x0000000b
#endif

#include <algorithm>
#include <atomic>
#include <deque>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "webview/webview.h"
#include "eclipse_core.h"
#include "run.h"           // the multi-run registry: create / list / cap

// The sections split out of this file. host.h is the reverse direction:
// the functions here that those TUs call back into.
#include "host.h"
#include "attach.h"
#include "dpi.h"
#include "launch.h"
#include "log.h"
#include "offsets_fetch.h"
#include "overlay.h"
#include "placename.h"
#include "snipwin.h"
#include "watch.h"
#include "bundle.h"
#include "tree.h"
#include "trash.h"

// ----------------------------- shared helpers -------------------------------

std::string narrow(const wchar_t* w) {
    if (!w) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "";
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

// ----------------------------- CLI mode -------------------------------------
//
// Parity with the Linux original:
//     eclipse_console --run FILE [--arm] [--conf PATH] [--log PATH]
// This binary is GUI subsystem, so it borrows the parent console when launched
// from a shell; --log is the fully reliable capture.

static FILE* g_cli_log = nullptr;

static void cli_sink(const char* line) {
    const char* s = line ? line : "";
    if (g_cli_log) { fputs(s, g_cli_log); fflush(g_cli_log); }
    fputs(s, stdout);
    fflush(stdout);
}

static int run_cli(const std::string& runfile, const std::string& conf,
                   const std::string& logfile, bool arm) {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
    if (!logfile.empty()) g_cli_log = fopen(logfile.c_str(), "w");

    ec::set_log(cli_sink);
    ec::set_armed(arm);

    // Stopping a looping snippet from the command line.
    //
    // Ctrl+C cannot work here, structurally: this exe is GUI subsystem, so
    // the shell does not wait on it and consumes Ctrl+C itself at the idle
    // prompt -- a SetConsoleCtrlHandler here never sees the event. So the
    // stop key is polled: GetAsyncKeyState reads global key state regardless
    // of focus or console ownership. Ctrl+Shift+Q, not Ctrl+Break: VK_CANCEL
    // is not a physical key -- Windows synthesises it for Ctrl+Break -- so
    // polling it fired before the client was even attached. Don't retry.
    //
    // The loop does NOT return after firing: one spurious trigger would
    // disarm it for the rest of the run, and an early stop is erased anyway
    // -- build_and_run clears g_stop just before fx_run. Without any of this
    // a looping --run can only be killed, which skips the snippet's own
    // cleanup: claimed keys stay swallowed system-wide, a noclip stays
    // ghosted. No polling thread -- the hook both notices the key and eats
    // it; see kb_proc for why those two cannot split across threads. The
    // release lives in a destructor because run_cli returns from several
    // places, and each return is a chance to forget it.
    ec::set_cli_stop_key(true);
    struct Restore { ~Restore() { ec::set_cli_stop_key(false); } } restore;

    ec::logf("  (Ctrl+Shift+Q stops a running snippet)\n");

    // A directory means a project: every source in it, compiled together. Same
    // thing the ▶ on a group header does, so the CLI is not the poor relation --
    // which mattered the moment a project needed debugging, because --log is far
    // easier to read than the console.
    DWORD attr = GetFileAttributesW(widen(runfile).c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        int rc = 1;
        if (ec::attach(conf)) {
            const ec::State& s = ec::state();
            ec::logf("eclipse_console | pid %u | base 0x%llx | %zu offsets | dm %s | writes %s\n",
                     s.pid, (unsigned long long)s.base, s.noffsets,
                     s.dm ? (s.dm_verified ? "ok" : "UNVERIFIED") : "MISSING",
                     arm ? "ARMED" : "dry-run");
            rc = ec::run_project(runfile) ? 0 : 1;
        }
        ec::detach();
        if (g_cli_log) { fclose(g_cli_log); g_cli_log = nullptr; }
        return rc;
    }

    std::ifstream f(runfile, std::ios::binary);
    if (!f) { ec::logf("x cannot open %s\n", runfile.c_str()); return 2; }
    std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (body.find_first_not_of(" \t\r\n") == std::string::npos) {
        ec::logf("empty snippet\n");
        return 1;
    }

    int rc = 1;
    if (ec::attach(conf)) {
        const ec::State& s = ec::state();
        ec::logf("eclipse_console | pid %u | base 0x%llx | %zu offsets | dm %s | writes %s\n",
                 s.pid, (unsigned long long)s.base, s.noffsets,
                 s.dm ? (s.dm_verified ? "ok" : "UNVERIFIED") : "MISSING",
                 arm ? "ARMED" : "dry-run");
        rc = ec::run_snippet(body) ? 0 : 1;
    }
    ec::detach();
    if (g_cli_log) { fclose(g_cli_log); g_cli_log = nullptr; }
    return rc;
}

// ----------------------------- GUI mode -------------------------------------

static webview::webview* g_wv   = nullptr;
static HWND              g_hwnd = nullptr;
static std::string       g_conf;
static std::string       g_snipdir;

// The overlay machine's read side of the main window and page (overlay.cpp:
// app mode positions this window over the game, and the composed panel
// reuses the main page's WebView2 environment). Functions, not published
// globals -- the values stay owned here.
HWND ui_main_hwnd() { return g_hwnd; }
void* ui_browser_controller() {
    if (!g_wv) return nullptr;
    auto r = g_wv->browser_controller();
    return (r.ok() && r.value()) ? r.value() : nullptr;
}

// The engine's callback arrives on its worker thread; dispatch() hops to the UI
// thread, which WebView2 requires.
static void ui_eval_msg(const std::string& msg) {    // UI thread only
    if (!g_wv) return;
    g_wv->eval("window.__onHostMsg&&window.__onHostMsg(\"" + json_escape(msg) + "\")");
}

void ui_post(const std::string& msg) {
    if (!g_wv) return;
    g_wv->dispatch([msg]() { ui_eval_msg(msg); });
}


// The log funnel's host side (the queue, history and file live in
// log.cpp, which owns them outright). Three facts it cannot know itself:
// whether a page exists to queue for, the UI-thread hop that runs
// flush_logs -- WebView2 only accepts calls on the UI thread -- and delivery
// of one funnel message to BOTH views (the desktop page and the in-game
// panel), so the two can never show different histories.
// The conf path, for the attach layer (set once at startup, before the
// watcher or the page can ask).
std::string ui_conf() { return g_conf; }

bool ui_page_live() { return g_wv != nullptr; }
void ui_dispatch_log_flush() { if (g_wv) g_wv->dispatch(flush_logs); }
void ui_dispatch(std::function<void()> fn) { if (g_wv) g_wv->dispatch(std::move(fn)); }
void hud_post(const std::string& msg) { hud_push(msg); }
void ui_deliver_logmsg(const std::string& msg) {
    ui_eval_msg(msg);
    hud_push(msg);
}


// Map the console's own message types onto the kit's four log levels. STATUS is
// informational; anything the engine flags with a leading "x" or "!" is an
// error or warning respectively.
// SPLIT, because they are two different wishes. Wanting the console to come
// back the size you made it says nothing about wanting it in the same corner,
// and one switch meant turning off the half you disliked cost you the half you
// wanted. Both default on, which is what the single switch used to do.
// They govern ONE geometry: the app window is the same HWND on the desktop and
// in the game, so these are the only two switches over it and nothing outside
// this file needs to ask -- write_window_rect answers for both.
static bool g_remember_pos  = true;
static bool g_remember_size = true;


// The page keeps its own session state (focus, collapsed groups, open tabs,
// settings) in localStorage. Holding it here in a file was tried and reverted:
// localStorage is synchronous and readable before the first render, while a
// host-held copy only lands on READY, so every value needed a default, a
// second apply and a re-render once the real one arrived. Snippets are a
// different matter entirely and are real files -- see resolve_snipdir.


void push_state() {
    refresh_identity();
    const ec::State& s = ec::state();
    char base[32] = "";
    if (s.base) snprintf(base, sizeof base, "0x%llx", (unsigned long long)s.base);
    char dm[32] = "";
    if (s.dm) snprintf(dm, sizeof dm, "0x%llx", (unsigned long long)s.dm);

    std::ostringstream j;
    j << "{"
      << "\"pid\":"        << s.pid                              << ","
      << "\"base\":\""     << base                        << "\","
      << "\"dm\":\""       << dm                          << "\","
      << "\"dmVerified\":" << (s.dm_verified ? "true" : "false") << ","
      << "\"offsets\":"    << s.noffsets                         << ","
      << "\"version\":\""  << json_escape(ec::conf_version())      << "\","
      // What the attached client actually is, and whether that disagrees with
      // the conf. The page disables Run on `stale` -- the host refuses too,
      // but a button that looks live and then explains itself is worse than
      // one that says up front why it cannot be pressed.
      << "\"clientVersion\":\"" << json_escape(ec::client_version()) << "\","
      << "\"stale\":"      << (s.offsets_stale ? "true" : "false") << ","
      << "\"armed\":"      << (s.armed ? "true" : "false")
      << ",\"user\":\""     << json_escape(ident_user())  << "\""
      << ",\"place\":\""    << json_escape(ident_place()) << "\""
      // Detached-only question: is there a client to attach TO? The sidebar
      // readout says "No client" or "Not attached" from this.
      << ",\"client\":"     << (client_running() ? "true" : "false")
      << "}";
    ui_post("STATE:" + j.str());
    hud_push("STATE:" + j.str());
}

// The offset table, sent once so the palette can search it locally.
void push_offsets() {
    std::ifstream f(g_conf);
    std::ostringstream j;
    j << "[";
    bool first = true;
    std::string line;
    while (std::getline(f, line)) {
        size_t h = line.find('#');
        if (h != std::string::npos) line = line.substr(0, h);
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
        if (k.empty() || v.empty()) continue;
        if (!first) j << ",";
        first = false;
        j << "[\"" << json_escape(k) << "\",\"" << json_escape(v) << "\"]";
    }
    j << "]";
    ui_post("OFFSETS:" + j.str());
}


// The runnable tree for the in-game panel. Deliberately not the page's payload:
// that carries order, dirty flags, file kinds and source text, none of which a
// list you tap to run has any use for.
//
// A PROJECT is one entry, not a folder of files, because that is what it is:
// its .cpp files compile together and a member on its own does not build. The
// panel offering `backdash/gates.cpp` would be offering something that cannot
// work -- which is exactly what running a group member as a lone file did,
// failing on a sibling #include that a project build resolves.
void push_hud_snippets() {
    std::ostringstream j;
    j << "[";
    bool first = true;
    auto entry = [&](const char* type, const std::string& path) {
        if (!first) j << ",";
        first = false;
        j << "{\"t\":\"" << type << "\",\"p\":\"" << json_escape(path) << "\"";
    };

    // Line counts, cached against size + write time.
    //
    // Counting them means READING every snippet, and this runs on the watcher's
    // 2-second poll whether or not anything changed -- measured at 329 KB per
    // push across 28 files, almost all of it thrown away again by the
    // unchanged-snapshot check below. A stat per file answers "is this the same
    // file" for a fraction of the cost, and a file that did change is read once
    // and remembered.
    struct Counted { uint64_t size; FILETIME mtime; int lines; };
    static std::map<std::string, Counted> cache;

    auto lines_in = [](const std::string& rel) -> int {
        std::string full = g_snipdir + "\\" + win_path(rel);
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(widen(full).c_str(), GetFileExInfoStandard, &fad))
            return 0;
        uint64_t sz = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;

        auto it = cache.find(rel);
        if (it != cache.end() && it->second.size == sz &&
            CompareFileTime(&it->second.mtime, &fad.ftLastWriteTime) == 0)
            return it->second.lines;

        std::ifstream f(full, std::ios::binary);
        if (!f) return 0;
        int n = 1 + (int)std::count(std::istreambuf_iterator<char>(f),
                                    std::istreambuf_iterator<char>(), '\n');
        cache[rel] = { sz, fad.ftLastWriteTime, n };
        return n;
    };

    // .order, exactly as the sidebar reads it -- one sequence holding both
    // snippet paths and group paths, a line being whichever list it is found
    // in. Without this the panel listed things in whatever order the filesystem
    // handed back, so the same tree appeared in two different orders depending
    // on which window you looked at.
    std::vector<std::string> order = read_order(g_snipdir);
    auto rank = [&](const std::string& path) {
        auto it = std::find(order.begin(), order.end(), path);
        // Unmentioned entries sort after everything named, matching the
        // sidebar's "saved order first, then whatever is new" rule.
        return it == order.end() ? order.size() : (size_t)(it - order.begin());
    };

    std::vector<std::string> loose;
    scan_cpp(g_snipdir, "", loose);
    loose.erase(std::remove_if(loose.begin(), loose.end(), [](const std::string& f) {
        return f.size() < 4 || f.compare(f.size() - 4, 4, ".cpp") != 0;
    }), loose.end());

    // Top level is loose files and groups interleaved, because .order holds
    // them in one sequence and a group can sit between two snippets.
    struct Top { std::string path; bool group; };
    std::vector<Top> top;
    for (const std::string& f : loose) top.push_back({ f, false });
    for (const std::string& g : list_groups(g_snipdir)) top.push_back({ g, true });
    std::stable_sort(top.begin(), top.end(), [&](const Top& a, const Top& b) {
        return rank(a.path) < rank(b.path);
    });

    for (const Top& t : top) {
        if (!t.group) {
            entry("file", t.path);
            j << ",\"n\":" << lines_in(t.path) << "}";
            continue;
        }
        std::string marker = g_snipdir + "\\" + win_path(t.path) + "\\.project";
        if (GetFileAttributesW(widen(marker).c_str()) != INVALID_FILE_ATTRIBUTES) {
            entry("project", t.path);
            j << "}";
            continue;
        }

        std::vector<std::string> kids;
        scan_cpp(g_snipdir + "\\" + win_path(t.path), t.path + "/", kids);
        kids.erase(std::remove_if(kids.begin(), kids.end(), [](const std::string& f) {
            return f.size() < 4 || f.compare(f.size() - 4, 4, ".cpp") != 0;
        }), kids.end());
        std::stable_sort(kids.begin(), kids.end(), [&](const std::string& a, const std::string& b) {
            return rank(a) < rank(b);
        });

        entry("group", t.path);
        j << ",\"c\":[";
        bool fk = true;
        for (const std::string& f : kids) {
            if (!fk) j << ",";
            fk = false;
            j << "{\"p\":\"" << json_escape(f) << "\",\"n\":" << lines_in(f) << "}";
        }
        j << "]}";
    }
    j << "]";
    hud_push("SNIPS:" + j.str());
}

void push_snippets() {
    // Loose snippets first, then one level of group directories. The DIRECTORY
    // is the source of truth for what exists and which group it is in; .order
    // only ever answers "in what sequence".
    std::vector<std::string> groups = list_groups(g_snipdir);

    std::vector<std::string> files;
    scan_cpp(g_snipdir, "", files);
    for (const std::string& g : groups)
        scan_cpp(g_snipdir + "\\" + win_path(g), g + "/", files);

    // .order holds BOTH snippet paths and group paths, in one sequence. The
    // two are told apart by lookup rather than by any marker in the file: a
    // line is whichever list it is found in, and a line in neither is stale
    // and skipped. That keeps the format a plain list of paths.
    std::vector<std::string> order = read_order(g_snipdir);

    // Saved order first, then anything the order file doesn't mention (a newly
    // created or externally dropped snippet) appended as the filesystem gave
    // it. Names in .order that no longer exist are simply skipped.
    std::vector<std::string> ordered;
    for (const std::string& want : order) {
        auto it = std::find(files.begin(), files.end(), want);
        if (it != files.end()) { ordered.push_back(*it); files.erase(it); }
    }
    for (const std::string& rest : files) ordered.push_back(rest);

    // Same treatment for the groups, so a folder keeps the position it was
    // dragged to. Unmentioned groups keep list_groups()' own sequence, which
    // is sorted -- so a brand new folder lands predictably rather than
    // wherever the directory scan happened to put it.
    {
        std::vector<std::string> rest = groups;
        std::vector<std::string> gordered;
        for (const std::string& want : order) {
            auto it = std::find(rest.begin(), rest.end(), want);
            if (it != rest.end()) { gordered.push_back(*it); rest.erase(it); }
        }
        for (const std::string& g : rest) gordered.push_back(g);
        groups = gordered;
    }

    // Groups are sent SEPARATELY from the snippets rather than being inferred
    // from their paths. Inferring them means a group only exists while it holds
    // something, so a newly made one is invisible until you put a file in it --
    // which makes "create a group, then fill it" impossible.
    std::ostringstream j;
    j << "{\"groups\":[";
    for (size_t i = 0; i < groups.size(); i++) {
        if (i) j << ",";
        j << "\"" << json_escape(groups[i]) << "\"";
    }
    // Which of those groups are projects. A marker FILE rather than a rule
    // about the contents: deriving it from "does some file define fx_run" would
    // mean dropping a full snippet into a group silently turns it into a
    // project and stops its neighbours being runnable, with nothing on screen
    // to say why.
    j << "],\"projects\":[";
    bool firstp = true;
    for (const std::string& g : groups) {
        std::string marker = g_snipdir + "\\" + win_path(g) + "\\.project";
        if (GetFileAttributesW(widen(marker).c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (!firstp) j << ",";
        firstp = false;
        j << "\"" << json_escape(g) << "\"";
    }
    j << "],\"items\":[";
    for (size_t i = 0; i < ordered.size(); i++) {
        std::ifstream f(g_snipdir + "\\" + win_path(ordered[i]), std::ios::binary);
        std::string code((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (i) j << ",";
        // "n" is the path relative to snippets/ and is the identity used by
        // every other message; "g" is its parent group, split out so the page
        // does not have to parse it.
        j << "{\"n\":\"" << json_escape(ordered[i]) << "\",\"g\":\""
          << json_escape(parent_of(ordered[i])) << "\",\"code\":\""
          << json_escape(code) << "\"}";
    }
    j << "]}";
    ui_post("SNIPPETS:" + j.str());
    push_hud_snippets();
}







// ----------------------------- liveness watcher -----------------------------
//
// attach() takes a one-off snapshot, so without this the UI claims attached
// long after the client exits, and the DataModel stays unresolved until a
// manual Attach. Polling once a second covers both: client gone -- detach and
// say so; DM not there yet -- retry quietly, since it appears by itself on
// joining a place. The retry removes the "join, then re-attach" step.
static std::atomic<bool> g_watch_run{false};
static HANDLE            g_watch_thread = nullptr;

// ---- stop watchdog -----------------------------------------------------------
//
// Stopping is cooperative by necessity: a snippet polls fx_running() and
// returns, because killing the thread would unload its DLL with its own code
// still executing. The cost of that is a snippet which stops polling can never
// be stopped -- and the only signal was the Run button sitting on "Stop"
// forever, which looks identical to a snippet that is simply slow.
//
// This cannot force anything. What it can do is stop the silence: notice that
// Stop was asked for a while ago and nothing happened, and say so once, with
// the actual remedy. Found the hard way in Rivals, where an unbounded child
// walk wedged a snippet and the console had to go through Task Manager with no
// hint as to why.
//
// FX_MAX_CHILDREN fixed that particular cause. This is for the next one.
// The truth about what is running now lives in the core registry (run.h);
// the UI derives its list from core_run_list() rather than keeping a parallel
// copy. g_run_label is only the LAST name requested over RUNNAME:, kept so the
// panel has something to show between a request and the run actually starting.
static std::string g_run_label;
void push_runstate();

// Cap + create, naming what holds the slots so the refusal doubles as the
// diagnosis (docs/multi-run.md). Host-side because the message is UI text.
static unsigned begin_run_slot(const std::string& label) {
    auto runs = core_run_list();
    if ((int)runs.size() >= CORE_RUN_CAP) {
        std::string names;
        for (size_t i = 0; i < runs.size(); i++) { if (i) names += ", "; names += runs[i].label; }
        ui_log("WARN", label + " cannot run: " + std::to_string(CORE_RUN_CAP) +
                       " already running (" + names + ") - stop one first");
        return 0;
    }
    unsigned id = core_run_create(label);   // may still lose a race to the cap
    if (!id) ui_log("WARN", label + " cannot run: too many already running - stop one first");
    return id;
}

// One run per call, on its own thread. No stop-then-start any more -- several
// runs coexist, so a new one just takes a slot (or is refused by the cap). The
// core registry owns the Run; this sets the thread's current run, runs the
// body, then clears and frees it. t_run is set BEFORE body() so every fx_*
// inside resolves to this run.
static void spawn_run(const std::string& label, std::function<void()> body) {
    std::thread([label, body]() {
        unsigned id = begin_run_slot(label);
        if (!id) { push_state(); return; }         // refused; nothing started
        core_run_set_current(id);
        push_runstate();               // the list now carries the new run to both views
        body();                        // run_snippet / run_project
        core_run_clear_current();
        core_run_destroy(id);
        push_runstate();               // ...and its removal
        push_state();
    }).detach();
}

// RUNSTATE is a LIST now, not a boolean. The console sidebar reads runs[]; the
// in-game panel reads the aggregate fields (running = any live, building = any
// compiling, name = a representative label) so its single summary line keeps
// working unchanged.
void push_runstate() {
    auto runs = core_run_list();
    bool any_building = false;
    for (auto& r : runs) if (r.building) any_building = true;
    std::string name = runs.empty() ? g_run_label : runs.back().label;

    std::ostringstream j;
    j << "{\"running\":" << (!runs.empty() ? "true" : "false")
      << ",\"building\":" << (any_building ? "true" : "false")
      << ",\"count\":" << runs.size()
      << ",\"name\":\"" << json_escape(name) << "\""
      << ",\"runs\":[";
    for (size_t i = 0; i < runs.size(); i++) {
        if (i) j << ",";
        j << "{\"id\":" << runs[i].id
          << ",\"name\":\"" << json_escape(runs[i].label) << "\""
          << ",\"building\":" << (runs[i].building ? "true" : "false")
          << ",\"stopping\":" << (runs[i].stopping ? "true" : "false") << "}";
    }
    j << "]}";
    // To BOTH views: the console page reads runs[] to drive its sidebar and Run
    // button, the in-game panel reads the aggregate fields. Same snapshot, one
    // source of truth -- the same pattern STATE follows.
    ui_post("RUNSTATE:" + j.str());
    hud_push("RUNSTATE:" + j.str());
}

// ---- stop watchdog ---------------------------------------------------------
// Cannot force anything. What it does is break the silence: notice that Stop
// was asked of a run a while ago and it has not left, and say so once, per run,
// with the remedy. Now keyed per run id (the registry's stop_ms), because with
// several live one wedged snippet must be named without implicating the others.
static std::map<unsigned,int> g_wedge_stage;   // run id -> which warning given

static void watchdog_tick() {
    auto runs = core_run_list();
    // Forget stages for runs that have ended.
    for (auto it = g_wedge_stage.begin(); it != g_wedge_stage.end(); ) {
        bool live = false;
        for (auto& r : runs) if (r.id == it->first) { live = true; break; }
        it = live ? std::next(it) : g_wedge_stage.erase(it);
    }
    for (auto& r : runs) {
        if (!r.stopping) continue;             // no stop pending; a long run is fine
        int& stage = g_wedge_stage[r.id];
        // 5s: generous. A snippet polling fx_running() once a frame returns
        // within a tick; even one on a long fx_pace is under a second.
        if (r.stop_ms > 5000 && stage < 1) {
            stage = 1;
            ui_log("WARN", "! " + r.label +
                   " not stopped yet - may be busy in a long read rather than sleeping");
        }
        // 15s: it is not coming back on its own. Only the first line is an
        // error; the second is what to DO. Restarting DOES lose unsaved buffers
        // (rememberTabs persists names and positions, never contents), so the
        // remedy names that rather than a false "nothing is lost".
        if (r.stop_ms > 15000 && stage < 2) {
            stage = 2;
            ui_log("ERROR", "x " + r.label +
                   " is not responding to Stop - it stopped polling fx_running()");
            ui_log("WARN",  "  only that snippet is stuck: save any unsaved tabs, then restart the console");
        }
    }
}

static DWORD WINAPI watch_client(LPVOID) {
    ec::mark_host_thread();   // attach_poll reads the client; see eclipse_core.h
    while (g_watch_run) {
        attach_poll();

        // Outside the attached/detached branches: a wedged snippet is worth
        // reporting whether or not the client is still there. In fact a client
        // that exited mid-run is one of the ways a snippet gets stuck.
        watchdog_tick();

        for (int i = 0; i < 10 && g_watch_run; i++) Sleep(100);
    }
    return 0;
}

// The window starts hidden -- the library's own ShowWindow is patched out (see
// tools/patch_webview.cmake) because it fires before embed(), leaving an empty
// frame on screen for the whole of WebView2's startup.
//
// Idempotent, because two things race to call it: the page reporting that it has
// painted, and the timeout below. If rendering fails, or a message never lands,
// a window that stays hidden forever is a far worse bug than the flash this
// fixes -- so something always shows it.
static void reveal_window(bool last_resort) {
    if (!g_hwnd || IsWindowVisible(g_hwnd)) return;
    // The PAGE painting is not evidence about attach. PAINTED arrives BEFORE
    // do_attach has run, so in overlay mode the gate below could not know yet
    // and showed the window anyway -- the flash the user kept reporting. In
    // overlay mode only the LAST RESORT timer may reveal; the no-client valve
    // in hud_tick owns the ordinary case, once a probe has actually happened.
    if (overlay_pref() && !last_resort) return;
    // OVERLAY MODE: the console lives in the game, and this call used to
    // flash the desktop window at startup before the tick hid it again (user
    // report, 2026-08-22). Skip the show while the attach machinery is
    // demonstrably alive -- a probe has run, or a client is attached; the
    // no-client valve in hud_tick owns showing the window when there is
    // nothing to attach to. If neither is true by the time the 4s timer
    // lands here, something upstream is broken (the page never announced,
    // so do_attach never ran) and a flash beats a console with no window.
    if (overlay_pref() && (attach_probed() || ec::state().pid)) return;
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    SetForegroundWindow(g_hwnd);
    SetFocus(g_hwnd);
}

// Set during startup, reported on READY. Logging it at the point it is decided
// loses it: the sink posts into the page, and the page is not listening yet.
static bool g_nonclient = false;

void handle(const std::string& msg) {
    auto starts = [&](const char* p) {
        size_t n = strlen(p);
        return msg.size() >= n && msg.compare(0, n, p) == 0;
    };

    if (msg == "PAINTED") { reveal_window(false); return; }
    // The open leg's paint witness (overlay.cpp, app_open_finish).
    if (msg == "APPPAINTED") { app_painted(); return; }

    if (msg == "READY") {
        // Only reported when it FAILS. Working is the normal case and needs no
        // announcement; not working is what explains a titlebar with no system
        // menu, moved by the page's SC_MOVE fallback instead of by Windows.
        if (!g_nonclient)
            ec::logf("  x native titlebar unavailable - WebView2 runtime too old, "
                     "using the page fallback\n");
        // Where snippets actually live. Not beside the exe, so without this
        // there is nothing on screen saying which tree an external editor
        // should be pointed at.
        ec::logf("  snippets: %s\n", g_snipdir.c_str());
        push_offsets();
        push_snippets();
        push_runstate();   // the current run list, so a reloaded page shows what is live
        // Which bootstrapper the Launch button should name. Sent once: install
        // paths do not change while the app is open, and the button only needs
        // a label.
        ui_post(std::string("LAUNCHER:") + detect_launcher().label);
        do_attach();
        return;
    }
    if (msg == "REFRESH_OFFSETS") { refresh_offsets_async(); return; }

    if (msg == "LAUNCH_ROBLOX") { launch_roblox(); return; }

    // The desktop asked to move the console into the game: the same window,
    // positioned over the client with the overlay animating the hand-off.
    // Needs a client to sit over and a composed overlay for the travel box.

    // The console page's own close-the-app-mode control (Esc in the app
    // window). The badge and the Insert hotkey are the other two doors.
    // The badge menu's X. The one irreversible action in the overlay, and it
    // is deliberately the only way to quit from in-game now that the
    // titlebar X returns to the badge -- right-click, then aim at a specific
    // button. WM_CLOSE rather than exit(): the window's own teardown saves
    // placement, restores the desktop identity and releases the hooks.
    if (msg == "QUIT") { if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0); return; }

    if (msg == "CHECK_OFFSETS") {
        // Off the UI thread: a few hundred reads is quick, but every one is a
        // syscall into another process and the window should not stutter.
        std::thread([]() {
            ec::mark_host_thread();   // health_check walks services
            auto res = ec::health_check();
            int pass = 0, fail = 0, skip = 0;
            ui_log("STATUS", "offset health check:");
            for (const auto& c : res) {
                if (c.status == ec::HealthCheck::Pass) pass++;
                else if (c.status == ec::HealthCheck::Fail) fail++;
                else skip++;

                std::string line = std::string(c.status == ec::HealthCheck::Pass ? "  + "
                                             : c.status == ec::HealthCheck::Fail ? "  x "
                                                                                 : "  - ")
                                 + c.name + ": " + c.detail;
                // The suspect offsets only on a failure -- on a pass they are
                // noise, and the point of naming them is to shorten the hunt.
                if (c.status == ec::HealthCheck::Fail && !c.offsets.empty())
                    line += "   [" + c.offsets + "]";

                ui_log(c.status == ec::HealthCheck::Pass ? "STATUS"
                     : c.status == ec::HealthCheck::Fail ? "ERROR" : "WARN", line);
            }
            char b[160];
            snprintf(b, sizeof b, "%d passed, %d failed, %d skipped%s",
                     pass, fail, skip,
                     fail ? " - a failure means the offset is wrong, not the game"
                          : " - offsets look sane");
            ui_log(fail ? "ERROR" : "STATUS", b);
        }).detach();
        return;
    }

    // Deliberate teardown. The handle is closed and every cached pointer is
    // dropped by ec::detach(), so a snippet still polling reads nothing rather
    // than reading something stale -- failures, not wrong answers.
    if (msg == "DETACH") {
        attach_user_detach();
        // TELL THE PAGE. Without this the UI keeps its last state -- still
        // showing the old pid, with the button still reading "Detach" -- so the
        // detach appeared to do nothing and got pressed again and again. The
        // watchdog is no help here: once the pid is zero and the user asked for
        // it, that branch deliberately does nothing at all, and pushing state
        // is not something "nothing" includes.
        push_state();
        ui_log("STATUS", "detached - press Attach to reconnect");
        return;
    }

    if (msg == "REATTACH") {
        attach_reattach();
        return;
    }
    if (msg == "CLIENTS") {
        // The clients panel opened and wants a list. The old picker only ever
        // got one when 2+ clients existed -- that was enough for a modal whose
        // whole reason to exist was ambiguity, and is not enough for a panel
        // you open on purpose: with one client it would have come up empty.
        list_clients_async();
        return;
    }
    if (starts("ATTACH_PID:")) {
        // The page picked one from the CANDIDATES modal.
        uint32_t pid = (uint32_t)strtoul(msg.c_str() + 11, nullptr, 10);
        attach_to_pid(pid);
        return;
    }
    if (starts("LOOKUP_PLACE:")) {
        // Page-driven request to resolve a PlaceId to a game title. Fires
        // and forgets -- the WinHTTP calls run on their own thread and the
        // result arrives asynchronously as PLACE_NAME:<pid>|<name>.
        uint64_t pid = strtoull(msg.c_str() + 13, nullptr, 10);
        if (pid) lookup_place_name_async(pid);
        return;
    }
    if (starts("LOOKUP_AVATAR:")) {
        // Same shape as LOOKUP_PLACE, and for the same reason -- the page
        // cannot reach Roblox from a file:// origin. Answered as
        // AVATAR:<userId>|<url>, or not at all if there is no headshot.
        int64_t uid = strtoll(msg.c_str() + 14, nullptr, 10);
        if (uid > 0) lookup_avatar_async(uid);
        return;
    }

    if (msg == "LOGCLEAR") {
        // ONE FUNNEL, ONE CLEAR. Both views replay from g_loghist, so a view
        // that cleared only itself came back full the next time the host
        // replayed -- a panel rebuilt on reattach, or the app frame on first
        // entry into overlay mode. Clearing the history is what makes it
        // stick, and it is the same "the panel is a view onto the same lines,
        // not a second log" rule flush_logs is written around.
        //
        // Told to both views explicitly rather than by pushing an empty
        // LOGS: batch -- LOGS appends rather than replaces, so an empty one
        // would clear nothing.
        log_clear_history();
        ui_deliver_logmsg("LOGCLEAR");
        return;
    }

    if (msg == "STOP") {
        // Bare STOP now means stop-all (the console's global Stop, and the
        // safe answer when nothing more specific is asked). The per-run stop
        // timestamp that drives the watchdog and the "stopping..." UI is stamped
        // inside the registry, so nothing to track here.
        ec::stop_all_runs();
        push_runstate();
        ui_log("STATUS", "stopping...");
        return;
    }

    // Stop ONE run by id (the sidebar's per-snippet Stop button).
    if (starts("STOP:")) {
        unsigned id = (unsigned)strtoul(msg.c_str() + 5, nullptr, 10);
        if (id) { ec::stop_run(id); push_runstate(); }
        return;
    }

    if (starts("EXPORT:")) {
        // Three sections, each introduced by its own marker line:
        //   [opts] settings=1 offsets=1
        //   [files]   one snippet path per line, exactly what was ticked
        //   [settings] k=v, written into the bundle verbatim
        std::string body = msg.substr(7);
        bool wantSettings = false, wantOffsets = false;
        std::vector<std::string> files;
        std::string settings;

        int section = 0;                          // 0 none, 1 files, 2 settings
        size_t p = 0;
        while (p <= body.size()) {
            size_t nl = body.find('\n', p);
            std::string line = body.substr(p, nl == std::string::npos
                                              ? std::string::npos : nl - p);
            while (!line.empty() && line.back() == '\r') line.pop_back();

            if (line.compare(0, 6, "[opts]") == 0) {
                wantSettings = line.find("settings=1") != std::string::npos;
                wantOffsets  = line.find("offsets=1")  != std::string::npos;
                section = 0;
            } else if (line == "[files]")    section = 1;
            else if (line == "[settings]")   section = 2;
            else if (!line.empty()) {
                if      (section == 1) files.push_back(line);
                else if (section == 2) settings += line + "\n";
            }
            if (nl == std::string::npos) break;
            p = nl + 1;
        }
        do_export(g_hwnd, g_snipdir, files, wantSettings, wantOffsets, settings);
        return;
    }

    if (msg == "IMPORT") { do_import(g_hwnd, g_snipdir); return; }

    if (starts("SETTINGS:")) {
        // key=value, one per line. Deliberately not JSON: the host only ever
        // BUILDS json and has no parser, and every host-side setting is a
        // scalar. An unrecognised key is ignored rather than an error, so the
        // page and the host can be updated in either order.
        std::string body = msg.substr(9);
        size_t p = 0;
        while (p <= body.size()) {
            size_t nl = body.find('\n', p);
            std::string line = body.substr(p, nl == std::string::npos
                                              ? std::string::npos : nl - p);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string k = line.substr(0, eq), v = line.substr(eq + 1);
                if      (k == "console.dryFlushMs")  ec::set_dry_window_ms((unsigned)atoi(v.c_str()));
                else if (k == "snippets.extraFlags") ec::set_extra_flags(v);
                else if (k == "snippets.showWarnings") ec::set_show_warnings(v == "1");
                else if (k == "console.logToFile")   set_log_file(v == "1");
                // Off means off NOW, not at the next close: drop that half of
                // the stored in-game geometry and go back to the default, so
                // the switch is something the user can see the effect of on
                // the next open rather than after a restart. Only the half
                // being switched off is dropped -- the other switch's value
                // has to survive this one being turned off.
                else if (k == "window.rememberPos") {
                    g_remember_pos = (v == "1");
                    if (!g_remember_pos) ui_geometry_forget(true, false);
                }
                else if (k == "window.rememberSize") {
                    g_remember_size = (v == "1");
                    if (!g_remember_size) ui_geometry_forget(false, true);
                }
                // Both overlay switches hand the whole transition to the
                // machine that owns the flags (overlay.cpp) -- the
                // reasoning lives with the code there.
                else if (k == "console.overlayMode") overlay_pref_set(v == "1");
                else if (k == "console.hud")         overlay_hud_set(v == "1");
                // Inverted: the setting is phrased as the SAFE action, so on
                // means disarm on detach and the core flag is its opposite.
                else if (k == "safety.disarmOnDetach") ec::set_keep_armed(v != "1");
            }
            if (nl == std::string::npos) break;
            p = nl + 1;
        }
        return;
    }

    if (starts("ARM:")) {
        bool on = msg.substr(4) == "1";
        ec::set_armed(on);
        ui_log("STATUS", on ? "! ARMED - writes will reach the process"
                            : "disarmed - writes are logged only");
        push_state();
        return;
    }

    // Sent by the page immediately before RUN:, so the panel can say what is
    // running. Purely a label -- it starts nothing.
    if (starts("RUNNAME:")) {
        g_run_label = msg.substr(8);
        push_runstate();
        return;
    }

    // Run a snippet BY PATH. RUN: carries a body because the page has one in an
    // editor; the panel has a list of names and no source, so the host reads
    // the file itself. safe_rel is what keeps a name from the page (or from a
    // stale list) reaching outside snippets/.
    if (starts("RUN_FILE:")) {
        std::string rel = msg.substr(9);
        if (!safe_rel(rel)) { ui_log("ERROR", "x refusing to run - invalid path"); return; }
        std::ifstream f(g_snipdir + "\\" + win_path(rel), std::ios::binary);
        if (!f) { ui_log("ERROR", "x cannot read " + rel); return; }
        std::string body((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
        if (body.find_first_not_of(" \t\r\n") == std::string::npos) {
            ui_log("WARN", "! " + rel + " is empty");
            return;
        }
        size_t slash = rel.find_last_of('/');
        std::string label = (slash == std::string::npos) ? rel : rel.substr(slash + 1);
        g_run_label = label;
        spawn_run(label, [body]() { ec::run_snippet(body); });
        return;
    }

    if (starts("RUN:")) {
        std::string body = msg.substr(4);
        // The label came over RUNNAME: just before this (the page always sends
        // it first). Captured per run so concurrent runs keep their own names.
        std::string label = g_run_label.empty() ? std::string("(snippet)") : g_run_label;
        // Off the UI thread: compiling a snippet shells out to g++ and would
        // otherwise freeze the window for the duration.
        spawn_run(label, [body]() { ec::run_snippet(body); });
        return;
    }

    if (starts("RUN_PROJECT:")) {
        std::string rel = msg.substr(12);
        if (!safe_rel(rel)) { ui_log("ERROR", "x refusing to run - invalid path"); return; }
        std::string dir = g_snipdir + "\\" + win_path(rel);
        size_t slash = rel.find_last_of('/');
        std::string label = (slash == std::string::npos) ? rel : rel.substr(slash + 1);
        g_run_label = label;
        spawn_run(label, [dir]() { ec::run_project(dir); });
        return;
    }

    if (starts("PROJECT_MARK:") || starts("PROJECT_UNMARK:")) {
        bool mark = starts("PROJECT_MARK:");
        std::string rel = msg.substr(mark ? 13 : 15);
        if (!safe_rel(rel)) { ui_log("ERROR", "x invalid group"); return; }
        std::string marker = g_snipdir + "\\" + win_path(rel) + "\\.project";
        if (mark) {
            if (GetFileAttributesW(widen(marker).c_str()) == INVALID_FILE_ATTRIBUTES) {
                std::ofstream o(marker, std::ios::binary);
                o << "# This folder is a project: every .cpp in it is compiled and\n"
                     "# linked together, and one of them must define fx_run().\n"
                     "#\n"
                     "# Optional:\n"
                     "#   flags = -O3 -DSOMETHING\n";
            }
            ui_log("STATUS", "+ " + rel + " is now a project");
        } else {
            // The marker is deleted outright rather than trashed: it holds no
            // work, and leaving a stale copy in .trash would be more confusing
            // than losing two comment lines.
            DeleteFileW(widen(marker).c_str());
            ui_log("STATUS", "+ " + rel + " is a plain group again");
        }
        push_snippets();
        return;
    }

    // SAVE overwrites, SAVE_NEW refuses to. Creating a snippet and saving one
    // are the same write, but they have opposite expectations about a file
    // that is already there: the editor's save MUST overwrite, that is what
    // saving means -- while "new snippet" or "duplicate" landing on a name
    // already taken has destroyed the user's work, and silently, because a
    // truncating ofstream reports nothing and the status line still says
    // "saved". GROUP_NEW, GROUP_RENAME and SNIP_RENAME all guard against
    // clobbering; SAVE was the one creation path that did not. (Observed: a
    // "new snippet" named radar over an existing radar.cpp, contents gone.)
    if (starts("SAVE:") || starts("SAVE_NEW:")) {
        bool creating = starts("SAVE_NEW:");
        std::string rest = msg.substr(creating ? 9 : 5);
        size_t nl = rest.find('\n');
        if (nl == std::string::npos) return;
        std::string name = rest.substr(0, nl), code = rest.substr(nl + 1);
        if (!safe_rel(name)) {
            ui_log("ERROR", "x refusing to save - invalid name");
            return;
        }
        CreateDirectoryW(widen(g_snipdir).c_str(), nullptr);

        std::string path = g_snipdir + "\\" + win_path(name);

        // Checked before ensure_group_dirs so a refused create leaves no
        // stray folders behind. Catches a collision with a GROUP too: the
        // name would be a directory, and the ofstream below would fail
        // without saying so.
        if (creating &&
            GetFileAttributesW(widen(path).c_str()) != INVALID_FILE_ATTRIBUTES) {
            ui_log("ERROR", "x " + name + " already exists");
            return;
        }

        // Saving into a group that does not exist yet creates it, so a new
        // group needs no separate "make folder" step.
        ensure_group_dirs(g_snipdir, name);
        std::ofstream o(path, std::ios::binary);
        o << code;
        o.close();
        // Reporting the stream state rather than assuming it: claiming "+
        // saved" for a write that did not happen is how work gets lost
        // without anyone noticing until much later.
        if (o.fail()) ui_log("ERROR",  "x could not write " + name);
        else          ui_log("STATUS", "+ saved " + name);
        push_snippets();
        return;
    }

    if (starts("SNIP_DELETE:")) {
        std::string name = msg.substr(12);
        if (!safe_rel(name)) { ui_log("ERROR", "x refusing to delete - invalid name"); return; }
        trash_remember(g_snipdir, flatten_name(name), name);
        // Moved aside rather than unlinked: recoverable, so the UI doesn't need
        // to interrupt with a confirmation for something this small.
        std::string trash = trash_dir(g_snipdir);
        CreateDirectoryW(widen(trash).c_str(), nullptr);
        std::string flat = flatten_name(name);
        std::string from = g_snipdir + "\\" + win_path(name);
        std::string to   = trash + "\\" + flat;
        DeleteFileW(widen(to).c_str());                 // overwrite a previous copy
        if (MoveFileW(widen(from).c_str(), widen(to).c_str()))
            ui_log("STATUS", "+ moved " + name + " to snippets\\.trash");
        else
            ui_log("ERROR", "x could not delete " + name);
        push_snippets();
        // Keeps the sidebar's trash count honest without the page having to
        // ask after every delete.
        push_trash(g_snipdir);
        return;
    }

    if (msg == "TRASH_LIST") { push_trash(g_snipdir); return; }

    if (starts("TRASH_RESTORE:")) {
        std::string flat = msg.substr(14);
        if (!safe_trash_name(flat)) { ui_log("ERROR", "x refusing to restore - invalid name"); return; }

        // Where it came from, if we know. Anything trashed before the index
        // existed goes back to the top level under its flattened name -- the
        // underscores are not reliably reversible, and inventing a group from
        // them would put work somewhere it never was.
        std::string dest = flat;
        for (auto& e : trash_load_index(g_snipdir)) if (e.first == flat) { dest = e.second; break; }
        if (!safe_rel(dest)) dest = flat;

        std::string to = g_snipdir + "\\" + win_path(dest);
        if (GetFileAttributesA(to.c_str()) != INVALID_FILE_ATTRIBUTES) {
            ui_log("ERROR", "x " + dest + " already exists - rename it first");
            return;
        }
        // The group may have been deleted since. Recreate one level so a
        // restore does not fail for a folder the user can trivially remake.
        size_t slash = dest.find_last_of('/');
        if (slash != std::string::npos)
            CreateDirectoryW(widen(g_snipdir + "\\" + win_path(dest.substr(0, slash))).c_str(), nullptr);

        if (MoveFileW(widen(trash_dir(g_snipdir) + "\\" + flat).c_str(), widen(to).c_str())) {
            trash_forget(g_snipdir, flat);
            ui_log("STATUS", "+ restored " + dest);
        } else {
            ui_log("ERROR", "x could not restore " + flat);
        }
        push_snippets();
        push_trash(g_snipdir);
        return;
    }

    if (starts("TRASH_PURGE:")) {
        std::string flat = msg.substr(12);
        if (!safe_trash_name(flat)) { ui_log("ERROR", "x refusing to delete - invalid name"); return; }
        if (DeleteFileW(widen(trash_dir(g_snipdir) + "\\" + flat).c_str())) {
            trash_forget(g_snipdir, flat);
            ui_log("STATUS", "+ deleted " + flat + " permanently");
        } else {
            ui_log("ERROR", "x could not delete " + flat);
        }
        push_trash(g_snipdir);
        return;
    }

    if (msg == "TRASH_EMPTY") {
        int n = 0;
        // Collected first, then deleted: removing entries while the find handle
        // is walking the same directory is undefined.
        std::vector<std::string> names;
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(widen(trash_dir(g_snipdir) + "\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                names.push_back(narrow(fd.cFileName));
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        for (const std::string& nm : names)
            if (DeleteFileW(widen(trash_dir(g_snipdir) + "\\" + nm).c_str()) && nm != "index.txt") n++;
        ui_log("STATUS", "+ emptied trash (" + std::to_string(n) + " file" +
                         (n == 1 ? "" : "s") + ")");
        push_trash(g_snipdir);
        return;
    }

    if (starts("SNIP_ORDER:")) {
        // One filename per line, already in the order the UI is showing. No
        // push_snippets() afterwards: the UI is the source of this change and
        // re-sending would just fight whatever the user does next.
        std::string body = msg.substr(11);
        std::ofstream o(order_file(g_snipdir), std::ios::binary);
        size_t p = 0;
        while (p <= body.size()) {
            size_t nl = body.find('\n', p);
            std::string n = body.substr(p, nl == std::string::npos ? std::string::npos : nl - p);
            while (!n.empty() && n.back() == '\r') n.pop_back();
            // Same guard as save/rename: this is written to disk and read back
            // as a path, so nothing may point outside snippets/.
            if (safe_rel(n)) o << n << "\n";
            if (nl == std::string::npos) break;
            p = nl + 1;
        }
        return;
    }

    // Group names are paths, so they validate exactly like a snippet path.
    // The only extra rule is that a group may not be moved inside itself, which
    // MoveFile would otherwise happily do and leave unreachable.
    auto inside = [&](const std::string& outer, const std::string& inner) {
        return inner.size() > outer.size() &&
               inner.compare(0, outer.size(), outer) == 0 &&
               inner[outer.size()] == '/';
    };

    // Rename and move are the same filesystem operation here -- a move is a
    // rename with a different folder in front -- but they are not the same event
    // to read about. Saying "moved Aimbot to aimbot" for a change of case, or
    // "renamed" for a drag into another group, is a log that describes the
    // implementation instead of what happened.
    auto move_or_rename = [&](const std::string& what,
                              const std::string& from, const std::string& to) {
        if (parent_of(from) == parent_of(to))
            return "+ renamed " + what + " " + leaf_of(from) + " to " + leaf_of(to);
        if (parent_of(to).empty())
            return "+ moved " + what + " " + from + " out to the top level";
        return "+ moved " + what + " " + from + " to " + parent_of(to);
    };

    // Windows filenames are case-insensitive, so "does the destination already
    // exist" answers YES when the only change is capitalisation -- renaming
    // "aimbot" to "Aimbot" was refused because it found itself. MoveFile handles
    // a case-only rename perfectly well; it is just the guard that has to know
    // the two names are the same file.
    auto same_file = [&](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++)
            if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
        return true;
    };

    if (starts("GROUP_NEW:")) {
        std::string g = msg.substr(10);
        while (!g.empty() && (g.back() == '\r' || g.back() == '\n')) g.pop_back();
        if (!safe_rel(g)) { ui_log("ERROR", "x refusing to create group - invalid name"); return; }
        if (GetFileAttributesW(widen(g_snipdir + "\\" + win_path(g)).c_str())
                != INVALID_FILE_ATTRIBUTES) {
            ui_log("ERROR", "x " + g + " already exists");
            return;
        }
        ensure_group_dirs(g_snipdir, g + "/x");                 // creates every level of g
        ui_log("STATUS", "+ created group " + g);
        push_snippets();
        return;
    }

    if (starts("GROUP_RENAME:")) {
        std::string rest = msg.substr(13);
        size_t nl = rest.find('\n');
        if (nl == std::string::npos) return;
        std::string oldg = rest.substr(0, nl), newg = rest.substr(nl + 1);
        while (!newg.empty() && (newg.back() == '\r' || newg.back() == '\n')) newg.pop_back();
        if (!safe_rel(oldg) || !safe_rel(newg)) {
            ui_log("ERROR", "x refusing to rename group - invalid name");
            return;
        }
        if (inside(oldg, newg)) {
            ui_log("ERROR", "x cannot move a group inside itself");
            return;
        }
        if (!same_file(oldg, newg) &&
            GetFileAttributesW(widen(g_snipdir + "\\" + win_path(newg)).c_str())
                != INVALID_FILE_ATTRIBUTES) {
            ui_log("ERROR", "x " + newg + " already exists");
            return;
        }
        // Renaming the directory renames the group; every member's path
        // follows, and a different parent makes it a MOVE. .order still holds
        // the old paths and simply drops them, so the group lands at the
        // bottom until reordered once.
        // No "/x" here: ensure_group_dirs stops at the last separator, so
        // this makes the PARENT chain only. Creating the destination first
        // makes MoveFile fail -- it will not move onto an existing directory.
        ensure_group_dirs(g_snipdir, newg);
        if (MoveFileW(widen(g_snipdir + "\\" + win_path(oldg)).c_str(),
                      widen(g_snipdir + "\\" + win_path(newg)).c_str()))
            ui_log("STATUS", move_or_rename("group", oldg, newg));
        else
            ui_log("ERROR", "x could not move group " + oldg);
        push_snippets();
        return;
    }

    if (starts("GROUP_UNGROUP:")) {
        std::string g = msg.substr(14);
        if (!safe_rel(g)) { ui_log("ERROR", "x refusing to ungroup - invalid name"); return; }

        // Members move up into the PARENT, not to the root -- ungrouping
        // "a/b" leaves its contents in "a". Deliberately not called "delete":
        // no folder operation in here should be able to destroy a snippet.
        std::string up  = parent_of(g);
        std::string dst = g_snipdir + (up.empty() ? "" : "\\" + win_path(up));
        std::string src = g_snipdir + "\\" + win_path(g);

        int moved = 0, clashed = 0;
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(widen(src + "\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            std::vector<std::string> names;
            do {
                std::string n = narrow(fd.cFileName);
                if (n == "." || n == "..") continue;
                names.push_back(n);                  // collected first: moving
            } while (FindNextFileW(h, &fd));         // during the walk is unsafe
            FindClose(h);

            // Subdirectories move too, so ungrouping a group that holds groups
            // promotes them rather than refusing.
            for (const std::string& n : names) {
                // .project describes the folder being dissolved, so it dies
                // with it. Promoting it would mark the PARENT as a project --
                // and when the parent is the snippets root it leaves an inert
                // marker there that the NEXT ungroup then collides with,
                // which is what used to keep the folder alive.
                if (n == ".project") {
                    DeleteFileW(widen(src + "\\" + n).c_str());
                    continue;
                }
                std::string to = dst + "\\" + n;
                if (GetFileAttributesW(widen(to).c_str()) != INVALID_FILE_ATTRIBUTES) {
                    ui_log("ERROR", "x " + n + " already exists in the parent - left in place");
                    clashed++;
                    continue;
                }
                if (MoveFileW(widen(src + "\\" + n).c_str(), widen(to).c_str())) moved++;
            }
        }
        // Only succeeds once empty, which is the wanted behaviour: anything that
        // could not move keeps the group alive rather than being orphaned.
        bool gone = RemoveDirectoryW(widen(src).c_str()) != 0;
        if (clashed) {
            // Each clash already logged its own line; the folder surviving is
            // the consequence of those, not a separate failure.
        } else if (gone) {
            ui_log("STATUS", "+ ungrouped " + g + " (" + std::to_string(moved) + " moved)");
        } else {
            // Saying "ungrouped" for a folder still sitting there is the same
            // class of lie as reporting a save that did not happen.
            ui_log("ERROR", "x moved " + std::to_string(moved) + " out of " + g +
                            " but could not remove the folder");
        }
        push_snippets();
        return;
    }

    if (starts("SNIP_RENAME:")) {
        std::string rest = msg.substr(12);
        size_t nl = rest.find('\n');
        if (nl == std::string::npos) return;
        std::string oldn = rest.substr(0, nl), newn = rest.substr(nl + 1);
        while (!newn.empty() && (newn.back() == '\r' || newn.back() == '\n')) newn.pop_back();
        if (!safe_rel(oldn) || !safe_rel(newn)) {
            ui_log("ERROR", "x refusing to rename - invalid name");
            return;
        }
        // Renaming across groups is a move, so the destination group may not
        // exist yet. This is also how a snippet leaves a group: rename it from
        // "grp/x.cpp" to "x.cpp".
        ensure_group_dirs(g_snipdir, newn);
        std::string from = g_snipdir + "\\" + win_path(oldn);
        std::string to   = g_snipdir + "\\" + win_path(newn);
        if (!same_file(oldn, newn) &&
            GetFileAttributesW(widen(to).c_str()) != INVALID_FILE_ATTRIBUTES) {
            ui_log("ERROR", "x " + newn + " already exists");
            return;
        }
        if (MoveFileW(widen(from).c_str(), widen(to).c_str()))
            ui_log("STATUS", move_or_rename("snippet", oldn, newn));
        else
            ui_log("ERROR", "x could not rename " + oldn);
        push_snippets();
        return;
    }

    if (starts("WIN:")) {
        std::string what = msg.substr(4);
        if (!g_hwnd) return;
        // App mode: the window sits over the game. Minimise and maximise are
        // desktop ideas that make no sense there (the controls are hidden,
        // but the titlebar double-click still sends ZOOM); the drag stays --
        // it is the whole point of the real window.
        if (overlay_app_mode() && (what == "MIN" || what == "ZOOM")) return;
        // ...and CLOSE means BACK TO THE BADGE, not quit. In HUD mode the
        // window is a VIEW of the console, so closing the view returning to
        // the badge is the ordinary reading; killing the process on one
        // misclick, mid-game, is not. Quitting moved to the badge's
        // right-click menu, where it costs two deliberate steps.
        if (overlay_app_mode() && what == "CLOSE") { app_close_begin(true); return; }
        if      (what == "CLOSE") PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        else if (what == "MIN")   ShowWindow(g_hwnd, SW_MINIMIZE);
        else if (what == "ZOOM")  ShowWindow(g_hwnd, IsZoomed(g_hwnd) ? SW_RESTORE : SW_MAXIMIZE);
        else if (what == "DRAG")  { ReleaseCapture(); SendMessageW(g_hwnd, WM_SYSCOMMAND, SC_MOVE | 0x0002, 0); }
        else if (what.compare(0, 5, "SIZE:") == 0) {
            // The WM_NCHITTEST resize borders are unreachable: WebView2's
            // child window owns every client pixel, so the parent is never
            // asked to hit-test and dragging an edge did nothing. So the
            // edges are grab strips in the page that hand the drag back here
            // -- the titlebar's trick -- and Windows runs its own modal
            // resize loop, which is what makes WM_GETMINMAXINFO's minimum
            // apply.
            if (IsZoomed(g_hwnd)) return;            // nothing to drag when maximised
            std::string d = what.substr(5);
            int edge = 0;
            if      (d == "L")  edge = WMSZ_LEFT;        else if (d == "R")  edge = WMSZ_RIGHT;
            else if (d == "T")  edge = WMSZ_TOP;         else if (d == "B")  edge = WMSZ_BOTTOM;
            else if (d == "TL") edge = WMSZ_TOPLEFT;     else if (d == "TR") edge = WMSZ_TOPRIGHT;
            else if (d == "BL") edge = WMSZ_BOTTOMLEFT;  else if (d == "BR") edge = WMSZ_BOTTOMRIGHT;
            if (edge) { ReleaseCapture(); SendMessageW(g_hwnd, WM_SYSCOMMAND, SC_SIZE + edge, 0); }
        }
        return;
    }
}

// ----------------------------- frameless window -----------------------------

static WNDPROC g_orig = nullptr;
// The main window's close handler brings the snippet window down first
// (snip_close, snipwin.h), or the library's shared window refcount stays
// > 0 and never PostQuitMessages.


// Centres a window of a given PHYSICAL size; defined below, next to
// centre_window, which is now a thin logical-units wrapper over it.
static void centre_phys(HWND h, int w, int hgt);
// The one writer of window.txt; defined below with the rest of the geometry.
static void write_window_rect(int x, int y, int w, int h);

static void save_window_rect(HWND h) {
    if (!h) return;
    // Deleted only when BOTH are off, so the file cannot outlive the
    // preference entirely; otherwise write_window_rect puts the sentinel in
    // for whichever half is switched off.
    if (!g_remember_pos && !g_remember_size) {
        DeleteFileW(widen(window_rect_path()).c_str());
        return;
    }
    // Maximised or minimised is a STATE, not a rect. Saving the restored size
    // means unmaximising later gives back something sensible rather than a
    // full-screen "window" that cannot be told from being maximised.
    WINDOWPLACEMENT wp{};
    wp.length = sizeof wp;
    if (!GetWindowPlacement(h, &wp)) return;
    RECT r = wp.rcNormalPosition;
    write_window_rect(r.left, r.top, r.right - r.left, r.bottom - r.top);
}

// ONE GEOMETRY, ONE STORE, ONE WRITER.
//
// It is one window either way -- app mode and hud mode differ in chrome, owner
// and z-order, not in where the window is -- so how big you made it and where
// you put it is a single fact, and window.txt holds it in screen coordinates
// from both modes. The in-game position used to live in overlay_app.txt as a
// client-relative anchor; two stores that had to agree is what made position
// disagree between the modes, and no amount of mirroring fixed it (five
// attempts, docs/window-geometry.md). The anchor still exists so the window can
// follow the client, but it is DERIVED at the open and never written down.
//
// Called from whichever mode ended the move/size, always with the full rect.
// THE FILE SAYS WHAT IT MEANS; THE FLAGS ARE NOT READABLE WHEN IT IS READ.
//
// restore_window_rect runs from WinMain while the window is being created --
// BEFORE the page has loaded and pushed the settings over -- so at that moment
// both switches are still their compiled defaults and consulting them there
// restores whatever the defaults say. So each half writes a SENTINEL when its
// switch is off: -1 -1 for "no position wanted", 0 0 for "no size wanted".
// Restore needs no flag, and both halves get the same treatment (the size half
// used to trust the flag and silently ignored the switch on every cold start).
static void write_window_rect(int x, int y, int w, int h) {
    std::ofstream o(window_rect_path(), std::ios::binary);
    if (g_remember_pos)  o << x << " " << y << " ";
    else                 o << -1 << " " << -1 << " ";
    if (g_remember_size) o << w << " " << h << "\n";
    else                 o << 0 << " " << 0 << "\n";
}

void ui_geometry_changed(int x, int y, int w, int h) {
    if (w < 200 || h < 150) return;               // absurd; not a real rect
    write_window_rect(x, y, w, h);
}

void ui_geometry_forget(bool pos, bool size) {
    (void)pos; (void)size;   // the switches themselves decide; see write_window_rect
    // Both off: the file must not outlive the preference at all.
    if (!g_remember_pos && !g_remember_size) {
        DeleteFileW(widen(window_rect_path()).c_str());
        return;
    }
    // Rewrite through the same writer, so the half that is still wanted keeps
    // its value and the half just switched off becomes its sentinel now rather
    // than at the next close.
    RECT r{};
    WINDOWPLACEMENT wp{}; wp.length = sizeof wp;
    if (g_hwnd && GetWindowPlacement(g_hwnd, &wp)) r = wp.rcNormalPosition;
    else {
        std::ifstream f(window_rect_path());
        int x, y, w, h;
        if (!(f >> x >> y >> w >> h)) return;
        r = RECT{ x, y, x + w, y + h };
    }
    write_window_rect(r.left, r.top, r.right - r.left, r.bottom - r.top);
}

static bool restore_window_rect(HWND h) {
    std::ifstream f(window_rect_path());
    int x, y, w, hgt;
    if (!(f >> x >> y >> w >> hgt)) return false;

    // NO FLAG IS READ HERE -- the file's two sentinels say what was wanted,
    // and the flags are still their compiled defaults this early (see
    // write_window_rect). A size below the floor is the "no size wanted"
    // sentinel and a corrupt file both at once, and the answer is the same.
    const bool want_pos  = !(x < 0 && y < 0);
    const bool want_size = (w >= 200 && hgt >= 150);
    if (!want_pos && !want_size) return false;   // the caller centres at default

    // HALF A RECT IS STILL A RESTORE, and returning false for a half would
    // throw away the half that was wanted -- the caller's fallback centres at
    // the DEFAULT size, which is not what "remember the size but not the
    // position" asked for. So each half is filled in here instead.
    if (!want_size) {
        UINT dpi = dpi_for(h);
        w   = scaled(kOpenW, dpi);
        hgt = scaled(kOpenH, dpi);
    }
    // Centred rather than left where it was: forgetting the position has to
    // mean the same thing as never having had one.
    if (!want_pos) { centre_phys(h, w, hgt); return true; }

    // Must still be on a monitor that exists. A rect saved on a second screen
    // that has since been unplugged would put the window off in space with no
    // way to drag it back.
    RECT r{ x, y, x + w, y + hgt };
    HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
    if (!mon) return false;

    SetWindowPos(h, nullptr, x, y, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
}

// Centre on the monitor the cursor is on, clamped to that monitor's WORK area.
// Two separate failures to avoid: opening in a corner, and opening at a size the
// screen cannot show -- 1180x780 does not fit a 1366x768 laptop, and rcWork
// rather than rcMonitor keeps it off the taskbar.
// Takes PHYSICAL pixels, because a remembered size is already physical and
// scaling it a second time would shrink the window on every restore.
static void centre_phys(HWND h, int w, int hgt) {
    POINT cur{};
    GetCursorPos(&cur);
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(MonitorFromPoint(cur, MONITOR_DEFAULTTOPRIMARY), &mi)) return;

    UINT dpi = dpi_for(h);
    RECT wa = mi.rcWork;
    int aw = wa.right - wa.left, ah = wa.bottom - wa.top;

    // A margin so it opens as a window rather than a takeover...
    int gap = scaled(48, dpi);
    if (w   > aw - gap) w   = aw - gap;
    if (hgt > ah - gap) hgt = ah - gap;
    // ...but being usable beats being polite: the minimum wins over the margin,
    // even if that means overhanging a very small screen.
    if (w   < scaled(kMinW, dpi)) w   = scaled(kMinW, dpi);
    if (hgt < scaled(kMinH, dpi)) hgt = scaled(kMinH, dpi);

    SetWindowPos(h, nullptr,
                 wa.left + (aw - w) / 2, wa.top + (ah - hgt) / 2,
                 w, hgt, SWP_NOZORDER);
}

// The same thing in LOGICAL units, which is what the default open size is in.
static void centre_window(HWND h, int w, int hgt) {
    UINT dpi = dpi_for(h);
    centre_phys(h, scaled(w, dpi), scaled(hgt, dpi));
}

static bool g_in_sizemove = false;   // inside the modal move/size loop
// hud_tick (overlay.cpp) consults it: the tick must not re-glue the app
// window to its anchor while the user is dragging it.
bool ui_in_sizemove() { return g_in_sizemove; }

static LRESULT CALLBACK subclass(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_ENTERSIZEMOVE:
            // The tick must not re-glue the app window to its anchor while
            // the user is dragging it -- the anchor is re-derived at the end.
            g_in_sizemove = true;
            break;
        case WM_EXITSIZEMOVE:
            g_in_sizemove = false;
            if (overlay_app_mode()) {
                app_drag_settled();          // folds size AND anchor, then syncs
            } else {
                // APP MODE. app_drag_settled is the in-game path and returns
                // immediately here, so without this the geometry reached the
                // store only when the console was closed -- move or resize,
                // switch to hud, and it opened at the old rect.
                //
                // THE RESTORED RECT, not the live one. Dragging a window to a
                // screen edge ends in this same modal loop, so GetWindowRect
                // here hands back the SNAPPED rect and the remembered size
                // becomes "half the monitor" -- which is a state the user
                // asked Windows for, not a size they chose for the console.
                // rcNormalPosition is what save_window_rect already stores,
                // so this only makes the two writers agree.
                WINDOWPLACEMENT wp{};
                wp.length = sizeof wp;
                if (GetWindowPlacement(h, &wp)) {
                    RECT r = wp.rcNormalPosition;
                    ui_geometry_changed(r.left, r.top,
                                        r.right - r.left, r.bottom - r.top);
                }
            }
            break;
        case WM_CLOSE: {
            // Saved here rather than at WM_DESTROY: by then the window is
            // already being torn down and GetWindowPlacement reports whatever
            // is left of it.
            //
            // In APP MODE the window is parked over the game at the overlay
            // anchor's rect -- saving that as the desktop rect would teleport
            // the next launch's desktop window into wherever the game was.
            // Restore the desktop placement first (hidden; the window is
            // about to go anyway), then save the honest rect.
            //
            // CLOAK FIRST so the app VANISHES instead of blinking behind the
            // game on the way out (Fault C, 2026-09-04). Quitting from the
            // flyout runs app_abort_restore (which hides, then re-shows the
            // window at the desktop rect for the save) and then DestroyWindow
            // -- and Windows plays its close/hide FADE on a visible top-level
            // window, dissolving the app into the game over ~220ms with a
            // one-frame uncovered flash first (measured with DXGI duplication:
            // app rect 121215 -> ffffff over 220ms, the user's "the app
            // blinks"). A cloaked window is composed to NOTHING immediately,
            // with no fade and no flash, and GetWindowPlacement still reports
            // its true rect for the save below. DWMWA_CLOAK = 13.
            // ASKED BEFORE app_abort_restore, which clears the flag -- and it
            // must be "is the app in the game RIGHT NOW", not overlay_pref().
            // The preference merely says app-as-HUD is available; keying the
            // save on it meant that with the setting on, the DESKTOP rect was
            // never saved either, so nothing was remembered at all.
            const bool was_in_game = overlay_app_mode();
            if (was_in_game) {
                BOOL cloak = TRUE;
                DwmSetWindowAttribute(h, 13 /*DWMWA_CLOAK*/, &cloak, sizeof cloak);
                app_abort_restore();
            }
            // ALWAYS, EITHER MODE. app_abort_restore above leaves the
            // window at the rect it occupied in the game, and under one
            // geometry that IS the geometry -- the same screen rectangle the
            // desktop window would have had. Skipping the save here was right
            // only while the in-game position had a store of its own.
            save_window_rect(h);
            // User closed the console. Take the snippet window down first so
            // the webview library's shared window refcount reaches 0 when the
            // main is destroyed and terminate() fires PostQuitMessage. Without
            // this the snippet window keeps the process alive after the main
            // is gone -- visible as an orphaned HUD with no console.
            snip_close();
            // The in-game panel is a second webview window and counts the same
            // way. Left alive it outlives the console exactly as described
            // above, which is not hypothetical -- it happened.
            hud_destroy();
            // And drop the low-level mouse hook explicitly. Leaving it to
            // process exit is a window in which the cursor stutters; leaving it
            // to overlay_tick is worse, because the tick does not run during
            // shutdown at all.
            overlay_release_hooks();
            break;
        }
        // WM_NCACTIVATE is suppressed; WM_NCPAINT is NOT. The frame on left,
        // right and bottom is real (see WM_NCCALCSIZE), and swallowing its paint
        // leaves that margin unpainted -- which showed up as a white band down
        // both sides and along the bottom.
        case WM_NCACTIVATE: return TRUE;
        case WM_NCCALCSIZE:
            if (w && !IsZoomed(h)) {
                // Keep the real frame on left/right/bottom, reclaim only the
                // top. The kept frame is the invisible ~8px resize margin -- it
                // sits OUTSIDE the client area, so WebView2's child window
                // cannot swallow it and Windows hit-tests those edges natively.
                // Returning 0 would leave nothing to grab.
                //
                // The asymmetry is forced: DWM composites left/right/bottom
                // transparently but the top OPAQUELY, so frame kept up there is
                // a solid band above the titlebar. WS_CAPTION, WM_NCPAINT and
                // DWMWA_CAPTION_COLOR were all tried -- don't retry.
                NCCALCSIZE_PARAMS* p = (NCCALCSIZE_PARAMS*)l;
                RECT before = p->rgrc[0];
                LRESULT r = CallWindowProcW(g_orig, h, m, w, l);
                p->rgrc[0].top = before.top;
                return r;
            }
            if (w) {
                // Maximising expands the window to the monitor rect PLUS the
                // frame thickness (8px a side on this machine). Normally the
                // non-client frame absorbs that overhang; this window has no
                // frame, so it runs off-screen and clips the content at every
                // edge -- the close button, the sidebar, the bottom bar.
                // Clamp the client rect to the work area instead.
                if (IsZoomed(h)) {
                    NCCALCSIZE_PARAMS* p = (NCCALCSIZE_PARAMS*)l;
                    MONITORINFO mi{};
                    mi.cbSize = sizeof mi;
                    HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
                    if (GetMonitorInfoW(mon, &mi)) {
                        RECT work = mi.rcWork;

                        // With an auto-hide taskbar rcWork covers the whole
                        // monitor, and a window flush to that edge stops the
                        // bar ever revealing itself. Leave it one pixel.
                        APPBARDATA abd{};
                        abd.cbSize = sizeof abd;
                        if (SHAppBarMessage(ABM_GETSTATE, &abd) & ABS_AUTOHIDE) {
                            const UINT edges[4] = { ABE_BOTTOM, ABE_TOP, ABE_LEFT, ABE_RIGHT };
                            for (UINT e : edges) {
                                APPBARDATA q{};
                                q.cbSize = sizeof q;
                                q.uEdge  = e;
                                q.rc     = mi.rcMonitor;
                                if (!SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &q)) continue;
                                if      (e == ABE_BOTTOM) work.bottom -= 1;
                                else if (e == ABE_TOP)    work.top    += 1;
                                else if (e == ABE_LEFT)   work.left   += 1;
                                else                      work.right  -= 1;
                            }
                        }
                        p->rgrc[0] = work;
                    }
                }
                return 0;
            }
            break;
        // WM_NCHITTEST is deliberately NOT handled.
        //
        // Hand-rolling it -- hit-testing a 6px band -- fails twice over. It
        // never fires, because WebView2's child window owns every client pixel.
        // And with non-client region support enabled it is actively harmful:
        // WebView2 reports the CSS app-region areas as caption, and a handler
        // answering HTCLIENT overrides that -- a titlebar with no system menu.
        //
        // Letting it through means the caption behaviours are Windows' own.
        case WM_SIZE: {
            // Tell the UI when the maximise state flips so the button can show
            // a restore glyph. Driven from here rather than from the click so
            // it stays correct for Win+Up, snapping and double-click too.
            static bool was_zoomed = false;
            bool z = IsZoomed(h) != 0;
            if (z != was_zoomed) {
                was_zoomed = z;
                ui_post(std::string("ZOOM:") + (z ? "1" : "0"));
            }
            break;
        }
        // No WM_NCHITTEST handler, and Snap Layouts is not reachable.
        //
        // The Win11 flyout is driven purely by answering HTMAXBUTTON from
        // WM_NCHITTEST -- there is no API to request it -- and that hit test
        // never arrives for anything the PAGE owns (measured: no
        // WM_NCMOUSEMOVE over the titlebar, HTRIGHT immediately at the real
        // border). WebView2 does not delegate non-client behaviour, it
        // IMPLEMENTS it in its own child window -- we get exactly the
        // behaviours it chose, and Snap Layouts is not one of them. The
        // frame's own edges still reach us, which is why resizing is native.

        case WM_WINDOWPOSCHANGED:
            // The z-move watch (overlay.h): who moves the APP window. Every
            // SetWindowPos we make on it lives in overlay.cpp and is logged
            // there; a raise arriving with no zlog line is exactly what this
            // catches, with the caller's stack. Falls through to the library's
            // proc -- observation only.
            zwatch_poschanged("APP", h, (const void*)l);
            // A z-order restack of the app that WE did not make -- its own
            // activation on a click, or USER32's owner enforcement -- carries
            // the owner group (the client) above the overlay. Re-glue the
            // overlay under the app now, in this message, before the frame is
            // composed. No-op for our own moves and mid-choreography.
            app_zorder_changed((const void*)l);
            break;
        case WM_TIMER:
            // Last resort: show the window even if the page never reported that
            // it painted. Runs on the UI thread, which ShowWindow/SetForeground
            // want, and which a worker thread could not have given them.
            if (w == 1) { KillTimer(h, 1); reveal_window(true); return 0; }
            // The in-game panel: follows the client's rect and holds its
            // z-order. ~30 Hz, and almost every tick does nothing but three
            // window queries -- SetWindowPos only fires when something moved.
            if (w == 2) { hud_tick(); overlay_tick(); health_tick(); identity_tick(); return 0; }
            break;
        case WM_GETMINMAXINFO: {
            // This is the one that actually stops the drag. The webview's MIN
            // hint alone does not: it is applied when set, not enforced against
            // the user pulling an edge.
            UINT dpi = dpi_for(h);
            ((MINMAXINFO*)l)->ptMinTrackSize = { scaled(kMinW, dpi), scaled(kMinH, dpi) };
            return 0;
        }

        case WM_DPICHANGED: {
            // Dragged onto a monitor with a different scale factor. Windows
            // works out where the window should go and hands it over in lParam;
            // ignoring it leaves the window the wrong size on the new display
            // until something else resizes it.
            const RECT* suggested = (const RECT*)l;
            if (suggested) {
                SetWindowPos(h, nullptr,
                             suggested->left, suggested->top,
                             suggested->right  - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }
    }
    return CallWindowProcW(g_orig, h, m, w, l);
}

// ---- native non-client regions ---------------------------------------------
// The proper fix for a custom titlebar. By default WebView2's child window
// covers the whole client area and swallows every mouse message, so the
// host's WM_NCHITTEST is never consulted -- hence the SC_MOVE drag fake, the
// page resize grips, and no system menu or Snap Layouts.
// IsNonClientRegionSupportEnabled makes WebView2 report the CSS app-region
// areas back as non-client, so Windows treats them as a real titlebar.
//
// Needs ICoreWebView2Settings9 (SDK 1.0.2210.55+, hence the bump from
// 1.0.1150.38). Older runtimes simply fail the QueryInterface, which is why
// this returns a bool rather than asserting -- the page-driven fallbacks
// stay in place and nothing breaks.
static bool enable_nonclient_regions(webview::webview& wv) {
    auto ctrl = wv.browser_controller();
    if (!ctrl.ok() || !ctrl.value()) return false;

    auto* controller = static_cast<ICoreWebView2Controller*>(ctrl.value());
    ICoreWebView2* core = nullptr;
    if (FAILED(controller->get_CoreWebView2(&core)) || !core) return false;

    // The IID is written out rather than taken from IID_PPV_ARGS. That macro
    // goes through __uuidof, and MinGW only knows an interface's uuid if the
    // header declared it with __CRT_UUID_DECL -- which WebView2.h does not do
    // for the newer interfaces, so it links with an undefined reference to
    // __mingw_uuidof<ICoreWebView2Settings9>. The value is the MIDL_INTERFACE
    // attribute on the declaration in WebView2.h.
    static const GUID kSettings9 =
        { 0x0528a73b, 0xe92d, 0x49f4, { 0x92, 0x7a, 0xe5, 0x47, 0xdd, 0xda, 0xa3, 0x7d } };

    ICoreWebView2Settings* settings = nullptr;
    bool ok = false;
    if (SUCCEEDED(core->get_Settings(&settings)) && settings) {
        ICoreWebView2Settings9* s9 = nullptr;
        if (SUCCEEDED(settings->QueryInterface(kSettings9, (void**)&s9)) && s9) {
            ok = SUCCEEDED(s9->put_IsNonClientRegionSupportEnabled(TRUE));
            s9->Release();
        }
        settings->Release();
    }
    core->Release();
    return ok;
}

// The overlay machine -- the overlay window, the in-game panel, docked
// snippet panels' overlay side, the hit-test region, focus/app mode, the
// travel machinery and the hooks -- lives WHOLE in overlay.cpp. See the note
// at the top of that file: it is one subsystem with cross-referential
// invariants and was moved as one unit; do not split it into pieces.

// ----------------------------- snippet home ---------------------------------
//
// Snippets live in %APPDATA%\EclipseConsole\snippets, NOT next to the exe.
// Beside the exe means the live copy sits in build/, which drifts silently from
// any repo copy and loses everything if the exe is moved. One per-user
// location removes both. Nothing ships in this tree -- a fresh install starts
// with an empty sidebar and the user fills it themselves.

// Returns the per-user snippets directory, creating it on first launch.
// Falls back to an exe-adjacent path if %APPDATA% is somehow missing (service
// account, say) -- degraded, but still runnable.
// %APPDATA%\EclipseConsole, created on demand. Everything the app keeps per
// user lives here: the snippets tree, the optional console.log, the remembered
// window rect. Falls back to the exe's own directory if %APPDATA% is missing
// (a service account, say) -- degraded, but still runnable.
//
// Cached: this is called from ui_log, which is on the path of every single log
// line, and re-reading an environment variable there would be silly.
std::string appdata_home() {
    static std::string home;
    if (!home.empty()) return home;

    wchar_t appdata[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    home = (n && n < MAX_PATH) ? narrow(appdata) + "\\EclipseConsole" : ec::self_dir();
    CreateDirectoryW(widen(home).c_str(), nullptr);
    return home;
}

static std::string resolve_snipdir(const std::string& selfdir) {
    wchar_t appdata[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (!n || n >= MAX_PATH) {
        std::string fb = selfdir + "\\snippets";
        CreateDirectoryW(widen(fb).c_str(), nullptr);
        return fb;
    }

    std::string home = appdata_home();
    std::string dir = home + "\\snippets";
    // Unconditional. The watcher opens this path with FindFirstChangeNotification
    // and silently dies if it does not exist -- which on a fresh install would
    // read as the app being broken.
    CreateDirectoryW(widen(dir).c_str(), nullptr);
    return dir;
}


// ----------------------------- entry ----------------------------------------

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
    // First thing, before anything can fault: this app has been dying with
    // heap corruption every few days and WER left nothing usable behind.
    ec::install_crash_handler();
    // The UI thread reads the client every tick (health_tick, identity_tick).
    // Without this its fx_service walks die on the stop-all latch -- see
    // mark_host_thread in eclipse_core.h.
    ec::mark_host_thread();

    // First thing, before webview creates the window: Windows latches a
    // process's DPI awareness at the first window and ignores the request after.
    enable_dpi_awareness();

    std::string selfdir = ec::self_dir();
    g_conf    = selfdir + "\\conf\\offsets_windows.conf";
    g_snipdir = resolve_snipdir(selfdir);

    // Load the offsets NOW, before any enumeration. Otherwise
    // enumerate_candidates -- which runs at the very first do_attach, before
    // ec::attach's load_offsets has fired -- reads through an empty offset
    // table and every field comes out "(unknown)". The picker only started
    // showing real usernames after the first attach.
    ec::preload_offsets(g_conf);

    {   // --run means headless; parsed before anything is created.
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        std::string runfile, conf, logfile;
        bool arm = false;
        for (int i = 1; argv && i < argc; i++) {
            std::string a = narrow(argv[i]);
            if      (a == "--run"  && i + 1 < argc) runfile = narrow(argv[++i]);
            else if (a == "--conf" && i + 1 < argc) conf    = narrow(argv[++i]);
            else if (a == "--log"  && i + 1 < argc) logfile = narrow(argv[++i]);
            else if (a == "--arm")                  arm     = true;
        }
        if (argv) LocalFree(argv);
        if (!runfile.empty())
            return run_cli(runfile, conf.empty() ? g_conf : conf, logfile, arm);
    }

    // Chromium throttles renderers it considers backgrounded or occluded, and
    // a panel that refuses focus by design is backgrounded permanently -- which
    // is what makes it feel like it runs at a few frames a second while the
    // game has focus. Set before ANY webview is created; the loader reads it
    // when the browser process starts, and the process is shared by both.
    SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS",
                            L"--disable-background-timer-throttling "
                            L"--disable-backgrounding-occluded-windows "
                            L"--disable-renderer-backgrounding");

    std::unique_ptr<webview::webview> owner;
    try {
        owner.reset(new webview::webview(false, nullptr));
    } catch (const webview::exception& e) {
        std::string m =
            "Eclipse needs the Microsoft Edge WebView2 Runtime, which does not appear "
            "to be installed.\r\n\r\nGet it from:\r\n"
            "https://developer.microsoft.com/microsoft-edge/webview2/\r\n\r\nDetails: ";
        m += e.what();
        MessageBoxW(nullptr, widen(m).c_str(), L"Eclipse", MB_OK | MB_ICONERROR);
        return 1;
    }

    webview::webview& wv = *owner;
    g_wv = &wv;
    wv.set_title("Eclipse");

    auto wr = wv.window();
    g_hwnd = wr.ok() ? (HWND)wr.value() : nullptr;
    if (g_hwnd) {
        // The taskbar, Alt-Tab and the titlebar take their icon from the
        // WINDOW, not from the executable. The icon compiled into the exe is
        // what Explorer shows for the FILE and for shortcuts -- a different
        // thing, which is why the exe looked right in a folder while Alt-Tab
        // showed the default. webview registers its class without an icon, so
        // it has to be attached here.
        //
        // Both sizes: Windows uses the small one for the titlebar and Alt-Tab
        // and the large one for the switcher preview, and letting it scale one
        // into the other looks worse than handing it the layer drawn for that
        // size. IDI_APPICON is 1; see src/app.rc.
        HINSTANCE inst = GetModuleHandleW(nullptr);
        HICON big = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXICON),
                                      GetSystemMetrics(SM_CYICON), 0);
        HICON sml = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXSMICON),
                                      GetSystemMetrics(SM_CYSMICON), 0);
        if (big) SendMessageW(g_hwnd, WM_SETICON, ICON_BIG,   (LPARAM)big);
        if (sml) SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)sml);

        LONG st = GetWindowLongW(g_hwnd, GWL_STYLE);
        st |= WS_THICKFRAME | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
        SetWindowLongW(g_hwnd, GWL_STYLE, st);
        // Matches --bg-0 so a resize doesn't flash white against the dark UI.
        SetClassLongPtrW(g_hwnd, GCLP_HBRBACKGROUND, (LONG_PTR)CreateSolidBrush(RGB(10, 10, 12)));
        g_orig = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)subclass);

        // Windows 11 rounds every ordinary window, but WM_NCCALCSIZE above
        // removed the frame that would have carried it -- leaving hard 90°
        // corners that look wrong next to every other app. Ask for the rounding
        // explicitly, and give the window a visible edge so it doesn't melt
        // into a dark desktop. Both attributes are Win11-only; on Win10 the
        // calls simply fail and the window stays square, which is correct there.
        // The frame is real again, and DWM paints it from the system theme --
        // so on a light-themed desktop it came out white against a near-black
        // UI. This is what keeps it dark.
        enum { DARK_MODE = 20 };                              // DWMWA_USE_IMMERSIVE_DARK_MODE
        BOOL dark = TRUE;
        DwmSetWindowAttribute(g_hwnd, DARK_MODE, &dark, sizeof dark);

        enum { CORNER_PREFERENCE = 33, BORDER_COLOR = 34 };   // DWMWA_* (Win11)
        DWORD round = 2;                                      // DWMWCP_ROUND
        DwmSetWindowAttribute(g_hwnd, CORNER_PREFERENCE, &round, sizeof round);
        COLORREF edge = RGB(0x2b, 0x2c, 0x35);                // the kit's --line-strong
        DwmSetWindowAttribute(g_hwnd, BORDER_COLOR, &edge, sizeof edge);

        SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }

    bool nonclient = enable_nonclient_regions(wv);

    wv.set_size(kMinW, kMinH, WEBVIEW_HINT_MIN);
    if (g_hwnd) {
        // Positioned while still hidden, so it appears already in place rather
        // than jumping to centre after the fact.
        // A remembered rect wins over centring, if one was saved AND still
        // lands on a monitor that exists -- unplugging the screen it was on
        // would otherwise reopen the window somewhere unreachable.
        if (!restore_window_rect(g_hwnd)) centre_window(g_hwnd, kOpenW, kOpenH);
        SetTimer(g_hwnd, 1, 4000, nullptr);   // see WM_TIMER
        SetTimer(g_hwnd, 2, 16, nullptr);     // the in-game panel's tick
    } else {
        wv.set_size(kOpenW, kOpenH, WEBVIEW_HINT_NONE);
    }

    wv.bind("host_send", [](const std::string&, const std::string& req, void*) {
        // req is a JSON array: ["message"]
        std::string msg;
        size_t a = req.find('['), b = req.rfind(']');
        if (a != std::string::npos && b != std::string::npos && b > a) {
            std::string in = req.substr(a + 1, b - a - 1);
            size_t s = in.find('"'), e = in.rfind('"');
            if (s != std::string::npos && e > s) {
                std::string raw = in.substr(s + 1, e - s - 1);
                for (size_t i = 0; i < raw.size(); i++) {
                    if (raw[i] == '\\' && i + 1 < raw.size()) {
                        switch (raw[++i]) {
                            case 'n':  msg += '\n'; break;
                            case 'r':  msg += '\r'; break;
                            case 't':  msg += '\t'; break;
                            case '"':  msg += '"';  break;
                            case '\\': msg += '\\'; break;
                            case 'u': {
                                if (i + 4 < raw.size()) {
                                    int cp = (int)strtol(raw.substr(i + 1, 4).c_str(), nullptr, 16);
                                    i += 4;
                                    if (cp < 0x80) msg += (char)cp;
                                }
                                break;
                            }
                            default: msg += raw[i]; break;
                        }
                    } else msg += raw[i];
                }
            }
        }
        handle(msg);
        return std::string("null");
    }, nullptr);

    ec::set_log([](const char* line) { ui_log("LOG", line ? line : ""); });
    // Compile finished, the snippet is about to execute: the indicator
    // stops being "building". Fires on the run thread, and push_runstate
    // dispatches, so nothing here touches WebView2 directly.
    ec::set_run_phase([](unsigned) { push_runstate(); });
    ec::set_overlay_mode(overlay_mode_sink);
    ec::set_overlay_data(overlay_data_sink);
    ec::set_window_open   (window_open_sink);
    ec::set_window_html   (window_html_sink);
    ec::set_window_nav    (window_nav_sink);
    ec::set_window_eval   (window_eval_sink);
    ec::set_window_close  (window_close_sink);
    ec::set_window_get_msg(window_get_msg_sink);
    ec::set_window_dock   (window_dock_sink);
    ec::set_window_pass   (window_pass_sink);

    g_nonclient = nonclient;   // reported on READY; see the note there

    // Before the watcher: the first tick after an attach consults it to decide
    // whether the console belongs in the game or in this window.
    overlay_load_pref();

    g_watch_run = true;
    g_watch_thread = CreateThread(nullptr, 0, watch_client, nullptr, 0, nullptr);
    snipwatch_start(g_snipdir);

    wv.navigate("file:///" + selfdir + "/web/index.html");
    wv.run();

    // A looping snippet must be told to leave before teardown, or the process
    // sits here with a thread still reading the game's memory.
    ec::stop_snippet();

    // Overlay window is our own popup HWND -- tear it down explicitly, or it
    // outlives WinMain briefly and paints against a destroyed message pump.
    destroy_overlay_window();
    snip_close();
    hud_destroy();
    overlay_release_hooks();   // before the process goes; see WM_CLOSE

    g_watch_run = false;
    if (g_watch_thread) {
        WaitForSingleObject(g_watch_thread, 3000);
        CloseHandle(g_watch_thread);
        g_watch_thread = nullptr;
    }
    snipwatch_stop();

    ec::detach();
    g_wv = nullptr;
    return 0;
}
