# Making Eclipse an in-game overlay

Written 2026-08-20, at the end of the session that built the in-game panel and
chased its flicker into the ground. This is the plan for the next steps plus
everything that was learned the hard way, so none of it has to be rediscovered.

`CLAUDE.md` points here; the code carries the same conclusions as comments where
the decisions live.

## Where it stands today

- The console has an **in-game panel**: a composition-hosted WebView2 window
  glued to the attached client, showing the log, arm state, health, account and
  a runnable snippet tree. It never takes focus, sits above the game and below
  apps you alt-tab to, and passes clicks outside itself through to the game.
- `fx_window_dock` gives a snippet **the same kind of window**. Panel and dock
  share one implementation (`ComposedHost`, `composed_create`), one promote
  (`raise_overlays`), one focus hand-back (`overlay_handback`) and one hook
  owner (`overlay_tick`).
- Both windows are provably identical at runtime — same class, ex-styles,
  style, class style and hosting, checked with a probe rather than assumed.

## Decisions (settled 2026-08-21)

**The badge grows into the app.** Badge, panel and full app are three SIZES OF
ONE WINDOW, not three windows. Click the badge and it grows — animated, inside
Roblox — into the console. The app takes focus while open; closing hands focus
back to the game and shrinks it again.

**The panel is not separately active in app mode.** The app already shows
everything the panel does — log, arm state, snippet list. Two live views of one
state is the bug class that ate a whole session; one window, one state, three
sizes.

**One window, sized to the client area.** This falls out of the above: the app
is large when open, so the window has to be big regardless. Docked snippet
panels therefore become ELEMENTS INSIDE IT rather than separate HWNDs, which is
what finally removes the promote-order blink — a single window is always the
first raised. `fx_window_dock(0)`, the undocked mode, stays a real floating
window; that is a genuinely different thing and snippets in the wild rely on it.

The cost is HIT-TESTING: a client-sized transparent window swallows every click
unless its region is the union of the visible pieces. This works — the region is
the hit-test mask while the anti-aliased pixels live inside it — but the page
must report its interactive rects to the host whenever layout changes.

**Snippet pages are iframes with per-snippet origins.** Not the same document.
WebView2's virtual-host mapping gives each page its own host name so same-origin
policy does the isolating. A snippet pasted from a forum must not be one
`document.querySelector` from the arm toggle.

**`fx_window_dock` keeps its signature and semantics.** Docked means "your page
is a panel inside the overlay"; `fx_window_close` destroys that panel; the
message channel is unchanged. A snippet does not need to know which it got.

**Each works alone.** Panel with nothing docked, docks with the app closed.
Trivial in a one-window model, which is another reason for it.

**The window never resizes while anything animates.** The growth from badge to
app happens in the PAGE, inside a window that is already client-sized. Resizing
a visible WebView2 window shows a stale surface — see below, it cost most of a
day. The hit-test region updates at the ends of the animation, not per frame.

## The plan, in the order worth doing it

Each step is useful alone and de-risks the next.

**1. Merge the dock into the panel's window. DONE, verified 2026-08-21.**
One window hosting several panels; hit-testing is the union of the visible
rects. This is the real fix for the last flicker rather than a relocation of it
(see "promote order" below), and it answers the multi-page hosting questions
cheaply before anything bigger depends on them.

What was measured, with the console panel open and a docked snippet up:

- one `EclipseHudHost`, `0,0 1920x1080`, both panels drawn inside it;
- `GetRegionData` returns exactly TWO rects -- `1668,12 240x150` (the dock) and
  `1548,310 360x460` (the panel) -- matching the page's report byte for byte;
- `WindowFromPoint` gives us the dock and the panel, and the game
  (`WINDOWSCLIENT`) in the GAP BETWEEN THEM and everywhere else. The gap is the
  check that can fail: a client-sized window with a wrong region passes the
  first two and swallows the map;
- collapsing the panel reports `RECTS:1864,507,44,66` and the region follows to
  the badge alone.

The instrument was the message trace (`ECLIPSE_ZLOG=1`, `MSG <...>` plus the
parsed rect count), not a capture -- it separates "the page never sent it" from
"the host ignored it", which is exactly where an early guess had put the bug.

**2. The focus mechanism. SOLVED 2026-08-21 — and it belongs to APP MODE.**

Two findings, one correction.

*No hotkey is needed.* A CLICK on the overlay is this process receiving the
last input event, which is exactly the documented `SetForegroundWindow`
exception — the same permission `overlay_handback` was already spending in the
other direction. Measured both ways: a plain `SetForegroundWindow` from a
background probe is REFUSED, the click path reports TAKEN. `RegisterHotKey`
remains the escape hatch if that permission ever tightens; it is not shipped.

*What the keyboard actually is in composition hosting* (harness-measured one
variable at a time, then confirmed live). There is no `SendKeyboardInput`;
WebView2 creates an in-process `Chrome_WidgetWin_0` input child under the host:

| state | keydown |
|---|---|
| foreground taken, `WS_EX_NOACTIVATE` still set | none |
| `WS_EX_NOACTIVATE` cleared, no re-show | none |
| + `MoveFocus(PROGRAMMATIC)` | **keys arrive**, focus on `Chrome_WidgetWin_0` |

Real IME focus and the real clipboard come with it (Ctrl+C of a page selection
landed a sentinel on the system clipboard). `AcceleratorKeyPressed` fires for
Esc, F-keys and modified combos only, never plain letters — which is why Esc is
handled controller-side and still exits while a docked snippet's iframe holds
DOM focus. The code is `overlay_focus_enter` / `overlay_focus_exit` in
`eclipse_ui.cpp`, entered by `HUD_FOCUS:1` from the page; the page says which
click means focus, the host never guesses from coordinates.

*The correction: THE PANEL MUST NEVER TAKE FOCUS.* Opening the panel was wired
to take focus for exactly one build, and it was wrong twice over. It broke the
design — the panel exists to read as a Roblox-owned UI, and it must not hold
the keyboard the game is using. And it was instantly VISIBLE, because Windows
draws a focused window differently: an accent band appeared around the panel
and its text re-rendered. Nothing sends `HUD_FOCUS` today; the mechanism is
there for app mode, which is a thing you type into.

*Update 2026-09-04: a DOCKED SNIPPET PANEL'S TEXT FIELD now sends it.* The
frame shim injected into the dock iframe (`kOverlayFrameShim`, `overlay.cpp`)
posts `__ec_focus:1` once a click has put DOM focus on an `<input>`,
`<textarea>` or contenteditable, and `__ec_focus:0` once DOM focus leaves one;
`hud.html` relays them as `HUD_FOCUS:1/0`. The panel rule above is unchanged --
the console panel never sends it -- and the objection to it was re-examined
rather than assumed away:

- **The accent band is a DWM frame property, and the composed host has no
  frame.** Measured with a probe of the host's exact styles (`WS_POPUP`,
  `WS_EX_NOACTIVATE|TOOLWINDOW|NOREDIRECTIONBITMAP`, DComp content, shown
  inactive, then FOREGROUND with `WS_EX_NOACTIVATE` cleared -- the state
  `overlay_focus_enter` leaves it in): 0 border pixels on every ring from the
  edge to 10px in, in both states, and still 0 with `DWMWA_BORDER_COLOR` set
  to red and asked for. The positive control, a real captioned
  `WS_CAPTION|WS_THICKFRAME` window beside it, drew the 1px border on all four
  sides unasked, inactive and foreground alike (293/300 rows left and right,
  386/386 columns along the bottom, where there is no caption to confuse it). Every
  frameless variant in between (thickframe with `WM_NCCALCSIZE=0`, with and
  without caption, with and without an explicit colour) drew none. The band
  seen on the panel came from the wrapper-hosted fallback, which has a frame
  and carries `DWMWA_BORDER_COLOR=NONE` for exactly that reason.
- **The "text re-rendered" half was NOT measured** -- it needs the live page in
  focus mode -- and it is accepted for a docked panel on design grounds: the
  page is the user's own UI, the user clicked into its field, and a caret and
  focus ring appearing there is the thing they asked for. It is still wrong for
  the console panel, which must read as the game's.
- **Why the page decides, not an `fx_window_*` flag.** The field is the intent,
  exactly as the hit rects are; a host-side flag would have to guess which
  click meant typing, from coordinates -- this tree's recurring bug class. A
  snippet-side flag was considered for the one thing this cannot do (keys to a
  canvas or a button with no field) and left for when someone needs it.
- **Entry is on `mousedown` + `setTimeout(0)`, not `focus`.** Blink moves
  `activeElement` on the click whether or not the page is focused, but it
  dispatches the focus event only while the page is focused -- and the page is
  never focused when the first click lands. Exit is `focusout` + `setTimeout(0)`
  with `activeElement` inspected: another field keeps the mode, anything else
  ends it. A page-level focus loss (Esc, a click on the game) dispatches
  `blur` only, and the host already left on those paths.

**3. The app in the game. SHIPPED 2026-08-22, but NOT as an iframe.**

The first build put the console's UI in an iframe inside the composition
overlay. It worked and it was wrong, for two structural reasons rather than
bugs:

- **Dragging could never be smooth.** Nothing moved at the OS level: the
  overlay window is client-sized and static, so "dragging" re-laid-out boxes
  in the page, and every pointer move travelled `OS -> host thread ->
  SendMouseInput -> browser input pipeline -> renderer`, because a
  composition-hosted WebView2 has no HWND to receive input. Removing the
  worst two hops (binding the drag in the overlay page instead of relaying
  postMessage from the frame's main thread) still left it visibly behind the
  cursor. A real window move is one `SetWindowPos` and DWM composites it.
- **The overlay's activation gate.** Entry to the topmost band is refused in
  a state that is still not fully characterised (see the section below), and
  when it is refused the badge sits under the client and cannot be clicked.

**What ships instead:** the console's OWN WINDOW, over the client,
`WS_EX_TOOLWINDOW` (off the taskbar, out of Alt-Tab), DWM-rounded. Badge,
panel and docked snippet panels stay in the composition overlay, which is
what they need: visible while the client has focus, never taking focus,
small and static.

That split pays for itself several times over. The drag is a window move
(measured 1:1 against the cursor, zero trailing). A window the USER clicks
takes foreground the ordinary way, so no `SetForegroundWindow` gymnastics and
no band to be refused for the app. The app needs no hit region at all, which
removes the region/pixels disagreement that made the badge vanish twice. And
the whole app-frame apparatus died with it: the `APPMSG` relay, host->frame
routing, `__toApp`, and the `app.eclipse` virtual host whose SEPARATE
localStorage used to push its own stale `console.overlayMode=0` at the host on
load and throw the user out of overlay mode a second after opening.

### How it behaves, and why each rule exists

- **`Insert` toggles it** (`RegisterHotKey` on the overlay window, `WM_HOTKEY`
  in `hud_host_wndproc`, registered only while a client is attached). This is
  the door that does not depend on z-order, hit regions or what is on top --
  which is what makes everything else testable. It was in the original design,
  dropped once clicks were found to take foreground, and every failure that
  followed was in the one door left.
- **Opening is a HANDOFF, not a resize.** A visible WebView2 window cannot be
  resized without showing a stale surface. So the overlay travels an EMPTY box
  (background, rim, radius) from the badge to the target rect, then the real
  window is shown at exactly that rect and the box drops. Ordering is
  load-bearing: show unactivated, let a compose pass, then drop the box.
  Backwards is a one-frame flash of the game.
- **The app rides the same band rule as the overlay**: `WS_EX_TOPMOST` iff the
  CLIENT is foreground; an ordinary window when anything else is, including
  the console itself. Continuous, read from the real `GWL_EXSTYLE`, never
  latched at open time. Without it, returning from a third app left the app
  open and BURIED (measured: app z#22, client z#21).
- **Clicking the client closes it -- decided in the MOUSE HOOK.** See the dead
  end below; this is the single most expensive mistake of the day.
- **Position is remembered** in `%APPDATA%\EclipseConsole\overlay_app.txt`
  (client-relative), separate from `window.txt`, which is the desktop rect.
- **`WS_EX_TOOLWINDOW` keeps it out of Alt-Tab as well as the taskbar, and
  that is accepted.** While the app is open, Alt-Tab's previous window is the
  CLIENT, because the console is not a candidate at all; and once you tab out
  to a third app you cannot Alt-Tab back to the console. Decided 2026-08-22:
  leave it. Tabbing from an overlay to the game it sits on is coherent, and
  `Insert` is the way back from anywhere. The alternative is dropping
  `WS_EX_TOOLWINDOW` (or adding `WS_EX_APPWINDOW`) while open -- but Alt-Tab
  membership and a taskbar button travel together, and the taskbar button is
  what the user asked to remove. Do not "fix" this by re-adding it.
- **Startup in overlay mode shows nothing but the badge.** Two separate causes
  had to be fixed to get there; both are recorded below.

### Dead ends bought on 2026-08-22, in the order they hurt

- **Reacting to FOREGROUND EVENTS for anything a click causes.** Every
  app-mode decision about "the user clicked Roblox" was written in
  `hud_on_foreground`, which by definition runs AFTER Windows has activated
  and raised the client. So the app was always a frame or more late: clicks on
  the client appeared to do nothing, the app landed under the client, and the
  close animation showed the game through the gap. The panel had solved this
  long before -- its promote lives in the `WH_MOUSE_LL` hook, which runs
  BEFORE the click reaches the client. Moving the app's decision into the same
  hook fixed all three symptoms at once. **The general rule: if the panel
  already solves an input or z-order problem, port ITS mechanism; do not write
  a second one where it happens to be convenient.**
- **The PANEL's model does not transfer to a real window, and four separate
  places assumed it did** (found by auditing every overlay behaviour against
  app mode, 2026-08-22, after "clicking a dock flickers the app / puts the
  badge behind it"). The panel is a borrowed-click surface: a click on it is
  handed back to the client, because it must read as a game-owned UI. With
  app mode open the console IS the window in front, so the same click is
  ordinary use and handing it back raises the client and reshuffles both
  windows in the band. The four:
    1. the hook SET `g_hud_used` on any overlay click -- now only when app
       mode is closed;
    2. `overlay_handback` could still fire on a flag set just before the app
       opened -- now suspended outright while app mode is open;
    3. the close gesture's "ours" test omitted WebView2's BROWSER process, and
       clicking a docked panel parks Win32 focus there -- so the next click on
       the client read as "arriving from elsewhere", took the return branch,
       and closing took two clicks;
    4. the overlay's demote took "the slot directly above the GAME", but with
       app mode open the APP is what sits there -- so the overlay went under
       it, badge and dock behind the app. It anchors above the app now.
  The general form: any rule written for "overlay vs client" needs re-reading
  the day a third window of ours joins the band.
- **"Raise ours BEFORE handing focus over" applies to APP MODE too, and it did
  not inherit it.** `overlay_handback` has carried that line since the panel
  existed. App mode's close hands focus to the client, and on the badge/Insert
  path the app is NOT in the topmost band when it does -- the band rule is
  "topmost iff the CLIENT is foreground", and on that path WE are foreground.
  So the client's raise crossed above a non-topmost app window and the game
  showed through for a frame or two before the box covered it. Only on that
  path: clicking into the game to close is clean, because the hook runs before
  Windows raises anything and no foreground is forced. Fix: promote the app,
  then `raise_overlays()` (the travel box must compose over the app it is
  covering), then hand focus over.

  **Third time it applied, 2026-08-22:** the badge summoning the console from
  a THIRD app (Chrome, an editor) raises the client first so the pair arrives
  together -- and that raise crossed over the badge until `raise_overlays()`
  was put in front of it, and again after. Take it as the general rule for
  this tree: **place ourselves in the band before any foreground change we
  ask for, and again after it lands.** Note the ordering has a real cost that
  cannot be designed away -- Windows will not do a cross-window z-order
  change and a foreground change in one compose, so either our window crosses
  above the app being left, or the client's raise crosses above ours. Raising
  first is the one that reads better.
- **A rounded corner drawn by an overflow CLIP is not durable.** The badge's
  radius used to come from `#shape`'s rounded `overflow:hidden` clipping
  `#tab`'s square gradient. When a sibling's height transition restructures
  Chromium's layer tree, that clip is intermittently dropped and the square
  paint underneath fills the corners -- the badge went hard-edged while the
  flyout, whose radius is a background on its own box, stayed round.
  Reproduced 28 times in 120 samples, 0 after making the badge paint its own
  rounding (gradient on `#shape`, `#tab` paintless). The square pixels
  sampled the badge's gradient colour exactly, which is what identified it.
  **`WS_EX_LAYERED` was the suspicion and was FALSIFIED** -- every square
  sample read `ex=0x8200088`, both LAYERED and TRANSPARENT clear, with
  mid-travel samples reading `0x82800A0` as the positive control. If a
  composited element must be rounded, give it its own background radius.
- **The badge's right-click switch** (2026-08-22): right-click the collapsed
  badge in overlay mode and a single icon button pops below it showing the
  target you would switch TO -- app or panel -- and picking it switches AND
  opens that target. Each glyph depicts what it opens: a window with its
  titlebar filled, or the same frame with its right-hand badge strip filled.
  The choice persists in the page's localStorage. Dismissal is split by
  necessity: the page handles clicks on the overlay itself, and the host's
  mouse hook sends `CLICKAWAY` for a click on the CLIENT -- only the client,
  because a click in a third app is the user switching away, not dismissing.
- **The app's own rect eats a press for ~30ms during a close, and that is
  ACCEPTED** (decided 2026-08-22). The overlay's box has to be composed over
  the app window before it hides, or the game shows through for a frame -- so
  the window is still on screen, and still taking input, for about 30ms after
  the close starts. A press landing inside the APP'S OWN rectangle in that
  window goes to the app rather than the client. The corridor between app and
  badge is live throughout; this is only the app's footprint, and 30ms is
  shorter than a press-release-press. The fix would be layering a WebView2
  window mid-handover, which risks the blank frame the whole choreography
  exists to prevent -- a worse bug traded for an unreachable one. Do not
  "fix" this without a reproduction of someone actually hitting it.
- **Two owners for one window's band.** A hook-time TOPMOST cover and the
  tick's band rule demoted and promoted each other sixty times a second (1067
  fight lines in one trace). One owner, like `raise_overlays` is for the
  overlay.
- **The alt-tab-return flash was the HWND_TOP + tick two-step, measured
  2026-08-22.** The FG handler's "APP stays" branch raised the app to
  `HWND_TOP` and left the tick's band rule to promote it into the topmost
  band up to 16ms later, with the client's own activation raise landing
  between the two. Z-order sampled at ~1ms cadence through staged alt-tab
  returns (verified to land on the client, 8 trials per variant, one
  instrument): that left the app under the game **avg 12.6ms / worst 51ms**
  per return -- one to three composed frames, the flash the user reported.
  Collapsing to a single `HWND_TOPMOST` in the handler: **avg 1.8ms / worst
  2.8ms**, which is the WinEvent delivery latency itself; the panel-only
  reference on the same event is 0.7ms. The residual ~2ms is the same
  accepted tab-in cost the panel has. Two facts worth keeping: a per-tick
  glue cannot beat the FG event on the HEALTHY path (the below-interval
  starts ~1ms after the foreground flip and the promote lands at ~2.5ms --
  a 16ms tick is slower than the event; but see the next entry for the
  UNHEALTHY path, where a position glue is exactly what is needed), and the
  badge blinks ~2ms longer than the app in app mode because the handler
  raises the app first, deliberately: the 44x66 badge flashing beats the
  client-sized console flashing.
- **The flash the user still saw after that fix was TWO different things,
  neither of them promote ordering -- measured in PIXELS, 2026-08-22**
  (screen-strip capture at ~7ms cadence, app chrome + badge + game sampled
  in one readback, z-relation and fg pid riding along per sample; staged
  returns plus a passive watcher over the user's own real alt-tabs):
    1. **The alt-tab switcher card.** After the fix, no sampled return ever
       showed game content over the app area -- what covers the app for
       ~30-50ms after the foreground flip is the SWITCHER's own dismissing
       card (constant EDEDEB/424040 pixels, drawn above topmost windows,
       DWM-owned). The app sits mid-screen under the card on every return;
       the badge sits at the right edge and only catches the card's rim
       (~15ms, ~1 in 3 returns, B3B3B1). **The panel/app differential in
       the normal case is card GEOMETRY, not anything we do**, and no
       z-order or repaint change can touch it.
    2. **The entry gate can grant the TOPMOST BIT without applying the
       MOVE.** Caught live by the passive watcher: one return left the app
       below a foreground client for 900ms+ with the band rule reading
       app_top=1 and never retrying -- ONE promote line in the whole trace,
       then silence. A bit-only check cannot go red on this, which is why
       the band rule alone was not enough. In the same minutes the classic
       storm was also captured (`FF attach path -> ok=0`, then seven
       overlay heals `ret=1 err=0 now_top=0 ext=0` until our own window
       took foreground). **Fix shipped: `app_ensure_above` -- a POSITION
       check (is the app actually above the client?), run per tick while
       the client is foreground and once from each promote site, gluing the
       app directly above the client in the normal band when the band
       mechanism has failed.** The overlay never showed this failure only
       because its demote glue re-anchors by position every tick; this is
       that mechanism for the app. It is a no-op whenever the promote
       really took, so the band rule stays the band's one owner.
- **Clicking the APP from a third app summons the client under it**
  (2026-08-22, sibling of the badge summon; user: "the console is behind it
  instead of the roblox focusing too"). Stamped in the LL hook
  (`g_app_arrive_at`) -- only when the foreground at click time is a third
  app; ordinary use must not trip it -- and consumed by OUR OWN foreground
  event, when the raise is legal: a z-only `SetWindowPos` inserts the
  client directly below the app (backdrop, not focus -- the user clicked
  the console to type into it, so nothing calls SetForegroundWindow), then
  `raise_overlays()`. Deliberately separate from `g_game_click_*`: a click
  on the app is not a click on the client and must not arm the close
  gesture. Verified live: game z 44 -> 30 (directly under the app), focus
  stays on the app, close gesture still closes, and clicking the app with
  the client foreground stamps nothing.
- **`fg = true` forced for "app mode"** rather than for "our process is
  foreground" fed the band rule unconditionally, and the app floated above
  whatever app the user switched to.
- **Closing on any foreground loss.** The client re-asserts foreground BY
  ITSELF every second or two (measured: every open logged `APP shown fg=1`
  then a client foreground event 0.4-2s later, untouched). Closing on that
  shut the console by itself moments after opening. The close is a CLICK, and
  only the hook can see one.
- **Two startup flashes, not one.** `PAINTED` arrives before the first attach
  probe, so "no client" there only means "have not looked yet" -- gating the
  reveal on the probe could not help, and only the last-resort timer may
  reveal in overlay mode now. Then the no-client valve fired in the ~250ms gap
  between the probe completing and the attach landing; it now requires the
  no-client state to HOLD for 1.5s. Measured 17 visible samples across a 9s
  launch before, 0 after, same instrument.

### What to watch

- **Isolation.** A snippet's page inside the console's WebView2 sits much closer
  to the console's DOM than it does today. Iframes with distinct origins, or it
  is a footgun waiting for a snippet pasted from somewhere.
- **You are exposed while typing.** Roblox keeps running while unfocused. Same
  as alt-tabbing, but far easier to forget when the game is still visible behind
  the editor.
- **Keyboard is the whole reason for step 2.** Clicks are delivered by position
  and need no focus; the wheel arrives only because Windows' "scroll inactive
  windows on hover" is on by default; keystrokes go to the focus window, full
  stop.

## What was learned, and must not be re-attempted blindly

### Window shaping

- **Resizing a visible WebView2 window is the enemy.** Its rendering surface is
  window-sized, so growing it displays the OLD surface stretched until the
  renderer catches up — seen as a band along the growing edge and as content
  stranded in a corner. A fixed-size page anchored to an edge does NOT help;
  it is the surface, not the layout.
- **A region is a 1-bit mask.** It clips content without touching the surface,
  but gives hard aliased corners, and does not clip the window's shadow, border
  or DWM rounding — each of which then ghosts over the game.
- **The drop shadow comes from the window CLASS** (`CS_DROPSHADOW`), which is
  webview's. `DWMWA_NCRENDERING_POLICY` does not remove it; clearing the class
  bit does.
- **What ships:** composition hosting (`CreateCoreWebView2CompositionController`
  on a DirectComposition visual, `WS_EX_NOREDIRECTIONBITMAP`,
  `DefaultBackgroundColor` alpha 0). The page owns its shape, corners are truly
  anti-aliased, and animation is compositor-run CSS. Input is forwarded by hand;
  the cursor is mirrored from `CursorChanged`.
- **Fading over a transparent window shows the whole rectangle at partial
  strength.** The open/close animation therefore GROWS a box rather than fading
  one, with contents held still inside a fixed frame so the edge sweeps across
  them.

### Z-order, focus and the flicker

- **Promote order decides who blinks.** Two sequential `SetWindowPos` calls are
  two z-order changes with a composable gap between them; whichever window is
  raised LAST spends that frame under the game. Proven by swapping the order and
  watching the blink move. There is no order in which both are first — which is
  exactly why step 1 above is the real fix.
- **`DeferWindowPos` is NOT the answer.** Batching both raises made the flicker
  MORE frequent in use. It is documented as unreliable for topmost transitions,
  and a batch whose z-order part does not always take is worse than two calls
  that always do. Tried, measured, reverted.
- **An alt-tab is three foreground changes**, not one: the game loses focus,
  EXPLORER's switcher takes it for ~10ms, then the target gets it. Reacting to
  the middle one demoted our windows mid-alt-tab. Delaying the demote until the
  foreground settled fixed that and did NOT fix the flicker — and left our
  window over a newly focused fullscreen app for the settle. Also reverted.
- **DWM's alt-tab activation animation** is a separate, larger effect: the
  game's preview card scaling to fullscreen is built from the game's own
  surface, so every overlay is absent from it by construction. Turning Windows'
  Animation effects off removes that class entirely. Nothing in our code can.
- **Cross-process ownership** (`GWLP_HWNDPARENT` = the game window) gives
  perfect z-order and no flicker, but attaches the two input queues: the cursor
  vanishes over the panel and the UI hitches with the game. Code retained behind
  a flag, off.
- **Hiding instead of re-ordering** removes the blink and makes the panel vanish
  whenever a window sits over it. Rejected. So were "stay topmost unless
  overlapped" and "treat hover over the game as focus" — each traded the blink
  for the panel climbing above apps it should be under.

### Ownership bugs, which is what actually bit

- **Duplication is the recurring cause.** Three copies of "promote the panel and
  maybe the dock" meant the focus hand-back only ever raised the panel — which
  is why clicking the panel blinked the dock. One `raise_overlays` now.
- **The hooks and hand-back belonged to the panel's lifecycle**, and switching
  the panel off in Settings tore them down, silently degrading a docked window.
  They belong to "any overlay is up" (`overlay_tick`).
- **The webview library's shared window refcount** never reaches zero while a
  second window lives, so closing the console left an orphaned window and a
  running process. Every extra window must be destroyed in the main window's
  `WM_CLOSE`.

### Measuring this stuff

Pixel sampling fooled us twice and produced two confident wrong answers:

- **Sequential grabs are a phase trap.** Two ~8ms captures against an event one
  compose long: whichever window's slot lands inside it is hit every cycle and
  the other never. This reported "dock 5/14, panel 0/14" from an event that hit
  both. Sample both in ONE readback.
- **`WindowFromPoint` cannot see DWM's transition visuals** — the zoom preview
  is not a hit-testable window — so "our window is still on top" includes
  exactly the frames where it was covered.
- **A patch over changing content measures the content.** The panel's log
  scrolls when a docked snippet logs on focus changes; sample static chrome and
  re-baseline from the settled state each cycle.
- **What worked:** tracing our own z-order calls with timestamps
  (`ECLIPSE_ZLOG=1`, still compiled in, inert otherwise). It cannot misattribute
  what this process asked for, which is the one thing no capture can get right.
- **`CWP_SKIPTRANSPARENT` skips windows that real input does not** (found
  2026-08-21, the close-travel dead corridor). win32k only routes a click past
  a `WS_EX_TRANSPARENT` window when it is ALSO `WS_EX_LAYERED`; the
  `ChildWindowFromPointEx`/`WindowFromPoint` family answers from style flags,
  not from routing. So every hit-test said the game owned the corridor between
  the app and the badge while the bare-TRANSPARENT overlay was still receiving
  every button there, capturing it and forwarding it to the browser -- which
  then pulled Win32 focus onto the overlay host, leaving the game input-dead
  even after the travel until its ~2s foreground self-reassert. The check that
  settled it was the game itself: `Camera.Rotation` sampled through the
  console's own reads around a synthetic right-drag (static mid-travel
  in-region, moving mid-travel out-of-region and after the fix -- each side a
  control for the other). `overlay_clickthrough` now toggles the
  `WS_EX_LAYERED | WS_EX_TRANSPARENT` pair; with `WS_EX_NOREDIRECTIONBITMAP`
  there is no redirection surface, so no `SetLayeredWindowAttributes` is
  needed and the travel box composes unchanged (pixel-verified against the
  pre-fix baseline). The `overlay ATE` zlog line in `hud_host_wndproc` is the
  standing tripwire: any button logged there while transparent is this bug
  back.

## The topmost fight, measured (2026-08-22)

The overlay was seen losing WS_EX_TOPMOST with the game foreground, the heal
re-asserting every tick and the bit reading clear afterwards each time. What
the harnesses established, so nobody re-runs them:

- **Generic Windows fullscreen is NOT the mechanism.** A fake fullscreen
  foreground window -- borderless popup covering the monitor, and the system
  agreed it was fullscreen (notification state BUSY) -- neither denied nor
  stripped another process's HWND_TOPMOST: 9 re-asserts over 18s, every one
  `ret=1, bit=1`, no strip between them. "Exclusive fullscreen owns the band"
  is a red herring; do not stop at it again.
- **A quiet SetWindowLong strip is impossible.** user32 ignores WS_EX_TOPMOST
  changes through SetWindowLong in BOTH directions (measured); the bit only
  moves via SetWindowPos. Any strip is therefore a real z-order operation.
- **Ownership is a real strip vector but heals fine.** Making a window's
  OWNER non-topmost strips the owned window's bit without it asking
  (documented, confirmed); the next SetWindowPos(HWND_TOPMOST) restores it.
  The overlay is unowned while kHudUseOwner is false, so this is not the
  live mechanism either -- but the diag line reports the owner in case.
- **The live evidence never distinguished "call failed" from "stripped
  within 16ms".** The heal runs on a ~16ms tick and nothing captured
  SetWindowPos's return, GetLastError, or the immediate read-back. That gap
  is closed: hud_swp records ret/err, and hud_host_wndproc counts z-moves
  arriving OUTSIDE our own calls (a cross-process SetWindowPos on our window
  lands as WM_WINDOWPOSCHANGING on our thread). Verified live: an external
  probe's z-move logged as `overlay EXTERNAL z-move` and was healed to the
  correct band within 7ms; our own promote/demote calls do not trip it.
  If `ext` climbs during the next fight, the client's process is moving us;
  if it stays 0, ret/err in the same trace say how the calls are failing.
- **When the band is refused** (8 consecutive failed heals) the overlay now
  rides the normal band directly above the game -- the demote path's slot,
  re-glued per tick -- instead of sitting wherever the fight dropped it. If
  even that placement will not hold for ~1s in overlay mode, the desktop
  window comes back (the same safety valve as attach failure) rather than
  leaving the console invisible while believing it is showing.

## Drag cost, measured (2026-08-22)

SetWindowRgn on the client-sized composed overlay, measured against the LIVE
window with DwmFlush pacing: **1.29ms per call average, 22.6ms worst, and the
compose cadence's worst gap goes from 9.1ms (baseline) to 27.3ms** -- three to
four dropped frames at 144Hz. A region rebuild per pointer move is therefore
a stutter machine by measurement, not by suspicion. The drag now sends ONE
superset region (the whole client) at drag start and the exact union at drag
end, and moves #root with a compositor translate instead of right/top layout
writes -- the anchor arithmetic still advances per move, so the fold-in at the
end is exact. Mid-drag the whole client area eats clicks; nothing else needs
them while the titlebar holds pointer capture.


## Badge/app position: TWO positions, not one (corrected 2026-08-22)

An earlier pass tied badge and app to ONE anchor (`anchorRight`/`anchorCY`),
on the reasoning that "where the app was" and "where the badge is" are one
fact that cannot disagree. **That was wrong about what the user wanted.** The
correct model, from the user directly:

- **The badge has a FIXED HOME**: the right edge of the client, vertically
  centred, the 12px inset it has always had. Closing always returns it there
  -- a badge is a known place you can always find.
- **The app REMEMBERS ITS OWN position**: drag it anywhere, close it, reopen
  from the home badge, and it returns where you left it. The badge *remembers*
  the app's place; it does not *adopt* it.

Two facts, and the badge's is a constant. Do not reimplement the one-anchor
version from any older note.

Consequence for the animation: the signed-off `#shape` width/height grow
depends on every size sharing a right edge and a vertical centre, so `#inner`
never moves in page coords while the box grows. That invariant holds for the
badge and the PANEL (both at the home anchor) and they are unchanged. It does
NOT hold for an app at an arbitrary remembered rect: a grow from the home
badge to a box elsewhere moves both edges and the centre. The app-mode reveal
therefore animates a `clip-path: inset(...)` over a fixed box at the app's
remembered rect -- content laid out and stationary, the clip edge sweeping
across it, anti-aliased (unlike the 1-bit window region). The PANEL never uses
this path; its animation stays byte-identical. NB the panel is not draggable
(no titlebar); only app mode has a remembered position.

**SUPERSEDED the same day** by the real-window architecture in step 3: the app
is no longer an iframe, so there is no live content to reveal -- the overlay
travels `#apptrav` alone (an EMPTY box) and the real window is shown at the
end of it. The rest of this note stands as the reasoning behind the travel,
and the numbers below were measured on the iframe version.

**Shipped 2026-08-22** as `#appstage` in hud.html: `#apptrav`, a content-free
box (background, rim, radius) whose rect lerps home-badge -> remembered-rect,
under `#appbox`, the app iframe fixed at its final rect and revealed by a
`clip-path: inset()` that lerps the same four edges on the same clock -- the
two coincide frame by frame, so it reads as one box. Content-free matters:
the old `#shape` grow re-clipped the live app iframe every frame (the "laggy"
report; its numbers were never captured -- the band fight ate the measuring
session). The travel measures avg 6.9ms worst 7ms over25=0 steady-state,
identical to the signed-off panel; only the FIRST open, with index.html
loading in parallel, shows avg 11.6 worst 28 (rAF gap sampler, `ANIM:` lines
in the zlog -- the sampler ships, so any future "laggy" report comes with
numbers). The badge never moves; while the app is open the
home badge stays reported as its close control; travels are bracketed by one
bounding-box region report (every interpolated rect lies inside it), exact
rects land at transitionend. The at-the-badge content swap is gone -- panel
and app are separate layers, so `pendingAtBadge` survives only to order
"retract the open panel, then start the travel".


## The topmost fight: mechanism, as far as it is pinned (2026-08-22)

Reproduced on the user's machine; the instruments caught it. The crux anomaly:
`SetWindowPos(HWND_TOPMOST)` returns **ret=1, err=0** and the next read of
`GWL_EXSTYLE` on our own thread shows the bit clear -- 37 times running.

Eliminations, each with a negative control that PASSED (proving the check can
distinguish states):

- **Not a failing call.** ret=1/err=0, and the read-back is on our thread
  immediately after -- now logged per heal (`heal ret= err= now_top= ext=`).
- **Not ZBID window bands.** `GetWindowBand` reads band=1 (DESKTOP) for the
  game, our overlay, the taskbar and a fresh mimic alike -- no boundary. A
  fresh mimic took topmost cleanly in the same run (control).
- **Not ownership.** owner=0 on both windows (measured); the owner-strip
  vector needs a non-topmost owner, which does not exist here.
- **Not our AttachThreadInput hand-back, on its own.** Attaching our thread to
  the client's input thread and issuing the identical topmost call: ret=1 and
  the bit STICKS (bit=1), attached or detached, every time (control =
  detached, also sticks). So the hand-back is not SUFFICIENT to cause it.
- **Not (visibly) an external z-mover.** ext=1 for the whole failing session,
  logged once BEFORE the storm. The `SWP_NOSENDCHANGING` blind spot the
  catcher had is now closed (catch on WM_WINDOWPOSCHANGED, which that flag
  does not suppress -- verified live: an external NOSENDCHANGING poke is now
  counted where it was invisible before).

~~What every failing capture shares: **the client is borderless with its rect
exactly the monitor** (FSO), and windowed topmost sticks -- so FSO is the
necessary condition.~~ **FALSIFIED 2026-08-22 by its own instrument**: a full
failing session was captured with `fso=0` and the client a normal maximised
window, and then the fight was reproduced ON DEMAND with the client windowed
at 1797x960. FSO was a coincidence of when the captures were taken. The real
mechanism, pinned by harness (fresh mimic windows, one variable at a time,
each state shown both passing and failing):

- Windows runs an **entry gate on the topmost band** (and on relative
  inserts above the game window): `SetWindowPos` returns ret=1 err=0 and the
  z-change is simply not applied. Not our window (a fresh mimic in a fresh
  process is refused in the same state), not the window's shape (small and
  monitor-covering refused alike), not an external strip (`ext=0`, and it
  reproduces with a single call in a single-threaded probe).
- The gate **opens around input-driven activation of the game** -- a real
  click landing on it -- and measurably stays open for tens of seconds after
  a synthetic activating click. The LL mouse hook and the foreground event
  fire exactly inside that window, which is why their promotes stick.
- The gate does **not** open for a programmatic `SetForegroundWindow`: the
  overlay hand-back (using the panel while the game is unfocused, or leaving
  app mode) transfers foreground with the gate still closed, and every heal
  after it is refused -- that is the storm the captures kept showing.
- The **foreground process is exempt**: app mode (focus taken) promotes
  successfully regardless. And a bit once granted **persists** (held 20+
  minutes under refusal of new entries); the losses are our own demotes on
  foreground flaps.
- What stays unpinned: the exact opening/closing rule of the gate (real user
  input closed it in one measured series while entries still succeeded 90s
  after an activating click in another), and whether true FSO adds a second,
  stricter mechanism on top. The instruments (`heal ret/err/now_top/ext`,
  the diag line, the mimic probes) make the next anomaly capturable.

**Resolution shipped:** `raise_overlays` clears the band-lost latch on ANY
successful TOPMOST -- the hook paths run while the gate is open, so the
user's next click on the game fully recovers the overlay, including
re-hiding the restored desktop window (previously only the tick's own heal
could clear the latch, so a hook-path win left the valve stuck). The
desktop-window valve now waits for the 1/s backoff to fail too (fails >= 11,
~3s) instead of firing 130ms into a storm that the next click resolves; and
the fso=0 WARN says the true remedy (click the game once) instead of
blaming fullscreen.

## The tab-in blink, actually removed: ownership (2026-08-22)

The long-standing "rare, barely-visible blink when you alt-tab back to the
game" was documented twice in `eclipse_ui.cpp` as unremovable **by design** --
Windows raises the client as part of the activation, our foreground event
arrives afterwards, and the measured floor for reacting is 0.5-1.4ms. Both
statements were true. They were also the reason three rounds of work went into
the wrong place.

**What the user actually reported**: the console's app window flashed on every
alt-tab return, while the panel "barely" did. Every instinct said the app's
promote was late.

**What the user's own trace said** (their gesture, not a scripted one -- the
scripted ones kept landing on a third app and measured the wrong transition,
twice): the app's promote lands **0.5-1.4ms** after the foreground event and
the overlay's lands **6-8ms** after. The window that is out of the band
*longer* is the one that barely blinks. So the difference was never latency:
the app is 1180x780 in the middle of the screen and the badge is 44x66 at the
edge. Identical odds of catching a mis-ordered composed frame, wildly
different visibility. **Any fix that made the app faster was going to fail,
and three of them did.**

**The fix**: give the **app window** the client as its **owner**
(`SetWindowLongPtr(GWLP_HWNDPARENT)`). USER32 keeps an owned window above its
owner as part of the activation itself, so there is no interval left to catch.
This is the third option the old comment did not weigh -- it had considered
hiding (rejected: the panel vanishes under any window) and region clipping
(rejected: unmaintainable). User-confirmed: the app stopped flashing.

**The overlay does NOT get the same owner, and the reason is the owner rule
running backwards.** *(SUPERSEDED the same day — see "The overlay gets the
owner after all" below; the mechanism written here was close but wrong, and
the fix follows from the corrected one.)* It was applied there too, on the same day, to remove the
panel's own (much less visible) blink -- and reverted within the hour: clicking
another app left **the client covering it**.

An owned window is kept above its owner, and Windows maintains that by keeping
**the owner directly beneath the owned window**. Raising ours therefore DRAGS
THE CLIENT UP with it. The overlay has to sit above the game essentially all
the time -- the badge is always visible over the client -- so every time the
user clicked another app, the game was hoisted above the window that had just
taken focus. It was never a hit-testing or focus-stealing bug: the click landed
and focus moved correctly, and the game was simply lifted over the winner.
Diagnosed from a user screenshot after three synthetic probes failed to
reproduce it.

The **app** window is safe with the same owner because its band rule drops it
out of the topmost band and slots it directly above the client whenever a third
app is foreground. There is no moment where it is above the third app and able
to drag the client along. The overlay has no such moment to give up: being
above the game *is* its job.

**Chaining it to the app was also tried, and is also out.** It should have been
the safe version -- overlay owned by the app, app owned by the client, so one
activation lifts all three and the drag-the-owner-up effect hoists our own app
rather than the game. Measured with a deterministic probe (10 verified client
activations with the app open, counting EXTERNAL overlay z-moves -- reorders
someone else performed that we then correct, and the corrections are what the
flicker is made of):

| configuration | external overlay z-moves / 10 activations |
|---|---|
| no ownership at all | 0 |
| app owned by client (shipped) | 0 |
| overlay owned by app (chain) | 21 |

So the app's owner costs the overlay **nothing**, and the chain costs it a
reorder per activation. Neither direction of overlay ownership should be
re-attempted without beating those numbers.

**A correction worth recording, because it nearly drove a bad decision.** An
earlier count off the organic log said app ownership had taken the overlay from
0.016 to 0.86 external moves per activation -- a 54x regression -- and that
number was used to tell the user their badge flicker was caused by the app fix.
It was **contaminated by the probes themselves**: those harnesses set the
overlay's owner from outside the process, and each of those writes IS an
external z-move. Counting a log that your own instrument has been writing into
measures the instrument. The deterministic probe above, run against three
builds with the log cleared between them, is what settled it.

So the panel keeps its rare, documented blink, and that is the right way round.

Measured before building on it, each with a control that could go red:

- **Mechanism**, against the live client: two identical windows, neither
  topmost, neither moving itself, then the game activated. Owned sat at z=5
  with the game at z=6; the unowned control at z=7. Above and below
  respectively, on every pass.
- **The lifetime hazard** -- "owned windows are destroyed with their owner",
  which would have killed the console whenever Roblox closed -- **does not
  hold across processes**. An owner process was killed while an owned window
  in a second process was watched: `owner_alive=0 self_alive=1 visible=1` for
  the rest of the run. That cleanup is within-process.
- **The panel regression check PASSED AND WAS WRONG**, which is the part worth
  keeping. It confirmed the region was still present with the badge's box at
  `1443,391 44x66`, still topmost, and that clicking the region's centre still
  took the app from hidden to shown -- behaviour, not a style-flag read, and
  all true. It never clicked a **third app** into focus, which is what the
  overlay's owner actually broke. A check that exercises the surface you
  changed is not the same as a check that can go red on what you broke: the
  badge kept working perfectly while focus did not.

Owner, **not** parent: the window keeps its own focus, input queue and
lifetime.

Accepted costs, both of which are what "the console lives in the game" already
meant: our windows minimise and restore **with** the client, and can no longer
be placed under it. The overlay's show/hide valve already hid it when the
client was minimised or gone, so for the panel this is behaviour-neutral.

The band rules are unchanged and still own the question ownership does not
answer: whether we also float above **other** apps.

**The lesson worth keeping**, which is the same one this file keeps recording
in different clothes: a measurement taken from inside our own process
(`ECLIPSE_ZLOG`, "when did we ask") answers *when we called*, never *when the
screen was wrong*. It said our promote was fast, and it was -- while the user
watched a window flash. The disproof came from the user's sentence "roblox
came on top of the overlay and the app", not from any trace we took.

## The overlay gets the owner after all (2026-08-22, same day, later)

The section above said the overlay must not be owned because "raising ours
drags the client up". That diagnosis was close but wrong, and the difference
is what makes the fix possible. Mimic-probed against the live client, each
state shown both passing and failing:

- **Owning a window does not hoist anything by itself.** A client-owned
  topmost mimic sat through a full game-then-terminal click cycle with the
  terminal staying green (z + pixels).
- **The poison is an owned window EXITING the topmost band.** `NOTOPMOST` and
  a relative insert into the normal band both hoist the whole owner group --
  game, app, everything -- to the top of the normal band, over whatever app
  the user just clicked. The destination slot is ignored; the exit is the
  trigger. That is exactly what the overlay's demote does on every switch to
  a third app, which is why the first attempt broke the desktop everywhere
  while three probes said it was fine.
- **TOPMOST entries, in-band moves and the owner-set itself never hoist.**
  The owner-set at most pulls the owned window DOWN to its group. In-band
  raises of an owned window are clamped to the group.
- Once hoisted, a *fresh* activation of another window climbs back above the
  group; the old regression was permanent because the demote re-poisoned it
  on every foreground change.

Two consequences shipped, both in `eclipse_ui.cpp`:

- **The bracket** (`hud_swp`): every non-`HWND_TOPMOST` move of the overlay
  happens UNOWNED -- drop the owner, move, take it back. One funnel, so one
  bracket covers the tick demote, the band-lost glue and everything else.
- **Owner only while demoted** (end of `hud_tick`): the client owns the
  overlay exactly while a third app is foreground -- the state a tab-in
  starts from. Once the game is foreground the promote has landed and the
  owner is dropped, because holding it while the game is focused **hides the
  cursor over the badge** (CURSOR_SHOWING=0 sampled over the badge, game
  focused, owned; visible in the unowned baseline and in the owned
  third-app state; the per-tick cursor top-up does not win against it).
  That is the first attempt's input-queue bleed, still real in 2026.

What was measured, real terminal (the user's own WindowsTerminal window,
overlapping the client), coverage judged by z-relation AND pixels in the
overlap, focus checked separately because it is always fine:

| configuration (`ECLIPSE_HUD_OWNER`) | click the real terminal from the game | ext z-moves / 10 activations | blink: worst overlay-below-game, app open |
|---|---|---|---|
| 0 = no owner (old shipped) | green, 16/16 pixels | 0 | **8.8 ms** (more than a composed frame -- the visible badge flash) |
| 1 = owner, no bracket (the reverted attempt) | **RED: game covers the terminal**, 1-8/16 pixels, focus correct | 40 | -- |
| 2 = owner + bracket + owner-only-while-demoted (ships) | green, 16/16, 11/11 runs | 5, each one benign (below) | **0.10-0.20 ms** |

Notes that keep those numbers honest:

- Mode 2's externals are USER32's own enforcement carrying the owned overlay
  above the rising group at activation -- once per return, placed exactly
  where the badge belongs (directly above the app), followed by at most one
  successful heal. The `EXTERNAL z-move` tripwire's semantics changed with
  ownership; its comment in the code says so. A storm is still a storm.
- The carry is an ENFORCEMENT PASS, not an atomic move: at microsecond
  sampling the overlay is briefly passed (~2 ms worst app-closed, 0.2 ms
  app-open) before USER32 re-places it. Sub-compose, invisible; the earlier
  "atomic ride" reading of the mimic probe was resolution-limited
  (EnumWindows sampling at ~2-5 ms cannot catch it either way -- the fast
  `GW_HWNDNEXT`-walk sampler at ~10 MHz can, and it is also what proved the
  mode-0 8.8 ms dip it replaced the claim with).
- The blink instrument was proven able to go red on mode 0 before mode 2's
  green was trusted, and the coverage instrument on mode 1 (twice, plus once
  by mimic) before any green counted.
- Alt-tab with the app open activates the CONSOLE APP, not the game --
  alt-tab targets the most-recently-active window of the owner group, and
  the app has been owned since the app-ownership fix this morning. Not new,
  but first observed here; the pair still rises together.
- `ECLIPSE_HUD_OWNER` ships: 0 is the instant no-rebuild revert if any
  desktop misbehaves (this feature broke the user's desktop once already),
  1 is the negative control that reproduces the regression on demand.

### What ownership made dead, and what it did not (cleanup pass, same day)

The coverage boundary, stated once: the APP is owned for its whole visible
lifetime, so app-vs-client is settled structurally; the OVERLAY is owned only
while a third app is foreground (the owner drops when the client focuses --
the cursor finding), so in normal play the band machinery is still the only
thing holding the overlay up; and ownership orders windows against THE CLIENT
only -- overlay-vs-app, anything-vs-third-apps and the demote slot stay ours.

Removed under that boundary: **`app_ensure_above`** (and `app_below_client`),
the per-tick position heal shipped earlier the same day for "bit granted,
move never applied, app buried behind a foreground client for 900ms". With
the app owned, that state is unreachable: a hostile external SetWindowPos
inserting an owned window directly below the game returns success and USER32
restores it in **0.24ms** (~100x faster than the 16ms tick the heal ran on),
while the same insert on an unowned control lands and stays -- the control
that proved the probe could fail. The 900ms burial predates app ownership.

Evaluated and deliberately KEPT: every overlay promote/heal and the demote
glue (sole owner of overlay z whenever the client is foreground, and the
backstop for an external demote while owned, which bypasses the bracket);
the hook's client-click `raise_overlays()` (mode 0's click-path protection,
and one of the two gate-open moments that clear the band-lost latch); the
post-promote `raise_overlays()` calls (badge above app is sibling order,
which ownership does not touch); the app band rule (floating over OTHER
apps); `g_game_click_*` (close-gesture semantics, not z-order); and the
duplicate `g_hud_preempt_at` stamp in the foreground handler (provably
redundant with the one inside `raise_overlays`, left because its comment
carries the keyboard-path lesson and it costs one assignment).


## A floating snippet window could not be focused in app mode (2026-08-27)

Symptom, as reported: with overlay mode on and the app open over the game, run
`radar.cpp` (or any snippet with a window), focus the app, then try to click
back to the radar -- and the app comes forward instead. Alt-tabbing to the
radar did not work either. Intermittent: "sometime works tho for some reason."

**Root cause: the app band rule demoted in its own steady state.** The pair was

```cpp
if (app_fg && !app_top) promote;      // client foreground, app not topmost
else if (app_top)       demote;       // <-- fires when app_fg is ALSO true
```

With the client foreground the resting state is `app_fg && app_top`, which
falls into `else if (app_top)` and demotes. The next tick promotes, the next
demotes. **The rule could never rest** -- a z-order war at the 30 Hz tick rate,
two `SetWindowPos` round trips plus a `raise_overlays()` per cycle, running for
as long as app mode was up. Fixed by making the second condition exclusive:

```cpp
} else if (!app_fg && app_top) {
```

**Why it hid from the day it shipped.** With only the app, the overlay and the
game in the band, the promoted and demoted slots look identical on screen --
nothing is between them to reveal the difference. A floating snippet window in
the normal band is the first thing that ever sat in that gap: every promote
crossed the app over the focused radar, every demote dropped it back. The
intermittency was the race, not a second code path -- whether your click landed
before or after the war's next move.

Measured, same instrument either side: **468 z-order moves at ~30 ms spacing
before, 156 at 250-470 ms after.**

### The sibling restack -- the fix that actually resolved it

The band-rule war above was real and worth killing, but it was **not** what the
user was seeing. The burial came from one line in `hud_swp`, the funnel for
every overlay z-move:

```cpp
bool pause = g_hud_owner && after != HWND_TOPMOST && hud_owner_mode() == 2;  // was
bool pause = g_hud_owner && hud_owner_mode() == 2;                           // now
```

The ownership bracket (drop the overlay's `GWLP_HWNDPARENT`, move, restore it)
used to exempt TOPMOST moves. **`TOPMOST entries never hoist` is true of the
OWNER and incomplete about owned SIBLINGS.** A TOPMOST entry by an owned window
makes USER32 restack the rest of its owner group -- the client-owned app *and*
the game -- to the top of the normal band, **inside the same `SetWindowPos`
call**, burying any unowned window sitting above them. The unowned window here
is the focused snippet window, and it is buried 13 ms after its own activation
raise.

The chain, every step of it in the captured trace: user activates the radar ->
its activation raise lands (external, `flags=1803`, which is USER32's own raise
and correctly omits `SWP_NOACTIVATE` because it IS the activation) -> FG event
-> `hud_on_foreground` -> `hud_tick` -> the overlay's TOPMOST heal (needed
because the overlay demotes during the explorer interlude of any alt-tab or
taskbar transition) -> and USER32 restacks the owner group inside our own call.
The `ZWATCH APP` stack ending *inside* `hud_swp` is what proved the move happens
within our call rather than after it.

Probe-reproduced both ways on mimic windows with the same ownership shape:
owned HUD -> TOPMOST gave `HUD* APP GAME RADAR` -- app **and game** over the
focused radar, matching the report "making the roblox + the app go above"
exactly; bracketed, it gave `HUD* RADAR APP GAME`.

**This is also the intermittency.** An overlay that is already TOPMOST needs no
heal, so there is no restack and the click works. "Sometimes works" was that,
not a second path.

The bracket now covers all moves. It costs the owner-carry nothing: the carry
mode 2 exists for happens during the GAME's activation, outside our calls.

### Two wrong answers bought first, in order

Both looked right and both were confidently argued. Recorded so nobody spends
the afternoon again.

1. **"The NOTOPMOST demote buries it."** `HWND_NOTOPMOST` is the TOP of the
   normal band, so demoting the app lands it above a snippet window -- true,
   probe-confirmed, and the fix for it was written and kept. But that branch
   **never ran in the reported configuration**: it is gated on `app_top`, and
   the app's `WS_EX_TOPMOST` was measurably clear throughout. `zlog` showed
   the string "APP band" appearing ZERO times across a 300-line trace covering
   several app-enter and radar/app swaps. A fix in a branch that does not
   execute reads exactly like a fix that did not work.
2. **"It is not our code at all."** Every `SetWindowPos(ui_main_hwnd(), ...)`
   in `overlay.cpp` was audited: promotes and demotes in branches that were not
   firing, and all three position-only calls passing `SWP_NOZORDER`. Conclusion
   drawn: the raise must come from USER32 owner enforcement or the client. That
   was **wrong** -- the audit was of the call sites, and the war was two of
   those same sites firing from a state nobody had enumerated. Auditing what a
   line *can* do is not the same as measuring what it *does*.

### The instrument that settled it

`zwatch_poschanged` (`overlay.cpp`, gated behind `ECLIPSE_ZLOG=1` like the rest
of `zlog`): the app's wndproc and the snippet windows' subclass log every
z-order-changing `WM_WINDOWPOSCHANGED` with `hwndInsertAfter`, flags, `GW_OWNER`
at that instant, the foreground window, and **a call stack as module+offset**.

The stack is the part that mattered. `eclipse_console+...` frames above
`USER32` prove the move is ours; a pump-only stack (`USER32`/`ntdll` only)
proves it is external. Two distinct offsets alternating at 30 ms named the war
in one reading, after three rounds of reasoning had failed to.

**Keep this instrument.** It costs one `getenv` when disabled and it is the only
thing here that can attribute a z-order move to a caller.

### Two blind spots in the ad-hoc instruments, both of which misled

- An `EnumWindows` poller watching only OUR windows showed `Eclipse > Eclipse
  Radar` and was read as "the app is above the radar". Including the client
  showed the truth: `Eclipse > Roblox > Eclipse Radar`. **A z-order poller that
  omits the client cannot distinguish a raise from owner enforcement.**
- `GetWindow(GW_OWNER)` sampled while our process was NOT foreground returned
  NULL for the app and was taken as "ownership is not involved". The owner is
  set per state; `ZWATCH` showed `owner=<client>` at every sample during the
  actual fault. **Sample ownership at the moment of the fault, not afterwards.**

### What this says in general

The band rule is a state machine over `(app_fg, app_top)` and only three of its
four cells were ever considered. When adding a condition here, write the
transition table out and check that **every steady state is a no-op** -- a rule
that acts in a state it is already satisfied by does not converge, it
oscillates, and on screen that looks like an unrelated bug in whatever happens
to be sitting in the gap.

## The overlay goes UNDER the app window (2026-09-04)

While the app window is open (`g_app_mode`, steady), the whole overlay --
badge, docked snippet panel, flyout -- sits **directly below the app in the
z-order**, occluded where the app covers it and visible where it does not. The
user chose this over "badge above, dock behind".

**What it replaced, the same day it shipped: the APP HOLE.** The overlay rode
above the app (the badge as the app's close control) and the dock was put
"behind" by subtracting the app's rect from the window REGION -- DWM clips
composited pixels to the region, so pixels and clicks both fell to the app. It
worked, and it was a 1-bit RECT standing in for the app's real shape,
re-applied at the tick rate: the app's DWM-rounded corners showed the GAME
through the hole's square corners, and a drag moved the window per frame
while the hole followed at ~30Hz (a region rebuild per pointer move is a
stutter machine, "Drag cost" above). That was the "dock and app not seamless"
report, and it is not cheaply fixable inside the hole: a rounded hole guesses
DWM's radius, the shadow cannot be cut, and per-frame tracking is the measured
stutter. Z-order has none of it. The hole and its `^` rect prefix are gone,
not dormant -- two mechanisms for one ordering is this tree's bug class.

**The way out is verified, not assumed**: the app's titlebar close sends
`WIN:CLOSE`, which in app mode is `app_close_begin(true)` (`eclipse_ui.cpp`),
exercised with a synthetic click on the button: `APP close ... (hand=1)`, no
hotkey line, window hidden, focus handed to the client. Insert is the other.

### The mechanism, and the four things it took to make it clean

1. **One slot, one rule** (`overlay_glue_under_app`): the overlay's place is
   *directly* under the app, in whichever band the app's own rule put it --
   inserting after a topmost app lands in the topmost band, after a
   normal-band app in the normal band, always above the client (the app is
   owned by the client and never rests below it). `raise_overlays` takes this
   branch too while the app is steady, so every "ours above the game" caller
   keeps its meaning; mid-leg the overlay is TOPMOST as before (the travel
   box composes over the app). Verified by POSITION after the call, with the
   heal's 8-then-1/s backoff, because the entry gate returns success without
   applying. **Dead end: "somewhere between the app and the client" as the
   test.** It let the overlay rest in the normal band under a just-promoted
   topmost app, and the next owner-group hoist crossed it: 5-14ms of
   overlay-under-game per return click. Adjacent or nothing.
2. **The app window's ownership bracket** (`app_swp`, `app_owner_drop` /
   `app_owner_restore`), hud_swp's for the app. The 2026-08-27 note "the
   app's band exits measurably do not hoist" was made with the overlay
   TOPMOST above the app, where nothing of ours could be crossed. With the
   overlay now in the normal band whenever the app is, every band move of the
   owned app restacked its group and hoisted the client over the overlay:
   the hook's TOPMOST on a return click 11.4ms (the call blocks ~16ms
   synchronising with the owner's thread), the tick's demote 8.3ms, "APP
   stays" 1.5-2.7ms, the Insert close 7.5ms. Every band move of the app is
   bracketed now; the owner carry ownership exists for happens during the
   CLIENT's activation, outside our calls, so it costs that nothing.
3. **Overlay first on the way down, app first on the way up.** The tick's
   demote used to move the app and let the glue's second call follow 3-10ms
   later -- a composed frame of dock-over-console. Now the overlay goes to
   the destination's below-slot and the app in above it; the promote is the
   mirror. `app_close_begin` raises the overlay BEFORE the app's TOPMOST for
   the same reason (7.5ms of overlay-under-game at every Insert/badge close
   without it).
4. **The two non-SetWindowPos group moves.** The summon's client insert now
   goes *after the overlay* with the app unowned: after the app it put the
   client between app and overlay for ~20ms (cross-process call), and with
   the app owned USER32 re-placed it under the overlay inside the call
   (4ms). The return-from-a-third-app hand-over runs with the app unowned
   and the owner **held off for 400ms** (`g_app_owner_holdoff`): the click's
   own activation of the client follows the hook, and the owner carry
   re-placed the owned app under the topmost overlay for 11.5ms until "APP
   stays" put it back (ZWATCH with `owner=<client>` and a pump-only stack).
   Unowned, there is nothing to carry.

### Measured (`build/zprobe.exe --watch N app`, z-walk sampler at ~0.24ms)

The sampler reports the order of app/overlay/client top-down (`AHG` is right
with the app up, `HG` with it hidden) and times every other order. It was
proven able to go red on the first build of this design before any green was
trusted; the two probes it replaced could not see the failures at all
(below).

| gesture | first build (loose slot, no app bracket) | shipped |
|---|---|---|
| click the client from a third app (return) | 5 wrong intervals, AGH 6.7/2.5/7.1ms + HAG 3.7/5.7ms | **0** |
| click the app from a third app (summon) | HAG 4.0ms (and ~20ms AGH before the insert change) | **0** |
| Insert/badge close | AGH 7.5ms before the box | 0 (the box interval only) |
| app open, 20s passive, client playing | -- | 96654 samples, 0 changes |
| app open, badge overlapped by the app | -- | region rect intact, `WindowFromPoint` = the app, pixel = the app's |

The opens and closes still show the DESIGNED intervals: the app shown under
the box before it takes foreground (`HGA`, ~10-20ms) and the overlay above the
app while the box travels (`HAG`, 40-100ms); they are the choreography, not
z-order faults. The only residue is the overlay being absent from the
`GW_HWNDNEXT` chain for 0.3-2.8ms during our OWN SetWindowPos of it (a relink
mid-call) -- sub-frame, and inherent to any z-move.

### Instrument traps this round paid for

- **`Sleep(1)` is 15.6ms.** The first "1ms" sampler took 902 samples in 14s
  and could not separate 3ms from 16ms. Spin on QPC instead.
- **A probe that compares only overlay-vs-app cannot see overlay-under-game**,
  which is what every flash here actually was. The old `holeprobe` reported
  "APP over HUD" as the fault and would have called the 20ms summon burial a
  pass. Walk all three.
- **Tick stamps in both logs** (`zlog` now prints `GetTickCount64`) are what
  lined a 10ms interval up with the call that caused it; the QPC-vs-tick
  offset guessed from "the click was about here" was off by up to 100ms and
  pointed at the wrong call twice.

### Not verified (needs the user's hands)

A docked panel overlapping the app (the seam itself, and its region staying
whole) and typing into a docked panel's text field with the app open
(`overlay_focus_enter` re-glues after taking foreground; not exercised). The
bracketed band moves were measured on the app/overlay/client triple only --
a floating snippet window in the band was not in the scene.

### The dock-in-the-scene faults (2026-09-04, user testing with a real dock)

With a docked panel actually present, four faults the app/overlay/client
triple could not show. All are the DOCK -- i.e. the overlay host -- going
**behind the GAME** (occluded by the client), except (4) which is the
opposite. Confirmed by the user "the docks blinking behind roblox".

The shared root of (1),(2),(4): the app is owned by the client, the overlay
is NOT while the console is foreground, so the app's owner-group carry moves
one of {client above overlay, overlay above app} on a foreground/activation
change, and the tick's under-app glue only corrected it a tick (~16-33ms)
later. The fixes react at MESSAGE latency instead of tick latency:

1. **Click the app's title bar -> dock behind the game (short).** The click
   activates the app; USER32's activation carries the client above the
   overlay. Measured with the 0.24ms sampler: **26ms of overlay-under-game**,
   and a Z snapshot INSIDE the app's `WM_WINDOWPOSCHANGED` proved the carry is
   already done by the time we hear (app rank 28, game 31, overlay 40) -- the
   carry is a move on the CLIENT, another process, so nothing but the app
   message is left to react to. Fix: `app_zorder_changed` (overlay.cpp, called
   from the app wndproc's `WM_WINDOWPOSCHANGED` in eclipse_ui.cpp) re-glues the
   overlay under the app in that same message, for any z-move of the app that
   is not ours (`g_app_in_swp` guards our own). **26ms -> ~7ms** (single-digit;
   the residual is the window between the kernel carry and this thread's
   message; a synthetic titlebar click that trips the SC_MOVE drag loop shows
   an occasional larger outlier that a real single click does not). NO
   `overlay_under_app` early-out -- it read the pre-carry instant and skipped.
2. **Click the dock, then the app -> dock behind the game ~500ms.** The
   app-click half is (1). The dock-click half parks Win32 focus on the
   browser process (and may enter focus mode); UNVERIFIED end-to-end because
   it needs the iframe. Same two reactive catches apply.
3. **Clicking the dock closes the app to the badge and focuses the game**
   (user decision: the dock is in-game UI, so touching it means "back to the
   game"). Wired page-side: the frame shim posts `__ec_click` on mousedown,
   `hud.html` relays `DOCKCLICK:1`, the host calls `app_close_begin(true)` --
   but ONLY while the app is open (`g_app_mode` && not mid-leg). Click
   classification: it fires only for a click INSIDE the docked iframe (the
   shim is injected only there), and a PASSTHROUGH dock has
   `pointer-events:none` so its iframe never sees a mousedown -- double-guarded
   with `!dock.pass` in the page. A click that lands in the overlay but not on
   the dock (badge, panel gap) never carries the shim, so it cannot trip this.
4. **Click the dock with the app in front -> dock flashes IN FRONT of the
   app.** The browser child's activation raises the overlay host to the top of
   its band, above the app. Caught in the overlay host's `WM_WINDOWPOSCHANGED`
   (`hud_host_wndproc`, the existing EXTERNAL-z-move branch): while `app_steady`
   and not already under the app, re-glue under it in the same message. The
   user notes (4) is still wrong even though (3) closes the app, because a
   frame of dock-over-app before the close begins is visible -- the re-glue
   fires before `app_close_begin`, so the raise should not reach a compose.

### Badge-close from a THIRD app (2026-09-04, user), and the who-moves-the-windows audit

Fault: app open, a third app foreground, click the badge to close -- the app
AND the overlay blinked behind the game. Reproduced with the badge as witness
(no dock). Two causes, both from a state nothing had been measured in (a THIRD
app foreground at the close, so `console_fg` is false and the overlay is in
the demoted normal-band slot):

1. **The summon fought the close.** The LL hook marks any overlay click from
   a third app as a SUMMON (`g_hud_used` -> `overlay_handback` force-foregrounds
   the game a tick later). In app mode the badge is a CLOSE control and the
   page's `WIN:CLOSE` already runs `app_close_begin(true)` -- so two
   independent hand-overs and owner rewrites raced. Fix: the summon mark is
   now gated on `!g_app_mode`. The app-mode close is the one owner of that
   transition.
2. **The close dropped the app's owner from a state where the owner was the
   only thing holding it up.** `app_close_begin` did `app_swp(TOPMOST);
   app_owner_drop()` -- correct when WE are foreground (the TOPMOST takes and
   the owner-carry would only add a 3-4ms re-place), fatal from a third app.
   THE TRAP: the first fix keyed the drop on the topmost BIT
   (`GWL_EXSTYLE & WS_EX_TOPMOST`), and measured GAH (game over app+overlay
   ~15ms) STILL happened with no `refused` line -- because from a third-app
   close **the entry gate grants the bit without applying the move** (the same
   bit-granted/move-swallowed state `app_ensure_above` once handled, from the
   topmost-fight notes above). The bit read true, the owner was dropped, and
   nothing carried the app. Fix: key the decision on FOREGROUND, latched once
   at close into `g_app_close_keep_owner` (a third app foreground -> keep the
   owner; us foreground -> drop it). The tick's app-owner block and the
   overlay owner block both read the latch for the leg; `close_finish` drops
   it at the hide. **Lesson, again: never trust the topmost bit as evidence
   the window is where the bit says -- the gate lies about it. Verify by
   foreground or by position.**
3. **The overlay also dipped after the app closed** (same third-app state).
   Same fix: the overlay is kept owned through a third-app close
   (`close_carry = g_app_closing && g_app_close_keep_owner`), so the client's
   activation carries the badge above it too. A console-fg close leaves it
   dropped -- it was topmost throughout and stays clean.

Measured, badge as witness (0.24ms sampler), each state-controlled (ensure
closed -> open -> third app -> badge; the multi-gesture sweep DESYNCS -- a
mis-timed click toggles the app and every later click misaligns, so it is not
a valid instrument here): **third-app badge close** 0 wrong on 3 of 4 runs,
2.0ms worst on the 4th (was GAH ~15ms); **console-fg close** 0 wrong x3
(unchanged, perfectly clean); **title-bar x3** 0 wrong. Return and summon are
unaffected -- the latch is read only under `g_app_closing`.

### The client left TOPMOST (2026-09-04, user: x-close from a third app "makes the roblox z axis bugged and unable to focus other app")

Not a blink: a persistent state on a window we do not own the lifetime of.
Reproduced deterministically with the four-cell probe (`zprobe --client`:
the client's `WS_EX_TOPMOST` and z-rank before/after each gesture): x-close
with a third app foreground left the client `ex 0x100 -> 0x108 TOPMOST=1,
z 39 -> 12`; badge/third-app, x/ours and badge/ours all clean.

**Mechanism -- neither suspicion, and my own change.** The x is a click on
the APP, which ACTIVATES it (the badge never activates). The activation's
tick promotes app+overlay TOPMOST (`console_fg`, new today), and the FG
handler's summon then runs `SetWindowPos(game, after=<overlay>)` -- and
**a window placed after a TOPMOST window becomes TOPMOST itself**. Nothing of
ours ever clears it. The trace shows the race in order: `APP band ->
TOPMOST (console)` at +0ms, `APP summon: raising client under the overlay`
at +10ms. Before `console_fg` the app was in the normal band whenever we
were foreground, so the insert could never land after a topmost window --
the summon was safe for the same reason the badge cell is: no promote runs
between the click and the insert. Neither the owner (dropped around that
insert) nor `g_app_close_keep_owner` (read only under `g_app_closing`,
cleared with it) was involved.

**Fix, mover and lifetime:** the summon insert checks the slot's
`WS_EX_TOPMOST` and uses `HWND_TOP` instead -- for a non-topmost window that
is the top of the NORMAL band, directly under every topmost window, app and
overlay included, with no bit. And `client_topmost_guard`, after that insert
and once per tick: a client found TOPMOST is stripped (`HWND_NOTOPMOST` from
our process works on the client's window: ret=1, bit clear -- probed) with
a one-time WARN naming the site, so a recurrence is visible and heals within
a tick rather than lasting until a Roblox restart. Result, all five cells
(x/third-app x3, badge/third-app, x/ours, badge/ours, plain summon): **bit
0 -> 0, z 36 -> 36**, 0 wrong intervals; the fix fired on 3 of 4 summons.

**If a client is ever stuck:** attach the console (the tick guard clears it)
or restart Roblox. The invariant every close and summon must hold: the
client's topmost bit and band are the same after as before.

**Who moves the app and the overlay, after today.** The map the audit asked
for -- every z-mover, and the one rule each obeys:

- **`hud_swp`** -- the only mover of the OVERLAY, owner-bracketed (drop owner,
  move, restore) so an owned band-exit never hoists the group. Callers:
  `overlay_glue_under_app` (slot under the app while it is in front),
  `raise_overlays` (TOPMOST when the app is hidden; the under-app slot when it
  is), and the tick's promote/demote/heal (only when the app is NOT in front).
- **`app_swp`** -- the only band-mover of the APP, owner-bracketed the same
  way. Callers: the app band rule (TOPMOST while client OR console foreground,
  the relative-under-client slot for a third app / focused snippet window),
  `app_close_begin`/`app_open_finish` (TOPMOST for the leg), the FG handler's
  "APP stays", the hook's "APP return".
- **Plain `SetWindowPos(app, ..., SWP_NOZORDER)`** -- position only, the tick's
  client-follow and `app_drag_settled`. Never touches z.
- **`SetWindowPos(game, after, ...)`** -- once, the summon backdrop insert, now
  unreachable in app mode (see fix 1) except the non-app summon it was written
  for.
- **Owner writes**: the app owned by the client for its visible life (tick);
  the overlay owned by the client while demoted, held-off after a return, or
  mid-third-app-close (tick); both cleared in `app_abort_restore` and
  `hud_destroy`. Nothing is owned by the app -- the `app_abort_restore` comment
  that said so was wrong and is corrected (the code was right).

**Two reactors to the same event, checked for a fight:** `app_zorder_changed`
(app's `WM_WINDOWPOSCHANGED`), the overlay's `WM_WINDOWPOSCHANGING` veto and
its `WM_WINDOWPOSCHANGED` re-glue all converge on one invariant -- overlay
directly under the app while it is in front -- and all reach it through
`raise_overlays`, which no-ops when it already holds. Idempotent, not
fighting; three entry points because the restack can notify via the app, the
overlay, or neither. **Every band steady state is a no-op** (verified against
the transition table): console-fg -> app TOPMOST, rests; client-fg -> app
TOPMOST, rests; third app -> app under client, rests; the promote and demote
slots are only taken when the read disagrees.

**Suspicions written down, not fixed on a hunch:**
- **`under-app insert did not take` false positives.** Our own `MSCTFIME UI`
  / `IME` window transiently sits directly under the app during the app's own
  TOPMOST move, so the adjacency test counts a non-refusal and ticks `fails`
  toward the 8-count backoff. Never observed reaching 8 (it resets the next
  tick), but a real refusal coinciding with the transient could trip the
  backoff early. The diagnostic now names the interloper. Left as-is.
- **The `WM_WINDOWPOSCHANGING` veto is still unexercised on a real dock.**
  Every rebuild since it landed killed the user's dock, so nothing has clicked
  a docked panel on a build that carries it. The dock-click flash (4) is
  expected gone by it; confirm with one dock click, app in front, and watch
  for a `VETOED` line.
- **`app_ensure_above` is gone and its state is claimed unreachable** by
  ownership; with the app now also promoted while the console is foreground,
  that claim is wider than when it was written. No repro; noted.

**A regression shipped for three builds while doing this, and the build line
was green every time.** Inserting the `DOCKCLICK:` branch into `hud_on_msg`
replaced the `else if (RECTS:)` line without re-adding it, so the RECTS
handler body sat inside the DOCKCLICK branch: the page's rects were never
applied, the region stayed EMPTY, and the badge and any dock were neither
drawn nor clickable -- for the user's own relaunch, for two minutes, until
`zprobe` read `region: 0 rects` and the badge's home sampled one uniform
colour. The z-order measurements of that period stand (they are on the host
window, not the region), the app still opened via Insert (host-side, the
watchdog covers a dead page). Caught by the region snapshot, not by any
build or test: `grep -c "RECTS ->"` in the trace read 0 with `MSG <RECTS:`
lines present. Same class as the `if (dock) {` deletion earlier in the day
-- a patch that lands next to the right line is not a patch that kept it.

### Second pass, same day: the badge is the witness (user: "the badge is also an overlay so testable via that")

The reactive re-glue above was not enough -- the user still saw (1) and (4).
The badge is the same window as the dock, so (1) needed no dock at all, and
re-measured with the app at the user's own position the number was **13-44ms
of overlay-under-game per title-bar click**, with NO `app_zorder_changed`
line: a click on the title bar of the already-active app does not move the
APP (SC_MOVE's bring-to-top), it re-places the owner group -- only the CLIENT
rises, over the overlay, and nothing tells us. The 7ms measured before was a
different path (the app not yet active). Reacting cannot fix a move nobody
reports; the fix is structural:

- **The app is TOPMOST while the CONSOLE is foreground too** (`console_fg`
  in the app band rule: ours and not a snippet window, or the overlay's
  browser). The old "ordinary window when the console itself is focused --
  a focused window needs no bit" was true of the app and false of the overlay
  under it: in the normal band any raise of the app hoists the client over
  the overlay; in the topmost band the hoist stays in the NORMAL band, under
  the topmost overlay, and there is no interval to catch. The demote for a
  third app or a focused snippet window is unchanged. Result: **0 order
  changes over three title-bar clicks** (0.24ms sampler; 13-44ms before).
- **The slot keys on `app_in_front` (the app VISIBLE), legs included, and
  the legs never put the overlay above the app.** The box only has to be
  composed before the hide / dropped after the show; beneath the app it
  is. What the legs need is the overlay above the CLIENT's coming raise,
  which is the app's TOPMOST plus the slot under it. `app_close_begin` does
  `app_swp(TOPMOST); raise_overlays(); app_owner_drop()` on both paths (the
  owner is off for the rest of the leg -- the client's activation re-placed
  an owned app 19ms after every APPCLOSE and the overlay sat above it, box
  drawn, for 3-4ms); `app_open_finish` does `app_swp(TOPMOST);
  raise_overlays()` before the hand-over. Result: the close legs read
  `AHG -> HG` with no `HAG` at all (was 3.7 / 3.0ms of overlay-over-app at
  the start of each close), the open legs `HG -> AHG` directly (was 48-53ms
  of box-over-app before the glue); the return click 0; the summon 0.3ms.
  Final sweep on the shipped build: 34s, 133,503 samples, three title-bar
  clicks, two Insert/x closes, two opens, a return and a summon -- ONE wrong
  interval, the summon's 0.3ms.

**Instrument note.** A GDI pixel sampler at the badge (`zprobe --pix`) FAILED
its control -- a BitBlt-to-memory copy read the badge through a covering
window, and GetPixel(screen) costs ~42ms a sample -- and is kept only as the
record. What was PRESENTED would need DXGI output duplication; the z-order
walk is the witness for all of the above.

**Status after the second pass.** (1) measured to zero with the badge as the
witness. (3) confirmed by the user. (4) measured on the user's REAL dock
(four of their clicks in the trace, then two of mine with the sampler
running): the dock click ACTIVATES THE OVERLAY HOST ITSELF -- the FG event
names our host, not a browser-pid window (the "browser pid" reading in older
notes was this) -- and the activation raises it to HWND_TOP, `overlay
EXTERNAL z-move: after=0 flags=1803`, above the app; the CHANGED-catcher
re-glue lands 0.5-1.1ms later in the same message. Sampler: **HAG 1.5ms and
2.6ms** per dock click, then the app on top through the whole close leg
(`AHG` 49-57ms, `HG`) -- the dock-click close matches the Insert/x route
exactly (DOCKCLICK arrives 11-12ms after the raise; BOXUP +32ms; hidden
+53ms). Before the console-foreground band change the same raise put the
host above a NORMAL-band app until the tick -- the flash the user saw.

The last 1.5-2.6ms is now VETOED rather than corrected: the activation raise
arrives with `WM_WINDOWPOSCHANGING` (flags 1803, no NOSENDCHANGING), so
`hud_host_wndproc` sets `SWP_NOZORDER` on any external raise WITHOUT
`SWP_NOACTIVATE` while the app is on glass -- the discriminator is in the
trace: activation raises carry 1803, USER32's owner carries 1813, and the
carry (which places the overlay above a rising client) must pass. Verified
on a sweep without a dock: no VETOED line fires on titlebar / open / close /
return / summon (the carries pass, `AHG`/`HG` throughout, one 0.3-0.4ms
chain-break blip); the veto ITSELF has not been exercised -- it needs a
dock click, which only the user can make after this build.

`under-app insert did not take` lines in the dock trace were FALSE
POSITIVES of the adjacency test: `ret=1`, then adjacent at the next tick
with no further insert -- a transient interloper directly under the app
during the app's own TOPMOST move, not a refused insert. The diagnostics
now name the window under the app when it happens. They never reached the
8-fail backoff (fails reset on the next adjacent read).

**(2) is LATENT, not proven fixed.** The user can no longer reach "click the
dock, then the app" (3 closes the app on the dock click), so the ~500ms
under-game state was never reproduced or timed. The only mechanism found
that fits the duration is the under-app insert being REFUSED by Windows'
entry gate (docs above: relative inserts above the game are gated, the
foreground process is exempt -- and after a dock click the foreground is
the overlay's BROWSER process, not ours), with `overlay_glue_under_app`
backing off to once a second after 8 refusals: ~0.5-1s under the game until
a retry took. The console-foreground band change removes the need for that
insert in this sequence (the app stays TOPMOST across a dock click, so the
app click's hoist stays in the normal band), which is why it is expected
gone, not shown gone. It would resurface for any path that drops the
overlay out of the topmost band while a browser-pid window is foreground
and the app is open -- watch for `under-app insert did not take` in the
trace. The fix if it does: exempt-time entries only (hook / FG-event
paths), the way the band-lost latch is cleared.

### The open leg's two flashes were one hold with the wrong witness (2026-09-04, later)

User, after the Fault A fix (the box held above the app until the hud page
drops it): a docked panel FLASHES above the app as it opens, and "kinda
often" the box ends up on top of the already-painted app -- three frames:
box travelling, app shown with the box's edges framing it, box fully on top.
Both a duration, not a state.

Measured with DXGI desktop duplication (`build/zdup.exe`: presented frames
sampled at the app titlebar, app body, inside the centred dock, the badge and
two game-only points; z-walk at ~0.2ms; SendInput gestures; `GetTickCount64`
stamps lined up with `eclipse_z.log`), the dock centred directly under the
app, five opens: **the box composed over the ALREADY-SEEN app for 8-11 frames
/ 56-63ms, the dock over the painted app for 1-3 frames / 4.6-12.7ms**, both
intervals ending at the same line, `overlay -> under app` from the
`APPUNCOVER` handler. The z-walk said why the app was seen before the drop:
under the hold, `app_open_finish` still ran `app_swp(TOPMOST)` and
`force_foreground`, so the app crossed above the box TWICE (the promote, then
the activation raise) and `raise_overlays`' TOPMOST branch re-covered it each
time -- a painted app seen for a frame, then the box on top of it. And the
hud page dropped the box a few ms BEFORE the host's glue landed, with the
overlay still above the app, so whatever the overlay held under the box --
the dock -- showed through for those frames. One hold, wrong witness: the
hud page confirming its box drop says nothing about whether the APP has
painted, and it arrives after the drop by construction.

**Shipped:** the witness is now the APP page's own double-rAF after the show
(`APPPAINT` -> `APPPAINTED`, the close leg's `APP_BOXUP` instrument applied
to the other window), and the leg is split: `app_open_finish` shows the
window under the box and asks; `app_open_uncover` (on the witness, or the
tick's 250ms belt, or the 1.5s watchdog) does one `DwmFlush`, promotes the
app above the box ONCE, glues the overlay under it, takes foreground, and
only then sends `APPSHOWN` -- so the page drops the box BENEATH the app,
where it changes nothing on screen. `APPUNCOVER` is a belt now, never the
first glue. Same instrument, 14 opens (Insert and badge, the process's cold
first open included): **box over the seen app 0 frames, dock over the app 0
frames, game in the app rect 0 frames; the witness lands 15-16ms after the
show; the app crosses the box in one 0.1-0.4ms z-change.** The check went
red on the previous build's capture before its green was trusted.

Cost, stated: the box now rests at the rect for ~35-60ms before the app
appears (witness 16ms + `DwmFlush` + the owner-synchronised `app_swp`),
against ~14ms before -- a longer travel end, no bounce.

Also fixed on the way, cheaply: `overlay_under_app` skips INVISIBLE windows
in the chain (our own `MSCTFIME UI` sat directly under the app after every
promote and made the strict next-window test a permanent false negative --
3 `did not take` lines per open, `fails` ticking toward the backoff) and
compares the band bits so it cannot accept the normal-band-under-topmost-app
resting place the loose test once allowed. Zero `did not take` lines and
zero `under app` inserts across the 14 opens: the glue is a no-op at every
site now.

Not verified this round: typing into a docked panel's field with the app
open (needs hands), the dock-click close (the centred dock is under the app,
so no click can reach it while the app is open), the third-app SUMMON
(the script's off-by-one turned it into a third-app close, which was clean:
`AHG -> HG`, no `GAH`), and the rare full-screen blank -- ~14,000 more
presented frames without it, which is not evidence of anything.

### The COLD open: the promote is swallowed, and the whole UI shows the game (2026-09-05)

The user saw a blink on the FIRST open of a console process after the fix
above, never on later ones. The trace of the cold open differs from every
warm one: no `ZWATCH ... flags=1813` for `app_open_uncover`'s
`app_swp(TOPMOST)`, three `overlay -> under app` glues instead of none, an
`APP band -> TOPMOST` from the tick AFTER `APP shown`. Read against the
topmost-fight notes above: **our process has never been foreground, so the
entry gate swallows the app's TOPMOST** (ret=1, err=0, no z-change), the
glue then puts the overlay into the NORMAL band under a normal-band app,
and `force_foreground`'s activation raise of the client-owned app hoists
the CLIENT over the overlay; the reactive re-glue and the tick's band rule
clean up 2-20ms later.

Measured (cold-open sampler: kill the console, relaunch with
`ECLIPSE_ZLOG=1`, wait for the badge, one open under `zdup.exe`, repeat --
4 with the game foreground via `SwitchToThisWindow` from the probe's own
foreground window, 4 summoned from a third app): **8 of 8 cold opens
presented 1-5 frames (7-30ms) in which the app titlebar, the app body AND
the badge all read the game** -- `ffffff` over a bright scene, the game's
own `444446/404042` UI over a dark one -- between the box at the rect and
the app appearing; the z-walk shows `GAH`/`AGH` for 2-16ms each time. The
warm opens in the same captures: 0.

**Shipped:** `app_open_uncover` takes FOREGROUND FIRST, with the overlay
still TOPMOST above the app and the box covering it (the activation's
owner-group hoist stays in the normal band beneath a topmost overlay --
nothing visible changes), and only then promotes -- as the foreground
process, which the gate exempts -- and glues under. `app_swp` now logs
`APP swp TOPMOST ret= err= top= above_hud=` (position, not the bit: the
gate grants the bit without the move) so a swallowed promote is a trace
line. Same protocol after: **0 of 8 cold opens with any wrong frame, no
under-game interval in any z-walk, `above_hud=1` on all 8**; four warm
opens re-measured on the same build: 0 box-over-seen-app, 0 badge-wrong,
0 game-in-rect. Cost: the app appears ~30ms later on every open (the
hand-over now precedes the crossing; seen at +261..357ms after the gesture
against +266..286 before).

**On the rare full-screen blank.** What the before-captures show -- every
one of our sample points reading the game, for a duration set by a
cross-process carry and our reaction to it, darker or brighter with the
scene -- is the same SHAPE as the blank the user has reported since the
start, and this fault was 8/8 on cold opens. It is NOT shown to be the
blank: this one is once per process and 7-30ms, the report was "about 1 in
4" and read as longer. If the blank persists on a build carrying this fix,
it is something else; if it stops, this was probably it. Both remain
possible on the evidence here. (The user has since decided to stop chasing
the blank on the strength of this being the most likely cause.)

**The dock re-measured on the reordered build (same day, later).** The
warm re-measure above had no dock. With `docktest.cpp` running (centred,
under the app) in one console process, no restart, six warm opens (Insert
and badge alternating), sample points derived LIVE -- the app rect from the
window while visible (it had been moved to 400,195), the dock and badge
from the client's client area -- and the guard that the points were
actually reading something (282 dock-coloured frames while closed, 1168
app-painted frames while open): **dock-over-app 0/6, box-over-seen-app 0/6,
badge 0, game-in-rect 0, no under-game interval, `above_hud=1` on all six.**
The drop path (box dropped beneath the app after the glue) was not affected
by the foreground-first reorder; a clean negative. NOT measured: a cold open
with a dock present -- a fresh process has no running snippet, so it needs
a manual F5 between launch and the first open per cycle, and nothing here
presses F5 with the user's real snippets one tab away. The badge is the same
HWND as the dock and was the witness on all 8+8 cold opens, so the z-order
half of that question is covered; the box-drop half on a cold open is not.
