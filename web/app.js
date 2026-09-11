// app.js — behaviour for the Eclipse console UI.
//
// The kit ships no JS by design; this is the layer that drives its documented
// state classes and talks to the host. Message format matches the console's own
// convention: "TYPE:body".
(function () {
  "use strict";

  // ---- host bridge --------------------------------------------------------
  // One place, one transport: the host binds host_send() and calls
  // window.__onHostMsg() back, over the webview library's own channel. This
  // page runs ONLY in the console's own window -- in app mode (the overlay)
  // the host repositions that same window over the game, it does not load a
  // second copy of this page anywhere. The iframe transport shim that used
  // to live here went with the iframe (re-architected 2026-08-22).
  function send(msg) {
    if (window.host_send) window.host_send(msg);
  }

  var $ = function (id) { return document.getElementById(id); };

  /* Drop the caret, wherever it is. Used by app mode: the window is shown and
     hidden constantly there, and Windows hands focus back to the control that
     had it -- the editor -- so reopening the console armed the next keystroke
     to edit whatever file was open. Blurring is enough; a click puts the
     caret back exactly where the user wants it. */
  function blurEditable() {
    var a = document.activeElement;
    if (a && a !== document.body && typeof a.blur === "function") a.blur();
  }
  /* APPWIN:1 arrives BEFORE the window is shown, and the activation that
     follows is what restores DOM focus -- so blurring only on the message
     loses the race. This catches the window actually coming forward.

     SCOPED TO AN OPEN, and that scope is the whole point. Listening for every
     focus event in app mode also catches ALT-TAB BACK, which is the user
     returning to work they were in the middle of -- and blurring there threw
     away a caret they had deliberately placed. Reported 2026-08-26.

     So APPWIN:1 arms a stamp and the next focus event consumes it. An open
     always pairs the two within a frame or two; an alt-tab back arrives with
     nothing armed and leaves the caret alone. */
  var appOpenedAt = 0;
  window.addEventListener("focus", function () {
    if (!document.body.classList.contains("app-mode")) return;
    if (!appOpenedAt || Date.now() - appOpenedAt > 1500) return;
    appOpenedAt = 0;
    blurEditable();
  });

  function esc(s) {
    return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
  }

  // ---- persisted UI state ---------------------------------------------------
  // Focus, collapsed groups, open tabs and settings live in localStorage.
  //
  // This was briefly moved out to %APPDATA%\EclipseConsole\ui-state.json and
  // moved back. What it bought -- an inspectable file beside the snippets --
  // did not pay for what it cost: localStorage is SYNCHRONOUS and readable
  // before the first render, while a host-held copy only arrives on READY, so
  // every value needed a default, a second apply, and a re-render once the
  // real one landed.
  //
  // The one genuine hazard is worth knowing rather than fixing: WebView2 names
  // its user-data folder after the EXECUTABLE's filename, so renaming
  // eclipse_console.exe silently orphans all of this. Snippets are unaffected
  // -- they are real files and never lived here.
  //
  // ---- settings -------------------------------------------------------------
  // One declarative table drives everything: the modal's rows, the search, the
  // storage, and the defaults. Adding a setting is adding an entry here and
  // reading it with S() -- there is no second place to register it, which is
  // the only way a list this long stays correct.
  //
  // Values are read through S() at the point of use rather than copied into
  // locals at startup. A copy would have to be re-synced on every change, and
  // the one that got forgotten would be a setting that silently does nothing
  // until a restart.
  //
  // `desc` is not decoration -- it is what the search matches on, and it is
  // the only explanation most of these will ever get.
  var SETTINGS = [
    { k: "editor.fontSize", cat: "Editor", type: "int", def: 14, min: 9, max: 28,
      label: "Font size", desc: "Code text size, in pixels." },
    { k: "editor.tabWidth", cat: "Editor", type: "int", def: 4, min: 2, max: 8,
      label: "Tab width", desc: "Spaces the Tab key inserts." },
    { k: "editor.autoIndent", cat: "Editor", type: "bool", def: true,
      label: "Auto-indent new lines",
      desc: "Enter keeps the current indentation, plus a level after { [ or (." },
    { k: "editor.dedentClose", cat: "Editor", type: "bool", def: true,
      label: "Outdent closing braces",
      desc: "Typing } ] or ) on a blank line pulls it back one level." },
    { k: "editor.blockIndent", cat: "Editor", type: "bool", def: true,
      label: "Tab indents the selection",
      desc: "Tab indents every selected line; Shift+Tab outdents." },
    { k: "editor.smartHome", cat: "Editor", type: "bool", def: true,
      label: "Smart Home key",
      desc: "Home goes to the first non-space character, then to column 0." },
    { k: "editor.lineTools", cat: "Editor", type: "bool", def: true,
      label: "Line shortcuts",
      desc: "Ctrl+D duplicates, Ctrl+/ comments, Alt+Up and Alt+Down move lines." },
    { k: "editor.matchBrackets", cat: "Editor", type: "bool", def: true,
      label: "Highlight matching bracket",
      desc: "Outlines the bracket beside the caret and its partner." },
    { k: "editor.minimap", cat: "Editor", type: "bool", def: true,
      label: "Show minimap",
      desc: "Overview of the file beside the editor. Click or drag it to jump." },

    { k: "console.height", cat: "Console", type: "int", def: 210, min: 80, max: 800,
      label: "Console height", desc: "Log panel height, in pixels." },
    { k: "console.splitHoverMs", cat: "Console", type: "int", def: 300, min: 0, max: 1000,
      label: "Divider highlight delay",
      desc: "Hover time before the console divider lights up, in milliseconds." },
    { k: "console.scrollback", cat: "Console", type: "int", def: 500, min: 100, max: 5000,
      label: "Scrollback lines", desc: "Log lines kept before the oldest are dropped." },
    { k: "console.showInfo", cat: "Console", type: "bool", def: true,
      label: "Show info lines", desc: "Progress output from snippets and the host." },
    { k: "console.showOk",   cat: "Console", type: "bool", def: true,
      label: "Show ok lines",   desc: "Saves, attaches, successful writes." },
    { k: "console.showWarn", cat: "Console", type: "bool", def: true,
      label: "Show warn lines", desc: "Recoverable problems." },
    { k: "console.showErr",  cat: "Console", type: "bool", def: true,
      label: "Show error lines", desc: "Failures." },

    { k: "sidebar.width", cat: "Sidebar", type: "int", def: 208, min: 140, max: 480,
      label: "Sidebar width", desc: "Snippet list width, in pixels." },
    { k: "sidebar.springEnabled", cat: "Sidebar", type: "bool", def: true,
      label: "Spring-loaded folders",
      desc: "Hovering a collapsed group while dragging opens it." },
    { k: "sidebar.springMs", cat: "Sidebar", type: "int", def: 600, min: 150, max: 2000,
      label: "Spring delay",
      desc: "Hover time before a group opens, in milliseconds." },
    { k: "sidebar.dragEdge", cat: "Sidebar", type: "int", def: 44, min: 10, max: 120,
      label: "Drag auto-scroll edge",
      desc: "Scrolling band at the top and bottom of the sidebar, in pixels." },
    { k: "sidebar.dragSpeed", cat: "Sidebar", type: "int", def: 18, min: 4, max: 40,
      label: "Drag auto-scroll speed",
      desc: "Top speed at the very edge, in pixels per frame." },
    { k: "sidebar.dropZone", cat: "Sidebar", type: "float", def: 0.3, min: 0.1, max: 0.45, step: 0.05,
      label: "Group drop zone",
      desc: "How much of a group header reorders it instead of dropping into it." },

    { k: "tabs.maxWidth", cat: "Tabs", type: "int", def: 190, min: 120, max: 320,
      label: "Tab width", desc: "Width a tab keeps while there is room." },
    { k: "tabs.minWidth", cat: "Tabs", type: "int", def: 150, min: 80, max: 320,
      label: "Minimum tab width",
      desc: "Tabs shrink no further than this; then the strip scrolls." },
    { k: "tabs.dragEdge", cat: "Tabs", type: "int", def: 48, min: 10, max: 160,
      label: "Drag auto-scroll edge",
      desc: "Scrolling band at each end of the tab strip, in pixels." },
    { k: "tabs.dragSpeed", cat: "Tabs", type: "int", def: 8, min: 2, max: 40,
      label: "Drag auto-scroll speed",
      desc: "Top speed at the end of the strip, in pixels per frame." },
    { k: "tabs.freezeOnClose", cat: "Tabs", type: "bool", def: true,
      label: "Hold widths while closing",
      desc: "Keeps close buttons in place while you close several tabs." },
    { k: "tabs.restoreOnStart", cat: "Tabs", type: "bool", def: true,
      label: "Reopen tabs on launch",
      desc: "Restores last session's tabs. Contents always come from disk." },

    // These only ever make the write gate STRICTER, or leave it where it is.
    // There is still deliberately no setting to remember the armed state
    // across LAUNCHES: starting armed would defeat the point of having a gate
    // at all. Surviving a detach is a different and much smaller thing -- the
    // session is still the one you armed -- which is why disarmOnDetach can be
    // turned off and "armed at startup" cannot.
    { k: "safety.confirmArm", cat: "Safety", type: "bool", def: false,
      label: "Confirm before arming", desc: "Ask before arming." },
    { k: "safety.autoDisarmMin", cat: "Safety", type: "int", def: 0, min: 0, max: 120,
      label: "Auto-disarm after",
      desc: "Minutes until arming turns itself off. 0 disables." },
    // On by default: detaching and attaching again is how you move to a
    // DIFFERENT client, and staying armed through that means the next
    // session's first write happens without anyone deciding it should.
    { k: "safety.disarmOnDetach", cat: "Safety", type: "bool", def: true, host: true,
      label: "Disarm on detach",
      desc: "Drop back to disarmed when the client exits or you detach. Off " +
            "keeps the arm state, so re-attaching is already armed." },

    // host:true means the value is pushed to the host over SETTINGS: -- the
    // page cannot act on these itself.
    { k: "console.dryFlushMs", cat: "Console", type: "int", def: 1000, min: 50, max: 5000,
      host: true,
      label: "Dry-run report interval",
      desc: "How often refused writes are summarised, in milliseconds." },
    // dimIf: inert while the console lives in the game -- but NOT because the
    // panel is unreachable there. Right-clicking the badge retargets it to
    // open the panel, and in overlay mode the hud page keeps the badge and
    // panel live REGARDLESS of this value (its visibility and rect tests all
    // short-circuit on overlay mode), so that flyout, not this switch, is
    // what decides -- flipping this would still do nothing, which is what the
    // dim claims. Dimmed rather than hidden: a setting that vanishes reads
    // as a bug.
    { k: "console.hud", cat: "In-game", type: "bool", def: true, host: true,
      dimIf: "console.overlayMode",
      label: "In-game panel",
      desc: "Log and arm state over Roblox. Takes clicks, never keystrokes." },
    { k: "console.overlayMode", cat: "In-game", type: "bool", def: false, host: true,
      label: "App as HUD",
      desc: "The console lives in the game. The badge opens it, Insert toggles " +
            "it, Esc hands the keyboard back to Roblox. Right-click the badge " +
            "to make it open the panel instead." },
    { k: "console.logToFile", cat: "Console", type: "bool", def: false, host: true,
      label: "Also write the log to a file",
      desc: "Mirrors the log to console.log beside the snippets folder." },

    { k: "snippets.extraFlags", cat: "Snippets", type: "text", def: "", host: true,
      label: "Extra compile flags",
      desc: "Added to the g++ line for every snippet, e.g. -O3. A project's own " +
            "flags win any conflict." },
    { k: "snippets.showWarnings", cat: "Snippets", type: "bool", def: true, host: true,
      label: "Show compiler warnings",
      desc: "Print what g++ said when a snippet builds but looks wrong." },

    // An action row: it runs something instead of storing something. See
    // actionRow() for why that is a separate shape rather than a value with a
    // button attached.
    { k: "offsets.check", cat: "Offsets", type: "action",
      label: "Check offsets", button: "Run check", msg: "CHECK_OFFSETS",
      desc: "Reads known values and checks they make sense. The status line only " +
            "proves three offsets; this covers the rest. Results go to the console." },

    { k: "offsets.refresh", cat: "Offsets", type: "action",
      label: "Refresh offsets", button: "Fetch now", msg: "REFRESH_OFFSETS",
      desc: "Re-downloads the offset table for the attached client. Needs to be " +
            "attached." },

    { k: "window.rememberPos", cat: "Window", type: "bool", def: true, host: true,
      label: "Remember window position",
      desc: "Reopen where you left it, in the game and on the desktop." },

    { k: "window.rememberSize", cat: "Window", type: "bool", def: true, host: true,
      label: "Remember window size",
      desc: "Reopen at the size you left it." }
  ];

  // "In-game" is where the console LIVES, which is a different question from
  // how it logs -- everything else under Console is the log panel. The KEYS
  // stay console.* deliberately: the host parses them by exact name and they
  // are already on disk in user configs, so renaming them would silently
  // reset both settings.
  var SETTING_CATS = ["Editor", "Console", "In-game", "Snippets", "Sidebar", "Tabs",
                      "Safety", "Offsets", "Window"];

  var settingDefs = {};
  SETTINGS.forEach(function (s) { settingDefs[s.k] = s; });

  // An action has no value, which every value-shaped path has to be told: it
  // is not loaded, not stored, not exported, and never counts as "changed".
  // Cheaper to ask than to give it a sentinel value and hope nothing compares
  // against it.
  function isAction(s) { return s.type === "action"; }

  var settings = {};

  function loadSettings() {
    var saved = {};
    try { saved = JSON.parse(localStorage.getItem("settings") || "{}") || {}; } catch (e) {}
    SETTINGS.forEach(function (s) {
      if (isAction(s)) return;
      settings[s.k] = (saved[s.k] === undefined) ? s.def : clampSetting(s, saved[s.k]);
    });
  }

  function saveSettings() {
    try { localStorage.setItem("settings", JSON.stringify(settings)); } catch (e) {}
  }

  // A stored value can be out of range because the bounds changed under it, or
  // because someone edited localStorage. Clamping on read means a bad value
  // degrades to a usable one instead of breaking whatever reads it.
  function clampSetting(s, v) {
    if (s.type === "bool") return !!v;
    if (s.type === "text") return String(v === undefined || v === null ? "" : v);
    var n = parseFloat(v);
    if (isNaN(n)) return s.def;
    if (s.min !== undefined && n < s.min) n = s.min;
    if (s.max !== undefined && n > s.max) n = s.max;
    return s.type === "int" ? Math.round(n) : n;
  }

  // The accessor everything else uses.
  function S(k) { return settings[k]; }

  // Host-side settings go over their own message as key=value lines. Not JSON:
  // the host only ever BUILDS json, it has no parser, and a line format needs
  // no dependency to read. Values are scalars, so nothing is lost.
  //
  // Sent whole every time rather than as deltas -- the host then has no state
  // to get out of step, and a reload re-establishes everything for free.
  function pushHostSettings() {
    var lines = [];
    SETTINGS.forEach(function (s) {
      if (!s.host) return;
      var v = settings[s.k];
      if (s.type === "bool") v = v ? "1" : "0";
      // Newlines would break the line framing, and a flag string is the one
      // place a user could introduce one.
      lines.push(s.k + "=" + String(v).replace(/[\r\n]+/g, " "));
    });
    send("SETTINGS:" + lines.join("\n"));
  }

  function setSetting(k, v) {
    var d = settingDefs[k];
    if (!d || isAction(d)) return;   // an action has nothing to set
    settings[k] = clampSetting(d, v);
    saveSettings();
    applySettings();
  }

  // Anything that cannot simply be read at the point of use -- CSS, and the
  // handful of live structures that hold their own copy.
  function applySettings() {
    var r = document.documentElement.style;
    r.setProperty("--set-editor-font", S("editor.fontSize") + "px");
    // Only affects LITERAL tabs -- the Tab key inserts spaces. It still has to
    // track the setting, because the highlight layer and the textarea render
    // the same file and must agree on how wide one is.
    r.setProperty("--set-tab-size", String(S("editor.tabWidth")));
    r.setProperty("--set-tab-max", S("tabs.maxWidth") + "px");
    // A minimum above the maximum is a contradiction the CSS cannot resolve
    // sensibly, so it is folded here rather than left to the browser.
    r.setProperty("--set-tab-min",
      Math.min(S("tabs.minWidth"), S("tabs.maxWidth")) + "px");
    // One delay for both dividers. It lives under Console because that is where
    // the first one was; splitting it into two settings that would always hold
    // the same number is worse than the mild misfiling.
    r.setProperty("--set-split-delay", S("console.splitHoverMs") + "ms");
    applyConsoleHeight();
    applySidebarWidth();

    pushHostSettings();

    var mm = $("minimap");
    if (mm) {
      mm.hidden = !S("editor.minimap");
      // Turning it back on has to redraw: it was skipping draws while hidden,
      // so the canvas holds whatever was last visible -- or nothing at all.
      if (!mm.hidden) scheduleMinimap();
    }

    filters.info = S("console.showInfo");
    filters.ok   = S("console.showOk");
    filters.warn = S("console.showWarn");
    filters.err  = S("console.showErr");
    applyLogFilters();
    trimLog();
  }

  // ---- C++ syntax highlighting -------------------------------------------
  // The kit's syntax tokens were written for Luau; the mapping here is by role,
  // not by name. Note fx_* gets --syn-glob, the "known globals" colour: in this
  // app the fx API really is the global surface a snippet writes against.
  var KEYWORDS = ("alignas alignof auto break case catch class const constexpr continue " +
    "default delete do double else enum explicit extern false float for friend goto if " +
    "inline int long mutable namespace new nullptr operator private protected public " +
    "register return short signed sizeof static struct switch template this throw true " +
    "try typedef typename union unsigned using virtual void volatile while " +
    "int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t size_t uintptr_t " +
    "char bool float3 std string").split(" ");
  var KW = Object.create(null);
  KEYWORDS.forEach(function (k) { KW[k] = 1; });

  var TOK = new RegExp([
    "(\\/\\/[^\\n]*|\\/\\*[\\s\\S]*?\\*\\/)",          // 1 comment
    "(\"(?:\\\\.|[^\"\\\\])*\"|'(?:\\\\.|[^'\\\\])*')", // 2 string / char
    "(^[ \\t]*#[A-Za-z_]+)",                            // 3 preprocessor
    "(\\b0[xX][0-9a-fA-F]+\\b|\\b\\d[\\d.]*[fFuUlL]*\\b)", // 4 number
    "(\\bfx_[A-Za-z0-9_]+\\b)",                         // 5 fx API
    "([A-Za-z_][A-Za-z0-9_]*)",                         // 6 word
    "([{}()\\[\\];,.])",                                // 7 punctuation
    "([-+*/%=<>!&|^~?:]+)"                              // 8 operator
  ].join("|"), "gm");

  // Tokenise the whole buffer first so block comments spanning lines are
  // handled, then split into lines at token level.
  function tokenize(src) {
    var out = [], last = 0, m;
    TOK.lastIndex = 0;
    while ((m = TOK.exec(src)) !== null) {
      if (m.index > last) out.push({ t: src.slice(last, m.index), c: null });
      var cls = null;
      if      (m[1]) cls = "t-com";
      else if (m[2]) cls = "t-str";
      else if (m[3]) cls = "t-key";
      else if (m[4]) cls = "t-num";
      else if (m[5]) cls = "t-glob";
      else if (m[6]) {
        var w = m[6];
        if (KW[w]) cls = "t-key";
        else {
          // a word directly followed by "(" is a call
          var after = src.slice(m.index + w.length);
          cls = /^\s*\(/.test(after) ? "t-fn" : "t-var";
        }
      }
      else if (m[7]) cls = "t-punc";
      else if (m[8]) cls = "t-op";
      out.push({ t: m[0], c: cls });
      last = m.index + m[0].length;
    }
    if (last < src.length) out.push({ t: src.slice(last), c: null });
    return out;
  }

  // Tokens regrouped per line. Split out of highlight() so the minimap can
  // draw from the same pass -- tokenising a large file twice per keystroke,
  // once for the editor and once for the map, is the obvious way to make
  // typing feel heavy.
  function tokenLines(src) {
    var lines = [[]];
    tokenize(src).forEach(function (tok) {
      var parts = tok.t.split("\n");
      for (var i = 0; i < parts.length; i++) {
        if (i > 0) lines.push([]);
        if (parts[i]) lines[lines.length - 1].push({ t: parts[i], c: tok.c });
      }
    });
    return lines;
  }

  function highlight(src) {
    var lines = tokenLines(src);
    return lines.map(function (toks) {
      var h = toks.map(function (k) {
        return k.c ? '<span class="' + k.c + '">' + esc(k.t) + "</span>" : esc(k.t);
      }).join("");
      return '<span class="ln">' + (h || "&nbsp;") + "</span>";
    }).join("");
  }

  // ---- editor -------------------------------------------------------------
  var input = $("codeInput"), codeHi = $("codeHi"), gutter = $("gutter");

  // Replace the current selection while KEEPING the native undo stack.
  //
  // Assigning input.value wipes it: Chromium treats a scripted assignment as a
  // reset, not an edit, so Ctrl+Z afterwards undoes whatever came BEFORE and
  // the assignment itself is unrecoverable. That silently made replace-all a
  // one-way operation.
  //
  // execCommand is deprecated and has no replacement for this -- the
  // Undo Manager API was never shipped by anyone. It is the only way to put a
  // scripted edit onto the browser's own undo stack.
  function insertText(text) {
    input.focus();
    if (document.execCommand("insertText", false, text)) return;
    // Older engine, or the command refused. Correctness beats undo.
    var a = input.selectionStart, b = input.selectionEnd;
    // Assigning .value fires NO input event, so nothing else will record this.
    // Snapshot by hand, or this path silently edits the buffer with no way back
    // -- and leaves editPrev stale, which makes the NEXT undo restore a state
    // that never existed.
    snapshot();
    input.value = input.value.slice(0, a) + text + input.value.slice(b);
    input.selectionStart = input.selectionEnd = a + text.length;
    editPrev = { c: input.value, s: input.selectionStart, e: input.selectionEnd };
    lastInputAt = Date.now();
    repaint();
    refreshDirty();
  }

  // ---- find / replace -------------------------------------------------------
  // Match positions are absolute character offsets into the textarea's value,
  // which is the one coordinate system the textarea, the highlight layer and
  // the scroll maths all agree on.
  var findState = { ranges: [], index: -1, cs: false, re: false };

  function findOpen() { return !$("findBar").hidden; }

  function computeMatches(q) {
    findState.ranges = [];
    findState.index = -1;
    if (!q) return;

    var src = input.value, rx;
    try {
      // A literal search still goes through RegExp, so one code path handles
      // both -- escaping the query is cheaper than a second scanner.
      var pat = findState.re ? q : q.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
      rx = new RegExp(pat, findState.cs ? "g" : "gi");
    } catch (e) {
      return;            // half-typed regex; not an error worth shouting about
    }

    var m, guard = 0;
    while ((m = rx.exec(src)) !== null) {
      if (m[0].length === 0) { rx.lastIndex++; continue; }   // /x*/ matches empty
      findState.ranges.push({ a: m.index, b: m.index + m[0].length });
      if (++guard > 5000) break;    // a pathological query is not worth hanging on
    }
  }

  // Nearest match at or after the caret, so opening find while somewhere in
  // the file continues from there rather than jumping to the top.
  function nearestMatch(from) {
    for (var i = 0; i < findState.ranges.length; i++)
      if (findState.ranges[i].a >= from) return i;
    return findState.ranges.length ? 0 : -1;
  }

  function findPaint() {
    var n = findState.ranges.length;
    $("findCount").textContent = n ? (findState.index + 1) + "/" + n
                                   : ($("findInput").value ? "0/0" : "");
    $("findInput").classList.toggle("none", !!$("findInput").value && !n);
    repaint();                       // re-render with the marks
  }

  // Selecting the match is what makes Enter-then-type replace it, and gives
  // the native selection colour for free.
  function findGo(i) {
    var n = findState.ranges.length;
    if (!n) return;
    findState.index = ((i % n) + n) % n;       // wrap both ways
    var r = findState.ranges[findState.index];
    input.focus();
    input.setSelectionRange(r.a, r.b);
    scrollToOffset(r.a);
    findPaint();
    // Focus goes back to the box so you can keep pressing Enter.
    $("findInput").focus();
  }

  // A textarea cannot be told to scroll to a selection, so the line is counted
  // and converted through the line height.
  function scrollToOffset(off) {
    var line = input.value.slice(0, off).split("\n").length - 1;
    var lh = parseFloat(getComputedStyle(input).lineHeight) || 18;
    var want = line * lh - input.clientHeight / 2;
    input.scrollTop = Math.max(0, Math.min(want, input.scrollHeight - input.clientHeight));
    syncScroll();
  }

  function findRefresh(keepIndex) {
    var before = findState.index >= 0 ? findState.ranges[findState.index] : null;
    computeMatches($("findInput").value);
    if (!findState.ranges.length) { findPaint(); return; }
    findState.index = keepIndex && before ? nearestMatch(before.a)
                                          : nearestMatch(input.selectionStart);
    findPaint();
  }

  function openFind(withReplace) {
    var bar = $("findBar");
    bar.hidden = false;
    $("findReplaceRow").hidden = !withReplace;
    // A selection is almost always what you meant to search for.
    var sel = input.value.slice(input.selectionStart, input.selectionEnd);
    if (sel && sel.indexOf("\n") < 0) $("findInput").value = sel;
    findRefresh(false);
    $("findInput").focus();
    $("findInput").select();
  }

  function closeFind() {
    $("findBar").hidden = true;
    findState.ranges = [];
    findState.index = -1;
    repaint();                       // drop the marks
    input.focus();
  }

  // Re-point the find at whatever the editor is showing NOW.
  //
  // Ranges are character offsets into the buffer that produced them, so every
  // path that swaps input.value invalidates them: switching tabs, a watched
  // file reloading from disk, closing the last tab. Left alone they repaint the
  // previous file's positions onto the new text -- highlighting words that do
  // not contain the query at all, with an n/N counter describing a file you are
  // no longer looking at.
  //
  // Re-runs rather than closing the bar: carrying the query between files is
  // the useful behaviour -- it is how you chase one symbol through three
  // snippets -- and only the positions were ever wrong.
  function findResync() {
    if (findOpen()) { findRefresh(false); return; }
    if (!findState.ranges.length) return;
    findState.ranges = [];
    findState.index  = -1;
  }

  function replaceCurrent() {
    if (findState.index < 0 || !findState.ranges.length) return;
    var r = findState.ranges[findState.index];
    var rep = $("replInput").value;
    input.setSelectionRange(r.a, r.b);
    insertText(rep);                 // undoable
    var keep = findState.index;
    computeMatches($("findInput").value);
    // Stay where you were in the list rather than restarting, so repeated
    // Replace walks forward through the file.
    findState.index = findState.ranges.length
                    ? Math.min(keep, findState.ranges.length - 1) : -1;
    findPaint();
  }

  function replaceAll() {
    if (!findState.ranges.length) return;
    var n = findState.ranges.length, rep = $("replInput").value, src = input.value, out = "", at = 0;
    // Built in one pass from the ranges we already have. Repeatedly calling
    // replaceCurrent would rescan the whole file per hit, and a replacement
    // containing the query would match itself forever.
    findState.ranges.forEach(function (r) {
      out += src.slice(at, r.a) + rep;
      at = r.b;
    });
    out += src.slice(at);
    // Select everything and insert the finished text: one edit, so Ctrl+Z
    // takes the whole replace-all back in a single press rather than needing
    // one per occurrence.
    input.setSelectionRange(0, src.length);
    insertText(out);
    computeMatches($("findInput").value);
    findState.index = findState.ranges.length ? 0 : -1;
    findPaint();
    addLog("ok", "+ replaced " + n + " occurrence" + (n === 1 ? "" : "s"));
  }

  function goToLine() {
    var total = input.value.split("\n").length;
    askFor("Go to line (1-" + total + ")", "", function (v) {
      var n = parseInt(v, 10);
      if (isNaN(n)) return;
      n = Math.max(1, Math.min(total, n));
      var off = 0, lines = input.value.split("\n");
      for (var i = 0; i < n - 1; i++) off += lines[i].length + 1;
      input.focus();
      input.setSelectionRange(off, off + lines[n - 1].length);
      scrollToOffset(off);
    });
  }

  $("findInput").addEventListener("input", function () { findRefresh(false); });
  $("findInput").addEventListener("keydown", function (e) {
    e.stopPropagation();                       // app chords stay out of the box
    if (e.key === "Enter")       { e.preventDefault(); findGo(findState.index + (e.shiftKey ? -1 : 1)); }
    else if (e.key === "Escape") { e.preventDefault(); closeFind(); }
  });
  $("replInput").addEventListener("keydown", function (e) {
    e.stopPropagation();
    if (e.key === "Enter")       { e.preventDefault(); replaceCurrent(); }
    else if (e.key === "Escape") { e.preventDefault(); closeFind(); }
  });
  $("findNext").addEventListener("click", function () { findGo(findState.index + 1); });
  $("findPrev").addEventListener("click", function () { findGo(findState.index - 1); });
  $("findClose").addEventListener("click", closeFind);
  $("replOne").addEventListener("click", replaceCurrent);
  $("replAll").addEventListener("click", replaceAll);
  $("findCase").addEventListener("click", function () {
    findState.cs = !findState.cs;
    this.classList.toggle("on", findState.cs);
    findRefresh(true);
  });
  $("findRe").addEventListener("click", function () {
    findState.re = !findState.re;
    this.classList.toggle("on", findState.re);
    findRefresh(true);
  });

  // ---- minimap --------------------------------------------------------------
  // A scaled overview of the whole buffer: one thin band per line, with runs
  // of non-whitespace drawn as coloured marks. Leading whitespace is skipped
  // rather than drawn, which is what makes indentation legible -- the shape of
  // the nesting is most of what a minimap is actually for.
  //
  // Drawn from the same token pass the editor uses, so it costs a loop rather
  // than a second parse.
  var mmLines = [];          // token lines, set by repaint()
  var mmRAF = null;

  var MM_LINE_MAX = 3;       // px per line when the file is short enough
  var MM_CHAR_W   = 1;       // px per character

  // Token colours resolved once from the stylesheet. Reading them per draw
  // would be a getComputedStyle call per token, which is the expensive way to
  // learn a value that never changes.
  var mmColours = null;
  function minimapColours() {
    if (mmColours) return mmColours;
    var cs = getComputedStyle(document.documentElement);
    function v(n, fb) { return (cs.getPropertyValue(n) || "").trim() || fb; }
    mmColours = {
      "t-key":  v("--syn-key",  "#b58bf0"),
      "t-str":  v("--syn-str",  "#7fc99a"),
      "t-fn":   v("--syn-fn",   "#6fb2e8"),
      "t-num":  v("--syn-num",  "#d99a6c"),
      "t-glob": v("--syn-glob", "#e0a850"),
      "t-com":  v("--syn-com",  "#6a6d75"),
      "t-op":   v("--syn-op",   "#9aa0aa"),
      "t-punc": v("--syn-punc", "#7d838d"),
      "t-var":  v("--syn-var",  "#d2d4da"),
      "":       v("--fg-dim",   "#9aa0aa")
    };
    return mmColours;
  }

  // Coalesced to one draw per frame. Every keystroke calls repaint(), and
  // redrawing the whole map synchronously on each would tie typing latency to
  // file length.
  function scheduleMinimap() {
    if (!S("editor.minimap")) return;
    if (mmRAF !== null) return;
    mmRAF = requestAnimationFrame(function () {
      mmRAF = null;
      drawMinimap();
    });
  }

  function drawMinimap() {
    var box = $("minimap");
    if (!box || box.hidden) return;
    var cv = $("minimapCanvas");
    var w = box.clientWidth, h = box.clientHeight;
    if (!w || !h) return;

    var dpr = window.devicePixelRatio || 1;
    if (cv.width !== w * dpr || cv.height !== h * dpr) {
      cv.width = w * dpr; cv.height = h * dpr;
    }
    var g = cv.getContext("2d");
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, w, h);

    var n = mmLines.length;
    if (!n) { positionMinimapView(0); return; }

    // Fit the file if it fits; otherwise pin to 1px a line and let the MAP
    // scroll in proportion to the editor. Shrinking below a pixel would blur
    // every line into the same grey and stop being a map of anything.
    var lineH = Math.min(MM_LINE_MAX, h / n);
    var scrolls = false;
    if (lineH < 1) { lineH = 1; scrolls = true; }

    var offset = 0;
    if (scrolls) {
      var span = Math.max(1, input.scrollHeight - input.clientHeight);
      offset = (n * lineH - h) * (input.scrollTop / span);
    }

    var col = minimapColours();
    for (var i = 0; i < n; i++) {
      var y = i * lineH - offset;
      if (y + lineH < 0) continue;
      if (y > h) break;

      var x = 0;
      var toks = mmLines[i];
      for (var j = 0; j < toks.length; j++) {
        var t = toks[j].t;
        // Whitespace advances but draws nothing -- that gap IS the indent.
        var k = 0;
        while (k < t.length) {
          while (k < t.length && /\s/.test(t[k])) { k++; x += MM_CHAR_W; }
          var run = 0;
          while (k < t.length && !/\s/.test(t[k])) { k++; run++; }
          if (run) {
            g.fillStyle = col[toks[j].c] || col[""];
            // Bands are drawn slightly short of lineH so consecutive lines
            // stay distinguishable instead of merging into a block.
            g.fillRect(x, y, run * MM_CHAR_W, Math.max(1, lineH - 0.5));
            x += run * MM_CHAR_W;
          }
          if (x > w) break;
        }
        if (x > w) break;
      }
    }
    positionMinimapView(offset);
  }

  // Separate from drawMinimap so a scroll moves the box without redrawing the
  // canvas -- which is the common case by far.
  function positionMinimapView(offset) {
    var box = $("minimap"), view = $("minimapView");
    if (!box || box.hidden || !view) return;
    var h = box.clientHeight;
    var n = mmLines.length || 1;
    var lineH = Math.min(MM_LINE_MAX, h / n);
    if (lineH < 1) lineH = 1;

    var total = input.scrollHeight || 1;
    var top = (input.scrollTop / total) * (n * lineH) - (offset || 0);
    var hgt = (input.clientHeight / total) * (n * lineH);
    view.style.top = Math.max(0, top) + "px";
    view.style.height = Math.max(8, hgt) + "px";
  }

  // Click or drag anywhere on the strip to go there. The grab point is centred
  // on the viewport box, so the line under the cursor becomes the middle of
  // the screen rather than its top -- which is what it looks like it will do.
  function minimapScrollTo(clientY) {
    var box = $("minimap");
    var b = box.getBoundingClientRect();
    var frac = (clientY - b.top) / Math.max(1, b.height);
    var max = input.scrollHeight - input.clientHeight;
    input.scrollTop = Math.max(0, Math.min(max, frac * input.scrollHeight - input.clientHeight / 2));
    syncScroll();
  }

  (function () {
    var box = $("minimap");
    if (!box) return;
    var dragging = false;
    box.addEventListener("mousedown", function (e) {
      if (e.button !== 0) return;
      dragging = true;
      minimapScrollTo(e.clientY);
      e.preventDefault();
    });
    window.addEventListener("mousemove", function (e) {
      if (dragging) minimapScrollTo(e.clientY);
    });
    window.addEventListener("mouseup", function () { dragging = false; });

    // The strip's own height changes with the console panel and the window.
    window.addEventListener("resize", scheduleMinimap);
  })();

  // One token's text, with any search matches wrapped. `start` is the token's
  // absolute offset in the source, which is what lets a match found on the
  // whole string be placed inside a token that knows nothing about it.
  //
  // The mark goes INSIDE the syntax span rather than around it, so a match
  // keeps its token colour and only gains a background -- and a match that
  // straddles two tokens simply produces two marked pieces, which looks the
  // same and avoids generating overlapping tags.
  function markToken(text, start, ranges, curIdx) {
    if (!ranges.length) return esc(text);
    var end = start + text.length, out = "", at = 0;
    for (var i = 0; i < ranges.length; i++) {
      var r = ranges[i];
      if (r.b <= start || r.a >= end) continue;         // no overlap
      var a = Math.max(r.a, start) - start;
      var b = Math.min(r.b, end) - start;
      if (a > at) out += esc(text.slice(at, a));
      out += '<span class="mt' + (i === curIdx ? " cur" : "") + '">' +
             esc(text.slice(a, b)) + "</span>";
      at = b;
    }
    out += esc(text.slice(at));
    return out;
  }

  function repaint() {
    var src = input.value;
    // One tokenise, two consumers.
    var lines = tokenLines(src);

    // Absolute offset as the lines are walked, so each token knows where it
    // sits in the source and can be tested against the match ranges.
    var pos = 0;
    var ranges = findState.ranges, cur = findState.index;
    codeHi.innerHTML = lines.map(function (toks) {
      var h = toks.map(function (k) {
        var body = markToken(k.t, pos, ranges, cur);
        var here = pos;
        pos += k.t.length;
        // Every bracket gets an addressable span, ONCE per repaint. Matching
        // then costs a querySelector on caret movement instead of re-tokenising
        // the file -- which at 5000 lines is the difference between a highlight
        // and a stutter on every arrow key.
        if (k.t.length === 1 && "{}[]()".indexOf(k.t) >= 0)
          body = '<span class="bk" data-b="' + here + '">' + body + "</span>";
        return k.c ? '<span class="' + k.c + '">' + body + "</span>" : body;
      }).join("");
      pos += 1;                                          // the newline
      return '<span class="ln">' + (h || "&nbsp;") + "</span>";
    }).join("");

    var n = lines.length;
    var g = "";
    for (var i = 1; i <= n; i++) g += "<div>" + i + "</div>";
    gutter.innerHTML = g;
    mmLines = lines;
    scheduleMinimap();
    syncScroll();
    updateEmptyState();
    // NOT markDirty(true). repaint() only re-renders; it is called on load, on
    // tab switch, and when reloading a file changed on disk -- none of which are
    // edits. Marking dirty here made the startup repaint flag an EMPTY editor as
    // unsaved, and the snippet list then refused to load over it ("never
    // clobber a dirty buffer"), leaving a blank editor for a file with content.
    // Only typing means dirty, so only the input handler says so.
  }

  // Shown only with no file open AND nothing typed, so it gets out of the way
  // the moment you paste into the empty buffer rather than sitting behind your
  // code. Guarded on the previous value because repaint() runs per keystroke
  // and this would otherwise touch the DOM on every one of them.
  var emptyShown = null;
  function updateEmptyState() {
    var show = !currentName && !input.value;
    if (show === emptyShown) return;
    emptyShown = show;
    $("editorEmpty").hidden = !show;
  }

  function syncScroll() {
    codeHi.scrollTop  = input.scrollTop;
    codeHi.scrollLeft = input.scrollLeft;
    gutter.scrollTop  = input.scrollTop;

    // Just the box, not the canvas -- scrolling does not change what the map
    // shows. The exception is a file long enough that the map itself scrolls,
    // where the content really does have to move with it.
    positionMinimapView(0);
    if (mmLines.length * MM_LINE_MAX > $("minimap").clientHeight) scheduleMinimap();
  }

  // Typing is the ONLY thing that makes a buffer dirty. Everything else that
  // repaints -- load, tab switch, disk reload -- sets the flag itself.
  // ---- undo / redo, PER TAB -------------------------------------------------
  //
  // The browser's own undo stack belongs to the <textarea>, and there is only
  // one of those -- switching tabs just reassigns .value. So the history from
  // the file you were editing a minute ago stayed live, and Ctrl+Z in one
  // snippet pasted fragments of another into it. Worse, every programmatic
  // .value assignment (tab switch, the insertText fallback) desynced the native
  // stack from the buffer, so undo could splice a stale snapshot into current
  // text and produce the interleaved mess that ctrl+a, backspace, paste, undo
  // reproduces.
  //
  // Neither is fixable while the history lives on the element: there is no API
  // to clear it. So the stacks live on the TAB, and Ctrl+Z is intercepted so the
  // native one is never consulted at all.
  var editPrev   = { c: "", s: 0, e: 0 };   // buffer as of the last snapshot
  var lastInputAt = 0;
  var UNDO_MAX   = 300;
  var BURST_MS   = 400;                     // typing closer than this is one step

  function curTab() {
    var t = tabIndex(currentName);
    return t < 0 ? null : tabs[t];
  }
  // Tracing for the undo machinery. OFF, but kept: this bug was mis-diagnosed
  // three times from screenshots and only fell to a log line reading
  // "focus=snippetList". If undo ever misbehaves again, flip this and the next
  // reproduction will say what actually happened instead of what it looked like.
  var DBG_UNDO = false;
  function dbg(what, extra) {
    if (!DBG_UNDO) return;
    var tab = curTab();
    addLog("info", "undo/" + what +
      " tab=" + (currentName || "-") +
      " buf=" + input.value.length +
      " prev=" + editPrev.c.length +
      " un=" + ((tab && tab.un) ? tab.un.length : "-") +
      " re=" + ((tab && tab.re) ? tab.re.length : "-") +
      (extra ? " " + extra : ""));
  }

  function snapshot() {
    var tab = curTab();
    // Logged BEFORE the bail-out, on purpose. The first attempt logged after
    // it, so a snapshot that never happened produced no line at all and the
    // diagnostic was blind to the one failure it existed to catch.
    if (DBG_UNDO)
      addLog("info", "undo/snap name=" + (currentName || "-") +
                     " idx=" + tabIndex(currentName) +
                     " ntabs=" + (tabs ? tabs.length : "?") +
                     " buf=" + input.value.length +
                     " prev=" + editPrev.c.length);
    if (!tab) return;
    if (!tab.un) tab.un = [];
    // NEVER record "the file was empty" as the oldest state of a file that was
    // not. editPrev starts empty, so any buffer loaded by a path that forgot to
    // call undoReset left that stale marker in place -- and undoing to the
    // bottom of the stack then wiped the snippet. A real "you deleted
    // everything" edit cannot look like this: its previous state is the text
    // you deleted, not "".
    if (!tab.un.length && editPrev.c === "" && input.value !== "") {
      if (DBG_UNDO) addLog("warn", "undo/skip bogus empty base state");
      return;
    }
    tab.un.push({ c: editPrev.c, s: editPrev.s, e: editPrev.e });
    if (tab.un.length > UNDO_MAX) tab.un.shift();
    tab.re = [];                            // a fresh edit forks the future
  }
  // Called after a tab switch or a load, so the first edit in the new buffer
  // snapshots the new text rather than whatever was there before.
  function undoReset() {
    editPrev = { c: input.value, s: input.selectionStart, e: input.selectionEnd };
    lastInputAt = 0;
    dbg("reset");
  }
  function applySnap(sn) {
    dbg("apply", "to=" + sn.c.length);
    input.value = sn.c;
    try { input.setSelectionRange(sn.s, sn.e); } catch (err) {}
    editPrev = { c: sn.c, s: sn.s, e: sn.e };
    lastInputAt = 0;                        // next keystroke starts a new burst
    if (findOpen()) computeMatches($("findInput").value);
    repaint();
    // Undoing back to what is on disk is CLEAN, not dirty. Marking it dirty
    // regardless left a dot on a tab whose content matched the file exactly --
    // "ABC" -> "ABD" -> undo -> "ABC" still looked unsaved.
    refreshDirty();
    if (findOpen()) findPaint();
  }
  function doUndo() {
    var tab = curTab();
    dbg("undo-key");
    if (!tab || !tab.un || !tab.un.length) { dbg("undo-empty"); return; }
    if (!tab.re) tab.re = [];
    tab.re.push({ c: input.value, s: input.selectionStart, e: input.selectionEnd });
    applySnap(tab.un.pop());
  }
  function doRedo() {
    var tab = curTab();
    dbg("redo-key");
    if (!tab || !tab.re || !tab.re.length) { dbg("redo-empty"); return; }
    tab.un.push({ c: input.value, s: input.selectionStart, e: input.selectionEnd });
    applySnap(tab.re.pop());
  }

  input.addEventListener("input", function () {
    // Snapshot the state BEFORE this edit, but only when it opens a new burst.
    // Taking one per keystroke would make Ctrl+Z walk back a character at a
    // time; taking one per burst undoes the word or paste you just did.
    var now = Date.now();
    if (now - lastInputAt > BURST_MS) snapshot();
    lastInputAt = now;
    editPrev = { c: input.value, s: input.selectionStart, e: input.selectionEnd };

    // Match ranges are absolute offsets into the value that just changed, so
    // they have to be recomputed before repaint() tries to mark with them --
    // stale ranges would highlight whatever now sits at those offsets.
    if (findOpen()) computeMatches($("findInput").value);
    repaint();
    scheduleBrackets();      // the spans were just rebuilt; re-mark the pair
    refreshDirty();
    if (findOpen()) findPaint();
  });
  // The SELECTION as it was before the edit. editPrev.c is captured after each
  // input event, which is right for the text, but its selection is then the
  // one AFTER that edit. Selecting a block with the mouse fires no input event
  // at all, so undoing a delete restored the text with the caret collapsed
  // rather than the block re-selected. beforeinput fires with the selection
  // still intact.
  input.addEventListener("beforeinput", function () {
    editPrev.s = input.selectionStart;
    editPrev.e = input.selectionEnd;
  });

  // ---- matching bracket ----------------------------------------------------
  // Cheap by construction. repaint() has already wrapped every bracket in a
  // span carrying its offset, so this never re-tokenises: it finds the pair by
  // scanning the raw text and then toggles a class on two elements.
  //
  // Unbalanced brackets simply do not highlight. That is deliberate and is
  // most of the value -- putting the caret on a brace and seeing nothing light
  // up is the fastest way to find the one you failed to close.
  var brkA = -1, brkB = -1;

  function findPartner(v, i) {
    var open = "{[(", close = "}])";
    var ch = v.charAt(i);
    var oi = open.indexOf(ch), ci = close.indexOf(ch);
    var dir, want, self;
    if (oi >= 0)      { dir = 1;  self = ch; want = close.charAt(oi); }
    else if (ci >= 0) { dir = -1; self = ch; want = open.charAt(ci); }
    else return -1;

    // Bounded. A runaway scan over a huge file on every keystroke is worse than
    // no highlight, and a partner further than this is not something you are
    // reading anyway.
    var LIMIT = 20000;
    var depth = 0;
    for (var k = i, n = 0; n < LIMIT; k += dir, n++) {
      if (k < 0 || k >= v.length) break;
      var c = v.charAt(k);
      if (c === self) depth++;
      else if (c === want) { depth--; if (!depth) return k; }
    }
    return -1;
  }

  function updateBrackets() {
    var a = -1, b = -1;
    if (S("editor.matchBrackets") && document.activeElement === input &&
        input.selectionStart === input.selectionEnd) {
      var v = input.value, at = input.selectionStart;
      // The bracket after the caret wins, then the one before it -- the same
      // order editors use, so the highlight follows the caret rather than
      // jumping ahead of it.
      var cand = -1;
      if (at < v.length && "{}[]()".indexOf(v.charAt(at)) >= 0) cand = at;
      else if (at > 0 && "{}[]()".indexOf(v.charAt(at - 1)) >= 0) cand = at - 1;
      if (cand >= 0) {
        var p = findPartner(v, cand);
        if (p >= 0) { a = cand; b = p; }
      }
    }
    if (a === brkA && b === brkB) return;      // nothing moved; touch no DOM

    var old1 = codeHi.querySelector(".bk.on");
    while (old1) { old1.classList.remove("on"); old1 = codeHi.querySelector(".bk.on"); }
    brkA = a; brkB = b;
    if (a < 0) return;
    var e1 = codeHi.querySelector('.bk[data-b="' + a + '"]');
    var e2 = codeHi.querySelector('.bk[data-b="' + b + '"]');
    if (e1) e1.classList.add("on");
    if (e2) e2.classList.add("on");
  }

  // Caret moves do not fire `input`, so this rides on the events that actually
  // move it. Debounced: holding an arrow key must not do a DOM sweep per repeat.
  // selectionchange fires on EVERY caret move, including the auto-repeat of a
  // held arrow key -- keyup only fires when you let go, which is what made the
  // highlight lag behind the caret. Coalesced with rAF so a burst of moves
  // costs one update at frame rate rather than a timer each.
  var brkPending = false;
  function scheduleBrackets() {
    if (brkPending) return;
    brkPending = true;
    requestAnimationFrame(function () { brkPending = false; updateBrackets(); });
  }
  document.addEventListener("selectionchange", function () {
    if (document.activeElement === input) scheduleBrackets();
  });
  input.addEventListener("click", scheduleBrackets);
  input.addEventListener("blur", function () { brkA = brkB = -2; updateBrackets(); });
  input.addEventListener("scroll", syncScroll);

  // An image or a file on the clipboard has no text/plain, so the browser
  // quietly inserts nothing and the keystroke looks broken. Say what happened
  // instead -- doing nothing silently is the one outcome with no explanation.
  input.addEventListener("paste", function (e) {
    var dt = e.clipboardData;
    if (!dt) return;
    if (dt.getData("text/plain")) return;        // ordinary paste, carry on

    e.preventDefault();
    var what = "";
    if (dt.files && dt.files.length) what = dt.files[0].type || "a file";
    else if (dt.types && dt.types.length) what = String(dt.types.join(", "));
    var msg = "nothing to paste - the clipboard holds " +
              (what || "no text") + ", and this is a text editor";
    flash(msg);
    addLog("warn", msg);
  });

  // Tab inserts a tab rather than moving focus - this is a code editor.
  // UNDO IS CAUGHT ON THE DOCUMENT, IN THE CAPTURE PHASE.
  //
  // A listener on the textarea was not enough: measured, Ctrl+Z re-inserted 191
  // characters of another file into this one while no handler of mine ran at
  // all. Capture on the document sees the key before any bubbling listener and
  // before the editing command, whatever happens to be focused.
  //
  // stopPropagation as well as preventDefault, because Chromium's textarea undo
  // is COMMAND based, not snapshot based -- it replays "delete range / insert
  // text" against whatever buffer is present now. After a tab switch that is a
  // different file, which is exactly how one snippet's text ended up spliced
  // mid-line into another. Leaving it even partly enabled is not a fallback,
  // it is the corruption.
  document.addEventListener("keydown", function (e) {
    if (!(e.ctrlKey || e.metaKey) || e.altKey) return;
    var k = (e.key || "").toLowerCase();
    if (k !== "z" && k !== "y") return;

    // STAND ASIDE ONLY FOR ANOTHER REAL TEXT FIELD.
    //
    // The first version required focus to be ON the textarea, which was exactly
    // wrong: you switch tabs by clicking the sidebar, so focus sits on
    // snippetList and the guard handed Ctrl+Z back to the browser -- which ran
    // its own undo against the textarea regardless of a non-editable having
    // focus, splicing 191 characters of another file in. Measured, not guessed.
    //
    // So anything that is not itself an editable -- the sidebar, the tab strip,
    // the body -- means "the editor is what you meant", and we handle it. Only
    // the find box, a settings input or the palette keeps native undo, because
    // there the browser's history is the correct one.
    // SWALLOWED EVERYWHERE, including in other text fields, and that is not
    // over-reach. insertText() uses execCommand, which by design pushes onto
    // the BROWSER's stack -- so it still holds "replace all" from whatever file
    // you did it in. Letting the browser run undo anywhere replays that command
    // against whichever textarea exists now: measured, Ctrl+Z with focus in the
    // find box pasted the whole of tab 1 into tab 2.
    //
    // The stack cannot be cleared, so the only safe move is to never let it
    // run. The cost is no undo inside the find box and the settings fields --
    // single-line inputs you can retype -- against files being spliced
    // together, which is not a trade worth thinking about.
    e.preventDefault();
    e.stopPropagation();

    // The find and replace boxes count as the editor for this purpose. They
    // have no undo of their own now anyway, and after a Replace All the focus
    // is sitting in one of them -- Ctrl+Z there plainly means "take that
    // replace back", which is the case that started this.
    //
    // NOT extended to every field. A settings input or the palette is a
    // different surface, often over the top of the code; quietly rewriting the
    // file behind a dialog because you pressed Ctrl+Z in a number box is a
    // worse surprise than the key doing nothing.
    var ae = document.activeElement;
    var editorish = !ae || ae === input ||
                    ae.id === "findInput" || ae.id === "replInput";
    if (!editorish) return;              // swallowed, and nothing else happens

    if (k === "y" || e.shiftKey) doRedo(); else doUndo();
  }, true);

  input.addEventListener("keydown", function (e) {
    // ---- line-level editing helpers ---------------------------------------
    // Shared by everything below. Offsets are into the CURRENT value, so a
    // caller must not hold one across an edit.
    function lineBounds(v, pos) {
      var a = v.lastIndexOf("\n", pos - 1) + 1;
      var b = v.indexOf("\n", pos);
      return { a: a, b: b < 0 ? v.length : b };
    }
    // Whole lines the selection touches. A selection ending exactly at a line
    // START does not include that line -- otherwise dragging from one line to
    // the beginning of the next silently operates on two.
    function selectedLines(v, sa, sb) {
      var a = v.lastIndexOf("\n", sa - 1) + 1;
      if (sb > sa && v.charAt(sb - 1) === "\n") sb--;
      var b = v.indexOf("\n", sb);
      return { a: a, b: b < 0 ? v.length : b };
    }
    function indentUnit() { return new Array(S("editor.tabWidth") + 1).join(" "); }
    // Replace a range and reselect, as ONE undo step. Selecting before
    // insertText is what makes execCommand treat it as a replacement rather
    // than an insertion.
    function replaceRange(a, b, text, selA, selB) {
      input.setSelectionRange(a, b);
      insertText(text);
      if (selA !== undefined) {
        input.setSelectionRange(selA, selB === undefined ? selA : selB);
      }
    }

    // ---- Alt+Up / Alt+Down: move the line, or the selected lines ------------
    if (S("editor.lineTools") && e.altKey && !e.ctrlKey &&
        (e.key === "ArrowUp" || e.key === "ArrowDown")) {
      var up = e.key === "ArrowUp";
      var v0 = input.value, sa0 = input.selectionStart, sb0 = input.selectionEnd;
      var blk = selectedLines(v0, sa0, sb0);
      var body = v0.slice(blk.a, blk.b);
      e.preventDefault();

      if (up && blk.a > 0) {
        var prev = lineBounds(v0, blk.a - 1);
        var prevText = v0.slice(prev.a, prev.b);
        // The block and its neighbour swap wholesale, so the caret keeps its
        // offset WITHIN the moved text instead of landing wherever the other
        // line happens to end.
        var shiftUp = -(prevText.length + 1);
        replaceRange(prev.a, blk.b, body + "\n" + prevText,
                     sa0 + shiftUp, sb0 + shiftUp);
        repaint();
      } else if (!up && blk.b < v0.length) {
        var next = lineBounds(v0, blk.b + 1);
        var nextText = v0.slice(next.a, next.b);
        var shiftDn = nextText.length + 1;
        replaceRange(blk.a, next.b, nextText + "\n" + body,
                     sa0 + shiftDn, sb0 + shiftDn);
        repaint();
      }
      return;                      // at the first/last line: consumed, no move
    }

    // ---- Ctrl+D: duplicate the line, or the selection -----------------------
    if (S("editor.lineTools") && (e.ctrlKey || e.metaKey) && !e.altKey &&
        String(e.key).toLowerCase() === "d") {
      e.preventDefault();
      var vd = input.value, da = input.selectionStart, db = input.selectionEnd;
      if (da !== db) {
        var seld = vd.slice(da, db);
        replaceRange(da, db, seld + seld, db, db + seld.length);
      } else {
        var lb = lineBounds(vd, da);
        var lineText = vd.slice(lb.a, lb.b);
        replaceRange(lb.b, lb.b, "\n" + lineText, da + lineText.length + 1);
      }
      repaint();
      return;
    }

    // ---- Ctrl+/: toggle // on every touched line ----------------------------
    // Uncomments only when EVERY non-blank line is already commented. A mixed
    // selection comments, because that is the direction that cannot silently
    // delete a comment you meant to keep.
    if (S("editor.lineTools") && (e.ctrlKey || e.metaKey) && !e.altKey &&
        (e.key === "/" || e.code === "Slash")) {
      e.preventDefault();
      var vc = input.value, ca = input.selectionStart, cb = input.selectionEnd;
      var span = selectedLines(vc, ca, cb);
      var rows = vc.slice(span.a, span.b).split("\n");
      var real = rows.filter(function (r) { return r.trim() !== ""; });
      var allCommented = real.length > 0 && real.every(function (r) {
        return /^\s*\/\//.test(r);
      });
      var outRows = rows.map(function (r) {
        if (r.trim() === "") return r;
        if (allCommented) return r.replace(/^(\s*)\/\/ ?/, "$1");
        var lead = /^\s*/.exec(r)[0];
        return lead + "// " + r.slice(lead.length);
      });
      var joined = outRows.join("\n");
      replaceRange(span.a, span.b, joined, span.a, span.a + joined.length);
      repaint();
      return;
    }

    // ---- Home: first non-space, then column 0 -------------------------------
    if (S("editor.smartHome") && e.key === "Home" && !e.ctrlKey && !e.altKey) {
      var vh = input.value, ha = input.selectionStart;
      var hb = lineBounds(vh, ha);
      var firstNs = hb.a + /^[ \t]*/.exec(vh.slice(hb.a, hb.b))[0].length;
      var target = (ha === firstNs) ? hb.a : firstNs;
      e.preventDefault();
      if (e.shiftKey) {
        var anchor = input.selectionEnd;
        input.setSelectionRange(Math.min(target, anchor), Math.max(target, anchor));
      } else {
        input.setSelectionRange(target, target);
      }
      return;
    }

    // ---- } ] ) : pull a lone closing bracket back one level -----------------
    if (S("editor.dedentClose") &&
        (e.key === "}" || e.key === "]" || e.key === ")") &&
        !e.ctrlKey && !e.altKey && input.selectionStart === input.selectionEnd) {
      var vb2 = input.value, ba = input.selectionStart;
      var lb2 = lineBounds(vb2, ba);
      var beforeCaret = vb2.slice(lb2.a, ba);
      // Only when the line is nothing but indentation so far. Typing a closing
      // bracket after code must not reflow the line under you.
      if (/^[ \t]+$/.test(beforeCaret)) {
        var u = indentUnit();
        var cut = 0;
        if (beforeCaret.slice(-u.length) === u) cut = u.length;
        else if (beforeCaret.charAt(beforeCaret.length - 1) === "\t") cut = 1;
        if (cut) {
          e.preventDefault();
          replaceRange(ba - cut, ba, e.key);
          repaint();
          return;
        }
      }
    }

    // ---- Tab / Shift+Tab across the selection -------------------------------
    if (S("editor.blockIndent") && e.key === "Tab") {
      var vt = input.value, ta = input.selectionStart, tb = input.selectionEnd;
      if (ta !== tb || e.shiftKey) {
        e.preventDefault();
        var sp = selectedLines(vt, ta, tb);
        var u2 = indentUnit();
        var rows2 = vt.slice(sp.a, sp.b).split("\n");
        var delta0 = 0, deltaAll = 0;
        var res = rows2.map(function (r, i) {
          var d;
          if (e.shiftKey) {
            var strip = 0;
            if (r.slice(0, u2.length) === u2) strip = u2.length;
            else if (r.charAt(0) === "\t") strip = 1;
            d = -strip;
            r = r.slice(strip);
          } else {
            d = u2.length;
            r = u2 + r;
          }
          if (i === 0) delta0 = d;
          deltaAll += d;
          return r;
        });
        var joined2 = res.join("\n");
        replaceRange(sp.a, sp.b, joined2,
                     Math.max(sp.a, ta + delta0),
                     Math.max(sp.a, tb + deltaAll));
        repaint();
        return;
      }
    }

    // AUTO-INDENT. A new line starts where the current one started, plus a level
    // if the current one opens a block. Without it every line after an indented
    // one begins at column 0 and you retype the leading space by hand.
    //
    // Goes through insertText like Tab does, so it is one undoable step and
    // fires a real input event -- assigning .value would bypass both.
    if (S("editor.autoIndent") && e.key === "Enter" && !e.ctrlKey && !e.altKey) {
      var val = input.value, at = input.selectionStart;
      // Only when there is no selection: Enter over a selection REPLACES it,
      // and carrying indent into that is not what anyone means.
      if (at === input.selectionEnd) {
        var lineStart = val.lastIndexOf("\n", at - 1) + 1;
        var before    = val.slice(lineStart, at);
        var lead      = /^[ \t]*/.exec(before)[0];

        // One more level after a line that opens something. Tested against the
        // text BEFORE the caret, so pressing Enter mid-line does not indent on
        // the strength of a brace further along that you have not reached.
        var code  = before.replace(/\s+$/, "");
        var opens = /[{[(]$/.test(code);
        var unit  = new Array(S("editor.tabWidth") + 1).join(" ");

        e.preventDefault();
        insertText("\n" + lead + (opens ? unit : ""));
        return;
      }
    }

    if (e.key === "Tab") {
      e.preventDefault();
      // insertText, not a value assignment: the latter wipes the undo stack,
      // so Tab used to make everything before it unrecoverable. It also fires
      // a real input event, which is what repaints and flags dirty.
      insertText(new Array(S("editor.tabWidth") + 1).join(" "));
    }
  });

  // ---- snippets + tabs ----------------------------------------------------
  var snippets = [];      // [{n: name, code: string}]
  var current  = -1;      // index into `snippets` — valid only until the list is rebuilt
  var dirty    = false;

  // The open snippet is tracked by NAME, not by index. The host re-sends the
  // whole list after every save/rename/delete, and a delete shifts every index
  // after it. Relying on the index alone left the editor showing a deleted
  // file, and — worse — made save() write the buffer under whichever name had
  // slid into that slot.
  var currentName = null;
  var painted = false;      // has the host been told there is something to show
  var tabsRestored = false; // last session's open tabs are restored once

  // ---- sidebar selection --------------------------------------------------
  // SEPARATE from currentName, and the distinction is the whole model here:
  //   .active   the one file open in the editor
  //   .selected what a bulk action (delete, move, drag) would act on
  // A plain click does both. Ctrl/Shift-click only ever touch the selection,
  // so you can gather five files without the editor thrashing through five
  // tabs on the way.
  //
  // Held as NAMES for the same reason currentName is: the host re-sends the
  // whole list after every change and indices slide underneath us.
  var selected  = [];       // names
  var selAnchor = null;     // name a Shift range measures from
  var cursor    = null;     // name/path the keyboard is on; may lead the selection

  // What the sidebar last rendered, in visual order. Shift-ranges and the
  // arrow keys both need "what is above this row", and that is a property of
  // the TREE as drawn -- not of the snippets array, whose order says nothing
  // about nesting or collapse.
  var renderOrder = [];     // [{kind:"snippet"|"group", key, index}]

  function isSelected(n) { return selected.indexOf(n) >= 0; }

  function setSelection(names, anchor) {
    selected = names.slice();
    if (anchor !== undefined) selAnchor = anchor;
    paintSelection();
  }

  function toggleSelected(n) {
    var at = selected.indexOf(n);
    if (at >= 0) selected.splice(at, 1);
    else selected.push(n);
    selAnchor = n;
    paintSelection();
  }

  // Repaints the marks in place. Cheaper than renderSnippets() and, more to
  // the point, it does not rebuild the rows -- which would drop the drag the
  // user is in the middle of starting.
  function paintSelection() {
    var list = $("snippetList");
    Array.prototype.forEach.call(list.children, function (el) {
      var k = el.dataset ? el.dataset.key : null;
      if (!k) return;
      el.classList.toggle("selected", isSelected(k));
      el.classList.toggle("cursor", k === cursor);
    });
  }

  // Names between two rows in VISUAL order, inclusive, snippets only -- a
  // Shift range that swept up group headers would have no meaning, since a
  // header is not something the bulk actions can act on.
  function rangeBetween(fromKey, toKey) {
    var a = -1, b = -1, i;
    for (i = 0; i < renderOrder.length; i++) {
      if (renderOrder[i].key === fromKey) a = i;
      if (renderOrder[i].key === toKey)   b = i;
    }
    if (a < 0 || b < 0) return [toKey];
    if (a > b) { var t = a; a = b; b = t; }
    var out = [];
    for (i = a; i <= b; i++)
      if (renderOrder[i].kind === "snippet") out.push(renderOrder[i].key);
    return out;
  }

  // Walked rather than queried: a snippet name is arbitrary text and would
  // need escaping to survive an attribute selector.
  function rowFor(key) {
    var kids = $("snippetList").children;
    for (var i = 0; i < kids.length; i++) {
      if (kids[i].dataset && kids[i].dataset.key === key) return kids[i];
    }
    return null;
  }

  // Selection in the order it is DRAWN, not the order it was clicked in. A
  // multi-row move should land the files the way they looked, not the way
  // they were picked up.
  function orderedSelection() {
    var out = [];
    renderOrder.forEach(function (r) {
      if (r.kind === "snippet" && isSelected(r.key)) out.push(r.key);
    });
    // Anything selected but no longer drawn (a collapsed group closed over it)
    // still belongs to the drag.
    selected.forEach(function (n) { if (out.indexOf(n) < 0) out.push(n); });
    return out;
  }

  // Selection -> live indices, dropping anything the last refresh removed.
  // Sorted descending so a caller deleting by index does not invalidate the
  // entries it has not reached yet.
  function selectedIndices() {
    var out = [];
    selected.forEach(function (n) {
      var i = indexOfName(n);
      if (i >= 0) out.push(i);
    });
    out.sort(function (x, y) { return y - x; });
    return out;
  }

  // Is this file unsaved? The open one answers from `dirty`; any other from its
  // tab record. A file with no tab cannot be dirty -- there is nowhere for an
  // unsaved edit to it to live.
  function rowDirty(name) {
    if (name === currentName) return dirty;
    var t = tabIndex(name);
    return t >= 0 && !!tabs[t].dirty;
  }

  // Dirty means "differs from the file on disk", not "you touched it". Typing a
  // character and deleting it again leaves the buffer identical to the file, so
  // the dot should go out -- it was staying lit because every edit path just
  // asserted markDirty(true).
  //
  // A full string compare per keystroke sounds expensive and is not: it exits at
  // the first differing byte, which for a buffer you are editing is almost
  // immediately. Only the case that ends up EQUAL walks the whole string, and
  // that is the rare one.
  function refreshDirty() {
    var ki = (typeof indexOfName === "function" && currentName)
             ? indexOfName(currentName) : -1;
    var onDisk = (ki >= 0 && snippets[ki]) ? snippets[ki].code : null;
    markDirty(onDisk === null ? true : input.value !== onDisk);
  }

  function markDirty(d) {
    dirty = d;
    var tab = document.querySelector(".tabstrip .tab.active");
    if (tab) tab.classList.toggle("dirty", !!d);
    // The active row is updated in place so typing does not rebuild the list.
    // Every other row's dot is rendered from rowDirty() by renderSnippets().
    var row = document.querySelector(".script-row.active .dirty-dot");
    if (row) row.style.display = d ? "" : "none";
    renderToolbarFile();
  }

  // The toolbar's left-hand context line: which file the buttons beside it act
  // on. In place, from here and from renderTabs, for the same reason the dot
  // above is -- this runs on every keystroke and must not rebuild anything.
  function renderToolbarFile() {
    var el = $("tbFile");
    if (!el) return;
    el.innerHTML = "";
    if (!currentName) {
      // Not "no file": an empty buffer you paste into and Run without ever
      // saving is a supported way to work here, so the bar says what Run will
      // act on rather than implying nothing will happen.
      el.textContent = "untitled";
      el.title = "Unsaved buffer — Run works on it, Save will ask for a name";
      return;
    }
    el.appendChild(document.createTextNode(currentName));
    if (dirty) {
      var d = document.createElement("span");
      d.className = "dirty";
      d.textContent = " ●";
      el.appendChild(d);
    }
    el.title = currentName + (dirty ? "  ·  unsaved changes" : "");
  }

  // ---- grouping -----------------------------------------------------------
  // A group is a subdirectory of snippets/. The DIRECTORY decides membership;
  // .order only decides sequence. Exactly one level: a tree deep enough to need
  // navigation is a tree that should not be in a sidebar this size.
  // The last segment. indexOf would have returned everything after the FIRST
  // separator, which stops being the file name as soon as groups nest.
  function baseName(n) {
    var i = n.lastIndexOf("/");
    return i < 0 ? n : n.slice(i + 1);
  }

  var groupList = [];      // every group path the host reported, incl. empty ones
  var projectList = [];    // the subset of those holding a .project marker

  function isProject(g) { return projectList.indexOf(g) >= 0; }

  // ---- directory focus ------------------------------------------------------
  // Focus is a LENS, not a transform: it changes which node the tree is drawn
  // from, and nothing else. Every path in `snippets`, in a drag, and in every
  // send() stays a full relative path, so rename, move, run and the tabs never
  // learn that it exists. Making paths focus-relative instead would mean
  // un-relativising them at every send site, and one missed site moves a file
  // somewhere you did not ask for.
  //
  // Deliberately NOT scoped: tabs and Run stay global. A file open in a tab
  // outside the focus keeps working. Scoping them would turn a navigation aid
  // into a mode, and a mode that silently changes what F5 does is the kind of
  // thing you only get bitten by once before you stop trusting the button.
  var focus   = "";
  var navBack = [];
  var navFwd  = [];

  // Persisted for the same reason the collapse state is: it is where you were
  // working. Safe to restore only because the breadcrumb is always visible
  // while focused -- a partial tree with nothing explaining it would just read
  // as lost files.
  try { focus = localStorage.getItem("focus") || ""; } catch (e) {}
  function saveFocus() { try { localStorage.setItem("focus", focus); } catch (e) {} }

  // The project a path belongs to, or "" -- walking UP, so a file nested inside
  // a project's subfolder still belongs to it.
  function projectOwning(path) {
    var p = path;
    while (p) {
      if (isProject(p)) return p;
      p = parentOf(p);
    }
    return "";
  }

  // Exactly one .cpp on the end, however many the typing produced.
  //
  // askFor selects only the STEM and leaves the extension in place, which is the
  // right rename behaviour -- but it means anyone who types the whole filename
  // out of habit ends up with "thing.cpp.cpp". Stripping any run of them and
  // adding one back makes "thing", "thing.cpp" and "thing.cpp.cpp" all land in
  // the same place.
  function withCpp(name) {
    // Leave an explicit source extension alone. Forcing .cpp onto a header would
    // rename tracker.h to tracker.h.cpp the first time anyone touched it.
    if (/\.(h|hpp|inc)$/i.test(name)) return String(name);
    return String(name).replace(/(\.cpp)+$/i, "") + ".cpp";
  }

  // A header is editable but not runnable on its own.
  function isHeader(n) { return /\.(h|hpp|inc)$/i.test(n || ""); }

  // Collapse state is UI state, not project state, so it lives in the browser
  // rather than in .order -- it should not show up in a diff or follow the
  // snippets to another machine. Only plain groups have it; a project does not
  // unfold, so there is nothing to remember.
  function isCollapsed(g) {
    try { return localStorage.getItem("grp:" + g) === "1"; } catch (e) { return false; }
  }
  function setCollapsed(g, v) {
    try { localStorage.setItem("grp:" + g, v ? "1" : "0"); } catch (e) {}
  }

  // Bucket by group, keeping first-appearance order. Bucketing rather than
  // walking the flat list matters: .order can interleave groups, and rendering
  // a header every time the group changed would repeat the same header.
  // Build a real tree. Groups nest now, so a flat bucket list will not do: a
  // node holds its child groups AND its snippets, and rendering walks it.
  //
  // Every ancestor is materialised even if the host only reported the leaf, so
  // a group can never be orphaned by a missing parent.
  function buildTree() {
    var root = { path: "", kids: [], items: [] };
    var byPath = { "": root };

    function nodeFor(path) {
      if (byPath[path]) return byPath[path];
      var parent = nodeFor(parentOf(path));
      var node = { path: path, kids: [], items: [] };
      byPath[path] = node;
      parent.kids.push(node);
      return node;
    }

    // Order comes from the host, which puts parents before children -- so this
    // stays a single pass. nodeFor() would cope with a child arriving first by
    // creating the parent on demand, but the parent would then take the
    // child's position in the sidebar rather than its own.
    groupList.forEach(function (g) { nodeFor(g); });
    snippets.forEach(function (s, i) { nodeFor(s.g || "").items.push(i); });
    return root;
  }

  function parentOf(p) {
    var i = p.lastIndexOf("/");
    return i < 0 ? "" : p.slice(0, i);
  }

  // Walk to a node by path; null if any segment is missing. That null is how a
  // focus on a folder that has since been renamed, ungrouped or deleted gets
  // noticed -- the path itself stays perfectly well-formed, so nothing else
  // would have caught it.
  function nodeAt(root, path) {
    if (!path) return root;
    var parts = path.split("/"), cur = root, acc = "";
    for (var i = 0; i < parts.length; i++) {
      acc = acc ? acc + "/" + parts[i] : parts[i];
      var hit = null;
      for (var j = 0; j < cur.kids.length; j++)
        if (cur.kids[j].path === acc) { hit = cur.kids[j]; break; }
      if (!hit) return null;
      cur = hit;
    }
    return cur;
  }

  // A dead path falls back to its nearest surviving ancestor rather than to the
  // root: deleting one leaf folder should leave you standing next to where you
  // were, not at the top of the tree with your place lost.
  function nearestLive(p) {
    var tree = buildTree();
    while (p && !nodeAt(tree, p)) p = parentOf(p);
    return p;
  }

  // ---- drag and drop ------------------------------------------------------
  // A CUSTOM POINTER DRAG, not HTML5 drag-and-drop -- ported from vemacro
  // (web/index.html: refreshProfiles, getProfileDropIndex). The native model
  // computed the target by HIT-TESTING, and the 1px seams between rows
  // hit-test to nothing: no dragover fires there, the browser resets
  // dropEffect, and the rail blinked out with a block cursor no CSS rule can
  // override -- during a native drag the cursor is the browser's alone.
  // Unfixable from inside that model; docs/sidebar-drag.md has the history
  // and the six dead ends. Here the target is ARITHMETIC over row rects (see
  // updateDropTarget), mousemove/mouseup live on the document, and there is
  // no dataTransfer, so there is no cursor to lose.
  //
  // Two things can be dragged: snippets (dragNames, below) or a whole group
  // (dragGroup is its path). Cross-level drops are MOVES on disk, so dropping
  // is a rename -- which is why nothing here has to reason about .order.
  var dragGroup = null;

  // Snippets being dragged, in visual order. A single-row drag is just the
  // one-element case, so nothing below needs to special-case it.
  var dragNames = [];

  // Set just before a GROUP_RENAME goes out, consumed when the refreshed list
  // comes back. See followMove().
  var pendingMove = null;

  function groupOf(name) {
    var i = indexOfName(name);
    return i < 0 ? null : (snippets[i].g || "");
  }

  // Do ALL of the dragged files already live in `g`? Decides reorder-vs-move
  // when a row is the target. N files, not one: a `length <= 1` guard here
  // once made reorder refuse SILENTLY whenever two or more were selected --
  // same gesture, different outcome, invisible cause.
  function sameGroupAs(g) {
    return dragGroup === null && dragNames.length > 0 &&
           dragNames.every(function (n) { return groupOf(n) === g; });
  }

  // Would moving `path` under `into` be legal? A group cannot go inside itself
  // or its own descendants, and moving something to where it already is is a
  // no-op rather than an error.
  function canDropInto(into) {
    if (dragGroup !== null) {
      if (into === dragGroup) return false;
      if (into.indexOf(dragGroup + "/") === 0) return false;   // its own subtree
      return parentOf(dragGroup) !== into;
    }
    // Legal if ANY of them would actually move. A mixed selection where some
    // already live in `into` still drops; the ones already there are skipped
    // rather than the whole drag being refused.
    for (var k = 0; k < dragNames.length; k++) {
      var g = groupOf(dragNames[k]);
      if (g !== null && g !== into) return true;
    }
    return false;
  }

  function performDrop(into) {
    if (dragGroup !== null) {
      var to = (into ? into + "/" : "") + baseName(dragGroup);
      pendingMove = { from: dragGroup, to: to };
      send("GROUP_RENAME:" + dragGroup + "\n" + to);
    } else {
      // One rename per file. The host handles them in order, and each is
      // independently valid, so a name that collides in the destination fails
      // on its own rather than taking the rest of the move down with it.
      dragNames.forEach(function (n) {
        if (groupOf(n) === into) return;               // already there
        send("SNIP_RENAME:" + n + "\n" + (into ? into + "/" : "") + baseName(n));
      });
    }
    dragGroup = null; dragNames = [];
    clearDropMarks();
  }

  // ---- drag auto-scroll -----------------------------------------------------
  // Push the pointer against the pane's edge mid-drag and the list travels.
  // The wheel works during a drag too now -- the native drag loop that
  // swallowed it is gone -- but the edge scroll stays: one hand is holding
  // the drag.
  //
  // Driven by rAF off a stored Y rather than by mousemove, which stops firing
  // when the pointer is held still -- exactly what you do when you are
  // waiting for a list to scroll.
  var scrollRAF = null, scrollY = 0;


  // The scrolling element is .side-scroll, NOT #snippetList. #snippetList is
  // a content-sized flex column with no overflow of its own, so setting
  // scrollTop on it silently does nothing -- and the empty space below the
  // last row belongs to the pane, not the list, which is why a marquee
  // started down there never reached the list's handlers.
  function scrollPane() { return document.querySelector(".side-scroll"); }

  function autoScrollTick() {
    var list = scrollPane();
    if (!list) { scrollRAF = null; return; }
    var b = list.getBoundingClientRect();
    var step = 0;
    var EDGE = S("sidebar.dragEdge");
    if (scrollY < b.top + EDGE)         step = -(1 - (scrollY - b.top) / EDGE);
    else if (scrollY > b.bottom - EDGE) step =  (1 - (b.bottom - scrollY) / EDGE);
    if (step) {
      // Squared so it creeps near the boundary and only races at the very
      // edge; linear made fine positioning impossible.
      var mag = Math.min(1, Math.abs(step));
      list.scrollTop += (step < 0 ? -1 : 1) * mag * mag * S("sidebar.dragSpeed");
      // A rubber band has to be recomputed as the content moves: the cursor
      // can be stationary while rows slide past under it, and those rows
      // still have to fall into the sweep.
      if (typeof band !== "undefined" && band) updateBand();
    }
    scrollRAF = requestAnimationFrame(autoScrollTick);
  }

  function autoScrollAt(clientY) {
    scrollY = clientY;
    if (scrollRAF === null) scrollRAF = requestAnimationFrame(autoScrollTick);
  }

  function endAutoScroll() {
    if (scrollRAF !== null) cancelAnimationFrame(scrollRAF);
    scrollRAF = null;
  }

  // Recompute the target on SCROLL, not only on mousemove: the auto-scroll
  // above (and the wheel) slide rows under a stationary pointer, and the rail
  // has to follow the rows, not the pixel. Same pattern the rubber band uses
  // (updateBand from the tick); this one can hang off the scroll event
  // because everything it needs is in dragPt.
  scrollPane().addEventListener("scroll", function () {
    if (dragLive) updateDropTarget();
  });

  // ---- the pointer-drag engine ----------------------------------------------
  // The reason the 1px flicker cannot come back: the drop target is computed
  // from the pointer's Y by ARITHMETIC, never by hit-testing. Each row owns
  // the span from its own top to the NEXT row's top, so the flex gap between
  // rows belongs to the row above it and every Y maps to exactly one target.
  // A pixel in a seam is not a dead zone -- it is a number on one side of a
  // midpoint.
  var dragLive   = false;          // a press has crossed the 4px threshold
  var dragPt     = { x: 0, y: 0 }; // last pointer position, for recomputes
  var dropAction = null;           // what a release would do; null = nothing

  // The elements the banding walks, in visual order. group-empty rides along
  // so an empty group is still somewhere to drop -- which is mostly what you
  // make one for.
  function dropRows() {
    var out = [];
    Array.prototype.forEach.call($("snippetList").children, function (el) {
      if (el.classList.contains("script-row") ||
          el.classList.contains("group-head") ||
          el.classList.contains("group-empty")) out.push(el);
    });
    return out;
  }

  // Dim what is being carried, matched by NAME on every recompute rather than
  // by holding nodes: a spring-loaded expand rebuilds the list mid-drag, and
  // under the pointer model that is harmless -- the next paint lands on
  // whatever rows exist now. (Also why the HTML5-era "no dimming" rule could
  // be retired; see app.css.)
  function paintDragDim() {
    var keys = dragGroup !== null ? [dragGroup] : dragNames;
    Array.prototype.forEach.call($("snippetList").children, function (el) {
      var k = el.dataset ? el.dataset.key : null;
      el.classList.toggle("drag-src", !!k && keys.indexOf(k) >= 0);
    });
  }

  // Recomputes the target and the marks from dragPt. Called on every
  // mousemove, on scroll, and after a spring-loaded expand -- it reads only
  // state, so it can run as often as anything moves.
  function updateDropTarget() {
    dropAction = null;
    clearDropMarks();
    paintDragDim();
    var x = dragPt.x, y = dragPt.y;
    var springWant = null;   // set only by a header's into zone

    // One exit. springLoad re-arms only when the key changes, so calling it
    // every move is what keeps its timer running; any other outcome cancels,
    // matching the old dragleave.
    function done() {
      if (springWant) springLoad(springWant);
      else cancelSpring();
    }

    // Crumb segments first -- they sit above the list's banding, and each is
    // a move-into target. Not a nicety: the list background means "into the
    // focus", so the crumbs are the only way to drag something OUT of the
    // folder you are standing in.
    var crumbs = document.querySelectorAll("#crumbs .crumb");
    for (var ci = 0; ci < crumbs.length; ci++) {
      var cel = crumbs[ci], cr = cel.getBoundingClientRect();
      if (cel.dataset.dropInto === undefined) continue;
      if (x < cr.left || x > cr.right || y < cr.top || y > cr.bottom) continue;
      if (canDropInto(cel.dataset.dropInto)) {
        cel.classList.add("drop-into");
        dropAction = { kind: "into", path: cel.dataset.dropInto };
      }
      return done();
    }

    // Off the pane: no target, and a release is a no-op -- same as letting
    // the old drag go over dead space.
    var pane = scrollPane().getBoundingClientRect();
    if (x < pane.left || x > pane.right || y < pane.top || y > pane.bottom)
      return done();

    // The background target: "move into whatever is focused", root included.
    // Deliberately unpainted -- see the app.css note on the whole-list mark.
    function background() {
      if (canDropInto(focus)) dropAction = { kind: "into", path: focus };
      return done();
    }

    var rows = dropRows();
    if (!rows.length || y < rows[0].getBoundingClientRect().top)
      return background();

    // The banding walk. Row i owns [top(i), top(i+1)); the last row owns down
    // to its own bottom, and past that is the end-of-level gap.
    var el = null, rect = null;
    for (var i = 0; i < rows.length; i++) {
      var r = rows[i].getBoundingClientRect();
      var limit = i + 1 < rows.length
                  ? rows[i + 1].getBoundingClientRect().top : r.bottom;
      if (y < limit) { el = rows[i]; rect = r; break; }
    }

    // THE GAP BELOW THE LAST ROW: reorder to the end of this level, or a move
    // to it from elsewhere. Without it, dragging a file out of a group had no
    // visible answer to "where will this land", and a file already at this
    // level could not be reordered to the end (performDrop skips "already
    // there").
    if (!el) {
      if (dragGroup !== null || !dragNames.length) return background();
      // The rail sits on the last REAL row; a trailing empty-group
      // placeholder takes no paint and would swallow it.
      for (var q = rows.length - 1; q >= 0; q--) {
        if (!rows[q].classList.contains("group-empty")) {
          rows[q].classList.add("drop-after");
          break;
        }
      }
      if (sameGroupAs(focus || "")) dropAction = { kind: "end" };
      else if (canDropInto(focus))  dropAction = { kind: "into", path: focus };
      return done();
    }

    if (el.classList.contains("group-empty")) {
      var ep = el.dataset.dropInto;
      if (canDropInto(ep)) {
        el.classList.add("drop-into");
        dropAction = { kind: "into", path: ep };
      }
      return done();
    }

    if (el.classList.contains("group-head")) {
      var g = el.dataset.key;
      // A header is a three-zone target while a SIBLING group is in hand: the
      // outer zones place it before/after this one, the middle drops into it.
      // Only siblings reorder -- across levels the gesture is a MOVE, which
      // the middle zone already covers. A header has to mean both things, and
      // splitting it by height is how every tree view resolves that.
      if (dragGroup !== null && dragGroup !== g &&
          parentOf(dragGroup) === parentOf(g)) {
        var f = (y - rect.top) / rect.height;  // >1 in the gap below = after
        var z = S("sidebar.dropZone");
        if (f < z || f > 1 - z) {
          var ga = f > 1 - z;
          el.classList.add(ga ? "drop-after" : "drop-before");
          dropAction = { kind: "groupReorder", target: g, after: ga };
          return done();
        }
      }
      if (canDropInto(g)) {
        el.classList.add("drop-into");
        springWant = g;         // hover long enough and a collapsed one opens
        dropAction = { kind: "into", path: g };
      }
      return done();
    }

    // A script row. Same group: reorder against its midpoint. From another
    // level: a MOVE into this row's group -- position within the destination
    // is not expressed; the rail says where you aimed, and the host appends.
    // (Without the cross case, dragging a file out of a group onto a root row
    // showed no rail and did nothing, so only the gap at the bottom worked.)
    if (dragGroup !== null || !dragNames.length) return background();
    var key = el.dataset.key;
    var ki = indexOfName(key);
    if (ki < 0) return background();
    var after = y > rect.top + rect.height / 2;
    el.classList.add(after ? "drop-after" : "drop-before");
    if (sameGroupAs(snippets[ki].g || ""))
      dropAction = { kind: "reorder", key: key, after: after };
    else
      dropAction = { kind: "into", path: snippets[ki].g || "" };
    return done();
  }

  // Executes whatever updateDropTarget last decided. Indices are resolved
  // HERE, from names: the list may have re-rendered (a spring expand) or even
  // re-arrived from the host since the marks were painted, and a cached index
  // would move a file that slid into the slot.
  function performDropAction() {
    var a = dropAction;
    dropAction = null;
    if (!a) return;

    if (a.kind === "into") { performDrop(a.path); return; }

    if (a.kind === "groupReorder") { moveGroup(dragGroup, a.target, a.after); return; }

    if (a.kind === "reorder") {
      var to = indexOfName(a.key);
      if (to < 0) return;
      if (a.after) to++;
      if (dragNames.length > 1) {
        moveSnippets(orderedSelection(), to);   // owns its own index correction
      } else {
        var from = indexOfName(dragNames[0]);
        // Removing the dragged row first shifts everything after it up one,
        // so a downward move lands a slot too far without this.
        if (from >= 0 && from < to) to--;
        moveSnippet(from, to);
      }
      return;
    }

    if (a.kind === "end") {
      var here = focus || "";
      var endIdx = -1;
      for (var q = 0; q < snippets.length; q++)
        if ((snippets[q].g || "") === here) endIdx = q;
      if (endIdx < 0) return;
      if (dragNames.length > 1) moveSnippets(orderedSelection(), endIdx + 1);
      else {
        var from2 = indexOfName(dragNames[0]);
        if (from2 >= 0 && endIdx !== from2) moveSnippet(from2, endIdx);
      }
    }
  }

  // The ghost: a CLONE of the picked-up row, fixed under the cursor. A real
  // element rather than a browser snapshot, so it can carry a count badge and
  // is never stale; pointer-events:none in its CSS keeps it invisible to
  // everything here (which reads coordinates, not elements, anyway).
  function makeGhost(srcEl, count) {
    var g = srcEl.cloneNode(true);
    g.className = (srcEl.classList.contains("group-head")
                   ? "group-head" : "script-row") + " drag-clone";
    g.style.width = srcEl.offsetWidth + "px";
    if (count > 1) {
      var b = document.createElement("span");
      b.className = "drag-count";
      b.textContent = count;
      g.appendChild(b);
    }
    document.body.appendChild(g);
    return g;
  }

  // A drag that ends anywhere the click would land next would OPEN whatever
  // row the pointer stopped over. Eaten once, in capture; the timeout covers
  // a release that produces no click at all (off the window, or over an
  // element that vanished).
  var suppressClick = false;
  document.addEventListener("click", function (e) {
    if (!suppressClick) return;
    suppressClick = false;
    e.stopPropagation();
    e.preventDefault();
  }, true);

  // Arms a drag on a press. `begin` runs ONCE, at the 4px threshold, and
  // stocks dragNames/dragGroup; before that the press is just a press and a
  // release is just a click -- the threshold is the whole click-vs-drag
  // distinction. move/up bind to the DOCUMENT, the point of the entire port:
  // they fire whatever is under the pointer, so there is no enter/leave
  // bookkeeping to lose in a seam and no mouseup to have swallowed.
  function armDrag(e, srcEl, begin) {
    var sx = e.clientX, sy = e.clientY;
    var sr = srcEl.getBoundingClientRect();
    var offX = sx - sr.left, offY = sy - sr.top;
    var ghost = null;

    function onMove(ev) {
      if (!dragLive) {
        if (Math.abs(ev.clientX - sx) < 4 && Math.abs(ev.clientY - sy) < 4) return;
        dragLive = true;
        begin();
        ghost = makeGhost(srcEl, dragGroup !== null ? 1 : dragNames.length);
      }
      ghost.style.left = (ev.clientX - offX) + "px";
      ghost.style.top  = (ev.clientY - offY) + "px";
      dragPt.x = ev.clientX; dragPt.y = ev.clientY;
      // Auto-scroll only while the pointer is over the pane's width, as the
      // old model's dragover gating had it -- parking the pointer over the
      // editor must not creep the sidebar.
      var pr = scrollPane().getBoundingClientRect();
      if (ev.clientX >= pr.left && ev.clientX <= pr.right) autoScrollAt(ev.clientY);
      else endAutoScroll();
      updateDropTarget();
    }
    function teardown() {
      document.removeEventListener("mousemove", onMove);
      document.removeEventListener("mouseup", onUp);
      document.removeEventListener("keydown", onKey, true);
    }
    function finish(commit) {
      if (ghost && ghost.parentNode) ghost.parentNode.removeChild(ghost);
      ghost = null;
      if (commit) performDropAction();
      dropAction = null;
      dragLive = false;
      dragGroup = null; dragNames = [];
      clearDropMarks();
      paintDragDim();          // no keys left, so this clears every dim
      endAutoScroll();
      cancelSpring();
      suppressClick = true;
      setTimeout(function () { suppressClick = false; }, 0);
    }
    function onUp() {
      teardown();
      if (dragLive) finish(true);
    }
    // Escape cancels, as the native drag did. Capturing, so the list's own
    // Escape (clear selection) does not also fire on the same press.
    function onKey(ev) {
      if (ev.key !== "Escape" || !dragLive) return;
      ev.preventDefault();
      ev.stopPropagation();
      teardown();
      finish(false);
    }
    document.addEventListener("mousemove", onMove);
    document.addEventListener("mouseup", onUp);
    document.addEventListener("keydown", onKey, true);
  }

  // ---- spring-loaded groups -------------------------------------------------
  // Hover a collapsed group mid-drag and it opens, so a nested target can be
  // reached without putting the drag down first. Dropping straight onto the
  // header already worked; this is for aiming at something INSIDE it.
  //
  // Re-arming only when the target changes is what makes it usable:
  // updateDropTarget runs on every mousemove, and restarting the timer on
  // each one would mean it never elapsed.
  var springKey = null, springTimer = null;

  function cancelSpring() {
    if (springTimer) clearTimeout(springTimer);
    springTimer = null;
    springKey = null;
  }

  function springLoad(g) {
    if (!S("sidebar.springEnabled")) return;
    if (!g || g === springKey) return;      // already counting down on this one
    cancelSpring();
    // A project does not unfold -- it is a place you go -- so there is nothing
    // for a spring to open.
    if (isProject(g) || !isCollapsed(g)) return;
    springKey = g;
    springTimer = setTimeout(function () {
      springTimer = null;
      // Re-checked on fire: the drag may have ended or moved on during the
      // wait, and expanding then would be a group opening on its own.
      if (springKey !== g) return;
      if (dragGroup === null && !dragNames.length) return;
      setCollapsed(g, false);
      renderSnippets();
      // The pointer may be dead still -- nothing else would repaint the
      // marks onto the freshly built rows.
      if (dragLive) updateDropTarget();
    }, S("sidebar.springMs"));
  }

  function snippetRow(s, i, depth) {
    var row = document.createElement("div");
    row.className = "script-row" + (i === current ? " active" : "") +
                    (isSelected(s.n) ? " selected" : "") +
                    (s.n === cursor ? " cursor" : "") +
                    (isHeader(s.n) ? " is-header" : "");
    row.dataset.key = s.n;          // paintSelection() finds rows by this
    row.style.paddingLeft = (12 + (depth || 0) * 16) + "px";
    row.innerHTML =
      '<span class="script-ico">' +
        '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" ' +
        'stroke-linecap="round" stroke-linejoin="round">' +
        '<path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/>' +
        '<path d="M14 2v6h6"/></svg></span>' +
      '<span class="script-meta">' +
        '<span class="script-name">' + esc(baseName(s.n)) + "</span>" +
        '<span class="script-sub">' + s.code.split("\n").length + " lines</span>" +
      "</span>" +
      // Rendered from the actual state, not hardcoded hidden.
      //
      // It used to always start hidden and rely on markDirty() switching it on
      // afterwards -- so every renderSnippets() silently wiped it while the tab
      // strip, which derives its own dot properly, kept showing dirty. The
      // sidebar dot vanishing looked exactly like the file had been saved.
      //
      // Same rule renderTabs uses: the ACTIVE file's state lives in `dirty`,
      // because the tab record is only updated on stash and would otherwise
      // report the state as of the last switch rather than this keystroke.
      '<span class="dirty-dot" style="display:' +
        (rowDirty(s.n) ? "" : "none") + '"></span>';
    // Selection lands on MOUSEDOWN, not on click.
    //
    // The row is draggable, and Chromium starts a drag after a few pixels of
    // movement with the button held -- at which point no click event is ever
    // delivered. A slightly shaky press therefore selected nothing at all,
    // which is the "sometimes it does not select the one I clicked" case.
    // mousedown always arrives, drag or not.
    //
    // It also makes the highlight move under your finger rather than after the
    // work: opening re-tokenises the whole file and rebuilds the sidebar, and
    // on a 900-line snippet that is long enough to feel like lag if nothing
    // has acknowledged the press yet.
    //
    // Only the SELECTION moves here. Opening stays on click, so dragging a
    // file does not haul the editor along to it.
    row.addEventListener("mousedown", function (e) {
      if (e.button !== 0) return;                        // right-click has its own path
      if (e.ctrlKey || e.metaKey || e.shiftKey) return;  // the click handler owns those
      // Pressing inside an existing multi-selection leaves it alone, so a drag
      // of several files still carries all of them. The `|| selected.length > 1`
      // that used to be here did the exact opposite of this comment: it
      // collapsed the selection to the pressed row, so dragging four snippets
      // into a group moved one and silently dropped the rest.
      if (!isSelected(s.n)) setSelection([s.n], s.n);
      cursor = s.n;
      paintSelection();
      // The same press is the PICKUP. Explorer's rule is the setSelection
      // just above -- pressing outside the selection replaced it, so the drag
      // that may follow carries exactly what is highlighted, all of it.
      armDrag(e, row, function () {
        dragGroup = null;
        dragNames = orderedSelection();
      });
    });

    // Ctrl and Shift only ever move the SELECTION -- neither opens the file.
    // Gathering several to delete or drag should not drag the editor through
    // a tab per click on the way.
    row.addEventListener("click", function (e) {
      cursor = s.n;
      if (e.shiftKey && selAnchor) {
        setSelection(rangeBetween(selAnchor, s.n));
        return;
      }
      if (e.ctrlKey || e.metaKey) {
        toggleSelected(s.n);
        return;
      }
      setSelection([s.n], s.n);
      openSnippet(i);
    });
    row.addEventListener("contextmenu", function (e) {
      e.preventDefault();
      e.stopPropagation();
      cursor = s.n;
      // Right-clicking INSIDE a multi-selection keeps it, so "delete these
      // five" works. Right-clicking outside one replaces it, which is what
      // every file manager does and stops a stale selection being acted on
      // by surprise.
      if (!isSelected(s.n)) setSelection([s.n], s.n);
      else paintSelection();
      snippetMenu(e.clientX, e.clientY, i);
    });

    return row;
  }

  function groupHeader(g, depth) {
    // The one distinction the sidebar turns on: a plain group UNFOLDS where it
    // sits, a project is a place you GO. Everything below reads off this.
    var proj = isProject(g);
    var open = !isCollapsed(g);
    var head = document.createElement("div");
    head.className = "group-head" + (proj || open ? "" : " collapsed") +
                     (g === cursor ? " cursor" : "");
    head.dataset.key = g;      // keyboard cursor and paintSelection find it here
    head.style.paddingLeft = (12 + (depth || 0) * 16) + "px";
    // Only the LAST segment is shown; the indent already says where it sits,
    // and the full path would wrap in a sidebar this narrow.
    head.innerHTML =
      // One glyph, and which one IS the distinction. The filled folder now
      // means project and nothing else; a group carries only its caret. Telling
      // them apart is a matter of shape rather than of noticing that two
      // otherwise identical folders are different colours.
      //
      // Both slots are the same width, so the names stay on one column.
      (proj
        ? '<span class="group-ico">' +
            '<svg viewBox="0 0 24 24" fill="currentColor">' +
            '<path d="M3 6.5A1.5 1.5 0 0 1 4.5 5h4.2c.5 0 .97.24 1.25.65l.9 1.3h8.65A1.5 1.5 0 0 1 21 8.45v9.05A1.5 1.5 0 0 1 19.5 19h-15A1.5 1.5 0 0 1 3 17.5z"/>' +
            "</svg>" +
          "</span>"
        // The caret is the promise that this row unfolds, which is the only
        // thing a group has to say about itself.
        : '<span class="group-caret">' +
            '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" ' +
            'stroke-linecap="round" stroke-linejoin="round"><path d="M9 6l6 6-6 6"/></svg>' +
          "</span>") +
      '<span class="group-name">' + esc(baseName(g)) + "</span>" +
      // No "PROJECT" word: at 210px it truncated the name to "DEM…", and the
      // run triangle already says the folder is runnable. The tooltip carries
      // the explanation for anyone who wants it.
      (isProject(g) ? '<span class="group-run" title="Run this project">&#9654;</span>' : "") +
      '<span class="group-add" title="New snippet in this group">+</span>';
    head.title = g + (isProject(g) ? "  (project: all .cpp compiled together)" : "");
    if (isProject(g)) head.classList.add("is-project");

    head.addEventListener("click", function (e) {
      if (e.target.closest(".group-add")) {
        e.stopPropagation();
        newSnippet(g);
        return;
      }
      if (e.target.closest(".group-run")) {
        e.stopPropagation();
        runProject(g);
        return;
      }
      // A project does not unfold, so a single click on one has nothing to do.
      // It opens on a double click, below.
      if (proj) return;
      setCollapsed(g, open);
      renderSnippets();
    });

    // A real dblclick listener works here only BECAUSE a single click on a
    // project does nothing: the row is never re-rendered between the two
    // clicks, so the browser sees both on this same element. On a group the
    // first click rebuilds the row, the second lands on a different node, and
    // dblclick would fire on the list if it fired at all.
    if (proj) head.addEventListener("dblclick", function () { focusOn(g); });

    head.addEventListener("contextmenu", function (e) {
      e.preventDefault();
      e.stopPropagation();
      groupMenu(e.clientX, e.clientY, g);
    });

    // A group is draggable as a unit: dropping it elsewhere renames its
    // directory, and every member's path follows for free. The + and run
    // controls keep their presses -- a wiggle on a button should stay a
    // click, not pick the folder up.
    head.addEventListener("mousedown", function (e) {
      if (e.button !== 0) return;
      if (e.target.closest(".group-add, .group-run")) return;
      armDrag(e, head, function () {
        dragGroup = g;
        dragNames = [];
      });
    });
    return head;
  }

  function renderNode(list, node, depth) {
    // Child groups first, then loose snippets -- the convention every file pane
    // uses, and it keeps a group's own contents visually beneath it.
    node.kids.forEach(function (kid) {
      list.appendChild(groupHeader(kid.path, depth));
      renderOrder.push({ kind: "group", key: kid.path });
      // A project is a unit you open, so the recursion stops here whatever its
      // stored collapse flag says. Its files are shown by going in, which is
      // also what makes "run this folder" mean something definite.
      if (isProject(kid.path)) return;
      if (isCollapsed(kid.path)) return;
      if (!kid.kids.length && !kid.items.length) {
        var empty = document.createElement("div");
        empty.className = "group-empty";
        empty.style.paddingLeft = (12 + (depth + 1) * 16) + "px";
        empty.textContent = "empty";
        // Still a drop target, or an empty group would be the one place you
        // could not drop anything. The engine reads this marker.
        empty.dataset.dropInto = kid.path;
        list.appendChild(empty);
        return;
      }
      renderNode(list, kid, depth + 1);
    });
    node.items.forEach(function (i) {
      list.appendChild(snippetRow(snippets[i], i, depth));
      renderOrder.push({ kind: "snippet", key: snippets[i].n, index: i });
    });
  }

  function setFocus(to) {
    focus = to;
    saveFocus();
    renderSnippets();
  }

  function focusOn(to) {
    if (to === focus) return;
    navBack.push(focus);
    navFwd.length = 0;          // browser rule: a new destination kills Forward
    setFocus(to);
  }

  // History entries are validated when POPPED, not on every refresh. Most are
  // never visited again, so checking them all on each SNIPPETS message would
  // walk the tree once per entry to answer a question nobody asked.
  function goBack() {
    if (!navBack.length) return;
    navFwd.push(focus);
    setFocus(nearestLive(navBack.pop()));
  }
  function goFwd() {
    if (!navFwd.length) return;
    navBack.push(focus);
    setFocus(nearestLive(navFwd.pop()));
  }
  // Up is NOT Back. Back retraces where you have been; up climbs the path you
  // are standing on, which is usually somewhere you have never been.
  function goUp() { if (focus) focusOn(parentOf(focus)); }

  // A folder that is renamed or dragged elsewhere takes the focus and the
  // history with it. The host reports the new tree but not what became what,
  // so the intent is recorded at send time and applied when the refresh lands.
  // Without this, renaming the folder you are standing in drops you to its
  // parent -- correct, but not what you asked for.
  function followMove(from, to) {
    function rw(p) {
      if (p === from) return to;
      return p.indexOf(from + "/") === 0 ? to + p.slice(from.length) : p;
    }
    focus   = rw(focus);
    navBack = navBack.map(rw);
    navFwd  = navFwd.map(rw);
    saveFocus();
  }

  function renderCrumbs() {
    var bar = $("crumbs");
    // TWO ROWS, and the split is the point: the path is the part that GROWS,
    // so it gets a row of its own and wraps into it. With both on one line a
    // deep folder pushed the arrows around and wrapped them to a second line
    // anyway -- the row moved under you as you navigated. Now the arrows sit
    // in a strip whose size never changes.
    //
    // Always up, even at the root with no history: it says where the tree
    // lives, which is a thing users ask (snippets are in %APPDATA%, not next
    // to the exe), and a header that appears and disappears reflows the list.
    bar.hidden = false;
    bar.innerHTML = "";

    var path = document.createElement("div");
    path.className = "crumb-path";
    var nav = document.createElement("div");
    nav.className = "crumb-row";

    function arrow(glyph, title, live, go) {
      var b = document.createElement("span");
      b.className = "crumb-nav" + (live ? "" : " off");
      b.textContent = glyph;
      b.title = title;
      if (live) b.addEventListener("click", go);
      return b;
    }
    nav.appendChild(arrow("←", "Back  (Alt+Left, or mouse back)",     navBack.length > 0, goBack));
    nav.appendChild(arrow("→", "Forward  (Alt+Right, or mouse fwd)",  navFwd.length > 0, goFwd));

    // NO DISK-ROOT SEGMENT. "%APPDATA% › Snippets" spent the first half of a
    // 208px line on a fact that never changes, and read as two levels of
    // nesting where there is one. Where the tree lives is still worth being
    // able to find, so it stays as the root crumb's tooltip -- one hover, not
    // one line, and the answer is the same.
    var trail = [{ label: "Snippets", path: "" }], acc = "";
    (focus ? focus.split("/") : []).forEach(function (p) {
      acc = acc ? acc + "/" + p : p;
      trail.push({ label: p, path: acc });
    });

    trail.forEach(function (seg, i) {
      // A separator goes BETWEEN segments. With the disk root gone the first
      // crumb starts the line, and a leading "›" there read as a level that
      // had been cut off.
      if (i) {
        var sep = document.createElement("span");
        sep.className = "crumb-sep";
        sep.textContent = "›";
        path.appendChild(sep);
      }

      var here = i === trail.length - 1;
      var c = document.createElement("span");
      c.className = "crumb" + (here ? " here" : "");
      c.textContent = seg.label;
      c.title = seg.path || "%APPDATA%\\EclipseConsole\\snippets";
      if (!here) c.addEventListener("click", function () { focusOn(seg.path); });
      // Every segment is a drop target -- the engine reads this marker. Not a
      // nicety: the list background means "into the focus", so without these
      // there would be no way to drag anything OUT of the folder you are
      // standing in. Set on "here" too; canDropInto refuses the no-ops.
      c.dataset.dropInto = seg.path;
      path.appendChild(c);
    });

    bar.appendChild(path);
    bar.appendChild(nav);
  }

  function renderSnippets() {
    var list = $("snippetList");
    list.innerHTML = "";
    renderOrder = [];          // rebuilt by renderNode as it walks

    var tree = buildTree();
    var node = nodeAt(tree, focus);
    if (!node) {
      // Only surrender the focus when there is a tree to judge it against. At
      // startup both lists are empty because SNIPPETS has not arrived yet, and
      // clamping here would silently discard the focus restored from storage.
      if (groupList.length || snippets.length) { focus = nearestLive(focus); saveFocus(); }
      node = nodeAt(tree, focus) || tree;
    }

    if (!node.kids.length && !node.items.length) {
      list.innerHTML = '<div class="empty-note">' +
        (focus ? "Nothing here yet. Press + to add a snippet."
               : "No snippets yet. Press + to make one.") + "</div>";
    }
    renderNode(list, node, 0);
    renderCrumbs();
    renderTabs();
    paintRunning();   // freshly built rows: re-apply the live/stopping markers
  }

  // The gap below the last row and the list background are drop targets too
  // -- both live in updateDropTarget now, which is the one place that knows
  // where every Y lands.

  // ---- rubber-band selection ------------------------------------------------
  // Drag from empty space to sweep a box over rows. Only from EMPTY space: a
  // row's own mousedown belongs to the HTML5 drag that moves it, and starting
  // a band there would make files impossible to pick up.
  //
  // The anchor is kept in CONTENT coordinates (clientY + scrollTop) rather
  // than screen ones, so when the list auto-scrolls mid-sweep the box stays
  // glued to the rows it was drawn over instead of sliding off them.
  var band = null;       // {x0,y0,x1,y1,additive} content coords
  var bandEl = null;
  var bandBase = [];     // selection to union with, for a Ctrl+drag
  var bandClientX = 0, bandClientY = 0;

  // Content coordinates of the SCROLL PANE. Everything below works in these
  // and converts row positions the same way, via getBoundingClientRect rather
  // than offsetTop -- offsetTop depends on which ancestor happens to be
  // positioned, and that is not something this should be coupled to.
  function listPoint(e) {
    var pane = scrollPane();
    var b = pane.getBoundingClientRect();
    return { x: e.clientX - b.left, y: e.clientY - b.top + pane.scrollTop };
  }

  function updateBand() {
    if (!band) return;
    var pane = scrollPane();
    var b = pane.getBoundingClientRect();
    // Recomputed from the last pointer position every frame, because
    // auto-scroll moves the content under a stationary cursor.
    band.x1 = bandClientX - b.left;
    band.y1 = bandClientY - b.top + pane.scrollTop;

    var top = Math.min(band.y0, band.y1), bot = Math.max(band.y0, band.y1);
    var left = Math.min(band.x0, band.x1), right = Math.max(band.x0, band.x1);

    if (bandEl) {
      bandEl.style.top    = top + "px";
      bandEl.style.height = (bot - top) + "px";
      bandEl.style.left   = left + "px";
      bandEl.style.width  = (right - left) + "px";
    }

    // Vertical overlap only. The sidebar is one narrow column, so requiring
    // horizontal overlap too would just make the sweep fussy for no gain.
    var hits = bandBase.slice();
    Array.prototype.forEach.call($("snippetList").children, function (el) {
      if (!el.classList.contains("script-row")) return;
      var key = el.dataset && el.dataset.key;
      if (!key) return;
      var r = el.getBoundingClientRect();
      var elTop = r.top - b.top + pane.scrollTop;
      var elBot = elTop + r.height;
      if (elBot > top && elTop < bot && hits.indexOf(key) < 0) hits.push(key);
    });
    selected = hits;
    paintSelection();
  }

  // Bound to the PANE so the empty space under the last row starts a band too
  // -- that is most of the sidebar when there are only a few snippets, and it
  // is the natural place to begin a sweep.
  scrollPane().addEventListener("mousedown", function (e) {
    if (e.button !== 0) return;
    // Rows own their mousedown (it begins the drag that moves them), and the
    // header controls are all clickable in their own right.
    if (e.target.closest(".script-row, .group-head, .group-empty, .crumbs, .side-label, button, input"))
      return;

    var additive = e.ctrlKey || e.metaKey;
    bandBase = additive ? selected.slice() : [];
    // Clicking empty space clears, which is the no-drag case of this same
    // gesture -- a band of zero height simply selects nothing.
    if (!additive) setSelection([], null);

    var p = listPoint(e);
    band = { x0: p.x, y0: p.y, x1: p.x, y1: p.y };
    bandClientX = e.clientX; bandClientY = e.clientY;

    bandEl = document.createElement("div");
    bandEl.className = "sel-band";
    // Appended to the PANE, not the list: the list is a flex column that
    // renderSnippets() empties wholesale, and it does not span the empty area
    // the band needs to be drawn over.
    scrollPane().appendChild(bandEl);
    updateBand();
    // Keep the keys live -- the pane's own mousedown-to-focus only covers the
    // list element.
    $("snippetList").focus({ preventScroll: true });
    e.preventDefault();          // no text selection drag-along
  });

  // On the document, not the list: the pointer routinely leaves the sidebar
  // mid-sweep and the band has to keep tracking it.
  document.addEventListener("mousemove", function (e) {
    if (!band) return;
    bandClientX = e.clientX; bandClientY = e.clientY;
    autoScrollAt(e.clientY);     // same rAF loop the drag-and-drop uses
    updateBand();
  });

  // Torn down on mouseup. (The old HTML5 drag could swallow that mouseup and
  // strand the band; the pointer drag cannot, and a band never starts on a
  // row anyway -- rows own their press as a pickup.)
  function endBand() {
    if (!band) return;
    band = null;
    bandBase = [];
    endAutoScroll();
    if (bandEl && bandEl.parentNode) bandEl.parentNode.removeChild(bandEl);
    bandEl = null;
    // Whatever the sweep landed on becomes the anchor for a following Shift.
    selAnchor = selected.length ? selected[selected.length - 1] : null;
  }

  document.addEventListener("mouseup", endBand);

  // Leaving the pane drops the selection, so a stale highlight cannot sit
  // there looking like it is still what a bulk action would hit.
  $("snippetList").addEventListener("blur", function () {
    // Except when a menu or prompt has taken focus -- those are acting ON the
    // selection, and clearing it underneath them would pull the highlight
    // away exactly while it is being decided about.
    if (ctxEl && ctxEl.classList.contains("open")) return;
    if (promptOpen()) return;
    // The cursor goes too. It marks where the ARROW KEYS are, and they are no
    // longer here -- leaving it behind put a ring on a row that was neither
    // open nor selected, which reads as a third kind of highlight rather than
    // as a keyboard position. Before setSelection, which is what repaints.
    cursor = null;
    setSelection([], null);
  });

  // ---- keyboard navigation --------------------------------------------------
  // Bound to the LIST, not the document. The editor holds focus almost all the
  // time and every unmodified key belongs to whatever is being typed -- which
  // is why the existing document-level shortcuts are all on Alt. Scoping these
  // to the pane is what buys us the bare Explorer keys.
  //
  // Clicking a row focuses the list, so the keys are live straight after the
  // click that made a selection.
  $("snippetList").addEventListener("mousedown", function () {
    $("snippetList").focus({ preventScroll: true });
  });

  function cursorAt() {
    for (var i = 0; i < renderOrder.length; i++)
      if (renderOrder[i].key === cursor) return i;
    return -1;
  }

  // Moves the keyboard cursor by `d` rows. `extend` grows the selection from
  // the anchor instead of replacing it, which is Shift+Arrow.
  function moveCursor(d, extend) {
    if (!renderOrder.length) return;
    var at = cursorAt();
    var to = at < 0 ? (d > 0 ? 0 : renderOrder.length - 1) : at + d;
    if (to < 0 || to >= renderOrder.length) return;

    var row = renderOrder[to];
    cursor = row.key;

    if (row.kind === "snippet") {
      if (extend) {
        if (!selAnchor) selAnchor = row.key;
        setSelection(rangeBetween(selAnchor, row.key));
      } else {
        setSelection([row.key], row.key);
      }
    } else if (!extend) {
      // A group header is not selectable, so landing on one clears rather
      // than dragging the previous selection along behind the cursor.
      setSelection([], null);
    }

    paintSelection();
    var el = rowFor(cursor);
    if (el && el.scrollIntoView) el.scrollIntoView({ block: "nearest" });
  }

  $("snippetList").addEventListener("keydown", function (e) {
    // Never swallow the app-wide chords; those are handled on the document.
    if (e.ctrlKey || e.metaKey || e.altKey) {
      if (e.key.toLowerCase() === "a" && (e.ctrlKey || e.metaKey)) {
        e.preventDefault();
        var all = [];
        renderOrder.forEach(function (r) { if (r.kind === "snippet") all.push(r.key); });
        setSelection(all, all[0] || null);
      }
      return;
    }

    var at, row;
    if (e.key === "ArrowDown")      { e.preventDefault(); moveCursor(+1, e.shiftKey); }
    else if (e.key === "ArrowUp")   { e.preventDefault(); moveCursor(-1, e.shiftKey); }
    else if (e.key === "Home")      { e.preventDefault(); moveCursor(-renderOrder.length, e.shiftKey); }
    else if (e.key === "End")       { e.preventDefault(); moveCursor(+renderOrder.length, e.shiftKey); }
    else if (e.key === "Enter") {
      e.preventDefault();
      at = cursorAt(); if (at < 0) return;
      row = renderOrder[at];
      if (row.kind === "snippet") openSnippet(row.index);
      else if (isProject(row.key)) focusOn(row.key);
      else { setCollapsed(row.key, !isCollapsed(row.key)); renderSnippets(); }
    }
    else if (e.key === "ArrowRight") {
      e.preventDefault();
      at = cursorAt(); if (at < 0) return;
      row = renderOrder[at];
      // Right opens a closed group; on an open one it steps INTO it, which is
      // just the next row down given how the tree is flattened.
      if (row.kind === "group" && !isProject(row.key) && isCollapsed(row.key)) {
        setCollapsed(row.key, false);
        renderSnippets();
      } else {
        moveCursor(+1, false);
      }
    }
    else if (e.key === "ArrowLeft") {
      e.preventDefault();
      at = cursorAt(); if (at < 0) return;
      row = renderOrder[at];
      if (row.kind === "group" && !isProject(row.key) && !isCollapsed(row.key)) {
        setCollapsed(row.key, true);
        renderSnippets();
      } else {
        // Otherwise jump to the parent header -- Explorer's "out one level".
        var owner = row.kind === "snippet" ? groupOf(row.key) : parentOf(row.key);
        if (owner) { cursor = owner; paintSelection();
                     var el = rowFor(owner);
                     if (el && el.scrollIntoView) el.scrollIntoView({ block: "nearest" }); }
      }
    }
    else if (e.key === "F2") {
      e.preventDefault();
      at = cursorAt(); if (at < 0) return;
      row = renderOrder[at];
      if (row.kind === "snippet") snippetRename(row.index);
      else groupRename(row.key);
    }
    else if (e.key === "Delete") {
      e.preventDefault();
      var names = orderedSelection();
      if (!names.length) return;
      names.forEach(function (n) { send("SNIP_DELETE:" + n); });
      setSelection([]);
    }
    else if (e.key === "Escape") { setSelection([], null); }
  });

  // Width the tabs are pinned to while a run of closes is in progress, or 0
  // when they are free to size themselves.
  //
  // Closing a tab makes the survivors re-expand to fill the gap, which slides
  // the next close button out from under the cursor -- so closing five in a
  // row means re-aiming five times. Chrome's answer, and this one: freeze the
  // widths on the first close and only let them recompute once the pointer
  // leaves the strip. Every X then lands in the same place.
  var frozenTabW = 0;

  // Which tab we last auto-scrolled to. renderTabs runs for all sorts of
  // reasons -- renderSnippets calls it, and that fires on every host refresh
  // -- so scrolling to the active tab unconditionally drags the strip back
  // under you the moment you try to scroll away from it. Only move when the
  // active tab has actually CHANGED.
  var lastScrolledTab = null;

  // ---- tab reordering -------------------------------------------------------
  // Pointer-driven, NOT native drag-and-drop, and the trade is deliberate.
  //
  // Native DnD gives the OS move cursor for free -- which is why the sidebar
  // uses it -- but the browser draws the ghost and will not let you constrain
  // it to one axis or animate anything around it. Chrome's own tab strip has
  // the same problem and solves it the same way: the tab itself is the drag
  // image, moved by transform, and its neighbours slide aside to open the gap.
  // The cost is the cursor, which stays a plain arrow throughout.
  //
  // Uniform tab widths make the arithmetic trivial: every displacement is
  // exactly one tab width, and the target index is (from + round(dx / W)).
  // That is only true because the strip sizes every tab identically -- if that
  // ever changes, this needs per-tab centres instead.
  var tabDrag = null;          // null unless a drag is past the threshold
  var pendingTabRender = false;
  var suppressTabClick = false;

  // How far the pointer must travel before it is a drag rather than a click.
  // Below this a tab still activates normally, so a slightly shaky click does
  // not silently reorder anything.
  var TAB_DRAG_SLOP = 4;

  function tabIndex(n) {
    for (var i = 0; i < tabs.length; i++) if (tabs[i].n === n) return i;
    return -1;
  }

  function moveTab(name, to) {
    var from = tabIndex(name);
    if (from < 0) return;
    // Lifting the source first shifts everything after it down one, so a
    // rightward move lands a slot short without this.
    if (from < to) to--;
    if (to === from) return;
    tabs.splice(to, 0, tabs.splice(from, 1)[0]);
    renderTabs();
    rememberTabs();
  }

  function beginTabDrag(e, name, el) {
    var strip = $("tabstrip");
    var els = strip.querySelectorAll(".tab");
    if (els.length < 2) return;                  // nothing to reorder against

    var from = tabIndex(name);
    if (from < 0) return;

    // offsetLeft, not getBoundingClientRect: content coordinates are immune to
    // the strip scrolling under the drag, which it does when auto-scroll kicks
    // in. Everything below is in that same space.
    var W = els[1].offsetLeft - els[0].offsetLeft;
    if (!(W > 0)) return;

    tabDrag = {
      name: name, el: el, from: from, to: from,
      els: els, w: W, n: els.length,
      startX: e.clientX, startScroll: strip.scrollLeft,
      lastX: e.clientX, live: false, raf: null
    };

    // Capture is taken in tabDragTick, NOT here. While an element holds pointer
    // capture Chromium retargets the compatibility mouse events to it as well,
    // so capturing on pointerdown sent the click to the strip and the tab's own
    // handler never ran -- clicking a tab stopped switching to it. Until the
    // drag goes live, pointermove still reaches the strip by bubbling from the
    // tab, so nothing is lost by waiting.
    tabDrag.pointerId = e.pointerId;
    tabDrag.raf = requestAnimationFrame(tabDragTick);
  }

  // One rAF loop drives both the auto-scroll and the layout, because the two
  // are coupled: at the edge the strip keeps scrolling while the pointer is
  // held still, and every one of those frames changes where the tab sits.
  // Driving this off pointermove alone would freeze the moment you stopped.
  function tabDragTick() {
    var d = tabDrag;
    if (!d) return;
    var strip = $("tabstrip");

    // Edge auto-scroll. Its own dials rather than the sidebar's: the geometry
    // differs by roughly 7x -- the sidebar scrolls past 28px rows, this scrolls
    // past 190px tabs -- so the same pixels-per-frame that races there crawls
    // here. Shared values would have meant one of the two was always wrong.
    var b = strip.getBoundingClientRect(), EDGE = S("tabs.dragEdge"), step = 0;
    if (d.lastX < b.left + EDGE)       step = -(1 - (d.lastX - b.left) / EDGE);
    else if (d.lastX > b.right - EDGE) step =  (1 - (b.right - d.lastX) / EDGE);
    if (step && d.live) {
      // Squared, as in the sidebar: it creeps near the boundary and only races
      // at the very end.
      var mag = Math.min(1, Math.abs(step));
      strip.scrollLeft += (step < 0 ? -1 : 1) * mag * mag * S("tabs.dragSpeed");
    }

    // Scroll delta folded in, so content-space displacement is what the
    // pointer did PLUS what the strip did underneath it.
    var dx = (d.lastX - d.startX) + (strip.scrollLeft - d.startScroll);

    if (!d.live) {
      if (Math.abs(dx) < TAB_DRAG_SLOP) { d.raf = requestAnimationFrame(tabDragTick); return; }
      d.live = true;
      // Now that it is unambiguously a drag and not a click, take the capture:
      // from here the pointer has to be followed outside the strip, and there
      // is no click left to protect.
      try { strip.setPointerCapture(d.pointerId); } catch (err) {}
      strip.classList.add("reordering");
      d.el.classList.add("tab-dragging");
      for (var i = 0; i < d.n; i++) if (d.els[i] !== d.el) d.els[i].classList.add("tab-slide");
    }

    // The tab cannot leave the run of tabs: at either end there is no slot
    // past it, so following the pointer out there would promise a move that
    // cannot happen.
    dx = Math.max(-d.from * d.w, Math.min((d.n - 1 - d.from) * d.w, dx));
    d.dx = dx;
    d.el.style.transform = "translateX(" + dx + "px)";

    // Round, not floor: the gap opens once the dragged tab is more than half
    // way into the next slot, which is the point at which it looks like it
    // belongs there.
    var to = Math.max(0, Math.min(d.n - 1, d.from + Math.round(dx / d.w)));
    if (to !== d.to) {
      d.to = to;
      for (var k = 0; k < d.n; k++) {
        if (d.els[k] === d.el) continue;
        var shift = 0;
        if (to > d.from && k > d.from && k <= to) shift = -d.w;
        else if (to < d.from && k >= to && k < d.from) shift = d.w;
        d.els[k].style.transform = shift ? "translateX(" + shift + "px)" : "";
      }
    }

    d.raf = requestAnimationFrame(tabDragTick);
  }

  function endTabDrag() {
    var d = tabDrag;
    // `ending` is not paranoia: releasePointerCapture below fires
    // lostpointercapture, which is one of the events bound to this function.
    // Without the guard the settle would be armed twice, with two independent
    // transitionend listeners and two timers racing to commit the same move.
    if (!d || d.ending) return;
    d.ending = true;
    if (d.raf !== null) cancelAnimationFrame(d.raf);
    var strip = $("tabstrip");
    try { strip.releasePointerCapture(d.pointerId); } catch (err) {}

    if (!d.live) {                     // never passed the slop: it was a click
      tabDrag = null;
      if (pendingTabRender) { pendingTabRender = false; renderTabs(); }
      return;
    }

    // Swallow the click this pointer sequence is about to synthesise, or the
    // release would also activate whatever tab ended up under it.
    suppressTabClick = true;
    // Cleared on the next task: the synthesised click lands before it, the
    // user's next real click lands after.
    setTimeout(function () { suppressTabClick = false; }, 0);

    // Settle into the gap rather than snapping: the slot is already open, so
    // the tab only has to travel the last few pixels to land in it.
    var target = (d.to - d.from) * d.w;
    d.el.classList.add("tab-settling");
    d.el.style.transform = "translateX(" + target + "px)";

    var done = false;
    function commit() {
      if (done) return;
      done = true;
      d.el.removeEventListener("transitionend", commit);
      strip.classList.remove("reordering");
      // Cleared before the reorder: moveTab re-renders, and a transform left
      // on a recycled element would offset a tab that is already in place.
      for (var i = 0; i < d.n; i++) {
        d.els[i].style.transform = "";
        d.els[i].classList.remove("tab-slide", "tab-dragging", "tab-settling");
      }
      var name = d.name, to = d.to + (d.to > d.from ? 1 : 0);
      tabDrag = null;                  // before moveTab: renderTabs is guarded on it
      pendingTabRender = false;
      moveTab(name, to);
      renderTabs();
    }
    d.el.addEventListener("transitionend", commit);
    // transitionend never fires when the tab is already where it is going --
    // a drag that ends back in its own slot sets no new value to animate.
    setTimeout(commit, 220);
  }

  function renderTabs() {
    // A rebuild mid-drag would swap out the elements the drag holds references
    // to, leaving it animating orphans. Deferred to the drag's own commit.
    if (tabDrag) { pendingTabRender = true; return; }
    var strip = $("tabstrip");
    // Rebuilding from innerHTML resets scrollLeft to 0. Harmless when the strip
    // fits, but a reorder while scrolled right would otherwise yank the view
    // back to the first tab mid-gesture.
    var keepScroll = strip.scrollLeft;
    strip.innerHTML = "";

    tabs.forEach(function (t) {
      var isActive = t.n === currentName;
      // The ACTIVE tab's dirty state lives in `dirty`, not in the tab record --
      // the record is only updated on stash, so reading it here would show the
      // state as of the last switch rather than as of this keystroke.
      var isDirty = isActive ? dirty : t.dirty;

      var tab = document.createElement("div");
      tab.className = "tab" + (isActive ? " active" : "") + (isDirty ? " dirty" : "");
      // renderTabs rebuilds the strip from scratch, so the pin has to be
      // re-applied here rather than set once on the old elements.
      if (frozenTabW) tab.style.flex = "0 0 " + frozenTabW + "px";
      // The file name, not the path: "walkspeed.cpp" rather than
      // "movement/walkspeed.cpp", which would fill the tab and ellipsis away
      // the part you actually read. The full path is the tooltip.
      tab.title = t.n;
      tab.innerHTML =
        '<span class="dot"></span>' +
        '<span class="tname">' + esc(baseName(t.n)) + "</span>" +
        '<span class="close" title="Close">' +
          '<svg viewBox="0 0 10 10" fill="none" stroke="currentColor" ' +
          'stroke-width="1.2" stroke-linecap="round">' +
          '<path d="M1 1l8 8M9 1L1 9"/></svg>' +
        "</span>";

      tab.addEventListener("click", function (e) {
        // A reorder ends with a synthesised click on whatever the pointer came
        // to rest over. Acting on it would switch tabs every time you dragged.
        if (suppressTabClick) return;
        if (e.target.closest(".close")) {
          e.stopPropagation();
          // Unsaved work would vanish silently otherwise, and the dot is the
          // only warning there is.
          var d = (t.n === currentName) ? dirty : t.dirty;
          if (d && !confirm(baseName(t.n) + " has unsaved changes. Close it?")) return;
          // Measured BEFORE the close, while this tab still exists -- it is
          // the width every remaining tab gets pinned to.
          if (S("tabs.freezeOnClose") && !frozenTabW)
            frozenTabW = tab.getBoundingClientRect().width;
          closeTab(t.n);
          return;
        }
        var i = indexOfName(t.n);
        if (i >= 0) openSnippet(i);
      });

      // Middle-click closes, as everywhere else with tabs.
      tab.addEventListener("mousedown", function (e) {
        if (e.button !== 1) return;
        e.preventDefault();
        var d = (t.n === currentName) ? dirty : t.dirty;
        if (d && !confirm(baseName(t.n) + " has unsaved changes. Close it?")) return;
        closeTab(t.n);
      });

      // Reordering is pointer-driven; see the tab-drag block above renderTabs
      // for why it is not native drag-and-drop.
      tab.addEventListener("pointerdown", function (e) {
        if (e.button !== 0 || e.target.closest(".close")) return;
        beginTabDrag(e, t.n, tab);
      });

      strip.appendChild(tab);
    });

    // New snippet, in the same group as whatever is open -- a new file usually
    // belongs beside the one you were just looking at.
    var add = document.createElement("div");
    add.className = "tab-add";
    add.title = "New snippet";
    add.innerHTML =
      '<svg viewBox="0 0 12 12" fill="none" stroke="currentColor" ' +
      'stroke-width="1.3" stroke-linecap="round"><path d="M6 1v10M1 6h10"/></svg>';
    add.addEventListener("click", function () {
      var into = (current >= 0 && snippets[current]) ? (snippets[current].g || "") : "";
      newSnippet(into);
    });
    strip.appendChild(add);

    var sp = document.createElement("div");
    sp.className = "spacer";
    strip.appendChild(sp);

    if (strip.scrollLeft !== keepScroll) strip.scrollLeft = keepScroll;

    // The toolbar names the same file the tabs do, so they update together.
    renderToolbarFile();

    // Switching to a tab that is scrolled off does nothing visible otherwise
    // -- the editor changes and the strip still shows somewhere else.
    // "nearest" so it only moves when the tab is genuinely out of view.
    if (currentName !== lastScrolledTab) {
      lastScrolledTab = currentName;
      var act = strip.querySelector(".tab.active");
      if (act && act.scrollIntoView) act.scrollIntoView({ block: "nearest", inline: "nearest" });
    }
  }

  // Bound once: #tabstrip survives every render, so binding inside renderTabs
  // would stack a listener per repaint.
  (function () {
    var strip = $("tabstrip");

    // The strip holds the pointer capture, so every move and release during a
    // reorder arrives here rather than on the tab the pointer happens to be
    // over. The tick loop reads lastX; this only has to keep it current.
    strip.addEventListener("pointermove", function (e) {
      if (tabDrag) tabDrag.lastX = e.clientX;
    });
    strip.addEventListener("pointerup", endTabDrag);
    strip.addEventListener("pointercancel", endTabDrag);
    // Capture can be lost without a pointerup at all -- another window taking
    // focus, or the host running its own modal loop -- which would strand the
    // drag with its rAF still running and a tab still lifted.
    strip.addEventListener("lostpointercapture", endTabDrag);

    // Releasing the pin. Leaving the strip is the signal that the run of
    // closes is over, which is the same rule Chrome uses.
    strip.addEventListener("mouseleave", function () {
      if (!frozenTabW) return;
      frozenTabW = 0;
      renderTabs();
    });

    // Wheel scrolling, eased.
    //
    // Two dead ends first, so neither gets retried. Assigning scrollLeft from
    // deltaY JUMPS -- that is why it felt wrong beside the editor and the
    // log, which get Chromium's animated wheel scrolling; scaling the delta
    // down only makes smaller jumps. And leaving it to Chromium's native
    // vertical-wheel-scrolls-a-horizontal-box behaviour does not fire here at
    // all, so the strip simply would not scroll.
    //
    // So the easing is ours: hold a target and walk toward it each frame.
    // Wheeling again while it is still moving extends the target rather than
    // restarting, which is what makes a fast flick keep up.
    var EASE = 0.18;          // fraction of the remaining gap per frame
    var target = null, raf = null;

    function step() {
      var cur = strip.scrollLeft;
      var gap = target - cur;
      if (Math.abs(gap) < 0.5) {
        strip.scrollLeft = target;
        target = null; raf = null;
        return;
      }
      strip.scrollLeft = cur + gap * EASE;
      raf = requestAnimationFrame(step);
    }

    strip.addEventListener("wheel", function (e) {
      var max = strip.scrollWidth - strip.clientWidth;
      if (max <= 0) return;                       // nothing to scroll
      e.preventDefault();

      var d = e.deltaY !== 0 ? e.deltaY : e.deltaX;
      // deltaMode is not always pixels: 1 is lines, 2 is pages. Unconverted,
      // a mouse reporting lines would move the strip by 3.
      if (e.deltaMode === 1)      d *= 16;
      else if (e.deltaMode === 2) d *= strip.clientWidth;

      var from = (target === null) ? strip.scrollLeft : target;
      target = Math.max(0, Math.min(max, from + d));
      if (raf === null) raf = requestAnimationFrame(step);
    }, { passive: false });
  })();

  // ---- open tabs ----------------------------------------------------------
  // [{ n: path, code: working buffer, dirty: bool }]
  //
  // Each tab owns its OWN buffer, so switching away and back keeps unsaved
  // edits. The textarea only ever holds the active one; stashTab() copies it
  // back before any switch, which is the single place that must not be
  // forgotten -- miss it and the edit is gone with no warning.
  var tabs = [];

  function tabIndex(name) {
    for (var i = 0; i < tabs.length; i++) if (tabs[i].n === name) return i;
    return -1;
  }

  // Where you were in this file, kept with the buffer.
  //
  // There is ONE textarea behind every tab, so without this its scrollTop and
  // caret simply persist across a switch -- open a second snippet and it is
  // already scrolled to wherever the first one was, and going back and forth
  // between two places means re-finding both every time.
  function stashTab() {
    var t = currentName ? tabIndex(currentName) : -1;
    if (t >= 0) {
      tabs[t].code   = input.value;
      tabs[t].dirty  = dirty;
      tabs[t].scroll = input.scrollTop;
      tabs[t].selA   = input.selectionStart;
      tabs[t].selB   = input.selectionEnd;
    }
  }

  function rememberTabs() {
    // The scroll and caret of the ACTIVE tab live in the textarea, not yet in
    // its record -- stash them first or the position saved for whatever you
    // are looking at is the one from the last time you switched away.
    stashTab();
    try {
      localStorage.setItem("tabs", JSON.stringify({
        open: tabs.map(function (t) {
          // Names and POSITIONS only. Never the buffer: contents always come
          // from disk, so this cannot resurrect a stale unsaved edit as if it
          // were saved work.
          return { n: t.n, scroll: t.scroll || 0, selA: t.selA || 0, selB: t.selB || 0 };
        }),
        active: currentName
      }));
    } catch (e) {}
  }

  function activateTab(name) {
    var t = tabIndex(name);
    if (t < 0) return;
    var k = indexOfName(name);
    currentName = name;
    current = k;

    // A CLEAN tab has no edits worth keeping, so its text comes from disk, not
    // from the cached buffer. The cache is only authoritative when it holds
    // something disk does not -- i.e. when it is dirty.
    //
    // Trusting the cache unconditionally is what let an empty editor appear for
    // a file with 42 lines in it: any path that stored a bad buffer stayed
    // wrong forever, with the correct text sitting right there in `snippets`.
    // This way a stale buffer can cost you nothing you had not already saved.
    var fromDisk = (k >= 0) ? snippets[k].code : null;
    input.value = (tabs[t].dirty || fromDisk === null) ? tabs[t].code : fromDisk;
    // The buffer just changed underneath the editor. Without this the next
    // keystroke would snapshot the PREVIOUS file's text as its undo state --
    // which is how undo used to pull another snippet's content in. The tab's
    // own stacks are untouched, so switching away and back keeps its history.
    undoReset();

    repaint();

    // AFTER repaint. Assigning scrollTop before the new text is laid out
    // clamps against the OLD scrollHeight, so switching from a long file to a
    // short one silently lands at 0 -- and switching back gives the wrong
    // place rather than the one that was stashed.
    //
    // Clamped rather than trusted: the file may have shrunk on disk since,
    // and a stale offset past the end scrolls to nothing.
    var len = input.value.length;
    if (tabs[t].selA !== undefined) {
      input.selectionStart = Math.min(tabs[t].selA, len);
      input.selectionEnd   = Math.min(tabs[t].selB, len);
    } else {
      // A tab opened for the first time starts at the top. Left alone the
      // caret would keep the PREVIOUS file's offset, which is the same
      // carry-over problem one level down.
      input.selectionStart = input.selectionEnd = 0;
    }
    input.scrollTop = Math.min(tabs[t].scroll || 0,
                               Math.max(0, input.scrollHeight - input.clientHeight));
    syncScroll();

    // After the selection restore, not before: findRefresh picks the match
    // nearest the caret, and until this point the caret is still the previous
    // file's.
    findResync();
    // Belongs to the file that was open, not this one.
    diskSeen = (k >= 0 && snippets[k]) ? snippets[k].code : null;

    // Bring the sidebar with us.
    //
    // Opening from the sidebar already set these, but opening from a TAB did
    // not -- so switching tabs left the accent rail on the new file and the
    // cursor/selection sitting on whatever was last clicked in the list. Two
    // rows marked, for a navigation the user experienced as picking one file.
    //
    // The selection only follows when it is a single file. A multi-selection is
    // something you built on purpose to act on, and quietly throwing it away
    // because a tab was clicked would be worse than the mismatch it fixes.
    cursor = currentName;
    if (selected.length <= 1) {
      selected  = [currentName];
      selAnchor = currentName;
    }

    markDirty(tabs[t].dirty);
    renderSnippets();
    rememberTabs();
  }

  function openSnippet(i) {
    if (i < 0 || i >= snippets.length) return;
    var name = snippets[i].n;
    if (name === currentName) return;

    stashTab();                        // before anything else changes

    // Already open: switch to it, keeping whatever was typed there. Only a file
    // that is NOT open gets its buffer seeded from disk.
    if (tabIndex(name) < 0) tabs.push({ n: name, code: snippets[i].code, dirty: false });
    activateTab(name);
  }

  function closeTab(name) {
    var t = tabIndex(name);
    if (t < 0) return;
    tabs.splice(t, 1);
    if (name !== currentName) { renderTabs(); rememberTabs(); return; }

    // Closing the active one moves to its neighbour -- the tab to the right if
    // there is one, otherwise the left, which is what every editor does.
    if (tabs.length) {
      activateTab(tabs[Math.min(t, tabs.length - 1)].n);
    } else {
      current = -1;
      currentName = null;
      input.value = "";
      undoReset();
      repaint();
      markDirty(false);
      findResync();                  // nothing to match in an empty buffer
      renderSnippets();
      rememberTabs();
    }
  }

  // Nothing open at all. closeTab() handles the ordinary case of closing one of
  // several; this is the empty-editor state it falls through to.
  function closeSnippet() {
    tabs = [];
    current = -1;
    currentName = null;
    input.value = "";
    undoReset();
    repaint();
    markDirty(false);
    findResync();
    renderSnippets();
    rememberTabs();
  }

  function indexOfName(name) {
    for (var i = 0; i < snippets.length; i++) if (snippets[i].n === name) return i;
    return -1;
  }

  // ---- reordering ---------------------------------------------------------
  // The filesystem has no order, so the arrangement is persisted host-side as
  // snippets/.order. Only the list moves; no file is renamed or rewritten.

  // Everywhere, not just the list: the crumbs take .drop-into too, and the
  // old list-only sweep left a crumb lit after the pointer moved off it.
  function clearDropMarks() {
    document.querySelectorAll(".drop-before, .drop-after, .drop-into")
      .forEach(function (el) {
        el.classList.remove("drop-before", "drop-after", "drop-into");
      });
  }

  function pushOrder() {
    // Send the order the TREE renders in, not the raw array. That way .order
    // converges on siblings being contiguous instead of preserving an
    // interleaving nobody can see.
    //
    // GROUP paths go in too, each before its own contents -- that is how a
    // group keeps a position of its own rather than always arriving in
    // whatever sequence the directory scan produced. The host tells the two
    // kinds apart by looking each line up, so one file serves both.
    var flat = [];
    (function walk(node) {
      node.kids.forEach(function (k) { flat.push(k.path); walk(k); });
      node.items.forEach(function (i) { flat.push(snippets[i].n); });
    })(buildTree());
    send("SNIP_ORDER:" + flat.join("\n"));
  }

  function moveSnippet(from, to) {
    if (from < 0 || to < 0 || from === to) return;
    var item = snippets.splice(from, 1)[0];
    snippets.splice(to, 0, item);
    current = currentName ? indexOfName(currentName) : -1;
    renderSnippets();
    pushOrder();
  }

  // Several files to ONE insertion point, keeping their relative order.
  //
  // Not a loop over moveSnippet: each removal shifts every later index, so the
  // target slides out from under you. Remove all of them first, count how many
  // came from ABOVE the target (those are the ones that shift it), then insert
  // the block at the corrected position.
  function moveSnippets(names, toIndex) {
    var idxs = names.map(indexOfName)
                    .filter(function (i) { return i >= 0; })
                    .sort(function (a, b) { return a - b; });
    if (!idxs.length) return;

    var before = idxs.filter(function (i) { return i < toIndex; }).length;
    var items = [];
    for (var k = idxs.length - 1; k >= 0; k--) items.unshift(snippets.splice(idxs[k], 1)[0]);

    var at = toIndex - before;
    if (at < 0) at = 0;
    if (at > snippets.length) at = snippets.length;
    for (var m = 0; m < items.length; m++) snippets.splice(at + m, 0, items[m]);

    current = currentName ? indexOfName(currentName) : -1;
    renderSnippets();
    pushOrder();
  }

  // Reorder a group among its siblings. The whole SUBTREE travels with it --
  // groupList is flat, so a nested child left behind would be re-parented by
  // buildTree into whatever group ended up above it.
  function moveGroup(g, target, after) {
    if (!g || !target || g === target) return;
    var block = groupList.filter(function (p) {
      return p === g || p.indexOf(g + "/") === 0;
    });
    var rest = groupList.filter(function (p) { return block.indexOf(p) < 0; });

    var at = rest.indexOf(target);
    if (at < 0) return;
    if (after) {
      // Past the target's own subtree, not just past its header, or the block
      // lands inside the group it was meant to follow.
      while (at + 1 < rest.length && rest[at + 1].indexOf(target + "/") === 0) at++;
      at++;
    }
    groupList = rest.slice(0, at).concat(block, rest.slice(at));
    renderSnippets();
    pushOrder();
  }

  // Is anything already at this path? Snippets AND groups, because on disk
  // they are a file and a folder in the same place and cannot coexist. The
  // host refuses a colliding create either way -- that is the guard that
  // matters -- but catching it here lets us re-open the box with what was
  // typed, instead of the name being swallowed and the entry starting over.
  function pathTaken(path) {
    for (var i = 0; i < snippets.length; i++) if (snippets[i].n === path) return true;
    return groupList.indexOf(path) >= 0;
  }

  // Ask until the answer is a free name (or the box is dismissed). `pathFor`
  // turns what was typed into a full path, or "" if it is unusable; `make`
  // receives that path once it is known not to collide. The complaint shows
  // inside the dialog, so a collision costs one edit rather than the whole
  // entry.
  function askForFreeName(question, initial, pathFor, make, note) {
    askFor(question, initial, function (name) {
      var path = pathFor(name);
      if (!path) return;                    // nothing usable typed
      if (pathTaken(path)) {
        askForFreeName(question, name, pathFor, make,
                       path + " already exists - pick another name");
        return;
      }
      make(path);
    }, note);
  }

  // `into` is a group path, or "" for the root. The + on a group header passes
  // its own path so a new snippet lands inside it rather than at the top.
  function newSnippet(into) {
    var where = into ? into + "/" : "";
    askForFreeName("Name for the new snippet" + (into ? " in " + into : ""),
      "snippet" + (snippets.length + 1) + ".cpp",
      function (name) { return where + baseName(withCpp(name)); },
      function (path) {
        // SAVE_NEW, not SAVE: creating must never overwrite. See the handler
        // in eclipse_ui.cpp for why the two are separate messages.
        send("SAVE_NEW:" + path + "\n" +
             "// " + baseName(path) + "\nfx_log(\"hello\");\n");
      });
  }

  // `asProject` drops the .project marker straight after creating the folder,
  // so "New project" is one action instead of create-then-convert. The two
  // messages are handled in order and PROJECT_MARK only needs the folder to
  // exist, so no round trip is needed between them.
  function newGroup(into, asProject) {
    var where = into ? into + "/" : "";
    var what  = asProject ? "project" : "group";
    askForFreeName("Name for the new " + what + (into ? " in " + into : ""), "",
      function (name) {
        var clean = name.replace(/^[\s\/]+|[\s\/]+$/g, "");
        return clean ? where + clean : "";
      },
      function (path) {
        send("GROUP_NEW:" + path);
        if (asProject) send("PROJECT_MARK:" + path);
      });
  }

  // Into the focused folder, not the root -- otherwise every new file would
  // have to be dragged in from a tree you cannot currently see.
  $("btnNew").addEventListener("click", function () { newSnippet(focus); });

  // Right-clicking empty sidebar space. The creation items all land in `focus`
  // for the same reason the + button does; Trash is separated because it is the
  // one entry that does not make something.
  document.querySelector(".sidebar").addEventListener("contextmenu", function (e) {
    if (e.target.closest(".script-row")) return;   // rows have their own menu
    e.preventDefault();
    e.stopPropagation();
    openCtx(e.clientX, e.clientY, [
      { label: "New snippet",  run: function () { newSnippet(focus); } },
      { label: "New group…",   run: function () { newGroup(focus, false); } },
      // A project is a group whose .cpp files compile together, so it belongs
      // beside "New group" rather than only as a conversion after the fact.
      { label: "New project…", run: function () { newGroup(focus, true); } },
      { sep: true },
      { label: "Trash…", run: openTrash }
    ]);
  });

  function save() {
    if (!currentName) return;          // name, not index — see currentName above
    if (current >= 0 && snippets[current]) snippets[current].code = input.value;
    send("SAVE:" + currentName + "\n" + input.value);
    markDirty(false);
    renderSnippets();
  }
  $("btnSave").addEventListener("click", save);
  $("btnSettings").addEventListener("click", openSettings);

  // ---- context menu -------------------------------------------------------
  var ctxEl = document.getElementById("ctxMenu");

  function closeCtx() {
    ctxEl.classList.remove("open");
    // The sidebar's blur handler bails out while this menu is up -- it is
    // acting ON the selection and must not clear it underneath itself. That
    // left nothing to run once the menu went away, so the cursor survived with
    // the list no longer focused. Tidied here, which is the one path every
    // dismissal goes through. Guarded, because the menu can also be dismissed
    // while the list legitimately still has the keyboard.
    if (cursor !== null && document.activeElement !== $("snippetList")) {
      cursor = null;
      paintSelection();
    }
    // The Edit button stays lit while its menu is up; this is the one exit
    // that every dismissal path already goes through.
    var eb = $("btnEdit");
    if (eb) eb.classList.remove("open");
  }

  function openCtx(x, y, items) {
    ctxEl.innerHTML = "";
    items.forEach(function (it) {
      // Null entries are skipped so a caller can write a conditional item
      // inline rather than assembling the array in pieces.
      if (!it) return;
      if (it.sep) {
        var s = document.createElement("div");
        s.className = "ctx-sep";
        ctxEl.appendChild(s);
        return;
      }
      var el = document.createElement("div");
      el.className = "ctx-item" + (it.danger ? " danger" : "");
      el.innerHTML = "<span>" + esc(it.label) + "</span>" +
                     (it.hint ? '<span class="hint">' + esc(it.hint) + "</span>" : "");
      el.addEventListener("click", function (e) {
        e.stopPropagation();
        closeCtx();
        it.run();
      });
      ctxEl.appendChild(el);
    });

    // Show first so it can be measured, then keep it inside the window.
    ctxEl.classList.add("open");
    var w = ctxEl.offsetWidth, h = ctxEl.offsetHeight;
    if (x + w > window.innerWidth  - 6) x = window.innerWidth  - w - 6;
    if (y + h > window.innerHeight - 6) y = window.innerHeight - h - 6;
    ctxEl.style.left = Math.max(6, x) + "px";
    ctxEl.style.top  = Math.max(6, y) + "px";
  }

  document.addEventListener("click", closeCtx);
  document.addEventListener("scroll", closeCtx, true);
  window.addEventListener("blur", closeCtx);

  // This is an app window, not a web page: suppress WebView2's own menu
  // (Reload / Save as / Inspect) except in text fields, where cut/copy/paste
  // is genuinely wanted.
  document.addEventListener("contextmenu", function (e) {
    var t = e.target;
    if (t && (t.tagName === "INPUT" || t.tagName === "TEXTAREA")) return;
    e.preventDefault();
    closeCtx();
  });

  // ---- prompt (rename / new name) ----------------------------------------
  var promptOv = document.getElementById("promptOverlay");
  var promptIn = document.getElementById("promptInput");
  var promptCb = null;

  function promptOpen() { return promptOv.classList.contains("open"); }
  function closePrompt() { promptOv.classList.remove("open"); promptCb = null; }

  // Inline validation line under the input. Lives here rather than in the log
  // because the log is behind the scrim and blurred while the prompt is open.
  function promptNote(text) {
    var n = $("promptNote");
    if (!n) return;
    n.textContent = text || "";
    n.hidden = !text;
  }

  function askFor(question, initial, cb, note) {
    promptIn.placeholder = question;
    promptIn.value = initial || "";
    promptNote(note);              // carried over on a re-ask, cleared otherwise
    promptCb = cb;
    promptOv.classList.add("open");
    setTimeout(function () {
      promptIn.focus();
      // select the stem, leaving the extension - the usual rename behaviour
      var dot = promptIn.value.lastIndexOf(".");
      if (dot > 0) promptIn.setSelectionRange(0, dot); else promptIn.select();
    }, 0);
  }

  promptOv.addEventListener("click", function (e) {
    if (e.target === promptOv) closePrompt();
  });
  promptIn.addEventListener("keydown", function (e) {
    e.stopPropagation();
    if (e.key === "Enter") {
      e.preventDefault();
      var v = promptIn.value.trim(), cb = promptCb;
      closePrompt();
      if (v && cb) cb(v);
    } else if (e.key === "Escape") {
      e.preventDefault();
      closePrompt();
    }
  });

  // ---- snippet actions ----------------------------------------------------
  // Extracted so the context menu and F2 cannot drift apart.
  function snippetRename(i) {
    var s = snippets[i];
    if (!s) return;
    // Edits the file name only; the group is carried over. Moving between
    // groups is a separate action so neither can be done by accident.
    askFor("New name", baseName(s.n), function (name) {
      var to = (s.g ? s.g + "/" : "") + withCpp(name);
      if (to === s.n) return;
      send("SNIP_RENAME:" + s.n + "\n" + to);
    });
  }

  function groupRename(g) {
    askFor(isProject(g) ? "New project name" : "New group name", g, function (name) {
      name = name.replace(/^[\s\/]+|[\s\/]+$/g, "");
      if (!name || name === g) return;
      pendingMove = { from: g, to: name };
      send("GROUP_RENAME:" + g + "\n" + name);
    });
  }

  function groupMenu(x, y, g) {
    // Run and Open are also the ▶ and double-click on the header, and New
    // snippet is the +. Duplicated here on purpose: a glyph that only appears
    // on hover is not something you find unless you already know it is there,
    // and the menu is where you look when you don't.
    openCtx(x, y, [
      isProject(g) ? { label: "Run project", hint: "F5",
                       run: function () { runProject(g); } }
                   : null,
      // Only for a project. A group's one behaviour is unfolding where it sits,
      // so offering to open it here would put a second, hidden meaning on a row
      // that reads as having exactly one.
      isProject(g) ? { label: "Open folder", hint: "dbl-click",
                       run: function () { focusOn(g); } }
                   : null,
      isProject(g) ? { sep: true } : null,
      { label: "New snippet here",  run: function () { newSnippet(g); } },
      { label: "New group inside…", run: function () { newGroup(g, false); } },
      { sep: true },
      // The row calls itself a project everywhere else, so its own menu should
      // not switch back to calling it a group.
      { label: isProject(g) ? "Rename project…" : "Rename group…", hint: "F2",
        run: function () { groupRename(g); } },
      isProject(g)
        ? { label: "Back to a plain group", run: function () { send("PROJECT_UNMARK:" + g); } }
        : { label: "Make this a project", run: function () { send("PROJECT_MARK:" + g); } },
      { sep: true },
      // Was "Ungroup", which said nothing about the folder being deleted --
      // and is meaningless for a project, which nobody thinks of as a group.
      // Still not "Delete": the members move up into the parent and only the
      // emptied folder goes. No folder operation in here can destroy a
      // snippet, and the hint is there so nobody has to take that on trust.
      // Red for a project, plain for a group, because the two really do
      // differ in weight. Dissolving a group only unmakes a folder and is
      // undone by making it again. Dissolving a PROJECT also deletes its
      // .project file -- which can carry a custom `flags = ...` line -- so
      // build settings go with it and are not coming back.
      { label: "Remove folder", hint: "keeps snippets",
        danger: isProject(g),
        run: function () { send("GROUP_UNGROUP:" + g); } }
    ]);
  }

  // Several rows selected: the per-file actions (rename, duplicate) have no
  // sensible meaning across a set, so the menu becomes the two that do.
  function multiMenu(x, y) {
    var names = orderedSelection();
    var n = names.length;
    openCtx(x, y, [
      { label: "Move " + n + " to group…", run: function () {
          askFor("Group (blank for none)", "", function (g) {
            g = g.replace(/^[\s\/]+|[\s\/]+$/g, "");
            names.forEach(function (name) {
              if (groupOf(name) === g) return;
              send("SNIP_RENAME:" + name + "\n" + (g ? g + "/" : "") + baseName(name));
            });
          });
        } },
      { sep: true },
      { label: "Delete " + n + " snippets", danger: true, run: function () {
          names.forEach(function (name) { send("SNIP_DELETE:" + name); });
          setSelection([]);
        } }
    ]);
  }

  function snippetMenu(x, y, i) {
    var s = snippets[i];
    if (!s) return;
    if (selected.length > 1 && isSelected(s.n)) { multiMenu(x, y); return; }
    // Running a file inside a project builds the whole project -- see run().
    // The label says so rather than leaving "Run" to imply this one file.
    var owner = projectOwning(s.n);
    // A running snippet's first item is STOP, not Run. Offering "Run" against
    // something already live asks a question (restart? second copy?) that the
    // row itself already answers -- the marker beside it says it is running.
    var mine = runsNamed(baseName(owner || s.n));
    openCtx(x, y, [
      mine.length
        ? { label: "Stop", run: function () { send("STOP:" + mine[0].id); } }
        : { label: owner ? "Run project " + baseName(owner) : "Run", hint: "F5",
            run: function () { openSnippet(i); run(); } },
      { label: "Rename…", hint: "F2", run: function () { snippetRename(i); } },
      { label: "Move to group…", run: function () {
          // A move is a rename with a different folder in front. Naming a group
          // that does not exist creates it, so this is also how you make one --
          // the same as it works on disk. Blank moves the snippet back out.
          askFor("Group (blank for none)", s.g || "", function (g) {
            g = g.replace(/^[\s\/]+|[\s\/]+$/g, "");
            var to = (g ? g + "/" : "") + baseName(s.n);
            if (to === s.n) return;
            send("SNIP_RENAME:" + s.n + "\n" + to);
          });
        } },
      { label: "Duplicate", run: function () {
          var base = s.n.replace(/\.cpp$/i, "");
          askForFreeName("Name for the copy", baseName(base) + "_copy.cpp",
            function (name) { return (s.g ? s.g + "/" : "") + withCpp(name); },
            function (path) { send("SAVE_NEW:" + path + "\n" + s.code); });
        } },
      { sep: true },
      // Deletion moves the file to snippets/.trash rather than unlinking it,
      // so this needs no confirmation dialog and is recoverable.
      { label: "Delete", hint: "Del", danger: true, run: function () {
          send("SNIP_DELETE:" + s.n);
        } }
    ]);
  }

  // ---- console ------------------------------------------------------------
  // Scrollback is S("console.scrollback"), read at the point of use. Kept as
  // a function rather than a copy so raising it in the settings modal takes
  // effect on the next line rather than on the next launch.
  function logMax() { return S("console.scrollback"); }

  // Re-apply the level filters to every line already on screen, and resync the
  // chips. Called when the settings change them behind the chips' back.
  function applyLogFilters() {
    document.querySelectorAll(".chip[data-lvl]").forEach(function (chip) {
      chip.classList.toggle("on", !!filters[chip.dataset.lvl]);
    });
    document.querySelectorAll(".log-line").forEach(function (l) {
      l.style.display = filters[l.dataset.lvl] ? "" : "none";
    });
  }

  // Lowering the scrollback has to bite immediately, or the lines above the
  // new limit sit there until enough new ones arrive to push them out.
  function trimLog() {
    var log = $("log");
    if (!log) return;
    while (log.childNodes.length > logMax()) log.removeChild(log.firstChild);
  }
  var filters = { info: true, ok: true, warn: true, err: true };
  var logCount = 0;

  function stamp() {
    var d = new Date(), p = function (x) { return (x < 10 ? "0" : "") + x; };
    return p(d.getHours()) + ":" + p(d.getMinutes()) + ":" + p(d.getSeconds());
  }

  function logLine(level, text) {
    var line = document.createElement("div");
    line.className = "log-line l-" + level;
    line.dataset.lvl = level;
    line.innerHTML = '<span class="ts">' + stamp() + "</span>" +
                     '<span class="lvl lvl-' + level + '">' + level + "</span>" +
                     '<span class="msg">' + esc(text) + "</span>";
    if (!filters[level]) line.style.display = "none";
    return line;
  }

  function addLog(level, text) { addLogBatch([{ l: level, t: text }]); }

  // A batch is ONE reflow, not one per line. Reading scrollHeight forces layout,
  // so doing it per line turned a burst into a burst of synchronous layouts --
  // the host now coalesces lines precisely so this can be paid once.
  function addLogBatch(items) {
    if (!items || !items.length) return;
    var log = $("log");
    logCount += items.length;

    // Only the tail can survive the scrollback trim, so building nodes for the rest
    // is work spent purely to throw it away -- which is the exact situation a
    // flood produces.
    var start = Math.max(0, items.length - logMax());

    // STICK TO THE BOTTOM ONLY IF ALREADY THERE. Unconditional scrolling meant
    // reading anything older than the last screen was impossible while the log
    // was live: scroll up to read, and the next line yanked you back down --
    // and with it any text you were part-way through selecting. Measured before
    // the append, because appending is what changes scrollHeight.
    //
    // The 4px slack absorbs sub-pixel scroll positions and zoom, where an exact
    // comparison is never quite true and the log would never follow again.
    var atBottom = (log.scrollHeight - log.scrollTop - log.clientHeight) < 4;
    var keepTop  = log.scrollTop;

    var frag = document.createDocumentFragment();
    for (var i = start; i < items.length; i++)
      frag.appendChild(logLine(items[i].l || "info", items[i].t || ""));
    log.appendChild(frag);

    // Trimming from the TOP moves everything up under a reader who is holding
    // position, so give back what the trim took.
    while (log.childNodes.length > logMax()) {
      var h = log.firstChild.offsetHeight || 0;
      log.removeChild(log.firstChild);
      if (!atBottom) keepTop -= h;
    }
    if (atBottom) log.scrollTop = log.scrollHeight;
    else          log.scrollTop = Math.max(0, keepTop);
    $("logCount").textContent = logCount;
  }

  document.querySelectorAll(".chip[data-lvl]").forEach(function (chip) {
    chip.addEventListener("click", function () {
      // Written through the settings rather than straight into `filters`,
      // so a chip you toggled is still toggled next launch -- and so the
      // settings modal and the chips can never disagree.
      var lvl = chip.dataset.lvl;
      var key = { info: "console.showInfo", ok: "console.showOk",
                  warn: "console.showWarn", err: "console.showErr" }[lvl];
      if (key) setSetting(key, !S(key));
    });
  });

  // ---- copying out of the log ----------------------------------------------
  // .log-line is display:flex, so the browser serialises each span as its own
  // block and a copied entry arrives as THREE lines -- timestamp, level, then
  // the message -- which makes pasting a few lines of console output somewhere
  // useless. On screen the flex layout is right; only the clipboard is wrong,
  // so this rewrites the clipboard rather than changing the layout.
  //
  // Whole rows, for every row the selection touches. Dragging across the middle
  // of a line and getting half a timestamp is not what anyone copying a log
  // wants; they want the lines they highlighted.
  function logRowText(el) {
    var ts  = el.querySelector(".ts");
    var lvl = el.querySelector(".lvl");
    var msg = el.querySelector(".msg");
    var L = (lvl ? lvl.textContent : "").toUpperCase();
    while (L.length < 4) L += " ";
    return (ts ? ts.textContent : "") + "  " + L + "  " + (msg ? msg.textContent : "");
  }

  $("log").addEventListener("copy", function (e) {
    var sel = window.getSelection();
    if (!sel || sel.isCollapsed || !e.clipboardData) return;

    var out = [];
    var rows = $("log").querySelectorAll(".log-line");
    for (var i = 0; i < rows.length; i++) {
      // Filtered-out lines are display:none. They cannot be selected, but they
      // ARE still in the DOM, and containsNode does not care about visibility.
      if (rows[i].style.display === "none") continue;
      if (!sel.containsNode(rows[i], true)) continue;
      out.push(logRowText(rows[i]));
    }
    if (!out.length) return;                 // selection was not inside any row

    e.clipboardData.setData("text/plain", out.join("\n"));
    e.preventDefault();
  });

  // Through the HOST, not locally: the history it holds is what both this
  // view and the in-game panel replay from, so clearing here alone came back
  // on the next replay and left the two showing different logs. The host
  // answers with LOGCLEAR to both.
  $("btnClear").addEventListener("click", function () { send("LOGCLEAR"); });

  $("consoleHead").addEventListener("click", function (e) {
    if (e.target.closest(".chip, .link-btn")) return;
    var col = $("console").classList.toggle("collapsed");
    // The grip sits above the console, so no CSS sibling selector can reach it
    // from .collapsed -- it has to be told.
    $("consoleGrip").classList.toggle("off", col);
  });

  // ---- console resize -------------------------------------------------------
  // The console's height is a token (--h-console), not a flex basis, so a drag
  // only has to rewrite that one property and every rule reading it follows.

  // Floor keeps the head reachable: dragging the console down to nothing is
  // what the collapse toggle is for, and a pane you can no longer grab is a
  // trap. MIN_EDITOR is the same idea pointing the other way.
  var CONSOLE_MIN_H = 34, MIN_EDITOR = 80;

  // Live, because it depends on the window. Not a setting bound: a fixed
  // maximum would either be wrong on a small window or pointless on a large one.
  function consoleMaxH() {
    var main = document.querySelector(".main");
    var bb   = document.querySelector(".bottombar");
    // Before the first layout clientHeight is 0. Clamping against that would
    // pin the console to its floor, and nothing would recompute it until the
    // window happened to be resized -- so treat "not measured yet" as no limit.
    if (!main || !main.clientHeight) return Infinity;
    return Math.max(CONSOLE_MIN_H,
      main.clientHeight - MIN_EDITOR - 5 -
      ($("tabstrip") ? $("tabstrip").offsetHeight : 0) -
      (bb ? bb.offsetHeight : 0));
  }

  // Clamps what is APPLIED, never what is stored. A console that does not fit
  // this window still fits a bigger one, and silently shrinking the setting
  // would lose the user's real choice the first time they resized small.
  function applyConsoleHeight() {
    var px = Math.max(CONSOLE_MIN_H, Math.min(S("console.height"), consoleMaxH()));
    document.documentElement.style.setProperty("--h-console", px + "px");
  }

  (function () {
    var grip = $("consoleGrip"), con = $("console");
    if (!grip || !con) return;
    var startY = 0, startH = 0, live = 0;

    grip.addEventListener("pointerdown", function (e) {
      if (e.button !== 0 || con.classList.contains("collapsed")) return;
      e.preventDefault();
      startY = e.clientY;
      startH = con.getBoundingClientRect().height;
      live   = startH;
      // Capture so the drag survives the pointer leaving the 5px strip --
      // which it does immediately, since the strip moves with the pointer.
      grip.setPointerCapture(e.pointerId);
      grip.classList.add("dragging");
      con.classList.add("resizing");
      // The editor's textarea would otherwise start selecting text under the
      // pointer as the console grows over it.
      document.body.style.userSelect = "none";
    });

    grip.addEventListener("pointermove", function (e) {
      if (!grip.classList.contains("dragging")) return;
      // Up is a bigger console, hence the inverted delta.
      live = Math.max(CONSOLE_MIN_H,
                      Math.min(startH + (startY - e.clientY), consoleMaxH()));
      document.documentElement.style.setProperty("--h-console", live + "px");
    });

    function end(e) {
      if (!grip.classList.contains("dragging")) return;
      grip.classList.remove("dragging");
      con.classList.remove("resizing");
      document.body.style.userSelect = "";
      if (e && e.pointerId !== undefined && grip.hasPointerCapture(e.pointerId))
        grip.releasePointerCapture(e.pointerId);
      // Written once at the end rather than on every move: each write is a
      // localStorage round trip and a full applySettings pass.
      setSetting("console.height", Math.round(live));
    }
    grip.addEventListener("pointerup", end);
    grip.addEventListener("pointercancel", end);

    grip.addEventListener("dblclick", function () {
      setSetting("console.height", settingDefs["console.height"].def);
    });
  })();

  // ---- sidebar resize -------------------------------------------------------
  // Same shape as the console splitter, on the other axis. Kept as its own
  // block rather than generalised into one: the two share about six lines, and
  // the clamps, the settings key and the axis all differ.
  var SIDEBAR_MIN_W = 140, MIN_MAIN = 420;

  function sidebarMaxW() {
    var body = document.querySelector(".body");
    // Before first layout clientWidth is 0; clamping against that would pin the
    // sidebar to its floor with nothing to recompute it. Same trap as the
    // console's.
    if (!body || !body.clientWidth) return Infinity;
    return Math.max(SIDEBAR_MIN_W, body.clientWidth - MIN_MAIN - 5);
  }

  // Clamps what is applied, never what is stored -- a width that does not fit
  // this window still fits a bigger one.
  function applySidebarWidth() {
    var px = Math.max(SIDEBAR_MIN_W, Math.min(S("sidebar.width"), sidebarMaxW()));
    document.documentElement.style.setProperty("--set-sidebar-w", px + "px");
  }

  (function () {
    var grip = $("sidebarGrip"), body = document.querySelector(".body");
    if (!grip || !body) return;
    var startX = 0, startW = 0, live = 0;

    grip.addEventListener("pointerdown", function (e) {
      if (e.button !== 0) return;
      e.preventDefault();
      startX = e.clientX;
      startW = document.querySelector(".sidebar").getBoundingClientRect().width;
      live   = startW;
      grip.setPointerCapture(e.pointerId);
      grip.classList.add("dragging");
      body.classList.add("resizing");
      document.body.style.userSelect = "none";
    });

    grip.addEventListener("pointermove", function (e) {
      if (!grip.classList.contains("dragging")) return;
      live = Math.max(SIDEBAR_MIN_W,
                      Math.min(startW + (e.clientX - startX), sidebarMaxW()));
      document.documentElement.style.setProperty("--set-sidebar-w", live + "px");
      // The minimap is a canvas sized from its own box, so it has to be told
      // the layout moved -- it does not reflow itself.
      if (typeof scheduleMinimap === "function") scheduleMinimap();
    });

    function end(e) {
      if (!grip.classList.contains("dragging")) return;
      grip.classList.remove("dragging");
      body.classList.remove("resizing");
      document.body.style.userSelect = "";
      if (e && e.pointerId !== undefined && grip.hasPointerCapture(e.pointerId))
        grip.releasePointerCapture(e.pointerId);
      setSetting("sidebar.width", Math.round(live));
    }
    grip.addEventListener("pointerup", end);
    grip.addEventListener("pointercancel", end);

    grip.addEventListener("dblclick", function () {
      setSetting("sidebar.width", settingDefs["sidebar.width"].def);
    });
  })();

  // Shrinking the window can leave either pane larger than its live maximum.
  window.addEventListener("resize", function () {
    applyConsoleHeight();
    applySidebarWidth();
  });

  // ---- run / arm ----------------------------------------------------------
  var attachedPid = 0;
  // Is a Roblox client running at all, attached or not? Straight from the
  // host's own find_client(); see client_running() in attach.cpp.
  var clientRunning = false;
  var armed = false, running = false;
  // Mirrors the host's own refusal, so the page can say why before you click
  // rather than after. The host is still the one that enforces it.
  var offsetsStale = false;

  // What is running, and what was asked for while it still was.
  //
  // The host serialises runs -- one g_stop flag, one keyboard hook, one loaded
  // DLL at a time -- so a second run cannot simply start alongside the first.
  // Pressing Run on the SAME thing means "stop it"; pressing it on a DIFFERENT
  // one means "run that instead", which is what an editor does and what the
  // toggle used to throw away.
  // Live runs, from the host's RUNSTATE list: [{id,name,building,stopping}].
  // Several snippets run at once now, so this replaces the old single
  // runningWhat: `running` is derived (any run live), and the Run/Stop button
  // reads the state of the CURRENTLY OPEN snippet against this list.
  var liveRuns = [];
  function runsNamed(name) {
    return liveRuns.filter(function (r) { return r.name === name; });
  }
  // The label the host runs a snippet under is its base file name.
  function currentRunName() { return currentName ? baseName(currentName) : "(unsaved)"; }
  function currentLiveRun() { var m = runsNamed(currentRunName()); return m.length ? m[0] : null; }
  // The disk version we last warned about for the open file, so the warning
  // fires once per change rather than on every push. Cleared whenever the
  // editor and disk agree again.
  // The file's content on disk as of the last time the editor and disk
  // agreed -- opened, reloaded, or saved. Compared against a NEW push to
  // answer "did the file change underneath me", which is a different
  // question from "does my buffer differ from disk". The latter is true
  // the instant you type anything, which is why warning on it fired on
  // every first edit to every file.
  var diskSeen = null;

  // A run to start once a condition on the live list is met. `waitEmpty` waits
  // for every run to end (the "stop it and run this" answer); otherwise it
  // waits for the named run to be gone (the "restart" answer).
  var pendingRun = null;     // { name, waitEmpty, go }

  // The Run/Stop button reflects the OPEN snippet: Stop if it is live, Run
  // otherwise. `running` stays as a derived "anything live" flag for the
  // places that only care whether the process is busy.
  function refreshRunButton() {
    running = liveRuns.length > 0;
    // Run always means run. Stop always means stop. Neither label changes with
    // state -- a control that means something different depending on what is
    // live is the one you press by muscle memory and regret.
    //
    // WRITE THE .lbl, NOT THE BUTTON. Both carry an icon as a sibling of the
    // text now, so assigning textContent (or innerHTML) here would delete it.
    var st = $("btnStop");
    st.disabled = !running;
    st.querySelector(".lbl").textContent =
      liveRuns.length > 1 ? "Stop (" + liveRuns.length + ")" : "Stop";
    st.title = running ? liveRuns.map(function (r) { return r.name; }).join(", ") : "";
    st.classList.toggle("armed", running);
    $("pulse").classList.toggle("firing", running);
  }
  // Back-compat shim: a few call sites still call setRunning() optimistically
  // right before send("RUN..."). The authoritative state now arrives over
  // RUNSTATE, so this just refreshes the button from the current list.
  function setRunning() { refreshRunButton(); }

  // ---- the run/stop chooser --------------------------------------------------
  // A centred dialog rather than a menu hanging off the Run button. The old
  // dropdown put a decision about FOUR possible runs in a strip anchored to one
  // control, which read as a button's own menu instead of a question about the
  // session.
  //
  // opts: { title, sub, rows:[{label, sub, id}], multi, actions:[{label,
  //         primary, danger, run(selectedIndices)}] }
  var runDlg = null;

  function closeRunDlg() {
    $("runOverlay").classList.remove("open");
    runDlg = null;
  }

  function openRunDlg(o) {
    runDlg = o;
    $("runDlgTitle").textContent = o.title;
    $("runDlgSub").textContent = o.sub || "";
    $("runDlgAll").hidden = !o.multi;

    var list = $("runDlgList");
    list.innerHTML = "";
    // No rows means no list: .export-list carries a 90px min-height for the
    // trash and export pickers, and on a question with only buttons it left a
    // blank slab between the title and the footer.
    list.classList.toggle("empty", !(o.rows && o.rows.length));
    (o.rows || []).forEach(function (r, i) {
      var el = document.createElement("div");
      el.className = "export-item";
      el.dataset.i = i;
      el.innerHTML = (o.multi ? '<span class="run-chk"></span>' : "") +
                     '<span>' + esc(r.label) + "</span>" +
                     (r.sub ? '<span class="tag run-stopping">' + esc(r.sub) + "</span>" : "");
      list.appendChild(el);
    });

    var foot = $("runDlgFoot");
    foot.innerHTML = "";
    o.actions.forEach(function (a, i) {
      var b = document.createElement("button");
      b.className = "btn" + (a.primary ? " btn-primary" : "") + (a.danger ? " danger" : "");
      b.textContent = a.label;
      b.dataset.a = i;
      foot.appendChild(b);
    });
    $("runOverlay").classList.add("open");
  }

  function runDlgSelected() {
    var out = [];
    var rows = $("runDlgList").querySelectorAll(".export-item");
    for (var i = 0; i < rows.length; i++)
      if (rows[i].querySelector(".run-chk.on")) out.push(i);
    return out;
  }

  $("runOverlay").addEventListener("click", function (e) {
    if (e.target === $("runOverlay")) { closeRunDlg(); return; }   // click the scrim
    var row = e.target.closest(".export-item");
    if (row && runDlg && runDlg.multi) {
      var c = row.querySelector(".run-chk");
      if (c) c.classList.toggle("on");
      return;
    }
    var btn = e.target.closest("#runDlgFoot .btn");
    if (btn && runDlg) {
      var a = runDlg.actions[+btn.dataset.a];
      var sel = runDlg.multi ? runDlgSelected() : [];
      if (a && a.run) a.run(sel);
      return;
    }
    if (e.target.id === "runDlgAll") {
      var any = runDlgSelected().length === 0;
      var rows = $("runDlgList").querySelectorAll(".run-chk");
      for (var i = 0; i < rows.length; i++) rows[i].classList.toggle("on", any);
    }
  });

  // Deciding what a Run gesture means against the live list. `what` is
  // "snippet:<name>" / "project:<g>"; go() actually starts it.
  function requestRun(what, go) {
    var label = baseName(what.replace(/^snippet:/, "").replace(/^project:/, ""));
    var same = runsNamed(label);
    if (same.length) {
      // The SAME snippet again. Two copies writing one field at 60 Hz is almost
      // never intended, so this is a restart question, not a silent second run.
      openRunDlg({
        title: label + " is already running",
        sub: "two copies writing one field is almost never intended",
        actions: [
          { label: "Restart", primary: true, run: function () {
              pendingRun = { name: label, waitEmpty: false, go: go };
              send("STOP:" + same[0].id);
              closeRunDlg();
          } },
          { label: "Cancel", run: closeRunDlg }
        ]
      });
      return;
    }
    if (liveRuns.length > 0) {
      // A DIFFERENT snippet is live. Concurrency is opt-in: offer both, or the
      // old stop-then-start, or nothing. (The destructive answer is never the
      // default.)
      openRunDlg({
        title: "Something is already running",
        sub: liveRuns.map(function (r) { return r.name; }).join(", "),
        actions: [
          // Concurrency is opt-in and the destructive answer is never the
          // default -- "run both" is what the user pressing Run just asked for.
          // "Run" -- not "Run both". The count is not always two, and the
          // button answers the question the user already asked by pressing Run.
          { label: "Run", primary: true, run: function () { closeRunDlg(); go(); } },
          { label: "Stop the others and run this", run: function () {
              pendingRun = { name: null, waitEmpty: true, go: go };
              send("STOP");
              closeRunDlg();
          } },
          { label: "Cancel", run: closeRunDlg }
        ]
      });
      return;
    }
    go();
  }

  // The Stop gesture. One live run needs no question -- a dialog to confirm the
  // only possible answer is friction, not safety. Two or more is a real choice,
  // so it is multi-select with stop-all beside it rather than one-at-a-time.
  function requestStop() {
    if (!liveRuns.length) return;
    if (liveRuns.length === 1) { send("STOP:" + liveRuns[0].id); return; }
    var snapshot = liveRuns.slice();
    openRunDlg({
      title: "Stop which?",
      sub: snapshot.length + " running",
      multi: true,
      rows: snapshot.map(function (r) {
        return { label: r.name, sub: r.stopping ? "stopping…" : "" };
      }),
      actions: [
        { label: "Stop selected", primary: true, run: function (sel) {
            // Against the SNAPSHOT, not liveRuns: the list can change while the
            // dialog is open, and stopping by row index against a list that has
            // shifted would stop the wrong snippet.
            sel.forEach(function (i) { if (snapshot[i]) send("STOP:" + snapshot[i].id); });
            closeRunDlg();
        } },
        { label: "Stop all", danger: true, run: function () { send("STOP"); closeRunDlg(); } },
        { label: "Cancel", run: closeRunDlg }
      ]
    });
  }

  // Called whenever RUNSTATE arrives: refresh the button, the sidebar markers,
  // and fire a queued run if its condition is now met.
  function applyRunState(runs) {
    liveRuns = runs || [];
    refreshRunButton();
    paintRunning();
    if (pendingRun) {
      var ready = pendingRun.waitEmpty
                ? liveRuns.length === 0
                : runsNamed(pendingRun.name).length === 0;
      if (ready) { var p = pendingRun; pendingRun = null; p.go(); }
    }
  }

  // Light up the sidebar rows whose snippet is live (or stopping). Patches the
  // DOM in place rather than re-rendering, so hover and selection are untouched.
  function paintRunning() {
    var byName = {};
    (liveRuns || []).forEach(function (r) { byName[r.name] = r; });
    var rows = document.querySelectorAll(".script-row");
    for (var i = 0; i < rows.length; i++) {
      var key = rows[i].dataset.key || "";
      var r = byName[baseName(key)];
      rows[i].classList.toggle("running", !!r);
      rows[i].classList.toggle("stopping", !!(r && r.stopping));
    }
  }

  function runProject(g) {
    requestRun("project:" + g, function () {
      setRunning(true);
      addLog("info", "running project " + g);
      send("RUN_PROJECT:" + g);
    });
  }

  function run() {
    // Run no longer doubles as Stop: pressing it while the open snippet is live
    // is a RESTART question, and requestRun asks it. Stop has its own button.

    // F5 bypasses the disabled button, so the guard lives here too.
    if (offsetsStale) {
      addLog("err", "x offsets do not match this client - running is blocked");
      return;
    }

    // Running a file that belongs to a project builds the PROJECT. The
    // alternative -- refusing, and telling you to go press the other button --
    // punishes you for pressing the obvious one, and the file on its own would
    // not link anyway since its siblings hold the symbols it calls.
    var owner = current >= 0 && snippets[current]
              ? projectOwning(snippets[current].g || "") : "";
    if (owner) {
      if (dirty) save();          // build what is on screen, not what was saved
      runProject(owner);
      return;
    }

    // A header outside a project has no entry point and never will. Saying so
    // beats a page of compiler output about a missing fx_run.
    if (current >= 0 && snippets[current] && isHeader(snippets[current].n)) {
      addLog("warn", baseName(snippets[current].n) +
                     " is a header - open a .cpp, or put it in a project");
      return;
    }

    if (!input.value.trim()) { addLog("warn", "empty snippet"); return; }

    // Keyed by name so pressing Run twice on the same snippet toggles, while
    // pressing it on a different one swaps. The BODY is read at send time, not
    // now, so a queued run compiles whatever is in the editor when it starts.
    var name = currentName || "(unsaved)";
    requestRun("snippet:" + name, function () {
      addLog("info", "running " + baseName(name));
      // The host has no idea what this snippet is called -- RUN: carries a body
      // and nothing else -- and the in-game panel has no editor to infer it
      // from. Sent as a label first; it starts nothing on its own.
      send("RUNNAME:" + baseName(name));
      send("RUN:" + input.value);
    });
  }

  // The Run/Stop button. When the OPEN snippet is live it reads "Stop" and
  // stops THAT run (not everything -- other snippets keep going). Otherwise it
  // is Run, and run() applies the concurrency prompt.
  $("btnRun").addEventListener("click", function () { run(); });
  $("btnStop").addEventListener("click", function () { requestStop(); });

  // ---- the arm gate ---------------------------------------------------------
  // Everything here can only ever make arming HARDER to enter or shorter to
  // stay in. Nothing below may become a way to arm without meaning to.
  var disarmTimer = null;

  function cancelAutoDisarm() {
    if (disarmTimer) clearTimeout(disarmTimer);
    disarmTimer = null;
  }

  // Started on arming, cancelled on disarming. Reading the setting at arm time
  // rather than holding a copy means changing it takes effect on the next arm
  // instead of the next launch.
  function startAutoDisarm() {
    cancelAutoDisarm();
    var mins = S("safety.autoDisarmMin");
    if (!mins) return;
    disarmTimer = setTimeout(function () {
      disarmTimer = null;
      if (!armed) return;                 // beaten to it by a manual disarm
      armed = false;
      send("ARM:0");
      addLog("warn", "auto-disarmed after " + mins +
                     (mins === 1 ? " minute" : " minutes"));
    }, mins * 60 * 1000);
  }

  function setArmed(on) {
    armed = on;
    send("ARM:" + (on ? "1" : "0"));
    if (on) startAutoDisarm(); else cancelAutoDisarm();
  }

  $("btnArm").addEventListener("click", function () {
    if (!armed) {
      // Only the ARMING direction is confirmed. Making disarming ask too would
      // put a dialog between the user and the safe state, which is backwards.
      if (S("safety.confirmArm")) {
        var mins = S("safety.autoDisarmMin");
        if (!confirm("Arm the write gate?\n\n" +
                     "While armed, fx_wrblk writes reach the game for real. " +
                     "Disarmed, every write is logged and refused." +
                     (mins ? "\n\nWill auto-disarm after " + mins + " min." : ""))) return;
      }
      setArmed(true);
    } else {
      setArmed(false);
    }
  });

  // OPENS THE PANEL; it does not attach or detach by itself. It used to be a
  // button that toggled, which meant the one control for "which client am I
  // on" could only ever say yes or no to the client it had already chosen --
  // there was nowhere to answer "that one".
  $("btnAttach").addEventListener("click", function () {
    if (candIsOpen()) closeCand(); else openCand();
  });

  // Click on the attach readout (sidebar footer) opens the same panel. It only
  // looks clickable in a multi-instance session, which is when re-picking is a
  // thing you want, but the panel is the right answer either way.
  $("footState").addEventListener("click", function () {
    if (!candMulti) return;
    openCand();
  });

  // Escape closes the candidate picker without attaching. The user still has
  // to pick before the app can attach; nothing else changes state.
  document.addEventListener("keydown", function (e) {
    if (e.key === "Escape" && candIsOpen()) {
      e.preventDefault();
      closeCand();
    }
  });

  document.addEventListener("keydown", function (e) {
    if (e.key === "F5") { e.preventDefault(); run(); }
    else if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "s") { e.preventDefault(); save(); }
    else if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "k") { e.preventDefault(); openPalette(); }
    // Ctrl+, is the settings chord everywhere else, so it is the one here too.
    else if ((e.ctrlKey || e.metaKey) && e.key === ",") { e.preventDefault(); openSettings(); }
    else if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "f") { e.preventDefault(); openFind(false); }
    else if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "h") { e.preventDefault(); openFind(true); }
    else if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "g") { e.preventDefault(); goToLine(); }
    // F3 works from the editor as well as the box, which is the point of it.
    else if (e.key === "F3") {
      e.preventDefault();
      if (!findOpen()) openFind(false);
      else findGo(findState.index + (e.shiftKey ? -1 : 1));
    }
    // Explorer's own set. Deliberately all on Alt: the editor has focus almost
    // all the time, and every unmodified key -- Backspace especially, which is
    // Explorer's other "up" -- belongs to whatever you are typing.
    else if (e.altKey && e.key === "ArrowLeft")  { e.preventDefault(); goBack(); }
    else if (e.altKey && e.key === "ArrowRight") { e.preventDefault(); goFwd(); }
    else if (e.altKey && e.key === "ArrowUp")    { e.preventDefault(); goUp(); }
    else if (e.altKey && e.key === "Home")       { e.preventDefault(); focusOn(""); }
    else if (e.key === "Escape") {
      // Innermost first. Trash and Export sit ON TOP of Settings (Export is
      // opened from it), so closing Settings underneath them would leave a
      // dialog floating over nothing.
      if ($("runOverlay").classList.contains("open"))          closeRunDlg();
      else if ($("trashOverlay").classList.contains("open"))  closeTrash();
      else if ($("exportOverlay").classList.contains("open")) closeExport();
      else if (settingsOpen())   closeSettings();
      else if (findOpen())       closeFind();
      else if ($("paletteOverlay").classList.contains("open")) closePalette();
      // AND NOTHING BEYOND THAT. Esc used to close the whole console to its
      // badge when nothing else was open, which made a key everyone presses to
      // back out of a dialog also the key that puts the console away -- one
      // press too many and the window was gone. Insert already toggles it, and
      // so does the badge; Esc dismisses what is on top of the console, never
      // the console itself.
    }
  });

  // ---- mouse back / forward -------------------------------------------------
  // Chromium reports the side buttons as 3 and 4, and its own default for them
  // is to walk the WebView's history. That history is one entry long, so the
  // best case is nothing happening and the worst is the app navigating off its
  // own page -- refused on every event that could carry it, not just the one
  // being acted on.
  function sideButton(e) {
    if (e.button !== 3 && e.button !== 4) return false;
    e.preventDefault();
    return true;
  }
  document.addEventListener("mousedown", function (e) {
    if (sideButton(e)) { if (e.button === 3) goBack(); else goFwd(); }
  });
  document.addEventListener("mouseup",  sideButton);
  document.addEventListener("auxclick", sideButton);

  // ---- window controls ----------------------------------------------------
  $("winClose").addEventListener("click", function () { send("WIN:CLOSE"); });
  $("winMin").addEventListener("click",   function () { send("WIN:MIN"); });
  $("winZoom").addEventListener("click",  function () { send("WIN:ZOOM"); });
  // ---- game name lookup ---------------------------------------------------
  //
  // The DataModel has a PlaceId but no human-readable title -- Roblox stores
  // that on its web API, not in the client's memory. Two public endpoints,
  // no login required, cross-origin allowed (`Access-Control-Allow-Origin: *`
  // last time I checked):
  //
  //   1) apis.roblox.com/universes/v1/places/<placeId>/universe -> universeId
  //   2) games.roblox.com/v1/games?universeIds=<uid>            -> name
  //
  // Cache: undefined = never seen, null = in flight or failed, string = done.
  // Fetching from the picker means the app makes outbound requests to
  // roblox.com while the picker is open. Nothing sensitive is sent -- only
  // the PlaceId is in the URL -- but be aware the network activity happens.

  var placeNameCache = {};

  // The title landed. Fill every row showing this place, in place -- a full
  // re-render would drop the row under the pointer mid-hover.
  //
  // THE PLACE ID IS NOT IN THE LINE ANY MORE. The picker put it there because
  // it was the only thing guaranteed to be readable; the panel's rows are
  // already identified by the account above them, and "· place 8737899170"
  // beside a game title was a number nobody reads. It lives on the row's
  // tooltip with the pid.
  function refreshPlaceLabels(placeId) {
    var els = document.querySelectorAll('.att-place[data-place-id="' + placeId + '"]');
    var name = placeNameCache[placeId];
    els.forEach(function (el) {
      var raw = el.dataset.rawPlace || "";
      if (name) el.textContent = name;
      else      el.textContent = raw === "Ugc" ? "in a game" : raw;
    });
  }

  function lookupPlaceName(placeId) {
    if (placeNameCache[placeId] !== undefined) return;   // done or in flight
    placeNameCache[placeId] = null;
    // Host-side WinHTTP -- WebView2's file:// origin gets CORS-blocked by
    // Roblox's endpoints and page fetch() fails silently. The host replies
    // via PLACE_NAME:<pid>|<name>, handled below.
    send("LOOKUP_PLACE:" + placeId);
  }

  // ---- the clients panel --------------------------------------------------
  //
  // Slides in from the right edge. Opened by the toolbar's Roblox mark, and by
  // the host (OPEN_PICKER:) when an attach finds several clients and needs one
  // picked. That second case is all the centred modal this replaced ever did;
  // it is now one way into a panel rather than a component of its own.
  //
  // A row is avatar + display name + @username + place, with Detach on the one
  // we are attached to. Clicking any other row sends ATTACH_PID:<pid>.
  //
  // `candCache` holds the last list the host sent, so a re-render -- a place
  // title resolving, a headshot landing -- costs nothing.

  var candCache = [];
  var candMulti = false;   // stays true once we've seen >=2 instances, so the
                           // sidebar readout shows a caret afterwards.
  var avatarCache = {};    // userId -> url. undefined = never asked.

  function candIsOpen() { return $("attPanel").classList.contains("open"); }

  function openCand() {
    $("attPanel").classList.add("open");
    $("attScrim").classList.add("open");
    // ASK EVERY TIME. The host only pushes CANDIDATES unprompted at 2+ clients,
    // so in the normal single-client case the cache is empty and the panel
    // would open blank. Cheap: one process walk, and it renders what it has in
    // the meantime.
    requestClients();
  }
  function closeCand() {
    $("attPanel").classList.remove("open");
    $("attScrim").classList.remove("open");
  }

  function requestClients() {
    $("attRefresh").classList.add("spin");
    send("CLIENTS");
  }

  $("attScrim").addEventListener("click", closeCand);
  $("attClose").addEventListener("click", closeCand);
  $("attRefresh").addEventListener("click", requestClients);
  // ---- launching -----------------------------------------------------------
  //
  // THE PAGE DRIVES THE SPINNER, because the host cannot. launch_roblox() hands
  // a `roblox-player:` URI to the shell and returns; whichever bootstrapper is
  // registered then does its own thing -- which on an update is minutes -- and
  // nothing reports back. So the only honest end condition is A CLIENT EXISTING,
  // which the host already tells us on every state push.
  //
  // The timeout is NOT how long a launch takes -- a bootstrapper on an update
  // runs for minutes, and the client turning up is what ends the spin. It is
  // how long the click is worth acknowledging for. 15 seconds: long enough to
  // see it, short enough that a cancelled launch does not leave the toolbar
  // spinning at you.
  var launching = false, launchTimer = 0;
  // Whether a client was ALREADY running when this launch started. Without it
  // "a client exists" ends the spin instantly on the second-client case -- you
  // press Launch with one client up, the condition is already true on the next
  // state push, and the button never spins at all.
  var launchHadClient = false;

  function setLaunching(on) {
    launching = on;
    $("btnLaunch").classList.toggle("loading", on);
    $("attLaunch").classList.toggle("loading", on);
    clearTimeout(launchTimer);
    if (on) launchTimer = setTimeout(function () { setLaunching(false); }, 15000);
  }

  function doLaunch() {
    // A second press cannot help: the first URI is already with the shell, and
    // sending another only risks a client you did not ask for.
    if (launching) return;
    launchHadClient = clientRunning || !!attachedPid;
    setLaunching(true);
    send("LAUNCH_ROBLOX");
  }

  $("attLaunch").addEventListener("click", doLaunch);

  function lookupAvatar(uid) {
    if (!uid || avatarCache[uid] !== undefined) return;   // done or in flight
    avatarCache[uid] = null;
    send("LOOKUP_AVATAR:" + uid);
  }

  // The headshot landed. Fill every row already showing this account rather
  // than re-rendering: a rebuild would drop the row under the pointer.
  function applyAvatar(uid, url) {
    var els = document.querySelectorAll('.att-pfp[data-uid="' + uid + '"]');
    els.forEach(function (el) {
      if (el.querySelector("img")) return;
      var im = document.createElement("img");
      im.src = url;
      im.alt = "";
      // The initial stays underneath until the bytes actually decode, so a
      // dead URL leaves a letter rather than an empty circle.
      im.addEventListener("error", function () { im.remove(); });
      el.appendChild(im);
    });
  }

  function renderCand(list) {
    candCache = list || [];
    if (candCache.length >= 2) candMulti = true;
    $("attRefresh").classList.remove("spin");

    var container = $("attList");
    container.innerHTML = "";

    $("attCount").textContent = candCache.length
      ? (candCache.length + (candCache.length === 1 ? " client" : " clients"))
      : "";

    if (!candCache.length) {
      var none = document.createElement("div");
      none.className = "att-empty";
      none.textContent = "No Roblox client running.";
      container.appendChild(none);
      return;
    }

    candCache.forEach(function (c) {
      var mine = !!attachedPid && c.pid === attachedPid;

      var row = document.createElement("div");
      row.className = "att-row" + (c.ok ? "" : " disabled") + (mine ? " current" : "");
      row.title = "pid " + c.pid + (c.mon ? ("  ·  monitor " + c.mon) : "");

      // No username is USUALLY not a failure. Players.LocalPlayer does not
      // exist until you join a place, so a client sitting in the Roblox app has
      // a perfectly readable DataModel -- named "LuaApp" -- and simply nobody
      // to name. "(unknown)" made that indistinguishable from a client we could
      // not read at all, which is the case that actually needs attention.
      var label = c.user
        ? c.user
        : (!c.ok                 ? "(cannot read this client)"
        :  c.place === "LuaApp"  ? "in the Roblox menu"
        :                          "loading…");

      var pfp = document.createElement("div");
      pfp.className = "att-pfp";
      pfp.textContent = label.charAt(0).toUpperCase();
      if (c.uid) {
        pfp.dataset.uid = c.uid;
        if (avatarCache[c.uid]) applyAvatar(c.uid, avatarCache[c.uid]);
        else                    lookupAvatar(c.uid);
      }

      var meta = document.createElement("div");
      meta.className = "att-meta";

      // DISPLAY NAME BIG, @username small beside it -- Roblox's own ordering,
      // and the one the panel is read with: you recognise the display name and
      // you only need the handle to tell two similar ones apart. Shown only
      // when they differ, which for most accounts they do not.
      var name = document.createElement("div");
      name.className = "att-name";
      name.textContent = label;
      if (c.disp && c.disp !== c.user) {
        var at = document.createElement("span");
        at.className = "at";
        at.textContent = "@" + c.disp;
        name.appendChild(at);
      }
      meta.appendChild(name);

      var place = document.createElement("div");
      place.className = "att-place";
      if (c.place) {
        // Tagged so refreshPlaceLabels can find this element again once the
        // async title fetch resolves.
        if (c.placeId) {
          place.dataset.placeId  = c.placeId;
          place.dataset.rawPlace = c.place;
        }
        if (c.place === "LuaApp") {
          place.textContent = "Roblox menu";
        } else if (c.placeId) {
          var cached = placeNameCache[c.placeId];
          if (cached) {
            place.textContent = cached;
          } else {
            place.textContent = c.place === "Ugc" ? "in a game" : c.place;
            lookupPlaceName(c.placeId);
          }
        } else {
          place.textContent = c.place;
        }
      } else if (!c.ok) {
        place.textContent = "access denied";
      }
      meta.appendChild(place);

      row.appendChild(pfp);
      row.appendChild(meta);

      if (mine) {
        // Detach lives on the attached row, not in the footer: it is an action
        // ON THIS CLIENT, and a footer button would have to name which.
        var det = document.createElement("button");
        det.className = "att-act danger";
        det.textContent = "Detach";
        det.title = "Release the client. Nothing can be read or written until you attach again.";
        det.addEventListener("click", function (e) {
          e.stopPropagation();       // the row would re-attach us underneath
          send("DETACH");
        });
        row.appendChild(det);
      } else if (c.ok) {
        row.addEventListener("click", function () {
          send("ATTACH_PID:" + c.pid);
          closeCand();
        });
      }

      container.appendChild(row);
    });
  }

  // ---- offset palette -----------------------------------------------------
  var offsets = [];      // [[key, value], ...]
  var palSel = 0, palRows = [];

  // ---- settings modal -------------------------------------------------------
  // Rows are generated from SETTINGS, never hand-written, so a new setting
  // appears here the moment it is added to the table.
  var setCat = SETTING_CATS[0];

  function settingsOpen() { return $("settingsOverlay").classList.contains("open"); }

  function openSettings() {
    $("settingsOverlay").classList.add("open");
    $("setSearch").value = "";
    renderSettings();
    setTimeout(function () { $("setSearch").focus(); }, 0);
  }
  function closeSettings() { $("settingsOverlay").classList.remove("open"); }

  function settingMatches(s, q) {
    if (!q) return true;
    return (s.label + " " + s.desc + " " + s.k + " " + s.cat).toLowerCase()
             .indexOf(q) >= 0;
  }

  function renderSettings() {
    var q = $("setSearch").value.trim().toLowerCase();
    var hits = SETTINGS.filter(function (s) { return settingMatches(s, q); });

    // Categories, each with how many of its settings survived the search.
    var cats = $("setCats");
    cats.innerHTML = "";
    SETTING_CATS.forEach(function (c) {
      var n = hits.filter(function (s) { return s.cat === c; }).length;
      var el = document.createElement("div");
      // The kit's .nav-item, so hover and .active come from the same component
      // the sidebar's nav uses rather than being reinvented here.
      //
      // While searching, this list stops being a selector and becomes a map of
      // where the matches are -- so nothing is marked active and empties dim.
      el.className = "nav-item" + (!q && c === setCat ? " active" : "") + (n ? "" : " empty");
      el.innerHTML = '<span></span><span class="n"></span>';
      el.firstChild.textContent = c;
      el.lastChild.textContent = q ? String(n) : "";
      el.addEventListener("click", function () {
        setCat = c;
        $("setSearch").value = "";      // picking a category ends the search
        renderSettings();
      });
      cats.appendChild(el);
    });

    // Searching shows every match across all categories; otherwise just the
    // selected one.
    var shown = q ? hits : hits.filter(function (s) { return s.cat === setCat; });
    var list = $("setList");
    list.innerHTML = "";

    if (!shown.length) {
      var none = document.createElement("div");
      none.className = "set-empty";
      none.textContent = 'Nothing matches "' + q + '".';
      list.appendChild(none);
    }

    var lastCat = null;
    shown.forEach(function (s) {
      if (q && s.cat !== lastCat) {
        lastCat = s.cat;
        // The kit's own section heading, as used in the command palette.
        var h = document.createElement("div");
        h.className = "palette-sec";
        h.textContent = s.cat;
        list.appendChild(h);
      }
      list.appendChild(settingRow(s));
    });

    updateSettingCount();
    applySettingDims();
  }

  // Split out of renderSettings so changing a value can refresh the footer
  // WITHOUT rebuilding the list -- see settingRow().
  function updateSettingCount() {
    var q = $("setSearch").value.trim().toLowerCase();
    var hits = SETTINGS.filter(function (s) { return settingMatches(s, q); });
    var mod = SETTINGS.filter(function (s) {
      return !isAction(s) && settings[s.k] !== s.def;
    }).length;
    $("setCount").textContent =
      (q ? hits.length + " of " + SETTINGS.length + ' match "' + q + '"'
         : SETTINGS.length + " settings") +
      (mod ? "  ·  " + mod + " changed" : "");
  }

  // Everything a value change needs to update, and nothing more: the row's own
  // modified mark and the footer tally. No node is created or destroyed, so a
  // control mid-transition keeps animating.
  function touchSettingRow(row, s) {
    row.classList.toggle("modified", settings[s.k] !== s.def);
    updateSettingCount();
    // A row can turn another row inert (dimIf), and that has to show at
    // the moment of the click -- not on the next time Settings is opened.
    applySettingDims();
  }

  // Grey the rows whose controlling setting makes them do nothing. In
  // place rather than by re-rendering: rebuilding the list would replace
  // the switch that was just clicked, and it would jump to its final
  // state instead of sliding.
  function applySettingDims() {
    SETTINGS.forEach(function (s) {
      if (!s.dimIf) return;
      var row = document.querySelector('.set-row[data-k="' + s.k + '"]');
      if (row) row.classList.toggle("dim", !!settings[s.dimIf]);
    });
  }

  // Kept apart from settingRow rather than branched inside it: an action
  // shares only the row shell, and threading "does this one have a value"
  // through the reset button, the modified mark and the control would cost the
  // common case more than the duplication costs here.
  function actionRow(s) {
    var row = document.createElement("div");
    row.className = "set-row";

    var label = document.createElement("div");
    label.className = "set-label";
    label.textContent = s.label;

    var desc = document.createElement("div");
    desc.className = "set-desc";
    desc.textContent = s.desc;

    var ctl = document.createElement("div");
    ctl.className = "set-ctl";
    var btn = document.createElement("button");
    btn.className = "btn set-action";
    btn.textContent = s.button;
    btn.addEventListener("click", function () {
      send(s.msg);
      // The host reports on the console, which this modal is covering. Closing
      // is the whole feedback path -- without it the click looks like it did
      // nothing until you happen to dismiss the dialog.
      closeSettings();
    });
    ctl.appendChild(btn);

    row.appendChild(label);
    row.appendChild(ctl);
    row.appendChild(desc);
    return row;
  }

  function settingRow(s) {
    if (isAction(s)) return actionRow(s);

    var row = document.createElement("div");
    var modified = settings[s.k] !== s.def;
    row.className = "set-row" + (modified ? " modified" : "");
    row.dataset.k = s.k;

    var label = document.createElement("div");
    label.className = "set-label";
    label.textContent = s.label;

    var reset = document.createElement("button");
    reset.className = "set-reset";
    reset.textContent = "reset";
    reset.title = "Back to " + s.def;
    reset.addEventListener("click", function () {
      setSetting(s.k, s.def);
      // The control has to be re-read from the setting here -- unlike a user
      // edit, this changes the value without the input knowing.
      if (s.type === "bool") input.checked = !!settings[s.k];
      else                   input.value = settings[s.k];
      touchSettingRow(row, s);
    });
    label.appendChild(reset);

    var desc = document.createElement("div");
    desc.className = "set-desc";
    desc.textContent = s.desc;

    var ctl = document.createElement("div");
    ctl.className = "set-ctl";
    var input = document.createElement("input");
    if (s.type === "bool") {
      input.type = "checkbox";
      input.checked = !!settings[s.k];
      input.addEventListener("change", function () {
        setSetting(s.k, input.checked);
        // NOT renderSettings(). Rebuilding the list replaces this very input
        // with a fresh one already in its final state, so the knob has nothing
        // to slide from and the switch appears to jump. Updating in place is
        // also simply correct: a value change alters no row's existence, only
        // its modified mark and the footer tally.
        touchSettingRow(row, s);
      });
    } else if (s.type === "text") {
      input.type = "text";
      input.value = settings[s.k];
      input.placeholder = s.placeholder || "none";
      input.spellcheck = false;
      input.className = "set-text";
      input.addEventListener("change", function () {
        setSetting(s.k, input.value);
        touchSettingRow(row, s);
      });
      // Or the app-wide chords fire while you are typing a flag.
      input.addEventListener("keydown", function (e) { e.stopPropagation(); });
    } else {
      input.type = "number";
      input.value = settings[s.k];
      if (s.min !== undefined) input.min = s.min;
      if (s.max !== undefined) input.max = s.max;
      input.step = s.step || 1;
      // On `change`, not `input`: applying per keystroke means typing "150"
      // passes through 1 and 15, and clampSetting would snap each of those to
      // the minimum before the third digit arrived.
      input.addEventListener("change", function () {
        setSetting(s.k, input.value);
        input.value = settings[s.k];       // show what was actually accepted
        touchSettingRow(row, s);
      });
    }
    ctl.appendChild(input);

    row.appendChild(label);
    row.appendChild(ctl);
    row.appendChild(desc);
    return row;
  }

  $("setSearch").addEventListener("input", renderSettings);
  $("setSearch").addEventListener("keydown", function (e) { e.stopPropagation(); });
  $("setClose").addEventListener("click", closeSettings);
  $("settingsOverlay").addEventListener("click", function (e) {
    if (e.target === $("settingsOverlay")) closeSettings();
  });
  // ---- bundle export / import -----------------------------------------------
  // The host owns the file dialogs and the writing; the page only supplies the
  // flags and its own settings block, and applies any settings that come back.
  var expPicked = {};        // snippet path -> included

  function openExport() {
    $("expSettingsN").textContent = SETTINGS.length + " values";
    $("expOffsetsN").textContent  = offsets.length + " offsets";
    // Everything ticked to begin with: the common case is sending the lot,
    // and unticking a few is less work than hunting for the ones you want.
    expPicked = {};
    snippets.forEach(function (s) { expPicked[s.n] = true; });
    renderExportList();
    $("exportOverlay").classList.add("open");
  }
  function closeExport() { $("exportOverlay").classList.remove("open"); }

  // Grouped the way the sidebar groups them, so a project reads as a project
  // rather than as a handful of loose files that happen to share a prefix.
  function renderExportList() {
    var list = $("expList");
    list.innerHTML = "";

    var byGroup = {}, loose = [];
    snippets.forEach(function (s) {
      if (s.g) { (byGroup[s.g] = byGroup[s.g] || []).push(s); }
      else loose.push(s);
    });

    function row(label, checked, cls, tag, onToggle) {
      var el = document.createElement("label");
      el.className = "export-item" + (cls ? " " + cls : "");
      var cb = document.createElement("input");
      cb.type = "checkbox";
      cb.checked = checked;
      cb.addEventListener("change", function () { onToggle(cb.checked); });
      el.appendChild(cb);
      var nm = document.createElement("span");
      nm.textContent = label;
      el.appendChild(nm);
      if (tag) {
        var t = document.createElement("span");
        t.className = "tag";
        t.textContent = tag;
        el.appendChild(t);
      }
      list.appendChild(el);
      return el;
    }

    Object.keys(byGroup).sort().forEach(function (g) {
      var kids = byGroup[g];
      var allOn = kids.every(function (s) { return expPicked[s.n]; });
      // The folder row is a bulk toggle, not a thing of its own: there is no
      // such file, so ticking it means ticking everything under it.
      row(baseName(g), allOn, "is-group", isProject(g) ? "project" : "group",
        function (on) {
          kids.forEach(function (s) { expPicked[s.n] = on; });
          renderExportList();
        });
      kids.forEach(function (s) {
        row(baseName(s.n), !!expPicked[s.n], "child", null, function (on) {
          expPicked[s.n] = on;
          renderExportList();
        });
      });
    });

    loose.forEach(function (s) {
      row(baseName(s.n), !!expPicked[s.n], null, null, function (on) {
        expPicked[s.n] = on;
        renderExportList();
      });
    });

    updateExportSummary();
  }

  function exportSelection() {
    return snippets.filter(function (s) { return expPicked[s.n]; });
  }

  function updateExportSummary() {
    var sel = exportSelection();
    var bytes = 0;
    sel.forEach(function (s) { bytes += (s.code || "").length; });
    $("expSummary").textContent =
      sel.length + " of " + snippets.length + " snippets" +
      (sel.length ? ", " + Math.max(1, Math.round(bytes / 1024)) + " KB" : "");
    // Nothing selected anywhere is not an export, it is an empty file.
    $("expGo").disabled = !sel.length && !$("expSettings").checked && !$("expOffsets").checked;
  }

  $("expAll").addEventListener("click", function () {
    snippets.forEach(function (s) { expPicked[s.n] = true; });
    renderExportList();
  });
  $("expNone").addEventListener("click", function () {
    expPicked = {};
    renderExportList();
  });
  $("expSettings").addEventListener("change", updateExportSummary);
  $("expOffsets").addEventListener("change", updateExportSummary);

  $("setExport").addEventListener("click", function () {
    // The settings modal goes FIRST. The picker is a dialog opened from it,
    // and leaving the parent up behind a smaller child reads as the click
    // having done nothing.
    closeSettings();
    openExport();
  });
  $("expCancel").addEventListener("click", closeExport);
  $("exportOverlay").addEventListener("click", function (e) {
    if (e.target === $("exportOverlay")) closeExport();
  });
  $("expGo").addEventListener("click", function () {
    var out = ["[opts] settings=" + ($("expSettings").checked ? 1 : 0) +
                     " offsets="  + ($("expOffsets").checked  ? 1 : 0)];

    out.push("[files]");
    exportSelection().forEach(function (s) { out.push(s.n); });

    if ($("expSettings").checked) {
      out.push("[settings]");
      SETTINGS.forEach(function (s) {
        if (isAction(s)) return;      // no value to carry
        var v = settings[s.k];
        if (s.type === "bool") v = v ? "1" : "0";
        out.push(s.k + "=" + String(v).replace(/[\r\n]+/g, " "));
      });
    }

    send("EXPORT:" + out.join("\n"));
    closeExport();            // the Save dialog is the next thing on screen
  });

  $("setImport").addEventListener("click", function () {
    send("IMPORT");
    closeSettings();
  });

  $("setReset").addEventListener("click", function () {
    SETTINGS.forEach(function (s) { if (!isAction(s)) settings[s.k] = s.def; });
    saveSettings();
    applySettings();
    renderSettings();
  });

  // ---- trash ----------------------------------------------------------------
  // Deleting has always moved a snippet to snippets\.trash rather than
  // unlinking it. That is only a safety net if there is a way back, and until
  // now there was none -- the folder just grew, invisibly.
  var trashItems = [];

  function fmtBytes(n) {
    if (n < 1024) return n + " B";
    if (n < 1024 * 1024) return (n / 1024).toFixed(1) + " KB";
    return (n / (1024 * 1024)).toFixed(1) + " MB";
  }

  function fmtWhen(ms) {
    if (!ms) return "";
    var d = new Date(ms), now = new Date();
    var sameDay = d.toDateString() === now.toDateString();
    return sameDay
      ? "today " + d.toTimeString().slice(0, 5)
      : d.toISOString().slice(0, 10);
  }

  // The sidebar badge. Blank rather than "0" when empty -- an always-present
  // zero is noise, and the icon stays put either way so the strip never jumps.
  function renderTrashCount() {
    var n = trashItems.length;
    $("trashN").textContent = n ? String(n) : "";
    $("btnTrash").title = n
      ? n + " deleted snippet" + (n === 1 ? "" : "s") + " — click to restore"
      : "Trash is empty";
  }

  function openTrash() {
    $("trashOverlay").classList.add("open");
    send("TRASH_LIST");
  }
  function closeTrash() { $("trashOverlay").classList.remove("open"); }

  function renderTrash() {
    var list = $("trashList");
    list.innerHTML = "";
    $("trashCount").textContent =
      trashItems.length + " item" + (trashItems.length === 1 ? "" : "s");
    $("trashEmpty").hidden = trashItems.length === 0;

    if (!trashItems.length) {
      var e = document.createElement("div");
      e.className = "trash-empty";
      e.textContent = "Nothing deleted.";
      list.appendChild(e);
      $("trashHint").textContent = "";
      return;
    }

    // Newest first: what you just deleted by mistake is what you came for.
    trashItems.slice().sort(function (a, b) { return (b.t || 0) - (a.t || 0); })
      .forEach(function (it) {
        var row = document.createElement("div");
        row.className = "trash-row";

        var meta = document.createElement("div");
        meta.className = "trash-meta";
        var to = it.o || it.n;
        meta.innerHTML =
          '<span class="trash-name">' + esc(it.o || it.n) + "</span>" +
          '<span class="trash-sub">' + esc(fmtWhen(it.t)) + " · " +
            esc(fmtBytes(it.sz || 0)) +
            ' · restores to <span class="to">' + esc(to) + "</span></span>";
        row.appendChild(meta);

        var acts = document.createElement("div");
        acts.className = "trash-acts";

        var rb = document.createElement("button");
        rb.className = "btn";
        rb.textContent = "Restore";
        rb.addEventListener("click", function () { send("TRASH_RESTORE:" + it.n); });
        acts.appendChild(rb);

        var db = document.createElement("button");
        db.className = "btn";
        db.textContent = "Delete";
        db.title = "Delete permanently";
        db.addEventListener("click", function () {
          if (!confirm("Permanently delete " + (it.o || it.n) + "?\n\nThis cannot be undone."))
            return;
          send("TRASH_PURGE:" + it.n);
        });
        acts.appendChild(db);

        row.appendChild(acts);
        list.appendChild(row);
      });

    // Only worth saying when it is true of something in the list.
    var orphans = trashItems.filter(function (it) { return !it.o; }).length;
    $("trashHint").textContent = orphans
      ? orphans + " deleted before restore locations were recorded — those go to the top level"
      : "";
  }

  $("btnTrash").addEventListener("click", openTrash);

  // Fire and forget. The watcher picks the client up within a second or two of
  // it being ready, so there is nothing to wait on here and nothing to report
  // that the attach line will not say better.
  $("btnLaunch").addEventListener("click", doLaunch);
  $("trashClose").addEventListener("click", closeTrash);
  $("trashOverlay").addEventListener("click", function (e) {
    if (e.target === $("trashOverlay")) closeTrash();
  });
  $("trashEmpty").addEventListener("click", function () {
    if (!trashItems.length) return;
    if (!confirm("Permanently delete all " + trashItems.length +
                 " item" + (trashItems.length === 1 ? "" : "s") +
                 " in the trash?\n\nThis cannot be undone."))
      return;
    send("TRASH_EMPTY");
  });

  // ---- Edit menu ------------------------------------------------------------
  // Every item is something the keyboard already does. The menu exists so the
  // shortcuts are discoverable rather than folklore -- which matters most for
  // someone opening this app for the first time.
  //
  // Clipboard and undo go through execCommand rather than being reimplemented:
  // the textarea's own stack is the only one that knows what the user typed.
  function openEditMenu() {
    var btn = $("btnEdit");
    var r = btn.getBoundingClientRect();
    var sel = input.selectionStart !== input.selectionEnd;

    function exec(cmd) { input.focus(); document.execCommand(cmd); }

    btn.classList.add("open");
    openCtx(r.left, r.bottom + 4, [
      // Through the same per-tab stacks the keys use. execCommand("undo") would
      // drive the browser's own history, which is the one that bleeds between
      // tabs -- two menu items quietly doing something different from Ctrl+Z.
      { label: "Undo", hint: "Ctrl+Z", run: function () { input.focus(); doUndo(); } },
      { label: "Redo", hint: "Ctrl+Y", run: function () { input.focus(); doRedo(); } },
      { sep: true },
      // Greyed by omission rather than a disabled state: the ctx component has
      // no disabled item, and an item that does nothing is worse than one that
      // is not offered.
      sel ? { label: "Cut",  hint: "Ctrl+X", run: function () { exec("cut"); } }  : null,
      sel ? { label: "Copy", hint: "Ctrl+C", run: function () { exec("copy"); } } : null,
      { label: "Paste", hint: "Ctrl+V", run: function () {
          // execCommand("paste") is blocked outside a user gesture in
          // Chromium, so the async clipboard is the working path. Falls back
          // to telling you to use the key rather than failing silently.
          input.focus();
          if (navigator.clipboard && navigator.clipboard.readText) {
            navigator.clipboard.readText().then(function (t) { if (t) insertText(t); })
              .catch(function () { addLog("warn", "clipboard blocked - use Ctrl+V"); });
          } else if (!document.execCommand("paste")) {
            addLog("warn", "clipboard blocked - use Ctrl+V");
          }
        } },
      sel ? { label: "Delete", hint: "Del", run: function () { insertText(""); } } : null,
      { sep: true },
      { label: "Find…",    hint: "Ctrl+F", run: function () { openFind(false); } },
      { label: "Replace…", hint: "Ctrl+H", run: function () { openFind(true); } },
      { label: "Go to line…", hint: "Ctrl+G", run: goToLine },
      { sep: true },
      { label: "Select all", hint: "Ctrl+A", run: function () { input.focus(); input.select(); } }
    ]);
  }

  $("btnEdit").addEventListener("click", function (e) {
    // stopPropagation keeps the document-level closeCtx from firing on the
    // click that opened the menu, so a second click has to close it here.
    e.stopPropagation();
    if ($("btnEdit").classList.contains("open")) closeCtx();
    else openEditMenu();
  });

  function openPalette() {
    $("paletteOverlay").classList.add("open");
    $("paletteInput").value = "";
    $("paletteInput").focus();
    renderPalette("");
  }
  function closePalette() { $("paletteOverlay").classList.remove("open"); }

  $("cmdk").addEventListener("click", openPalette);
  $("paletteOverlay").addEventListener("click", function (e) {
    if (e.target === $("paletteOverlay")) closePalette();
  });

  function renderPalette(q) {
    q = q.toLowerCase();
    var list = $("paletteList");
    list.innerHTML = "";
    palRows = [];
    var shown = 0;
    var sec = document.createElement("div");
    sec.className = "palette-sec";
    sec.textContent = q ? "Matches" : "Offsets";
    list.appendChild(sec);

    for (var i = 0; i < offsets.length && shown < 60; i++) {
      var k = offsets[i][0];
      if (q && k.toLowerCase().indexOf(q) === -1) continue;
      shown++;
      var row = document.createElement("div");
      row.className = "palette-item";
      row.innerHTML = "<span>" + esc(k) + "</span>" +
                      '<span class="off-val">' + esc(offsets[i][1]) + "</span>";
      (function (key) {
        row.addEventListener("click", function () { insertAtCaret('fx_off("' + key + '")'); closePalette(); });
      })(k);
      list.appendChild(row);
      palRows.push(row);
    }
    if (!shown) {
      var none = document.createElement("div");
      none.className = "empty-note";
      none.textContent = offsets.length ? "No offset matches." : "No offsets loaded.";
      list.appendChild(none);
    }
    palSel = 0;
    paintSel();
  }

  function paintSel() {
    palRows.forEach(function (r, i) { r.classList.toggle("sel", i === palSel); });
    if (palRows[palSel]) palRows[palSel].scrollIntoView({ block: "nearest" });
  }

  $("paletteInput").addEventListener("input", function () { renderPalette(this.value); });
  $("paletteInput").addEventListener("keydown", function (e) {
    if (e.key === "ArrowDown") { e.preventDefault(); palSel = Math.min(palSel + 1, palRows.length - 1); paintSel(); }
    else if (e.key === "ArrowUp") { e.preventDefault(); palSel = Math.max(palSel - 1, 0); paintSel(); }
    else if (e.key === "Enter") { e.preventDefault(); if (palRows[palSel]) palRows[palSel].click(); }
    else if (e.key === "Escape") { e.preventDefault(); closePalette(); }
  });

  // Delegates so a palette insert is undoable and flags the buffer dirty --
  // it did neither while it assigned .value directly.
  function insertAtCaret(text) { insertText(text); }

  // ---- host -> UI ---------------------------------------------------------
  function applyState(s) {
    $("stPid").textContent  = s.pid ? s.pid : "—";
    $("stBase").textContent = s.base ? s.base : "—";
    $("stOff").textContent  = s.offsets || 0;

    // Offsets for a different client. The host refuses to run in this state
    // too, but a Run button that looks pressable and then explains itself is
    // worse than one that says up front why it is not -- especially as this
    // is the first thing a new install hits when its Roblox has moved on.
    offsetsStale = !!s.stale;
    var run = $("btnRun");
    run.disabled = offsetsStale;
    run.title = offsetsStale
      ? "Offsets are for " + (s.version || "another version") +
        " but the client is " + (s.clientVersion || "a different version") +
        ". Running is blocked: every read would come from the wrong offset."
      : "";
    document.body.classList.toggle("offsets-stale", offsetsStale);

    var dmEl = $("stDm");
    if (!s.pid)            dmEl.textContent = "not attached";
    else if (!s.dm)        dmEl.textContent = "no DataModel";
    else if (s.dmVerified) dmEl.textContent = "DataModel " + s.dm;
    else                   dmEl.textContent = "DataModel " + s.dm + " (unverified)";

    // Attach chip. If we've ever seen a multi-instance session, make the chip
    // clickable and add a caret -- so the user can re-pick without hitting
    // Attach. Single-instance sessions keep the plain text.
    var fs = $("footState");
    fs.innerHTML = "";
    fs.title = "";
    fs.classList.toggle("attach-chip", candMulti && !!s.pid);
    if (!s.pid) {
      // TWO DETACHED STATES, not one. "detached" answered neither question a
      // user actually has here: is Roblox even open, and if it is, why are we
      // not on it? s.client is the host's own find_client() answer, so the
      // wording follows what is really there -- No client means press Launch,
      // Not attached means press Attach.
      fs.textContent = s.client ? "Not attached" : "No client";
    } else {
      // THE DISPLAY NAME, from s.user -- the same field the in-game panel
      // header reads, resolved by the host's identity tick (display name with
      // the username as fallback). Not the candidate cache: that is only
      // populated in multi-instance sessions, so a single client showed the
      // bare pid, and the two surfaces could name the same account
      // differently.
      fs.appendChild(document.createTextNode("Attached"));
      if (s.user) {
        fs.appendChild(document.createTextNode(" • "));
        var nm = document.createElement("span");
        nm.className = "nm";
        nm.textContent = s.user;
        fs.appendChild(nm);
      }
      // The pid moves to the tooltip rather than being dropped -- it is what
      // you need when reporting something, and it is not what you read the
      // line for. Same split the panel header makes.
      fs.title = "pid " + s.pid + (s.version ? ("  ·  " + s.version) : "");
      if (candMulti) {
        var caret = document.createElement("span");
        caret.className = "caret";
        caret.textContent = "▾";
        fs.appendChild(caret);
      }
    }
    $("pulse").classList.toggle("on", !!s.pid);

    // Kept for the click handler, which has no state of its own.
    attachedPid = s.pid || 0;
    // Icon only, so there is no label to keep in step -- just what pressing it
    // opens, and what it will show when it does.
    $("btnAttach").title = s.pid ? "Clients — attached" : "Clients";
    // NOT HIDDEN WHEN ATTACHED -- see the note on the button in index.html.
    clientRunning = !!s.client;
    // A client turned up where there was none: the launch did what it was for,
    // whether or not it was ours that started it. Launching a SECOND client has
    // no such signal -- the host reports "a client exists", not how many -- so
    // that one just runs the spinner out.
    if (launching && !launchHadClient && (s.client || s.pid)) setLaunching(false);
    // Rows carry a Detach button on the attached one and are click-to-attach
    // otherwise, so a change of pid changes what the open panel means.
    if (candIsOpen()) renderCand(candCache);

    // The host is authoritative about the gate, so the countdown follows it
    // rather than the button -- arming through any other path still gets the
    // auto-disarm. Only on a CHANGE: this runs on every state push, and
    // restarting the timer each time would mean it never elapsed.
    var wasArmed = armed;
    armed = !!s.armed;
    if (armed !== wasArmed) {
      if (armed) startAutoDisarm(); else cancelAutoDisarm();
    }

    var arm = $("btnArm");
    // A BUTTON SAYS WHAT PRESSING IT DOES, not what the state is: a button
    // labelled with its current state reads as an instruction to anyone who
    // has not memorised which way round it is. The state stays legible through
    // the .armed class here and on the DataModel readout beside it.
    //
    // The in-game panel's pill keeps the OTHER rule, and that is not an
    // inconsistency: it is a status readout, so it names the STATE ("Dry-run" /
    // "Armed") and puts the action in its tooltip. A chip that named an action
    // would read as a promise that you are safe. See the note in hud.html.
    arm.textContent = armed ? "Disarm" : "Arm";
    arm.title = armed
      ? "Writes are being performed. Press to go back to logging them."
      : "Writes are logged and never performed. Press to allow them.";
    arm.classList.toggle("armed", armed);
    dmEl.classList.toggle("armed", armed);
  }

  /* NO SECOND SETTINGS STORE, and this comment is the tripwire against one
     coming back. App mode once ran this page in an iframe on its own origin
     (https://app.eclipse), which gave it its own localStorage -- a second
     settings store that pushed its stale `console.overlayMode=0` and
     `console.hud=0` to the host on load as if the user had just set them,
     throwing the user out of app mode a second after opening it. The app is
     the console's own WINDOW now: one page, one origin, one store, nothing
     to seed or reconcile. Do not reintroduce a second origin for this UI. */

  // Last SNIPPETS body actually applied, so identical re-sends can be dropped.
  // Page-side rather than host-side on purpose: a reloaded page starts with
  // this empty and so always accepts its first snapshot, where a host-side
  // dedupe would have to know about page reloads to replay it.
  var lastSnippetsBody = null;

  window.__onHostMsg = function (raw) {
    {
      raw = typeof raw === "string" ? raw : "";
      var i = raw.indexOf(":");
      var type = i === -1 ? raw : raw.slice(0, i);
      var body = i === -1 ? "" : raw.slice(i + 1);

      if (type === "APPWIN") {
        // App mode: this window now sits over the game as a tool window. The
        // desktop chrome that is meaningless there goes: min/zoom (no
        // taskbar, no desktop), the resize grips (the app's size is the
        // host's computed fit) and the Overlay button (already in). The host
        // drives this both ways; nothing here decides it.
        document.body.classList.toggle("app-mode", body !== "0");
        // Opening the console is not a decision to TYPE in it. In app mode
        // the window is shown and hidden constantly (Insert, the badge,
        // clicking back into the game), and Windows restores focus to
        // whatever control held it -- the editor -- so a reopen dropped a
        // caret into the user's code and the next keystroke edited it.
        // Clear it on the way in; clicking the editor puts the caret back.
        // Arm the focus-event blur as well as blurring now: this message
        // lands before the window is shown, so the activation that follows
        // would put the caret straight back.
        if (body !== "0") { appOpenedAt = Date.now(); blurEditable(); }
      } else if (type === "APPPAINT") {
        // App mode's open leg: the host has just shown this window under the
        // overlay's travel box and will not bring it forward until a frame
        // of it exists -- an unpainted WebView2 window shows the game
        // through. Two rAFs, as for PAINTED: the first fires before the
        // next paint, the second after that frame has been committed.
        requestAnimationFrame(function () {
          requestAnimationFrame(function () { send("APPPAINTED"); });
        });
      } else if (type === "LOG") {
        var b = body.indexOf("|");
        addLog(b === -1 ? "info" : body.slice(0, b), b === -1 ? body : body.slice(b + 1));
      } else if (type === "LOGCLEAR") {
        // The host dropped the shared history; this view empties with it.
        $("log").innerHTML = "";
        logCount = 0;
        $("logCount").textContent = "0";
      } else if (type === "LOGS") {
        // Coalesced batch: the host sends one of these however many lines piled
        // up, so a spamming snippet cannot bury a Stop click in the pump.
        try { addLogBatch(JSON.parse(body) || []); } catch (err) {}
      } else if (type === "CANDIDATES") {
        // Host is telling us who's out there. NEVER auto-opens: this only
        // updates the cache and redraws the panel's rows if it is up. The host
        // sends OPEN_PICKER separately when the user has explicitly asked.
        try { renderCand(JSON.parse(body) || []); } catch (err) {}
      } else if (type === "TRASH") {
        try { trashItems = JSON.parse(body) || []; } catch (err) { trashItems = []; }
        // The count always updates; the list only matters while the modal is
        // up, but rendering it closed costs nothing and avoids a stale first
        // paint when it opens.
        renderTrashCount();
        renderTrash();
      } else if (type === "OPEN_PICKER") {
        // An attach found several clients and needs one picked.
        openCand();
      } else if (type === "PLACE_NAME") {
        // Reply to LOOKUP_PLACE. Body is "<placeId>|<name>". Empty name
        // means the lookup failed or the game has no title (private).
        var bar = body.indexOf("|");
        if (bar > 0) {
          var pid  = body.slice(0, bar);
          var name = body.slice(bar + 1);
          placeNameCache[pid] = name || "";
          refreshPlaceLabels(pid);
        }
      } else if (type === "AVATAR") {
        // Reply to LOOKUP_AVATAR. Body is "<userId>|<url>". The host stays
        // quiet when there is no headshot, so there is no empty case here --
        // the row simply keeps the initial it already drew.
        var abar = body.indexOf("|");
        if (abar > 0) {
          var auid = body.slice(0, abar), aurl = body.slice(abar + 1);
          if (aurl) { avatarCache[auid] = aurl; applyAvatar(auid, aurl); }
        }
      } else if (type === "IMPORTSETTINGS") {
        // Same rule the files get: never overwrite something you made your
        // own. A setting still at its default is one you have no opinion
        // about, so the bundle's value is an improvement on nothing; a setting
        // you changed is a decision, and a bundle should not undo it silently.
        //
        // Applied through setSetting so every value goes past the same clamp
        // as a typed one -- a bundle is a file from someone else and may name
        // keys we do not have, or values we do not allow.
        var n = 0, kept = [];
        body.split("\n").forEach(function (line) {
          var eq = line.indexOf("=");
          if (eq < 0) return;
          var k = line.slice(0, eq), v = line.slice(eq + 1);
          var d = settingDefs[k];
          if (!d) return;                       // unknown key, quietly ignored
          if (settings[k] !== d.def) { kept.push(d.label); return; }
          setSetting(k, d.type === "bool" ? v === "1" : v);
          n++;
        });
        if (n) addLog("ok", "+ applied " + n + " setting" + (n === 1 ? "" : "s") +
                            " from the bundle");
        if (kept.length)
          addLog("warn", "! " + kept.length + " setting" +
                         (kept.length === 1 ? "" : "s") +
                         " left alone, you had changed " +
                         (kept.length === 1 ? "it" : "them") + ": " +
                         kept.join(", "));
        if (!n && !kept.length) addLog("info", "no settings in that bundle");
        renderSettings();
      } else if (type === "STATE") {
        try { applyState(JSON.parse(body)); } catch (err) {}
      } else if (type === "LAUNCHER") {
        // TOOLTIP ONLY -- the labels are fixed, and neither is written here.
        // The button used to name the detected bootstrapper ("Launch
        // Froststrap"), which is wrong twice over: the detection is a guess
        // from install paths, and what actually starts is whatever owns the
        // roblox-player protocol, which may be something else entirely. The
        // thing you want is Roblox; which wrapper gets you there belongs in the
        // tooltip. (Writing the label here would also delete the two icons
        // inside the button, which is why it is not just unnecessary.)
        var ltip = body ? "Start Roblox via " + body : "Start Roblox";
        $("btnLaunch").title = ltip;
        $("attLaunch").title = ltip;
      } else if (type === "OFFSETS") {
        try { offsets = JSON.parse(body) || []; } catch (err) { offsets = []; }
      } else if (type === "SNIPPETS") {
        // DEDUPE FIRST. The snippets watcher re-sends this snapshot every time
        // it looks (watch.cpp, POLL_MS = 2000) whether or not anything changed,
        // and this branch ends in renderSnippets(), which rebuilds the sidebar
        // -- and, through renderTabs(), the tab strip -- wholesale. Every
        // identical push therefore destroyed the element under the pointer:
        // hover lost and regained on a 2s beat, the drag rail flickering with
        // the pointer still, the same on the tabs. The in-game panel had this
        // exact bug and its fix is host-side (g_hud_last, overlay.cpp); the
        // console page dedupes here instead -- see lastSnippetsBody above.
        // pendingMove bypasses the skip: the push that answers a move must be
        // the one that consumes it, never a later unrelated change.
        if (body === lastSnippetsBody && !pendingMove) return;
        lastSnippetsBody = body;
        // The host now sends {groups, items}. Groups arrive separately from the
        // snippets so an EMPTY one still exists -- inferring them from snippet
        // paths would make a new group invisible until you put a file in it.
        try {
          var payload = JSON.parse(body) || {};
          snippets = payload.items  || [];
          groupList = payload.groups || [];
          projectList = payload.projects || [];
        } catch (err) { snippets = []; groupList = []; projectList = []; }

        // Drop anything that no longer exists. A deleted or renamed file left
        // in the selection would be silently re-included by the next bulk
        // action -- against whichever name had slid into its place.
        selected = selected.filter(function (n) { return indexOfName(n) >= 0; });
        if (selAnchor && indexOfName(selAnchor) < 0) selAnchor = null;
        if (cursor && indexOfName(cursor) < 0 && groupList.indexOf(cursor) < 0) cursor = null;

        // Before anything renders: if the folder we are standing in was just
        // renamed or moved, the focus follows it rather than going stale.
        if (pendingMove) { followMove(pendingMove.from, pendingMove.to); pendingMove = null; }

        // Restore the tabs that were open last session, once, before anything
        // decides what to show. Only the NAMES are remembered -- the buffers
        // come from disk, so this cannot resurrect a stale unsaved edit.
        // Turned off, the remembered list is DISCARDED rather than merely
        // ignored -- left in place it would reappear the moment the setting
        // went back on, restoring a session from whenever it was switched off.
        if (!tabsRestored && !S("tabs.restoreOnStart")) {
          tabsRestored = true;
          try { localStorage.removeItem("tabs"); } catch (e) {}
        }
        if (!tabsRestored) {
          tabsRestored = true;
          try {
            var saved = JSON.parse(localStorage.getItem("tabs") || "null");
            if (saved && saved.open) {
              saved.open.forEach(function (rec) {
                var n = rec.n || rec;          // tolerate the old flat form
                var k = indexOfName(n);
                if (k >= 0 && tabIndex(n) < 0)
                  tabs.push({ n: n, code: snippets[k].code, dirty: false,
                              scroll: rec.scroll || 0,
                              selA: rec.selA || 0, selB: rec.selB || 0 });
              });
              if (saved.active && tabIndex(saved.active) >= 0) currentName = saved.active;
            }
          } catch (e) {}
        }

        // Drop tabs whose file is gone -- deleted, renamed, or moved out from
        // under us. Keeping one would leave a tab that cannot be saved and
        // whose Run would compile nothing.
        tabs = tabs.filter(function (t) { return indexOfName(t.n) >= 0; });

        // Re-anchor to the open file by name. Its index may have moved (a
        // delete shifts everything after it) or it may be gone entirely.
        var at = currentName ? indexOfName(currentName) : -1;
        if (at >= 0) {
          current = at;
          // The file changed on disk while it was open. Take the new text only
          // if there is nothing to lose -- unsaved edits win, and the dirty dot
          // is already on screen saying why the editor is not following disk.
          // This is the same rule an editor uses: reload a clean buffer, never
          // a dirty one.
          if (!dirty) {
            if (snippets[at].code !== input.value) {
              input.value = snippets[at].code;
              undoReset();      // the buffer moved: re-mark what "before" means
              repaint();
              markDirty(false);
              // The text moved under the find, so its offsets no longer mean
              // what they did. Same staleness as a tab switch, one level
              // quieter because the file usually LOOKS the same afterwards.
              findResync();
            }
            // Clean MEANS the editor and disk agree, whether or not a reload
            // was needed -- so this is what future changes are measured
            // against. Updating it only inside the reload missed the case
            // right after a SAVE, where they already agree: diskSeen kept the
            // pre-save text, and the next edit then warned about a change that
            // was our own save.
            diskSeen = snippets[at].code;
          } else if (snippets[at].code !== diskSeen) {
            // Dirty, so the buffer is kept -- but SAY so. Silence here is the
            // trap: the file has moved on, the editor still shows your version,
            // and Run sends the editor. You then test something that is not
            // what is on disk and cannot tell from looking.
            //
            // The dirty dot alone does not cover this. It means "unsaved", not
            // "unsaved AND the file underneath has since changed", and those
            // are very different situations.
            //
            // Once per distinct disk version, or a file being written
            // repeatedly would log on every change.
            {
              diskSeen = snippets[at].code;
              // WHICH copy actually runs depends on how it is run, and the two
              // answers are opposites:
              //   a loose snippet  -> RUN: sends input.value, the editor wins
              //   a project file   -> RUN_PROJECT: names a directory and the
              //                       host compiles what is on DISK
              // Telling a project user "Run uses what is in the editor" is not
              // merely unhelpful, it is backwards -- their unsaved edits are
              // the one thing that will NOT be compiled.
              var inProj = isProject(groupOf(currentName) || "");
              addLog("warn", "! " + baseName(currentName) +
                             " changed on disk and you have unsaved edits - " +
                             (inProj
                               ? "a project compiles from DISK, so your edits " +
                                 "are not used. Save to keep them, or close " +
                                 "without saving to take the file version."
                               : "Run uses the editor. Close without saving to " +
                                 "take the file version."));
            }
          }
          // Keep the tab record in step with the editor. Without this the
          // cached buffer silently ages: reload from disk here, switch away and
          // back, and the tab would hand you the text from before the reload.
          var ct = tabIndex(currentName);
          if (ct >= 0) { tabs[ct].code = input.value; tabs[ct].dirty = dirty; }
          renderSnippets();
        } else if (currentName) {
          // The active file was deleted or renamed out from under us. Fall back
          // to another OPEN tab -- the rest of the session's tabs are still
          // perfectly valid.
          currentName = null;
          if (tabs.length) activateTab(tabs[0].n);
          else             closeSnippet();
        } else if (tabs.length) {
          activateTab(tabs[0].n);
        } else {
          // No tabs: stay empty. This used to open snippets[0], which meant a
          // fresh start, a closed last tab, or a deleted active file all pulled
          // an arbitrary file in -- alphabetically first, not anything you
          // chose. Worse on a refresh, since the watcher re-runs this block, so
          // closing everything would reopen a file on the next disk change.
          closeSnippet();
        }

        // The window is still hidden at this point: the host waits for a frame
        // that actually has content in it, or you get chrome with an empty
        // sidebar and an empty editor, which is only marginally better than the
        // blank window this replaced. Two rAFs -- the first fires before the
        // layout above has painted, the second after.
        if (!painted) {
          painted = true;
          requestAnimationFrame(function () {
            requestAnimationFrame(function () { send("PAINTED"); });
          });
        }
            } else if (type === "ZOOM") {
        // Swap maximise <-> restore. The host drives this off WM_SIZE, so it
        // stays right when the window is snapped or maximised outside the app.
        var z = body === "1", zb = $("winZoom");
        // Also drives the resize grips: maximised there is nothing to drag, and
        // live grips would put resize cursors on edges that cannot move.
        document.body.classList.toggle("zoomed", z);
        zb.title = z ? "Restore" : "Maximize";
        zb.innerHTML = z
          ? '<svg viewBox="0 0 10 10" fill="none" stroke="currentColor" stroke-width="1">' +
            '<path d="M2.5 2.5V0.5h7v7h-2"/><rect x="0.5" y="2.5" width="7" height="7"/></svg>'
          : '<svg viewBox="0 0 10 10" fill="none" stroke="currentColor" stroke-width="1">' +
            '<rect x="0.5" y="0.5" width="9" height="9"/></svg>';
      } else if (type === "RUNSTATE") {
        // The authoritative list of what is running now (possibly started by
        // the in-game panel, which this page knows nothing about otherwise).
        // Drives the Run/Stop button, the sidebar markers and any queued run.
        var rs;
        try { rs = JSON.parse(body); } catch (e) { return; }
        applyRunState(rs.runs || []);
      }
    }
  };

  // WebView2 ignores -webkit-app-region, so the titlebar drag is driven from
  // here: mousedown anywhere on the bar except the controls hands off to the
  // host, which runs a real window move. In app mode too, and that is the
  // point of the re-architecture: the window over the game is this same real
  // window, so its drag is the same real move -- the iframe design relayed
  // per-pointer-move deltas through two pages and could not be made
  // immediate.
  document.querySelector(".titlebar").addEventListener("mousedown", function (e) {
    if (e.button !== 0) return;
    // A TITLEBAR DRAG NEVER STARTS ON A CONTROL, and `button` is in this list
    // so that stays true for anything added later. Naming containers alone has
    // now failed twice the same way: the settings gear was safe only because it
    // happened to live inside .cmdk-group, and moving it out to the right
    // cluster made it dead again -- mousedown handed off to the host, Windows
    // ran its modal move loop, and the click never arrived. The giveaway both
    // times is that HOVER still works: the page keeps hit-testing for paint,
    // only the press is taken. CSS app-region is not involved; this handler
    // runs first, and the .menu-btn comment's "both have to agree" cuts this
    // way too.
    // NOT .tb-right itself: it is a flex:1 spacer holding the right THIRD of
    // the bar, and excluding the container made every empty pixel beside the
    // chip undraggable. The controls are what must opt out, and `button`
    // already names them.
    if (e.target.closest("button, .cmdk, .cmdk-group, .wincontrols")) return;
    send("WIN:DRAG");
  });
  // Edge grips. Windows runs its own modal resize loop once the host passes the
  // drag on, so the minimum size in WM_GETMINMAXINFO applies for free.
  Array.prototype.forEach.call(
    document.querySelectorAll("#resizers > [data-edge]"),
    function (el) {
      el.addEventListener("mousedown", function (e) {
        if (e.button !== 0) return;
        e.preventDefault();
        send("WIN:SIZE:" + el.getAttribute("data-edge"));
      });
    }
  );

  document.querySelector(".titlebar").addEventListener("dblclick", function (e) {
    // Same exclusions as the drag above, for a different reason: without them
    // opening the Edit menu twice in quick succession would also maximise the
    // window. (In app mode the host ignores ZOOM -- maximising over the game
    // is meaningless.)
    if (e.target.closest(".wincontrols, .cmdk-group, .menu-btn")) return;
    send("WIN:ZOOM");
  });

  // Last, deliberately. applySettings() writes into `filters` and touches the
  // log, both of which are initialised further up this same body -- running it
  // any earlier would apply to variables that had not been created yet.
  loadSettings();
  applySettings();

  repaint();
  send("READY");
  // Populate the sidebar's trash count on launch. Deletions push it afterwards,
  // but nothing would have told us about the ones from previous sessions.
  send("TRASH_LIST");
})();
