# A pre-composed cover for the app close

An IDEA, not built. Written 2026-09-07 because the close flicker is now rare
rather than gone, the remaining path is structural, and the next person to look
at it should not re-derive why timing it harder cannot finish the job.

Read `docs/app-close-flicker.md` first — this is the sequel to its 2026-09-07
section, and every measurement quoted below is from that run.

## Where it stands

The close is `APPCLOSE` (host readies the box) -> `APP_BOXUP` (page says the box
is up) -> host hides the window -> `APPGO` (page travels the box home). That
order is right and is not what is wrong.

Measured, live, with a DXGI desktop-duplication probe sampling a 7x4 grid inside
the app's composed rect per presented frame:

| | bare-game frames |
|---|---|
| idle, 28 closes, every entry point | **0** |
| 90% busy iGPU, 9 closes (before) | **4** — 1-2 whole-rect frames, up to ~110ms |
| same load, after the witness change | **0/8 loaded, 0/4 idle** |
| residual, across the whole synthetic-load set | **3/36** |

The z-walk read `AHG -> HG` on every close, loaded or not: the client never
crosses the app. It is a paint race, not a z-order one.

**And the user still sees it.** Reported after the fix shipped: *"it still
somehow blink on a very special sequence occasion that i cant even remember
nor replicate when wanted."* That is consistent with a 3/36 tail — rare, load
dependent, and not reproducible on demand. Do not treat the fix as failed; treat
it as incomplete.

## Why timing it harder cannot finish it

**Every witness available to this process reports SUBMISSION, never DISPLAY.**
That is the whole problem, and it is not a gap in our implementation:

- A double `requestAnimationFrame` proves Chromium drew the box. Under load a
  rAF is paced at one DWM pass, exactly like a flush.
- **Four rAFs are not a witness**: 0/18 bad, then 2/8 at higher load.
- **Six `DwmFlush` reach zero** — at 141-234ms of blocking per close, which is a
  visible stall traded for an invisible flash.
- **Element Timing's `renderTime` is Chromium's commit too.** In a failing close
  it reported presented at +92ms while the display carried bare game until
  +289ms. On a starved GPU, DWM keeps composing the overlay's OLD surface until
  the new one's GPU work completes, and nothing this side of the fence can see
  that.

The code already says this in one line, over the two `DwmFlush` in
`app_close_finish`: *"no public API can prove a foreign process's commit is on
glass."* Those two flushes are measured slack (8/40 bad without, 0/40 with) — if
the flash ever widens, widen them; never delete them.

## The idea

**Stop needing the verification.** Keep the cover composed underneath the app for
the app's whole life, not readied at close time.

The app is a separate HWND above the overlay, so a rect painted at the app's
position in the overlay page is invisible while the app is up — the app covers
it — and is revealed the instant the app hides. At close time there is nothing
to ready and nothing to wait for: the cover has been on glass for seconds, so
the hide cannot expose the game no matter how starved the GPU is. `APP_BOXUP`
stops being a promise about the future and becomes a fact about the past.

The close then reduces to: hide the window, then travel the cover home.

## What it has to satisfy

- **The z-order rules are load-bearing and asymmetric on purpose**:
  badge-above-app, dock-behind-app. The cover lives in the same overlay page as
  both, so its stacking inside that page has to leave the badge reachable and
  the dock where it is.
- **It has to track the app.** `hud_tick` re-glues the app to the client on
  every client move and resize; the cover needs the same rect, or a client drag
  slides the app off its own cover.
- **The open leg needs the mirror, or the flash just moves.** The open already
  has its own paint witness (`APPPAINT` -> double rAF -> `APPPAINTED`) gating
  one promote — see the must-not-regress list in `docs/window-geometry.md`.
- **A permanently composed full-size surface has a cost** that has not been
  measured: overlay surface memory, and whatever it does to the composition
  controller's own paint load while the app is open.
- The alternative shape, if the page turns out to be the wrong home for it: a
  **host-owned DirectComposition cover**, drawn by us rather than by the overlay
  page. Heavier to build, but it removes the page from the close path entirely.

## Measuring it, when someone does build it

`tools/zdup/` is the instrument — the presented-frame probe with the z-walk and
the gesture driver, the only thing here that goes red when a `SetWindowPos`
succeeds and does nothing. Its README carries the build line and the three ways
to misread it.

Two constraints on the measurement itself, both learned the expensive way:

- **The failure needs LOAD.** Idle runs were 0/30 either way, before and after.
  A green idle run proves nothing. The 2026-09-07 run used synthetic whole-iGPU
  starvation, which is *not* the realistic shape — a busy dock is — so the
  user's real closes have never been counted on any build.
- **Desktop duplication turns MPO off for the run**, and this display is
  MPO3-capable. A plane blink would be invisible to the probe. If a blink
  survives everything here, test it with a screen recorder running.

And the trap that nearly cost the last run its result: **a SendInput gesture
driver measures nothing trustworthy in this subsystem while a human is also
moving the foreground.** Every close/return decision is a foreground test. See
the annotation on the badge-click entry in `docs/app-close-flicker.md`.

## One correction carried over

`app_close_finish` and `app_abort_restore` used to name a "close-gesture cover —
the temporary TOPMOST the mouse hook grants". **That mechanism does not exist**;
the hook-time TOPMOST lift was removed when the band rule replaced it. Both
comments are fixed. Do not go looking for it.
