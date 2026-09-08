// eclipse_core.h — what the UI (or a CLI front end) drives.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ec {

// Log sink. Everything the console prints — including a snippet's own printf,
// which the UI redirects — arrives here. Set before attach() to catch startup.
typedef void (*LogFn)(const char* line);
void set_log(LogFn fn);

// Write a crash record to %APPDATA%\EclipseConsole\crash.txt if the process
// dies unexpectedly. Call once, first thing in main/WinMain. Additive: WER
// still files its report and the process still dies the same way -- this only
// leaves a note behind, because a fault bucket and an ntdll offset were never
// enough to find the bug that prompted it. See crash.cpp for what it cannot
// catch.
void install_crash_handler();
void logf(const char* fmt, ...);

// Overlay sinks. Optional -- the GUI binary wires these to a page-side
// renderer; the CLI binary leaves both null and fx_overlay_* become no-ops.
//
// Two sinks so the two flows can be handled differently: mode changes are
// rare and MUST deliver (an on/off command dropped by conflation would leave
// the window transparent forever); data updates are 10 Hz and stale ones
// SHOULD be dropped.
// Run phase. A run is BUILDING from the moment it is asked for until the
// snippet actually starts executing; this fires once, at that hand-over.
// The UI shows the two apart -- compiling a large project is seconds of
// "running" that has not run anything yet.
// Carries the run id, so the UI can flip the RIGHT run in its list from
// "building" to "running" when several are compiling at once.
typedef void (*RunPhaseFn)(unsigned run_id);
void set_run_phase(RunPhaseFn fn);

typedef void (*OverlayModeFn)(int on);
typedef void (*OverlayDataFn)(const char* json);
void set_overlay_mode(OverlayModeFn fn);
void set_overlay_data(OverlayDataFn fn);

// Snippet-driven floating window. A running snippet can open ONE window at a
// time and drive it with HTML. Backed by a second WebView2 instance, so the
// snippet can render arbitrary DOM, run JS, and receive messages from the
// page. fx_window_text is a convenience wrapper -- it escapes the string and
// pushes it as a <pre> inside the page.
//
// get_msg pops one page-to-snippet message off a FIFO queue populated by
// JS calling window.eclipse_send(...). Sync, so a snippet's poll loop is:
//   while (auto m = fx_window_get_msg()) { handle(m); }
//
// CLI binary leaves all null and fx_window_* become no-ops there.
//
// run is the CALLING run's id (multi-run phase 2: one window per run). The
// export resolves it from its own thread's run before handing off, because
// the sinks dispatch to the UI thread where the current-run TLS means
// nothing. 0 is the legacy/un-adopted id and still gets a window.
typedef void (*WindowOpenFn)(unsigned run, const char* title, int w, int h);
typedef void (*WindowHtmlFn)(unsigned run, const char* html);
typedef void (*WindowNavFn)(unsigned run, const char* url);
typedef void (*WindowEvalFn)(unsigned run, const char* js);
typedef void (*WindowCloseFn)(unsigned run);
typedef bool (*WindowGetMsgFn)(unsigned run, std::string* out);
// Docking turns that same window into an IN-GAME panel: glued to the attached
// client, refusing focus, no caption, no taskbar entry. One call rather than a
// second window API, because everything else about it -- html, eval, messages,
// close -- is identical whether it floats or is docked. THE DOCK IS SINGULAR
// across runs (first-come, refused by name); the windows are per run.
typedef void (*WindowDockFn)(unsigned run, int mode, int corner, int dx, int dy);
typedef void (*WindowPassFn)(unsigned run, int on);
void set_window_open(WindowOpenFn fn);
void set_window_html(WindowHtmlFn fn);
void set_window_nav(WindowNavFn fn);
void set_window_eval(WindowEvalFn fn);
void set_window_close(WindowCloseFn fn);
void set_window_get_msg(WindowGetMsgFn fn);
void set_window_dock(WindowDockFn fn);
void set_window_pass(WindowPassFn fn);

struct State {
    uint32_t    pid          = 0;
    uint64_t    base         = 0;   // RobloxPlayerBeta.exe module base
    uint64_t    dm           = 0;   // resolved DataModel
    bool        dm_verified  = false;
    size_t      noffsets     = 0;
    bool        armed        = false;
    // How the process handle was obtained. False means we opened the client
    // ourselves, which is the detectable path -- reported on the attach line so
    // the guarantee is visible without a log entry saying "the normal thing
    // happened" on every attach.
    bool        via_crash_handler = false;

    // Set when client_version is known AND differs from what the conf is for.
    // Running anything in that state is refused: every read would be from the
    // wrong offset, and an armed write would go to an address that was never
    // right. That is not a broken feature, it is a corrupted client.
    bool        offsets_stale = false;
};

const State& state();

// THE STRINGS ARE NOT MEMBERS OF State, deliberately -- see the long note at
// their definition in eclipse_console.cpp. Members of a global handed out as
// `const State&` were being reassigned on the attach path while another thread
// read them, which frees a heap block out from under a reader: corruption, not
// a stale value. These return references to IMMUTABLE, never-freed snapshots,
// so a reader on any thread is safe for as long as it likes.
//
//   dm_class        what the DataModel candidate actually classed as
//   conf_path       the offsets file this attach loaded
//   conf_version    the ClientVersion line from the conf, if it has one
//   client_version  what the ATTACHED client is, from its exe path; empty when
//                   it could not be determined (a Microsoft Store install has
//                   no version-<hash> directory, so this is best-effort)
const std::string& dm_class();
const std::string& conf_path();
const std::string& conf_version();
const std::string& client_version();

// Find the client, open a handle, locate the module base, load the conf and
// resolve the DataModel. Safe to call repeatedly — it re-attaches.
//
// All-or-nothing: on any failure the state is rolled back so pid reads 0. A
// half-attached state (pid set, base 0) is worse than none, because callers
// treat a non-zero pid as "attached" and stop retrying.
//
// `quiet` suppresses the diagnostics for poll-driven retries.
// `target_pid` picks a specific instance for multi-instance users. 0 means
// "any Roblox" (current behaviour when only one is running).
bool attach(const std::string& conf_path, bool quiet = false, uint32_t target_pid = 0);
void detach();

// A running Roblox process, enriched with what we could read out of it.
// See enumerate_candidates: the picker uses this to distinguish two clients
// by account + place rather than by pid alone.
//
// `readable == false` means we couldn't get a handle at all (different
// integrity level, protected process). The pid is still reported so it shows
// in the list -- the row is just "pid N (access denied)" instead of the full
// account label.
struct Candidate {
    uint32_t    pid          = 0;
    std::string username;                 // Player.LocalPlayer's Instance.Name
    std::string display_name;             // Player.LocalPlayer's DisplayName
    std::string place;                    // DataModel's Instance.Name -- "LuaApp"
                                          // for the home shell, "Ugc" for a game
    uint64_t    place_id     = 0;         // DataModel.PlaceId. Non-zero in-game;
                                          // roblox.com/games/<place_id> resolves
    uint64_t    user_id      = 0;
    int         monitor      = 0;         // 1-based; 0 if no window
    bool        readable     = false;
};

// `explain` logs, per candidate it could not name, which step failed. Only the
// call that is about to SHOW the picker should pass true: the background
// refresh runs every tick, and a client that is still starting is the expected
// state rather than news.
std::vector<Candidate> enumerate_candidates(bool explain = false);

// Load the offsets conf without attaching to any client. Idempotent -- does
// nothing if the offsets are already in memory. Called by the UI at startup
// so enumerate_candidates() can read usernames/places on a fresh session,
// before the user has picked a client to attach to.
void preload_offsets(const std::string& conf_path);

void set_armed(bool on);

// Does the arm state survive a detach? Default false: a detach/attach cycle is
// how you move to a different client, so the safe answer is that the next
// session starts disarmed. Only the flag's lifetime changes -- disarmed still
// refuses every write.
void set_keep_armed(bool on);
bool keep_armed(void);

// Host-side settings, pushed from the page over SETTINGS: and applied here.
// Kept as individual setters rather than a struct so the page and the host do
// not have to agree on a layout -- the wire format is key=value, and an
// unknown key is simply ignored on either side.
//
// dry_window_ms: how often a collapsed run of refused writes reports its
// count. extra_flags: appended to every snippet's g++ line, before any the
// project itself supplies, so the project's win. show_warnings: whether a
// compile that SUCCEEDED still prints what g++ said.
void set_dry_window_ms(unsigned ms);
void set_extra_flags(const std::string& flags);
void set_show_warnings(bool on);

// Is the attached client still running? attach() takes a snapshot and nothing
// else notices the process dying, so this has to be polled.
bool alive();

// The client's pid, or 0. Side-effect free and silent, so a poll loop can ask
// "has a client appeared yet?" without attaching or logging.
uint32_t find_client();

// Re-try DataModel resolution without re-attaching, quietly. The slot reads 0
// while the client is still loading or sitting outside a place, so the answer
// changes on its own once you join one. Returns true if it changed.
bool refresh_dm();

// Compile `body` to a DLL, load it into OUR process, call fx_run()/run().
// Returns true if the snippet compiled and ran. Blocks for as long as the
// snippet does, so callers run it off the UI thread.
bool run_snippet(const std::string& body);

// Compile and run a whole folder as one unit -- every .cpp in `dir` on the same
// g++ line, so they can call into each other. `dir` is an absolute path.
//
// Unlike run_snippet, nothing is wrapped: a bare body only makes sense when
// there is one file to wrap, so a project must define fx_run() itself.
bool run_project(const std::string& dir);

// Ask running snippets to finish. Cooperative by necessity: a looping snippet
// polls fx_running() and returns, which is the only safe way out -- killing the
// thread would unload its DLL with its code still executing.
//
// stop_all_runs stops EVERY run; it is wired to detach, client swap, offsets
// going stale and process teardown, and is what the CLI and the app teardown
// call. stop_run stops one run by id (the sidebar's per-snippet Stop).
// stop_snippet is the historical name and now means stop-all.
void stop_all_runs();
void stop_run(unsigned id);
void stop_snippet();

// Declare the CALLING thread as one of the console's own, so the fx_* walks it
// makes (fx_service, fx_find, anything guarded on fx_running) are not cut short
// by the stop-all latch stop_all_runs sets. Call it once at the top of every
// host thread that reads the client; a snippet's threads must NOT be marked.
// The full reasoning is on t_host in core/features/runtime.cpp.
void mark_host_thread();

// CLI only: swallow Ctrl+Shift+Q so pressing the stop key does not also echo
// "^Q" into the shell. A combination rather than a whole key, because claiming
// 'Q' would stop the letter working everywhere.
void set_cli_stop_key(bool on);

// Directory of the running executable — where fx.h and the import library live.
std::string self_dir();

// ---- offset health check ----------------------------------------------------
//
// The gap this exists to close: `dm_verified` is exactly
// class_of(dm) == "DataModel", which exercises three offsets --
// FakeDataModel.Pointer, Instance.ClassDescriptor, ClassDescriptor.ClassName.
// Everything else is unchecked. A wrong Instance.Name, Camera.*, Primitive.* or
// Humanoid.* reads as plausible garbage with the status line still green.
//
// Twice that has cost real time. Client version-d584fb6c717a43d9 moved names
// behind a container pointer and every name went empty while the DataModel
// verified perfectly; separately, a pointer bound that excluded module
// addresses made class_of fail while names read fine. Neither is visible to a
// check that only asks "did the read succeed" -- both need a check that asks
// "is the VALUE sane".
//
// So each check asserts a relationship the game cannot violate: a head sits
// above a torso, health does not exceed max health, a rotation matrix is
// orthonormal, a point down the camera's forward axis lands at the middle of
// the screen. Reads only; nothing here writes, and it ignores the arm gate
// because it has nothing to gate.
struct HealthCheck {
    enum Status { Pass, Fail, Skip };
    Status      status = Skip;
    std::string name;     // what was checked
    std::string detail;   // the value seen, or why it was skipped
    std::string offsets;  // the keys involved, so a failure names its suspect
};

// Runs the whole set in order. Cheap -- a few hundred reads -- and safe to call
// while a snippet is running, though the numbers will be a moment out of date.
std::vector<HealthCheck> health_check();

}  // namespace ec
