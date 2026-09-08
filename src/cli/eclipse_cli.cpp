// eclipse_cli.cpp — the CLI as a CONSOLE-subsystem program.
//
// Why this exists as a separate binary rather than a flag on the other one: an
// executable has exactly one subsystem, and the two modes want opposite ones.
//
//   eclipse_console.exe  GUI subsystem, so double-clicking the app does not
//                        flash a console window behind it. It can only *borrow*
//                        a console via AttachConsole, and that has consequences:
//                        the shell does not wait for it, so the prompt comes
//                        back while output is still arriving and the two
//                        interleave; and Ctrl+C goes to the shell, never to us.
//
//   eclipse_cli.exe      console subsystem. The shell waits, output is ordered,
//                        the prompt returns when the run ends, and Ctrl+C is a
//                        real console control event delivered to this process.
//
// That last point is the whole reason for the file. The GUI build needed a
// global keyboard hook to fake a stop key; here Ctrl+C simply works, which is
// what anyone running a terminal program expects.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "eclipse_core.h"
// The multi-run registry. The CLI is deliberately SINGLE-run (see
// docs/multi-run.md: one stdin, one Ctrl+C, ordered output), but it still
// registers its one execution as a Run -- see the note at the run site.
#include "core/run.h"

static FILE* g_log = nullptr;

// ---- resolving what to run --------------------------------------------------
// Snippets live in %APPDATA%\EclipseConsole\snippets, which is nowhere near
// wherever the terminal happens to be sitting. Without this, running one means
// pasting an absolute path every time -- the install directory is the LAST
// place a snippet is, and it is exactly where you end up after installing.
//
// Only read here, never created: that tree is the user's own work and nothing
// but their own actions should be making directories in it.
static bool path_exists(const std::string& p) {
    return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static std::string snippets_dir() {
    char appdata[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
    if (!n || n >= MAX_PATH) return "";
    return std::string(appdata) + "\\EclipseConsole\\snippets";
}

// The literal argument is tried FIRST, so an explicit .\foo.cpp or an absolute
// path always wins. This can only ever add a fallback, never redirect a file
// that is really there.
static std::string resolve_runfile(const std::string& given) {
    if (given.empty() || path_exists(given)) return given;
    std::string dir = snippets_dir();
    if (dir.empty()) return given;
    std::string candidate = dir + "\\" + given;
    return path_exists(candidate) ? candidate : given;
}

static void sink(const char* line) {
    const char* s = line ? line : "";
    if (g_log) { fputs(s, g_log); fflush(g_log); }
    fputs(s, stdout);
    fflush(stdout);
}

// Returning TRUE means "handled", which is what stops Windows terminating the
// process. The snippet then leaves through fx_running() and main() returns
// normally -- so its cleanup runs and claimed keys are released. A killed
// process skips all of that.
static BOOL WINAPI on_ctrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        ec::logf("\n  stopping...\n");
        ec::stop_snippet();
        return TRUE;
    }
    return FALSE;
}

int main(int argc, char** argv) {
    ec::install_crash_handler();
    std::string runfile, conf, logfile;
    bool arm = false, check = false, list = false;
    // 0 = any Roblox, the historical default. attach() has always taken a pid
    // for the UI's multi-instance picker; the CLI simply never exposed it, so
    // with two clients open it silently took whichever find_pid() saw first --
    // which is how a whole session got run against an idle menu instance.
    uint32_t pid = 0;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if      (a == "--run"  && i + 1 < argc) runfile = argv[++i];
        else if (a == "--conf" && i + 1 < argc) conf    = argv[++i];
        else if (a == "--log"  && i + 1 < argc) logfile = argv[++i];
        else if (a == "--arm")                  arm     = true;
        else if (a == "--check")                check   = true;
        else if (a == "--list")                 list    = true;
        else if (a == "--pid"  && i + 1 < argc) pid = (uint32_t)strtoul(argv[++i], nullptr, 10);
    }

    // Which clients are running, so --pid has something to name. Cheap enough
    // to be the first thing tried when an attach looks wrong.
    if (list) {
        // Offsets first, or every row is "(unknown)": the usernames and places
        // are READ out of each client, which needs the offset table even though
        // we are not attaching to anything yet.
        if (conf.empty()) conf = ec::self_dir() + "\\conf\\offsets_windows.conf";
        ec::preload_offsets(conf);
        auto cs = ec::enumerate_candidates(true);
        if (cs.empty()) { printf("no RobloxPlayerBeta.exe running\n"); return 1; }
        printf("%-8s %-20s %-14s %s\n", "pid", "player", "place", "placeid");
        for (const auto& c : cs)
            printf("%-8u %-20s %-14s %llu\n", c.pid,
                   c.username.empty() ? "(unknown)" : c.username.c_str(),
                   c.place.empty() ? "-" : c.place.c_str(),
                   (unsigned long long)c.place_id);
        return 0;
    }

    if (runfile.empty() && !check) {
        fprintf(stderr,
            "usage: eclipse_cli --run FILE|FOLDER [--arm] [--pid N] [--conf PATH] [--log PATH]\n"
            "       eclipse_cli --check [--pid N] [--conf PATH]\n"
            "       eclipse_cli --list\n"
            "\n"
            "  FILE    a snippet; a bare body is wrapped in fx_run()\n"
            "  FOLDER  a project: every source in it compiled together\n"
            "  --arm   perform writes instead of logging them\n"
            "  --pid   attach to THIS client. Without it you get whichever\n"
            "          Roblox is found first, which is a coin toss with two open.\n"
            "  --list  the running clients and their pids, for --pid.\n"
            "  --check offset health check: reads known values and asserts they\n"
            "          make sense. Exits 1 if any check fails, so it can gate a\n"
            "          script. Same checks as Settings > Offsets in the UI.\n"
            "\n"
            "A name that is not a path is looked up in your snippets folder, so\n"
            "`--run explorer.cpp` and `--run aimbot` work from any directory.\n"
            "\n"
            "Ctrl+C stops a running snippet.\n");
        return 2;
    }

    // --check attaches, reports, and leaves. No snippet, no compiler, no arm
    // gate to consider: every check is a read.
    if (check) {
        // Same default as the --run path below. Without it --check attaches
        // with an empty conf and reports "no offsets" on a machine where the
        // offsets are sitting right next to the exe.
        if (conf.empty()) conf = ec::self_dir() + "\\conf\\offsets_windows.conf";
        if (!ec::attach(conf)) { fprintf(stderr, "x could not attach\n"); return 1; }
        auto res = ec::health_check();
        int pass = 0, fail = 0, skip = 0;
        for (const auto& c : res) {
            const char* tag = c.status == ec::HealthCheck::Pass ? "+"
                            : c.status == ec::HealthCheck::Fail ? "x" : "-";
            if      (c.status == ec::HealthCheck::Pass) pass++;
            else if (c.status == ec::HealthCheck::Fail) fail++;
            else skip++;
            printf("%s %-34s %s", tag, c.name.c_str(), c.detail.c_str());
            if (c.status == ec::HealthCheck::Fail && !c.offsets.empty())
                printf("   [%s]", c.offsets.c_str());
            printf("\n");
        }
        printf("\n%d passed, %d failed, %d skipped\n", pass, fail, skip);
        ec::detach();
        return fail ? 1 : 0;
    }

    runfile = resolve_runfile(runfile);

    if (conf.empty()) conf = ec::self_dir() + "\\conf\\offsets_windows.conf";
    if (!logfile.empty()) g_log = fopen(logfile.c_str(), "w");

    SetConsoleCtrlHandler(on_ctrl, TRUE);
    ec::set_log(sink);
    ec::set_armed(arm);

    DWORD attr = GetFileAttributesA(runfile.c_str());
    bool is_dir = attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);

    int rc = 1;
    if (ec::attach(conf, false, pid)) {
        const ec::State& s = ec::state();
        ec::logf("eclipse_cli | pid %u | base 0x%llx | %zu offsets | dm %s | writes %s\n",
                 s.pid, (unsigned long long)s.base, s.noffsets,
                 s.dm ? (s.dm_verified ? "ok" : "UNVERIFIED") : "MISSING",
                 arm ? "ARMED" : "dry-run");

        // ONE Run, registered like the UI's. The CLI stays single-run -- this
        // is not a step toward concurrency here -- but without a Run the
        // thread has no t_run, so fx_run_id() answered 0 INSIDE a run and
        // fx_adopt_run(fx_run_id()) was a silent no-op. A snippet that spawns
        // a worker (backdash) would then behave differently depending on which
        // binary launched it, with nothing saying so. Registering costs one
        // slot of a cap that nothing else here competes for.
        size_t cut = runfile.find_last_of("\\/");
        unsigned run_id = core_run_create(
            cut == std::string::npos ? runfile : runfile.substr(cut + 1));
        if (run_id) core_run_set_current(run_id);

        if (is_dir) {
            rc = ec::run_project(runfile) ? 0 : 1;
        } else {
            std::ifstream f(runfile, std::ios::binary);
            if (!f) {
                ec::logf("x cannot open %s\n", runfile.c_str());
                // Name the other place too: "cannot open explorer.cpp" while
                // standing in the install directory reads as a broken install
                // rather than as having looked in the wrong folder.
                std::string sd = snippets_dir();
                if (!sd.empty() && runfile.find(sd) == std::string::npos)
                    ec::logf("    not here, and not in %s\n", sd.c_str());
                rc = 2;
            } else {
                std::string body((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
                if (body.find_first_not_of(" \t\r\n") == std::string::npos) {
                    ec::logf("empty snippet\n");
                } else {
                    rc = ec::run_snippet(body) ? 0 : 1;
                }
            }
        }

        if (run_id) { core_run_clear_current(); core_run_destroy(run_id); }
    }

    ec::detach();
    if (g_log) { fclose(g_log); g_log = nullptr; }
    return rc;
}
