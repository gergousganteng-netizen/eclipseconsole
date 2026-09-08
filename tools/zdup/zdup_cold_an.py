# THROWAWAY -- build/zdup_cold_an.py. Cold-open analysis over zdup_cold_*.{px,z,g,zlog}.
# For each capture: presented frames inside the open (Insert -> close Insert):
#   badge  = frames where the badge pixel is neither the badge colour nor the box colour (overlay under something)
#   game   = frames where the app titlebar/body read something other than app or box (game through the app rect)
#   boxseen= frames of box over an already-seen app
# and from the z-walk: intervals where the overlay is under the game (order has G before H) while the app is up.
import re, sys, glob
APP = "121215"; BOX = "0d0d10"; BADGE = "7c74f0"; BADGE_OPEN = "101013"
tag = sys.argv[1] if len(sys.argv) > 1 else ""
def load(fn, pat):
    out = []
    for ln in open(fn, errors="replace"):
        m = re.match(pat, ln)
        if m: out.append(m)
    return out
for px in sorted(glob.glob("zdup_cold_%s*.px" % tag)):
    base = px[:-3]
    frames = [(float(m.group(2)), dict(kv.split("=") for kv in m.group(4).split()))
              for m in load(px, r"t(\d+)\s+([\d.]+) acc=(\d+) (.*)")]
    gest = [(float(m.group(2)), m.group(3)) for m in load(base + ".g", r"t(\d+)\s+([\d.]+)\s+(.*)")]
    zev = [(float(m.group(2)), m.group(3), m.group(4)) for m in load(base + ".z", r"t(\d+)\s+([\d.]+)\s+(\S+) fg=(\S)")]
    ins = [t for t, w in gest if w == "KEY Insert"]
    fgm = [w for t, w in gest if w.startswith("gamefg") or w.startswith("third app fg")]
    if len(ins) < 2: print(base, "no open/close pair"); continue
    t0, t1 = ins[0], ins[1]
    win = [f for f in frames if t0 <= f[0] < t1]
    seen = [f for f in win if f[1]["tb1"] == APP and f[1]["tb3"] == APP]
    if not seen: print(base, "app never seen"); continue
    ts = seen[0][0]
    after = [f for f in win if f[0] >= ts]
    arr = [f for f in win if f[1]["tb1"] == BOX and f[1]["tb3"] == BOX and f[1]["bd1"] == BOX]   # box fully at the rect
    box_up = [f for f in win if f[0] >= arr[0][0]] if arr else []
    badge_bad = [f for f in box_up if f[1]["badge"] not in (BADGE, BADGE_OPEN, BOX)]
    game_bad = [f for f in box_up if f[1]["tb1"] not in (APP, BOX) or f[1]["bd1"] not in (APP, BOX)]
    box_seen = [f for f in after if f[1]["tb1"] == BOX and f[1]["tb3"] == BOX]
    # z-walk: overlay under game while app visible (order contains 'A' and G precedes H)
    zw = [(t, o) for t, o, fg in zev if t0 <= t < t1 + 300]
    under = []
    for k, (t, o) in enumerate(zw):
        if "A" in o and "G" in o and "H" in o and o.index("G") < o.index("H"):
            nxt = zw[k + 1][0] if k + 1 < len(zw) else t
            under.append((t - t0, nxt - t, o))
    zl = open(base + ".zlog", errors="replace").read()
    zl = zl[zl.rfind("HOTKEY registered"):]   # last launch only: the trace file appends across launches
    glues = zl.count("overlay -> under app"); band = zl.count("APP band -> TOPMOST"); ext = zl.count("APP external z-move")
    swp = re.findall(r"APP swp .*", zl)
    print("%s  fg=%s  seen+%.0fms  badge-wrong %d fr  game-in-rect %d fr  box-over-seen %d fr  | z under-game: %s | glues=%d band=%d ext=%d %s" % (
        base.split("zdup_cold_")[1], ",".join(fgm), ts - t0, len(badge_bad), len(game_bad), len(box_seen),
        " ".join("%s@%.1f+%.1fms" % (o, a, d) for a, d, o in under) or "none", glues, band, ext, " | ".join(swp[:3])))
