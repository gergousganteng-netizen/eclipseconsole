// cache.h -- the LocalPlayer/Character cache, the memoised child lookup, and the
// character/DataModel generation counters. This TU OWNS all of that state AND
// the mutex that guards the part cache; nothing outside sees g_fxparts or its
// lock. Functions only, no extern globals.
//
// The exports it defines (fx_player/fx_character/fx_gen/fx_dm_gen/fx_part) are
// declared in fx.h and reached the usual way; only the host-side write entry
// points are declared here.
#pragma once

// Drop the character cache: called when the DataModel is replaced (leaving,
// joining, teleporting) and on detach.
void char_cache_reset(void);

// The two DELIBERATE generation bumps refresh_dm makes on a DataModel swap. See
// the comment at that call site for why they are explicit rather than implied by
// char_cache_reset.
void char_cache_bump_gen(void);   // the character generation (fx_gen)
void dm_gen_bump(void);           // the DataModel generation (fx_dm_gen)
