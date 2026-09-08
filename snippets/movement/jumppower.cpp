// Change the local character's jump.
//
//   Disarmed -> logs the writes it would make, changes nothing.
//   ARMED    -> actually writes them.
//
// There are TWO jump fields and the engine uses exactly one of them:
//   UseJumpPower = true   -> JumpPower governs the jump   (default 50)
//   UseJumpPower = false  -> JumpHeight governs it        (default 7.2)
// Writing the wrong one fails silently: the value changes and the character
// jumps exactly as before.
//
// THIS USED TO READ THE FLAG AND PICK A BRANCH. It does not any more, and the
// reason is measured rather than suspected:
//
//   Humanoid.DisplayName is a std::string at +0xb8, so its inline buffer covers
//   +0xb8..+0xc7. The conf puts UseJumpPower (and PlatformStand) at +0xc5 --
//   INSIDE that buffer. Read across a live server: a 15-character display name
//   puts a literal name character there ('t', for kuchigng1point0), shorter
//   names leave 0x02 in the untouched tail, and names past 15 push the string
//   onto the heap and leave union junk (0x8b). All non-zero, so the old
//   `fx_rd8(hum + oUse) != 0` answered "true" every single time -- for reasons
//   that have nothing to do with jumping. It happened to be RIGHT on stock R6,
//   which is exactly why it went unnoticed.
//
// So the flag is not read at all. BOTH fields are written: the engine applies
// whichever is live and ignores the other, which is correct without ever
// knowing which one that is, and cannot rot when the next conf moves the flag
// somewhere else. movement/boost.cpp does the same thing for the same reason.
//
// The cost of writing both is one extra 4-byte write into a field the engine
// is ignoring. That is the whole downside, and it buys not having to trust an
// offset that is provably pointing at a username.

const float TARGET_POWER  = 200.0f;   // used if UseJumpPower is true
const float TARGET_HEIGHT = 30.0f;    // used if it is false

char b[224];

// Players -> LocalPlayer -> Character -> Humanoid
uint64_t players = fx_service("Players");
uint64_t lp  = players ? fx_rd(players + fx_off("Player.LocalPlayer")) : 0;
uint64_t chr = lp ? fx_rd(lp + fx_off("Player.ModelInstance")) : 0;
if (!chr) { fx_log("no character - are you spawned in?"); return; }

uint64_t hum = 0;
uint64_t n = fx_nchildren(chr);
for (uint64_t i = 0; i < n; i++) {
    uint64_t c = fx_child(chr, (int)i);
    if (c && strcmp(fx_class(c), "Humanoid") == 0) { hum = c; break; }
}
if (!hum) { fx_log("no Humanoid under the character"); return; }

uint64_t oPow = fx_off("Humanoid.JumpPower");
uint64_t oHgt = fx_off("Humanoid.JumpHeight");
if (oPow == (uint64_t)-1 && oHgt == (uint64_t)-1) {
    fx_log("x conf has neither Humanoid.JumpPower nor Humanoid.JumpHeight");
    return;
}

float pow = oPow != (uint64_t)-1 ? fx_rf(hum + oPow) : -1.0f;
float hgt = oHgt != (uint64_t)-1 ? fx_rf(hum + oHgt) : -1.0f;

snprintf(b, sizeof b, "Humanoid 0x%llx  JumpPower=%.2f  JumpHeight=%.2f",
         (unsigned long long)hum, pow, hgt);
fx_log(b);

if (oPow != (uint64_t)-1) fx_wf(hum + oPow, TARGET_POWER);
if (oHgt != (uint64_t)-1) fx_wf(hum + oHgt, TARGET_HEIGHT);

// Report both read-backs. Whichever one the game is actually using is the one
// you will feel, and this cannot tell you which -- so it shows both rather
// than claiming an answer it does not have.
snprintf(b, sizeof b,
         "wrote both - JumpPower %.2f -> %.2f, JumpHeight %.2f -> %.2f "
         "(the engine uses whichever is live)",
         pow, oPow != (uint64_t)-1 ? fx_rf(hum + oPow) : -1.0f,
         hgt, oHgt != (uint64_t)-1 ? fx_rf(hum + oHgt) : -1.0f);
fx_log(b);
