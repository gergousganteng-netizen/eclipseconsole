// Stage L -- deriving Workspace.World and World.Gravity.
//
// The conf is WRONG about both on this build, and it cost most of a session to
// discover that from the symptoms rather than the numbers:
//
//   Workspace.World  reads 0x0, or a non-null pointer to an object whose
//                    World.Gravity is 0.0000
//   World.Gravity    +0x1b4 -- no object reachable from Workspace holds the
//                    real gravity there
//
// Unlike the attribute hunt (stage_j/k), this one has GROUND TRUTH:
// Workspace.ReadOnlyGravity reads the true value. Writing it does nothing --
// it is a mirror -- but reading it tells us exactly what to search for, which
// is the anchor stage_j never had. That is the whole difference between a
// search that can be graded and one that cannot.
//
// The derivation, and it is one sweep:
//
//   mirror = Workspace.ReadOnlyGravity          (the value)
//     <- an object holding that float           (the world)
//     <- a Workspace slot pointing at it        => Workspace.World
//     and the float's offset within it          => World.Gravity
//
// Nothing is assumed about either offset; both fall out as whichever survived.

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

static void L(const char* fmt, ...) {
    char b[512];
    va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    fx_log(b);
}

static bool heapish(uint64_t p) {
    return p >= 0x10000ull && p < 0x7ff000000000ull;
}

extern "C" void fx_run() {
    if (!fx_dm()) { L("no DataModel -- attach and join a place first"); return; }

    uint64_t ws = fx_service("Workspace");
    if (!ws) { L("no Workspace"); return; }

    uint64_t oRO = fx_off("Workspace.ReadOnlyGravity");
    if (oRO == (uint64_t)-1) { L("conf has no Workspace.ReadOnlyGravity"); return; }
    float mirror = fx_rf(ws + oRO);
    L("Workspace 0x%llx", (unsigned long long)ws);
    L("ground truth: Workspace.ReadOnlyGravity (+0x%llx) = %.4f",
      (unsigned long long)oRO, mirror);
    if (!(mirror > 1.0f)) { L("x that is not a gravity -- cannot grade anything"); return; }

    // ---- CONTROL 1: the anchor must be selective -------------------------
    // Sweep for a value that is NOT gravity. If that finds objects too, then
    // "an object containing the number" means nothing.
    int bogus_objs = 0;
    {
        unsigned char wb[0x1000];
        size_t wg = fx_rdblk(ws, wb, sizeof wb);
        for (size_t o = 0; o + 8 <= wg; o += 8) {
            uint64_t p; memcpy(&p, wb + o, 8);
            if (!heapish(p)) continue;
            unsigned char sub[0x800];
            size_t sg = fx_rdblk(p, sub, sizeof sub);
            for (size_t k = 0; k + 4 <= sg; k += 4) {
                float f; memcpy(&f, sub + k, 4);
                if (fabsf(f - 4321.125f) < 0.001f) { bogus_objs++; break; }
            }
        }
    }
    L("CONTROL  sweep for a value that is not there: %d object(s) %s",
      bogus_objs, bogus_objs == 0 ? "-> the anchor is selective"
                                  : "-> matches anything, distrust everything below");

    // ---- the sweep --------------------------------------------------------
    struct Obj { uint64_t addr; std::vector<uint64_t> at; std::vector<uint64_t> slots; };
    std::vector<Obj> objs;

    unsigned char wb[0x1000];
    size_t wg = fx_rdblk(ws, wb, sizeof wb);
    int slots_scanned = 0;
    for (size_t o = 0; o + 8 <= wg; o += 8) {
        uint64_t p; memcpy(&p, wb + o, 8);
        if (!heapish(p)) continue;
        slots_scanned++;

        unsigned char sub[0x800];
        size_t sg = fx_rdblk(p, sub, sizeof sub);
        std::vector<uint64_t> at;
        for (size_t k = 0; k + 4 <= sg; k += 4) {
            float f; memcpy(&f, sub + k, 4);
            if (fabsf(f - mirror) < 0.001f) at.push_back(k);
        }
        if (at.empty()) continue;

        // Several Workspace slots can alias the same object; record the slot
        // against the object rather than reporting it twice.
        bool merged = false;
        for (Obj& e : objs)
            if (e.addr == p) { e.slots.push_back(o); merged = true; break; }
        if (!merged) objs.push_back(Obj{ p, at, { o } });
    }

    L(" ");
    L("scanned %d pointer slots in Workspace, %d object(s) hold the value:",
      slots_scanned, (int)objs.size());
    for (const Obj& e : objs) {
        std::string slots;
        for (uint64_t s : e.slots) {
            char b[24]; snprintf(b, sizeof b, "+0x%llx ", (unsigned long long)s);
            slots += b;
        }
        std::string offs;
        for (uint64_t a : e.at) {
            char b[24]; snprintf(b, sizeof b, "+0x%llx ", (unsigned long long)a);
            offs += b;
        }
        L("  object 0x%-13llx  reached from %s", (unsigned long long)e.addr, slots.c_str());
        L("      value at %s(%d cop%s)", offs.c_str(), (int)e.at.size(),
          e.at.size() == 1 ? "y" : "ies");
    }

    if (objs.empty()) { L("x nothing holds it -- cannot derive"); return; }

    // ---- the answer, and how it grades against the conf --------------------
    // The world is the object holding gravity. Where several qualify, the one
    // with ADJACENT copies is the right shape: Roblox keeps the value twice,
    // and a lone match in some unrelated object does not.
    const Obj* best = &objs[0];
    for (const Obj& e : objs) {
        bool adjacent = false;
        for (size_t i = 1; i < e.at.size(); i++)
            if (e.at[i] - e.at[i-1] == 4) adjacent = true;
        if (adjacent) { best = &e; break; }
    }

    uint64_t confWorld = fx_off("Workspace.World");
    uint64_t confGrav  = fx_off("World.Gravity");

    L(" ");
    L("DERIVED:");
    L("  Workspace.World = +0x%llx     conf says +0x%llx   %s",
      (unsigned long long)best->slots[0], (unsigned long long)confWorld,
      (confWorld == best->slots[0]) ? "MATCH" : "MISMATCH");
    L("  World.Gravity   = +0x%llx     conf says +0x%llx   %s",
      (unsigned long long)best->at[0], (unsigned long long)confGrav,
      (confGrav == best->at[0]) ? "MATCH" : "MISMATCH");
    if (best->at.size() > 1)
        L("  (a second copy sits at +0x%llx -- BOTH must be written or a zero "
          "does not hold)", (unsigned long long)best->at[1]);

    // ---- CONTROL 2: the derived pair must round-trip ----------------------
    uint64_t w = fx_rd(ws + best->slots[0]);
    float back = heapish(w) ? fx_rf(w + best->at[0]) : -1.0f;
    L(" ");
    L("CONTROL  Workspace+0x%llx -> 0x%llx, +0x%llx reads %.4f  %s",
      (unsigned long long)best->slots[0], (unsigned long long)w,
      (unsigned long long)best->at[0], back,
      fabsf(back - mirror) < 0.001f ? "-> the derived pair resolves" : "-> IT DOES NOT");

    // And the conf's pair, for the record.
    if (confWorld != (uint64_t)-1 && confGrav != (uint64_t)-1) {
        uint64_t cw = fx_rd(ws + confWorld);
        float cg = heapish(cw) ? fx_rf(cw + confGrav) : -999.0f;
        L("         the conf's pair reads %.4f from 0x%llx  %s", cg,
          (unsigned long long)cw,
          fabsf(cg - mirror) < 0.001f ? "-> also fine" : "-> WRONG, as expected");
    }

    L(" ");
    L("RE-RUN THIS AFTER A REJOIN. One session proves the offsets exist; the");
    L("dump README's standard is that they survive a world change, and the");
    L("conf's own values presumably looked right on the build they came from.");
}
