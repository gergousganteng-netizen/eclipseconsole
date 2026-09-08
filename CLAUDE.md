# CLAUDE.md

Context for Claude Code working in this repo. Read this before touching anything;
`README.md` is the human-facing reference for details this file skips.

## What this is

A Windows external-memory console for the Roblox client. Paste C++ into the
UI, it compiles to a DLL, loads into **our own** process, and reads/writes the
game's memory from the outside. Pure data access — no Luau VM, no bytecode, no
injection, no control-flow writes. `fx_wrblk` is the only write primitive and
it is **dry-run until armed** (the arm toggle logs the intended write instead
of performing it).

Ported from a Linux original. The port shape is the same; the OS primitives
changed. See the top of `src/core/eclipse_console.cpp` for the mapping table and
the BOUNDARY paragraph — the safety guarantees are load-bearing, don't weaken
them.

## Architecture in one screen

```
web/index.html + app.js + Eclipse UI kit css
        │  WebView2 (child HWND covers the whole client area)
        ▼
eclipse_console.exe  (GUI subsystem)   eclipse_cli.exe  (console subsystem)
        │       ▲                              │
        │       │ import lib libeclipse_console.a       (same core, different subsystem
        │       │ each snippet DLL links back against    is the only real difference)
        │       │
        ▼       │
     ec::attach() → indirect NtRead/NtWriteVirtualMemory syscall stubs
                    (built at attach time, past any ntdll hook — no RPM/WPM
                    fallback, attach fails hard if the stubs can't be built)
                    handle via NtDuplicateObject from RobloxCrashHandler.exe,
                    so our process never issues NtOpenProcess on the game
                    (falls back to a direct OpenProcess if the crash handler
                    is not running — still works, but is detectable)
```

The `fx_*` boundary in `src/fx.h` is what a snippet sees. It's pure C
(extern "C", POD arguments, char*), and every snippet is compiled with
`-include fx.h` so it's always in scope. Do NOT put C++ objects across that
boundary.

## Where things live

| Path | What |
|---|---|
| `src/` | The whole console. `fx.h` and `eclipse_core.h` at the root. `core/` splits by **ownership of state**, and the folders name the split: `core/memory/` is the ONLY code that can touch the client's memory (`stubs.cpp` = the NT trampolines + handle duplication with `make_nt_stub` private; `read.cpp`, `write.cpp` = the read/write layers, separate files on purpose so read code cannot reach the write trampoline). `core/offsets/` is the offset table and its loaders (`offsets.cpp`, `conf.cpp`, `health.cpp`). `core/features/` is the **snippet-facing feature modules** (`world.cpp` = geometry, `input.cpp` = keyboard/mouse + gating, `runtime.cpp` = the run state / sleep-pace clock / log sink, `window.cpp` = the snippet's window+overlay surface, `net.cpp` = the one HTTPS GET in the tree plus `fx_http_get`/`fx_avatar_url`; `ui/client/placename.cpp` wraps it rather than keeping a second copy). Flat in `core/` is **infrastructure** that happens to export a few functions: `eclipse_console.cpp` (attach, DataModel, process discovery, the attach-state accessors `fx_dm`/`fx_base`/`fx_pid`/`fx_armed`), `cache.cpp` (the LocalPlayer/Character + part caches, owns its mutex), `snippet_build.cpp` (the compile/run procedure), and `core_internal.h` (the functions-only seam). **The distinction that decides where a new file goes: a feature module exists to serve snippets; infrastructure serves the console and happens to expose a few `fx_*`.** Every `.h` seam is functions only, no `extern` globals. `ui/` mirrors the same split: `eclipse_ui.cpp` is the host (the WebView2 front end plus the deliberately-unsplit overlay/panel/app-mode machinery), with `host.h` the functions-only seam back into it; flat beside it are `log` (the log funnel, owns the queue/history/file), `dpi` (monitor/DPI + the remembered desktop rect) and `snipwin` (the snippet-driven floating/docked window, owns its state; the overlay side of the dock stays in the host). `ui/client/` is everything about the Roblox client itself — `attach` (attach/identity state, owns the flags and the picker), `placename` (PlaceId -> title via the web API, plus `http_get`), `offsets_fetch` (the per-version conf fetch), `launch` (starting a client). `ui/snippets/` is everything about the user's snippets folder — `tree`, `trash`, `watch`, `bundle` — and nothing else may touch it (see the non-negotiable). `cli/` is the terminal binary, `res/` the icon resources. Both `core/**/*.cpp` build into BOTH targets. |

| **`build/conf/offsets_windows.conf`** | The ONLY offsets file anything reads. Fetched per client version by `ui/client/offsets_fetch.cpp` and written next to the exe; re-fetched whenever Roblox updates. Not in the repo -- there is no seed copy, deliberately (see `tools/stage_conf.cmake`). |
| `web/` | UI. `app.css`, `app.js`, `index.html` are ours. `tokens.css` + `components.css` came from the Eclipse kit — editable, see below. |
| `web/hud.html` | The in-game panel's page. Self-contained apart from `tokens.css`. See "The in-game panel" in `README.md` before changing its window behaviour — four z-order approaches were tried and three rejected for reasons that are not obvious from the code. |
| **`%APPDATA%\EclipseConsole\overlay.txt`, `window.txt`** | Overlay mode's preference, and the app window's ONE geometry (`x y w h`, screen px, both modes). Runtime state, not in the repo. `overlay_app.txt` was the in-game position's separate store and is retired -- see `docs/window-geometry.md`. |
| `docs/snippet-window-flags.md` | An IDEA, not built: giving snippets the app's window behaviours (toolwindow, band rule, position memory) as `fx_window_*` flags. Read before building it -- it records why the full version is not the default answer. |
| `docs/window-geometry.md` | The app window is ONE HWND with ONE geometry: mode switches never move it, `window.txt` is the only store, the in-game anchor is derived at the open. Records the five failed fixes, the startup ordering trap (restore runs before settings arrive, so the file carries sentinels and no flag is read) and what the two switches mean. Read before touching `write_window_rect`, `restore_window_rect`, `app_anchor_capture` or `app_restore_show`. |
| `docs/app-cover-precomposed.md` | An IDEA, not built: keep the close cover composed UNDER the app for its whole life, so the close is hide-then-travel with nothing to wait for. Read it before timing the close leg any harder -- it records why no witness can finish the job (every one reports Chromium's commit, not DWM's display), the 3/36 residual that survives the current fix, and the two constraints that make measuring it possible at all. |
| `docs/hud-transparency.md` | An IDEA, not built: a translucent app window in hud mode, like a docked panel. Read before attempting -- the CSS half is trivial and the window half sits on top of the z-order machinery, and it records which composition route is already known not to work. |
| `docs/overlay-plan.md` | Where the overlay is going (merge the dock into the panel, then a focus-taking hotkey mode, then the app itself) and every dead end already paid for: window shaping, z-order and flicker, ownership bugs, and how measuring this fooled us twice. Read it before touching the panel's window behaviour. |
| `build/` | Output. Staged copies of `web/` and `fx.h` sit here (synced every build). `build/conf/` is NOT staged from the repo -- the app owns it. |
| `build/toolchain/` | The bundled g++ that ships with the app. NOT produced by `cmake --build` — see "Shipping a build". |
| `tools/dump/` | Proof that we can derive offsets from a live client ourselves, with no endpoint. Read its `README.md` before re-attempting — it records four wrong answers that all looked right. |
| `tools/zdup/` | The presented-frame probe: what was actually COMPOSED, per frame, with a z-walk and a gesture driver. The only instrument here that can go red when a `SetWindowPos` succeeds and does nothing — which is exactly what the cold-open blink was. Not a CMake target; build line and the three ways to misread it are in its `README.md`. Reach for it before reasoning about any z-order or paint symptom. |
| `tools/*.ps1`, `tools/installer.iss` | Release machinery: bundle the compiler, verify it, build the installer. |
| `dist/` | The finished installer. |
| **`%APPDATA%\EclipseConsole\snippets`** | The user's live snippets. NOT in the repo. See the rule below. |
| `src/res/app.rc`, `src/res/icon.ico` | The app icon, compiled in. `web/icon.png` is the same artwork for the titlebar; both need replacing when it changes. |

## Non-negotiables

- **The arm gate is safety-critical.** `fx_wrblk` MUST log-and-return-N when
  disarmed. If a change would let a write reach the process in dry-run mode, it
  is wrong regardless of what else it enables.
- **This is not injection.** Nothing compiled or loaded here should touch the
  Luau VM, emit bytecode, install a hook in the game process, or write anything
  the client interprets as code. The BOUNDARY comment in
  `eclipse_console.cpp` is the exact statement to preserve.
- **No RPM/WPM fallback for reads/writes.** The syscall stubs are the only path.
  If they can't be built the attach fails loud; do not add a fallback back in.
- **The `fx_*` extern "C" boundary is C, not C++.** No C++ objects cross it.
  This is what lets a snippet built with its own libstdc++ still call in
  correctly.
- **Never touch `%APPDATA%\EclipseConsole\snippets` outside the user's own
  actions.** It holds real user work (their aimbot, their tuned params). No
  sync, no reset, no rebuild step ever writes there.
- **Snippets are user data, not source code.** The repo does not ship examples,
  and the sidebar starts empty on a fresh install by design. If you write a
  demo snippet for verification, write it into the user's live tree only if
  they asked; otherwise write it in `build/` and delete it after.

## How I should work

- **Act autonomously on reversible code changes.** Small edits, refactors,
  compressing verbose comments — go ahead. Only stop to ask when a choice
  locks something in: picking hysteresis defaults, changing user-visible
  behaviour, structural repo layout changes, anything destructive.
- **Verify with the build**, not with hopeful thinking. After code edits:

  ```
  cmake --build build
  ```

  This produces `build/eclipse_console.exe` (UI) and `build/eclipse_cli.exe`
  (terminal). The app locks its own exe while running — the user closes it
  before building. If a build fails on `Permission denied` for
  `eclipse_console.exe`, that's why.
- **Throwaway harnesses go in `build/` and are deleted when the check passes.**
  Do not add tests or scaffolding to the repo. If a check requires a harness
  (validating a JSON payload, exercising a state machine), compile it out of
  tree, capture the result in the reply, then remove the file.
- **Do not run the app to "screenshot" it or "confirm it works" as a habit.**
  Launch it only when the change is UI-visible or the user asks. A 6-second
  sanity launch that confirms it starts is fine after risky changes; don't do
  it after every edit.
- **Don't touch settings the user has tuned.** The live aimbot's
  `SMOOTHING` / `SWITCH_FRACTION` / `SWITCH_DWELL_MS` are the user's dials;
  never revert them to any default I once shipped.
- **Keep user-facing text short.** One sentence per message, 2-3 lines for a
  block, action first, and match the log level to the content — only a real
  error is an error, a remedy is a WARN. The reasoning belongs in the code
  comment, where whoever changes it will look. This is UI text only; the dense
  comment style below stays.

## When to offer a Fable agent

**Never spawn one unasked — offer it, and wait.** Subagents are expensive and
they edit this tree; the user decides.

Offer one when the work is **deep, self-contained and verification-heavy**, and
especially when I have already iterated three or more times without converging.
That last condition is the honest signal: if the third attempt is a variation on
the first two, more attempts are unlikely to help, and continuing to guess in
front of the user is worse than saying so.

Fits the shape:

- platform behaviour with no clean documentation — compositor, DWM, window
  shaping, input routing, WebView2 hosting
- anything needing a long measure-change-measure loop that would otherwise be
  dozens of build-and-look rounds
- a problem where the *symptoms keep changing* as I fix them, which usually
  means the real cause is a layer below where I am looking

Does NOT fit: small edits, anything touching the arm gate or the BOUNDARY, and
anything where the user is mid-conversation about how it should feel — taste
belongs with them, not with an agent.

**What made the one that worked, work** (the in-game panel's shaping, which
produced the composition-controller host):

- **Hand over every failed approach and why it failed.** Half that prompt was
  the list of dead ends. Without it the agent re-runs the same experiments and
  reports the same wrong conclusions I already paid for.
- **State what must not regress**, by name: focus behaviour, z-order rule, the
  click pre-emption, the message protocol. An agent optimising one axis will
  happily break the other four.
- **Name the suspected answer, and say it is a suspicion.** "Evaluate this, do
  not assume it" gets a real evaluation; asserting it gets a confirmation.
- **Demand an honest limits section.** Ask what it could NOT verify. The good
  report came back with three unverified items; a report with none would have
  been the thing to distrust.
- Tell it to kill the console before building and relaunch after — it locks its
  own exe, and the user keeps it running.

**Confirmed a second time** (2026-09-02, the app-mode close flicker and the
permanent stuck-overlay state, both fixed and live-verified). Same five points,
plus two the second run added:

- **The negative results were worth as much as the fix.** Two suspicions were
  handed over — the overlay's region, then hook re-entrancy — and both were
  killed by measurement (region applies in 4-7ms and never lost in 60 trials;
  0/75 nesting inside `SetWindowPos`/`force_foreground`/`DwmFlush`). Naming them
  as suspicions is what got them tested rather than confirmed. Both were mine
  and both were wrong, which is the point.
- **Feed corrections back the moment they arrive.** "It is an old issue" and
  "stuck as in stuck forever" each arrived mid-run and each invalidated a live
  theory — the second one ruled out every watchdog explanation at a stroke,
  because permanence is a constraint no self-recovering mechanism can satisfy.
  A user fact beats an agent's model; relay it immediately rather than at the
  end.

**Confirmed a third time** (2026-09-03, keyboard focus for a docked snippet
panel, live-verified by typing into one). Same five points, and one the third
run added that is worth more than the rest:

- **A DEAD END IS NOT ALWAYS CLOSED, AND HANDING IT OVER IS WHAT GETS THAT
  TESTED.** The brief said "the panel was wired to take focus for one build and
  reverted -- Windows drew an accent band around it and re-rendered its text".
  The agent did not avoid the approach, and did not re-run it blind: it built a
  border probe and measured that the band is a property of a DWM FRAME, that
  the composed host has none, and that the band therefore belonged to the
  WRAPPER-HOSTED fallback, not to focus. Positive control included -- a real
  captioned window went red on all four sides, so the check could visibly fail.
  The dead end was real when it was written and context-specific in a way
  nobody had noticed.
- The other half of why it was cheap: the brief pointed at the mechanism that
  ALREADY EXISTED (`overlay_focus_enter`/`exit`, entered by `HUD_FOCUS:1`,
  solved for app mode 2026-08-21) instead of describing the goal. The work came
  back as plumbing -- an injected-shim change plus a relay -- with no new Win32
  and no new `fx_*` export.

## Verification: the part I keep getting wrong

Every wasted hour in this project has come from a check that could not fail,
not from a missing check. Written down because it recurs:

- **`ExtractAssociatedIcon` reports the Windows default icon**, so it said
  "icon present" for an exe with none. Enumerate `RT_ICON` instead.
- **Icon byte-SIZES are identical** between two different images of the same
  dimensions, so a size comparison "passed" while the exe still held the old
  artwork. Hash the resource bytes.
- **`MainWindowHandle` picked the overlay**, not the app window, and
  `GetClassNameW` without `CharSet.Unicode` truncated every class name to one
  character — so the diagnostic was wrong twice over and the code was fine.
- **A JScript parse-check rejected the known-good shipped `app.js`**, so it
  proved nothing. Always run a check against a case you KNOW is good.
- **A comment claimed the loop was "paced against a deadline"** while the code
  was a flat `Sleep`. Measurement showed 110 Hz at stddev 1.25 ms where it
  claimed 144. Comments are not evidence.

So: **prove the check can fail.** Corrupt an offset and watch the health check
report it; measure the old path against the new one; feed a known-good input
and confirm it passes. A green result from a check that cannot go red is worse
than no check, because it stops the search.

When something reads wrong in-game, prefer the probes over reasoning:
`eclipse_cli --check` (offset health) and `EclipseAimbot_probe.exe` (world
layer) both print values a human can call obviously-right or obviously-wrong.

## Reading the game: what actually breaks

- **Offsets are values, but SHAPES change too.** `Instance.Name` moved behind
  `NameContainer` and no offset value could express it; the code had to change.
  Suspect the shape when reads fail selectively — names empty while classes
  work, or vice versa.
- **Read widths matter.** `Misc.Value` is a `double`. Reading it as a `float`
  returns 0.0, which looked exactly like a dead player.
- **Pointer plausibility must include module addresses.** Class descriptors
  live at `0x7ff6xxxxxxxx`; a bound of `0x7FF000000000` (right for heap part
  pointers) silently rejected all of them.
- **Bound every walk over a game-derived count.** A stale instance yields two
  unrelated pointers and `(end - begin) / 0x10` comes back in the billions;
  that is a hang, not a wrong number. See `FX_MAX_CHILDREN`.
- **Filters over memory reads must fail OPEN.** A health filter that treated an
  undecodable `0.0` as "dead" removed every player in a normal game. If a
  reading cannot be corroborated, include the target and move on.

## Simple beats clever

An in-flight-movement ledger was added to the cursor aim to cancel dead time,
then patched twice — first because it drove the aim backwards, then because the
clamp made it drive nothing at all, so the aim snapped once and stalled.
Deleting it fixed the problem: correcting a fixed fraction of the CURRENT error
every tick self-corrects whatever the latency is.

If the user says "just make it work like X", port X's logic rather than
improving on it. Add machinery only after the simple version is measured and
found wanting.

## The sibling app

`C:\Users\user\downloads\veaim` — **Eclipse Aimbot**, a standalone C++/WebView2
app (exe is `EclipseAimbot.exe`; the folder name is stale and the user does not
care). Same brand, same UI kit, same offsets endpoint, and it reuses this
tree's WebView2 SDK via a path list in its `CMakeLists.txt`.

It is NOT this console: plain `OpenProcess`/`ReadProcessMemory` by choice, and
it writes one field (`Camera.Rotation`) behind its own arm gate. Rig support is
table-driven, so a game with a custom character (Bad Business) is a table row.
Its `README.md` carries the hard-won details; read it before changing anything
there.

Changes often belong in both — the lead cap, the deadline pacer and the
key-down/focus split were all fixed in one and needed porting to the other.

## Build-system traps, all hit at least once

- **`.rc` files do not track their `.ico`.** Editing the artwork alone left the
  resource stale and the build reported success. Both projects now declare
  `set_property(SOURCE src/res/app.rc PROPERTY OBJECT_DEPENDS ...)`.
- **The window icon is separate from the exe icon.** Explorer uses the compiled
  resource; the taskbar and Alt-Tab need `WM_SETICON` on the window.
- **`CMakeCache.txt` stores absolute paths**, and so does
  `build/_deps/*-subbuild`. After moving the tree, delete both caches — but
  never `build/` wholesale, because `build/toolchain` is the 319 MB bundled
  compiler that `cmake --build` cannot reproduce.

## The two binaries, briefly

Two subsystems, one core. `eclipse_console.exe` is GUI subsystem so launching
from the shell doesn't flash a console; `eclipse_cli.exe` is console subsystem
so `--run FILE|FOLDER` works properly from a terminal (ordered output,
Ctrl+Break is a real event). Both build from the same sources and export the
same `fx_*` set. Users should invoke the CLI binary from a terminal — using
the GUI one via `AttachConsole` is subtly broken (prompt doesn't wait, Ctrl+C
goes to the shell).

## Comments in this codebase

Comments here are deliberately dense and explanatory — that's the style. They
document constraints, dead ends ("was tried, doesn't work, don't retry"),
"why not the obvious alternative", and measurement conclusions.

**Keep, always:**
- Constraints and invariants.
- Dead-end notes (someone will otherwise re-attempt them).
- The safety statements (arm gate, BOUNDARY, "collapsing is not filtering",
  "never test fx_gen against 0").
- Conclusions of measurements.

**Drop only:**
- Changelog-style history ("used to be", "the first version did") when the
  code stands on its own now.
- Restatement of what the code obviously does.
- Supporting detail behind a measurement, once the conclusion is stated.

If in doubt, keep. A prior pass already removed the genuinely useless material.

## Common tasks

**Add or fix an offset.** There is no local source to edit. The conf is fetched
per client version by `ui/client/offsets_fetch.cpp` and written to
`build/conf/offsets_windows.conf`; a wrong or missing value there comes from the
endpoint's dump for that version and is replaced on the next Roblox update.

So the only durable fix is code that does not depend on the number, and the
pattern is: read a value you can TRUST, then use it to check the key. The
trustworthy value for gravity is `Workspace.ReadOnlyGravity` -- a read-only
mirror, so it still holds the true figure even when the live field has been
zeroed.

`fly.cpp`'s `find_gravity()` gets HALF of that, and the missing half is worth
knowing before relying on it. It range-checks the `World` pointer and takes the
mirror as the RESTORE value -- which is why a run that died without restoring
cannot poison the next one. It does NOT verify that `world + World.Gravity`
actually holds the mirror's value, and it does not search for the value when it
does not. A wrong `World.Gravity` therefore still reaches an armed write to an
address that was never right, and the `live != mirror` branch misreports it as
"a previous run left it that way" and writes the true gravity into an unrelated
float on exit.

Not built because the endpoint's value came good again (2026-09-03) and the
simple version has not yet been measured and found wanting -- but the endpoint
"fails sometimes" in its author's own words, so this is the shape to reach for
when it next does: verify the key against the mirror, and only then fall back
to searching the `World` object for it. Note the search is not unambiguous --
`0x228` and `0x22c` BOTH held 196.2 on this build and only `0x22c` drove
physics -- so a fallback that picks a candidate must say loudly that it
guessed, and refusing is better than guessing wrong.

UNVERIFIED is a much narrower net than it looks. `dm_verified` is exactly
`class_of(dm) == "DataModel"`, so it only exercises three offsets:
`FakeDataModel.Pointer`, `Instance.ClassDescriptor` and
`ClassDescriptor.ClassName`. Everything else — `Instance.Name`, all of
`Camera.*`, `Primitive.*`, `Humanoid.*` — is unchecked, and a wrong value
there reads as plausible garbage with the status line still green. Verified
by corrupting `Instance.Name` and watching it report DataModel verified.

That is why the CLIENT VERSION check is load-bearing rather than a
convenience: it is the only thing standing between a conf for the wrong
build and an armed write to an address that was never right. `run_snippet`
and `run_project` both refuse when `offsets_stale` is set.

**Overlay mode ("Run inside Roblox").** Settings -> Console. On, and with a
client attached, the console LIVES in the game: no desktop window, a badge at
the right edge, `Insert` or the badge opens the console's OWN WINDOW over the
client (`WS_EX_TOOLWINDOW`, off the taskbar), clicking the client closes it.
The badge, the panel and docked snippet panels stay in the composition
overlay; the app is a real HWND, and that split is deliberate --
`docs/overlay-plan.md` step 3 says why, and carries the day's dead ends. The
one that generalises: **if the panel already solves an input or z-order
problem, port ITS mechanism.** App mode reacted to foreground events where the
panel uses the `WH_MOUSE_LL` hook, and every symptom -- clicks on the client
doing nothing, the app landing under it, the close animation showing the game
-- was that one lateness.

**Change what the UI shows or handles.** `src/ui/eclipse_ui.cpp` is the WebView2
host and the message pump; `web/app.js` is the page. Messages cross via
`window.host_send()` and `window.__onHostMsg()` with `TYPE:body` framing.

**Change styling.** `tokens.css` and `components.css` came from the Eclipse kit
but are ours to edit — nothing fetches or regenerates them, the build only
copies `web/` into `build/web/`. Still reach for `app.css` first when an
override will do: kit rules are shared, so changing one to suit a single
dialog moves every other user of it. Edit the kit when the change genuinely
belongs to the component — a wrong token, a rule no caller wants — rather than
to dodge writing an override.

**Ship a build.** In order, from the repo root:

```
cmake --build build
powershell tools\bundle_toolchain.ps1     # only when the compiler changes
powershell tools\verify_toolchain.ps1
powershell tools\build_installer.ps1
```

Bump `AppVersion` in `tools/installer.iss` first. The `AppId` GUID is fixed, so
a new version upgrades in place instead of installing alongside.

Three things about this that are load-bearing rather than incidental:

- **The bundled toolchain must share the host's C runtime.** The UI captures a
  snippet's `printf` by rebinding fd 1, which only works while the snippet DLL
  and the host use the same CRT. A UCRT host loading an MSVCRT-built snippet
  gets two independent stdio states and the output vanishes with no error at
  all. That is why `bundle_toolchain.ps1` copies the compiler that built the
  host rather than downloading a convenient one, and why
  `verify_toolchain.ps1` compares their imports. Do not swap in a different
  toolchain without re-running that check.
- **`verify_toolchain.ps1` compiles with the dev install off PATH.** A bundle
  that is only complete because `C:\mingw64` happens to be reachable looks
  identical to a working one until it ships. Three things were missing this
  way and each failed silently: `liblto_plugin.dll` (the driver passes
  `-fuse-linker-plugin` by default), `as`/`ld` needing the bundle's `bin` on
  the child's PATH (an absolute path to `g++` is *not* enough), and
  `libwinpthread-1.dll`, which snippet DLLs import and which must sit beside
  the **exe**, not in `toolchain\bin`, because the host is what `LoadLibrary`s
  a snippet.
- **The installer is per-user on purpose.** The app rewrites
  `conf\offsets_windows.conf` next to its own exe on every Roblox update. Under
  Program Files a standard user cannot write there, the fetch fails, and since
  running is blocked while offsets are stale the app is unusable without ever
  saying why. Moving to Program Files means moving the conf to `%APPDATA%`
  first. Uninstall deliberately leaves `%APPDATA%\EclipseConsole` alone.

When there is no bundle, the compile falls back to whatever `g++` is on PATH —
that is the dev setup, and why `cmake --build` alone still works.

**Change what a snippet can call.** `src/fx.h` is the surface (users see it as
documentation — be conservative with comments there). Define the export in the
TU that OWNS the state it exposes, not in `eclipse_console.cpp` by default: a
read wrapper in `memory/read.cpp`, an offset one in `offsets/offsets.cpp`, a
world/input/runtime/window one in the matching `core/features/` file, an
attach-state accessor with attach. `fx.h` gives every export C linkage, so no
`extern "C" {}` wrapper is needed — include `fx.h` and define it. `fx_wrblk`
stays in `memory/write.cpp`: the write trampoline is private to that TU, which
is what keeps the arm gate unbypassable. To see where the exports actually live,
`grep -rn "^FX_API" src --include=*.cpp` is the authority.

**Verify a change against the live client.** The user launches Roblox; you
build; the user runs. Attach happens automatically once both are up. Do NOT
try to script a Roblox launch.

## What the README covers that this file doesn't

- The full port-mapping table from Linux to Windows.
- The offset-file format and the four fields whose names don't mean what they
  suggest (`MouseService.SensitivityPointer`, `TaskScheduler.MaxFPS`,
  `Camera.FieldOfView`, `Humanoid.PlatformStatePointer`).
- How the syscall stubs and handle duplication actually work
  ("How it reads and writes" section).
- The snippet mechanics: groups, projects, watched files, the dry-run
  collapsing behaviour, hotkey loops, `fx_gen` for stale-pointer invalidation.

If a question touches one of those, read the README first rather than
reconstructing from memory.
