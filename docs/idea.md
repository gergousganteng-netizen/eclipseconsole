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
- Red underlines or icons next to problematic lines
- Hover tooltips showing specific error messages
- Real-time feedback as code is typed, preventing failed compilation attempts

## Quick Access Menu
Dedicated pinned menu tab with shortcuts:
- Launch Roblox
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

## Status Bar Redesign
Unified bottom status bar with integrated trash counter and attachment status:
- **Trash Counter** (top-left) — Large icon with numeric badge (e.g., "9"), no text label
- **Separator line** divides trash counter from status bar below
- **Attachment Status** (bottom-right) — "Attached • [Display Name]" or "Not Attached"
  - Display name reuses existing panel HUD code for consistency
- All elements in one seamless layout, reducing visual clutter

**Current Layout:**
```
_______________________________
(Trash Icon) Recover Deleted  9
Attached • Pid 1234
```

**New Layout:**
```
(Trash Icon)  9
_______________________________
Attached • DisplayName
```
