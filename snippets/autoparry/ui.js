const send = m => { if (window.eclipse_send) window.eclipse_send(m); };

// No console in this window, so an uncaught error would kill the panel in
// silence. Route it to the snippet's log.
window.onerror = (m, s, l) => { send('dbg:' + m + ' @line ' + l); return false; };

const $ = id => document.getElementById(id);

// ---- collapse / expand ------------------------------------------------------
// COLLAPSES TO THE HEADER, it does not vanish: a panel that disappears entirely
// leaves nothing to click to get it back, and the snippet is still running.
let collapsed = false;
$('close').onclick = () => {
  collapsed = !collapsed;
  $('wrap').classList.toggle('min', collapsed);
  $('close').innerHTML = collapsed ? '&#9633;' : '&#215;';
  // The snippet resizes the dock rect to match, or the collapsed panel still
  // owns the full rect and eats clicks under a header that is not there.
  send(collapsed ? 'size:min' : 'size:full');
};

// ---- tabs -------------------------------------------------------------------
for (const b of document.querySelectorAll('.tab')) {
  b.onclick = () => {
    for (const o of document.querySelectorAll('.tab')) o.classList.toggle('on', o === b);
    for (const v of document.querySelectorAll('.view'))
      v.classList.toggle('show', v.id === b.dataset.tab);
  };
}

$('go').onclick = () => send('toggle');

// ---- dragging ---------------------------------------------------------------
// A docked panel has no window of its own -- it is an iframe inside the
// console's overlay, positioned by fx_window_dock. So it reports deltas and the
// snippet re-docks. Coalesced to one send per frame: pointermove fires at the
// mouse's polling rate and every message is a blocking round trip.
const bar = $('bar');
let dragging = false, lastX = 0, lastY = 0, pendX = 0, pendY = 0, raf = 0;
function flushDrag() {
  raf = 0;
  if (!pendX && !pendY) return;
  send('drag:' + pendX + ',' + pendY);
  pendX = pendY = 0;
}
bar.addEventListener('pointerdown', e => {
  if (e.target.closest('#close')) return;
  dragging = true; lastX = e.screenX; lastY = e.screenY;
  bar.setPointerCapture(e.pointerId);
  // Touching the panel closes the console app to its badge, the same as any
  // other click on it -- preventDefault below cancels the compatibility
  // mousedown the host's shim listens for, so it is posted directly.
  try { parent.postMessage({ __ec_click: 1 }, '*'); } catch (_) {}
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
  dragging = false; flushDrag();
  try { bar.releasePointerCapture(e.pointerId); } catch (_) {}
}
bar.addEventListener('pointerup', endDrag);
bar.addEventListener('pointercancel', endDrag);

// ---- the dial rows ----------------------------------------------------------
// BUILT FROM WHAT THE SNIPPET SENDS, not hard-coded here: a dial added in C++
// shows up without editing this file, and one removed cannot leave a dead row
// posting a key nothing reads.
const rows = new Map();
let holdUntil = 0;          // typed values win over the refresh for a moment

function makeRow(d) {
  const el = document.createElement('div');
  el.className = 'dial';
  const lbl = document.createElement('div'); lbl.className = 'lbl'; lbl.textContent = d.n;
  const dn  = document.createElement('button'); dn.className = 'stp'; dn.textContent = '−';
  const inp = document.createElement('input'); inp.type = 'text'; inp.inputMode = 'decimal';
  const up  = document.createElement('button'); up.className = 'stp'; up.textContent = '+';
  el.append(lbl, dn, inp, up);

  const push = v => {
    v = Math.round(v * 1000) / 1000;
    if (v < 0) v = 0;
    inp.value = v;
    holdUntil = Date.now() + 900;
    send('set:' + d.k + ':' + v);
  };
  dn.onclick = () => push(parseFloat(inp.value || 0) - d.s);
  up.onclick = () => push(parseFloat(inp.value || 0) + d.s);
  inp.onfocus = () => { holdUntil = Date.now() + 60000; };
  inp.onblur  = () => push(parseFloat(inp.value) || 0);
  inp.onkeydown = e => { if (e.key === 'Enter') inp.blur(); };
  return { el, inp };
}

// Shown until the first state arrives, so the Dials tab is never a blank box.
// fx_window_navigate is async and the snippet pushes on its own clock, so there
// is a real window -- a few hundred ms -- where the page exists with no data.
$('dials').innerHTML = '<div class="lbl" style="color:#5d6474;font-size:11px">loading...</div>';

function renderDials(list) {
  const ph = $('dials').firstElementChild;
  if (ph && !ph.classList.contains('dial') && ph.id !== 'reset') ph.remove();
  for (const d of list) {
    let r = rows.get(d.k);
    if (!r) {
      r = makeRow(d);
      rows.set(d.k, r);
      $('dials').append(r.el);
    }
    if (Date.now() > holdUntil && document.activeElement !== r.inp)
      r.inp.value = d.v;
  }
  if (!$('reset')) {
    const b = document.createElement('button');
    b.id = 'reset'; b.textContent = 'reset to defaults';
    b.onclick = () => { holdUntil = 0; send('reset'); };
    $('dials').append(b);
  }
}

// ---- state ------------------------------------------------------------------
// S(state) -- the snippet's whole view, a few times a second.
window.S = function (s) {
  s = s || {};
  $('go').textContent = s.on ? 'ON' : 'OFF';
  $('go').className   = 'tgl' + (s.on ? ' on' : '');

  const st = $('state');
  st.textContent = !s.on ? 'off'
                 : s.spam ? 'spamming'
                 : s.targeted ? 'TARGETED'
                 : 'watching';
  st.className = 'pill' + (s.spam ? ' act' : s.targeted ? ' hot' : '');

  $('ball').textContent = s.eta > 0
      ? 'eta ' + s.eta.toFixed(3) + 's   ' + Math.round(s.dist) + ' studs   ' +
        Math.round(s.spd) + ' studs/s'
      : (s.dist > 0 ? Math.round(s.dist) + ' studs, not closing' : '');

  $('c_ok').textContent   = s.ok   || 0;
  $('c_turn').textContent = s.turn || 0;
  $('c_bad').textContent  = s.bad  || 0;
  $('c_unk').textContent  = s.unk  || 0;

  const tot = (s.ok || 0) + (s.turn || 0) + (s.bad || 0) + (s.unk || 0);
  $('rate').textContent = tot ? Math.round(100 * (s.ok || 0) / tot) + '%' : '';
  $('last').textContent = s.last || '';

  if (s.dials) renderDials(s.dials);
};

send('ready');
