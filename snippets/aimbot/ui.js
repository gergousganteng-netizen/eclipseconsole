const $ = id => document.getElementById(id);
const send = m => { if (window.eclipse_send) window.eclipse_send(m); };

// The page has no visible console, so a script error here would silently
// disable half the handlers. Route them to the snippet's log instead.
window.onerror = (m, s, l) => { send('dbg:' + m + ' @line ' + l); return false; };

let cfg = null;          // last settings pushed by the snippet
let editing = null;      // key being dragged/typed right now

// ---- controls ---------------------------------------------------------------
// Every input carries data-k, and a key may have two of them (slider + number).
// Writing one updates the other immediately so the pair never disagrees while
// the round trip to the snippet is in flight.

function paintKey(k, v) {
  for (const el of document.querySelectorAll('[data-k="' + k + '"]')) {
    if (el.classList.contains('sw')) el.classList.toggle('on', v >= 0.5);
    else if (document.activeElement !== el) el.value = v;
  }
  // A radius of 0 means "no limit", so the slider next to it is meaningless
  // then -- dim it rather than hide it, since a control that vanishes reads
  // as a bug.
  if (k === 'pickRadius') {
    for (const el of document.querySelectorAll('[data-k="pickRadius"]'))
      el.style.opacity = (+v > 0) ? '1' : '.45';
  }
}

function setKey(k, v) {
  paintKey(k, v);
  send('set:' + k + '=' + v);
}

for (const el of document.querySelectorAll('input[data-k]')) {
  const k = el.dataset.k;
  // 'input' rather than 'change' so a drag is live -- you are tuning a feel,
  // and feeling it only on mouse-up makes that guesswork.
  el.addEventListener('input', () => {
    editing = k;
    const v = parseFloat(el.value);
    if (!isNaN(v)) setKey(k, v);
  });
  el.addEventListener('change', () => { editing = null; });
  el.addEventListener('blur',   () => { editing = null; });
}

for (const el of document.querySelectorAll('select[data-k]')) {
  el.addEventListener('change', () => setKey(el.dataset.k, parseFloat(el.value)));
}

for (const el of document.querySelectorAll('.sw[data-k]')) {
  el.onclick = () => {
    const on = !el.classList.contains('on');
    setKey(el.dataset.k, on ? 1 : 0);
  };
}

$('reset').onclick = () => { editing = null; send('reset'); };

// ---- state from the snippet -------------------------------------------------
// U() is called by the host with the whole state each push; the page renders
// from it rather than tracking deltas, so a dropped push costs one frame of
// staleness and nothing else.
window.U = function (s) {
  const arm = $('arm');
  arm.textContent = s.armed ? 'ARMED' : 'disarmed';
  arm.className = s.armed ? 'on' : 'off';

  const sub = $('sub');
  sub.classList.toggle('live', !!s.aiming);
  if (s.aiming && s.target)  sub.innerHTML = 'aiming at <b>' + esc(s.target) + '</b>';
  else if (s.targets > 0)    sub.innerHTML = '<b>' + s.targets + '</b> in view';
  else                       sub.textContent = 'no targets in view';

  if (s.cfg) {
    cfg = s.cfg;
    for (const k in cfg) {
      // Do not fight the control the pointer is currently on: the snippet
      // clamps, so its value can differ from what is being dragged, and
      // writing it back mid-drag makes the slider jump under the cursor.
      if (k === editing) continue;
      paintKey(k, cfg[k]);
    }
  }
};

// The status line the snippet sends after acting on a set.
window.S = function (text) { $('status').textContent = text || ' '; };

function esc(t) {
  return String(t).replace(/[&<>]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]));
}

send('ready');
