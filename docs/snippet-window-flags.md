# Idea, not built: window flags for `fx_window_*`

Written 2026-08-22, after app mode shipped as a real window over the client.
Nothing here is implemented. It is recorded so the reasoning does not have to
be re-derived, and so the expensive version does not get built by accident.

## The question

App mode gives the CONSOLE a real window over the game: focus, IME, clipboard,
1:1 drag, no taskbar button, a badge it grows out of, and a close gesture. Should
a SNIPPET be able to ask for the same thing?

## What snippets already have

- `fx_window_dock(1, corner, dx, dy)` — the snippet's page becomes a panel
  inside the overlay. Takes clicks and the wheel, never focus, never keystrokes.
  This is the right shape for anything glanceable.
- `fx_window_dock(0, …)` — a plain floating window. Real window, real focus,
  ordinary desktop behaviour.

So the gap is narrow: a window that is over the CLIENT, typable, and behaves
like part of the game session rather than like another app on the desktop.

## The cheap subset, if it is ever wanted

Flags on the existing `fx_window_*`, not a new subsystem:

- **`WS_EX_TOOLWINDOW`** — off the taskbar and out of Alt-Tab. One style bit.
  Note the cost app mode already accepted: Alt-Tab then cannot reach the window
  either, so something else has to be able to summon it.
- **Band rule** — topmost while the client is foreground, ordinary window
  otherwise. The rule already exists in `hud_tick` for the app window; a
  snippet window would have to go through THAT owner, not grow a second copy.
  One owner for the band is the whole reason today's z-order is stable.
  **Cheaper since 2026-08-22: OWNERSHIP.** See below.
- **Client-relative position (and size) memory** — the same fact
  `overlay_app.txt` holds for the app, keyed per snippet.

That is a few lines against machinery that exists, and it adds no new
participant to the promote/demote logic.

## Ownership changes the band bullet (noted 2026-08-22, still not built)

Same day this file was written, the app window and the overlay were given the
CLIENT as their window owner (`SetWindowLongPtr(GWLP_HWNDPARENT)`). USER32 then
keeps them above the client as part of the activation itself, rather than us
reacting to a foreground event 0.5-1.4ms late. It removed a flash that a day of
reaction-speed work could not. See `overlay-plan.md`, "The tab-in blink,
actually removed".

**This weakens the main objection in this file.** A floating snippet window
owned by the client is "above the game, below other apps" **structurally** —
no tick, no promote, no new participant in the promote/demote logic at all,
which is exactly what the band bullet above was worried about. It is less code
than routing the window through `hud_tick`'s band owner, not more.

Note the split that already exists, because half the question is answered:

- **`fx_window_dock(1, …)`** — a docked panel is an element of the overlay's
  own page and has no HWND of its own, so it ALREADY inherits the owner, the
  unowned-move bracket and the promote glue. Nothing to add, nothing to
  decide. This is the merge-into-one-window decision paying off again.
- **`fx_window_dock(0, …)`** — a floating window is a real top-level window
  and gets none of it. That is the only case this idea is about.

**Two constraints any implementation inherits**, both paid for in blood
already and both documented in `overlay-plan.md`:

- **An owned window must never EXIT the topmost band while owned.** That is
  what hoists the whole owner group — the client included — over whatever app
  the user just clicked. Our overlay solves it by bracketing every non-TOPMOST
  move so it happens unowned. A snippet window that changes bands would need
  the same bracket, or the same "owner only while demoted" rule.
- **Ownership attaches the input queue to the client's**, so while the game
  hides the cursor for mouse-lock, the cursor is hidden over the owned window
  too. You cannot click what you cannot point at. The overlay solves this by
  dropping the owner whenever the client is foreground.

Neither is hard, but both are invisible until a user hits them, and both were
found the expensive way. If this is ever built, the flag should route through
the SAME ownership discipline the app and overlay use rather than calling
`SetWindowLongPtr` from snippet-facing code — one owner for the rule, which is
the same argument the band bullet makes.

Still an idea. The trigger to build it is unchanged: a real snippet that needs
to be typed into over the game.

## The expensive version, and why it is not the default answer

Handing a snippet the full app-mode experience means the badge → travel →
real-window handoff: the two-legged choreography, its watchdogs, click-through
during the travel, the close-gesture classification, and the empty travel box.
Generalising that to N windows is real work; copying it is the duplication that
has produced most of this tree's bugs.

The deeper objection is z-order. **Every window in the topmost band is another
participant in the promote/demote logic, and that is exactly where this
project's bugs live.** The dock was merged into the panel's window because "one
window is always raised first" was the only thing that removed the blink
outright — see `overlay-plan.md`. An API that hands out band members re-opens
that, once per snippet.

## When to revisit

When a real snippet needs to be TYPED into over the game — a config panel, a
search box, a parameter editor. Then implement the flag it actually needs and
nothing else, and route it through the existing band owner.

Until then this stays an idea: `CLAUDE.md` — add machinery only after the
simple version is measured and found wanting.

## What has actually been built since (2026-09-02)

One flag, on the narrow rule above: implement the one a real snippet needs and
route it through the existing band owner. `fx_window_passthrough(1)` makes a
DOCKED panel decorative -- drawn, but clicks land on the game. No new window,
no new band member, so neither objection above applies.

The mechanism is worth knowing before adding a second one, because the obvious
reading of the code is wrong:

- **The overlay's region IS its click path.** `hud_set_rects` builds the window
  region from the page's reported rects, and a rect in a region is a place the
  window EXISTS -- so it takes the click. The `~` SOFT prefix does not change
  that on its own.
- The soft/solid split fed only two things: the cursor top-up, and the
  pointer-following click-through, which was gated to app travels. A soft
  docked panel therefore drew correctly, stopped owning the cursor, and still
  ate every click. That is what shipped first and it looked plausible.
- The fix was to widen the gate rather than build anything: the pointer rule
  now runs whenever ANY soft rect is present (`ct_following`). That widens
  nothing else -- with no soft rect the region is exactly the solid set, so
  region hit-testing already returns what the pointer rule would.

So: **a new "decorative" surface needs a soft rect AND the pointer rule, not
one of them.** Verify it by clicking the game through the panel, never by
reading back the reported rects or the extended style.

### CSS pointer-events is not a substitute, and looks like one

A snippet author's first instinct is `pointer-events: none` on the page, and it
does nothing for click-through. It chooses which ELEMENT inside the page
receives an event; the overlay window has already taken the click by then, on
the strength of its region. `ultwatch` shipped with `pointer-events: none` on
its toast stack from the first version and was not click-through until
`fx_window_passthrough` existed -- the two are unrelated mechanisms and the
page-side one cannot reach past our own window. Said plainly in `fx.h` for the
same reason it is said here.

### The gap this leaves

Passthrough is WHOLE-PANEL. A panel that is part readout and part control
cannot exist today, and no page-side trick makes one -- see above. Doing it
properly means the docked page reporting its own sub-rects up through the
iframe, so the overlay marks the decorative ones soft and the controls solid.
That is a protocol addition (the frame shim, the SNIPMSG relay, per-frame
coordinate translation), not a flag. Wait for a snippet that actually needs it,
per the rule at the top of this file.
