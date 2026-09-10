# UI/UX Improvements

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
Dedicated pinned menu tab with shortcuts:
(preview mockup needed)
- Launch Roblox (With Rocket icon and Loading icon)
- New snippet
- Other actions (TBD)

## Custom Appearance
New settings tab for visual customization:
- Custom background/wallpaper
- Window transparency
- Preview option

## Settings Redesign
Replace current settings with tabbed interface:
- **General** - App behavior
- **Editor** - Editor preferences
- **Console** - Output & logging
- **Client** - In Client Options such App as Hud and etc
- **Appearance** - Theme & visuals
  
## Bottom Bar Optimization
Reduce clutter and improve space efficiency:
- Smaller bottom bar height
- Reduce text and button size
- Fixed button size
- Remove "F5" icon and add Run icon on left side of "Run"

## Status Bar Redesign
Unified bottom status bar with integrated trash counter and attachment status:
- **Trash Counter** (top-left) — Large icon with numeric badge (e.g., "9"), no text label
- **Separator line** divides trash counter from status bar below
- **Attachment Status** (bottom-right) — "Attached • [Display Name]" or "No Client/Not Attached" Not attached only if there a client but not attached
  - Display name reuses existing panel HUD code for consistency
- Removed "Launch roblox) from Status bar
- All elements in one seamless layout, reducing visual clutter

**Current Layout:**
```
_______________________________
(Trash Icon) Recover Deleted  9
● Attached • Pid 1234
```

**New Layout:**
```
(Trash Icon)  9
_______________________________
● Attached • DisplayName
```

```
(Trash Icon)  9
_______________________________
● No Client/Not Attached
```

## Side bar weird gap and improvement
- Fix the gap between top bar and "Snippet" text
- Better "snippet" text accent choice
