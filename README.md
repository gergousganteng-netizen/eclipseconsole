# Eclipse Offset Console — Windows port

The Linux console, ported to run against the **Windows** Roblox client
(`RobloxPlayerBeta.exe`). Same idea, same safety model: you paste C++, it is
compiled to a DLL, loaded into **this** process, and run against the
game's memory from the outside. Writes are dry-run until armed.

Still **pure external data access** — no Luau VM, no bytecode, no injection into
the client, no control-flow writes. `fx_wrblk` is the only write primitive.

## Layout

```
CMakeLists.txt
src/          fx.h · eclipse_core.h at the root; core/ has eclipse_console.cpp,
              cache.cpp, snippet_build.cpp, core_internal.h flat, plus
              memory/ (stubs, read, write — the only code that touches client
              memory), offsets/ (offsets, conf, health), and features/
              (world, input, runtime, window — the snippet-facing modules) ·
              ui/ (eclipse_ui.cpp + host.h/log/dpi/snipwin split out of it,
              client/ — attach, placename, offsets_fetch, launch — and
              snippets/ — tree, trash, watch, bundle) ·
              cli/ (eclipse_cli.cpp) · res/ (app.rc, icon.ico)
web/          the UI — tokens.css + components.css came from the Eclipse kit,
              app.css/app.js/index.html are ours. All of it is ours to edit;
              nothing fetches or regenerates the kit files
              (no conf/ in the repo -- the app fetches offsets_windows.conf
              next to its own exe; see tools/stage_conf.cmake)
tools/        release machinery: bundle_toolchain.ps1, verify_toolchain.ps1,
              build_installer.ps1, installer.iss, stage_conf.cmake
              (dump/ derives offsets from a live client, as a proof)
third_party/  pinned WebView2 SDK
build/        output, plus staged copies of conf/ web/ fx.h — and toolchain/,
              the bundled compiler, which `cmake --build` does NOT produce
dist/         the finished installer
```

### Where the `fx_*` exports live

For what a snippet can *call*, read `src/fx.h`. This is the contributor's
question of where each export is *defined*.

**The rule: an export lives with the state it exposes.** `memory/` holds the
read and write primitives, `offsets/` the offset table plus its conf and health
check, `features/` the snippet-facing modules (`world`, `input`, `runtime`,
`window`), and the attach-state accessors (`fx_dm`, `fx_base`, `fx_pid`,
`fx_armed`) stay with attach in `eclipse_console.cpp`.

**One deliberate exception:** `fx_wrblk` lives in `memory/write.cpp` and stays
there — the write trampoline is private to that translation unit, and that is
exactly what makes the arm gate unbypassable.

The **authority** is the source, not this prose. To list every export and its
home:

```
grep -rn "^FX_API" src --include=*.cpp
```

The paragraphs above summarise the rule; that command is the index.


`conf/` and `web/` are read from disk beside the exe, so both can be edited
without rebuilding.

### Where snippets live

Not in the repo, not beside the exe. One fixed directory per user:

```
%APPDATA%\EclipseConsole\snippets
```

The full path is printed to the console on startup. **Point your editor there** —
that is the tree the sidebar lists, `Save` writes to, and the watcher watches.
Nothing ever copies over it, so a rebuild cannot resurrect a snippet you
deleted, and moving the exe cannot lose one.

A fresh install starts with an **empty sidebar**. The repo does not ship
example snippets — write your own, or paste in what you need. Everywhere below,
`snippets/` means that per-user directory.

## Build

Needs MinGW g++ (C++17) and CMake.

```
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
```

Produces two binaries, plus `conf/` and `fx.h` staged beside them:
`build/eclipse_console.exe` (the UI) and `build/eclipse_cli.exe` (the terminal
one). See **Run** for why it is two.

The console shells out to a compiler at *runtime*, once per snippet. It prefers
`toolchain\bin\g++.exe` beside the exe and falls back to whatever `g++` is on
`PATH`. A dev build has no bundled toolchain, so there the fallback is what
runs and `g++` must stay on `PATH`; an installed copy ships its own and needs
nothing. See **Shipping a build**.

The app locks its own exe while running. A build that fails with
`Permission denied` on `eclipse_console.exe` means it is still open.

## Shipping a build

`cmake --build` produces the binaries. It does **not** produce
`build/toolchain/` — that is the bundled compiler, staged separately, and the
installer refuses to build without it. In order, from the repo root:

```
cmake --build build
powershell tools\bundle_toolchain.ps1     # only when the compiler changes
powershell tools\verify_toolchain.ps1
powershell tools\build_installer.ps1
```

Bump `AppVersion` in `tools/installer.iss` first. The `AppId` GUID is fixed, so
a new version upgrades in place rather than installing alongside. Output lands
in `dist/`.

Three things here are load-bearing rather than incidental:

**The bundled toolchain must share the host's C runtime.** The UI captures a
snippet's `printf` by rebinding fd 1, which only works while the snippet DLL and
the host use the same CRT. A UCRT host loading an MSVCRT-built snippet gets two
independent stdio states, and the output vanishes with no error at all. That is
why `bundle_toolchain.ps1` copies the compiler that built the host rather than
downloading a convenient one, and why `verify_toolchain.ps1` compares their
imports.

**`verify_toolchain.ps1` compiles with the dev install off `PATH`.** A bundle
that is only complete because `C:\mingw64` happens to be reachable looks
identical to a working one until it ships. Three things were missing this way
and each failed silently: `liblto_plugin.dll` (the driver passes
`-fuse-linker-plugin` by default), `as`/`ld` needing the bundle's `bin` on the
child's `PATH` — an absolute path to `g++` is *not* enough — and
`libwinpthread-1.dll`, which snippet DLLs import and which must sit beside the
**exe**, not in `toolchain\bin`, because the host is what `LoadLibrary`s a
snippet.

**The installer is per-user on purpose.** The app rewrites
`conf\offsets_windows.conf` next to its own exe on every Roblox update. Under
Program Files a standard user cannot write there, the fetch fails, and since
running is blocked while offsets are stale the app is unusable without ever
saying why. Moving to Program Files means moving the conf to `%APPDATA%` first.
Uninstall deliberately leaves `%APPDATA%\EclipseConsole` alone.

## Run

**UI** — just launch the exe. It renders `web/` in WebView2 using the Eclipse UI
kit (`tokens.css` + `components.css`). The kit files started as a drop-in and
are now ours: nothing fetches or regenerates them, so they can be edited when a
change genuinely belongs to the component. Reach for `app.css` first when an
override will do — kit rules are shared, and bending one to suit a single
dialog moves every other user of it.

| Control | What it does |
|---|---|
| Sidebar | the sources in `snippets/` — `.cpp`, plus `.h`/`.hpp`/`.inc` and `.html`/`.css`/`.js`, which are editable but dimmed and not runnable. Drag to reorder or to move between groups |
| Tabs | the open files; drag to reorder, locked to the horizontal axis |
| Editor | C++ with syntax highlighting; Tab indents |
| **Run** (F5) | compile → load → execute against the attached process |
| **Run** on something else | stops what is running, then starts yours — the host serialises runs (one stop flag, one hook, one loaded DLL), so they cannot overlap |
| **Disarmed / ARMED** | the write gate. Disarmed, `fx_wrblk` only logs |
| **Save** (⌘S) | writes back to `snippets/` |
| ⌘K | searches the offset table; Enter inserts `fx_off("…")` at the caret |
| Console | real engine output, filterable by level; the divider above it resizes |
| Settings | searchable; every value is one entry in the `SETTINGS` table in `app.js`. Also where **Offsets → Refresh offsets** lives |
| **Recover deleted** | deleting moves to `snippets\.trash` and can be restored — nothing here unlinks a file outright. Labelled rather than a bare bin icon, because an icon reads as *delete* and gave no hint the work was still there |
| **Launch** | starts Roblox, and only appears while detached. Named after whichever bootstrapper you have (`Launch Froststrap`) |
| Status cluster | live pid / base / offset count / DataModel |

**Launch does not hunt for a bootstrapper and run it.** The `roblox-player:`
protocol handler is by definition whichever launcher you actually installed —
Froststrap, Bloxstrap, Fishstrap and stock Roblox all register themselves as it
— so the URI goes to the shell and the right one starts without the console
knowing any install path. Detection exists only to *name* the button and as a
fallback if nothing is registered. Nothing waits on it: the watcher attaches by
itself within a second or two of the client being ready.

**The kit was drawn for a Luau executor; this is not one.** Nothing here runs
script inside the game. The components are mapped onto what the console
actually does — the editor holds C++, ⌘K searches offsets instead of running
commands, and the kit's identity footer (which would need accounts) carries the
attach readout instead. No control in the UI is decorative.

**CLI** — same shape as the Linux original:

```
eclipse_cli.exe --run FILE|FOLDER [--arm] [--conf PATH] [--log PATH]
```

**Use `eclipse_cli.exe`, not `eclipse_console.exe`, from a terminal.** They share
all their logic; the difference is the subsystem, and an executable only gets
one.

`eclipse_console.exe` is GUI subsystem so that launching the app does not flash a
console window. It can only *borrow* a console via `AttachConsole`, and that has
three consequences: the shell does not wait for it, so the prompt returns while
output is still arriving and the two interleave; the prompt is never redrawn
afterwards; and **Ctrl+C goes to the shell, never to us** — measured, the handler
never fired once.

`eclipse_cli.exe` is console subsystem. The shell waits, output is ordered, the
prompt comes back, and Ctrl+C is a real console control event. It stops a running
snippet through the same cooperative path as the Stop button, so the snippet's
cleanup runs and claimed keys are released — which killing the process would
skip.

Both export `fx_*` and emit their own import library, because a snippet binds
back into whichever process loaded it. `build_and_run()` takes the `-l` from the
running module name rather than hard-coding one.

A **folder** runs it as a project, the same as the ▶ on a group header. Worth
having: `--log` is far easier to read than driving the window when something in a
project needs debugging.

`--log` writes the transcript to a file, which stays useful even with the
console binary: it is the only ordered record if a snippet prints a lot.

## What changed from Linux, and why

| Linux | Windows |
|---|---|
| `process_vm_readv` / `writev` | indirect `NtReadVirtualMemory` / `NtWriteVirtualMemory` syscall stubs |
| `/proc` scan | Toolhelp32 snapshot |
| `dlopen` / `dlsym` / `dlclose` | `LoadLibrary` / `GetProcAddress` / `FreeLibrary` |
| `.so`, `-fPIC` | `.dll` (PIC is implicit) |
| `-rdynamic` | host exports `fx_*`, snippets link its import library |
| `/tmp` | `GetTempPath` |
| `/proc/self/exe` | `GetModuleFileName` |
| `system("sed …")` | the error file is read and echoed directly |

Four differences are worth knowing about because they are behavioural, not
mechanical:

**1. `-rdynamic` has no Windows equivalent.** On ELF the host published its
symbols and a `dlopen`'d `.so` bound `fx_*` straight back into it. A Windows DLL
can only bind to an EXE through an import library, so the build emits
`libeclipse_console.a` and every snippet is compiled against it. That is what
the `--out-implib` line in `CMakeLists.txt` is for; without it nothing links.

**2. Offsets are RVAs and need a module base.** Under flux the binary sat at a
fixed VA with ASLR off, so the Linux build never tracked one. Here the base is
read from the module list and added to every static offset. `fx_base()` exposes
it.

**3. Names are boxed, and where the box lives has moved.** `ClassName` is a
pointer to a `std::string`, not an inline one: reading it inline returns empty
every time, one dereference gives `DataModel`, `Workspace`, `RunService`.
`read_str_field()` dereferences first and falls back to an inline read, guarded
by a printable-ASCII check.

`Instance.Name` used to work the same way. Since client
`version-d584fb6c717a43d9` it does not — see **When a name read stops working**
under Offsets. The Linux build had no equivalent of either (it resolved classes
from a vtable map), so this is a genuine layout difference rather than a
translation.

**4. No vtable map and no DM cache.** The Linux build needed `/tmp/vtmap.tsv`
for class names and `/tmp/flux_game_dm` for the DataModel. Neither exists here:
classes come from `Instance.ClassDescriptor → ClassName`, and the DataModel from
`base + FakeDataModel.Pointer → +RealDataModel`. Both are in the conf, so both
survive a version bump without a rebuild.

## How it reads and writes

The obvious way to read another process is two Win32 calls: `OpenProcess` for a
handle, then `ReadProcessMemory` / `WriteProcessMemory` through it. Both are
exactly what a memory-integrity anti-cheat watches for — a hook on the `ntdll`
side sees every read and write, and the `NtOpenProcess` a handle-open issues
names the protected process as its target. The console reaches the same result
without leaving either signature. Neither of these is an injection or a
control-flow write; it is still pure external data access, only obtained without
routing through the two APIs that are trivially hooked.

### Indirect syscall stubs

`ReadProcessMemory` and `WriteProcessMemory` are thin wrappers over
`NtReadVirtualMemory` / `NtWriteVirtualMemory` in `ntdll`, and a hook at the
function entry — an inline patch or an import-table swap — sees the call before
it reaches the kernel. The console never calls the wrappers. At attach it builds
a small trampoline per syscall (`make_nt_stub`):

- copy the **first 8 bytes** of the real ntdll function — the
  `mov r10, rcx` / `mov eax, <syscall number>` prologue — into a private page;
- scan the function body for the `syscall; ret` gadget (`0F 05`) and append a
  `mov r11, <gadget>; jmp r11` that lands on it;
- mark the page `PAGE_EXECUTE_READ` once and never write it again.

Calling the stub sets the syscall number and jumps straight to the `syscall`
instruction **inside ntdll's own memory**, past the hooked function entry
entirely. A function-level hook never sees it. The one thing that defeats it is a
prologue hooked *past* its own syscall instruction, leaving no `0F 05` gadget to
find — and that is treated as fatal, not silently worked around.

There is **no `ReadProcessMemory` / `WriteProcessMemory` fallback.** If the stubs
cannot be built the attach fails outright, because limping back onto the hooked
wrappers is exactly what they exist to avoid.

### Handle duplication

A handle still has to come from somewhere, and `OpenProcess(pid)` on the game
issues an `NtOpenProcess` naming it as the target — the obvious thing to flag. So
the console does not open the game at all. `RobloxCrashHandler.exe` runs beside
the client and already holds a handle to it, to collect a dump if it crashes.
`handle_from_crash_handler` borrows that one:

- find the crash handler's PID by walking `NtQuerySystemInformation`'s process
  list — not `CreateToolhelp32Snapshot`;
- open the **crash handler** with `PROCESS_DUP_HANDLE`. This is the one
  `OpenProcess` that remains, and its target is the crash handler, not the
  protected game;
- enumerate every open handle on the system (`SystemHandleInformation`); for each
  one the crash handler owns, duplicate it in with `NtDuplicateObject` and check
  whether the copy points at the game PID. The match is the game handle, arriving
  with `VM_READ | VM_WRITE | VM_OPERATION | QUERY_INFORMATION` — our process never
  having issued `NtOpenProcess` for the game.

If the crash handler is not running, this returns nothing and attach falls back
to a direct `OpenProcess` on the game: the handle works, but the `NtOpenProcess`
is back in the trace. Unlike the read/write path this fallback is kept — with no
crash handler there is no other stealthy route to a handle, and a working attach
beats none.

## Offsets

`conf/offsets_windows.conf` **next to the exe** is the only offsets file
anything reads. The app fetches the dump for whatever client it attached to and
writes it there itself -- see **Keeping the conf current**.

There is no copy in the repo and no generator. Both existed during the Linux
port (`tools/offsets_windows.h` plus `gen_conf.py`), but the generator only ran
on Linux, so the seed it produced could not be regenerated here -- it could only
go stale, and a stale seed is worse than none: either the version check rejects
it, confusingly, on a first run, or it is silently wrong for the attached
client.

The consequence for anyone adding or fixing an offset: **there is nothing local
to edit.** A wrong value comes from the endpoint's dump for that version and is
replaced on the next Roblox update, so the only durable fix is code that does
not depend on the number. `movement/fly.cpp`'s `find_gravity()` is the worked
example -- it reads a value it can trust, tries the conf key, verifies it, and
searches for the value when the key does not hold it.

### Three kinds of number live in that file

Most entries are **field offsets** — distances inside an object you already
hold, like `Instance.Parent = 0x68`. A few are **module RVAs**: absolute
locations in `RobloxPlayerBeta.exe` holding a pointer to a singleton. These
change on every client build, unlike field offsets, which often survive several.

Four are neither — they are **bit masks** into `Primitive.Flags`, and adding one
to an address reads nonsense:

```
PrimitiveFlags.Anchored    = 0x2
PrimitiveFlags.CanCollide  = 0x8
PrimitiveFlags.CanTouch    = 0x10
PrimitiveFlags.CanQuery    = 0x20
```

Verified on a live character: colliding parts read `0x38`
(`CanQuery|CanTouch|CanCollide`), non-colliding ones `0x30`. The `PrimitiveFlags.`
prefix is the only thing marking them, so read the name before using the number.

```
FakeDataModel.Pointer            = 0x8a5d748
TaskScheduler.Pointer            = 0x879a6e8
VisualEngine.Pointer             = 0x811d0a0
Humanoid.PlatformStatePointer    = 0x9d55d45e
```

The values above are for `version-d584fb6c717a43d9` and are stale the moment
Roblox ships a new build. Read them from the conf, never from here.

On Linux these lived in a separate file (`phasm_dm_ptr.conf`) precisely because
they are a different kind of number. Here they share one conf, so `fx.h` carries
the distinction instead:

```cpp
fx_rva("TaskScheduler.Pointer")    // base + offset — the slot's address
fx_deref("TaskScheduler.Pointer")  // what the slot points at
```

Adding the base to a field offset, or forgetting it on an RVA, both read
garbage; going through these makes that hard to write by accident.

Four entries do not mean what their names suggest. All were checked
against a live in-game client (gravity 196.2, Humanoid at 100/100 HP):

- **`MouseService.SensitivityPointer` is not a pointer — and is now 0.** When it
  last had a value it dereferenced to `0x3e75c28f3e75c28f`: two float32s,
  `0x3e75c28f` ≈ 0.24. It was an RVA to a *value pair*, so `fx_rf(fx_rva(...))`
  rather than `fx_deref`. The current dump publishes it as `0x0`, which the
  conf carries through verbatim — so anything using it reads from the module
  base. Test for zero before trusting it.

- **`TaskScheduler.MaxFPS` is a period, not a rate.** The offset (`0xb0`) is
  correct; the field is a `double` holding seconds-per-frame. On a client capped
  at 144 it reads `0.006944444`, and `1/0.006944444 = 144`. Read as a float, or
  taken at face value as an fps number, it looks like 0. Sweeping the struct
  found no other plausible candidate. Use the wrappers:

  ```cpp
  double now = fx_getfps();     // 144.00
  fx_setfps(240);               // a real write — honours the arm gate
  ```

- **`Camera.FieldOfView` is in radians, not degrees.** It reads `1.22` on a
  default camera, which is 70 degrees. Treating it as degrees makes `tan(fov/2)`
  about 66x too small, so a world-to-screen projection puts players at x=55000 on
  a 1920-wide viewport. A projection worth trusting accepts either unit: a radian
  FOV cannot exceed pi, so anything above that is unambiguously degrees.

- **`Humanoid.PlatformStatePointer = 0x9d55d45e` is dead.** Tested both ways
  against a live Humanoid: as an RVA it lands at `0x7ff7796dd45e`, as a field at
  `0x2593e35c2de`. Both unreadable. It is ~2.6 GB into a module nowhere near
  that size -- stale, or a pattern rather than an offset. Don't rely on it until
  it's re-resolved.

### Two keys whose VALUES are wrong, and why

Different from the four above: the names mean what they say, the numbers are
simply another field's. Both were measured against a live client (2026-09-06)
and both have the same cause, which is visible in the dumper now that it is
open source (`RbxDumperV2`, GPL-3.0, `git.imtheo.lol/theo`): every offset is
found by scanning a controlled scene for a KNOWN value, so two fields carrying
similar values in that scene can capture each other's scan.

- **`GuiObject.BackgroundTransparency` is really `0x564`.** The conf publishes
  `0x54c`, which is where `BorderColor3` starts -- and `BorderColor3 = 0x54c` is
  correct. Read across a live Frame: `0x540` is `BackgroundColor3`, then
  `0x54c/0x550/0x554` hold `0.752941` three times (a Color3), and `0x564` holds
  `0.752` alone (the float). The dumper searches `0.752f` for the transparency
  and `Color3{0.752941f, ...}` for the border, and the float scan reaches the
  colour's first component first.

- **`Humanoid.Sit` is really `0x1de`.** The conf publishes `0x1dd`, which is
  `RequiresNeck` -- and `RequiresNeck = 0x1dd` is correct. Both are bools, and a
  bool scan for `1` matches any true byte in range. Confirmed by watching the
  block on ONE Humanoid across seven sit/stand transitions: `0x1de` tracked the
  seat exactly, `0x1dd` never moved, and `0x1e0` (`UseJumpPower`) never moved
  either -- which is what rules it out, because sampling two DIFFERENT
  characters had made `0x1e0` look just as good a candidate as `0x1de`.

Neither is used by anything in this tree. They are written down because the
conf is refetched per version, so the wrong number comes back every update and
the next person to reach for either field will otherwise re-derive this.

### Keeping the conf current

Keeping the conf external mostly preserves the property the Linux README called
out: a Roblox version bump is a conf change, not a recompile. The app leans on
that hard enough to do it for you.

On attach it reads the client's own `version-…` directory name and compares it
to the conf. On a mismatch it fetches `offsets.imtheo.lol/<version>/offsets.hpp`,
validates it (the file must *declare* the version being asked for, and parse to
a plausible number of offsets), writes it through a temp file so a failure
cannot leave half a conf, then re-attaches through it. Only that re-attach
clears the stale flag.

**Running is blocked while the conf is stale.** Not warned about — blocked, in
both binaries. The alternative is an armed write to an address that was never
right.

Two things follow from how the fetch is gated. It is tried **once per version**,
because a version with no published dump would otherwise mean a request against
someone else's server on every re-attach and every watcher tick, for an answer
that will not change. And a dump can be *republished* for a version already
fetched, with fields added. Neither case recovers on its own, so there is a
manual override: **Settings → Offsets → Refresh offsets**. It ignores the
once-per-version gate — a person pressing a button is the new information that
asking again might be worth it.

### Where the version check earns its keep

`UNVERIFIED` is a much narrower net than it looks. `dm_verified` is exactly
`class_of(dm) == "DataModel"`, so it exercises three offsets:
`FakeDataModel.Pointer`, `Instance.ClassDescriptor`, `ClassDescriptor.ClassName`.
Everything else — `Instance.Name`, all of `Camera.*`, `Primitive.*`,
`Humanoid.*` — is unchecked, and a wrong value there reads as plausible garbage
with the status line still green. Verified by corrupting `Instance.Name` and
watching it report the DataModel as verified.

That is why the client-version check is load-bearing rather than a convenience.
It is the only thing standing between a conf for the wrong build and an armed
write to an address that was never right.

### When a name read stops working

Client `version-d584fb6c717a43d9` moved an instance's name behind a container
pointer, and this is the counter-example to "a version bump is never a
recompile" — no offset value could express it, because the *shape* of the read
changed:

```
old   std::string* at   inst + Instance.Name
new   std::string  at  *(inst + Instance.NameContainer) + Instance.Name
```

Note it is not simply one more dereference. The new one is the string itself,
inline, so it is one deref added and one removed. Verified against live memory,
where `container + Name` reads `Workspace` in place: bytes `57 6f 72 6b …` at
+0, length `9` at +0x10, capacity `15` at +0x18.

The failure is worth recognising because it is so selective. `class_of` goes
through `ClassDescriptor` and was untouched, so the DataModel still verified and
the status line stayed green — while every name-driven lookup (`fx_name`, and
so `fx_find` and `fx_service`) silently returned nothing, and offset-only paths
like the FPS cap kept working perfectly. If names come back empty while
everything else looks fine, suspect the shape of the read, not the numbers.

`read_name()` skips the deref when the conf has no `NameContainer` key, so an
older conf still works against an older client.

### When the DataModel is replaced

A rejoin, a teleport, or a round ending in a game that swaps places replaces
the DataModel, and every pointer derived from the old one is then reading freed
memory. The visible symptom is not a wrong number — it is a hang.

`fx_nchildren` computes `(end - begin) / 0x10` from two pointers read out of
the game. On a freed instance those are unrelated, and the count comes back in
the billions. `fx_find` then walks it at four syscall reads an entry, which
runs for hours — and because the snippet is *busy* rather than sleeping, Stop
cannot reach it. Found in Rivals, whose lobby-to-match transition replaced the
DataModel twice in six seconds; the console had to be killed from Task Manager.

Two guards, both in `fx.h`:

- **`FX_MAX_CHILDREN` (65536).** An implausible count means the pointer is
  stale, so it returns 0 rather than trusting it. The DataModel itself has
  ~145 children, so the bound is nowhere near anything real. `fx_ptr_ok()`
  rejects the container and `begin` before they are dereferenced.
- **`fx_find` and `fx_service` poll `fx_running()`.** Even a legitimate walk is
  thousands of syscalls now that a name costs the extra `NameContainer`
  dereference, and a snippet that cannot be stopped mid-walk has to be killed.

Two host-side walks needed the same bound: the one inside `fx_textbox`, which
runs on **every** `fx_typing()` call, and the process-picker enumeration, which
runs on the **UI thread** where a garbage count freezes the window itself.

The consequence to know: during a swap a snippet now briefly sees an empty
world — no players, no character — and recovers on the next tick. Anything that
assumes a non-empty player list should watch `fx_dm_gen()` and back off, which
is what the shipped snippets do.

## The in-game panel

A second WebView2 window glued to the attached client, showing the console's log
and arm state over Roblox so output does not need alt-tabbing to read. Click its
tab to collapse it to a strip. Settings → Console → **In-game panel** turns it
off; it is on by default and only exists while attached.

It is a **view onto host state, not a second app**. Lines reach it through
`flush_logs` — the same funnel that feeds the console's own log pane — and arm
state through `push_state`, so the two can never disagree about whether writes
are armed. Its buttons go through the existing `handle()` messages (`ARM:`,
`STOP`), not a parallel path. `web/hud.html` links `tokens.css`, so it is the
same palette as the app rather than a lookalike.

### What input a window like this can have

Measured, because it decides the whole design:

| | |
|---|---|
| clicks | **yes** — mouse messages are hit-tested by position, focus is irrelevant |
| wheel | **yes**, but only because Windows 10's "scroll inactive windows when I hover" is on by default. With it off the wheel goes to the focused window and the log will not scroll |
| keystrokes | **never** — they go to the focus window, and this one refuses focus |

So the panel is click-and-read. Text entry stays in the app, and that is not a
WebView2 limitation to configure around: exactly one window in the system has
keyboard focus, and taking it means an unfocused Roblox that ignores WASD.

`WS_EX_NOACTIVATE` must be applied **before the first `ShowWindow`**. Set
afterwards, the window has already activated once, which pulls focus off the
game — the exact thing the flag exists to prevent.

### Z-order, and four attempts at it

The rule wanted: above Roblox, below whatever you alt-tab to. `hWndInsertAfter`
names the window to sit **below**, so passing the game's HWND puts the panel
*behind* the game — which looks identical to the window never having been
created, and cost an hour on its own.

The remaining problem is that Windows raises the game the instant it is
activated, so anything that has to *re-assert* the panel afterwards is a frame
late and blinks. What was tried:

- **Hiding instead of re-ordering.** No blink, but the panel vanishes whenever a
  window sits over it. Rejected: the point is reading the log over the game
  while something else is focused.
- **Owning the panel with the client window** (`GWLP_HWNDPARENT`). Windows keeps
  an owned window above its owner permanently — perfect z-order, no polling, no
  event, no race. Rejected after real play: cross-process ownership attaches the
  two threads' input queues, so cursor visibility bleeds between them (the
  pointer disappears over the panel, in third person with a free mouse, not just
  under mouse-lock) and message processing serialises with the game's, so the UI
  hitches when the game does. The code is kept and switchable, with the
  measurement written beside it.
- **Treating a hover over the game as focus**, to pre-empt the raise. Killed the
  click blink completely, but parking the cursor over the game while another app
  was focused put the panel above that app. Rejected, and so was the wider
  version of the same idea (stay topmost unless the focused window overlaps).
- **Pre-empting on the click itself**, which is what ships. A `WH_MOUSE_LL` hook
  sees the button-down before it reaches its target, so a click on the game
  makes the panel topmost *before* Windows raises it. It fires on the act of
  focusing rather than on the cursor resting somewhere, so the panel never
  climbs above an app it should be under. All four buttons count — X1 and X2
  will focus a window like any other.

Alt-tab and taskbar clicks have no equivalent early signal and fall back to
`EVENT_SYSTEM_FOREGROUND`, which is delivered *after* the activation. Those can
still blink.

Two smaller things that are easy to get wrong: the panel is rounded by DWM
(`DWMWA_WINDOW_CORNER_PREFERENCE`) rather than by `border-radius`, because a
CSS-rounded corner on a square window leaves WebView2's default white showing
through it; and `hud_destroy()` runs on the main window's `WM_CLOSE`, because
the webview library's shared window refcount never reaches zero while a second
window lives, leaving the process running with an orphaned panel and no console.

## Snippets

The repo ships no example snippets — the sidebar starts empty and you write
your own. The kinds the console supports are documented below: **Groups** and
**Projects** for structure, **Includes in a bare snippet** for what the wrapper
does around a paste, **Files are watched** for external edits, **Snippets that
keep running** for hotkey loops with a real Stop, **A loop that writes every
tick** for the dry-run collapsing behind writes, and **The local character**
for the helpers that make Players / LocalPlayer / Character usable without
walking the chain by hand.

### Groups

A group is a **subdirectory** of `snippets/`, and groups nest:

```
snippets/
  set_fps.cpp
  movement/
    walkspeed.cpp
    advanced/
      noclip.cpp
```

Everything is managed from the sidebar. Right-click empty space for **New
snippet** / **New group…**; each group header carries a **+** for a snippet
inside it; right-click a header for **Rename group…** / **Ungroup**.

**Dragging is a real move on disk.** Drop a snippet on a group header and the
file is renamed into that folder; drop it on empty space below the list and it
goes back to the root. Whole groups drag too, and their members' paths follow for
free — that is the pay-off of the directory being the source of truth. Dropping
*on a row* still means "sit next to this one", which only applies within a group,
so those events are left to bubble up to the header rather than being swallowed.

`snippets/.order` stores paths (`movement/advanced/noclip.cpp`) and keeps its old
semantics: unknown files are appended, missing ones skipped, and any reorder
rewrites it from what is actually on screen. Collapse state is *not* in there —
that is UI state, so it lives in the page's localStorage rather than in something
you might commit.

**Ungroup, not Delete.** It moves the members up into the parent and removes the
empty folder; anything that could not move (a name clash) keeps the group alive
rather than being orphaned. No folder operation in here can destroy a snippet.

**Empty groups are real.** The host sends the group list *separately* from the
snippets, because inferring groups from snippet paths means a group only exists
while it holds something — and a newly made one would be invisible until you put
a file in it.

### Includes in a bare snippet

A bare body is wrapped in `fx_run()`, and an include cannot go inside a function
— left in place, `#include <cmath>` fails with a wall of errors from inside
`math.h` that never names the real cause. So include lines are lifted above the
wrapper, and the line they came from is left blank so the body keeps its own
numbering.

A `#line 1` sits just before the body, so g++ reports **the line you typed** —
without it, the wrapper's two prepended lines make a mistake on line 3 read as
line 5.

### Files are watched

`snippets/` is watched while the app runs, so editing a file in another editor,
dropping one in, or making a folder shows up without a restart. The build always
compiled from disk, so without this the sidebar and the compiler could disagree
silently — the worst of both.

The watcher rearms with `FindNextChangeNotification` *before* it settles the
burst, and again for each notification it absorbs. A change-notification handle
is not reset by waiting on it — only `FindNextChangeNotification` resets it — so
a settle loop that just waits again gets `WAIT_OBJECT_0` back instantly, forever.
That spins a core and never reaches the refresh, which looks exactly like the
watcher not being wired up at all.

An external change to the **open** file reloads the editor only when the buffer
is clean. Unsaved edits win, and the dirty dot already says why the editor is not
following disk. Same rule an editor uses: reload a clean buffer, never a dirty
one.

### Projects

A group whose folder holds a **`.project`** file is a *project*: every `.cpp` in
it goes on one `g++` line, so the files can call into each other. The header
shows a ▶ instead of listing N independent snippets, and **Run on any member
builds the project** — the file alone would not link anyway, since its siblings
hold the symbols it calls.

```
demo_project/
  .project          <- the marker; may be empty
  main.cpp          <- defines fx_run()
  client_info.cpp
  world.cpp
```

Nothing is wrapped in a project. A bare body only works when there is exactly one
file to put the wrapper around; with several there is no way to say which, so one
file must define `fx_run()` itself.

**A marker file, not a rule about the contents.** Deriving it — "this folder is a
project if some file defines `fx_run`" — would reuse a rule the console already
has, and costs no new concepts. It was rejected because dropping a full snippet
into a group would then silently turn it into a project and stop its neighbours
being runnable, with nothing on screen to say why. The marker is also the natural
place for per-project settings; `flags = -O3 …` is honoured today, and the file
may otherwise be empty.

Toggle it from a group header's context menu: **Make this a project** / **Back to
a plain group**. Unmarking deletes only the marker — the code is untouched.

**It is a tree, not a folder you navigate into.** Every code editor's file pane
works this way, because drilling in costs the context of everything else and buys
back/up affordances you only need *because* you drilled in. Finder and Explorer
drill in and pay for it with two separate mechanisms — history (`Alt+←`) and
hierarchy (`Alt+↑`) — plus a breadcrumb. That trade only pays off when the tree is
too big to show at once, which a snippet sidebar never is.

Depth is capped at 8. `safe_rel()` is what stands between a name typed in the
page and a write outside `snippets/`: every segment must be a plain name, so no
path can climb out however deeply it nests. The cap keeps the recursive scan and
the on-disk path length bounded.

A snippet is either a bare body or a full `extern "C" void fx_run(){…}` /
`int run(){…}`, exactly as on Linux.

### Snippets that keep running

Stop is **cooperative**, and has to be: the host holds a snippet's DLL loaded for
exactly as long as `fx_run()` has not returned, so terminating the thread would
unload code with its own frames still on the stack. A loop that never checks
cannot be stopped short of killing the app.

| | |
|---|---|
| `fx_running()` | 0 once Stop is pressed. Poll it as the loop condition |
| `fx_sleep(ms)` | interruptible sleep; returns 0 on Stop rather than finishing the wait |
| `fx_key(vk)` | is that key down right now |
| `fx_key_seq(vk)` | how many times it has gone down since the run began |
| `fx_key_pressed(vk)` | true once per press (built on `fx_key_seq`) |
| `fx_wait_key(ms)` | park until the next key-down / Stop / timeout; `ms == 0` waits forever |
| `fx_claim_key(vk,on)` | swallow the key so the game never sees it |

Keys are written by name, not by hex — `fx.h` defines the `VK_*` set (function
keys, arrows, modifiers, numpad, mouse buttons, punctuation). **Letters and
digits need no constant at all**, because a virtual-key code for those *is* the
ASCII value of the uppercase character:

```cpp
if (fx_key_pressed('K'))                      // the K key
if (fx_key(VK_SHIFT) && fx_key_pressed('W'))  // Shift+W
```

Mind the uppercase: `'k'` is `0x6B`, which is `VK_ADD` — numpad plus. Lowercase
letters are not their own key codes, and the compiler cannot warn you.

`fx.h` deliberately does **not** include `<windows.h>` to get these. That would
add about a second to every snippet compile and drag in the `min`/`max` macros,
and Run should feel immediate. The definitions are individually `#ifndef`-guarded
instead, so a snippet that pulls in `<windows.h>` for its own reasons works
either way round.

### A loop that writes every tick

Disarmed, `fx_wrblk` logs every write it *would* have made — and a snippet that
rewrites the camera every tick makes thousands a second. Two things keep that
from taking the app down with it.

**Runs collapse.** Consecutive writes to the same destination become one line
with a count:

```
  [dry-run] would write 12 bytes -> 0x1f4a2c30  [00 00 80 3f ...]  ×418 (value varying, last shown)  (arm to write)
```

Collapsing keys on the *destination*, not on the whole line. An aim or lock
snippet writes a slightly different value every tick, so byte-identical runs
would essentially never occur and nothing would collapse — and the useful fact
is "this field is being hammered", not five hundred near-identical hex dumps.
Whether the bytes actually held still is reported rather than assumed. Nothing
is discarded: every write is counted, and a run is closed by a write elsewhere,
by any other log line (so ordering holds), by arming, or by the run ending.

An open run is also closed every 250 ms. Otherwise a loop hammering one address
would keep it open until you stopped the snippet, and a dry run that prints
nothing at all while writing furiously reads as broken.

**The log is coalesced, and that part is not cosmetic.** Lines are batched into
at most one queued UI update at a time. Posting each line individually meant a
dispatch, a script evaluation, a DOM insert and a forced layout *per line*,
which a worker thread outruns without effort — and since **Stop and Arm arrive
through that same queue**, they ended up behind every line already in it. A
spamming snippet took a very long time to stop, not because the click was
ignored, but because it was thousands of log lines back in the pump while the
snippet kept adding more. If the page still cannot keep up, the oldest lines are
dropped and the count says so rather than the log quietly lying.

### The local character

Walking `Players` → `LocalPlayer` → `Character` by hand is two traps in one, and
five snippets each hand-rolled it before this existed.

| | |
|---|---|
| `fx_player()` | LocalPlayer, or 0 |
| `fx_character()` | the current Character, or 0 while dead or loading |
| `fx_gen()` | a counter that changes whenever the Character does |
| `fx_parts(out, max)` | the character's BaseParts, class-filtered |

**`fx_gen()` is the general answer to stale pointers.** Anything derived
from the character — a Humanoid, a Primitive, a part list — is void the moment
that number moves, and one integer compare is the whole check:

```cpp
static unsigned gen = 0;
if (fx_gen() != gen || !hum) { gen = fx_gen(); /* re-resolve */ }
```

Two things about that line. Never compare it against 0 to mean "not resolved
yet" — it never resets, precisely so a stale holder cannot get a coincidental
match. And keep the `|| !hum`: a character is assembled piece by piece, so the
Humanoid frequently does not exist at the instant the generation ticks. Without
the retry you resolve once, find nothing, and stay empty forever because the
generation never moves again.

`fx_parts` exists because reading `BasePart.Primitive` on a non-BasePart returns
whatever sits at that offset, and it looks pointer-shaped — `Script` gives back a
module address, `Shirt` gives back the float `1.0`, `Humanoid` gives back
garbage. Writing a flags byte through one corrupts the client. The whitelist
lives in the host so nobody rediscovers that. It descends one level into
`Accessory`/`Model`/`Tool`/`Folder`, since a welded hat or tool collides too.

The caching is the other half: `fx_service` compares class names across every
service in the DataModel, so calling it per frame is very expensive. It runs
once and re-runs only if the DataModel changes; the per-call cost is one read,
throttled to about a frame.

**End a hotkey loop with `fx_wait_key`, not `fx_sleep`.** They look
interchangeable — both return 0 on Stop — but polling loses keystrokes. Measured,
same 12 taps sent as fast as `SendKeys` emits them:

| loop tail | caught |
|---|---|
| `fx_sleep(30)` + `fx_key` edge-detect | **1 of 12** |
| `fx_wait_key(250)` + `fx_key_pressed` | **12 of 12** |

A tap whose down and up both land inside one 30ms interval is invisible to a
poll, and `Sleep`'s granularity is the ~15.6ms scheduler tick, so the interval is
usually worse than asked for. `fx_wait_key` is woken *by* the keystroke instead:
sub-millisecond, and idle at 0% CPU. Pass a timeout anyway if the loop has
periodic housekeeping — it wakes on either.

Macro-speed input is handled. Twenty down/up pairs submitted in a single
`SendInput` call — zero gap, faster than any hardware or software macro produces
— arrive as twenty separate presses:

| loop body | handled, 1.5s window |
|---|---|
| trivial | 20 of 20 |
| 50ms of work | 20 of 20 |

That second row only became true once the wake signal was a **semaphore** rather
than an event. An event is binary, so twenty presses landing while the loop was
busy collapsed into one signal: the loop woke once, handled one, and drained the
rest at one per timeout — 6 of 20 in the same window. Nothing was ever lost,
since `fx_key_seq` had already counted them, but "not lost" and "not late" are
different properties. A semaphore counts, so each press is its own release.

The hook behind this is a `WH_KEYBOARD_LL` owned by **the host**, on its own
thread with its own message pump. A snippet never installs one and never supplies
a callback, which is what makes it safe: a hook proc living in a snippet's DLL
would be called after that DLL unloaded. The hook is started on first use and
torn down when the snippet ends, and every claimed key is released at the same
point — a claim outliving its snippet would leave that key dead session-wide with
nothing on screen to explain why.

Two limits worth knowing. The hook proc must stay well under
`LowLevelHooksTimeout` (300ms default) or Windows silently stops calling it —
ours does only a few atomic stores, so there is room, but don't expect to add
work there. And if Roblox is running **elevated** while the console is not, UIPI
blocks the hook from seeing input aimed at it; run both at the same integrity
level.

## Verified working

Against a live in-game `RobloxPlayerBeta.exe` on client
`version-d584fb6c717a43d9` (base `0x7ff677180000`, 389 offsets, handle acquired
via the crash handler):

- attach, module base, conf load, DataModel resolved **and class-verified**
- 145 DataModel children enumerated with class and name
- names through the container: `Workspace`, `Players`, `Run Service`,
  `NonReplicatedCSGDictionaryService` — short *and* heap-allocated both correct
- `fx_service`, `fx_find`, `fx_character` all resolve; `Head`,
  `HumanoidRootPart` and `Humanoid` found by name
- Humanoid reads `100.0/100.0` HP, WalkSpeed `16`, JumpPower `50`
- Head sits `1.17` studs above the HumanoidRootPart, which is the shape a
  correct `Primitive.Position` gives
- camera FOV `1.22` rad (70°), viewport `1920x1080`, FPS cap `144`
- `VisualEngine.ViewMatrix` projects a point 20 studs down the camera's forward
  axis to `960, 540` — exact screen centre — and agrees to the pixel with the
  independent `Camera.Rotation`/`Position` basis path

The write path (dry-run gate logging without writing, armed write returning 4
bytes with the value provably unchanged) was verified on an earlier client and
has not been re-run since; the gate itself is unchanged.

A DataModel's `Instance.Name` is not the place name — it reports something like
`LuaApp` or `Ugc` regardless of whether you are in a game. `DataModel.PlaceId`
is the actual per-place identifier, and it is what the process picker resolves
a game title from.
