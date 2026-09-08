const send = m => { if (window.eclipse_send) window.eclipse_send(m); };

// No visible console in this window, so an error here would silently kill the
// panel. Route it to the snippet's log instead.
window.onerror = (m, s, l) => { send('dbg:' + m + ' @line ' + l); return false; };

const HOLD_MS = 6000;   // how long an ordinary toast stays
const MAX     = 5;      // oldest beyond this leave early rather than scroll

const live = [];        // oldest first, matching DOM order

// username -> headshot URL, pushed by the snippet as fx_avatar_url resolves
// them. Keyed on the USERNAME, not the display name: display names are not
// unique and two players can legitimately share one.
const avatars = {};
window.A = function (user, url) {
  if (avatars[user] === url) return;
  avatars[user] = url;
  // BACKFILL. A card reads this map once, when it is built, so a toast that
  // fired before the fetch landed would stay blank for its whole life -- which
  // is exactly the case that shows up, because someone joining and ulting
  // straight away is when the URL is least likely to be in yet.
  for (const rec of live) if (rec.user === user && !rec.el.querySelector('.pfp')) fill(rec, url);
};

function fill(rec, url) {
  const body = rec.el.querySelector('.body');
  if (!body) return;
  const img = document.createElement('img');
  img.className = 'pfp';
  img.src = url;
  img.alt = '';
  img.onerror = () => img.remove();
  body.prepend(img);
}

// T(display, user, kind, secs, isLocal)
//   kind 0 = has the ult, 1 = Death Counter armed, 2 = lost the ult
window.T = function (display, user, kind, secs, me) {
  const el = document.createElement('div');
  el.className = 't ' + (kind === 1 ? 'dc' : kind === 2 ? 'lost' : 'has') +
                 (me ? ' me' : '');

  // Line 1: who. DisplayName is what you read in game, the username is what
  // identifies them -- both, because in this game they are routinely
  // different. When they match, showing it twice is just noise.
  const top = document.createElement('div');
  top.className = 'top';
  const d = document.createElement('span'); d.className = 'disp';
  // textContent, never innerHTML: these are player-chosen names arriving from
  // the game and must not be parsed as markup.
  d.textContent = display;
  top.append(d);
  if (user && user !== display) {
    const u = document.createElement('span'); u.className = 'user';
    u.textContent = '@' + user;
    top.append(u);
  }

  // Line 2: what happened, and the clock if it has one.
  const bot = document.createElement('div');
  bot.className = 'bot';
  const t = document.createElement('span'); t.className = 'what';
  const a = document.createElement('span'); a.className = 'ago';
  t.textContent = kind === 1 ? 'DEATH COUNTER ACTIVE'
                : kind === 2 ? 'lost Saitama ult'
                             : 'has Saitama ult';
  a.textContent = kind === 1 ? secs + 's' : '';
  bot.append(t, a);

  // The picture, when we have one. Built as a column beside it rather than
  // inside the text, so a missing avatar just leaves the text where it is
  // instead of reflowing the card.
  const body = document.createElement('div');
  body.className = 'body';
  const txt = document.createElement('div');
  txt.className = 'txt';
  txt.append(top, bot);
  body.append(txt);
  el.append(body);
  // After the text is in, so prepend puts the picture on the left. A CDN
  // hiccup costs the picture, not the toast.
  if (avatars[user]) fill({ el }, avatars[user]);

  const rec = { el, ago: a, user: user, at: Date.now(),
                dc: kind === 1, secs: secs || 0 };

  if (rec.dc) {
    const bar = document.createElement('div');
    const fill = document.createElement('i');
    bar.className = 'bar';
    bar.append(fill);
    el.append(bar);
    rec.fill = fill;
  }

  // APPEND: the stack is bottom-anchored, so the newest lands at the bottom
  // and everything above is pushed up.
  document.getElementById('stack').append(el);
  live.push(rec);

  while (live.length > MAX) drop(live.shift());
};

// D(user) -- the Counter accessory cleared, so that player's countdown is over
// NOW. Driven by the real signal rather than the assumed 10s, so a Death
// Counter cut short (they died, the round ended) stops reading as live.
window.D = function (user) {
  for (const rec of live)
    if (rec.dc && rec.user === user && !rec.expired) finish(rec);
};

function finish(rec) {
  rec.expired = true;
  rec.el.classList.add('done');
  rec.ago.textContent = 'over';
  // Re-based so the 2.5s linger below is measured from the END, not from when
  // the toast appeared -- otherwise an early finish vanishes instantly.
  rec.at   = Date.now();
  rec.secs = 0;
}

function drop(rec) {
  if (!rec || rec.gone) return;
  rec.gone = true;
  rec.el.classList.add('out');
  // Removed only once the collapse finishes, so the toasts above settle
  // instead of snapping down.
  setTimeout(() => rec.el.remove(), 420);
}

// One timer for the whole stack rather than one per toast: every row needs
// re-rendering anyway, and a single pass cannot drift out of step with itself.
setInterval(() => {
  const now = Date.now();
  for (let i = live.length - 1; i >= 0; i--) {
    const rec = live[i];
    const age = (now - rec.at) / 1000;

    if (rec.dc) {
      const left = rec.secs - age;
      if (left > 0) {
        // Ceil, so it reads 10 the moment it appears and only shows 1 in the
        // final second. A counter that starts at 9 looks broken.
        rec.ago.textContent = Math.ceil(left) + 's';
        if (rec.fill) rec.fill.style.width = (left / rec.secs * 100) + '%';
      } else if (!rec.expired) {
        finish(rec);
      }
      if (rec.expired && age >= rec.secs + 2.5) { drop(rec); live.splice(i, 1); }
      continue;
    }

    if (age * 1000 >= HOLD_MS) { drop(rec); live.splice(i, 1); }
  }
}, 200);

send('ready');
