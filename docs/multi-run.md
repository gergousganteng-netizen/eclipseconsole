# Running several snippets at once

**Status: phases 1 and 2 are BUILT.** Phase 1 (the Run registry, per-run stop
events and key semaphores, the owner-table key claims, per-run dry-run
collapsing, the cap of 4) landed first; phase 2 (per-run `fx_window_*` — the
`SnipWin` map in `snipwin.cpp`, per-run message deques, the run id in the
docked SNIPMSG protocol) landed 2026-08-27. Still exactly as designed below
and NOT built: per-corner docking (the dock stays singular, first-come,
refused by name) and per-run `fx_overlay_*` (stays first-wins). The decision
record below stands — read it before touching either.

Today the host serialises runs on purpose: Run on a second snippet stops the
first. This is the shape that would replace that.

---

## Why it is serialised today

One of everything, and each one is a genuine blocker:

| what | where | why it blocks |
|---|---|---|
| `g_stop` | `core/features/runtime.cpp:29` | one atomic behind `fx_running()` — Stop is all-or-nothing |
| `g_stop_event` | `runtime.cpp:35` | every `fx_sleep`/`fx_wait_key` wakes on the same event |
| `g_kb_sem` | `core/features/input.cpp:47` | ONE semaphore; a release wakes exactly one waiter |
| `g_kb_claim[256]` | `input.cpp:40` | a flag with no owner; `kb_release_claims()` zeroes all 256 |
| the hook | `kb_start`/`kb_stop` | torn down when "the snippet" ends, not the last one |
| the loaded DLL | `core/snippet_build.cpp:419` | one `HMODULE`, `FreeLibrary`'d at run end |
| fd 1 | `snippet_build.cpp:330` | printf capture is process-global |
| `g_snip_wv`, `g_snip_msgs` | `ui/snipwin.cpp:40,42` | one window, one message deque |
| RUNSTATE | `core_run_phase_begin` | the UI models running as a boolean |

Already fine, do not "fix" these:

- `g_sleep_timer` and `g_pacer` (`runtime.cpp:67,189`) are **`thread_local`**, so
  pacing and interruptible sleep scale to N runs for free.
- `fx_key_pressed`'s `static unsigned seen[256]` lives inside a `static inline`
  in `fx.h`, so **each snippet DLL gets its own copy**. Two snippets watching the
  same key both see the press and neither eats the other's. Moving that counter
  host-side would BREAK this.

---

## The shape

One `Run` per execution, owned by a registry with a mutex, exposed functions-only
like every other seam here.

```cpp
struct Run {
    unsigned          id;          // 1..N, never reused
    std::string       label;       // snippet name: log tags and the UI list
    std::atomic<bool> stop{false};
    HANDLE            stop_event;  // its own, not the shared one
    HANDLE            key_sem;     // its own -- see "the semaphore" below
    HMODULE           dll;
    std::string       dll_path;    // deleted at teardown
};
```

**The context comes from the thread, not from a parameter.** This is what keeps
`fx.h` unchanged and the boundary C:

```cpp
static thread_local Run* t_run = nullptr;

FX_API int fx_running(void) {
    if (!t_run) return g_stop_all ? 0 : 1;   // un-adopted thread; see below
    return t_run->stop ? 0 : 1;
}
```

`spawn_run` (`ui/eclipse_ui.cpp:603`) sets `t_run` before calling `run_snippet`.
That is the whole plumbing — every `fx_*` export resolves its own run for free,
no snippet source changes, the boundary stays POD and `extern "C"`.

---

## Snippet-spawned threads: adopt explicitly, do NOT infer

A snippet that starts its own thread leaves `t_run` null there, because the host
never saw the spawn.

**The tempting rule — "un-adopted threads fall back to stop-all" as the only
answer — is WRONG, and there is a live counter-example in the user's tree.**
`backdash/main.cpp` spawns a worker running `dash_run`, which calls
`fx_running()`, `fx_sleep()` and `fx_pace()`. It ends with:

```cpp
// Before the window closes and before fx_run returns: this DLL is unloaded
// on return, and a thread still inside dash_run would be executing freed
// code. fx_running() is already false, so the burst exits on its own.
if (worker.joinable()) worker.join();
```

That join **depends on the worker observing `fx_running() == false`** after Stop.
Under the fallback rule the worker sees `true` on a single-snippet Stop, does not
exit, and `fx_run` blocks in `join()` until the dash reaches its wall-clock end.
Bounded to about a second here because `dash_run` is time-limited — but one
unbounded loop away from deadlocking the run thread forever.

So add two **additive** exports (nothing existing changes shape):

```cpp
unsigned rid = fx_run_id();                 // on the run thread
worker = std::thread([rid, plan, &busy]() {
    fx_adopt_run(rid);                      // now fx_running() means MY run
    dash_run(plan); busy = false;
});
```

Un-adopted threads keep the stop-all fallback, which is safe: they stop when
everything stops, never on their own snippet's Stop alone.

**`backdash` must be updated in the same change.** Its Stop regresses silently
otherwise, and the comment above shows the author reasoning from exactly the
guarantee this removes.

---

## The semaphore, and why per-run is not optional

`fx_wait_key` does `WaitForSingleObject(g_kb_sem, w)` on **one** semaphore
(`input.cpp:47`, `:262`). A release wakes **exactly one waiter**. With two
snippets parked, a key press wakes one of them arbitrarily; the other sits until
its timeout, then catches up because `fx_key_seq` counted the press.

Nothing is lost. It is *late* — which is the distinction `input.cpp:45` already
makes about events versus semaphores:

> Nothing was ever lost, since `fx_key_seq` had already counted them, but "not
> lost" and "not late" are different properties.

Multi-run reopens that exact scar. So `Run` owns a `key_sem`, and the hook
releases **every live run's** semaphore.

Two latent bugs this also fixes:

- `kb_reset()` drains `g_kb_sem` at run start (`input.cpp:209`). Shared, starting
  a second snippet swallows a wake the first had not consumed.
- `kb_wake()` (`input.cpp:295`), which `stop_snippet` uses to unpark a sleeping
  snippet, would otherwise wake whichever run happened to be waiting rather than
  the one being stopped.

**Constraint on the release loop:** it runs inside the `WH_KEYBOARD_LL` proc,
which must stay well under `LowLevelHooksTimeout` (300 ms) or Windows silently
stops calling it. The existing proc "does only a few atomic stores". Do **not**
walk a mutex-protected registry in there — keep a lock-free snapshot (fixed
array of handles plus an atomic count) so the hook never allocates and never
blocks on a descheduled thread.

---

## Key claims become owned

`g_kb_claim[256]` (`std::atomic<uint8_t>`) becomes an owner table:

```cpp
static std::atomic<unsigned> g_kb_owner[256];   // 0 = unclaimed, else run id
```

- release clears only entries matching the ending run
- a second claimant on a held key is **refused with a log line**, not a silent steal
- `kb_start`/`kb_stop` become refcounted so the hook dies on the *last* run
- the `VK_LBUTTON` refusal stays exactly as it is

---

## Dry-run collapsing must key on (run, destination)

`fx_wrblk`'s disarmed collapsing keys on the **destination**, and a run is closed
"by a write elsewhere, by any other log line, by arming, or by the run ending"
(README). Two snippets writing different addresses each close the other's run on
every tick, so collapsing stops working entirely and a disarmed pair floods the
log at full rate — defeating the machinery that exists specifically to stop a dry
run taking the app down.

Key on `(run id, destination)`.

---

## Log attribution

`fx_log`/`fx_logf` run on the snippet's own thread, so `t_run` is right there —
**those attribute perfectly and for free.** Tag them with the run label whenever
more than one run is live.

**Captured `printf` cannot be attributed, and this is structural, not hard.** The
host captures it by rebinding **fd 1**, which is per-*process*; by the time bytes
reach the pipe there is no origin left. Worse than mislabelling: two snippets
printing multi-line output produce spliced blocks, one snippet's line landing
inside another's paragraph.

- **Do:** mark captured printf as unattributed, and say so in `fx.h` — `fx_log`/
  `fx_logf` are the attributed path.
- **Do not:** attribute a printf line to whichever run last called `fx_log`. It
  would be right most of the time, which is precisely what makes it dangerous.

The only real fix is one process per run, which breaks the whole snippet-DLL-in-
our-process model. Not worth it.

---

## Windows and the overlay

**They are not the same problem.** A window is naturally N-instance; the overlay
is naturally 1-instance. Decided: windows go per-run, the overlay does not.

### `fx_window_*` — per run

```cpp
struct SnipWin {
    std::unique_ptr<webview::webview> wv;     // floating host (mode 0)
    std::deque<std::string>           msgs;   // bounded 1024, as today
    bool docked = false;
    int  w, h, corner, dx, dy;
    std::string src;
};
```

Keyed by run id in a map `snipwin.cpp` owns. Exports resolve their instance from
`t_run`, same as everything else, so no API change.

The per-run `msgs` deque is the point. Today `fx_window_get_msg` pops one shared
deque (`snipwin.cpp:42`), so snippet A can pop a message produced by B's page —
A acting on B's button click, silently and intermittently. Per-run makes that
impossible by construction.

Each floating window is its own webview with its own handler, so the closure
captures the run id for free. The **docked** path cannot: `DOCKP:`/`DOCKEVAL:`
are relayed through the overlay page, so those messages must carry a run id in
the protocol.

### Docking stays singular

`dock_apply_mapping()` maps ONE origin onto the composed overlay core — one
mapping, one corner. N floating windows is easy; N docked panels is deep overlay
surgery.

- N floating, **first-come on the dock**, second claimant refused with a log line.
- Per-corner docking (up to four) is the natural upgrade. Separate job.

### `fx_overlay_*` stays first-wins

`fx_overlay_post(json)` feeds one draw list for one drawing surface. Per-run
would mean merging N draw lists into every composed frame and deciding z-order
between runs. That is a renderer change, not a state split.

Consequence to state in the UI: `aim/esp_overlay.cpp` and `radar.cpp` cannot
co-run. The refusal must say so rather than one silently winning.

---

## Concurrency cap: 4

Threads are cheap; this is not about resources. The argument is the **wedged
snippet** — one that never polls `fx_running()` cannot be stopped, and holds its
thread, DLL, stop event and semaphore forever. Without a cap, a user who cannot
stop it simply runs another, and another, each leaking, with nothing on screen
explaining why the app is getting heavier.

Secondary: N runs × 60 Hz × syscall reads all land on the game process; the log
queue is the documented bottleneck and Stop and Arm ride it; and one faulting
snippet still takes every other run down with it.

**Cap at 4.** Count wedged runs toward it — a cap that excused the leaked ones
would be measuring the wrong thing. The refusal names what holds the slots, so
it doubles as the diagnosis:

```
x cannot run: 4 already running (boost, radar, dex, changes) - stop one first
```

With the cap at 4 a separate window cap is probably unnecessary — four WebView2
controllers is heavy but bounded. Revisit if the run cap ever rises.

---

## Lifecycle: everything routes to `stop_all()`

`stop_all()` = set every Run's flag, signal every stop event, release every key
semaphore. Wire it to:

- **detach** and **client swap**
- **offsets going stale** — the most important one. That gate is what stands
  between a stale conf and an armed write, so it must reach every run, not just
  the front one.
- **process teardown** — stop, then join, then unload. A run still inside its DLL
  when `FreeLibrary` lands is executing freed code.

---

## UI

- `RUNSTATE` becomes a **list**, not a boolean; the sidebar shows which snippets
  are live.
- **"Run on something else stops what's running" goes away.** This is the
  user-visible change in all of it, so it should be a deliberate decision rather
  than a side effect.
- Starting a second snippet **prompts**:

  > **`boost.cpp` is already running.**
  > [Run both] [Stop it and run this] [Cancel]

  The old behaviour survives as an explicit option, and concurrency is opt-in.
  A "don't ask again" may remember **Run both** only — the destructive answer
  must not get silently repeated.
- Re-running **the same** snippet is a different question ("already running,
  restart it?"), since two copies of `boost.cpp` writing one field at 60 Hz is
  almost never intended.
- A wedged run needs a visible **"stopping…"** state rather than a lie.

---

## The CLI stays single-run

`eclipse_cli --run FILE` keeps one run at a time. Ordered output, one stdin and
Ctrl+C as a real console control event are its entire reason to exist, and
multi-run costs all three. The host being *capable* of more does not oblige the
CLI to use it.

---

## What must not regress

- **The arm gate.** `fx_wrblk` logs and returns N when disarmed — per run, with
  per-run collapsing, and still nothing reaching the process.
- **The BOUNDARY guarantee.** External data access only.
- **`fx.h` stays C.** The two new exports are POD and `extern "C"`; nothing
  existing changes shape.
- **`fx_key_pressed`'s per-DLL `seen[]`.** Leave it in the header.
- **Hook proc timing.** Under `LowLevelHooksTimeout`, no locks, no allocation.
- **Log coalescing.** Stop and Arm ride the same queue as log lines; N runs put
  more pressure on it, not less.

---

## Verification

The failure modes here are races, so a green run proves little. Each of these
should be made to FAIL first:

- **Two snippets on one hotkey.** Both should act on every press. Before the
  per-run semaphore, one wins and the other is a timeout late — reproduce that,
  then fix it, so the test is known to be able to go red.
- **Stop one of three.** The other two keep running; the stopped one's key claims
  are released and the other two keep theirs.
- **backdash Stop.** With the adoption change, an in-flight dash still cuts
  mid-burst. Without it, Stop is visibly sluggish — check both.
- **Disarmed pair.** Two snippets writing different addresses at 60 Hz should
  still collapse into two counted runs, not flood.
- **Offsets going stale** with 4 runs live stops all 4.
- **Wedged snippet** counts toward the cap and shows as "stopping…".
