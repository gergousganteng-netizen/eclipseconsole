// Change the local character's walk speed.
//
//   Disarmed -> logs the writes it would make, changes nothing.
//   ARMED    -> actually writes them.
//
// TWO fields hold the speed: Walkspeed (0x1d0) and WalkspeedCheck (0x3bc).
// Probing a live character showed both holding exactly 16.0, and a sweep of the
// Humanoid's first 0x400 bytes found no other float matching — so the second is
// a validation copy the engine compares against. Writing only the first tends
// to get reverted, so this writes both and reports each.

const float TARGET = 100.0f;      // default is 16

char b[192];

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

uint64_t oWs = fx_off("Humanoid.Walkspeed");
uint64_t oCk = fx_off("Humanoid.WalkspeedCheck");

snprintf(b, sizeof b, "Humanoid 0x%llx  Walkspeed=%.2f  check=%.2f",
         (unsigned long long)hum, fx_rf(hum + oWs), fx_rf(hum + oCk));
fx_log(b);

fx_wf(hum + oWs, TARGET);
fx_wf(hum + oCk, TARGET);

snprintf(b, sizeof b, "wrote %.2f -> reads back  Walkspeed=%.2f  check=%.2f",
         TARGET, fx_rf(hum + oWs), fx_rf(hum + oCk));
fx_log(b);