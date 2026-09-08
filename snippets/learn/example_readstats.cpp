// example_readstats.cpp — read-only example. Prints your HP and walk speed.
//
// Safe to run disarmed: there is no write here at all. It is the shortest
// path from "attached" to "a number I can call obviously-right or obviously-
// wrong", which is what you want first when a conf is new.
//
// Bare body: the host wraps this in fx_run(), so a plain `return` ends it.

uint64_t chr = fx_character();
if (!chr) { fx_log("No character. Join a game first."); return; }

// fx_part is fx_find memoised — the right call for a lookup you repeat.
// Here it runs once, so plain fx_find would do just as well.
uint64_t hum = fx_part(chr, "Humanoid");
if (!hum) { fx_log("No Humanoid yet. Try again in a second."); return; }

// Note the key is Humanoid.Walkspeed, lowercase s. The conf spells it that
// way; "WalkSpeed" is absent and fx_off returns -1 for it.
float hp    = fx_rf(hum + fx_off("Humanoid.Health"));
float maxhp = fx_rf(hum + fx_off("Humanoid.MaxHealth"));
float speed = fx_rf(hum + fx_off("Humanoid.Walkspeed"));

fx_logf("HP %.0f / %.0f   speed %.0f", hp, maxhp, speed);
