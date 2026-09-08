// Noclip -- press V to walk through walls, press again to solidify.
//
// HOW, and why it is allowed here: Roblox keeps collision on the physics object,
// not on the Instance. BasePart+0x128 points at its Primitive, and Primitive
// +0x1b6 is a flags byte whose 0x8 bit is CanCollide. Noclip is that one bit,
// cleared on every part of your character. It is a DATA write like every other
// write in this tool -- no code injected, no control flow touched -- and the arm
// gate still decides whether it happens at all.
//
//   press V    collision off
//   press V    collision restored to exactly what it was, per part
//   Stop       restores too, so ending mid-noclip cannot leave you ghosted
//
// TOGGLED. This was hold-to-noclip first, and the reason is worth keeping in
// front of you rather than deleting: your character stands on its own
// collision. With it off you are not weightless, you are unsupported -- stand
// still over ground and you sink through the floor into the void, and no
// amount of restoring collision afterwards brings you back up. Held, letting go
// was the recovery. Toggled, there is nothing to let go of.
//
// So: toggle it on to cross something, toggle it off the moment you are through.
// Treat a lit noclip as a thing you are actively doing, not a mode you leave on.
//
// V is not claimed, so the game still receives the key -- if V does something in
// your game, it still does it. Set fx_claim_key(TOGGLE_KEY, 1) if you would
// rather it did not.
//
// The two hard parts are not the bit. They are:
//
//   STALENESS. Respawning frees every Primitive, so a cached flags address
//   becomes a write into freed memory. fx_gen() moves whenever the
//   character does, so one integer compare invalidates everything derived from
//   it. Only the Instance is cached; its Primitive is re-read on every use,
//   which also covers a Primitive rebuilt without a respawn.
//
//   RESTORING. Put back the byte saved at press time, never a blanket "solid".
//   Hitboxes and accessory handles are legitimately non-colliding, and making
//   them solid would be a new bug wearing the costume of a fix.

const int TOGGLE_KEY = 'Z';              // uppercase: 'v' is a different code

char b[224];

uint64_t oPrim  = fx_off("BasePart.Primitive");
uint64_t oFlags = fx_off("Primitive.Flags");
uint64_t mColl  = fx_off("PrimitiveFlags.CanCollide");
if (oPrim == (uint64_t)-1 || oFlags == (uint64_t)-1 || mColl == (uint64_t)-1) {
    fx_log("x conf is missing BasePart.Primitive / Primitive.Flags / PrimitiveFlags.CanCollide");
    return;
}

struct Part { uint64_t inst; uint8_t saved; bool had; };
Part parts[128];
int  nparts = 0;

// Address of the flags byte, resolved fresh every time. 0 means "not usable
// right now", which is the right answer for a part still being built.
auto flags_at = [&](uint64_t inst) -> uint64_t {
    uint64_t prim = fx_rd(inst + oPrim);
    if (prim < 0x10000 || prim > 0x7ff000000000ULL) return 0;
    return prim + oFlags;
};

auto collect = [&]() {
    uint64_t found[128];
    int n = fx_parts(found, 128);        // class filtering lives in the host now
    nparts = 0;
    for (int i = 0; i < n; i++) {
        parts[nparts].inst  = found[i];
        parts[nparts].saved = 0;
        parts[nparts].had   = false;
        nparts++;
    }
    return nparts;
};

auto restore = [&]() {
    for (int i = 0; i < nparts; i++) {
        if (!parts[i].had) continue;
        uint64_t a = flags_at(parts[i].inst);
        if (a) fx_wr8(a, parts[i].saved);
    }
};

unsigned gen     = fx_gen() - 1;    // force a first collect
// `engaged` is the toggle state -- what the key last set it to. `active` is
// whether the write actually LANDED, which stays false while disarmed.
bool     engaged = false, active = false;

fx_log("noclip armed - press V to pass through walls, press again to solidify");

while (fx_running()) {

    // One integer compare. When it moves, every part pointer we hold belongs to
    // a body that no longer exists -- so drop them without restoring, because
    // there is nothing left to restore to.
    if (fx_gen() != gen) {
        gen     = fx_gen();
        engaged = active = false;
        nparts = 0;
        if (fx_character()) {
            snprintf(b, sizeof b, "character changed - %d parts", collect());
            fx_log(b);
        }
    }

    if (!fx_character()) { if (!fx_sleep(200)) break; continue; }

    // fx_key_pressed FIRST: it consumes one press per call, so gating ahead of
    // it would eat presses made while a chat box was open and then lose the
    // next real one. Called once and stored, for the same reason.
    bool hit = fx_key_pressed(TOGGLE_KEY) && fx_input_ok();

    if (hit && !engaged) {
        // Re-collect on the press itself. A character is assembled piece by
        // piece, so a list gathered the instant it appeared is often missing the
        // only parts that collide -- which is exactly how this failed after a
        // respawn until the script was restarted.
        collect();

        int touched = 0;
        for (int i = 0; i < nparts; i++) {
            uint64_t a = flags_at(parts[i].inst);
            parts[i].had = false;
            if (!a) continue;
            uint8_t f = fx_rd8(a);
            parts[i].saved = f;
            if (f & mColl) { parts[i].had = true; fx_wr8(a, (uint8_t)(f & ~mColl)); touched++; }
        }

        // Read one back. Disarmed nothing moved, and without this the maintain
        // loop below would rewrite every 10ms and flood the console.
        active = false;
        for (int i = 0; i < nparts; i++) {
            if (!parts[i].had) continue;
            uint64_t a = flags_at(parts[i].inst);
            if (a) active = (fx_rd8(a) & mColl) == 0;
            break;
        }

        if (touched == 0) {
            snprintf(b, sizeof b, "noclip ON - none of the %d parts were colliding", nparts);
            fx_log(b);
            active = true;
        } else if (active) {
            snprintf(b, sizeof b, "noclip ON  (%d of %d parts)", touched, nparts);
            fx_log(b);
        } else {
            snprintf(b, sizeof b,
                     "noclip would clear CanCollide on %d parts - disarmed, nothing written",
                     touched);
            fx_log(b);
        }
        engaged = true;
    } else if (hit && engaged) {
        restore();
        fx_log(active ? "noclip off" : "noclip off - nothing had been written");
        engaged = active = false;
    }

    if (engaged && active) {
        // The engine puts the bit back, so hold it down every few milliseconds.
        // Only write when it has actually returned -- a blind write every pass
        // would be pointless traffic across a process boundary.
        for (int i = 0; i < nparts; i++) {
            if (!parts[i].had) continue;
            uint64_t a = flags_at(parts[i].inst);
            if (!a) continue;
            uint8_t f = fx_rd8(a);
            if (f & mColl) fx_wr8(a, (uint8_t)(f & ~mColl));
        }
        if (!fx_sleep(10)) break;        // tight, but only while noclip is on
    } else {
        if (!fx_wait_key(200)) break;    // idle: woken by the keypress itself
    }
}

// Never leave someone ghosted because they hit Stop with noclip still on --
// which matters more now than it did: held, releasing the key was the recovery,
// so Stop mid-noclip was a corner case. Toggled, it is the ordinary way to end.
if (engaged) {
    restore();
    fx_log("collision restored on exit");
}
fx_log("noclip stopped");
