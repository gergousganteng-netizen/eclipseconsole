// fx.h — the surface a pasted Eclipse console snippet sees.  (Windows port)
//
// Ported from the Linux fx.h. PURE external data access: reads and writes of
// DATA in another process — no Luau VM, no injection, no control-flow writes.
// fx_wrblk honours the arm flag: disarmed it LOGS the intended write and does
// nothing; armed it writes. That gate is the whole safety model.
//
// Symbols (the one real difference from Linux): ELF was -rdynamic, so a
// dlopen'd .so resolved fx_* straight back into the host. Windows cannot bind
// a DLL to arbitrary EXE symbols, so the host EXPORTS fx_* and emits an import
// library (libeclipse_console.a) each snippet links against. A snippet's
// source is unchanged, only the link line differs.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>   // memcpy etc — snippets use these freely
#include <string>
#include <cstdio>

#if defined(FX_HOST)
#  define FX_API __declspec(dllexport)
#else
#  define FX_API __declspec(dllimport)
#endif

struct float3 { float x, y, z; };

// Mouse virtual-key codes, for the hotkey calls further down.
//
// These names already reach a snippet today, but only by accident: something in
// the headers above drags in the VK_* defines while leaving the rest of Win32
// out, so VK_RBUTTON compiles and HWND does not. Measured, not assumed -- and
// exactly the kind of thing a toolchain bump removes without warning. Pinned
// here so the hotkey documentation below is true on purpose.
//
// Guarded, so this defers to <windows.h> whenever a snippet includes it.
// Keyboard keys have no such block: 'E' and 'W' are their own VK codes, and
// the rest are one hex constant in the docs.
#ifndef VK_LBUTTON
#  define VK_LBUTTON  0x01
#  define VK_RBUTTON  0x02
#  define VK_MBUTTON  0x04
#  define VK_XBUTTON1 0x05
#  define VK_XBUTTON2 0x06
#endif

extern "C" {
    FX_API uint64_t    fx_rd(uint64_t addr);                              // read 8 bytes (0 on fail)
    FX_API size_t      fx_rdblk(uint64_t addr, void* buf, size_t n);      // read n bytes
    FX_API size_t      fx_wrblk(uint64_t addr, const void* buf, size_t n);// write n bytes — honours arm
    FX_API uint64_t    fx_off(const char* key);                           // offset from conf, -1 if missing

    // Walk the whole conf, for tools that CHECK it rather than use it. The
    // order is stable (sorted), so an index means the same thing twice.
    //
    //   for (int i = 0; i < fx_off_count(); i++) {
    //       const char* k = fx_off_key(i);
    //       uint64_t    o = fx_off(k);
    //   }
    FX_API int         fx_off_count(void);
    FX_API const char* fx_off_key(int i);

    // ---- BORROWED STRINGS: copy before the next call ----
    // fx_class and fx_name each return a pointer into ONE per-thread buffer
    // that the next call to the same function overwrites. Fine for
    //
    //     if (strcmp(fx_class(x), "Camera") == 0)         // used immediately
    //     std::string cls = fx_class(x);                  // copied
    //
    // and silently wrong for
    //
    //     const char* cls = fx_class(inst);
    //     ... fx_class(other) ...                         // <- cls now says
    //     if (strcmp(cls, "Player") == 0)                 //    "other"
    //
    // That is not hypothetical: it cost an afternoon in dex, where a detail
    // pane resolved a pointer field mid-loop and every later comparison ran
    // against the TARGET's class. Nothing errors -- the field list just comes
    // back empty, and it reads as the conf knowing nothing about the class.
    //
    // Per-thread, so two threads do not fight over it; the hazard is entirely
    // within one thread holding a pointer across a second call.
    FX_API const char* fx_class(uint64_t obj);                            // RBX class name, "" if unknown
    FX_API uint64_t    fx_dm();                                           // cached DataModel pointer
    FX_API int         fx_pid();                                          // live client pid

    // The PlaceId, or 0 when no place is loaded. THIS is how you ask "am I in a
    // game" -- fx_dm() is not, because the client holds a DataModel at the menu
    // too and simply replaces it on join and on leave.
    //
    //   if (!fx_place()) { /* menu or loading -- do nothing yet */ }
    //
    // Deliberately the id rather than a bool: "in game" means different things
    // (place loaded / character spawned / world streamed in), so the caller
    // picks -- pair it with fx_character() for "spawned" or fx_geom() for "the
    // map is here". The value also lets a snippet behave per place.
    //
    // Corroborating signal when this reads 0 and you want to know why:
    // fx_name(fx_dm()) is "LuaApp" in the menu and "Ugc" in a place. NOT what
    // to key off -- those are Roblox's names for its own DataModels and have
    // changed before, while the PlaceId is the id the site itself uses.
    FX_API uint64_t    fx_place();
    FX_API void        fx_log(const char* msg);
    // fx_log / fx_logf are the ATTRIBUTED log path: when several snippets run
    // at once each line is tagged with its snippet's name, because these run on
    // your run's own thread. Captured printf is NOT -- the host captures it by
    // rebinding fd 1, which is per-process, so once several runs print there is
    // no way to say which line came from which, and multi-line printf output
    // from two snippets can interleave. Use fx_log / fx_logf when it matters.
    //
    // The same line, formatted. printf works too -- the host captures it --
    // but only while the snippet and the host share a C runtime, so this is
    // the one that always reports. Truncates at 1KB.
    //
    //   fx_logf("fps %.1f -> %.1f", before, after);
    //
    // fx_log stays non-variadic on purpose: it is handed runtime strings, and
    // a '%' in one of those must never be read as a format.
#if defined(__GNUC__)
    FX_API void        fx_logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#else
    FX_API void        fx_logf(const char* fmt, ...);
#endif

    // ---- overlay -----------------------------------------------------------
    // A transparent full-screen renderer that sits on top of the game. The
    // snippet is the controller: turn overlay mode on, post target lists as
    // JSON at whatever rate you want (10 Hz is plenty), turn it off at exit.
    // The GUI console renders. The CLI binary has no window and these are
    // no-ops there -- a snippet using overlay is still safe to --run.
    //
    //   fx_overlay_post takes a list of shapes:
    //     [{"n":"name","sx":100,"sy":200,"w":40,"h":60,"d":47.5}, ...]
    //   where sx/sy is the TOP-LEFT of the box in monitor pixels, w/h its
    //   size, and d the world distance drawn under it.
    //
    //   An entry with "r" is a CIRCLE of that pixel radius instead, and then
    //   sx/sy mean its CENTRE -- the one place in this format where they are
    //   not a corner. A circle carries no name or distance:
    //     {"sx":960,"sy":540,"r":150}
    //   Used for an aim FOV ring. Mixing both kinds in one list is fine.
    //
    //   "c" is an 0xRRGGBB outline+name colour, default white:
    //     {"n":"Bob","sx":100,"sy":200,"w":40,"h":60,"d":12,"c":7269288}
    //
    //   Fields that are not recognised are ignored, so extend freely.
    FX_API void fx_overlay_on(void);
    FX_API void fx_overlay_off(void);
    FX_API void fx_overlay_post(const char* json);

    // ---- network -----------------------------------------------------------
    // One HTTPS GET, host-side. A snippet's PAGE cannot do this itself: its
    // origin is CORS-blocked at Roblox's endpoints (they send no
    // Access-Control-Allow-Origin) and the failure is silent. WinHTTP has no
    // CORS layer, so the request happens here and the page is handed the
    // result. An <img src> straight to a CDN is fine from the page -- images
    // were never CORS-gated; it is the JSON lookups that must come through
    // here.
    //
    //   fx_http_get        https GET of https://host/path into buf.
    //
    //                        char j[8192];
    //                        int len = fx_http_get("games.roblox.com",
    //                                              "/v1/games?universeIds=1",
    //                                              j, sizeof j);
    //
    //                      Returns the reply's FULL length, so len >= sizeof j
    //                      means it was truncated and you need a bigger buffer
    //                      -- not that you may parse what arrived. 0 is a
    //                      failed request. https only, no headers, no POST: if
    //                      you need those, say so rather than working around
    //                      it here.
    //
    //                      IT BLOCKS, for as long as the round trip takes (up
    //                      to a 10s cap). That is on YOUR run's thread, so it
    //                      stalls your snippet and nothing else -- but never
    //                      put it in a hot loop. Fetch once, keep the result.
    FX_API int         fx_http_get(const char* host, const char* path,
                                   char* buf, size_t n);

    //   fx_avatar_url      a player's headshot URL, for an <img src> in your
    //                      window's page. Takes fx_player_info.userid.
    //
    //                        char u[256];
    //                        if (fx_avatar_url(p[i].userid, u, sizeof u)) ...
    //
    //                      NEVER BLOCKS -- it is built for the inside of a poll
    //                      loop. The FIRST call for an id returns 0 and queues
    //                      it; a later call returns the URL. So ask every pass
    //                      and use it when it turns up, rather than waiting:
    //
    //                        if (!x.avatar_sent && fx_avatar_url(id, u, sizeof u)) {
    //                            show(u); x.avatar_sent = true;
    //                        }
    //
    //                      Queued ids are fetched in ONE batched request, and
    //                      cached to disk between runs, so asking for a whole
    //                      roster every pass costs one request and then none.
    //                      0 forever means Roblox has no headshot for that id.
    FX_API int         fx_avatar_url(int64_t userid, char* buf, size_t n);

    // Corners for fx_window_dock, named so a call reads as a place rather than a
// number.
#define FX_DOCK_TL 0
#define FX_DOCK_TR 1
#define FX_DOCK_BL 2
#define FX_DOCK_BR 3

// ---- snippet window ----------------------------------------------------
    // A WebView2-backed floating window a snippet can open and drive with HTML.
    // Draggable, resizable, has a taskbar entry. The whole page is yours -- CSS,
    // JS, DOM, whatever. Good for status HUDs, small tools, control panels.
    //
    //   fx_window_open     create the window (blocking; ~200 ms while WebView2
    //                      spins up). Only ONE at a time per snippet; calling
    //                      open again resizes/re-titles the existing one.
    //   fx_window_html     replace the whole page. Anything left over from the
    //                      previous page (JS state, timers, listeners) is gone.
    //   fx_window_eval     run JS in the page. Use for partial updates so a
    //                      1 Hz refresh doesn't wipe the DOM every tick:
    //                        fx_window_eval("document.getElementById('x').textContent='42'");
    //   fx_window_text     convenience -- escapes and puts <pre>text</pre> as
    //                      the page. Backward-compatible with the pre-HTML API.
    //   fx_window_close    destroy the window.
    //   fx_window_get_msg  pop a page-to-snippet message. NULL if none. The
    //                      page sends by calling window.eclipse_send("payload").
    //                      Format is up to you; the console passes the string
    //                      through unchanged. Poll from your loop:
    //                        while (const char* m = fx_window_get_msg()) { ... }
    //                      The returned pointer is valid until the next call
    //                      from the same thread.
    //
    // Auto-closed if the snippet stops without calling fx_window_close. GUI
    // console only -- CLI leaves these as no-ops.
    //   fx_window_navigate  point the window at a FILE instead of a string.
    //                      fx_window_html uses NavigateToString, which gives
    //                      the page an opaque origin -- no relative URLs, no
    //                      local files, no fetch, so everything must be inline.
    //                      A file:// page is an ordinary page: <link>, <img>,
    //                      <script src> and fetching its neighbours all work.
    //
    //                        std::string dir = std::string(fx_snippets_dir())
    //                                        + "\\mytool\\";
    //                        fx_window_navigate((dir + "ui.html").c_str());
    //
    //                      A bare path becomes file://; anything with a scheme
    //                      is passed through, so https:// works.
    //
    //                      .html/.css/.js in a project are listed and EXPORTED
    //                      like source, so a UI built this way travels with a
    //                      bundle. Nothing compiles them -- a project builds
    //                      *.cpp only.
    FX_API void        fx_window_open(const char* title, int w, int h);
    FX_API void        fx_window_html(const char* html);
    FX_API void        fx_window_navigate(const char* path_or_url);
    // %APPDATA%\EclipseConsole\snippets -- where a snippet's own files are.
    FX_API const char* fx_snippets_dir(void);

    // ---- fx_cfg: settings that survive a rebuild ----------------------------
    //
    // A dial you tuned is a constant in your source today, so trying a new
    // value means an edit and a rebuild, and the old value is gone the moment
    // you type over it. These keep it instead:
    //
    //     double smooth = fx_cfg_num("aimbot.smoothing", 0.25);   // default
    //     ...
    //     fx_cfg_set_num("aimbot.smoothing", smooth);             // remember
    //
    // The store is %APPDATA%\EclipseConsole\settings.conf -- plain
    // `key = value` text, next to the snippets folder and never inside it.
    // Editing it by hand is a supported way to use it; that is why it is not a
    // binary blob.
    //
    // ONE FLAT NAMESPACE for every snippet, so PREFIX YOUR KEYS. "smoothing" is
    // a collision waiting to happen between two of your own snippets;
    // "aimbot.smoothing" is not. Automatic per-snippet namespacing was
    // considered and rejected -- a run's identity is its label, a project has
    // several files under one, and the CLI has no label at all, so it would
    // have been silently wrong in three places.
    //
    // fx_cfg_num returns the FALLBACK, not 0.0, when the key is missing OR
    // holds something that is not a number. A key edited into nonsense reading
    // back as 0.0 would be indistinguishable from a dial deliberately zeroed.
    //
    // Writes are coalesced: a set is visible to the next read immediately, and
    // reaches the file at most once a second (plus once when the run ends), so
    // storing a value every tick is safe.
    //
    // fx_cfg_str writes at most max-1 bytes plus a NUL and returns the length
    // written. A value cannot contain a newline -- newlines are replaced with
    // spaces on the way in, since the format is one pair per line.
    FX_API double fx_cfg_num(const char* key, double fallback);
    FX_API void   fx_cfg_set_num(const char* key, double value);
    FX_API int    fx_cfg_str(const char* key, char* out, int max, const char* fallback);
    FX_API void   fx_cfg_set_str(const char* key, const char* value);
    FX_API int    fx_cfg_has(const char* key);
    FX_API void        fx_window_eval(const char* js);
    FX_API void        fx_window_text(const char* text);
    FX_API void        fx_window_close(void);
    FX_API const char* fx_window_get_msg(void);

    //   fx_window_dock     turn that window into an IN-GAME panel: glued to the
    //                      attached client, always over it, no caption, no
    //                      taskbar entry, and it never takes focus -- so the
    //                      game keeps receiving your keys while it is up.
    //
    //                        fx_window_dock(1, FX_DOCK_TR, 12, 12);
    //                        fx_window_dock(0, 0, 0, 0);   // back to floating
    //
    //                      corner is which corner of the CLIENT to hang off,
    //                      dx/dy the inset from it, so the panel keeps its
    //                      place when the game window moves or is resized.
    //                      Size stays whatever fx_window_open asked for.
    //                      One snippet may be docked at a time - a later
    //                      docker keeps a floating window (logged).
    //
    //                      What a docked window can and cannot receive is not a
    //                      choice we made:
    //                        clicks   yes -- delivered by position
    //                        wheel    yes, unless the user turned off Windows'
    //                                 "scroll inactive windows on hover"
    //                        keys     only while a TEXT FIELD in your page has
    //                                 focus. Clicking into an <input>,
    //                                 <textarea> or contenteditable takes the
    //                                 keyboard from the game; it goes back the
    //                                 moment focus leaves the field -- Enter
    //                                 with a blur(), a click on anything else
    //                                 in the panel, Esc, or a click on the
    //                                 game. Nothing else in the page ever
    //                                 gets a key: there is no keydown on a
    //                                 button or a canvas, and focus() from
    //                                 script does not take the keyboard --
    //                                 only the user's click does.
    //
    //                      The page is yours, so it can look like anything --
    //                      including the game's own interface. It is still your
    //                      window sitting on top of the game: nothing is drawn
    //                      inside Roblox and the client cannot see it.
    FX_API void        fx_window_dock(int mode, int corner, int dx, int dy);

    //   fx_window_passthrough
    //                      make a DOCKED panel decorative: it still draws, but
    //                      clicks land on the game underneath instead of on it.
    //                      For a window that only reports -- a toast stack, an
    //                      ESP legend, a readout -- where a panel-sized hole in
    //                      the game's input would be a bug, not a feature.
    //
    //                        fx_window_passthrough(1);
    //
    //                      Set it alongside fx_window_dock; it is remembered
    //                      across a re-dock like the rest of the window state.
    //                      No effect while floating -- a floating window is a
    //                      real HWND and owns its clicks.
    //
    //                      NOTHING in the page can receive the mouse while this
    //                      is on: not a button, not a hover, not a scroll. It
    //                      is the WHOLE panel or none of it -- a docked panel
    //                      cannot be part clickable and part transparent.
    //
    //                      CSS pointer-events DOES NOT DO THIS, however much it
    //                      looks like it should. It only chooses which element
    //                      inside your page gets the event; the click has
    //                      already been taken by the console's overlay window
    //                      before your page is consulted, so the game never
    //                      sees it either way. A panel with pointer-events:none
    //                      on everything is still a panel-sized hole in the
    //                      game's input. This flag is the only thing that is
    //                      not.
    FX_API void        fx_window_passthrough(int on);

    // Windows additions. The Linux build had no equivalent because it never
    // needed a module base (ASLR was off and addresses were absolute); here
    // every static offset is an RVA that must be added to the base.
    FX_API uint64_t    fx_base(void);                                     // RobloxPlayerBeta.exe base
    // BORROWED, exactly like fx_class -- one per-thread buffer, overwritten by
    // the next fx_name call. Copy it if it has to outlive the next one. See the
    // note above fx_class for the failure this causes.
    FX_API const char* fx_name(uint64_t inst);                            // Instance.Name, "" if unreadable
    FX_API size_t      fx_str(uint64_t addr, char* buf, size_t n);        // read a std::string at addr

    // ---- long-running snippets ----
    // A snippet that loops must poll fx_running(), or it can never be stopped:
    // the host holds its DLL loaded for as long as fx_run() has not returned,
    // and Stop works by asking the snippet to leave rather than by killing the
    // thread underneath it (which would unload code mid-call).
    //
    //   while (fx_running()) { ...; fx_sleep(50); }
    //
    FX_API int fx_running(void);            // 0 once Stop is pressed, or on exit
    FX_API int fx_sleep(unsigned ms);       // interruptible sleep; 0 if stopping
    FX_API int fx_key(int vk);              // is a virtual-key down right now?

    // Press and release a key, as the OS sees it -- the one call here that
    // PRODUCES input rather than reading it. hold_ms is the gap between the
    // press and the release; it BLOCKS this run's thread, so pass 0 in a spin
    // loop. Capped at 500ms.
    //
    // Refuses (returns 0) unless the attached client is the foreground window,
    // because a synthetic keystroke goes wherever focus is -- ungated, a loop
    // sending keys types into whatever you alt-tabbed to. Add !fx_typing(), or
    // just use fx_input_ok(), when the chat box should stop it too.
    //
    // Our own sends are tagged, so fx_key and fx_key_seq do NOT report them:
    // a snippet cannot trip its own hotkey by pressing it.
    FX_API int fx_send_key(int vk, int hold_ms);

    // Several snippets can run at once. Each runs on its own thread, and
    // fx_running / fx_sleep / fx_pace / fx_wait_key resolve to THIS run
    // automatically -- you never pass a run id.
    //
    // The exception is a thread YOUR snippet starts. The host never saw that
    // spawn, so fx_running() on it falls back to "stop only when everything
    // stops" -- it will NOT return 0 on your snippet's own Stop. If a worker of
    // yours must stop when your snippet does (so a join at the end of fx_run
    // does not hang), adopt the run on it:
    //
    //     unsigned rid = fx_run_id();                 // on the run thread
    //     std::thread w([rid]{ fx_adopt_run(rid); ... work ... });
    //
    // fx_run_id() is 0 when called outside any run.
    FX_API unsigned fx_run_id(void);
    FX_API void     fx_adopt_run(unsigned run_id);

    // Hold a RATE. fx_sleep(16) does not give 60 Hz: the body's own runtime
    // adds to every period, so a loop that spends 5ms reading and then sleeps
    // 16 runs at 47 Hz. fx_pace waits until the next period boundary instead,
    // so the work comes out of the wait.
    //
    //     while (fx_running()) { collect(); if (!fx_pace(60)) break; }
    //
    // Returns 0 when stopping, same as fx_sleep, so it drops into the tail of
    // a loop in place of it. A body that overruns its period does not bank the
    // lost time: the rate simply degrades to what the work allows, rather than
    // firing several iterations back to back to catch up.
    //
    // Per-thread, so two threads can each hold their own rate.
    FX_API int fx_pace(unsigned hz);

    // ---- hotkeys ----
    // Backed by low-level keyboard AND mouse hooks the HOST owns: a snippet
    // never installs one or hands over a callback, so nothing can be called
    // after its DLL unloads.
    //
    // MOUSE BUTTONS COUNT AS KEYS here. VK_LBUTTON, VK_RBUTTON, VK_MBUTTON,
    // VK_XBUTTON1 and VK_XBUTTON2 go through the same counters as any letter,
    // so fx_key_pressed(VK_MBUTTON) and fx_claim_key(VK_XBUTTON1, 1) work
    // exactly the way the keyboard ones do. There is no separate mouse API.
    //
    //   fx_key_seq    a monotonic count of this key's down-edges. A counter,
    //                 not a flag: a tap landing entirely between two polls still
    //                 increments it, unlike GetAsyncKeyState. Read it once at
    //                 start for your baseline and act on the DIFFERENCE -- the
    //                 counter is shared and never resets (several snippets can
    //                 watch one key at once), so it is not zero when you begin.
    //                 fx_key_pressed does exactly this for you.
    //   fx_wait_key   park until the next key event / Stop / timeout; the wake
    //                 is the keystroke, not a sleep-loop turn, so no latency
    //                 and no CPU while idle. ms == 0 waits indefinitely.
    //                 Returns 0 when stopping, the same convention as
    //                 fx_sleep, so it substitutes for it.
    //   fx_claim_key  swallow the key so the game never receives it. Released
    //                 automatically when the snippet ends.
    //                 VK_LBUTTON is REFUSED: swallowing left click globally
    //                 would eat the click on Stop as well, leaving no way to
    //                 end the snippet from the UI.
    FX_API unsigned fx_key_seq(int vk);
    FX_API int      fx_wait_key(unsigned ms);
    FX_API void     fx_claim_key(int vk, int on);

    // ---- input gating ------------------------------------------------------
    // The keyboard hook is global: it does not care which window is in front,
    // and it cannot tell a keystroke meant for the game from one meant for a
    // chat box. Both of these are things a hotkey wants to know and neither is
    // checked for you -- gate on them yourself, or a bound key fires while you
    // are alt-tabbed and typing "hey" in chat runs whatever 'h' and 'e' do.
    //
    //   fx_focused   is the attached client the foreground window? Compared by
    //                pid, so it survives the client swapping top-level windows.
    //                0 while detached.
    //
    //   fx_typing    is an in-game text field focused right now? This is the
    //                CLIENT's own answer, read out of UserInputService, not a
    //                guess from how recently you pressed something -- true the
    //                instant the chat bar opens, false the instant it closes.
    //
    //   fx_textbox   the focused TextBox instance itself, or 0. fx_typing is
    //                just this compared against 0; use this one if you want to
    //                read the box (its class, its name, its text).
    //
    // Cheap enough to call in a poll loop: the service pointer behind it is
    // cached and only re-resolved when the DataModel changes.
    FX_API int      fx_focused(void);
    FX_API int      fx_typing(void);
    FX_API uint64_t fx_textbox(void);

    // Is the write gate open? fx_wrblk already honours it, so a snippet that
    // only writes memory never needs to ask.
    //
    // This is for a snippet whose effect is NOT a memory write -- moving the
    // mouse, pressing a key, anything done through the OS rather than through
    // the game's memory. Nothing else holds those to the gate, so they have to
    // hold themselves:
    //
    //     if (fx_armed()) SetCursorPos(x, y);
    //     else            fx_log("would move the cursor");
    //
    // Getting this wrong is not a small bug. Disarmed is meant to mean nothing
    // reaches the game, and a snippet that moves your mouse anyway while the
    // console reports refused writes teaches you not to trust the gate.
    FX_API int      fx_armed(void);

    // ---- memory map --------------------------------------------------------
    // The client's COMMITTED, WRITABLE regions. Pass max = 0 to get the count
    // without filling anything, so an array can be sized first:
    //
    //     int n = fx_regions(nullptr, nullptr, 0);
    //     std::vector<uint64_t> b(n), s(n);
    //     fx_regions(b.data(), s.data(), n);
    //
    // This exists for value SCANNING: to search for a number you first have to
    // know where there is memory to search, and nothing else here answers that.
    // fx_rdblk then reads each region in whatever chunks you like.
    //
    // Writable only. Read-only pages are code and constants -- gigabytes that
    // cannot hold a value which changes as you play, so scanning them is pure
    // cost. Guard pages are skipped too: touching one is how a process finds
    // out you are there.
    FX_API int      fx_regions(uint64_t* base, uint64_t* size, int max);

    // ---- the local character ----
    // Walking Players -> LocalPlayer -> Character by hand is two traps in one:
    // fx_service is far too expensive for a loop, and the Character pointer
    // dies on every respawn, taking everything derived from it along. Both are
    // handled here.
    //
    //   fx_player      LocalPlayer, or 0
    //   fx_character   the current Character model, or 0 while dead or loading
    //   fx_parts       the character's BaseParts, class-filtered
    //   fx_gen         THE staleness counter -- the only check a looping
    //                  snippet needs:
    //
    //     static unsigned gen = 0;
    //     if (fx_gen() != gen) { gen = fx_gen(); /* re-resolve everything */ }
    //
    // fx_gen moves whenever ANYTHING you cached may have been freed --
    // respawning, leaving the place, joining another, detaching -- and not just
    // the character: services, Instances, Primitives, part lists, all of it.
    // Never test it against 0 for "not yet resolved" -- it never resets,
    // exactly so a stale holder cannot get a coincidental match.
    //
    // THE RE-RESOLVE CAN COME BACK 0, AND THAT IS NOT A FAILURE. The generation
    // moves the moment the DataModel is replaced, which is BEFORE the new world
    // has finished building itself: on a rejoin the character does not exist
    // yet. Resolve ONCE on the generation edge and you store that 0 --
    // and since the generation never moves again, you keep it for the rest of
    // the session and every tick after reads "not spawned". Re-resolve until it
    // ANSWERS, not once when it changes:
    //
    //     if (fx_gen() != gen || !thing) {
    //         gen = fx_gen();
    //         thing = resolve();      // may still be 0 -- ask again next tick
    //     }
    //
    // The retry is bounded and costs nothing once it succeeds, so the `|| !thing`
    // half is cheap insurance against a bug that presents as "the snippet just
    // stopped working after I rejoined".
    //
    // fx_parts exists because reading BasePart.Primitive on a non-BasePart
    // returns whatever sits at that offset, and it looks pointer-shaped: Script
    // gives back a module address, Shirt the float 1.0. Writing through one
    // corrupts the client. Take the list from here rather than filtering
    // yourself; it descends one level into Accessory/Model/Tool/Folder, since a
    // welded hat or tool collides too.
    FX_API uint64_t fx_player(void);
    FX_API uint64_t fx_character(void);
    FX_API unsigned fx_gen(void);
    FX_API int      fx_parts(uint64_t* out, int max);

    // ---- fx_part: fx_find, memoised ----
    // Same answer as fx_find(model, name), same 0 when it is not there. Use it
    // for anything looked up EVERY TICK -- rig parts, a Health value -- and
    // plain fx_find for a one-off walk.
    //
    // fx_find re-reads every sibling's name on every call, ~125 us against a
    // 20-part rig, to re-derive a mapping that cannot change while the model
    // lives. Two parts on twenty players is about 5 ms a tick spent on that.
    // Measured 57x on the repeat lookups.
    //
    // Misses are cached too, so probing for a part a model does not have is
    // cheap after the first ask.
    //
    // Invalidated three ways, and the third is the one that matters: fx_gen
    // moving, the model's child count changing, and the cached part still
    // having this model as its PARENT. Without that last check another
    // player respawning could hand you pointers into their freed character --
    // fx_gen only tracks YOUR character, and a rebuilt one usually has the same
    // child count, so nothing else would notice. You do not need to clear it.
    FX_API uint64_t fx_part(uint64_t model, const char* name);

    // ---- fx_players: the whole roster, in one call --------------------------
    //
    // Every Player in the server with the pointers a snippet actually wants
    // hanging off it. Five snippets each hand-walked Players and each one paid
    // for the same three lessons; this is that walk, once, with the answers
    // that cannot change memoised.
    //
    // Returns the number of players FOUND, and fills at most `max` of them --
    // so fx_players(0, 0) is how you ask for a count. Order follows the
    // Players service's own child order, which is join order, and is NOT
    // stable across a rejoin.
    //
    // NOTHING IS FILTERED, and that is deliberate. A dead player, one still
    // loading, one whose Humanoid could not be resolved -- all of them come
    // back, with zeros in the fields that could not be read. A roster that
    // quietly dropped the players it could not decode would be indis-
    // tinguishable from a nearly-empty server, and "filters over memory reads
    // must fail OPEN" is the rule that exists because a health filter once
    // removed every player in a normal game. Decide who to skip at the call
    // site, where you can see what you are skipping:
    //
    //     fx_player_info p[32];
    //     int n = fx_players(p, 32);
    //     for (int i = 0; i < n; i++) {
    //         if (p[i].is_local || !p[i].root) continue;   // YOUR rule
    //         if (p[i].health <= 0.0f) continue;           // ...and this one
    //         ...
    //     }
    //
    // health/maxhealth are 0 when there is no Humanoid to read them from,
    // which is NOT the same fact as "dead" -- check `humanoid` first if the
    // difference matters to you.
    //
    // team is the Team instance, or 0 for no team (also every player in a
    // game with no Teams service). Compare it against your own entry's rather
    // than trying to read a name out of it.
    struct fx_player_info {
        uint64_t player;      // the Player instance
        uint64_t model;       // Character model; 0 while dead or still loading
        uint64_t humanoid;    // Humanoid under it; 0 if there is none
        uint64_t root;        // HumanoidRootPart; 0 if there is none
        uint64_t team;        // Team instance; 0 = no team
        int64_t  userid;      // Player.UserId
        float    health;      // 0 when humanoid is 0 -- see above
        float    maxhealth;
        int      is_local;    // 1 for you, 0 for everyone else
        char     name[64];    // Instance.Name (the username), NUL-terminated
    };
    FX_API int fx_players(fx_player_info* out, int max);

    // ---- fx_screen: world -> CLIENT-AREA pixels ----
    // THREE outcomes, and picking the right test matters:
    //
    //    1  on screen. sx/sy are inside the frame.
    //   -1  projected, but OUTSIDE the frame. sx/sy are still written and still
    //       correct -- the point is simply off the edge.
    //    0  no answer. Behind the camera, outside the depth range, or the view
    //       could not be read. sx/sy are untouched.
    //
    // So `== 1` means visible, and `!= 0` means "I got a position". An ESP wants
    // the first; a cursor aim wants the second, because in a mouse-locked game a
    // target just off the edge is still reachable by turning and dropping it
    // there is how a lock gets lost mid-turn.
    //
    // Uses Roblox's own world-to-clip matrix, so a scripted FOV or a custom
    // camera is already accounted for. Rebuilding a projection from
    // Camera.Rotation and FieldOfView instead is wrong for exactly that reason.
    //
    // These are CLIENT-AREA pixels. fx_overlay_post wants MONITOR pixels, so
    // add the Roblox window origin before posting.
    //
    // The view is read FRESH every call. It was briefly cached for a
    // millisecond and that broke closed-loop callers: a cursor aim measures the
    // error, pushes the mouse, and measures again, so a camera pose even
    // slightly stale makes the error wrong by however far the view turned and
    // the aim oscillates. An ESP would never notice. Do not re-add it.
    FX_API int      fx_screen(float3 world, float* sx, float* sy);

    // The viewport fx_screen divides by -- the centre is half of each. Same
    // source as the projection, so the two cannot disagree. 0 if unavailable.
    FX_API int      fx_viewport(float* w, float* h);

    // ---- fx_pos: a part's world position ----
    // 1 and *out written, or 0 and *out untouched. Class-checked against the
    // same whitelist fx_parts uses, so it cannot be talked into dereferencing
    // BasePart.Primitive on something that is not a BasePart.
    //
    // Returns 0 rather than a zero vector on failure ON PURPOSE: "at the origin"
    // and "could not read" must not look the same to a caller that filters on
    // the answer.
    FX_API int      fx_pos(uint64_t part, float3* out);

    // ---- world geometry: fx_raycast / fx_visible ----
    // A cache of every BasePart under Workspace as an oriented box, MINUS
    // characters -- any Model containing a Humanoid is skipped whole. Occlusion
    // means "is there a wall in the way", not "is there an elbow in the way".
    //
    // EVERY PART IS ITS BOUNDING BOX. Shape is not read, so a Ball is the cube
    // around it, a Wedge is the box it is half of, a Truss is solid rather than
    // a lattice, and a MeshPart or Union is whatever box contains it. The error
    // is always the same direction -- the box CONTAINS the part, so these can
    // report blocked when the line was actually clear, and never clear when
    // something was in the way. Worth knowing before concluding a miss is a
    // bug: a shot that "should" have passed over a ramp or past a sphere's
    // corner is this, not a wrong read.
    //
    // Built lazily on first use and kept until the DataModel changes. NOT
    // rebuilt on fx_gen: that ticks on every respawn and reindexing tens of
    // thousands of parts per death would cost more than it saves. The honest
    // consequence is that parts which MOVE go stale; static map geometry, which
    // is what these are for, does not. fx_geom(1) forces a rebuild.
    //
    //   fx_geom(0)   parts in the cache, building it if needed
    //   fx_geom(1)   rebuild now, then the same
    //   fx_geom_ms   milliseconds the last build took, NEGATIVE if it was cut
    //                short at the part ceiling
    FX_API int      fx_geom(int refresh);
    FX_API int      fx_geom_ms(void);

    // Nearest geometry along a ray. 1 on a hit, with *hit the world point and
    // *part the BasePart; 0 on a miss or with no cache. dir need not be
    // normalised. Either output may be null.
    FX_API int      fx_raycast(float3 origin, float3 dir, float maxdist,
                               float3* hit, uint64_t* part);

    // Is the line from A to B clear? 1 clear, 0 blocked.
    //
    // FAILS OPEN -- no cache, no offsets or a degenerate segment all return 1.
    // A visibility filter that fails closed hides everything, and a filter that
    // read an unreadable value as "dead" once removed every player in a normal
    // game. If it cannot be corroborated, keep the target.
    //
    // Both ends are pulled in slightly, so standing on a floor or having an eye
    // point inside a wall's skin does not report itself occluded.
    FX_API int      fx_visible(float3 from, float3 to);

    // ---- leaving a place ----
    // OPTIONAL. fx_gen already ticks when the DataModel is replaced, so it is
    // a superset: checking it alone invalidates correctly for both respawning
    // AND leaving. One check is enough:
    //
    //     static unsigned gen = 0;
    //     if (fx_gen() != gen || !hum) { gen = fx_gen(); re_resolve(); }
    //
    // fx_dm_gen exists only to tell the two apart, for code that REACTS
    // differently rather than just re-resolving. Respawning is routine: find
    // the Humanoid again and carry on. Leaving is not: there is no world, and
    // anything mid-write should stop rather than retry -- writing through a
    // freed pointer is a corrupted client, not a stale read.
    //
    // Never compare either against 0 for "not yet": neither resets, precisely
    // so a stale holder cannot get a coincidental match.
    FX_API unsigned fx_dm_gen(void);
}

// ---- key names -----------------------------------------------------------
// So a snippet can say VK_F6 instead of 0x75.
//
// LETTERS AND DIGITS NEED NOTHING FROM THIS LIST: a virtual-key code for those
// IS the ASCII value of the uppercase character, so write the character.
//
//     if (fx_key_pressed('K'))        // the K key
//     if (fx_key_pressed('5'))        // the 5 key, not numpad 5
//     if (fx_key(VK_SHIFT) && fx_key_pressed('W'))
//
// Note the uppercase: 'k' is 0x6B, which is VK_ADD -- numpad plus. Lowercase
// letters are NOT their own key codes.
//
// Each #define is guarded, so a snippet that pulls in <windows.h> for its own
// reasons gets whichever came first, no redefinition either way. fx.h does not
// include <windows.h> itself: that would cost every snippet compile a second
// or so and drag in the min/max macros, and Run is meant to feel immediate.
#ifndef VK_LBUTTON
#define VK_LBUTTON  0x01
#define VK_RBUTTON  0x02
#define VK_MBUTTON  0x04
#define VK_XBUTTON1 0x05
#define VK_XBUTTON2 0x06
#endif
#ifndef VK_BACK
#define VK_BACK    0x08
#define VK_TAB     0x09
#define VK_RETURN  0x0D
#define VK_SHIFT   0x10
#define VK_CONTROL 0x11
#define VK_MENU    0x12          /* Alt */
#define VK_PAUSE   0x13
#define VK_CAPITAL 0x14
#define VK_ESCAPE  0x1B
#define VK_SPACE   0x20
#define VK_PRIOR   0x21          /* Page Up */
#define VK_NEXT    0x22          /* Page Down */
#define VK_END     0x23
#define VK_HOME    0x24
#define VK_LEFT    0x25
#define VK_UP      0x26
#define VK_RIGHT   0x27
#define VK_DOWN    0x28
#define VK_INSERT  0x2D
#define VK_DELETE  0x2E
#endif
#ifndef VK_NUMPAD0
#define VK_NUMPAD0  0x60
#define VK_NUMPAD1  0x61
#define VK_NUMPAD2  0x62
#define VK_NUMPAD3  0x63
#define VK_NUMPAD4  0x64
#define VK_NUMPAD5  0x65
#define VK_NUMPAD6  0x66
#define VK_NUMPAD7  0x67
#define VK_NUMPAD8  0x68
#define VK_NUMPAD9  0x69
#define VK_MULTIPLY 0x6A
#define VK_ADD      0x6B
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL  0x6E
#define VK_DIVIDE   0x6F
#endif
#ifndef VK_F1
#define VK_F1  0x70
#define VK_F2  0x71
#define VK_F3  0x72
#define VK_F4  0x73
#define VK_F5  0x74
#define VK_F6  0x75
#define VK_F7  0x76
#define VK_F8  0x77
#define VK_F9  0x78
#define VK_F10 0x79
#define VK_F11 0x7A
#define VK_F12 0x7B
#endif
#ifndef VK_LSHIFT
#define VK_LSHIFT   0xA0
#define VK_RSHIFT   0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU    0xA4         /* left Alt */
#define VK_RMENU    0xA5         /* right Alt */
#endif
#ifndef VK_OEM_1
#define VK_OEM_1      0xBA       /* ;: */
#define VK_OEM_PLUS   0xBB       /* =+ */
#define VK_OEM_COMMA  0xBC       /* ,< */
#define VK_OEM_MINUS  0xBD       /* -_ */
#define VK_OEM_PERIOD 0xBE       /* .> */
#define VK_OEM_2      0xBF       /* /? */
#define VK_OEM_3      0xC0       /* `~ */
#define VK_OEM_4      0xDB       /* [{ */
#define VK_OEM_5      0xDC       /* \| */
#define VK_OEM_6      0xDD       /* ]} */
#define VK_OEM_7      0xDE       /* '" */
#endif

// True once per press. Consumes one from the counter per call rather than
// snapping to it, so several presses arriving between two polls each get their
// turn instead of collapsing into one.
static inline bool fx_key_pressed(int vk) {
    static unsigned seen[256] = { 0 };
    // Per-DLL, and PRIMED on first sight of each key to the live counter rather
    // than to 0. The counter is shared and monotonic across all runs now (it is
    // never reset -- see kb_run_begin in input.cpp), so a fresh snippet that
    // started with seen[vk] == 0 would treat every press another run had ALREADY
    // logged as its own, and fire on every poll forever. Priming to the current
    // value means "count presses from when I first looked", which is exactly
    // what the old per-run counter reset used to fake. Behaviour is identical to
    // before for a single run; correct for several. seen stays per-DLL, so two
    // snippets watching one key still each get every press.
    static unsigned char primed[256] = { 0 };
    if (vk < 0 || vk > 255) return false;
    unsigned seq = fx_key_seq(vk);
    if (!primed[vk]) { primed[vk] = 1; seen[vk] = seq; return false; }
    if (seen[vk] == seq) return false;
    seen[vk]++;
    return true;
}

// The gate a hotkey almost always wants: the game is in front AND you are not
// typing into it. Deliberately NOT applied to fx_key/fx_key_pressed
// automatically -- some snippets legitimately want a key that works while
// alt-tabbed (a panic key, a toggle you hit from the console), and silently
// swallowing those would be the harder bug to find.
//
//     if (fx_key_pressed('E') && fx_input_ok()) lock_on();
//
// Order matters: check the key FIRST. fx_key_pressed consumes one press per
// call, so testing the gate first would silently eat presses made while
// alt-tabbed and lose the next real one.
static inline bool fx_input_ok(void) { return fx_focused() && !fx_typing(); }

// ---- header-only conveniences (just wrap the externs; safe to inline) ----
static inline float  fx_rf (uint64_t a){ float v=0;  fx_rdblk(a,&v,4);  return v; }
static inline void   fx_wf (uint64_t a, float v){    fx_wrblk(a,&v,4); }
static inline float3 fx_rf3(uint64_t a){ float3 v{}; fx_rdblk(a,&v,12); return v; }
static inline void   fx_wf3(uint64_t a, float3 v){   fx_wrblk(a,&v,12); }

static inline uint32_t fx_rd32(uint64_t a){ uint32_t v=0; fx_rdblk(a,&v,4); return v; }
static inline uint8_t  fx_rd8 (uint64_t a){ uint8_t  v=0; fx_rdblk(a,&v,1); return v; }
static inline void     fx_wr32(uint64_t a, uint32_t v){ fx_wrblk(a,&v,4); }
static inline void     fx_wr8 (uint64_t a, uint8_t  v){ fx_wrblk(a,&v,1); }

// A count this large is not a big model, it is two unrelated pointers being
// subtracted. That happens for real: the instant the DataModel is replaced --
// a rejoin, a teleport, or a round ending in a game that swaps places -- every
// pointer derived from the old one reads freed memory, and the begin/end pair
// comes back as noise.
//
// Trusting it is not a slightly-wrong answer, it is a hang. A billion-entry
// walk at four syscall reads per entry runs for hours, and NONE of the loops
// below check fx_running(), so Stop cannot get you out of it -- the snippet is
// not sleeping, it is busy. Measured against Rivals, where the lobby-to-match
// transition replaced the DataModel twice in six seconds and the console had
// to be killed from Task Manager.
//
// 65536 is far above anything real (the DataModel itself has ~145 children)
// and far below anything that costs noticeable time to reject.
#define FX_MAX_CHILDREN 65536ull

// Anything outside this is not a heap pointer. Cheap, and it rejects the
// garbage before it is ever dereferenced.
static inline bool fx_ptr_ok(uint64_t p){
    return p >= 0x10000ull && p < 0x7FFFFFFFFFFFull;
}

// How many children an Instance has. On Windows the children vector is behind
// one indirection: Instance+ChildrenStart holds a pointer to the container, and
// the container holds begin at +0 and end at +ChildrenEnd. The Linux original
// had the vector inline on the Instance, which is why this differs.
static inline uint64_t fx_nchildren(uint64_t inst){
    uint64_t so = fx_off("Instance.ChildrenStart");
    uint64_t eo = fx_off("Instance.ChildrenEnd");
    if (so == (uint64_t)-1 || eo == (uint64_t)-1) return 0;
    uint64_t c = fx_rd(inst + so);
    if (!fx_ptr_ok(c)) return 0;
    uint64_t begin = fx_rd(c), end = fx_rd(c + eo);
    if (!fx_ptr_ok(begin) || end <= begin) return 0;
    uint64_t n = (end - begin) / 0x10;    // shared_ptr<Instance> = ptr + ctrl
    return n > FX_MAX_CHILDREN ? 0 : n;   // implausible means stale, not huge
}

// The i-th child pointer, or 0. Pure reads.
static inline uint64_t fx_child(uint64_t inst, int i){
    uint64_t so = fx_off("Instance.ChildrenStart");
    uint64_t eo = fx_off("Instance.ChildrenEnd");
    if (so == (uint64_t)-1 || eo == (uint64_t)-1) return 0;
    uint64_t c = fx_rd(inst + so);
    if (!fx_ptr_ok(c)) return 0;
    uint64_t begin = fx_rd(c), end = fx_rd(c + eo);
    if (!fx_ptr_ok(begin) || end <= begin) return 0;
    uint64_t n = (end - begin) / 0x10;
    if (n > FX_MAX_CHILDREN) return 0;    // same guard; this is called directly
    if ((uint64_t)i >= n) return 0;
    return fx_rd(begin + (uint64_t)i * 0x10);
}

// First child whose Name matches. 0 if absent.
//
// Bails on Stop. Even a legitimate walk is thousands of syscalls now that a
// name costs an extra dereference, and a snippet that cannot be stopped
// mid-walk is a snippet that has to be killed.
static inline uint64_t fx_find(uint64_t inst, const char* name){
    uint64_t n = fx_nchildren(inst);
    for (uint64_t i = 0; i < n; i++) {
        if (!fx_running()) return 0;
        uint64_t ch = fx_child(inst, (int)i);
        if (ch && std::strcmp(fx_name(ch), name) == 0) return ch;
    }
    return 0;
}

// ---- module-relative pointers -------------------------------------------
// Most conf entries are field offsets: distances *inside* an object you
// already hold. A handful are module RVAs -- locations in RobloxPlayerBeta.exe
// that hold a pointer to a singleton. (Linux kept those in a separate conf
// file; here one conf holds both, so these helpers carry the distinction.)
//
// Adding a base to a field offset, or forgetting to add it to an RVA, both
// read garbage. Going through fx_rva/fx_deref makes that hard to get wrong.
//
// Currently RVA-shaped in the conf:
//   FakeDataModel.Pointer, TaskScheduler.Pointer, VisualEngine.Pointer,
//   MouseService.SensitivityPointer, Humanoid.PlatformStatePointer

// The absolute address of a module-relative slot. 0 if the key is missing.
static inline uint64_t fx_rva(const char* key) {
    uint64_t o = fx_off(key);
    if (o == (uint64_t)-1) return 0;
    uint64_t b = fx_base();
    return b ? b + o : 0;
}

// What that slot points at -- the usual one-liner for reaching a singleton:
//   uint64_t sched = fx_deref("TaskScheduler.Pointer");
//   float fps = fx_rf(sched + fx_off("TaskScheduler.MaxFPS"));
static inline uint64_t fx_deref(const char* key) {
    uint64_t a = fx_rva(key);
    return a ? fx_rd(a) : 0;
}

// ---- frame-rate cap ------------------------------------------------------
// TaskScheduler.MaxFPS is named for what it controls, not for what it stores:
// the field is a DOUBLE holding the frame PERIOD in seconds. Verified against a
// live client capped at 144 — the slot read 0.006944444, and 1/0.006944444 is
// exactly 144. Reading it as a float (or as an fps number) gives 0.
//
// Both helpers go through the normal primitives, so the setter honours the arm
// gate like any other write.
static inline double fx_getfps(void) {
    uint64_t s = fx_deref("TaskScheduler.Pointer");
    uint64_t o = fx_off("TaskScheduler.MaxFPS");
    if (!s || o == (uint64_t)-1) return 0.0;
    double period = 0.0;
    fx_rdblk(s + o, &period, 8);
    return period > 0.0 ? 1.0 / period : 0.0;
}

static inline size_t fx_setfps(double fps) {
    uint64_t s = fx_deref("TaskScheduler.Pointer");
    uint64_t o = fx_off("TaskScheduler.MaxFPS");
    if (!s || o == (uint64_t)-1 || fps <= 0.0) return 0;
    double period = 1.0 / fps;
    return fx_wrblk(s + o, &period, 8);
}

// A service off the DataModel, by class name (Workspace, Players, Lighting…).
static inline uint64_t fx_service(const char* cls){
    uint64_t dm = fx_dm();
    if (!dm) return 0;
    uint64_t n = fx_nchildren(dm);
    for (uint64_t i = 0; i < n; i++) {
        // Same reason as fx_find: this is the most expensive walk in the API
        // -- a class name per child, each a descriptor deref plus a string
        // read -- and it must not be the thing that swallows a Stop.
        if (!fx_running()) return 0;
        uint64_t ch = fx_child(dm, (int)i);
        if (ch && std::strcmp(fx_class(ch), cls) == 0) return ch;
    }
    return 0;
}
