# Making the character fly from outside

**Status: WORKS.** `movement/fly.cpp` zeroes the world's gravity and steers with
velocity. Hovering is free once gravity is off — nothing accelerates you, so
writing a zero velocity means you stay put. No altitude hold, no gain to tune.

Written 2026-08-31, rewritten 2026-09-01 after the gravity offsets turned out to
be wrong and most of a night went into finding that out.

---

## How it works

1. **Zero the world's gravity.** `Workspace.World` → `World.Gravity`, both from
   the conf. This is what makes hovering possible at all.
2. **Write `Primitive.AssemblyLinearVelocity` flat out** for movement, with
   `fx_sleep(0)` — never a pacer.
3. Restore gravity on toggle-off, respawn, world change and Stop.

```
gravity normal  +15.2 +25.4 +30.8 +31.1 +26.3 +16.7  +1.6  -5.9    arcs, falls
gravity zeroed  +19.0 +39.0 +58.0 +77.0 +96.0 +115.0 +135.0 +154.0  never comes down
```

## The gravity offsets, and why the conf was wrong

The offsets endpoint is wrong about both keys on this build. Derived with
`tools/dump/stage_l.cpp` and cross-confirmed on two independent clients:

| | endpoint | derived |
|---|---|---|
| `Workspace.World` | `0x368` | **`0x3f0`** |
| `World.Gravity` | `0x1b4` | **`0x22c`** |

The endpoint's `Workspace.World` does not point at an object at all. Dumping it
shows a packed byte buffer — the qwords shift one position per row and it holds
**zero** pointers — so `+0x1b4` inside it is whatever byte landed there. That is
why it read `0.0`, `1.875` and `31.0` across three sessions. Three unrelated
garbage values is itself the tell.

**Two adjacent floats both read `196.2`, and only ONE of them is gravity.**
`+0x228` and `+0x22c` look identical from a value scan, which is what made this
confusing. Settled by isolation:

- zeroing `+0x228` alone — you rise while thrusting, then get dragged back down
- zeroing `+0x22c` alone — **flies correctly**

So `+0x22c` is gravity and `+0x228` is some other field that happens to hold the
same number. Writing both worked only because it included the right one. The
conf points at `+0x22c` and `fly.cpp` writes that single address.

The trap for anyone re-deriving this: a search that finds the value will return
**both**, and picking the first one gives you the wrong field with no obvious
symptom except a fly that sinks.

**The live conf has been corrected by hand.** `conf/offsets_windows.conf` next
to the exe is runtime-editable by design, so the two lines were edited there and
`fly.cpp` uses `fx_off` with no fallback. **A Roblox version bump wipes this** —
the app re-fetches on a version mismatch and the endpoint's numbers are still
wrong. `stage_l.cpp` re-derives them in seconds.

## What is settled

- **`Primitive.AssemblyLinearVelocity` is the live velocity field.** It tracks
  real movement, and writes to it land — write 55.0, read back 55.0.
- **It is a race, not a rate.** The engine rewrites that field every physics
  step; a write only counts if it lands in the gap. Same 0.6s upward push:

  | | climbed |
  |---|---|
  | 200 Hz | **−0.20 studs** |
  | 1000 Hz | +41.05 |
  | `fx_sleep(0)` | flies properly |

  At 200 Hz it does **nothing**, silently, while the writes still land in
  memory — indistinguishable from a wrong offset. **Do not add a pacer:**
  `fx_pace` sleeps out the remainder of its interval, which is the opposite of
  what a race wants.

- **The race is not close.** Measured armed write rate is about **520,000
  writes/second** (1.9 µs each) against a 240 Hz physics step — roughly 2000
  writes per engine step. That is why `fx_sleep(0)` wins, and why 200 Hz, being
  *below* the physics rate, lost almost every step.
- **Two writes per tick costs ~1.7×** (520k → 302k/s), so one address is worth
  having for more than tidiness.
- Roughly **57% of a commanded velocity** survives into movement, so the speed
  constants are about twice what they look.
- **The camera basis is correct.** Rotation is row-major; the look vector is
  column 2 negated. Verified by projecting a point 10 studs ahead and landing
  **0 px** from screen centre, with a transposed read as the control.
- **`fx_service` must not be called per tick.** It walks every service comparing
  class names; fly was paying that thousands of times a second to re-answer a
  question that only changes when the world does. Cached now, dropped on a
  DataModel change or a failed camera read.

## Dead ends, measured

### Holding `v.y = 0` does not hover

```
no writes (control)   -0.6  -3.1  -7.8 -14.3 -22.4 -33.2
hold v.y = 0          -0.1  -2.0  -6.1 -11.7 -19.5 -26.8
```

Barely different. We win only a fraction of the physics steps: a zero write
removes the fall speed at the instant it lands and gravity rebuilds it before
the next. **Zero injects no energy.** With gravity actually off the same write
hovers perfectly — the mechanism was never wrong, the gravity was.

**This is also the signature of a failed gravity write.** You still fly, and you
sink slowly. It reads as "nearly working" and is actually "gravity is fully on";
the velocity write softens a fall it cannot stop. Slow sinking never means
"gravity is nearly off".

### No fixed positive bias holds either

```
+10   -0.1  -2.2  -6.3 -12.0     falls
+20   +2.2  +2.5  +0.8  -2.7     nearly holds, then sinks
+25   +6.1  +8.4  +8.3  +6.3     rises, then sinks
+30   +9.3 +16.2 +20.9 +24.0     rises, plateaus ~+20
```

Not monotonic, because how often we win varies. A closed-loop P correction sank
more slowly and still sank.

### `Primitive.Position` is an OUTPUT

One teleport pass, exactly `clicktp/move.cpp`'s `place()` shape:

```
+ immediately after: delta +20.00     <- the write LANDS
height vs start: +0.0 +0.0 +0.0 ...   <- and is gone 150ms later
```

Reverted to exactly the start. Walking the step size found no threshold:

```
0.5 / 1 / 2 / 4 / 8 / 16 studs  -> every one reverted
60 consecutive 0.5-stud steps at 120 Hz -> climbed -0.0 studs
```

The engine writes that field from its own transform every frame. Our value
exists only in the gap.

### Anchoring works, and does not help

`PrimitiveFlags.Anchored` sets cleanly and the engine never clears it (7/7,
still set after 500 ms unmaintained). Launched into the air and anchored at the
apex:

```
anchored  +8.6 +12.1 +10.4  +3.6  -7.7 -25.1 -46.6 -60.4
control  +10.8 +16.7 +17.6 +13.4  +4.1 -10.0 -29.5 -53.6
```

Identical arcs — **no physical effect**. Note the sharp edge: `noclip.cpp`
drives `CanCollide` in the *same flags byte* and works. The engine reads
CanCollide from there every frame and takes anchored-ness from elsewhere. **Do
not assume a flag is live because its neighbour is.**

### A fling that was NOT ours

During the position experiments the character shot up +494, +1051, +1521, +2074
studs. Two explanations were written down — delta accumulation, then a
non-atomic per-part write confusing the solver — and **both were wrong**.
Another player in the server was flinging people.

The tell was available and misread: a run with a deliberately **zero** step
flung just as hard, which no working write could cause. **A live server is not a
controlled environment**; prefer a control that CANNOT move you over one that
merely should not.

## Operational traps that cost more than the physics

### Two clients, and the CLI could not choose

`eclipse_cli` had no way to target a specific client. `attach()` always took a
pid for the UI's picker, but the CLI never exposed it, so with two instances
open it silently took whichever `find_pid()` returned first. **Hours of
measurement ran against the wrong client.** `--list` and `--pid N` now exist.

Gravity is per-client and both instances resolve identically, so the log looks
correct either way — only the pid distinguishes them.

### A killed run poisons the next one

If a run dies without restoring — crashed, killed, or a probe wrapped in a hard
timeout — gravity is left at 0. The next run then reads 0 as "the original",
puts 0 back on exit, and logs `gravity 0.00 -> 0` while nothing flies.

**The restore value comes from `Workspace.ReadOnlyGravity`, not the live field.**
That mirror is unwritable and still holds the true value when the live one has
been zeroed, which is what makes a stuck world recoverable. `fly.cpp` also warns
when the two disagree on entry.

Corollary for anyone testing: **do not wrap an armed, state-changing probe in a
hard timeout.** Make it self-limiting instead.

### Writing every matching float crashes the client

An intermediate version zeroed every float equal to the gravity value behind
every Workspace pointer. **Roblox crashed.** Several unrelated objects hold
196.2 legitimately. Write only inside the one object that owns it.

## The lever table

| lever | result |
|---|---|
| **World gravity** (`Workspace+0x3f0` → `+0x22c`) | **the answer** — writable, makes hovering a non-problem |
| `AssemblyLinearVelocity` | partial input — moves you when written flat out, cannot hold against gravity |
| `Primitive.Position` | pure output — reverted at every size, always |
| `PrimitiveFlags.Anchored` | writes land, no physical effect |
| `PrimitiveFlags.CanCollide` | works (noclip) |
| `Workspace.ReadOnlyGravity` | mirror — unwritable, but the reliable READ and the recovery value |

## Still not tried

- Writing the full CFrame (48 bytes at `Primitive.Rotation`) as one block.
- `Humanoid.PlatformStand`. Added, suspected twice, never needed — both times
  the real cause was elsewhere.

## The lessons

1. **Port the working thing rather than improving on it.** 200 Hz instead of
   backdash's 1000; a single-part position write instead of `place()`. Both were
   deviations from code already in this tree.
2. **Validate the pointer before believing a negative.** "No gravity in World"
   was concluded from sweeping an object reached through an unvalidated conf
   offset — wrong object, worthless negative, and it sent the search sideways
   for hours.
3. **An instrument that reads back only what you wrote cannot see what you
   forgot to write.** The debug line printed `gravity 0.00` while the second
   copy still held 196.2, and a Humanoid theory got invented to explain data the
   instrument was hiding.
4. **Print the trajectory, not the endpoint.** The first hover experiment
   measured displacement over 2 s, but the launch reaches ~60 studs and 2 s of
   falling is ~390 — every trial hit the floor, so every number was really
   "height above the ground". It produced a confident, ordered table that meant
   nothing.
5. **When the user says which version last worked, start there.** Twice the
   answer was "what changed since it worked", not anything in the measurements.
