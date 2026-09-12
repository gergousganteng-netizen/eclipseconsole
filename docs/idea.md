# UI/UX Improvements

Two lists. **Added** is built and shipped; **Pending** is not. Move an entry up
when it lands, and say what actually got built if it differs from the note --
several of these were written before the shape was settled.

---

# Added

## Code Tab Toolbar  *(2026-09-11)*

An action bar under the tab strip. Read RIGHT TO LEFT -- Run is the rightmost
control in the window because it is pressed most, and the groups step away from
it in that order:

```
Save | [Roblox]  Launch | Arm  Stop  Run
```

- Run is the only filled button. Stop and Run stay separate: several snippets
  can run at once, so one control cannot express "run this" and "stop which?".
- Arm goes red when armed -- the same `--err` the bottom bar used.
- The left of the bar names the open file, with a dot when unsaved.
- Attach is the Roblox mark alone: no text, no outline, plain white.
- Launch has a rocket, and a spinner while launching. It stops when a client
  actually appears; nothing reports a launch back, so 15s is the cap.
- **Launch stays visible while attached** -- starting a second client is a real
  thing to want, and a vanishing button shifts everything to its left.

## Client Attachment Panel  *(2026-09-11)*

Slides in from the right edge, blurs the app behind it, starts below the
titlebar so the window controls stay reachable. Replaced the centred
multi-instance picker entirely.

- A row is headshot + display name + `@username` + game. pid and monitor on the
  row's tooltip.
- The attached row is outlined and carries **Detach**; any other row attaches on
  click.
- Footer has the client count and Launch Roblox. Refresh in the header.
- Esc, the close button, or a click outside closes it.
- Headshots come from the same batching avatar cache `fx_avatar_url` serves
  snippets from -- one request for every client, cached to disk. The initial
  stays underneath, so an account with no headshot keeps its letter.
- Asks the host for the list on every open. The old picker was only ever sent
  one at 2+ clients, so with one client the panel would have been blank.

## Bottom Bar  *(2026-09-11)*

36px, matching the toolbar, holding only pid / base / offsets / DataModel,
still pinned right. 52 was an empty shelf once the buttons left; 26 and 32 both
read as a crack rather than a strip.

The staged drop-a-field-at-each-breakpoint rules are gone -- they were sized
against a bar that also carried six buttons.

## Status Bar Redesign  *(2026-09-11)*

```
(Trash Icon)  9
_______________________________
* Attached . DisplayName
```

- Trash is icon + count, no label; the wording moved to its tooltip.
- A hairline separates the tool from the readout.
- Three states, not two: `Attached . Name`, `Not attached` (a client is running),
  `No client` (none is). Needed a new host signal -- `client_running()` in
  `ui/client/attach.cpp`.
- The name is `s.user`, the same field the in-game panel header reads, so the
  two always agree.
- The dot is grey and still while detached. It was tagged `.on` when attached
  and nothing styled it, so the kit's green pulse ran permanently.

## Sidebar gap and header  *(2026-09-11)*

- The 28px of dead space above the header is gone (the kit's `.side-label`
  top margin stacking on `.side-scroll`'s padding).
- "Snippets" -> **Explorer**, bigger, brighter, and sentence case rather than
  the kit's uppercase.
- The crumb is just `Snippets`; the `%APPDATA%` path moved to its tooltip.

---

# Pending

## Side Panel Navigation
Add a secondary navigation panel beside the main sidebar (preview mockup needed):
- **Main Tab** (top) — Console/Code interface with icon
- **Settings Tab** (bottom) — Application settings with gear icon
- Keeps primary controls accessible while reducing sidebar clutter
- [Design & implementation pending]

## Bracket Pair Guidelines
Visual dotted lines between matching brackets in the editor for better code structure navigation.

```cpp
struct Client {
|   std::string name;
|   int64_t     userid   = 0;
|   uint64_t    model    = 0; 
|   uint64_t    humanoid = 0;
};
```

## Editor Inline Diagnostics
Display compilation errors and syntax issues directly in the editor:
- **Error/Warning indicators in gutter** — Red icon for errors, yellow icon for warnings, displayed beside line numbers
- **Real-time feedback** as code is typed, preventing failed compilation attempts

## Quick Access Menu
Dedicated pinned menu tab welcoming user and with shortcuts:
(preview mockup needed)
- **Launch Roblox** — Rocket icon with loading state indicator
  *(built, but in the toolbar — see Added. Share the same button here.)*
- New snippet
- Settings button
- Other actions (TBD)

## Custom Appearance
New settings tab for visual customization:
- Custom background/wallpaper
- Window transparency
- Preview option

## Settings Redesign
Replace current settings with tabbed interface:
(preview mockup needed)
- **General** - App behavior
- **Editor** - Editor preferences
- **Console** - Output & logging
- **Client** - In Client Options such App as Hud and etc
- **Appearance** - Theme & visuals

## Another Change To Bottom Bar
Height Shorter than the original while keeping the text size the original

# How To Make Preview Mockup
Copy the current ui html and other files. make it functionable so it faithful to the original ui, make the changes for preview. user may ask to make some changes, and approve or deny it
