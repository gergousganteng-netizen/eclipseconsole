# The app-mode CLOSE flicker

**Status: the CLOSE FLICKER is fixed and live-confirmed (2026-09-02)** — the
race was the page's box frame reaching GLASS vs the window hide (submission vs
presentation, which the double-rAF cannot see); the fix is two `DwmFlush`
calls before the hide in `app_close_finish`, mirroring the open path.
Reproduced 8/40 and fixed 0/40 in a mimic harness under load; see "2026-09-02:
reproduced, measured, fixed" below, including why the REGION suspect is dead.
**Testing the fix surfaced an OLD, separate bug — the permanent
dead-rectangle wedge on open-then-spam-click — diagnosed and fixed the same
day**; see the final section. The history stands as written — this file
records a wrong diagnosis of the wrong PATH, a theory that was convincing on
two samples and wrong on six, and a sampler that could not fail — all
confident, all costly.

Written 2026-08-30, frame-by-frame evidence and the region suspect 2026-08-31,
harness reproduction and fix 2026-09-02.

---

## The symptom

Overlay mode on, attached, app mode. **Closing** the app: Roblox flashes to the
front, *then* the close animation plays. Intermittent — "not as often but still
annoying".

The user also notes it is not about interacting with the app: having given focus
to a third-party application beforehand is enough to set it up.

---

## WHAT THIS IS NOT — a wrong diagnosis, recorded so it is not repeated

An earlier pass (same day) read the phrase "opening it" in the first report and
diagnosed the **open** path in full, with a trace, convincingly, and wrote it up.
It is the wrong path. The symptom is on CLOSE.

That open-path finding is real in itself and may matter later, so it is kept
here rather than thrown away:

> Four consecutive OPENS, two flickered and two did not, differing by one event:
> **Roblox re-asserts foreground by itself ~9 ms after our `force_foreground` in
> the open path succeeds** (`clicked=0` in the `APP stays` line proves no click
> caused it). The band rule's `APP band -> TOPMOST (client fg)` a tick later is
> the recovery. It is the "foreground steal" mechanism Fable flagged as
> unaddressed on 2026-08-27, at ~9 ms rather than ~1 s.

Whether that has anything to do with the close flicker is **unknown**. Do not
assume it does.

## What has been RULED OUT for the close path

From an 18-close trace (`ECLIPSE_ZLOG=1`):

- **The box is never late.** `APP_BOXUP` always precedes `APP closed`, in all 18
  closes, by between +3.8 ms and +55.5 ms. Never inverted. So "the window hides
  before the page's covering box is up" is not it.
- **The overlay band is not contested during a close.** Across five consecutive
  closes there were NO `overlay -> NOTOPMOST`, no `overlay -> TOPMOST`, no heal
  lines at all. The overlay holds topmost throughout.
- Every close in that trace has the identical shape:

```
 264481.44  APP close gesture (hook: click on client)
 264482.08  APP close -> APPCLOSE:370,150,1180,780 (hand=0)
 264483.73  MSG <RECTS:...;~370,150,1538,780>
 264486.45  BAND-DIAG ... fgw=<game> fgpid=<game>
 264487.22  MSG <APP_BOXUP>
 264490.09  ZWATCH APP after=1409a2 flags=181B owner=<game> fg=<game>
 264492.62  APP closed
 264515.93  BAND-DIAG ... app=0
 264517.17  APP owner -> 0
 264695.67  MSG <ANIM:app-close frames=28 dur=194>
```

Which means **the trace does not contain a flickering close**, or the flicker
leaves no trace in the events currently logged. Both are possible and they lead
different places.

---

## The marked capture WAS done — and found nothing (2026-08-30)

Six closes, all by clicking the client, user marked **1 and 6 as flickering**,
2-5 clean. Every logged discriminator was checked and NONE separates them:

| close | BOXUP->closed | owner-drop vs game-fg | ANIM | flickered |
|---|---|---|---|---|
| 1 | +3.2 ms | owner first | 25f/198ms | **yes** |
| 2 | +5.2 ms | owner first | 28f/199ms | no |
| 3 | +4.2 ms | game first  | 27f/197ms | no |
| 4 | +10.7 ms | owner first | 28f/199ms | no |
| 5 | +46.6 ms | game first  | 28f/193ms | no |
| 6 | +6.0 ms | owner first | 28f/198ms | **yes** |

- **Box timing does not correlate.** Close 1 (flickered) has the SMALLEST gap;
  close 5 (clean) has the largest, ten times bigger.
- **Ownership ordering does not correlate.** A first pass on closes 1 and 3 alone
  suggested "owner dropped before the game's FG event" was the tell — checking
  all six killed it: closes 2 and 4 share that ordering and were clean. Recorded
  because it is convincing on two samples and wrong on six.
- **The overlay band is never touched** during any close.
- The event sequences are otherwise identical.

**So the conclusion is negative and it is worth something:** the flash is not a
z-order request this process makes. Every request is the same on the closes that
flash and the ones that do not. `zlog` records what we ASK for and `ZWATCH`
records `WM_WINDOWPOSCHANGED` — neither can see *when DWM actually presents*,
and that is the remaining suspect: a composed frame landing between the app
window hiding and the overlay's box being presented.

### What that implies for the next attempt

Do NOT collect another marked zlog capture of the same events. It has been done
and the answer is "no difference". Either:

- instrument at the presentation layer (DWM frame timing / a capture at high
  frame rate showing which frame the game is visible in), or
- change the design so the race cannot exist rather than trying to observe it —
  e.g. keep the app window visible until the box is provably presented, rather
  than hiding it a few ms after asking the page to draw one.

### The gesture is NOT part of it (tested 2026-08-30)

All three close gestures were tested, 6 closes each: **click-the-client, the
badge, and Escape all flash**, at a similar rate (roughly one in three). So
`hand_to_game` and the gesture-specific branches are innocent, and the race is
the same however the close is reached. Do not go looking there.

### FRAME-BY-FRAME VIDEO — the decisive evidence (2026-08-31)

A 30 fps screen recording of a flashing close, stepped frame by frame:

1. the app window, fully visible over the game
2. **the app is GONE and NOTHING covers it — the game is fully visible.** This
   held about TWO frames at 30 fps, so roughly **60-70 ms**
3. the travel box appears and the close animation runs normally

The box in frame 3 is BLACK because that is what the animation looks like —
`#apptrav` is opaque `--bg-1` with no fade. It is NOT an unpainted surface; an
earlier reading of these frames said so and was wrong.

So the finding is only the gap, and the gap is what matters: **between the app
window hiding and the travel box being visible, several composed frames show
bare game.**

### Two fixes that are already ruled out — do not propose them again

- **Double-`requestAnimationFrame` before `APP_BOXUP` IS ALREADY SHIPPED.**
  `web/hud.html`, `startAppClose()`: the box is placed inside `still()` (so
  transitions are off and a reflow commits it), then TWO rAFs are awaited before
  the message goes. The page really has painted. This was independently
  re-derived as "the fix" and found already present — check before proposing.
- **Nothing in our timeline is late.** `ANIM` is logged at the animation's END
  with its duration, so the start is derivable with no new capture. Five closes
  from the gesture-test log:

  | | gesture->BOXUP | BOXUP->closed | closed->ANIM start | frames |
  |---|---|---|---|---|
  | A | 31.2 ms | 9.9 | 3.5 | 28f/195ms |
  | B | 24.6 | 17.5 | 9.1 | 28f/194ms |
  | C | 25.9 | 16.8 | 10.1 | 28f/196ms |
  | D | 35.8 | 20.9 | 4.3 | 28f/195ms |
  | E | 23.8 | 18.9 | 8.3 | 28f/195ms |

  Every close hits 28/28 frames at the full duration, and the animation starts
  3-10 ms after the window hides. **There is no 60 ms anywhere in the timeline.**
  No message is late, no frame is dropped. Adding a delay or a paint handshake
  would be treating a lateness that does not exist.

### THE LEADING SUSPECT: the overlay's REGION, not its pixels

`src/ui/overlay.cpp:1093`, in the code's own words:

> A rect prefixed "~" is SOFT: it joins the region (**DWM clips composited pixels
> to the region, so the travel box must be inside it to be drawn**)

That is the constraint the whole thing hangs on. The travel box lives in the
overlay window, and **the overlay window only shows pixels inside its region.**
On a close the page reports the travel corridor as a soft rect and the host calls
`hud_set_rects` -> the region grows to include the app's rect. In the trace that
lands ~20 ms before the window hides:

```
324345.43  MSG <RECTS:1864,507,44,66;~370,150,1538,780>
324347.48  RECTS -> 1 solid rects      <- region now includes the app rect
324354.92  MSG <APP_BOXUP>
324364.77  APP closed                  <- app window hidden
```

So the ORDER of our requests is right, every time. But a region change on a
composition-hosted window is applied by **DWM on its own schedule**, and nothing
here waits for or observes that. If the region grow and the window hide land in
different composition passes, the overlay is still clipped to the badge at the
moment the app disappears — and the game is bare until the next pass. That
reproduces every property of the bug at once:

- the right DURATION (a few composed frames, not one)
- INTERMITTENT, with identical logs either way (it depends on where a DWM pass
  falls, which no instrument here records)
- INDEPENDENT OF GESTURE (all three gestures take this path)
- unaffected by the page painting earlier, which is why the shipped double-rAF
  did not help

**UNVERIFIED, and this is the thing to test rather than assume:** that region
updates on this window are asynchronous with respect to `ShowWindow`, and that
the lag can reach ~60 ms. Directions worth weighing, in the order they seem
promising:

1. Never SHRINK the region back to the badge while app mode is open, so the
   corridor is already in the region long before any close — the grow stops being
   on the close path at all. Cost: a larger composited region while open.
2. Order the two operations against a composition boundary rather than against
   wall-clock (`DwmFlush`, or a commit/present handshake) so the hide cannot
   precede the region taking effect.
3. Confirm the mechanism first by INVERTING it: delay the region grow
   deliberately by ~100 ms and check the flash becomes reliable rather than
   intermittent. **A test that can go red is the point** — this file already
   records one theory that was convincing on two samples and wrong on six.

## Older notes: the first capture, and why it failed

**The trace has no marker for which close flickered.** Eighteen closes, all
looking alike, and no way to tell which one the user saw flash. That is why this
is still undiagnosed — not a shortage of data.

So the next capture must pair the two:

1. Relaunch with `$env:ECLIPSE_ZLOG = "1"` and a fresh `%TEMP%\eclipse_z.log`.
2. Open and close the app repeatedly, **counting out loud**.
3. Note which closes flickered — "1st and 4th flickered, 2nd/3rd/5th clean".
4. Then diff a flickering close against a clean one from the SAME log.

The whole diagnosis is in that difference, exactly as it was for the open path.
Without the marking, every close looks identical and nothing can be concluded.

If several marked flickering closes look byte-identical to clean ones in the
log, that is itself the finding: the flash is happening somewhere the current
instruments do not watch — most likely in the compositor/DWM rather than in any
`SetWindowPos` this process issues — and the next move is a different instrument,
not more of this one.

## Instruments available

Both compiled in permanently, both free when `ECLIPSE_ZLOG` is unset:

- **`zlog`** — what this process ASKS for, with timestamps. `APP close gesture`,
  `APPCLOSE`, `APP_BOXUP`, `APP closed`, `APP owner ->`, `overlay ->`,
  `BAND-DIAG` (watch `fgpid`), `ANIM:app-close`.
- **`ZWATCH`** — every z-order-changing `WM_WINDOWPOSCHANGED` on the app and the
  snippet windows, with `insertAfter`, flags, `GW_OWNER`, foreground **and a call
  stack as module+offset**. `eclipse_console+…` frames above `user32` prove the
  move is ours; a pump-only stack proves it is external. This is what named the
  2026-08-27 owner-group restack in one reading.

## What must not regress

- App mode closes when the GAME takes foreground and **only** then; alt-tabbing
  to a third app leaves the console where it is.
- The `WH_MOUSE_LL` click pre-emption, the band rule, `WS_EX_NOACTIVATE` before
  the first `ShowWindow`, the panel never taking focus.
- `hud_swp`'s ownership bracket covering ALL moves including TOPMOST
  (`docs/overlay-plan.md`, 2026-08-27).
- The snippet window staying focusable in app mode.
- The arm gate, the BOUNDARY guarantee, `fx.h` staying C, multi-run phase 1/2,
  `g_hud_last_mux`.
- `%APPDATA%\EclipseConsole\snippets` is USER DATA — never write there.

## The lesson worth keeping

A convincing diagnosis of the wrong thing costs more than no diagnosis, because
it comes with a plan. **Confirm which path the symptom is on before reading a
trace**, and make the user's report name it unambiguously — "opening it" in the
first message meant the app was open, not that opening was when it flashed.

---

## 2026-09-02: reproduced, measured, fixed

Done entirely in a throwaway harness (`build/rgnrace2.cpp`, deleted per the
harness rule; design recorded below so it can be rebuilt in an hour, not a
session): three windows mimicking the real ones — a flip-model D3D swapchain
presenting green at full rate as the GAME, a plain blue popup as the APP, and a
real composition-hosted WebView2 as the OVERLAY, set up exactly as
`composed_create` (NOREDIRECTIONBITMAP, dcomp visual, transparent background,
rasterization 1.0), whose page reproduces `startAppClose` byte-for-byte: show
the box, reflow, double-rAF, post BOXUP. The host then runs the close per mode
and a sampler classifies what is on glass at the app-rect centre: green = bare
game = the flash.

### The REGION suspect is dead — measured twice

- A dcomp-only harness first: `SetWindowRgn` grow, then hide, same thread.
  The grow APPLIES in 4–7 ms (measured by growing 100 ms late and timing the
  pixel), and in 60 trials across three orderings — grow 20 ms before the
  hide, grow immediately before, grow with two DwmFlush between — the hide
  NEVER presented ahead of the region. The region cannot produce a 60 ms gap;
  it cannot even produce a one-frame gap when the calls share a thread.
- The full WebView2 harness repeated the real region choreography (parked at
  the badge, grown just before the box is requested) — 0/30, and later, under
  the load that DOES reproduce the flash, region choreography vs region-always-
  full made no difference. The region was never the variable.

### A sampler that could not fail — the project's recurring lesson, again

The first pixel sampler was `GetPixel` on the screen DC. It "proved" the
detector could not go red: an inversion that HAD to show bare game showed
`red@0.1ms`. `GetPixel` blocks for a DWM sync and returns the END state, so a
20–40 ms transition vanishes inside one call and the recorded timestamp is
stale. Every clean result it produced was worthless. Replaced with **DXGI
output duplication** — a thread acquiring every frame DWM actually PRESENTS,
stamped with its present QPC, copying out the test pixel. That is the
presentation-layer instrument the 2026-08-30 entry said was missing, and it
was validated the only way that counts: the inversion (hide first, request the
box after) went red — single bare-game presented frames on an idle system,
40/40 with spans up to 60 ms under load.

### The reproduction — it needed BOTH load and size

| conditions | hide on BOXUP+10ms (shipped shape) | + DwmFlush x2 (fix) | inversion |
|---|---|---|---|
| idle, 1000x700 windows | 0/30 | 0/30 | 7/30, ~1 frame |
| loaded renderer OR gpu, small windows | 0/30 | 0/30 | 30/30, ~15 ms |
| loaded renderer + gpu + fullscreen game/overlay | **8/40, bare game up to ~35 ms** | **0/40** | 40/40, up to 60 ms |

Load was a rAF-driven 1600x1200 canvas repaint in the overlay page (standing in
for the shared browser/GPU process being busy) plus a copy-engine hammer thread
(standing in for Roblox). Size was the monitor-covering game and overlay the
user actually runs. Neither alone reproduced — which is why every desk test
ever looked clean, and possibly why MPO-eligible geometry matters (fullscreen
flip-model + size is what earns hardware planes; whether the flash needs the
plane machinery or just the bigger compose cost is NOT separated, see limits).

### The mechanism, stated plainly

`APP_BOXUP` after two rAFs proves the box frame was **submitted to Chromium's
compositor**. It says nothing about DWM having **presented** it, and under GPU
load those are tens of milliseconds apart. The hide is a win32 state change
that DWM picks up on its next pass — so a loaded pipeline lets the pass with
the hide overtake the box frame still queued in the WebView2/GPU-process leg,
and bare game is composed until that frame lands. Every recorded property
falls out: intermittent with identical logs (depends where DWM's passes fall),
gesture-independent, the right duration, and — the one that kept misleading —
**immune to the double-rAF, because rAF only moves submission.** "Nothing in
our timeline is late" was true; the lateness was in a pipeline none of our
timestamps touch.

### The fix

`app_close_finish`: `DwmFlush(); DwmFlush();` immediately before the
`ShowWindow(SW_HIDE)` — the exact pattern `app_open_finish` has carried since
the open path was fixed ("one real compose ... two, because the first may
already have been in flight"). Two composition passes of slack, paid while the
box already covers the window, so the user sees nothing extra. 0/40 under the
reproducing load vs 8/40 without (Fisher p ≈ 0.006).

It is measured SLACK, not a presentation proof — no public API can prove a
foreign process's commit is on glass. If the flash ever returns under some
heavier load, widen this (more passes), do not delete it. Does not port to the
sibling app: Eclipse Aimbot has no app-mode choreography.

### LIMITS — what was NOT verified

- **Not confirmed against real Roblox closes.** The harness reproduces the
  symptom's shape, magnitude and intermittency and the fix eliminates it
  there; the real system has the same choreography and worse load, but the
  live 1-in-3 becoming 0-in-N needs the user counting closes, same protocol
  as 2026-08-30.
- **MPO vs plain compose cost is not separated.** Fullscreen geometry was
  required for the repro; whether the mechanism runs through hardware plane
  reconfiguration or simply bigger/slower composition is unknown, and the fix
  does not care — but if a future flash survives extra flushes, disabling MPO
  (`OverlayTestMode=5`) is the discriminating experiment.
- **Two flushes are calibrated to this machine's load, not a bound.** The
  harness never beat them, but the harness's load is synthetic.
- The harness itself is deleted. Rebuild recipe: mimic `composed_create` for
  the overlay + `startAppClose` in a `NavigateToString` page posting BOXUP
  after double-rAF; flip-model swapchain window as the game; DXGI output
  duplication thread stamping the test pixel per presented frame; modes =
  {hide-on-BOXUP, +flushes, inversion}; add the canvas + copy-engine load and
  fullscreen geometry or it will NOT reproduce.

### Addendum, same day: the stuck-input report is NOT the flushes

Right after the live confirmation the user hit "open, immediately click the
client, sometimes input is dead -- badge unclickable, right-click exit dead",
and then said it is an old issue. Measured anyway, because "did not cause it"
and "did not widen it" are different claims:

- **DwmFlush x2 costs 12 ms p50 idle, 20 ms max under GPU load** (probe on
  this machine). That is the whole widening the close-path fix adds, once per
  close, against phase belts of 1500/3000 ms.
- **A click arriving while the UI thread is blocked at flush scale is HELD by
  the LL-hook machinery and delivered HOOK-FIRST when the thread resumes** --
  the click pre-emption's ordering survives the block, it just runs ~15 ms
  late. The failure mode where input LEAKS past a blocked hook to the window
  is real but sits past the LL-hook timeout: measured leaking at a 600 ms
  block, held-and-ordered at 40 ms. The flushes are ~30x below the cliff.
- The repro's hazardous interval is the OPEN travel (`app_close_begin`
  refuses while `g_app_opening`), which runs before the close-path flush ever
  executes; the open path's own two flushes predate the fix (2026-08-22 era).

So: the flushes neither created nor measurably widened the stranding. The old
bug is left alone, deliberately. For whoever picks it up: the strand
self-recovers by design -- `APP open watchdog` at 1.5 s, `APP travel-ct belt`
at 3 s clears both `g_travel_ct` AND the click-through -- so the first
diagnostic question is whether the user's "stuck" outlives 3 s. If it does,
the strand is NOT travel-ct and the zlog to catch is which of those lines
fires; if it does not, the bug is the belt being the recovery instead of the
choreography, and the open-travel refusal window is where to look.

---

## 2026-09-02, later: the permanent stuck state -- found in the PAGE, fixed

The user pinned the repro (badge-open, then immediately SPAM-click the client;
one click never does it) and the outcome: app gone with NO close animation,
badge dead to both buttons, and the app's former rectangle a click-eating dead
zone over the game. Stuck FOREVER -- which killed every belt-covered theory at
once, and pointed at state no watchdog touches.

### The wedge, measured against the real page

A harness loaded the real `build/web/hud.html?c=1&hud=1&om=1` in a windowed
WebView2 and drove the app choreography messages in five orderings,
fingerprinting the outcome (ANIM:app-close seen? APPIDLE seen? final RECTS
badge-only or still carrying the "~" corridor?):

| ordering | result (pre-fix) |
|---|---|
| legal: OPEN, ATRECT, SHOWN, CLOSE, BOXUP, GO | clean |
| SHOWN delivered AFTER CLOSE | **WEDGED: no ANIM, no APPIDLE, corridor stuck** |
| CLOSE mid-open-travel | clean (self-heals) |
| SHOWN mid-open-travel (watchdog shape) | clean (self-heals) |
| SHOWN delivered AFTER GO | **WEDGED, same fingerprint** |

The wedged fingerprint is the user's report exactly. The mechanism, stated
plainly: a stale `APPSHOWN` runs `stage.classList.remove("on")` while a close
owns the stage; "on" off means `display:none`; **a `display:none` element runs
NO CSS transition**, so the close travel that `APPGO` starts never produces a
`transitionend`; `appAnim` wedges TRUE forever. Everything follows: the badge
gates on `appAnim` (left-click degrades to `APP_EXIT`, which the host ignores
because ITS app mode closed properly; right-click refuses the flyout),
`APPIDLE` is never sent so the corridor soft rect stays in the region, and the
overlay -- no longer click-through once the close's own path or the belt ran
-- eats every click in it: the dead rectangle. Permanent because every host
belt (watchdogs, travel-ct) resets HOST state; nothing anywhere reset the
page's.

### What did NOT hold up, recorded

- **The reentrancy theory in its strong form.** The suspected emitter -- a
  spam click's LL-hook callback running NESTED inside `app_open_finish` (whose
  first line cleared `g_app_opening`), via sent-message dispatch during
  `SetWindowPos`/`force_foreground`/`DwmFlush` -- was probed with a real
  WH_MOUSE_LL, a cross-thread-owned window and injected clicks: **0/75 nested**
  across all three call sites; the hook consistently fired only at the next
  real pump. Cross-PROCESS owner (the live shape -- the app is owned by the
  game) was not probed and stays possible. The host hardening below costs
  nothing if it never happens.
- **A `transitioncancel` listener is NOT the fix, do not add one.** The
  legitimate close-mid-travel ordering cancels the open travel on purpose
  (`still()` snaps the box) and recovers through the close's own
  `transitionend`; a cancel listener doing "travel finished" work would fire
  there and break the ordering that today heals itself.
- `hud_push`'s drop gates (`ui_page_live`, `overlay_wanted`) were suspected of
  eating APPSHOWN outright: analyzed, and a fully DROPPED APPSHOWN self-heals
  (the box just holds until the close re-uses it). Only a LATE one wedges.

### The fix -- page immunity plus host leg-guards

Page (`web/hud.html`), the load-bearing half -- the page is now immune to the
wedge orderings whatever emits them:

- `APPSHOWN` with a close begun (`!appOpen`) is stale and ignored; with the
  open travel still in flight (`appAnim`) the drop is deferred via
  `shownPending` and runs at the travel's end (the open watchdog can
  legitimately beat `transitionend`).
- `startAppTravelHome` re-stands a downed stage at the window rect before
  animating, so `APPGO` always produces a real transition and therefore a
  `transitionend`, whatever came before it.

Host (`src/ui/overlay.cpp`), closing the plausible emitters:

- `app_open_begin` now carries the same leg guard as `app_close_begin` -- the
  WM_HOTKEY comment had claimed both existed; only one did.
- `app_open_finish` clears `g_app_opening` LAST, after `APPSHOWN` is pushed,
  so no gesture accepted anywhere inside it can emit `APPCLOSE` ahead of
  `APPSHOWN` even in the unproven cross-process nesting case.

Re-run of the same five-ordering harness against the fixed page: **all five
clean** -- travel plays, `APPIDLE` arrives, RECTS return to badge-only. The
check could fail and did (2/5 wedged pre-fix). The close-flicker fix
(`DwmFlush x2` in `app_close_finish`) is untouched by all of this.

### LIMITS

- **The host-side emitter of the stale ordering in the LIVE repro is not
  identified.** Same-process nesting is probed out (0/75); cross-process is
  not. The page fix makes every candidate emitter produce a recovering close
  instead of a permanent wedge, so the symptom should be gone either way --
  but if the user's spam repro still shows even a transient oddity, an
  `ECLIPSE_ZLOG=1` capture of it is the next instrument: the line order
  around `APP shown` / `APP close gesture` / `MSG <APP_BOXUP>` will name the
  emitter directly.
- The live repro itself (badge + spam over a real Roblox client) was not
  reproduced here -- it needs real clicks in the user's session. The harness
  reproduces the page half exactly and the fingerprint matches on all three
  symptoms; the user re-running their repro is the confirmation that counts.
- Harnesses deleted per the rule. Rebuild recipe: windowed WebView2 loading
  the staged hud.html with `?c=1&hud=1&om=1`, host messages injected via
  `ExecuteScript("window.__onHudMsg(...)")` (that is `hud_push`'s real
  channel), page messages collected via WebMessageReceived, one Navigate per
  ordering for a clean slate, and the window kept visible -- an occluded
  Chromium throttles rAF and BOXUP hangs off a double-rAF.

---

## 2026-09-07: back under load, reproduced LIVE, and the lever is the page's rAF count

User: "it blinks and shows Roblox through BEFORE the animation starts", an old
issue that survived the 2026-09-02 fix. Measured on the REAL system this time,
not a mimic: a throwaway close-leg probe (`build/zclose.cpp`, deleted; recipe
below) sampling a 7x4 grid inside the app's DWM composed rect per presented
frame via DXGI desktop duplication, classifying each frame against a reference
snapshot of the steady app and the box colour, with a spin z-walk and a
SendInput gesture driver, the console under `ECLIPSE_ZLOG=1` for the timeline.

- **Unloaded, 0 bare-game frames in 28 closes** across every entry point and
  foreground state: console-fg click / Insert / badge, client-fg click / Insert
  / badge, third-app badge / Insert. The check can go red: hiding the app from
  outside put the game on all 28 points 13ms later, for as long as it was
  hidden. The z-walk read `AHG -> HG` on every close, loaded or not -- the
  client never crossed the app, so the band rule and its owner carry are not
  the story.
- **Under a 90% busy iGPU, 4 of 9 closes presented 1-2 whole-rect bare-game
  frames between the hide and the box**, up to ~110ms of glass time. The load
  was a throwaway D3D11 shader loop (`build/gpuload.cpp`, deleted) on the AMD
  680M -- which is the adapter DWM and the WebView2 GPU process run on here;
  Roblox is alone on the RTX 3050, so loading the dGPU is the wrong test. The
  box frame reached glass ~140ms after `APP_BOXUP` while the two `DwmFlush`
  spanned 31-78ms: the 2026-09-02 mechanism exactly, past the slack it was
  calibrated to. (The load generator's first build culled its own triangle
  and did nothing; validated with the per-process `GPU Engine` counter before
  any number from it was trusted -- a "load test" at 0/10 had already been
  recorded and thrown away.)
- Sweep under that load, same gestures: K=2 rAFs / 2 flushes (shipped)
  **4/9 bad**; K=4 / 2 flushes 0/18 in two runs and then **2/8 in a third**
  -- under load a rAF is paced at one DWM pass, so four rAFs were two more
  passes of slack, not a witness, and the tail came back; K=2 / 6 flushes 0/9,
  at 141-234ms of blocking per close and blind to Chromium.
- **Element Timing as the witness**: `PerformanceElementTiming.renderTime`
  comes from the compositor's presentation feedback, so an observer on the
  box (a 1x1 background image of its own colour, URL fragment changed per
  close because it reports once per element+image) fires when Chromium has
  COMMITTED the frame carrying the box -- renderer, raster, IPC and the GPU
  process's queue included. Idle it fired at +18..21ms (the double rAF's own
  timing; hide +47..63 vs +36..50), under load at +57..186ms with the
  callback one DWM pass after `renderTime`. **3/36 residual under the
  load vs 4/9**, and the residual named the limit: in the failing closes the
  feedback said presented at ~+92ms after `APPCLOSE` while duplication showed
  bare game until +289ms, when the box finally composed. **The feedback is
  Chromium's commit, not DWM's display: DWM keeps composing the overlay's OLD
  surface until the new one's GPU work completes on the starved GPU, and no
  page or `DwmFlush` signal can see that fence.**

**Shipped: `APP_BOXUP` off the Element Timing entry**, with the double rAF as
the fallback where the API is absent and a 600ms belt inside the host's 1.5s
watchdog (`hud.html`, `startAppClose`); the page suffixes the message with
which path fired and the host prefix-matches it, so a trace says. The two
flushes stay as DWM-side slack. If the residual tail matters in real use,
the fix is structural, not a longer wait: keep the box composed under the app
for the whole open (it provably is at `APPSHOWN`, and dropping it there is
what puts new GPU work on the close path), or a host-owned DComp cover /
travel that needs no new rendering at close time -- either is a choreography
change and was not done today.

**Negatives, so nobody re-runs them.** Today's `APPCLOSE` rect inset to the
composed bounds and `app_apply_overlay_styles`' `SWP_FRAMECHANGED` are not
involved: 28 clean closes on the build carrying both, and the failure needs
load. The "close-gesture cover" `app_close_finish`'s old comment named does
not exist -- the hook-time TOPMOST lift was removed when the band rule landed;
the `NOTOPMOST` there drops the band's bit, and the comment is corrected. The
frames at the swap show what the user sees on a clean close: app -> the same
rect as an opaque box with the DWM shadow gone and the corners a touch rounder
-> travel; not "Roblox through".

**Seen on the way, not fixed:** a badge click with the CLIENT foreground
closed the app 277ms late once and not at all once (no `APP_EXIT` for the
click; `FG-EVENT` on the overlay host plus a `VETOED` line), and the second
click closed it. Console-fg and third-app badge closes were prompt.

> **NOT A BUG -- CONTAMINATED SAMPLE (annotated 2026-09-07, same day).** The
> user opened a terminal window during that part of the run. Both anomalies
> follow from that directly, so do not chase them.
>
> The close-vs-return decision is made ENTIRELY from who held foreground when
> the click landed: `g_game_click_from` in `hud_mouse_ll` tests the app window,
> this process, the WebView2 browser process and the client, and a click that
> matches none of them is classified as ARRIVING FROM A THIRD APP -- which
> deliberately does not close, because that is the return gesture. A terminal
> taking foreground mid-gesture therefore turns a badge close into a return.
> That is exactly "no `APP_EXIT`, and the second click worked": by the second
> click foreground was ours again. The 277ms-late one is the same race landing
> on the other side of the timing.
>
> The general lesson is worth more than the entry: **a SendInput gesture driver
> measures nothing trustworthy about this subsystem while a human is also
> moving the foreground.** Every classification here is a foreground test.
> Anything anomalous in a run should be re-checked against "was the machine
> being touched" before it is written down as behaviour.

### LIMITS

- **Desktop duplication turns MPO off for the run**, and this display is
  MPO3-capable on the iGPU. A plane-reconfiguration blink is invisible to
  every capture here. If the user still sees a blink on this build, closing
  while a screen recorder runs (which also disables MPO) is the discriminating
  test; `OverlayTestMode=5` + reboot is the other arm.
- The load was synthetic whole-iGPU starvation, not a busy dock in the overlay
  page. A docked snippet redrawing per frame queues in the same GPU process
  ahead of the box frame and is the realistic shape; it was not in the scene.
- The residual (3/36 under the synthetic load) is a GPU-fence tail past
  Chromium's commit and is not closable by any wait this side of the fence;
  the shipped build measured 0/8 loaded and 0/4 idle, which is a sample, not
  a bound. The user's real closes were not counted on this build.
- The OPEN leg's paint witness (`APPPAINT` -> double rAF -> `APPPAINTED`) has
  the same submission-not-presentation gap by construction; it was not
  measured or changed today (close leg only).
- Probe recipe: resolve app/hud/game HWNDs as `tools/zdup` does; per presented
  frame read the app's `DWMWA_EXTENDED_FRAME_BOUNDS` while visible and sample
  a 7x4 grid inset 14px plus corners and just-outside points; take the app
  reference on command; classify A (ref +/-12), B (`0d0d10` +/-4), else O;
  a frame is bad when O >= 6 of 28 before the first B-dominant frame; drive
  with `ensureclosed`/`ensureopen` cells so a mis-timed gesture cannot
  desynchronise the rest; put a `third` window off the app rect and wait for
  `WindowFromPoint` to return it before clicking, or the click reaches the
  client and closes the app.
