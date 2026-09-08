// resolve the offset once, up front
uint64_t oWS = fx_off("Humanoid.Walkspeed");
// (worth a guard: what does fx_off return if the key is misspelled?)

// 1. your character -- one call, and the gate you described
uint64_t chr = fx_character();
if (!chr) { fx_log("no character"); return; }

// 2. the search: children are a walk, not an offset
uint64_t hum = 0;
uint64_t k = fx_nchildren(chr);
for (uint64_t i = 0; i < k; i++) {
    uint64_t c = fx_child(chr, (int)i);
    if (c && strcmp(fx_class(c), "Humanoid") == 0) { hum = c; break; }
}
if (!hum) { fx_log("no humanoid"); return; }

// 3. read and print
fx_logf("walkspeed: %.2f", fx_rf(hum + oWS));