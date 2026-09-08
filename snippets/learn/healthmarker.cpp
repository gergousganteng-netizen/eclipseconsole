// Resolved ONCE, above the loop: fx_off looks its key up by string, so calling
// it per player is 15 string searches for an answer that cannot change.
uint64_t oModel = fx_off("Player.ModelInstance"); 
uint64_t oHP    = fx_off("Humanoid.Health"); 
uint64_t oMax   = fx_off("Humanoid.MaxHealth");
uint64_t oWS = fx_off("Humanoid.Walkspeed");

fx_log("health list: press H");

while (fx_running()) {
    bool hit = fx_key_pressed('H');
    if (hit) {
        uint64_t players = fx_service("Players"); 
        if (!players) { fx_log("no Players service"); return; }

        uint64_t n = fx_nchildren(players);
        fx_logf("%llu players", (unsigned long long)n);

        for (uint64_t i = 0; i < n; i++) {
            uint64_t p = fx_child(players, (int)i);
            if (!p) continue;
    
            // Object address + field offset = the field's address; fx_rd reads 8 bytes
            // there. fx_rd and not fx_rf because this field holds a POINTER to the
            // character, not a number -- the wrong one returns nonsense, not an error.
            uint64_t chr = fx_rd(p + oModel);
    
            // 0x0 is normal: dead, or still loading. Printed rather than skipped, so a
            // short list is always explained instead of quietly getting shorter.
            if (!chr) { fx_logf("  %-22s (no character)", fx_name(p)); continue; }
    
            // The Humanoid is found by CLASS, not by name -- a game can rename the
            // instance but cannot change what it is. break leaves the INNER loop only;
            // the outer one carries on to the next player.
            uint64_t hum = 0;
            uint64_t nk = fx_nchildren(chr);
            for (uint64_t j = 0; j < nk; j++) {
                uint64_t c = fx_child(chr, (int)j);
                if (c && strcmp(fx_class(c), "Humanoid") == 0) { hum = c; break; }
            }
            if (!hum) { fx_logf("  %-22s (no Humanoid)", fx_name(p)); continue; }

            // fx_rf, not fx_rd: these are floats.
            fx_logf("  %-22s %3.0f / %-3.0f  ws %.0f",
                fx_name(p), fx_rf(hum + oHP), fx_rf(hum + oMax), fx_rf(hum + oWS));
        }
    }
    if (!fx_wait_key(200)) break;   // wakes on a key, on Stop, or after 200ms
}