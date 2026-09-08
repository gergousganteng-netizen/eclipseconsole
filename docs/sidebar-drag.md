# The sidebar drag, and the flicker that is left

**Status: BUILT, 2026-08-30.** The pointer drag described below is now the
sidebar's drag layer (`web/app.js`, `armDrag`/`updateDropTarget`; `web/app.css`,
`.drag-clone`/`.drag-src`). HTML5 DnD is gone entirely — no `draggable`, no
`drag*` listener, no `dataTransfer`. The banding walk gives every Y an owner
(row i owns from its top to row i+1's top), so the 1px dead zone cannot exist;
verified by driving the walk over synthetic seam coordinates and watching the
old hit-test model fail on the same input.

Of the two earlier fixes: the `lastSnippetsBody` dedupe STAYS (still wanted —
fewer rebuilds is still cheaper), and the `gap: 0` + transparent-border
workaround (dead end 6) is REVERTED, since the arithmetic makes the kit's real
1px gap safe — it is now the visible proof of the model. `pointer-events: none`
on row children (dead end 5) is KEPT: the row owns every handler.

Divergences from the vemacro reference: a two-axis threshold (Y-only would never
start a drag toward the crumbs), a capture-phase click suppressor instead of
`_wasDrag` (which eats the next genuine click when a release produces none),
`user-select: none` scoped to `.sidebar` rather than the body (the editor and
console must stay selectable), Escape-cancel, and marks that recompute on scroll
— so the wheel works mid-drag, which native DnD made impossible.

The dead-ends list below is kept as history of the HTML5 model.

Written 2026-08-28, at the end of the session that fixed the two real causes.

---

## The symptom (fixed)

Dragging a snippet in the sidebar: as the pointer crossed the **1px gap between
two rows**, the purple insertion rail blinked out and the cursor flashed
not-allowed. One pixel wide. Everything else about the drag worked.

## What was ALREADY fixed — do not re-fix these

Three causes were stacked on top of each other, which is why every single-fix
attempt looked like it did nothing. Two are fixed:

1. **The 2-second rebuild (the big one).** `src/ui/snippets/watch.cpp` polls with
   `POLL_MS = 2000` and calls `push_snippets()` unconditionally — a byte-identical
   `SNIPPETS:` snapshot every 2s. The page handler ended in `renderSnippets()`,
   which does `list.innerHTML = ""` and rebuilds every row, plus `renderTabs()`.
   **A drag holds a reference to a DOM NODE**; destroying it mid-drag loses the
   drop target until the next event lands on the replacement.

   The tell was not a drag symptom at all: **hover highlight dropping and
   returning on a 2s beat with no drag in progress**, and the same on the tabs.
   Fixed page-side in `web/app.js` (`lastSnippetsBody`): identical bodies return
   before touching anything. The host already does this for the in-game panel
   (`g_hud_last`, `overlay.cpp`); the console page simply had no equivalent.

2. **Child hit-targets.** A row's `.script-ico` and `.script-meta` are separate
   hit-test targets, so moving between them fires `dragleave` on the ROW and
   `dragenter` again without the pointer ever leaving it — traced as
   `dragleave <row>` immediately followed by a `dragover` on the same row with an
   unchanged rect. Fixed with `pointer-events: none` on those two (app.css). NOT
   applied to `.group-head`, whose `+` and run controls are genuinely clickable.

## The cause

`web/components.css`:

```css
.scripts { display: flex; flex-direction: column; gap: 1px; }
```

**A flex gap belongs to no child.** A drag crossing it hit-tests nothing, so no
row handler fires; `#snippetList`'s own handler runs instead and calls
`clearDropMarks()`, and the rail vanishes for exactly that pixel.

An interim workaround gave the gap back to the rows as a transparent border
(`.scripts { gap: 0 }` plus `border-bottom: 1px solid transparent`). It helped
and did not fully cure it — sub-pixel row heights at non-integer zoom still left
seams. **That workaround is now reverted**: the arithmetic below makes the kit's
real 1px gap safe, and leaving it visible is the proof.

### Why the HTML5 model could not really fix it

The drag cursor is **not settable**. During an HTML5 drag the browser owns it
entirely — no CSS `cursor` rule applies — and derives it from
`dataTransfer.dropEffect`, which is one of `copy`/`move`/`link`/`none` and is
**reset before every `dragover`**. `none` IS the block cursor. So "stop showing
the block" means "make sure every `dragover` is accepted", which fails when
**no `dragover` fires at all** — and in a seam between two elements, that is
exactly what happens. No event, nothing to accept, browser default.

That was the ceiling of the HTML5 approach, not a bug in it — and the reason the
fix below is a replacement rather than a patch.

---

## The fix: a custom pointer drag

`C:\Users\user\downloads\vemacro` — the user's own app, same house, same UI
idiom — does drag-and-drop **without HTML5 DnD at all** and has none of this.
`vemacro/web/index.html` is the reference implementation (see `refreshProfiles`,
`getProfileDropIndex`, `getMacroDropIndex`).

The whole trick is eight lines:

```js
function getProfileDropIndex(list, y) {
  const items = list.querySelectorAll('.profile-dd-item');
  for (let i = 0; i < items.length; i++) {
    const r = items[i].getBoundingClientRect();
    if (y < r.top + r.height / 2) return i;
  }
  return items.length;
}
```

**Every Y coordinate maps to an index**, by arithmetic rather than hit-testing.
A pixel in a gap is not a dead zone — it is a number that falls on one side of a
midpoint. Past the end returns "after the last". **The dead zone cannot exist in
this model.**

The rest of the shape, from the same file:

- `mousedown` records the start; a **4px threshold** before it counts as a drag,
  so a click is still a click.
- `mousemove` and `mouseup` are bound to **`document`**, so they fire regardless
  of what is underneath. No `dragleave`, no enter/leave bookkeeping, nothing to
  lose in a seam.
- The ghost is a **clone** (`.drag-clone`, `position: fixed`,
  `pointer-events: none`) following the cursor; the source gets `.dragging`
  (opacity 0.3).
- The rail is a real element per gap (`.profile-drop-ind`), toggled `.visible`.
  Nothing clears it behind your back.
- No `dataTransfer`, therefore no `dropEffect`, therefore **no block cursor** —
  and a CSS `cursor` can be set freely, because it is not a native drag.

### What the port touched

Not a patch — the sidebar's whole drag layer:

- row drag, and **multi-select drag** (`dragNames` / `orderedSelection`)
- group-header drag (`moveGroup`, whole-subtree moves)
- all four drop targets: group headers, breadcrumb segments, the list
  background, and the gap below the last row
- **spring-loaded group expansion** during a drag
- the auto-scroll (simpler here — it is already rAF-driven off a stored Y)
- the drag ghost and its count badge (`setDragCount`)

### What must not regress (checked at the port, and still binding)

- Multi-select drag carrying **all** selected files, and multi-file reorder via
  `moveSnippets` (which corrects indices for the items removed above the target).
- Reordering only within a group; cross-group drops being a MOVE.
- The gap below the last row as a drop target — move out of a group, or reorder
  to the end.
- Explorer's rule: dragging something outside the selection replaces it;
  pressing **inside** a multi-selection leaves it alone. (The condition here was
  once `!isSelected(s.n) || selected.length > 1`, which did the exact opposite of
  its own comment and silently reduced every multi-drag to one file.)
- Spring-loading, and `renderSnippets()` never running mid-drag.

---

## Dead ends, in the order they were bought

All six were attempts on the flicker before the 2s rebuild was found. Every one
is reverted. Listed because each looked correct at the time:

1. `preventDefault` on `dragover` at `.side-scroll` — no effect.
2. Same at `.sidebar`, to cover the footer and label boxes — no effect.
3. Same at `document`, capturing, so nothing could escape it — **no effect**.
   Three rounds spent on `dropEffect`; the cursor is downstream of the cause.
4. Deferring `renderSnippets()` during a drag and flushing on `dragend`. Closest
   to right, but it only covered drags — the hover symptom proved the rebuild
   happens with no drag at all — and it masks rather than fixes. It also breaks
   spring-loading, which depends on that render.
5. `pointer-events: none` on row children. **Real**, kept (see above), but not
   the 2s beat.
6. `.scripts { gap: 0 }` plus a transparent row border. **Real**, kept, but did
   not fully cure the seam.

The lesson worth keeping: **a symptom that persists with the pointer completely
still is not an input-event problem.** That one observation from the user
(“it flickers even when I don't move my cursor”, then “every like 2 seconds”)
identified the cause after five rounds of code reading had not.
