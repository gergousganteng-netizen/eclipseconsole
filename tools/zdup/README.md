# zdup — the presented-frame probe

What was actually **composed on screen**, sampled per presented frame, alongside
a z-order walk and a gesture driver. Built 2026-09-04 for the overlay open-leg
blinks and kept because this is the only instrument in the tree that can answer
"what did the user see", as opposed to "what did this process ask for".

That distinction is the whole reason it exists. `eclipse_z.log` records our own
`SetWindowPos` calls and their flags; it cannot see a call that **succeeds and
does nothing**. The cold-open fault was exactly that — Windows' topmost entry
gate swallowed `app_swp(TOPMOST)` and returned `ret=1` with the style bit set,
so every style-bit check read green while the app sat in the normal band. Only
presented frames went red.

## Build

Not wired into CMake — it is a diagnostic, not a target. From this directory:

```
g++ -O2 -std=c++17 zdup.cpp -o zdup.exe -ld3d11 -ldxgi -ldxguid -luser32 -lgdi32 -lole32
```

The bundled toolchain works: `..\..\build\toolchain\bin\g++.exe`.

## What it writes

Three streams per run, all stamped `GetTickCount64 % 1e8` so they line up with
`%TEMP%\eclipse_z.log` (run the console with `ECLIPSE_ZLOG=1`):

| file | contents |
|---|---|
| `*.px` | one line per **presented** frame: the colour at each sample point |
| `*.z`  | z-order walks — `A`pp, `G`ame, `H`ud, in front-to-back order |
| `*.g`  | the gestures it drove, with timings |

Sample points are the app titlebar and body, the badge, the dock, and **two
game-only control points** (`gm1`, `gm2`). The control points are the positive
control: they read the game's own background, so a claim of the form "this pixel
is the bare game" can be checked rather than assumed.

`coldloop.ps1` drives the restart-per-sample loop; `zdup_cold_an.py` and
`zdup_warm_an.py` reduce a capture to frame counts.

## Reading a result honestly

Three traps, all hit at least once while this was in use:

- **A zero means nothing without its control.** A probe aimed at the wrong rect
  produces clean zeros forever. Always report app-painted frames at the sample
  points beside the fault count — the cold set ran 218–472 painted frames per
  capture, and that is what makes its `0` mean "no fault" rather than "nothing
  was there".
- **Derive rects live, never from a remembered value.** The app's position is
  user-settable and moves between runs. A stale rect silently mis-samples the
  first open of a capture.
- **The badge has a hover state.** It reads `151519` under the cursor and
  `101013` otherwise; neither is a fault. The fault signature is a sample point
  transiently equal to the *game*, which is why the badge is the cleanest
  witness — it is dark whenever it is drawn at all, so it cannot be confused
  with a frame taken before the app opened.

## A cold open happens once per console process

There is no way to take two cold samples without restarting the console in
between, which is what `coldloop.ps1` is for. It also means a **docked snippet
cannot be present on a cold open** without a hand-driven F5 between launch and
the first open — the box-drop half of that case is still unmeasured, recorded in
`docs/overlay-plan.md`.
