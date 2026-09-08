// input.cpp -- host-owned low-level keyboard and mouse hooks, split out of
// eclipse_console.cpp. See input.h for the ownership statement; the dense
// comments below (the latency reasoning, the semaphore-vs-event note, the
// claimed-key trap) travel verbatim with the code.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "fx.h"
#include "eclipse_core.h"     // ec::logf
#include "core_internal.h"    // core_request_stop / core_stop_requested
#include "run.h"        // core_run_current_id / core_run_label (per-run keys)
#include "read.h"       // rd64, class_of (fx_textbox walks the graph)
#include "offsets.h"    // off_of
#include "input.h"

static const uint64_t BAD = (uint64_t)-1;   // offset sentinel, as off_of returns

using ec::logf;

// ---- keyboard: a low-level hook, owned by the HOST -------------------------
// The latency in a hotkey snippet is the POLL, not GetAsyncKeyState itself: a
// loop ending in fx_sleep(30) sees the key every 30ms at best. It does not
// merely react late, it MISSES -- a tap that goes down and up inside one
// interval is never seen, however accurate the sleep is.
//
// The hook is installed HERE, in the EXE, never by a snippet. A callback living
// in a snippet's DLL would be called after that DLL unloaded; host-side
// ownership removes that hazard rather than managing it, and a snippet supplies
// no function pointer at all -- it only reads the counters below.
//
// The proc must be quick. Windows silently drops a hook that exceeds
// LowLevelHooksTimeout (300ms default), so nothing in here does more than a few
// atomic stores and a SetEvent -- no logging, no locks.
static std::atomic<uint8_t>  g_kb_down[256];   // held, as the hook saw it
static std::atomic<uint32_t> g_kb_seq[256];    // key-down count; cannot miss a tap

// Stamped into dwExtraInfo by fx_send_key, so the hook can tell OUR OWN
// injected keys from a human's and decline to count them.
//
// Without this a snippet that sends a key is heard by its own hotkey: fx_key
// and fx_key_seq would report the press we just made, and a toggle bound to
// that key would flip itself. The tag is the narrow fix -- filtering
// LLKHF_INJECTED instead would also swallow every OTHER tool's synthetic input,
// including a remapper the user actually wants, which is not ours to break.
static const ULONG_PTR KB_OURS = (ULONG_PTR)0x45434B59;   // 'ECKY'
// Key CLAIMS are now OWNED. g_kb_owner[vk] is 0 (unclaimed) or the run id that
// swallows that key -- so a second run claiming a held key is refused by name
// rather than silently stealing it, and a run's claims are released by matching
// its id, not by zeroing all 256 (which would strip a co-running snippet's).
static const unsigned KB_OWNER_ANON = 0xFFFFFFFFu;   // a claim from id 0 (CLI / un-adopted)
static std::atomic<unsigned> g_kb_owner[256];
// A SEMAPHORE, not an event, and the distinction is load-bearing. An event is
// binary: twenty presses arriving while the loop is busy collapse into one
// signal, so the loop wakes once, handles one, and drains the other nineteen at
// one per timeout. Measured with a 50ms loop body and a 20-press burst, that was
// 6 handled in 1.5s. A semaphore counts, so every press is its own release and
// the loop drains at its own speed instead of the timeout's.
//
// PER RUN, and that is not optional (docs/multi-run.md, "the semaphore"): one
// release wakes exactly ONE waiter, so with two snippets parked a shared
// semaphore woke one arbitrarily and the other sat until its timeout -- late,
// not lost, but late. So each run waits on its OWN semaphore and the hook
// releases EVERY live run's.
//
// The pool is fixed and its handles are created once and NEVER CLOSED -- the
// same rule g_kb_sem always followed, and here it also closes the race that
// would otherwise exist between kb_run_end closing a handle and the hook
// calling ReleaseSemaphore on it. A run ACQUIRES a slot (marks its id) and
// RELEASES it (clears the id, drains the sem for the next acquirer); the handle
// stays put. The hook walks .h lock-free -- it is stable after kb_start built
// the pool, which happens before the hook is ever installed.
static HANDLE g_kb_sem = nullptr;              // the fallback sem: CLI + un-adopted threads
struct KbSem { std::atomic<unsigned> id{0}; HANDLE h = nullptr; };
static const int KB_SEM_MAX = CORE_RUN_CAP;    // one slot per possible concurrent run
static KbSem      g_run_sem[KB_SEM_MAX];        // pooled, h created once in kb_start
static std::mutex g_run_sem_mux;                // guards slot acquire/release only, NOT the hook
static std::atomic<int> g_kb_runs{0};           // live runs holding a slot; hook dies on the last
static std::atomic<bool> g_swallow_stopkey{false};   // see set_cli_stop_key
static HHOOK  g_kb_hook   = nullptr;
static HANDLE g_kb_thread = nullptr;
static DWORD  g_kb_tid    = 0;

// The hook's wake: release the fallback sem and every pooled sem. LOCK-FREE and
// ALLOCATION-FREE -- it runs inside WH_KEYBOARD_LL, which Windows drops if it
// overruns LowLevelHooksTimeout (300ms). A bounded walk of KB_SEM_MAX + 1
// handles, no lock, no registry walk. A release into a slot no run is waiting
// on is harmless: it is drained on the next acquire.
static inline void kb_wake_sems(void) {
    if (g_kb_sem) ReleaseSemaphore(g_kb_sem, 1, nullptr);
    for (int i = 0; i < KB_SEM_MAX; i++)
        if (g_run_sem[i].h) ReleaseSemaphore(g_run_sem[i].h, 1, nullptr);
}

static LRESULT CALLBACK kb_proc(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)l;
        DWORD vk = k->vkCode;
        // OUR OWN injection: pass it straight through to the game without
        // counting it. Falling through to CallNextHookEx rather than returning
        // 1 is the whole point -- swallowing it here would mean the key never
        // reached the client and fx_send_key would do nothing at all.
        if (k->dwExtraInfo == KB_OURS)
            return CallNextHookEx(g_kb_hook, code, w, l);
        if (vk < 256) {
            bool down = (w == WM_KEYDOWN || w == WM_SYSKEYDOWN);
            bool up   = (w == WM_KEYUP   || w == WM_SYSKEYUP);
            if (down) {
                // Auto-repeat re-sends WM_KEYDOWN while a key is held. Count
                // only the real edge, so holding F6 is one press and not one
                // per repeat.
                if (!g_kb_down[vk].exchange(1)) {
                    g_kb_seq[vk].fetch_add(1);
                    // If the count is somehow saturated the release fails, which
                    // costs a wake but not the press -- g_kb_seq already has it,
                    // and the caller's timeout still comes round.
                    kb_wake_sems();
                }
            } else if (up) {
                g_kb_down[vk].store(0);
                // Release on the UP edge too, so a hold-to-act snippet notices
                // you let go immediately instead of at the end of its timeout.
                // Only fx_key_seq counts presses, so this adds wakes, not
                // phantom presses.
                kb_wake_sems();
            }
            // The CLI stop key. Detected AND swallowed here, in one place.
            //
            // The two cannot be split across threads: a key this hook eats never
            // reaches the async key state, so a separate poller would never see
            // it and the swallow would silently disable the detector. Whatever
            // consumes a key has to be what notices it.
            //
            // A combination, not a whole key: claiming 'Q' outright would stop
            // the letter working everywhere.
            if ((down || up) && g_swallow_stopkey.load() && vk == 'Q' &&
                (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
                (GetAsyncKeyState(VK_SHIFT)   & 0x8000)) {
                // Nothing slow in here -- no logging, no locks. Windows drops a
                // hook that overruns LowLevelHooksTimeout.
                if (down) {
                    core_request_stop();
                    kb_wake_sems();
                }
                return 1;
            }

            // Swallow both halves or neither: passing the up while eating the
            // down leaves the game believing the key is still held.
            if ((down || up) && g_kb_owner[vk].load()) return 1;
        }
    }
    return CallNextHookEx(nullptr, code, w, l);
}

// ---- mouse: the same counters, a second hook -------------------------------
// A WH_KEYBOARD_LL hook does not see mouse buttons, so fx_key_seq(VK_MBUTTON)
// sat at zero forever and fx_claim_key could not swallow a click. Rather than
// grow a parallel fx_mouse_* API, this feeds the SAME arrays: every existing
// entry point starts working for VK_LBUTTON/RBUTTON/MBUTTON/XBUTTON1/2 with no
// new surface for a snippet to learn.
//
// Same speed rule as kb_proc, and it matters more here: WM_MOUSEMOVE arrives
// continuously, so the non-button path must fall straight through.
static HHOOK g_ms_hook = nullptr;

static int mouse_vk(WPARAM w, const MSLLHOOKSTRUCT* m, bool* down) {
    switch (w) {
        case WM_LBUTTONDOWN: *down = true;  return VK_LBUTTON;
        case WM_LBUTTONUP:   *down = false; return VK_LBUTTON;
        case WM_RBUTTONDOWN: *down = true;  return VK_RBUTTON;
        case WM_RBUTTONUP:   *down = false; return VK_RBUTTON;
        case WM_MBUTTONDOWN: *down = true;  return VK_MBUTTON;
        case WM_MBUTTONUP:   *down = false; return VK_MBUTTON;
        // Which side button it was rides in the high word of mouseData.
        case WM_XBUTTONDOWN: *down = true;
            return HIWORD(m->mouseData) == XBUTTON2 ? VK_XBUTTON2 : VK_XBUTTON1;
        case WM_XBUTTONUP:   *down = false;
            return HIWORD(m->mouseData) == XBUTTON2 ? VK_XBUTTON2 : VK_XBUTTON1;
    }
    return 0;                       // move, wheel, everything else
}

static LRESULT CALLBACK ms_proc(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION) {
        bool down = false;
        int vk = mouse_vk(w, (const MSLLHOOKSTRUCT*)l, &down);
        if (vk) {
            if (down) {
                // Held-button state, mirroring kb_proc. A mouse button has no
                // auto-repeat, but the exchange keeps the two paths identical
                // and costs nothing.
                if (!g_kb_down[vk].exchange(1)) {
                    g_kb_seq[vk].fetch_add(1);
                    kb_wake_sems();
                }
            } else {
                g_kb_down[vk].store(0);
                kb_wake_sems();
            }
            // Both halves or neither, or the game is left believing the button
            // is still held.
            if (g_kb_owner[vk].load()) return 1;
        }
    }
    return CallNextHookEx(nullptr, code, w, l);
}

static DWORD WINAPI kb_thread(void*) {
    // A low-level hook is dispatched through the installing thread's message
    // queue, so this thread exists to own one. Stop pumping and the hook stops
    // firing -- which is why it cannot live on the WebView2 thread, where a busy
    // UI would stall input for the whole desktop.
    //
    // Both hooks live on this one thread: they share the queue and the
    // counters, and a second thread would buy nothing but another way for the
    // two halves of the input state to disagree.
    g_kb_hook = SetWindowsHookExW(WH_KEYBOARD_LL, kb_proc, GetModuleHandleW(nullptr), 0);
    g_ms_hook = SetWindowsHookExW(WH_MOUSE_LL,    ms_proc, GetModuleHandleW(nullptr), 0);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    if (g_ms_hook) { UnhookWindowsHookEx(g_ms_hook); g_ms_hook = nullptr; }
    if (g_kb_hook) { UnhookWindowsHookEx(g_kb_hook); g_kb_hook = nullptr; }
    return 0;
}

// Started on first use, not at boot: a global keyboard hook is a conspicuous
// thing to hold, and a session that never arms a hotkey has no business with
// one. Torn down when the snippet ends, so it exists only while something is
// actually listening.
void kb_start(void) {
    if (g_kb_thread) return;
    // Created once and never closed. The hook thread is torn down per run, but
    // closing these alongside it would race Stop arriving from the UI thread
    // just as the snippet returns -- releasing a handle that had already been
    // closed, or worse, one since recycled. It costs a handful of handles for
    // the session. The pool is built HERE, before the hook is installed just
    // below, so the hook's lock-free walk of .h always sees stable handles.
    if (!g_kb_sem) g_kb_sem = CreateSemaphoreW(nullptr, 0, 65536, nullptr);
    for (int i = 0; i < KB_SEM_MAX; i++)
        if (!g_run_sem[i].h) g_run_sem[i].h = CreateSemaphoreW(nullptr, 0, 65536, nullptr);
    g_kb_thread = CreateThread(nullptr, 0, kb_thread, nullptr, 0, &g_kb_tid);
}

void kb_stop(void) {
    if (!g_kb_thread) return;
    PostThreadMessageW(g_kb_tid, WM_QUIT, 0, 0);
    WaitForSingleObject(g_kb_thread, 2000);
    CloseHandle(g_kb_thread);
    g_kb_thread = nullptr;
    g_kb_tid    = 0;
}

// Release only the claims a given run owns. A claim that outlived the snippet
// which set it would leave that key dead for the rest of the session; zeroing
// all 256 (as the single-run code did) would strip a CO-RUNNING snippet's, so
// this matches by owner id. id 0 is the CLI / un-adopted anon owner.
void kb_release_claims_for(unsigned id) {
    unsigned val = id ? id : KB_OWNER_ANON;
    for (int i = 0; i < 256; i++) {
        unsigned e = g_kb_owner[i].load();
        if (e == val) g_kb_owner[i].store(0);
    }
}

// ---- per-run keyboard lifecycle -------------------------------------------
// Called around each run by build_and_run. The shared counters (g_kb_seq /
// g_kb_down) are DELIBERATELY not zeroed here any more: a second run zeroing
// them would make every OTHER live run's per-DLL fx_key_pressed seen[] exceed
// the counter and fire forever. fx_key_pressed instead primes its baseline to
// the live counter on first poll (see fx.h), so a fresh run never inherits an
// old press without any global reset.
void kb_run_begin(unsigned id) {
    if (id == 0) {
        // CLI / un-adopted single run: drain the fallback sem so a key pressed
        // before this run started is not a wake it gets to see.
        if (g_kb_sem) while (WaitForSingleObject(g_kb_sem, 0) == WAIT_OBJECT_0) {}
        return;
    }
    std::lock_guard<std::mutex> lk(g_run_sem_mux);
    for (int i = 0; i < KB_SEM_MAX; i++) {
        if (g_run_sem[i].id.load() == 0) {
            g_run_sem[i].id.store(id);
            g_kb_runs.fetch_add(1);
            return;
        }
    }
    // No free slot -- impossible while KB_SEM_MAX == the run cap, but if it
    // ever happened fx_wait_key falls back to g_kb_sem rather than parking dead.
}

void kb_run_end(unsigned id) {
    kb_release_claims_for(id);
    if (id == 0) return;
    {
        std::lock_guard<std::mutex> lk(g_run_sem_mux);
        for (int i = 0; i < KB_SEM_MAX; i++) {
            if (g_run_sem[i].id.load() == id) {
                g_run_sem[i].id.store(0);
                // Drain for the next acquirer. The hook may add a count between
                // this drain and reuse; that is one spurious wake, not a lost
                // one -- fx_wait_key loops and re-checks, same "late not lost"
                // property the semaphore already relies on.
                if (g_run_sem[i].h)
                    while (WaitForSingleObject(g_run_sem[i].h, 0) == WAIT_OBJECT_0) {}
                if (g_kb_runs.load() > 0) g_kb_runs.fetch_sub(1);
                break;
            }
        }
    }
    // The hook dies with the LAST run, not the first to end.
    if (g_kb_runs.load() == 0) kb_stop();
}

// The current thread's run semaphore: its own if it holds a slot, else the
// shared fallback (CLI, un-adopted threads). Reads .h without the lock -- the
// handle is stable after kb_start built the pool, which fx_wait_key ensures.
static HANDLE current_run_sem(void) {
    unsigned id = core_run_current_id();
    if (id)
        for (int i = 0; i < KB_SEM_MAX; i++)
            if (g_run_sem[i].id.load() == id) return g_run_sem[i].h;
    return g_kb_sem;
}

// Wake one run parked in fx_wait_key (a targeted Stop), or every parked run
// (stop-all). Called from the run/stop path, never from the hook.
void kb_wake_run(unsigned id) {
    if (id == 0) { if (g_kb_sem) ReleaseSemaphore(g_kb_sem, 1, nullptr); return; }
    for (int i = 0; i < KB_SEM_MAX; i++)
        if (g_run_sem[i].id.load() == id && g_run_sem[i].h)
            { ReleaseSemaphore(g_run_sem[i].h, 1, nullptr); return; }
}
void kb_wake_all(void) { kb_wake_sems(); }

// GetAsyncKeyState: for "is it down right now" that is the authoritative
// answer, including for keys already held before the hook existed.
// Is this key down right now?
//
// CLAIMED KEYS COME FROM THE HOOK, not from the async state, and that is not an
// optimisation -- it is the only way this can work at all. A key the hook eats
// never reaches the async key state (the stop-key comment above says the same
// thing from the other side), so a snippet that claimed 'W' and then asked
// fx_key('W') got a permanent NO and had silently disabled its own input.
// That cost a working freecam: claiming the movement keys stopped the camera
// moving, and claiming Shift made the Shift+P that leaves it impossible to
// press.
//
// The hook's own down-state has no such problem, since it is recorded before
// the swallow. Unclaimed keys still use GetAsyncKeyState, which is right for a
// key already held when the snippet started -- the hook would not have seen
// that one go down.
FX_API int fx_key(int vk) {
    if (vk >= 0 && vk <= 255 && g_kb_owner[vk].load()) {
        kb_start();
        return g_kb_down[vk].load() ? 1 : 0;
    }
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// How many times this key has gone down since the run began. A counter rather
// than a flag is what makes a tap between two polls impossible to lose.
FX_API unsigned fx_key_seq(int vk) {
    if (vk < 0 || vk > 255) return 0;
    kb_start();
    return g_kb_seq[vk].load();
}

// Park until the next key event (down OR up), Stop, or the timeout. ms == 0 waits
// indefinitely. Returns 0 when stopping, mirroring fx_sleep, so it drops
// straight into the tail of a hotkey loop in place of it.
//
// THIS is the latency fix. The hook alone does not make a snippet react faster:
// it delivers the event promptly, but a loop sleeping 30ms still looks 30ms
// late. Waiting on the event instead means the wake IS the keystroke -- sub-
// millisecond, and burning no CPU in between.
FX_API int fx_wait_key(unsigned ms) {
    kb_start();
    HANDLE sem = current_run_sem();   // THIS run's semaphore; the hook wakes all
    const unsigned slice = 50;      // bounded so Stop is honoured even if the
    for (;;) {                      // wakeup were somehow missed
        if (core_stop_requested()) return 0;
        unsigned w = (ms == 0 || ms > slice) ? slice : ms;
        DWORD r = WaitForSingleObject(sem, w);
        if (core_stop_requested()) return 0;
        if (r == WAIT_OBJECT_0) return 1;
        if (ms) { ms -= w; if (!ms) return 1; }
    }
}

// Swallow a key so the game never sees it -- F6 toggles your speed without also
// reaching Roblox. Released automatically when the snippet ends.
FX_API void fx_claim_key(int vk, int on) {
    if (vk < 0 || vk > 255) return;
    // Left click is the one claim that cannot be undone from the UI. Claiming
    // a letter is merely annoying -- you can still reach Stop with the mouse.
    // Swallowing LBUTTON globally means no button in this app can be pressed
    // AT ALL, Stop included, so the only ways out are the CLI stop chord or
    // killing the process. Refused rather than documented.
    if (vk == VK_LBUTTON && on) {
        logf("x fx_claim_key: refusing to claim left click"
             " - it would swallow the clicks that stop the snippet\n");
        return;
    }
    kb_start();
    unsigned me = core_run_current_id();
    unsigned mine = me ? me : KB_OWNER_ANON;
    if (on) {
        unsigned cur = g_kb_owner[vk].load();
        // First-wins across runs: a second claimant on a held key is refused by
        // name, not a silent steal. Re-claiming your own key is idempotent.
        if (cur != 0 && cur != mine) {
            std::string who = core_run_label(cur);
            logf("x fx_claim_key: key %d is held by %s\n",
                 vk, who.empty() ? "another run" : who.c_str());
            return;
        }
        g_kb_owner[vk].store(mine);
    } else {
        // Release only if it is ours -- can't drop another run's claim.
        unsigned cur = g_kb_owner[vk].load();
        if (cur == mine) g_kb_owner[vk].store(0);
    }
}

// ---- interface for the run machinery (state stays owned here) --------------
// Two thin entry points so stop_snippet() and set_cli_stop_key() -- which live
// with the build/run path -- can poke this file's state without it being
// published. New code, not moved: the moved bodies referenced g_kb_sem and
// g_swallow_stopkey directly when they shared a translation unit.

// The CLI stop-chord swallow flag lives with the hook that reads it.
void kb_set_swallow_stopkey(bool on) { g_swallow_stopkey = on; }

// ---- SENDING a key ---------------------------------------------------------
// The counters above are the snippet READING input. This is the other
// direction, and it is the only place in the tree that produces any.
//
// STILL NOT INJECTION. SendInput hands the OS an input event; it is what a
// keyboard does. Nothing is written into the client, no code is placed in its
// address space, and the client cannot tell this apart from the user's own
// keypress because at the OS layer there is nothing to tell apart. The BOUNDARY
// statement in eclipse_console.cpp is untouched by it.
//
// GATED ON THE CLIENT BEING FOREGROUND, and that gate is not a nicety. A
// synthetic keystroke goes to whatever window has focus -- alt-tab to a browser
// while a parry loop is running and, ungated, this types into it. Refusing is
// the only safe answer, and it costs the caller nothing: a game that is not
// focused was not going to accept the input anyway.
//
// Deliberately NOT gated on fx_typing(): the chat box is a real reason not to
// send, but it is the SNIPPET's decision, and fx_input_ok() is the one call
// that expresses both. Building it in here would make the common case correct
// and the uncommon one impossible.
//
// Sends the virtual key WITH its scan code filled in, and no KEYEVENTF_SCANCODE
// flag -- that produces an ordinary WM_KEYDOWN with a correct lParam, which is
// what a game reading Windows messages expects. If a game ever ignores it, the
// thing to try is scancode-only (set the flag, leave wVk at 0), which is what
// titles reading raw input want instead.
//
// Returns 1 when both halves went out, 0 otherwise. hold_ms BLOCKS the calling
// thread, so a spin loop should pass 0 and let the release land next tick, or
// accept the stall knowingly -- it is capped at 500ms so a typo cannot park a
// run for a minute.
FX_API int fx_send_key(int vk, int hold_ms) {
    if (vk <= 0 || vk > 254) return 0;
    if (!fx_focused()) return 0;

    INPUT in[2]{};
    in[0].type           = INPUT_KEYBOARD;
    in[0].ki.wVk         = (WORD)vk;
    in[0].ki.wScan       = (WORD)MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    in[0].ki.dwExtraInfo = KB_OURS;
    in[1]                = in[0];
    in[1].ki.dwFlags     = KEYEVENTF_KEYUP;

    if (SendInput(1, &in[0], sizeof(INPUT)) != 1) return 0;
    if (hold_ms > 0) Sleep((DWORD)(hold_ms > 500 ? 500 : hold_ms));
    return SendInput(1, &in[1], sizeof(INPUT)) == 1 ? 1 : 0;
}

// ---- input gating: the object-graph questions a hotkey snippet asks --------
// Focus and the in-game textbox. These read the DataModel (published reads /
// offsets), not the hooks above -- co-located here as the snippet's INPUT view.
// Is the attached client the foreground window?
//
// Compared by PID rather than by a captured HWND: the client owns several
// top-level windows over its life and the one that is focused is not
// necessarily the one that existed at attach. Returns 0 while detached, which
// reads as "not focused" -- the safe answer for something used as a gate.
FX_API int fx_focused(void) {
    if (!ec::state().pid) return 0;
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == ec::state().pid ? 1 : 0;
}

// The Roblox TextBox that currently has focus, or 0.
//
// UserInputService.WindowInputState -> WindowInputState.CurrentTextBox. Non-null
// exactly while an in-game text field is focused: the chat bar, a name entry,
// any TextBox a game puts up. This is the client's own notion of "the user is
// typing", which is why it beats guessing from keystroke timing -- it is right
// the instant the box opens and right again the instant it closes.
//
// The service pointer is cached. Finding it means walking every DataModel child
// and reading each one's class name, which is a few hundred cross-process reads
// for ~50 instances; a loop polling at 60 Hz would spend most of its time here.
// Invalidated on the DataModel generation, because leaving a place frees it.
FX_API uint64_t fx_textbox(void) {
    static uint64_t uis     = 0;
    static unsigned uis_gen = (unsigned)-1;

    uint64_t dm = fx_dm();
    if (!dm) { uis = 0; return 0; }

    if (uis_gen != fx_dm_gen()) { uis = 0; uis_gen = fx_dm_gen(); }

    if (!uis) {
        // Same child-container layout fx_nchildren walks: a pointer to the
        // container, then [begin, end) of 0x10-byte entries.
        uint64_t cso = off_of("Instance.ChildrenStart");
        uint64_t ceo = off_of("Instance.ChildrenEnd");
        if (cso == BAD || ceo == BAD) return 0;
        uint64_t cptr  = rd64(dm + cso);
        uint64_t begin = cptr ? rd64(cptr) : 0;
        uint64_t end   = cptr ? rd64(cptr + ceo) : 0;
        if (!begin || end <= begin) return 0;
        uint64_t n = (end - begin) / 0x10;
        // Same stale-pointer guard as fx_nchildren, and it matters more here:
        // this runs on every fx_typing() call, so once per tick in every
        // snippet that gates on input. An unbounded walk here hangs the
        // snippet with no way to Stop it.
        if (n > FX_MAX_CHILDREN) return 0;
        for (uint64_t i = 0; i < n && !uis; i++) {
            uint64_t ch = rd64(begin + i * 0x10);
            if (ch && class_of(ch) == "UserInputService") uis = ch;
        }
        if (!uis) return 0;
    }

    uint64_t wo = off_of("UserInputService.WindowInputState");
    if (wo == BAD) return 0;
    uint64_t wis = rd64(uis + wo);
    if (!wis) return 0;

    uint64_t to = off_of("WindowInputState.CurrentTextBox");
    if (to == BAD) return 0;
    return rd64(wis + to);
}

FX_API int fx_typing(void) { return fx_textbox() ? 1 : 0; }
