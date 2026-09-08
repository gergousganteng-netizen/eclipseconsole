const send = m => { if (window.eclipse_send) window.eclipse_send(m); };

// No console in this window, so an uncaught error would kill the panel in
// silence. Route it to the snippet's log.
window.onerror = (m, s, l) => { send('dbg:' + m + ' @line ' + l); return false; };

const $ = id => document.getElementById(id);
const list = $('list'), empty = $('empty'), count = $('count'), status = $('status');
const ctl  = $('ctl'),  go    = $('go'),    mode  = $('mode');
const movPane = $('mov'), movNote = $('movnote');

// ---- collapse / expand ------------------------------------------------------
// COLLAPSES TO THE HEADER, it does not vanish. A panel that disappears entirely
// leaves nothing to click to get it back, and the tools are still running --
// the header staying put is what says so.
let collapsed = false;
function setCollapsed(v) {
  collapsed = v;
  $('wrap').classList.toggle('min', collapsed);
  $('close').innerHTML = collapsed ? '&#9633;' : '&#215;';
  $('close').title = collapsed ? 'Expand' : 'Collapse';
  // The snippet resizes the dock rect to match. Without this the collapsed
  // panel still owns the full rect and blocks clicks under a header that is
  // not there any more.
  send(collapsed ? 'size:min' : 'size:full');
}
$('close').onclick = () => setCollapsed(!collapsed);

// ---- movement ---------------------------------------------------------------
// Typed values are held locally while the box has focus, so the 4Hz refresh
// cannot overwrite what you are halfway through typing.
const movVal = { ws: 16, jp: 50 };
const movOn  = { ws: false, jp: false };
let movHold  = 0;

function movSend(f, v) {
  v = Math.max(0, Math.min(5000, Math.round(v)));
  movVal[f] = v;
  movHold = Date.now() + 800;
  $(f).value = v;
  send('mov:' + f + 'v:' + v);
}
for (const b of document.querySelectorAll('.stp'))
  b.onclick = () => movSend(b.dataset.f, movVal[b.dataset.f] + (+b.dataset.d));
for (const b of document.querySelectorAll('.tgl'))
  b.onclick = () => {
    // Echoed locally FIRST. Without this the button waited out movHold and
    // then the next push before it moved, which is most of a second of looking
    // like the click missed.
    const f = b.dataset.f;
    movOn[f] = !movOn[f];
    paintTgl(f);
    movHold = Date.now() + 800;
    send('mov:' + f);
  };

// Fly is a plain toggle with no value beside it, so it gets its own echo.
let flyOn = false, flyHold = 0;
$('flygo').onclick = () => {
  flyOn = !flyOn;
  paintFly();
  flyHold = Date.now() + 800;
  send('fly');
};
function paintFly() {
  const b = $('flygo');
  b.textContent = flyOn ? 'ON' : 'OFF';
  b.className = 'tgl wide' + (flyOn ? ' on' : '');
}

function paintTgl(f) {
  const b = $(f + 'go');
  b.textContent = movOn[f] ? 'ON' : 'OFF';
  b.className = 'tgl' + (movOn[f] ? ' on' : '');
}
for (const i of document.querySelectorAll('.num')) {
  i.onfocus = () => { movHold = Date.now() + 60000; };
  i.onblur  = () => movSend(i.dataset.f, parseFloat(i.value) || 0);
  i.onkeydown = e => { if (e.key === 'Enter') i.blur(); };
}

// Local echo after a click: the snippet takes a tick to hear us, and adopting
// its stale answer in the meantime made the highlight bounce straight back.
let hold = 0;
let pick = 0, never = new Set(), on = false, spec = 0, all = false;

$('all').onclick = () => {
  all = !all;
  on  = all;                       // on implies running, off implies stopped
  hold = Date.now() + 600;
  send('fling:all');
  render(last);
};

go.onclick = () => { on = !on; hold = Date.now() + 600; send('fling:' + (on ? 'on' : 'off')); render(last); };

let tab = 'players';
let last = { players: [] };

// ---- tabs -------------------------------------------------------------------
// A tab only changes what we ASK for. The snippet keeps every tool ticking.
for (const b of document.querySelectorAll('.tab')) {
  b.onclick = () => {
    tab = b.dataset.tab;
    for (const o of document.querySelectorAll('.tab')) o.classList.toggle('on', o === b);
    send('tab:' + tab);
    render(last);
  };
}

// ---- dragging ---------------------------------------------------------------
// A docked panel has no window of its own -- it is an iframe inside the
// console's overlay, positioned by fx_window_dock. So it reports deltas and the
// snippet re-docks. Coalesced to one send per frame: pointermove fires at the
// mouse's polling rate and every message is a blocking round trip, so sending
// them all makes the drag slower, not smoother.
const bar = $('bar');
let dragging = false, lastX = 0, lastY = 0, pendX = 0, pendY = 0, raf = 0;

function flushDrag() {
  raf = 0;
  if (!pendX && !pendY) return;
  send('drag:' + pendX + ',' + pendY);
  pendX = pendY = 0;
}
bar.addEventListener('pointerdown', e => {
  // Not the close button. The handler below captures the pointer and calls
  // preventDefault, which swallows the click on anything inside the bar -- so
  // anything clickable in the drag handle has to opt out here.
  if (e.target.closest('#close')) return;
  dragging = true; lastX = e.screenX; lastY = e.screenY;
  bar.setPointerCapture(e.pointerId);
  // TOUCHING THE PANEL CLOSES THE CONSOLE APP TO ITS BADGE -- grabbing the
  // header counts, the same way grabbing a native title bar means "I am working
  // in this window now". The console does that off the injected shim's
  // mousedown, and preventDefault below cancels the compatibility mouse events,
  // so the shim never sees the press and a drag was the one gesture on the
  // panel that left the app in front of the game. Posted directly instead:
  // this is the shim's exact message, and hud.html accepts it because the guard
  // is e.source === the dock frame, which is this page.
  try { parent.postMessage({ __ec_click: 1 }, '*'); } catch (_) {}
  // Suppresses the browser's own text selection and native drag on the bar.
  e.preventDefault();
});
bar.addEventListener('pointermove', e => {
  if (!dragging) return;
  // screenX/Y, not clientX/Y: client coords are relative to a document that is
  // itself moving, so the panel crawls instead of following.
  const dx = e.screenX - lastX, dy = e.screenY - lastY;
  if (!dx && !dy) return;
  lastX = e.screenX; lastY = e.screenY;
  pendX += dx; pendY += dy;
  if (!raf) raf = requestAnimationFrame(flushDrag);
});
function endDrag(e) {
  if (!dragging) return;
  dragging = false;
  flushDrag();
  try { bar.releasePointerCapture(e.pointerId); } catch (_) {}
}
bar.addEventListener('pointerup', endDrag);
bar.addEventListener('pointercancel', endDrag);

// ---- the roster -------------------------------------------------------------
// Rows are kept and UPDATED, never rebuilt. This refreshes four times a second,
// and clearing the list destroys the element under the pointer mid-click --
// which is why clicking a name only worked sometimes.
const rows = new Map();

function makeRow(u) {
  const el = document.createElement('div');
  el.className = 'row';
  const img = document.createElement('img');
  img.className = 'pfp';
  img.onerror = () => img.removeAttribute('src');
  const mid = document.createElement('div'); mid.className = 'mid';
  const nm  = document.createElement('div'); nm.className  = 'nm';
  const at  = document.createElement('span'); at.className  = 'at';
  nm.append(at);
  const sub = document.createElement('div'); sub.className = 'sub';
  const ban = document.createElement('span'); ban.className = 'ban';
  ban.textContent = 'whitelisted';
  mid.append(nm, sub, ban);

  // Explicit buttons rather than a row click that means different things on
  // different tabs -- which was impossible to guess at and easy to get wrong.
  const acts = document.createElement('div'); acts.className = 'acts';
  const tpb  = document.createElement('button'); tpb.className = 'act t'; tpb.textContent = 'TP';
  const flb  = document.createElement('button'); flb.className = 'act f'; flb.textContent = 'FLING';
  const spb  = document.createElement('button'); spb.className = 'act s'; spb.textContent = 'WATCH';
  acts.append(tpb, spb, flb);

  el.append(img, mid, acts);
  return { el, img, nm, at, sub, ban, tpb, flb, spb };
}

function render(s) {
  const players = s.players || [];
  count.textContent = players.length
      ? players.length + (players.length === 1 ? ' player' : ' players')
      : '';
  empty.style.display = players.length ? 'none' : 'block';
  const f = s.fling || {};
  if (Date.now() > hold) {
    on   = !!f.on;
    all  = !!f.all;
    pick = f.pick || 0;
  }
  never = new Set(f.never || []);
  const sp = s.spec || {};
  spec = sp.who || 0;

  // ONE tab for both tools, so the status line carries both. Fling wins while
  // it is on, because that is the one that changes on its own; teleport only
  // ever speaks just after you pressed something.
  const onPlayers = tab === 'players';
  const tpStatus  = (s.tp && s.tp.status) || '';
  // Three tools share one line, so the one that is actively doing something
  // wins: spectate is a standing state, fling changes on its own, teleport only
  // speaks right after a press.
  status.textContent = onPlayers
      ? (spec ? (sp.status || '') : (on ? (f.status || '') : tpStatus))
      : '';
  ctl.classList.toggle('show', onPlayers);

  // Movement replaces the roster rather than sitting under it.
  const m = s.mov || {};
  const onMov = tab === 'mov';
  movPane.classList.toggle('show', onMov);
  list.style.display  = onMov ? 'none' : '';
  if (onMov) empty.style.display = 'none';
  if (onMov) {
    movNote.textContent = m.ok ? '' : 'no character right now';
    if (Date.now() > movHold) {
      movVal.ws = m.ws; movVal.jp = m.jp;
      movOn.ws  = !!m.ws_on; movOn.jp = !!m.jp_on;
      if (document.activeElement !== $('ws')) $('ws').value = m.ws;
      if (document.activeElement !== $('jp')) $('jp').value = m.jp;
      paintTgl('ws'); paintTgl('jp');
    }
    const fl = s.fly || {};
    if (Date.now() > flyHold) { flyOn = !!fl.on; paintFly(); }
    $('flynote').textContent = flyOn ? (fl.status || '') : 'gravity is global while on';
  }
  go.textContent = on ? 'FLING ON' : 'FLING OFF';
  go.className   = on ? 'on' : 'off';
  $('all').className = all ? 'on' : 'off';
  mode.textContent = all ? 'everyone, 0.5s each'
                   : pick ? 'locked on one' : 'nearest';

  // A-Z, so a row stays where you last saw it. Distance reshuffles constantly.
  const sorted = players.slice().sort((a, b) =>
      ((a.dn || a.n || '')).localeCompare(b.dn || b.n || '', undefined,
                                          { sensitivity: 'base' }));

  const seen = new Set();
  let prev = null;
  for (const p of sorted) {
    seen.add(p.u);
    let r = rows.get(p.u);
    if (!r) { r = makeRow(p.u); rows.set(p.u, r); }

    // Display name leads because it is what you read in game; the @username
    // follows because it is what identifies them. Printing it twice when they
    // match is just noise.
    const name = p.dn || p.n || '';
    if (r.nm.firstChild && r.nm.firstChild.nodeType === 3) {
      if (r.nm.firstChild.data !== name) r.nm.firstChild.data = name;
    } else {
      r.nm.insertBefore(document.createTextNode(name), r.at);
    }
    const handle = (p.dn && p.dn !== p.n) ? '@' + p.n : '';
    if (r.at.textContent !== handle) r.at.textContent = handle;
    const dead = p.d < 0;
    const sub = dead ? 'respawning'
                     : Math.round(p.d) + ' studs' +
                       (p.hp > 0 ? '  ·  ' + Math.round(p.hp) + ' hp' : '');
    if (r.sub.textContent !== sub) r.sub.textContent = sub;
    if (p.a && r.img.getAttribute('src') !== p.a) r.img.src = p.a;
    // Lock and never-fling show on every tab now, since the buttons work from
    // every tab -- hiding the state on the TP tab only made it look dropped.
    const cls = 'row' + (dead ? ' gone' : '') +
                (p.u === pick   ? ' sel'   : '') +
                (p.u === spec   ? ' watching' : '') +
                (never.has(p.u) ? ' wl' : '');
    if (r.el.className !== cls) r.el.className = cls;

    r.tpb.onclick = e => { e.stopPropagation(); send('tp:' + p.u); };
    r.flb.onclick = e => {
      e.stopPropagation();
      pick = (pick === p.u) ? 0 : p.u;   // pressing the locked one unlocks it
      hold = Date.now() + 600;
      send('fling:sel:' + pick);
      render(last);
    };
    const fon = 'act f' + (p.u === pick ? ' on' : '');
    if (r.flb.className !== fon) r.flb.className = fon;
    r.spb.onclick = e => { e.stopPropagation(); send('spec:' + p.u); };
    const son = 'act s' + (p.u === spec ? ' on' : '');
    if (r.spb.className !== son) r.spb.className = son;
    // Right-click is free: the console disables the browser context menu across
    // the whole overlay.
    r.el.oncontextmenu = e => {
      e.preventDefault();
      send('fling:never:' + p.u);
      return false;
    };

    // Reorder by MOVING the node, which does not destroy it.
    const want = prev ? prev.nextSibling : list.firstChild;
    if (r.el !== want) list.insertBefore(r.el, want);
    prev = r.el;
  }
  for (const [u, r] of rows) {
    if (seen.has(u)) continue;
    r.el.remove();
    rows.delete(u);
  }
}

// S(state) -- the snippet's whole view, four times a second.
window.S = function (s) { last = s || { players: [] }; render(last); };

send('ready');
