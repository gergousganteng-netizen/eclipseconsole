# UI/UX Improvements

## New Side Tab Beside Sidebar
Settings and the main tab
Settings at the bottom that will have settings icon
Main tab will be Console icon or Code icon >_
Main tab at the top and settings at the bottom
[For preview later]

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

## In Editor Syntax Helper
Shows syntax error or error on the line beside the code if the code is not compilable

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

- # Better Attached Bottom Bar Status
- On Detached state: Not Attached
- On Attached state: Attached • Display Name(Reuse the panel code)
- The Trash Icon on top of this to be bigger with no text and only with the numbers along with seamless bar
- Current

_______________________________
(Trash Icon) Recover Deleted  9
Attached • Pid 1234

- After

(Trash Icon)  9
_______________________________
Attached • DisplayName
