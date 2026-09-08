const $ = id => document.getElementById(id);
const send = m => { if (window.eclipse_send) window.eclipse_send(m); };

// The page has no visible console, so a script error would just leave a blank
// window with no clue why. Anything that throws gets shown instead.
window.onerror = (m, s, l) => { document.body.textContent = "ui.js: " + m + " @" + l; };

function esc(s) {
  return String(s).replace(/[&<>"]/g, c =>
    ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
}

// Clipboard without navigator.clipboard: a file:// page is not a secure origin,
// so that API is unavailable here. The old textarea trick still works and needs
// no permission.
function copy(text) {
  const t = document.createElement("textarea");
  t.value = text;
  t.style.position = "fixed";
  t.style.opacity = "0";
  document.body.appendChild(t);
  t.select();
  try { document.execCommand("copy"); } catch (e) {}
  document.body.removeChild(t);
  // Report the TAIL, not the whole thing. What got copied is on the clipboard
  // already; the status line only has to confirm which one, and a full remote
  // path is a hundred characters of container you already knew.
  const shown = text.length > 46 ? "…" + text.slice(-45) : text;
  flash("copied " + shown, text);
}

let flashT = 0;
function flash(msg, full) {
  const s = $("stat");
  s.dataset.hold = msg;
  clearTimeout(flashT);
  flashT = setTimeout(() => { s.dataset.hold = ""; s.title = ""; }, 1600);
  s.textContent = msg;
  // The line ellipsizes, so the untruncated version has to be reachable.
  s.title = full || msg;
}

// U() is the ONLY entry point. The snippet hands over a finished picture --
// every visible row plus the detail for the selection -- and this draws it.
// The page deliberately keeps no tree state of its own: expansion lives in C++,
// which is what stops the two sides disagreeing about what is open.
// Signature of everything about the rows EXCEPT their values.
//
// The whole tree is re-sent twice a second because the values in it are live,
// and rebuilding innerHTML for that threw away the scroll position and any
// selection every time. So: rebuild only when the SHAPE changed -- which rows
// exist, what is open, what is selected -- and when only the numbers moved,
// patch the value spans in place and leave the DOM alone.
function shapeOf(rows) {
  let k = "";
  for (const r of rows) k += r.a + "|" + r.d + (r.o ? "o" : "") + (r.s ? "s" : "") + ";";
  return k;
}
let lastShape = "";

window.U = function (s) {
  const t = $("tree");

  // The snippet now decides this: it sends the rows only when the tree's shape
  // actually moved, and a bare value list otherwise. shapeOf below stays as a
  // second line of defence for a full payload that turns out unchanged.
  if (!s.full) {
    const kids = t.children;
    for (let i = 0; i < kids.length && i < s.vals.length; i++) {
      const span = kids[i].querySelector(".vl");
      if (span && span.textContent !== s.vals[i]) span.textContent = s.vals[i];
    }
    drawSide(s);
    return;
  }

  // Remotes inventory replaces the tree pane. Flat, complete, and static until
  // refreshed -- so no anchoring or diffing, it is simply drawn.
  if (s.rmode) {
    lastShape = "";                      // force a real rebuild on the way back
    const rs = s.remotes;
    if (rs) {
      let h = "";
      let group = null;
      for (const r of rs) {
        // One header per container. A game's remotes arrive in folders and the
        // folder is most of the meaning.
        const where = r.p.replace(/\.[^.]*$/, "");
        if (where !== group) {
          group = where;
          h += '<div class="rgroup">' + esc(group || "—") + "</div>";
        }
        // title= so an ellipsized name is still readable on hover -- the ones
        // that get truncated are exactly the ones you cannot guess.
        h += '<div class="row rrow" data-a="' + r.a + '" title="' + esc(r.p) + '">' +
             '<span class="nm">' + esc(r.n || "—") + "</span>" +
             '<span class="cl">' + esc(r.c) + "</span>" +
             '<span class="ad" data-cp="' + esc(r.p) + '">copy path</span></div>';
      }
      if (!rs.length) h = '<div class="empty">no remotes found</div>';
      t.innerHTML = h;
      t.scrollTop = 0;
    }
    drawSide(s);
    return;
  }

  const shape = shapeOf(s.rows);

  if (shape === lastShape) {
    // Same tree, new numbers. Touch only the text that changed -- no reflow, no
    // lost scroll, no dropped hover.
    const kids = t.children;
    for (let i = 0; i < kids.length && i < s.rows.length; i++) {
      const span = kids[i].querySelector(".vl");
      const want = s.rows[i].v || "";
      if (span && span.textContent !== want) span.textContent = want;
    }
  } else {
    lastShape = shape;

    // KEEP THE VIEW ON THE SAME ROW, not the same pixel.
    //
    // Saving scrollTop and putting it back only works if nothing above the
    // viewport changed. In a live game things appear and disappear constantly,
    // and every one of those above your position shifts the content under a
    // fixed pixel offset -- so the list creeps. Worse, when the tree gets
    // shorter the browser clamps scrollTop, and it springs back when the rows
    // return: that is the jumping up and down.
    //
    // So: remember WHICH row is at the top of the viewport and how far into it
    // we are, then put that row back in the same place afterwards. Rows are
    // identified by address, so they survive a rebuild.
    const scroll = t.scrollTop;
    let anchor = null;
    {
      const kids = t.children;
      if (kids.length) {
        const base = kids[0].offsetTop;
        for (let i = 0; i < kids.length; i++) {
          const el = kids[i];
          const y = el.offsetTop - base;
          if (y + el.offsetHeight > scroll) {   // first row still on screen
            anchor = { a: el.dataset.a, delta: y - scroll };
            break;
          }
        }
      }
    }

    let h = "";
    for (const r of s.rows) {
      const pad = 8 + r.d * 14;
      const tw = r.k ? (r.o ? "▾" : "▸") : " ";
      h += '<div class="row' + (r.s ? " sel" : "") + '" data-a="' + r.a + '"' +
           ' style="padding-left:' + pad + 'px">' +
           '<span class="tw" data-t="' + r.a + '">' + tw + "</span>" +
           '<span class="nm">' + esc(r.n || "—") + "</span>" +
           '<span class="cl">' + esc(r.c) + "</span>" +
           '<span class="vl">' + esc(r.v || "") + "</span>" +
           '<span class="ad" data-cp="0x' + r.a + '">0x' + r.a + "</span></div>";
    }
    t.innerHTML = h;

    // Put the anchor row back where it was. Falls back to the old pixel
    // behaviour only when that row is gone entirely -- collapsing its parent,
    // or a search replacing the list.
    let done = false;
    if (anchor) {
      const el = t.querySelector('.row[data-a="' + anchor.a + '"]');
      if (el && t.children.length) {
        t.scrollTop = (el.offsetTop - t.children[0].offsetTop) - anchor.delta;
        done = true;
      }
    }
    if (!done) t.scrollTop = scroll;
  }

  drawSide(s);
};

// Everything that is not the tree. Split out because a values-only update still
// has to refresh it -- the detail pane is the part with live numbers in it.
function drawSide(s) {
  $("tidy").textContent = s.tidy ? "show all services" : "hide empty services";
  $("rem").textContent = s.rmode ? "back to tree" : "remotes";
  $("rem").title = s.rmode ? "shift-click to rebuild the list" : "";
  if (!$("stat").dataset.hold) {
    // A note (an address lookup, a write result) outranks the row count -- it
    // is the answer to something the user just asked.
    $("stat").textContent = s.note ? s.note
      : (s.count + " rows" +
         // A cut tree is why a twisty opens onto nothing. Never leave it silent.
         (s.trunc ? "  ·  TREE CUT AT THE ROW LIMIT — collapse something" : "") +
         (s.find ? '  ·  find "' + s.find + '"' : ""));
  }

  const d = s.sel;
  if (!d) { $("side").innerHTML = '<div class="empty">nothing selected</div>'; return; }

  let side = "<h3>" + esc(d.name || "—") + "</h3>" +
             '<div class="path">' + esc(s.path) + "<br>" +
             esc(d.cls) + '  ·  <span class="lnk" data-cp="' + d.addr + '">' +
             d.addr + "</span>  ·  " + d.nkids + " children  ·  " +
             '<span class="lnk" id="refs" data-refs="' + d.addr + '">who points here</span>' +
             '  ·  <span class="lnk" data-props="1">' +
             (s.props ? "hide declared" : "declared properties") + "</span>" +
             "</div>";

  // A *Value instance's payload, shown as text. Every Value class the snippet
  // can decode gets one; the ones it cannot say so, rather than leaving a blank
  // where a number should be.
  if (d.cls.indexOf("Value") >= 0) {
    side += s.val
      ? '<div class="val">' + esc(s.val) + "</div>"
      : '<div class="hint">' + esc(d.cls) + " has no payload this can decode</div>";
  }

  if (!d.fields.length) {
    side += '<div class="empty">the conf knows no offsets for ' + esc(d.cls) + "</div>";
  } else {
    // Both widths, always. A double read as a float comes back 0.0, which looks
    // exactly like a real zero -- that mistake has cost this project hours.
    side += "<table><tr><th>field</th><th>off</th><th>f32</th><th>f64</th>" +
            "<th>i32</th><th>ptr</th></tr>";
    for (const f of d.fields) {
      const ptr = f.tgt
        ? '<span class="lnk" data-go="' + f.tgt + '">' + esc(f.ptr) + " →</span>"
        : esc(f.ptr);
      side += '<tr class="' + (f.prim ? "prim" : "") + '">' +
              '<td><span class="lnk" data-cp="0x' + f.addr + '">' + esc(f.k) + "</span></td>" +
              "<td>" + f.off + "</td>" +
              '<td class="n">' + f.f32.toFixed(3) + "</td>" +
              '<td class="n">' + f.f64.toFixed(3) + "</td>" +
              '<td class="n">' + f.i32 + "</td>" +
              '<td class="p">' + ptr + "</td></tr>";
      // A field that decodes as printable text gets its own full-width row.
      // This is where a game's ammo, score and timer actually live -- usually
      // in a HUD label long before they live anywhere the conf can name.
      if (f.str) {
        side += '<tr class="strrow"><td colspan="6">“' + esc(f.str) + "”</td></tr>";
      }
    }
    side += "</table>";
    side += '<div class="hint">click a field name to copy its ADDRESS — that is ' +
            "what namer.cpp and Cheat Engine want</div>";
  }


  // Declared properties, straight out of the client's reflection data. The
  // ones the conf can already locate are marked -- an UNMARKED name is the
  // interesting case: a field this client has that our offsets do not.
  if (s.props) {
    const miss = s.props.filter(p => !p.k).length;
    side += '<div class="wtitle">declared by ' + esc(d.cls) + " — " +
            s.props.length + " names, " + miss + " with no offset</div>";
    side += '<div class="props">';
    for (const p of s.props)
      side += '<span class="prop' + (p.k ? " known" : "") + '">' + esc(p.n) + "</span>";
    side += "</div>";
  }

  // Scan results. Named hits come first from the snippet -- a candidate landing
  // exactly on a field the conf knows is far likelier to be the answer than one
  // at an arbitrary offset in some GUI object.
  $("sstat").textContent = s.ncand
    ? s.ncand + " candidates" + (s.ncand > s.cands.length
        ? "  ·  showing " + s.cands.length : "")
    : "";
  if (s.cands && s.cands.length) {
    side += '<div class="wtitle">scan hits (' + s.stype + ")</div>";
    for (const c of s.cands) {
      side += '<div class="cand' + (c.f ? " namedhit" : "") + '" data-ci="' + c.a +
              '" data-co="' + c.o + '">' +
              '<span class="cn">' + esc(c.n || "—") + "</span>" +
              '<span class="cc">' + esc(c.c) + "</span>" +
              '<span class="co">+0x' + c.o + (c.f ? " = " + esc(c.f) : "") + "</span>" +
              '<span class="cv">' +
                (s.stype === "ptr" ? "0x" + c.v.toString(16) : c.v.toFixed(3)) +
              "</span></div>";
    }
  }

  // Rebuilding this pane twice a second used to wipe whatever was half-typed
  // in the writer -- the "UI keeps resetting" that killed the last explorer.
  // No input survives in here now, so only the SCROLL position has to be
  // carried across the swap; losing that mid-read is the same bug in a smaller
  // coat, and it is why this is not just an innerHTML assignment.
  const pane = $("side");
  const pscroll = pane.scrollTop;
  pane.innerHTML = side;
  pane.scrollTop = pscroll;
}

// One listener per container rather than one per row: the row list is replaced
// wholesale on every update, and per-row handlers would be rebound thousands of
// times a second.
function wire(el) {
  el.addEventListener("click", e => {
    const cp = e.target.closest("[data-cp]");
    if (cp) { copy(cp.dataset.cp); return; }
    const go = e.target.closest("[data-go]");
    if (go) { send("g:" + go.dataset.go); return; }
    const tw = e.target.closest(".tw");
    if (tw) { send("t:" + tw.dataset.t); return; }
    const row = e.target.closest(".row");
    if (row) send("s:" + row.dataset.a);
  });
}
wire($("tree"));
wire($("side"));

$("q").addEventListener("keydown", e => {
  if (e.key === "Enter") send("f:" + $("q").value);
});
$("addr").addEventListener("keydown", e => {
  if (e.key === "Enter") send("n:" + $("addr").value);
});

// The side pane is rebuilt on every update, so nothing in it can own a
// listener -- delegate from the pane and read the target at click time.
$("side").addEventListener("click", e => {
  if (e.target.closest("[data-props]")) { send("P"); return; }
  // A scan hit names an instance and an offset. Only the instance means
  // anything now: it selects that object and opens the path down to it, so a
  // hit you cannot place is one click from being in context.
  const ci = e.target.closest("[data-ci]");
  if (ci) { send("C:" + ci.dataset.ci + ":" + ci.dataset.co); return; }
});

$("root").addEventListener("click", () => { $("q").value = ""; send("r"); });
$("tidy").addEventListener("click", () => send("y"));
// Plain click toggles the inventory; shift-click forces a rebuild, because the
// walk is expensive and remotes do not change while you play.
$("rem").addEventListener("click", e => send(e.shiftKey ? "M!" : "M"));

// Scanning. A first scan sweeps every known instance; narrowing only re-reads
// the candidates, so it is effectively instant and meant to be spammed.
$("sgo").addEventListener("click", () => send("S:" + $("st").value + ":" + $("sv").value));
$("sv").addEventListener("keydown", e => {
  if (e.key === "Enter") send("S:" + $("st").value + ":" + $("sv").value);
});
$("sx").addEventListener("click", () => send("X"));

// "who points here": a ptr scan for the selected object's own address. Answers
// the reverse of every other question in this pane -- which object in the game
// holds a reference to this one, at what offset. Delegated because the link is
// inside the side pane, which is rebuilt on every update.
$("side").addEventListener("click", e => {
  const r = e.target.closest("[data-refs]");
  if (!r) return;
  $("st").value = "ptr";
  $("sv").value = "0x" + r.dataset.refs;
  send("S:ptr:0x" + r.dataset.refs);
});
document.querySelector(".bar2").addEventListener("click", e => {
  const n = e.target.closest("[data-n]");
  // eq/ne compare against the box; the others compare against what the value
  // was last pass, so they take no argument.
  if (n) send("N:" + n.dataset.n + ":" + $("sv").value);
});

// ---- the tree / side divider ----------------------------------------------
//
// The width lives on the element's own inline style rather than in a variable
// the renderer consults. The side pane's innerHTML is replaced twice a second
// but the ELEMENT is not, so an inline width survives every redraw for free --
// and a width the renderer had to reapply would be one more thing to lose.
//
// Pointer events, not mouse events: setPointerCapture keeps the drag alive once
// the cursor leaves the 6px handle, which it does within the first frame of any
// real drag. Without the capture a quick pull drops the moment you outrun it.
const SIDE_MIN = 260;      // narrower and the offset table starts wrapping
const TREE_MIN = 320;      // never let the pane eat the tree entirely
const SIDE_DEFAULT = 420;
(function () {
  const g = $("gutter"), side = $("side"), split = document.querySelector(".split");
  let dragging = false;

  const setw = px => {
    const max = Math.max(SIDE_MIN, split.clientWidth - TREE_MIN);
    side.style.width = Math.min(Math.max(px, SIDE_MIN), max) + "px";
  };

  g.addEventListener("pointerdown", e => {
    dragging = true;
    g.classList.add("drag");
    g.setPointerCapture(e.pointerId);
    e.preventDefault();          // otherwise the drag selects text in the tree
  });
  g.addEventListener("pointermove", e => {
    if (dragging) setw(split.getBoundingClientRect().right - e.clientX);
  });
  const end = e => {
    if (!dragging) return;
    dragging = false;
    g.classList.remove("drag");
    try { g.releasePointerCapture(e.pointerId); } catch (err) {}
  };
  g.addEventListener("pointerup", end);
  g.addEventListener("pointercancel", end);

  // Double-click restores the default. A pane dragged down to its minimum with
  // no way back is how a resizable split turns into a complaint.
  g.addEventListener("dblclick", () => { side.style.width = SIDE_DEFAULT + "px"; });

  // Shrinking the window does not move the divider, so a pane that was legal at
  // 1600px can end up wider than the whole client area at 900. Re-clamp against
  // the new width instead of leaving the tree at zero.
  window.addEventListener("resize", () => {
    if (side.style.width) setw(parseInt(side.style.width, 10));
  });
})();

send("u");
