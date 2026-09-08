// changes.cpp -- what did that button DO?
//
// dex answers "what is at this offset right now". This answers the other half:
// which offsets MOVED while you did something. Press M, fire a shot, press M
// again, and read back every field of that object which changed in the window,
// rarest first.
//
// READ ONLY. It sweeps one object's bytes and compares them against the last
// sweep. Nothing here writes, so nothing here needs the arm gate.
//
// WHAT IT WILL NOT FIND, AND THIS IS NOT A LIMITATION TO ENGINEER AROUND.
// Ammo, cooldowns, score, gun state: in most games these are LUAU values -- a
// local in a LocalScript, a field in a module table, sometimes an Attribute.
// None of that is a field on an Instance, so no amount of sweeping an
// Instance's bytes will ever reach it. dex.h says the same thing about its
// scanner and it is worth saying twice: this reads ENGINE state, the fields the
// C++ side of Roblox keeps on an object. Everything the game's own scripts
// invented lives somewhere this cannot see.
//
// So what IS in reach:
//   - Humanoid: health, walkspeed, jump, the state enum
//   - Primitive: position, velocity, the flags byte
//   - Camera, Viewport, the engine's own bookkeeping
//   - a *Value instance's payload, when a game happens to use one
//   - the DISPLAYED number, which is a different thing from the real one --
//     a HUD TextLabel's string is engine state and does move when you fire.
//     Point WATCH_ADDR at the label rather than at the gun. dex marks any
//     field that decodes as printable text, which is how you find it.
//
// WHY RAREST FIRST, AND WHY THE NOISE FILTER IS THE WHOLE DESIGN.
// A live object is never still. A Primitive's position and velocity move every
// single frame, and a naive diff over 0x900 bytes reports two hundred slots and
// buries the one you care about. So each slot is COUNTED, not just flagged:
//   - a slot that moved on nearly every tick is a live value (position, a
//     timer, an animation phase). Summarised as a count, never listed.
//   - a slot that moved ONCE in a four-second window is a reaction to what you
//     just did. That is the signal, and it sorts to the top.
// The cut is a fraction of the ticks rather than an absolute count, so it holds
// whether the window was half a second or a minute.
//
// BOTH DECODES, ALWAYS. Every row prints f32 AND i32, and adds the f64 reading
// when a slot and its neighbour moved together on an 8-aligned pair. A double
// read as a float comes back 0.0, which looks exactly like a field that went
// empty -- and a tool built for FINDING fields is the last place to repeat that.
//
// STALENESS. The address is absolute. A respawn frees it, so fx_gen() moving
// mid-window aborts the recording rather than diffing whatever owns that memory
// now: a report built from two different objects is worse than no report.

#include <algorithm>

// The object to watch. 0 resolves the local character's Humanoid, which is the
// useful default -- health, speed, jump, state enum, and whatever the game
// bolted on beside them. Paste an address from dex (click a field name to copy
// one) or from Cheat Engine to watch anything else.
static const uint64_t WATCH_ADDR = 0;

static const size_t   WINDOW     = 0x900;   // bytes swept -- the width dex scans
static const unsigned HZ         = 30;      // sweep rate while recording
static const int      MARK_KEY   = 'M';     // uppercase: 'm' is a different code
static const int      MAX_REPORT = 40;      // rows printed; the rest are counted
// A slot moving on this share of the ticks or more is a live value, not a
// reaction to anything you did.
static const double   NOISE_FRAC = 0.8;
// ...but only once the window is long enough for the fraction to mean anything.
// Over three ticks the cut lands at 2, so a field that moved twice would be
// filed as "always moving" and never shown -- the filter would be hiding the
// exact thing a short deliberate window was opened to catch.
static const unsigned NOISE_MIN_TICKS = 15;   // half a second at 30 Hz

static const int SLOTS = (int)(WINDOW / 4);

static unsigned char g_base[WINDOW];    // the object as it was when M was pressed
static unsigned char g_prev[WINDOW];    // the previous sweep
static unsigned char g_cur [WINDOW];
static unsigned      g_changes[SLOTS];  // ticks each slot moved on

// The Humanoid under the current character, matched on CLASS rather than on the
// name, because a game is free to rename the instance.
static uint64_t find_humanoid() {
    uint64_t chr = fx_character();
    if (!chr) return 0;
    uint64_t n = fx_nchildren(chr);
    for (uint64_t i = 0; i < n; i++) {
        uint64_t c = fx_child(chr, (int)i);
        if (c && strcmp(fx_class(c), "Humanoid") == 0) return c;
    }
    return 0;
}

// What the conf calls this offset on this class, or "" if it names nothing.
// An UNNAMED hit is the interesting one: a field this game has that our offsets
// have never heard of.
static const char* field_name(const char* cls, uint64_t off) {
    size_t clen = strlen(cls);
    for (int i = 0, n = fx_off_count(); i < n; i++) {
        const char* k = fx_off_key(i);
        if (!k) continue;
        const char* dot = strchr(k, '.');
        if (!dot) continue;
        if ((size_t)(dot - k) != clen || strncmp(k, cls, clen) != 0) continue;
        if (fx_off(k) == off) return dot + 1;
    }
    return "";
}

static float  f32_at(const unsigned char* p) { float  v; memcpy(&v, p, 4); return v; }
static int    i32_at(const unsigned char* p) { int    v; memcpy(&v, p, 4); return v; }
static double f64_at(const unsigned char* p) { double v; memcpy(&v, p, 8); return v; }

static void report(const char* cls, unsigned ticks) {
    if (!ticks) { fx_logf("  nothing recorded -- the window was empty"); return; }

    const unsigned cut = ticks >= NOISE_MIN_TICKS
                       ? (unsigned)(ticks * NOISE_FRAC) : 0;
    int idx[SLOTS], n = 0, noisy = 0;
    for (int i = 0; i < SLOTS; i++) {
        if (!g_changes[i]) continue;
        if (cut && g_changes[i] >= cut) { noisy++; continue; }
        idx[n++] = i;
    }

    // Rarest first. A field that moved once is the answer; one that moved forty
    // times is a counter you happened to catch running.
    std::sort(idx, idx + n, [](int a, int b) {
        if (g_changes[a] != g_changes[b]) return g_changes[a] < g_changes[b];
        return a < b;
    });

    // Say when the filter did not RUN. "0 moving continuously" after a short
    // window otherwise reads as "nothing was noisy", which is a different fact
    // from "nothing was filtered" and the one that sends you looking in the
    // wrong place.
    if (cut)
        fx_logf("  %u ticks (%.1fs)  --  %d slot(s) changed, %d moving continuously",
                ticks, (double)ticks / HZ, n, noisy);
    else
        fx_logf("  %u ticks (%.1fs)  --  %d slot(s) changed, everything listed"
                " (window under %u ticks, noise filter off)",
                ticks, (double)ticks / HZ, n, NOISE_MIN_TICKS);
    if (!n) {
        fx_logf("  nothing moved that was not already moving -- try a longer window,"
           " or watch a different object");
        return;
    }

    int shown = n < MAX_REPORT ? n : MAX_REPORT;
    for (int r = 0; r < shown; r++) {
        int i = idx[r];
        const unsigned char* a = g_base + i * 4;
        const unsigned char* b = g_cur  + i * 4;
        uint64_t off = (uint64_t)i * 4;
        const char* nm = field_name(cls, off);
        fx_logf("  +0x%-4llx %-18s x%-4u  f32 %.4f -> %.4f   i32 %d -> %d",
           (unsigned long long)off, nm[0] ? nm : "-", g_changes[i],
           f32_at(a), f32_at(b), i32_at(a), i32_at(b));

        // The pair reading, when the neighbouring slot moved in the same window
        // and the pair is 8-aligned. This is the line that stops a double from
        // reading as a field that dropped to zero.
        if ((off % 8) == 0 && i + 1 < SLOTS && g_changes[i + 1])
            fx_logf("           as f64: %.6f -> %.6f", f64_at(a), f64_at(b));
    }

    // Never let a cap pass silently: a truncated list reads exactly like a
    // complete one.
    if (shown < n)
        fx_logf("  ... and %d more, not shown (raise MAX_REPORT)", n - shown);
}

extern "C" void fx_run() {
    uint64_t target = WATCH_ADDR;
    if (!target) {
        target = find_humanoid();
        if (!target) { fx_logf("no Humanoid -- spawn in, or set WATCH_ADDR"); return; }
    }
    std::string cls = fx_class(target);
    if (cls.empty()) cls = "?";

    fx_logf("changes: watching %s at 0x%llx (%d bytes)", cls.c_str(),
       (unsigned long long)target, (int)WINDOW);
    fx_logf("  press M to start a window, M again to report. Stop ends it.");

    unsigned gen       = fx_gen();
    bool     recording = false;
    unsigned ticks     = 0;

    while (fx_running()) {

        // fx_key_pressed FIRST: it consumes one press per call, so gating ahead
        // of it would eat presses made while a chat box was open and then lose
        // the next real one.
        bool hit = fx_key_pressed(MARK_KEY) && fx_input_ok();

        if (!recording) {
            // Re-resolve between windows only -- doing it mid-window would swap
            // the object out from under the diff.
            //
            // THE `|| !target` HALF IS NOT OPTIONAL. A character is assembled
            // piece by piece, so on a rejoin the generation ticks BEFORE the
            // Humanoid exists: resolve once on that edge and you get 0, and
            // because the generation never moves again you stay at 0 forever
            // and every M reports "no target". Found exactly that way, leaving
            // and rejoining a game. Retrying costs a bounded walk over the
            // character's children, at most once per idle wake.
            if (fx_gen() != gen || (!target && !WATCH_ADDR)) {
                bool moved = fx_gen() != gen;
                gen = fx_gen();
                if (!WATCH_ADDR) {
                    uint64_t was = target;
                    target = find_humanoid();
                    cls = target ? fx_class(target) : "?";
                    if (target && target != was)
                        fx_logf("  watching %s at 0x%llx", cls.c_str(),
                                (unsigned long long)target);
                } else if (moved) {
                    // A pinned address cannot be re-resolved, and after a world
                    // change it points at freed memory. Say so rather than
                    // letting the next window diff whatever moved in there.
                    fx_logf("! world changed -- WATCH_ADDR 0x%llx is now stale;"
                            " re-copy it from dex", (unsigned long long)target);
                }
            }
            if (hit) {
                if (!target) {
                    fx_logf("  no target yet -- not spawned, or the character is"
                            " still being built");
                } else if (fx_rdblk(target, g_base, WINDOW) != WINDOW) {
                    fx_logf("  cannot read 0x%llx -- gone?", (unsigned long long)target);
                } else {
                    memcpy(g_prev, g_base, WINDOW);
                    memset(g_changes, 0, sizeof g_changes);
                    ticks = 0;
                    recording = true;
                    fx_logf("  recording -- do the thing, then press M again");
                }
            }
            if (!fx_wait_key(250)) break;   // idle: woken by the keypress itself
            continue;
        }

        // ---- recording ----
        if (fx_gen() != gen) {
            gen = fx_gen();
            recording = false;
            fx_logf("x character changed mid-window -- recording dropped, the address"
               " it was reading is freed");
            continue;
        }
        if (fx_rdblk(target, g_cur, WINDOW) != WINDOW) {
            recording = false;
            fx_logf("x read failed mid-window -- recording dropped");
            continue;
        }

        for (int i = 0; i < SLOTS; i++)
            if (memcmp(g_cur + i * 4, g_prev + i * 4, 4) != 0) g_changes[i]++;
        memcpy(g_prev, g_cur, WINDOW);
        ticks++;

        if (hit) {
            recording = false;
            report(cls.c_str(), ticks);
        }

        if (!fx_pace(HZ)) break;
    }

    // Stop mid-window should not throw the window away -- it is the ordinary
    // way to end a recording you have already seen enough of.
    if (recording) {
        fx_logf("  stopped mid-window -- reporting what was collected");
        report(cls.c_str(), ticks);
    }
    fx_logf("changes: done");
}
