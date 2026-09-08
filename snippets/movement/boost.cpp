// Boost toggle -- press X for fast + high jump, press again for stock.
//
//   Disarmed -> logs the writes it would make, changes nothing.
//   ARMED    -> actually writes them.
//
// FOUR fields, one toggle:
//   Humanoid.Walkspeed + Humanoid.WalkspeedCheck   the speed and its twin
//   Humanoid.JumpPower + Humanoid.JumpHeight       the jump, both of them
//
// WHY BOTH SPEED FIELDS. A live character showed Walkspeed and WalkspeedCheck
// both holding exactly 16.0, and writing only the first tends to get reverted --
// the second reads as a validation copy the engine compares against. Treat them
// as one value, never as two settings.
//
// WHY BOTH JUMP FIELDS, WHICH IS THE INTERESTING ONE. Roblox uses ONE of
// JumpPower / JumpHeight and ignores the other, and Humanoid.UseJumpPower is
// the flag that says which. That flag is NOT READABLE with this conf, and this
// is measured rather than suspected:
//
//   Humanoid.DisplayName is a std::string at +0xb8, so its inline buffer covers
//   +0xb8..+0xc7. The conf puts UseJumpPower (and PlatformStand) at +0xc5 --
//   INSIDE that buffer. Read on a live server: a 15-character display name puts
//   a literal name character there ('t', for kuchigng1point0), names shorter
//   than that leave 0x02 in the untouched tail, and names past 15 push the
//   string onto the heap and leave union junk (0x8b). Every one of those is
//   non-zero, so reading it as a bool answers "true" every time, for a reason
//   that has nothing to do with jumping.
//
// So do not read the flag. Write BOTH fields: the engine applies whichever is
// live and ignores the other, which makes the write correct without ever
// needing to know which one that is. This is the cheap answer to an unreadable
// discriminator, and it is why this does not do what movement/jumppower.cpp
// does (that one still branches on the flag, and so always picks JumpPower).
//
// WHY THE HIGH STATE LOOPS AND THE LOW ONE DOES NOT. The stock values are what
// the engine already wants, so nothing is trying to put them back: one write
// and they stay. The boosted ones are not, and anything that reasserts them --
// a respawn, a game's own movement handling, a Humanoid state change -- takes
// them away again. So HIGH re-writes on a 60 Hz beat and LOW writes once and
// goes back to sleep.
//
// fx_pace(60), not fx_sleep(16). The body's own reads and writes add to every
// fx_sleep period, so a sleeping loop runs slower than the rate it names;
// fx_pace waits until the next period boundary, so the work comes out of the
// wait. It returns 0 on Stop exactly like fx_sleep, so it drops straight into
// the tail of the loop.
//
// STALENESS. A respawn frees the Humanoid, so a cached pointer becomes a write
// into freed memory. fx_gen() moves whenever the character does, so one integer
// compare invalidates it. The `|| !hum` half of that test is not optional: a
// character is assembled piece by piece and the Humanoid frequently does not
// exist yet at the instant the generation ticks -- without the retry it
// resolves once, finds nothing, and stays empty forever, because the generation
// never moves again.
//
// Toggling back to stock on Stop is deliberate. Ending the snippet boosted
// would leave you fast with nothing on screen saying why.
//
// X is not claimed, so the game still receives the key -- if X does something
// in your game, it still does it. Set fx_claim_key(TOGGLE_KEY, 1) if you would
// rather it did not.

const int      TOGGLE_KEY = 'Z';        // uppercase: 'x' is a different code

const float    HIGH_WS    = 100.0f;     // boosted walkspeed
const float    LOW_WS     = 16.0f;      // stock

const float    HIGH_JP    = 150.0f;     // boosted JumpPower
const float    LOW_JP     = 50.0f;      // stock

// JumpHeight is on a completely different scale from JumpPower -- stock is 7.2
// studs against 50 -- so it gets its own pair rather than sharing a multiplier.
const float    HIGH_JH    = 21.0f;
const float    LOW_JH     = 7.2f;       // stock

const unsigned HZ         = 1000;         // reassert rate while boosted

uint64_t oWs = fx_off("Humanoid.Walkspeed");
uint64_t oCk = fx_off("Humanoid.WalkspeedCheck");
if (oWs == (uint64_t)-1 || oCk == (uint64_t)-1) {
    fx_log("x conf is missing Humanoid.Walkspeed / Humanoid.WalkspeedCheck");
    return;
}

// The jump fields are optional rather than fatal: a conf without them still
// gives a working speed boost, and half a feature beats refusing to run.
uint64_t oJp = fx_off("Humanoid.JumpPower");
uint64_t oJh = fx_off("Humanoid.JumpHeight");
bool hasJp = oJp != (uint64_t)-1;
bool hasJh = oJh != (uint64_t)-1;
if (!hasJp && !hasJh)
    fx_log("! conf has neither Humanoid.JumpPower nor JumpHeight - speed only");

uint64_t hum = 0;

// The Humanoid under the current character, or 0 if it is not there yet.
// Matched on CLASS, not on the name: a game is free to rename the instance.
auto find_hum = [&]() -> uint64_t {
    uint64_t chr = fx_character();
    if (!chr) return 0;
    uint64_t n = fx_nchildren(chr);
    for (uint64_t i = 0; i < n; i++) {
        uint64_t c = fx_child(chr, (int)i);
        if (c && strcmp(fx_class(c), "Humanoid") == 0) return c;
    }
    return 0;
};

auto apply = [&](float ws, float jp, float jh) {
    if (!hum) return;
    fx_wf(hum + oWs, ws);
    fx_wf(hum + oCk, ws);
    if (hasJp) fx_wf(hum + oJp, jp);
    if (hasJh) fx_wf(hum + oJh, jh);
};

unsigned gen  = fx_gen() - 1;    // force a first resolve
bool     high = false;
bool     told = false;           // has this stretch of HIGH been reported on?

fx_logf("boost toggle running - press %c for %.0f speed / %.0f jump, again for stock",
        TOGGLE_KEY, HIGH_WS, HIGH_JP);

while (fx_running()) {

    // One integer compare. When it moves, the Humanoid we hold belongs to a
    // body that no longer exists. `high` deliberately SURVIVES the swap: the
    // new character comes up at the defaults and the loop below puts the boost
    // back, which is what the toggle being on is supposed to mean.
    if (fx_gen() != gen || !hum) {
        gen = fx_gen();
        hum = find_hum();
        if (hum && high) { told = false; fx_log("character changed - reapplying boost"); }
    }

    // fx_key_pressed FIRST: it consumes one press per call, so gating ahead of
    // it would eat presses made while a chat box was open and then lose the
    // next real one.
    bool hit = fx_key_pressed(TOGGLE_KEY) && fx_input_ok();

    if (hit) {
        high = !high;
        told = false;
        if (!hum) {
            fx_log("no Humanoid yet - are you spawned in?");
        } else if (high) {
            fx_logf("boost ON  - speed %.0f, jump %.0f/%.1f  (was %.2f speed)",
                    HIGH_WS, HIGH_JP, HIGH_JH, fx_rf(hum + oWs));
        } else {
            // Nothing reverts the stock values, so one write is the whole job.
            apply(LOW_WS, LOW_JP, LOW_JH);
            fx_logf("boost off - back to %.0f speed, %.0f/%.1f jump  (reads back %.2f)",
                    LOW_WS, LOW_JP, LOW_JH, fx_rf(hum + oWs));
        }
    }

    if (high && hum) {
        apply(HIGH_WS, HIGH_JP, HIGH_JH);

        // Read one back, once per stretch, so a run that changes nothing says
        // so instead of just streaming dry-run lines. Reported as two possible
        // causes because the read cannot tell them apart: disarmed, or a game
        // that clamps the field back within the same frame.
        if (!told) {
            told = true;
            if (fx_rf(hum + oWs) < HIGH_WS - 0.5f)
                fx_log("speed did not move - either disarmed, or the game clamps it");
        }

        if (!fx_pace(HZ)) break;
    } else {
        if (!fx_wait_key(200)) break;   // idle: woken by the keypress itself
    }
}

// Re-resolve rather than trusting the cached pointer: Stop can land after a
// respawn the loop never got to see.
if (high) {
    hum = find_hum();
    if (hum) {
        apply(LOW_WS, LOW_JP, LOW_JH);
        fx_log("restored stock speed and jump on exit");
    }
}
fx_log("boost toggle stopped");
