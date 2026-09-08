// radar.cpp -- camera-relative minimap in an HTML snippet window.
//
// Top-down radar that rotates with your view: forward is always up, so a blip
// at the top of the dial is in front of you. Distance rings, health-coloured
// blips, height chevrons for players well above or below you, and the nearest
// one highlighted.
//
// Split of work between the two sides:
//   snippet  reads positions at ~30 Hz and pushes CAMERA-RELATIVE STUDS
//            (not pixels). It does not know or care how big the canvas is.
//   page     owns the zoom range and the pixel mapping, and interpolates
//            between snippet ticks at display rate so the dial glides instead
//            of stepping. Range changes are therefore instant -- no waiting
//            for the next tick.
//
// The range still round-trips back to the snippet ("range:N"), because the
// snippet culls its reads at that distance: zoomed in to 100 studs it stops
// walking distant players at all. The page is the source of truth, the
// snippet follows.
//
// Pure reads throughout -- nothing here writes to the client.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const int   MAX_BLIPS   = 32;
// A RATE, not a delay. The reads below cost real time and used to be added on
// top of the wait, so a nominal 33ms tick actually ran nearer 28 Hz.
static const int   TICK_HZ     = 30;      // the page smooths this up to 60
static const float RANGE_MARGIN = 1.25f;  // cull a bit past the drawn range so
                                          // blips fade in rather than pop

// ---- reads ------------------------------------------------------------------

static bool sane3(float3 v) {
    if (!(v.x == v.x && v.y == v.y && v.z == v.z)) return false;   // NaN
    const float L = 1.0e6f;
    return !(v.x > L || v.x < -L || v.y > L || v.y < -L || v.z > L || v.z < -L);
}

static uint64_t find_camera() {
    uint64_t ws = fx_service("Workspace");
    if (!ws) return 0;
    uint64_t n = fx_nchildren(ws);
    for (uint64_t i = 0; i < n; i++) {
        uint64_t c = fx_child(ws, (int)i);
        if (c && strcmp(fx_class(c), "Camera") == 0) return c;
    }
    return 0;
}

// Basis vectors are the COLUMNS of Camera.Rotation: right = (R0,R3,R6),
// up = (R1,R4,R7), back = (R2,R5,R8). Forward is -back. Established in the
// aimbot's view.cpp against a live camera -- don't re-derive it.
static bool camera_basis(float R[9], float3* pos) {
    uint64_t cam = find_camera();
    if (!cam) return false;
    uint64_t oRot = fx_off("Camera.Rotation"), oPos = fx_off("Camera.Position");
    if (oRot == (uint64_t)-1 || oPos == (uint64_t)-1) return false;
    if (fx_rdblk(cam + oRot, R, 36) != 36) return false;
    *pos = fx_rf3(cam + oPos);
    return sane3(*pos);
}

static bool part_pos(uint64_t part, float3* out) {
    uint64_t oPrim = fx_off("BasePart.Primitive");
    uint64_t oPos  = fx_off("Primitive.Position");
    if (oPrim == (uint64_t)-1 || oPos == (uint64_t)-1) return false;
    uint64_t prim = fx_rd(part + oPrim);
    if (prim < 0x10000 || prim > 0x7ff000000000ULL) return false;
    float3 p = fx_rf3(prim + oPos);
    if (!sane3(p)) return false;
    *out = p;
    return true;
}

// 0..1, or -1 when the Humanoid or its offsets aren't readable. -1 renders as
// a neutral blip rather than as "dead", which a 0 would look like.
// Takes the two numbers rather than the character, because the roster has
// already resolved the Humanoid and read both fields off it -- this used to
// walk every child of every player looking for one, on every frame. A player
// with no Humanoid arrives here as 0/0, which fails the m <= 0 test below and
// returns -1 exactly as the missing-Humanoid case used to.
static float health_frac(float h, float m) {
    if (!(h == h) || !(m == m) || m <= 0.0f) return -1.0f;
    float f = h / m;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

// ---- blips ------------------------------------------------------------------

struct Blip {
    char  name[40];
    float rx, ry;     // camera-relative studs: +rx right, +ry forward
    float dy;         // height difference, studs
    float dist;
    float hp;         // 0..1, or -1 unknown
};

static int collect(float range, Blip* out, int max) {
    // fx_players: one walk over Players with each player's name, Character,
    // Humanoid and health already resolved and memoised. It replaces the hand
    // walk here AND the per-player Humanoid hunt health_frac used to do.
    // Nothing is filtered out by it -- unspawned and dead players arrive with
    // zeros, and the skips below are still ours.
    fx_player_info pl[64];
    int np = fx_players(pl, 64);
    if (np <= 0) return 0;

    float R[9];
    float3 eye;
    if (!camera_basis(R, &eye)) return 0;

    // Horizontal forward / right, from the camera columns. Degenerate only
    // when looking dead vertical, where "which way is forward on a top-down
    // map" genuinely has no answer -- we keep the previous frame by bailing.
    float fx_ = -R[2], fz_ = -R[8];
    float flen = sqrtf(fx_ * fx_ + fz_ * fz_);
    if (flen < 1e-4f) return 0;
    fx_ /= flen; fz_ /= flen;
    float rx_ = R[0], rz_ = R[6];
    float rlen = sqrtf(rx_ * rx_ + rz_ * rz_);
    if (rlen < 1e-4f) return 0;
    rx_ /= rlen; rz_ /= rlen;

    // Radar centres on the CHARACTER, not the camera -- in third person the
    // camera trails several studs behind, which would push every blip forward
    // on the dial and make you look permanently further from things than you
    // are. Falls back to the camera while dead or loading.
    float3 origin = eye;
    if (uint64_t chr = fx_character()) {
        uint64_t hrp = fx_part(chr, "HumanoidRootPart");
        if (!hrp) hrp = fx_part(chr, "Torso");
        if (hrp) part_pos(hrp, &origin);
    }

    float cull = range * RANGE_MARGIN;

    int n = 0;
    for (int i = 0; i < np && n < max; i++) {
        if (pl[i].is_local) continue;
        uint64_t chr = pl[i].model;
        if (!chr) continue;                    // not spawned

        // pl[i].root IS fx_part(chr, "HumanoidRootPart") -- checked 15/15 on a
        // live server -- so the fallback chain is unchanged, just starting from
        // a value already in hand.
        uint64_t hrp = pl[i].root;
        if (!hrp) hrp = fx_part(chr, "Torso");
        if (!hrp) hrp = fx_part(chr, "UpperTorso");
        if (!hrp) continue;

        float3 p;
        if (!part_pos(hrp, &p)) continue;

        float dx = p.x - origin.x, dy = p.y - origin.y, dz = p.z - origin.z;
        float flat = sqrtf(dx * dx + dz * dz);
        if (flat > cull) continue;

        snprintf(out[n].name, sizeof out[n].name, "%s", pl[i].name);
        out[n].rx   = dx * rx_ + dz * rz_;
        out[n].ry   = dx * fx_ + dz * fz_;
        out[n].dy   = dy;
        out[n].dist = sqrtf(dx * dx + dy * dy + dz * dz);
        out[n].hp   = health_frac(pl[i].health, pl[i].maxhealth);
        n++;
    }
    return n;
}

// ---- payload ----------------------------------------------------------------
//
// Built as a JS call rather than a string the page parses: JSON is valid JS, so
// `R.f([{...}])` needs no quoting layer of its own. Only the names have to be
// escaped, and only for JSON.

static void json_escape(const char* in, char* out, size_t max) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 2 < max; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20)         { out[o++] = '?'; }
        else                       { out[o++] = (char)c; }
    }
    out[o] = 0;
}

static void build_call(const Blip* b, int n, char* out, size_t max) {
    size_t o = 0;
    o += snprintf(out + o, max - o, "R.f([");
    for (int i = 0; i < n && o + 160 < max; i++) {
        char safe[56];
        json_escape(b[i].name, safe, sizeof safe);
        o += snprintf(out + o, max - o,
                      "%s{\"n\":\"%s\",\"x\":%.1f,\"y\":%.1f,"
                      "\"z\":%.1f,\"d\":%.1f,\"h\":%.2f}",
                      i ? "," : "", safe,
                      b[i].rx, b[i].ry, b[i].dy, b[i].dist, b[i].hp);
    }
    snprintf(out + o, max - o, "])");
}

// ---- page -------------------------------------------------------------------

static const char* kPage = R"HTML(
<!doctype html>
<html><head><meta charset="utf-8"><style>
  :root { color-scheme: dark }
  * { box-sizing: border-box }
  body { margin:0; height:100vh; display:flex; flex-direction:column;
         background:#0e0e12; color:#dcdcdc;
         font:13px system-ui, Segoe UI, sans-serif; overflow:hidden;
         user-select:none }
  #wrap { flex:1; position:relative; min-height:0 }
  canvas { position:absolute; inset:0; width:100%; height:100% }
  #bar { display:flex; align-items:center; gap:6px; padding:8px 10px;
         border-top:1px solid #24242c; background:#141419 }
  button { padding:5px 9px; background:#22232a; color:#cfcfd4;
           border:1px solid #33343d; border-radius:4px; cursor:pointer;
           font:inherit; line-height:1 }
  button:hover { background:#2c2d36 }
  button.on { background:#2f4f3a; border-color:#3d6b4c; color:#bfe6cd }
  #rng { min-width:62px; text-align:center; font-variant-numeric:tabular-nums;
         color:#9ea0a6 }
  #cnt { margin-left:auto; color:#6e7078; font-size:12px }
</style></head>
<body>
  <div id="wrap"><canvas id="c"></canvas></div>
  <div id="bar">
    <button id="out">&minus;</button>
    <span id="rng">200 st</span>
    <button id="in">&plus;</button>
    <button id="nm" class="on">Names</button>
    <button id="fz">Freeze</button>
    <span id="cnt">&mdash;</span>
  </div>
<script>
const C = document.getElementById('c'), X = C.getContext('2d');
const RANGES = [50, 100, 200, 400, 800];
let ri = 2, names = true, frozen = false;
let prev = [], curr = [], lastT = 0;
const STEP = 1000 / 30;               // must match the snippet's TICK_HZ

// Snippet entry point. Keeps the previous frame so draw() can interpolate.
window.R = { f(list) { prev = curr; curr = list; lastT = performance.now(); } };

function range() { return RANGES[ri]; }

function send(m) { if (window.eclipse_send) window.eclipse_send(m); }

function setRange(d) {
  const n = Math.min(RANGES.length - 1, Math.max(0, ri + d));
  if (n === ri) return;
  ri = n;
  document.getElementById('rng').textContent = range() + ' st';
  send('range:' + range());
}
document.getElementById('in').onclick  = () => setRange(-1);
document.getElementById('out').onclick = () => setRange(+1);
document.getElementById('nm').onclick = e => {
  names = !names; e.target.classList.toggle('on', names);
};
document.getElementById('fz').onclick = e => {
  frozen = !frozen; e.target.classList.toggle('on', frozen);
  send('freeze:' + (frozen ? 1 : 0));
};

// Match by name so a blip glides to its new spot instead of teleporting.
// Unmatched blips (just joined / just came into range) snap, which is right --
// there is no previous position to come from.
function blend(t) {
  const m = new Map(prev.map(o => [o.n, o]));
  return curr.map(o => {
    const p = m.get(o.n);
    if (!p) return o;
    return { ...o, x: p.x + (o.x - p.x) * t, y: p.y + (o.y - p.y) * t };
  });
}

function hpColour(h) {
  if (h < 0) return '#8a8d96';                     // unknown
  return 'hsl(' + Math.round(h * 128) + ' 70% 55%)';
}

function draw() {
  const dpr = window.devicePixelRatio || 1;
  const w = C.clientWidth, h = C.clientHeight;
  if (C.width !== w * dpr || C.height !== h * dpr) {
    C.width = w * dpr; C.height = h * dpr;
  }
  X.setTransform(dpr, 0, 0, dpr, 0, 0);
  X.clearRect(0, 0, w, h);

  const cx = w / 2, cy = h / 2;
  const rad = Math.max(20, Math.min(w, h) / 2 - 18);
  // not `R` -- that name is the snippet's entry point on window.
  const rng = range(), scale = rad / rng;

  // field-of-view wedge, pointing up (forward)
  X.fillStyle = 'rgba(120,170,255,.055)';
  X.beginPath(); X.moveTo(cx, cy);
  X.arc(cx, cy, rad, -Math.PI / 2 - 0.61, -Math.PI / 2 + 0.61);
  X.closePath(); X.fill();

  // rings
  X.strokeStyle = '#232630'; X.lineWidth = 1;
  X.font = '10px Consolas, monospace'; X.fillStyle = '#3d4150';
  for (let i = 1; i <= 4; i++) {
    const rr = rad * i / 4;
    X.beginPath(); X.arc(cx, cy, rr, 0, Math.PI * 2); X.stroke();
    if (i < 4) X.fillText(Math.round(rng * i / 4), cx + 3, cy - rr - 2);
  }
  X.beginPath();
  X.moveTo(cx - rad, cy); X.lineTo(cx + rad, cy);
  X.moveTo(cx, cy - rad); X.lineTo(cx, cy + rad);
  X.stroke();

  const list = blend(Math.min(1, (performance.now() - lastT) / STEP));

  // nearest gets the highlight ring
  let near = null;
  for (const b of list) if (!near || b.d < near.d) near = b;

  for (const b of list) {
    const flat = Math.hypot(b.x, b.y);
    if (flat > rng) continue;
    // +y is forward, which is UP on screen, hence the negation.
    const px = cx + b.x * scale, py = cy - b.y * scale;

    if (b === near) {
      X.strokeStyle = '#e5c07b'; X.lineWidth = 1.5;
      X.beginPath(); X.arc(px, py, 9, 0, Math.PI * 2); X.stroke();
    }

    X.fillStyle = hpColour(b.h);
    X.beginPath(); X.arc(px, py, 4.5, 0, Math.PI * 2); X.fill();

    // height chevron -- only when the gap is big enough to matter
    if (Math.abs(b.z) > 8) {
      const up = b.z > 0, ty = up ? py - 9 : py + 9, s = up ? 1 : -1;
      X.fillStyle = '#9ea3b0';
      X.beginPath();
      X.moveTo(px, ty - 3 * s); X.lineTo(px - 3, ty + s); X.lineTo(px + 3, ty + s);
      X.closePath(); X.fill();
    }

    if (names) {
      X.font = '11px system-ui, sans-serif';
      X.textAlign = 'center';
      X.fillStyle = '#0e0e12'; X.lineWidth = 3;
      X.strokeStyle = '#0e0e12';
      X.strokeText(b.n, px, py + 20);
      X.fillStyle = b === near ? '#e5c07b' : '#b9bcc4';
      X.fillText(b.n, px, py + 20);
      X.fillStyle = '#6e7078'; X.font = '10px Consolas, monospace';
      X.fillText(Math.round(b.d) + 'm', px, py + 31);
      X.textAlign = 'left';
    }
  }

  // you: an arrow pointing forward
  X.fillStyle = '#7fb3ff';
  X.beginPath();
  X.moveTo(cx, cy - 7); X.lineTo(cx - 5, cy + 5);
  X.lineTo(cx, cy + 2); X.lineTo(cx + 5, cy + 5);
  X.closePath(); X.fill();

  document.getElementById('cnt').textContent =
    list.length + (list.length === 1 ? ' contact' : ' contacts');

  requestAnimationFrame(draw);
}
requestAnimationFrame(draw);
</script></body></html>
)HTML";

// ---- main -------------------------------------------------------------------

extern "C" void fx_run() {
    fx_log("radar: opening - close the window or press Stop to end");
    fx_window_open("Eclipse Radar", 460, 540);
    fx_window_html(kPage);

    float range  = 200.0f;      // mirrors the page's initial RANGES[ri]
    bool  frozen = false;
    unsigned dm  = fx_dm_gen();
    char call[8192];

    while (fx_running()) {
        // Page -> snippet. Drain fully; a fast click can queue several.
        while (const char* m = fx_window_get_msg()) {
            if (strncmp(m, "range:", 6) == 0) {
                float v = (float)atof(m + 6);
                if (v > 0.0f) {
                    range = v;
                    char line[64];
                    snprintf(line, sizeof line, "radar: range %.0f", v);
                    fx_log(line);
                }
            } else if (strncmp(m, "freeze:", 7) == 0) {
                frozen = (m[7] == '1');
                fx_log(frozen ? "radar: frozen" : "radar: live");
            }
        }

        // World reload invalidates every cached pointer. Clear the dial and
        // let the next tick repopulate rather than drawing stale blips.
        if (fx_dm_gen() != dm) {
            dm = fx_dm_gen();
            fx_window_eval("R.f([])");
            if (!fx_sleep(300)) break;
            continue;
        }

        if (!frozen) {
            Blip b[MAX_BLIPS];
            int n = collect(range, b, MAX_BLIPS);
            build_call(b, n, call, sizeof call);
            fx_window_eval(call);
        }

        if (!fx_pace(TICK_HZ)) break;
    }

    fx_window_close();
    fx_log("radar: stopped");
}
