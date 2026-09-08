# Deriving offsets ourselves

A working proof that this console can recover Roblox offsets from a live client
with no dump endpoint, written 2026-08-16 against `RobloxPlayerBeta.exe`
base `0x7ff7afba0000`, 144.5 MB image, ~3.6 GB committed.

**Status: proof of concept, deliberately stopped here.** The offsets endpoint
(`offsets.imtheo.lol`) is reliable and gets a new dump when Roblox updates, so
this is not a replacement for it. It exists so that "the endpoint is down" or
"this conf looks wrong" are answerable questions rather than dead ends.

Everything here reproduces offsets we *already had*, because that is the only
way to know the method works.

**Re-run 2026-08-28 against `version-f5a60436d48947d3`** — a build written three
Roblox updates after this file, with a different layout and a moved static. The
method held:

- every Instance offset, `DataModel.Workspace`, `Workspace.CurrentCamera`,
  `Camera.Rotation`/`Position`, `BasePart.Primitive` and all three `Primitive`
  fields came out MATCHing the conf;
- `FakeDataModel.Pointer` came out as `8ca9cc8`, the conf's exact value, from a
  set of 15 statics that reach the live DataModel;
- `stage_e` correctly REJECTED every hardcoded RVA from the old build, including
  the `8b79b58` quoted below — so nothing here is passing by coincidence;
- the known weaknesses reproduced too: `stage_f` still misses
  `Primitive.Rotation`/`Position` by signature alone (`88`/`ac`) and `stage_g`'s
  distinctness ranking still fixes them, and the CFrame tie at `+c8` vs `+110`
  is still decided by map ordering rather than by evidence.

What that establishes: **the technique survives a layout change**, which is the
thing that was genuinely unknown. What it still does NOT establish: deriving
offsets for a build nobody has published numbers for. The endpoint had already
published this one and the run was graded against it. That remains the real
test, and it has not happened yet — it needs a run in the window between a
Roblox update and the dump appearing.

Run any stage with:

```
build\eclipse_cli.exe --run tools\dump\stage_x.cpp
```

Roblox must be running and in a game. Nothing here writes to the client.

## What it recovered

Fifteen offsets, derived from nothing and then graded against the conf:

| offset | derived | matched |
|---|---|---|
| `Instance.ClassDescriptor` / `ClassName` | `0x18` / `0x8` | yes |
| `Instance.ChildrenStart` / `ChildrenEnd` / `Parent` | `0x78` / `0x8` / `0x68` | yes |
| `DataModel.Workspace` | `0x158` | yes |
| `Workspace.CurrentCamera` | `0x498` | yes |
| `Camera.Rotation` / `Position` | `0xd8` / `0xfc` | yes |
| `BasePart.Primitive` | `0x188` | yes |
| `Primitive.Rotation` / `Position` / `Size` | `0xc8` / `0xec` / `0x1bc` | yes |
| `FakeDataModel.Pointer` + `RealDataModel` | `0x8b79b58` `+0x1d8` | yes |

## The four techniques

**Anchor plus a backwards pointer scan.** You do not need symbols, you need one
thing that must exist. `"DataModel"` is in memory because the client names the
class with it, and `size == 9 && capacity == 15` proves the bytes are an MSVC
`std::string` rather than a coincidence. From there each level is one sweep
collecting *addresses whose qword is in the previous level's set*:

```
"DataModel" std::string
  <- a pointer to it          = descriptor + ClassName
  <- a field holding that      = instance + ClassDescriptor
  <- a field holding that      = fakeDataModel + RealDataModel
  <- a qword inside the module = the static, as an RVA
```

No layout is assumed. The offsets are whichever `k`/`j`/`m` survived.

**A round-trip invariant.** An Instance is an object whose children all point
back at it through one common offset. Three entries agreeing is not luck. This
one test pins `ChildrenStart`, `ChildrenEnd`, `Parent` and the object base
together, and it cut 521 candidates to 2.

**Signatures.** Nine floats that are orthonormal with determinant +1 are a
rotation matrix, and Roblox puts the position in the twelve bytes right after
it. One pattern yields `Camera.Rotation`, `Camera.Position`,
`Primitive.Rotation` and `Primitive.Position`.

**Discrimination by variance.** The one that generalises furthest, and the one
that fixed every wrong answer below:

- *distinctness* — 95 parts have 95 positions, their shared parents have one
- *on-grid-ness* — Roblox sizes are multiples of 0.05 because a human authored
  them; computed floats are not. `Primitive.Size` scored 37/124 on the grid
  while every rival scored 0
- *churn* — the live DataModel ticks, a dead one is frozen at 0 bytes

## Dead ends, so they are not re-attempted

- **`ClassName` is a pointer to the `std::string`, not the string inline.** The
  console's `read_str_field` tries the pointer first, which is why it works and
  a naive inline read returns garbage.
- **Class descriptors live in the MODULE**, at `0x7ff7xxxxxxxx`, and
  `fx_regions` is writable-only by design. A heap-only sweep cannot see them;
  add `{ base, SizeOfImage }` from the PE header.
- **After a rejoin, TWO instances pass the round trip.** The previous server's
  DataModel is still referenced somewhere. `Parent == 0` does not separate them
  — both are roots. Churn does.
- **Vote-counting picks `Instance.Parent` for `BasePart.Primitive`.** Every part
  has a parent, and parents are full of identity matrices, which satisfy
  orthonormality. The wrong answer scored a perfect 65/65, exactly like the
  right one. Exclude fields already identified, and rank by distinct positions.
- **"Three positive floats under 2100" describes a rotation matrix row**, which
  is how the first `Primitive.Size` attempt landed inside a CFrame.

## Gravity: the first time the endpoint's conf was caught WRONG (2026-09-01)

`stage_l.cpp` derives two offsets the conf gets wrong, and this is the first
run here that graded the endpoint rather than being graded by it.

```
ground truth: Workspace.ReadOnlyGravity = 196.2000

  Workspace.World = +0x3f0     conf says +0x368   MISMATCH
  World.Gravity   = +0x228     conf says +0x1b4   MISMATCH
  (a second copy at +0x22c -- both must be written or a zero does not hold)
```

**Why this one is derivable when attributes were not:** it has GROUND TRUTH.
`Workspace.ReadOnlyGravity` reads the true value -- writing it does nothing, it
is a mirror, but reading it says exactly what to search for. `stage_j`/`stage_k`
failed precisely because no such anchor existed for attribute names, so no
result could be graded. The difference is the whole lesson: **a search you
cannot grade is not a search.**

Both controls passed:

- a sweep for a value that is NOT in the process found **0 objects**, so
  "an object contains the number" is selective rather than vacuous;
- the derived pair round-trips (`Workspace+0x3f0 -> +0x228` reads `196.2000`),
  while the conf's pair reads `-999` from a null pointer.

Exactly one object holds the value, reached through two aliased Workspace slots
(`+0x3f0` and `+0x9e0`) -- so the slot is not unique but the object is.

### What the wrong offsets cost, symptom-first

Worth recording because none of it looked like a bad offset:

| what was seen | what it actually was |
|---|---|
| write fails, `NTSTATUS 0x8000000d` | `Workspace.World` read `0x0`; the write went to `0x1b4` |
| `fly on (gravity 0.0 -> 0)` | non-null pointer to an object whose `+0x1b4` is `0.0` |
| `fly on (gravity 31.0 -> 0)` | a range check accepting any float in 1..2000 |
| flies but **sinks slowly** | gravity never zeroed; the velocity write alone |
| **Roblox crashed** | zeroing every `196.2` under Workspace, not just the world's |

The slow sink is the one to remember: it reads as "nearly working" and is
actually "not working at all", because the velocity write softens a fall it
cannot stop.

### Confirmed across two independent clients

Two Roblox instances were running. The derivation was run against both:

| | pid 10516 (menu) | pid 4628 (in game) |
|---|---|---|
| Workspace | `0x21602d66280` | `0x26fa2557080` |
| world object | `0x21605370080` | `0x26fa33d5080` |
| pointer slots scanned | 122 | 218 |
| **Workspace.World** | **+0x3f0** | **+0x3f0** |
| **World.Gravity** | **+0x228** | **+0x228** |
| second copy | +0x22c | +0x22c |

Different processes, different allocations, different amounts of world loaded --
same offsets. That is a stronger check than the rejoin this file usually asks
for, because nothing is shared between the two.

The conf's pair lands somewhere different in each: `0.0` in one, `1.8750` in the
other, `-999` (null pointer) earlier. It is not that `Workspace.World` is
sometimes empty -- the offsets are simply wrong, and what they point at is
arbitrary.

### A CLI gap this exposed, now fixed

Every measurement before this ran against the WRONG CLIENT. `eclipse_cli` had no
way to choose: `attach()` has always taken a target pid for the UI's picker, but
the CLI never exposed it, so with two clients open it took whichever
`find_pid()` returned first -- an idle menu instance, silently.

`--list` and `--pid N` now exist:

```
> eclipse_cli --list
pid      player               place          placeid
10516    (unknown)            Ugc            0
4628     Temporal_Love        Ugc            123974602339071
```

`--list` loads the offsets first, or every row reads "(unknown)": the usernames
and places are READ out of each client and need the offset table even though
nothing is attached yet.

## The attribute hunt, and why it stopped (2026-08-31)

`fx_attr` would reach Roblox **Attributes** -- the one place a game's own data
lives engine-side rather than in a Luau table. Nothing in either conf names an
attribute offset, so `stage_j` and `stage_k` went looking. **Both failed, and
the failure is worth more than the attempt.**

**What is different about this hunt:** every other stage here recovered
something the conf already had, so the answer was checkable. Here there is no
key to grade against, and worse, **no ground truth at all** -- we do not know a
single attribute NAME that exists in the place being scanned. That makes every
negative result ambiguous in the same way, which is the finding.

### stage_j -- forward from Instances

Read the first `0x400` bytes of 4000 live Instances, and for each 8-aligned
slot asked: does this hold a pointer to memory containing a developer-shaped
identifier, at a CONSISTENT sub-offset across instances?

- Both controls passed: the std::string decoder agreed with `fx_name` on
  **200/200** instances, and produced **0** false positives on Instance headers.
- The result was noise. Best agreement was **10/40**, and the strings were
  engine vocabulary -- `LogServiceJob`, `MemoryCategory_150`, `RunService`.
- First version of the test accepted ANY string within 192 bytes, which passes
  on most of the heap. Tightening it to identifier-shaped names at a repeated
  sub-offset is what made the negative trustworthy rather than merely quiet.

**What that rules out:** if the attribute store were a struct with the key
inline within `0x400` of the Instance, this would have seen it.

### stage_k -- the map-node signature, swept from the heap

If the store is `std::map<std::string, Variant>`, the Instance holds only
`{ _Myhead, _Mysize }` and the keys live in tree NODES, which no forward scan
from the Instance can reach. So stage_k swept all **4.57 GB** for the MSVC
`_Tree_node` shape: three pointers, then an SSO `std::string` at `+0x20`.

It carried the control that matters -- **the same search at `+0x18`, where the
shape says there is no key**:

```
SIGNATURE  key at +0x20: 2173 nodes, 319 distinct keys
CONTROL    key at +0x18: 1100 nodes, 197 distinct keys
        -> NO BETTER THAN THE WRONG OFFSET, signature is meaningless
```

A real signature should find far more at the right offset than at a wrong one.
2173 against 1100 is not discrimination, it is noise at both. And every key
recovered was the client's own: WebRTC codec parameters (`useinbandfec`,
`minptime`, `opus`), telemetry tags (`tag_os_name`, `place_version`,
`universeid`), FACS blendshapes (`JawLeft`, `Pucker`, `TongueUp`), CUDA kernel
params (`cubin_size`, `xmmas_k`, `tile_w`). **No game vocabulary at all.**

### Why it stopped, and what would unblock it

Three searches, three negatives, and they cannot be told apart:

- the place genuinely sets no attributes, so there is nothing here to find; or
- the store has a shape none of these searches match.

**Without a known attribute name, every search here is unfalsifiable** -- there
is no way to separate "did not find it" from "it is not there", and no way to
tell a working search from a broken one. That is the same trap the top of
`CLAUDE.md` describes, arrived at from the other side: not a check that cannot
go red, but a search that cannot be shown to work.

So the next attempt should NOT be another search. It should start by
**manufacturing ground truth**: scan a place where an attribute is known to
exist, with a known name, so the search can be validated on a positive case
first -- exactly what `probe.cpp` did for stage_b when its test was the thing
that was wrong. Candidates, none yet tried:

- a place whose UI displays a value that is plausibly attribute-backed, giving
  a name to sweep for directly (stage_a's technique, which works when there IS
  an anchor);
- comparing two scans of the same instance before and after the game changes
  something about it, so the attribute store shows up as what CHANGED --
  `inspect/changes.cpp` already does exactly this kind of diff.

Until then `fx_attr` stays unbuilt. A guessed offset here reads as plausible
garbage with the status line still green, which is the specific failure the
CLIENT VERSION check exists to prevent.

## Unresolved

- **`Primitive.Rotation` `+0xc8` vs `+0x110`.** A Primitive holds two CFrames.
  They moved together on 85 of 123 character parts, so "which one is live" did
  not separate them. `0xc8` is the conf's; the tie was broken by map ordering,
  which is to say not broken at all.
- **The static is an equivalence class, not a value.** Across three servers, six
  module slots survive as chains to the live DataModel — the client genuinely
  reaches its DataModel through several singletons. All six work. A dumper
  should emit all of them and let the consumer try in order, which would be
  more robust than the single value a conf carries today.

## Stages

| file | what it does |
|---|---|
| `stage_a.cpp` | first pointer scan. Reported "FOUND it" among 297 slots on a wrong anchor — luck, not evidence |
| `stage_b.cpp` | adds the round trip. Returned 0 survivors, which is what sent me to the probe |
| `probe.cpp` | feeds the test `fx_dm()`, a known-good input. It failed there, so the test was wrong, not the search |
| `stage_c.cpp` | the pointer hop plus the module range. First correct run, with both controls |
| `stage_d.cpp` | first rejoin test. Picked the dead DataModel and so graded everything against a corpse |
| `stage_e.cpp` | churn separates live from dead. One of nine recorded slots survived — the conf's |
| `stage_f.cpp` | field offsets by signature. 9 of 12, and the 3 misses were all one cause |
| `stage_g.cpp` | distinctness fixes them. `Primitive`, `Rotation`, `Position` all match |
| `stage_h.cpp` | on-grid scoring gets `Primitive.Size`; the CFrame tie-break fails honestly |
| `stage_i.cpp` | third server. 8 candidates to 6, and the reason there is no unique answer |
| `stage_j.cpp` | ATTRIBUTES, forward from Instances. Found nothing; both controls passed |
| `stage_k.cpp` | ATTRIBUTES, heap sweep for map nodes. Control says the signature is meaningless |
| `stage_l.cpp` | GRAVITY. Derives `Workspace.World` and `World.Gravity`; both MISMATCH the conf |

Every stage carries its controls in the code. `stage_c` and `stage_e` both prove
the test can go green on a known-good input *and* red on a known-bad one before
any result from them is believed — the pattern the top of `CLAUDE.md` asks for,
and the only reason the four wrong answers above were caught rather than shipped.
