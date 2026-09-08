# A transparent app window in hud mode

An IDEA, not built. Asked for 2026-09-05: in **hud mode** — the console living
inside the game — the app window should have a translucent background like a
docked snippet panel, instead of the opaque ground it paints today.

Written down rather than attempted because the hard part sits on top of the
machinery that took two agent runs and a day of measurement to stabilise, and
because the cheap half is worth trying first.

## Why the hub gets this for free and the app does not

The hub's panel is **composition-hosted**. Its window is the alpha-0 overlay, so
a pixel the page does not paint simply *is* the game — transparency is the
default state and `ui.css` only has to avoid painting a ground:

```
html, body { background: transparent }   /* and NO color-scheme: dark */
```

The app is a **real HWND with a window-hosted WebView2**, created by the webview
library (`eclipse_ui.cpp`, `g_hwnd = wr.value()`). There, an alpha-0 page shows
the **parent window's** background, not the desktop. The window is opaque, so
the page would be transparent onto black.

That is the whole difficulty, and it is why this is not a CSS change.

## Three pieces, in increasing order of risk

1. **Controller alpha.** `put_DefaultBackgroundColor({0,0,0,0})` on the app's
   `ICoreWebView2Controller2`. Proven in this tree — `overlay.cpp:3103` and
   `snipwin.cpp:385` already do exactly this for the composed hosts. Cheap and
   revertible.

2. **A translucent ground in the page.** `--bg-0` (#0a0a0c) and `--bg-1`
   (#0d0d10) in `tokens.css` are opaque, and the editor, sidebar and console
   body all paint one of them. Hud mode would want an alpha variant, applied by
   a body class so **app mode is untouched** — a see-through window on the
   desktop is not the ask and would be unreadable over arbitrary wallpaper.

3. **Per-pixel alpha on the window itself.** The actual work. Without it, 1 and
   2 give a page that is transparent onto the app's own black.

## What to use for (3), and what not to

- **`WS_EX_LAYERED` is the obvious answer and does not work.** It is documented
  as incompatible with WebView2's DirectComposition rendering, and this tree has
  already hit the neighbouring version of that: `overlay.cpp`'s note that
  "WebView2 renders through DirectComposition, which bypasses LWA_COLORKEY --
  you get an opaque magenta rectangle instead of transparency (verified)". Do
  not spend a day rediscovering it.
- **`DwmExtendFrameIntoClientArea` with -1 margins** (sheet-of-glass) is the
  classic route that coexists with DComp content.
- **`SetWindowCompositionAttribute` with acrylic / blur-behind** gives the
  frosted look, which is closer to what "like the hub" actually looks like --
  the hub is `#12131af2`, i.e. 95% opaque, not clear glass.

Note which of those two is wanted before building either: the hub is barely
translucent. "Transparent" here probably means *tinted*, not *see-through*.

## Why this is a Fable job rather than an afternoon

Changing the app window's composition attributes touches the exact machinery
that produced today's worst bugs. Anything attempted here must not regress, by
name:

- the cold-open fix — the paint witness (`APPPAINT` -> double rAF ->
  `APPPAINTED`) gating one promote before the travel box drops beneath the app;
- the `WM_WINDOWPOSCHANGING` dock veto;
- `client_topmost_guard` and the `after_top -> HWND_TOP` guard, which exist
  because the client was once left `WS_EX_TOPMOST` and Roblox sat above every
  window on the desktop until restarted;
- badge-above-app and dock-behind-app, which are deliberately asymmetric;
- docked-panel keyboard focus (`overlay_focus_enter`/`exit`).

`docs/overlay-plan.md` carries the measurements behind all of those, and its
step 3 records why the app is a real HWND while the badge, panel and docks stay
in the composition overlay. That split is deliberate; a change here must not
quietly undo it.

## The cheap experiment, first

Do 1 and 2 alone and look at the result. Two outcomes, both informative:

- the window composites with alpha anyway (possible — it is DWM-composed, and
  the frame was already customised), and the job is done for nothing;
- the ground goes translucent against the app's own black, which proves (3) is
  required and costs one revert.

That is a five-minute test that decides whether an agent is needed at all, and
it is the honest first step: everything above about (3) is reasoning from
documentation, not from a measurement on this window.
