# THROWAWAY -- build/zdup_warm_an.py. Warm-open analysis over one zdup capture (px/g/z prefix).
import re, sys
base = sys.argv[1]
APP = "121215"; BOX = "0d0d10"; BADGE = "7c74f0"; BADGE_OPEN = "101013"; DOCK = "141418"
def load(fn, pat):
    return [m for ln in open(fn, errors="replace") for m in [re.match(pat, ln)] if m]
frames = [(float(m.group(2)), dict(kv.split("=") for kv in m.group(4).split())) for m in load(base + ".px", r"t(\d+)\s+([\d.]+) acc=(\d+) (.*)")]
gest = [(float(m.group(2)), m.group(3)) for m in load(base + ".g", r"t(\d+)\s+([\d.]+)\s+(.*)")]
zev = [(float(m.group(2)), m.group(3)) for m in load(base + ".z", r"t(\d+)\s+([\d.]+)\s+(\S+) fg=")]
acts = [(t, w) for t, w in gest if w == "KEY Insert" or "click badge" in w]
for k in range(0, len(acts) - 1, 2):
    t0, t1 = acts[k][0], acts[k + 1][0]
    win = [f for f in frames if t0 <= f[0] < t1]
    arr = [f for f in win if f[1]["tb1"] == BOX and f[1]["tb3"] == BOX and f[1]["bd1"] == BOX]
    seen = [f for f in win if f[1]["tb1"] == APP and f[1]["tb3"] == APP]
    if not arr or not seen: print("open @%.0f: box/app not seen" % t0); continue
    up = [f for f in win if f[0] >= arr[0][0]]
    after = [f for f in win if f[0] >= seen[0][0]]
    badge_bad = [f for f in up if f[1]["badge"] not in (BADGE, BADGE_OPEN, BOX)]
    game_bad = [f for f in up if f[1]["tb1"] not in (APP, BOX) or f[1]["bd1"] not in (APP, BOX)]
    box_seen = [f for f in after if f[1]["tb1"] == BOX and f[1]["tb3"] == BOX]
    dock_over = [f for f in after if f[1]["d1"] == DOCK or f[1]["d2"] == DOCK]
    zw = [(t - t0, o) for t, o in zev if t0 <= t < t1]
    under = ["%s@%.1f" % (o, t) for t, o in zw if "A" in o and "G" in o and "H" in o and o.index("G") < o.index("H")]
    print("open %-28s box@+%.0f seen@+%.0f  badge-wrong %d  game-in-rect %d  box-over-seen %d  dock-over-app %d  under-game: %s  z: %s" % (
        acts[k][1], arr[0][0] - t0, seen[0][0] - t0, len(badge_bad), len(game_bad), len(box_seen), len(dock_over), " ".join(under) or "none",
        " ".join("%s@%.0f" % (o, t) for t, o in zw)))
