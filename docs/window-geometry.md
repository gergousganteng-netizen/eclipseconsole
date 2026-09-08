# The app window's geometry: one window, one store

RESOLVED 2026-09-06, after five patches that each kept two stores and tried to
make them agree. Kept because the dead ends are the expensive part and the rule
below is easy to break by accident.

## The rule

**The app window's geometry is ONE screen rect, and switching modes never
changes it.** App mode and hud mode differ in chrome, owner, taskbar presence
and z-order -- not in where the window is or how big it is. It is the same
HWND; the user had to say so twice (*"they are both a window btw not
overlay"*).

Concretely:

- `%APPDATA%\EclipseConsole\window.txt` (`x y w h`, screen coordinates,
  physical px) is the **only** store, written by the **only** writer,
  `write_window_rect` in `eclipse_ui.cpp`. Both modes write it through
  `ui_geometry_changed`.
- Entering hud mode carries the rect the window occupies **right now**,
  clamped into the client (`app_anchor_capture`, `overlay.cpp`). Hud mode's
  opening geometry IS app mode's closing geometry, by construction -- there is
  nothing left to keep in step.
- **Maximised and snapped are states, not sizes.** A zoomed window carries its
  `rcNormalPosition`, and the desktop `WM_EXITSIZEMOVE` stores that too -- a
  drag to a screen edge ends in the same modal loop, so `GetWindowRect` there
  wrote "half the monitor" into the remembered size. Both writers now agree
  with `save_window_rect`, which always used the restored rect.
- Leaving hud mode restores the **show state only** (`app_restore_show`).
  Maximised is a state and comes back; the rect does not move.
- The client-relative anchor (right inset + centre Y) still exists, because the
  window must follow the Roblox window when it moves or resizes. It is
  **derived at the open and thrown away** -- never written to disk, never
  re-derived per tick.

## What the two switches mean

Stated once, because "undefined" was the actual bug:

| | `rememberPos` on | `rememberPos` off |
|---|---|---|
| **at startup** | reopen at the stored position | centre on the cursor's monitor |
| **on a mode switch** | carry the current rect | carry the current rect |

Same for `rememberSize`, with the default size in place of centring. The
switches govern **startup and persistence only**. They never make the two modes
answer differently, which is exactly what they used to do: with position off,
app mode centred on the monitor while hud mode kept an anchor of its own, and
that disagreement was what the user was looking at.

`restore_window_rect` reads **no flag** -- see the ordering trap below. The file
carries a sentinel per half: `-1 -1` for "no position wanted", `0 0` for "no
size wanted".

## What actually broke it

Two independent holes, both position-only, which is why size looked synced and
position never did:

1. **Both exits restored a pre-entry snapshot.** `app_open_begin` captured
   `GetWindowPlacement` before hud mode opened, and `app_close_finish` /
   `app_abort_restore` put that whole placement back -- teleporting the window
   to wherever it had been on the desktop and discarding every move made in the
   game. No file merge could fix this; it never consulted a file.
2. **The desktop position was only recorded at exit.** `WM_EXITSIZEMOVE` on the
   desktop called `ui_size_changed` and nothing else, so moving the desktop
   window and then entering hud mode used a stale position.

## Five dead ends, and why each failed

1. **Mirroring two stores.** Each side wrote its size into the other's file.
   Whichever wrote last won, and a mode that was never closed never wrote at
   all, so they drifted apart silently.
2. **`overlay_pref()` read as "the window is in the game".** It is the standing
   SETTING (app-as-HUD is available), not the current state. With the setting on
   and the desktop window in use, this skipped `save_window_rect` entirely and
   NOTHING was remembered. The right question is `overlay_app_mode()`, captured
   BEFORE `app_abort_restore()` clears it.
3. **Saving the desktop rect while in hud mode.** Correct *while the in-game
   position had a store of its own* -- the rect there was an anchored position
   and writing it over the desktop geometry lost both. Under one store the
   objection dissolves: the in-game rect is a screen rectangle like any other,
   and `WM_CLOSE` now saves in both modes.
4. **Reading a settings flag during restore.** `restore_window_rect` runs from
   `WinMain` while the window is being created -- BEFORE the page has loaded and
   pushed the settings across, so the flags are still their compiled defaults.
   THE NASTIEST CONSTRAINT HERE, and it applied to the size half too, silently:
   a user with "remember size" off got their old size back on every cold start.
   Fixed the same way as position -- a sentinel in the file, and no flag read at
   restore time.
5. **Deriving the anchor from the remembered screen position inside
   `app_target_rect`.** That function runs on every client move and resize, so
   the anchor was recomputed hundreds of times a session against a position that
   only changed at drag-end -- which pinned the window to a screen point and
   stopped it following the game. Capture once at the open instead.

## When the open takes a new rect, and when it keeps the anchor

`app_open_begin` re-captures only if the window **has moved since we last put
it somewhere** (`app_moved_since_placed`; every placement we make records its
rect through `app_placed`). Untouched, it keeps the anchor it has and goes on
following the game -- otherwise closing to the badge, moving the Roblox window,
and reopening would drag the console across the screen, or into a corner if the
client had changed monitor.

**The test cannot be "is the window visible".** That was tried and it is wrong:
the overlay valve hides the desktop window the moment a client attaches, so by
the time the badge is clicked visibility answers *no* for a window the user has
just dragged across the desktop. Both reported forms of the bug are that one
mistake -- move it while detached then attach, and move it in app mode then
switch app-as-HUD on. A never-placed anchor also counts as moved, which covers
the first open of a session that started in overlay mode.

`ECLIPSE_ZLOG=1` prints the decision: `APP carry TAKEN|kept from x,y wxh ->
right=... cy=...` at every open.

## What must not regress

This subsystem cost two agent runs and a day of measurement on 2026-09-05:

- the cold-open fix -- the paint witness (`APPPAINT` -> double rAF ->
  `APPPAINTED`) gating one promote before the travel box drops beneath the app;
- the `WM_WINDOWPOSCHANGING` dock veto;
- `client_topmost_guard` and the `after_top -> HWND_TOP` guard, which exist
  because the client was once left `WS_EX_TOPMOST` -- Roblox above every window
  on the desktop until restarted;
- badge-above-app and dock-behind-app, deliberately asymmetric;
- docked-panel keyboard focus (`overlay_focus_enter`/`exit`);
- the app following the client in hud mode;
- the cloak-before-restore on `WM_CLOSE`, which is what stops the app fading
  into the game on the way out.
- `WS_MAXIMIZEBOX` **and** `WS_MAXIMIZE` stripped in
  `app_apply_overlay_styles(true)`, the capability restored on the way out and
  the state never. The capability is what makes Windows offer edge-snap and the
  Snap Layouts flyout, and a snap to the top edge IS a maximise: the console
  came back zoomed to the monitor, every `SetWindowPos` after it fought Windows
  for the rect (the drag-settle clamp, then the tick's follow), and a zoomed
  window cannot be edge-resized. Reachable from both directions -- snap it
  inside the game, or snap it on the desktop and then attach. The refusal
  already existed one layer up (`handle()` declines `WIN:ZOOM` and `WIN:MIN` in
  hud mode) and the OS path walked past it.

  The un-maximise this replaced **did not work**: it called
  `SetWindowPlacement` with `showCmd = SW_HIDE`, which hides a window without
  restoring it, so `IsZoomed` stayed true and the comment claiming otherwise
  had never been measured.

  The state is not restored on the way out, and `g_app_prev_wp` is gone
  entirely. Re-maximising from a snapshot taken before the console entered the
  game is the same mistake as restoring its rect from one -- un-maximise inside
  the game and it came back full-screen anyway. Every bug in this file traces
  to some pre-entry snapshot; there is no longer one to consult.

`docs/overlay-plan.md` carries the measurements behind all of those.

## How to test it, given reasoning failed five times

The file plus the actual window rect. Write down the geometry, switch mode,
compare. The user's screenshots were ground truth and contradicted the reasoning
every time, so measure the window rather than argue from the code.

**Live-verified 2026-09-06**, by the user:

- app mode -> move the window -> switch app-as-HUD on -> the badge opens the
  console where they moved it;
- detached (so app mode, whatever the preference says) -> move the window ->
  attach -> the badge opens it where they moved it;
- snapped or maximised on the desktop -> attach -> the console arrives as an
  ordinary window at its PRE-maximise size, resizable, and keeps that size on
  the way back out;
- no snap offered at any screen edge while the console lives in the game.

One behaviour is accepted rather than understood: after a detach the window
centres at its remembered size. It is the owned-window path -- the disown plus
the hide/re-show lets Windows put the window back at its WINDOWPLACEMENT normal
position -- but that was never measured, and it needs an attach to reach, so a
fresh client is a fresh frame anyway.

**Still unmeasured**, and worth doing before trusting the rest of the rule: the
return leg (move it in the game, close to the desktop), a resize rather than a
move in each direction, the `kept` branch (close to the badge, move the Roblox
window, reopen -- the console should follow the game, not the screen point),
maximised in app mode across a switch, and a cold start with `rememberPos` on.
`ECLIPSE_ZLOG=1` prints `APP carry TAKEN|kept ...` at every open, which is the
one line that says which branch ran.
