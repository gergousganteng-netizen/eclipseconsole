const $ = id => document.getElementById(id);
const send = m => { if (window.eclipse_send) window.eclipse_send(m); };

// The page has no visible console, so a script error here would just silently
// disable half the handlers. Route them to the snippet's log instead.
window.onerror = (m, s, l) => { send('dbg:' + m + ' @line ' + l); return false; };

let cfg = null;          // last settings pushed by the snippet
let binding = null;      // 'key_dash' | 'key_fps' while capturing a key

function dot(el, s) { el.className = 'dot ' + s; }
function bar(el, frac, warn) {
  el.className = 'bar' + (warn ? ' warn' : '');
  el.firstElementChild.style.width = Math.max(0, Math.min(1, frac)) * 100 + '%';
}

// ---- key names --------------------------------------------------------------
// Only the ones worth showing; anything else falls back to its number, which is
// still enough to recognise and to type into settings.ini by hand.
const VK = { 1:'LMB', 2:'RMB', 4:'MMB', 5:'MB4', 6:'MB5', 8:'Backspace', 9:'Tab',
  13:'Enter', 16:'Shift', 17:'Ctrl', 18:'Alt', 20:'CapsLock', 27:'Esc', 32:'Space',
  33:'PgUp', 34:'PgDn', 35:'End', 36:'Home', 37:'Left', 38:'Up', 39:'Right',
  40:'Down', 45:'Insert', 46:'Delete' };
for (let i = 0; i < 12; i++) VK[112 + i] = 'F' + (i + 1);
function keyName(v) {
  if (VK[v]) return VK[v];
  if (v >= 48 && v <= 90) return String.fromCharCode(v);
  return String(v);
}

// ---- tabs -------------------------------------------------------------------
for (const t of document.querySelectorAll('.tab')) {
  t.onclick = () => {
    for (const o of document.querySelectorAll('.tab')) o.classList.toggle('on', o === t);
    for (const p of document.querySelectorAll('.pane'))
      p.classList.toggle('on', p.id === 'p-' + t.dataset.p);
  };
}

// ---- settings ---------------------------------------------------------------
// Every control carries the key it edits, so one handler covers all of them and
// adding a dial is one line of HTML.
function paintCfg(c) {
  cfg = c;
  for (const el of document.querySelectorAll('[data-k]')) {
    const k = el.dataset.k;
    if (!(k in c)) continue;
    if (el.classList.contains('sw')) el.classList.toggle('on', !!c[k]);
    else if (document.activeElement !== el) el.value = c[k];   // never fight typing
  }
  $('kdash').textContent = keyName(c.key_dash);
  $('kfps').textContent  = keyName(c.key_fps);
}

let savedTid = null;
function flashSaved() {
  $('saved').classList.add('show');
  clearTimeout(savedTid);
  savedTid = setTimeout(() => $('saved').classList.remove('show'), 900);
}

function put(k, v) { send('set:' + k + '=' + v); flashSaved(); }

for (const el of document.querySelectorAll('input[data-k]')) {
  // change, not input: committing on every keystroke would write the file on
  // each digit and fight you as you type "120" through "1" and "12".
  el.onchange = () => put(el.dataset.k, el.value.trim());
}
for (const el of document.querySelectorAll('select[data-k]')) {
  el.onchange = () => put(el.dataset.k, el.value);
}
for (const el of document.querySelectorAll('.sw[data-k]')) {
  el.onclick = () => {
    const on = !el.classList.contains('on');
    el.classList.toggle('on', on);
    put(el.dataset.k, on ? 1 : 0);
  };
}

$('reset').onclick = () => { send('reset'); flashSaved(); };

// ---- key rebinding ----------------------------------------------------------
// Click the chip, press a key. Captured from the PAGE's own keydown, which is
// why the window has to be focused -- the snippet's hook sees keys everywhere
// but cannot tell which one you meant to bind.
function startBind(which, el) {
  binding = which;
  el.classList.add('binding');
  el.textContent = 'press…';
}
$('kdash').onclick = () => startBind('key_dash', $('kdash'));
$('kfps').onclick  = () => startBind('key_fps',  $('kfps'));

window.addEventListener('keydown', e => {
  if (!binding) return;
  e.preventDefault();
  if (e.key !== 'Escape') put(binding, e.keyCode);
  const el = binding === 'key_dash' ? $('kdash') : $('kfps');
  el.classList.remove('binding');
  binding = null;
  if (cfg) paintCfg(cfg);          // restore the label until the push lands
});

// ---- live state -------------------------------------------------------------
window.U = st => {
  if (st.cfg) paintCfg(st.cfg);

  $('arm').textContent = st.armed ? 'ARMED' : 'disarmed';
  $('arm').className   = st.armed ? 'on' : 'off';
  $('ndash').textContent = st.dashes;
  $('vfps').textContent  = st.fps > 0 ? st.fps.toFixed(0) + ' fps' : 'uncapped';

  dot($('dhp'), !st.hpOn ? 'idle' : (st.hpOpen ? 'on' : 'off'));
  $('vhp').textContent = !st.hpOn ? 'off'
                       : (st.hpOpen ? 'open' : 'shut ' + st.hpFor.toFixed(2) + 's');

  dot($('dsnd'), !st.sndOn ? 'idle' : (st.sndOpen ? 'on' : 'off'));
  $('vsnd').textContent = !st.sndOn ? 'off'
                        : (st.sndOpen ? st.sndFor.toFixed(2) + 's' : 'shut');
  bar($('bsnd'), st.sndOn ? st.sndFor / Math.max(0.001, st.sndWin) : 0, false);

  // Mid-burst is its own state, not a kind of cooldown: full and green while
  // the velocity is being written, then draining as the cooldown runs down.
  const cd = st.cdLeft / Math.max(0.001, st.cd);
  dot($('dcd'), st.busy ? 'on' : (cd > 0 ? 'off' : 'on'));
  bar($('bcd'), st.busy ? 1 : cd, !st.busy && cd > 0);

  dot($('dchr'), st.ok ? 'on' : 'off');
  $('vchr').textContent = st.ok ? 'ready' : 'none';
  bar($('bhp'), st.hp, false);
  $('vhpv').textContent = st.ok ? (st.hp * 100).toFixed(0) + '%' : '—';

  $('status').textContent = st.status;
};

send('ready');
