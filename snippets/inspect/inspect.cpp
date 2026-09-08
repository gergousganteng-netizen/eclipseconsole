// inspect.cpp -- what does THIS game actually look like?
//
// Run this first in any game where the aimbot, ESP or lockon finds nothing.
// It answers the questions that otherwise take an afternoon of hand-written
// recon, and prints a rig-table row ready to paste into Eclipse Aimbot's
// game.cpp.
//
// It exists because Bad Business broke every assumption at once: Player's
// ModelInstance pointed at a DECOY model (35 BoolValues named a-z and 0-9),
// there was no Humanoid anywhere, the real rigs lived in Workspace.Characters
// under randomly-named containers, health was a NumberValue holding a DOUBLE,
// and Player.Team read 0 for everyone because the game runs its own teams.
// None of that is visible from "the aimbot doesn't work".
//
// Pure reads. Nothing is written, so the arm gate is irrelevant here.

#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>

// How much to print. A big server would otherwise scroll the useful part away.
const int MAX_RIGS_SHOWN  = 2;
const int MAX_PARTS_SHOWN = 40;

// ---- output ------------------------------------------------------------------
//
// NOT printf. fx_log is the only output that reaches the UI: the GUI binary has
// no console, so a snippet's printf writes to a stdout nobody is reading and the
// log pane stays empty with no error. It LOOKS fine under eclipse_cli.exe, which
// is console subsystem and prints direct to the terminal -- so testing there
// cannot catch this.
//
// fx_log is one message per call, so lg() buffers until it sees a newline. That
// keeps printf's semantics: a format string may span several lines or none.
static char   g_lb[2048];
static size_t g_lbn = 0;

static void lg_flush(void) {
    if (!g_lbn) return;
    g_lb[g_lbn] = 0;
    fx_log(g_lb);
    g_lbn = 0;
}

static void lg(const char* fmt, ...) {
    char b[2048];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    for (const char* p = b; *p; p++) {
        if (*p == '\n') { g_lb[g_lbn] = 0; fx_log(g_lb); g_lbn = 0; }
        else if (g_lbn + 1 < sizeof g_lb) g_lb[g_lbn++] = *p;
    }
}

// ---- helpers -----------------------------------------------------------------

// Only these read a Primitive meaningfully.
//
// This whitelist is not caution, it is a bug that already happened: reading
// BasePart.Primitive on an ObjectValue returns something pointer-shaped, and
// the "position" came back as y = -6251356. Without the class check the
// inspector confidently reports folders full of ObjectValues as rigs.
static bool is_part(const char* cls) {
    static const char* ok[] = { "Part", "MeshPart", "UnionOperation", "TrussPart",
                                "WedgePart", "CornerWedgePart", "SpawnLocation",
                                "Seat", "VehicleSeat", "BasePart" };
    for (const char* k : ok) if (strcmp(cls, k) == 0) return true;
    return false;
}

static bool part_pos(uint64_t c, float3* out) {
    if (!is_part(fx_class(c))) return false;
    uint64_t oPrim = fx_off("BasePart.Primitive");
    uint64_t oPos  = fx_off("Primitive.Position");
    if (oPrim == (uint64_t)-1 || oPos == (uint64_t)-1) return false;
    uint64_t prim = fx_rd(c + oPrim);
    if (prim < 0x10000 || prim > 0x7fffffffffffULL) return false;
    float3 p = fx_rf3(prim + oPos);
    if (!(p.x == p.x)) return false;
    *out = p;
    return true;
}

// Direct children that are positioned parts. Three or more says "rig".
static int part_count(uint64_t inst) {
    int k = 0;
    uint64_t n = fx_nchildren(inst);
    for (uint64_t i = 0; i < n && i < 64; i++) {
        float3 p;
        uint64_t c = fx_child(inst, (int)i);
        if (c && part_pos(c, &p)) k++;
    }
    return k;
}

// ---- readability probes --------------------------------------------------------

// A Value object stores its payload at the generic ValueBase slot, but the TYPE
// there varies -- NumberValue is a double, IntValue is not. Reading a double as
// a float returns 0.0, which is indistinguishable from "dead" and is exactly
// how a health filter once emptied an entire game. So report both widths and
// let the reader see which one is sane.
static void probe_value(uint64_t v, const char* label) {
    if (!v) return;
    uint64_t vo = fx_off("Misc.Value");
    if (vo == (uint64_t)-1) { lg("      %-14s Misc.Value offset absent\n", label); return; }

    float  f = fx_rf(v + vo);
    double d = 0;
    fx_rdblk(v + vo, &d, 8);

    lg("      %-14s float=%-10.3f double=%-12.3f %s\n", label, f, d,
           (d > 0.0 && d < 100000.0) ? "<- double looks right"
         : (f > 0.0f && f < 100000.0f) ? "<- float looks right"
                                       : "<- neither decodes; offset is wrong here");
}

// When the generic slot does not decode, sweep the object for any double that
// looks like a plausible quantity. This is how Bad Business's health was found.
static void scan_for_number(uint64_t v, const char* label) {
    if (!v) return;
    lg("      %-14s sweeping for a plausible number:\n", label);
    int hits = 0;
    for (uint64_t off = 0x40; off <= 0x200 && hits < 6; off += 8) {
        double d = 0;
        fx_rdblk(v + off, &d, 8);
        if (d > 0.5 && d < 100000.0 && d == d) {
            lg("        double @ +0x%-4llx = %.3f\n", (unsigned long long)off, d);
            hits++;
        }
    }
    if (!hits) lg("        nothing plausible found\n");
}

// ---- rig dump --------------------------------------------------------------------

// Sorted by height, because that is what identifies the lock points: the head
// is the top, the upper torso is below it, the rig centre lower still. Names
// alone do not tell you -- "Chest" and "Abdomen" mean nothing until you see
// which is higher.
static void dump_rig(uint64_t m, const char* where) {
    uint64_t n = fx_nchildren(m);
    lg("\n  RIG  %s  ->  '%s' (%s), %llu children\n",
           where, fx_name(m), fx_class(m), (unsigned long long)n);

    struct Row { char name[48]; float y; float3 p; };
    Row rows[64];
    int  k = 0;

    for (uint64_t i = 0; i < n && (int)i < MAX_PARTS_SHOWN && k < 64; i++) {
        uint64_t c = fx_child(m, (int)i);
        if (!c) continue;
        float3 p;
        if (!part_pos(c, &p)) continue;
        snprintf(rows[k].name, sizeof rows[k].name, "%s", fx_name(c));
        rows[k].y = p.y;
        rows[k].p = p;
        k++;
    }
    if (!k) { lg("    (no positioned parts directly here)\n"); return; }

    // Insertion sort, descending. Sixty-odd rows; nothing cleverer is wanted.
    for (int i = 1; i < k; i++) {
        Row t = rows[i];
        int j = i - 1;
        while (j >= 0 && rows[j].y < t.y) { rows[j + 1] = rows[j]; j--; }
        rows[j + 1] = t;
    }

    lg("    parts, highest first:\n");
    for (int i = 0; i < k; i++)
        lg("      %-24s y %8.2f   (%.1f %.1f %.1f)\n",
               rows[i].name, rows[i].y, rows[i].p.x, rows[i].p.y, rows[i].p.z);

    // Suggest a rig-table row. Name matching first, height as the fallback --
    // a guess that is visibly a guess beats silence.
    const char* head = "", *upper = "", *full = "";
    for (int i = 0; i < k; i++) {
        const char* nm = rows[i].name;
        if (!head[0]  && strstr(nm, "Head")) head = nm;
        if (!upper[0] && (strstr(nm, "UpperTorso") || !strcmp(nm, "Chest") ||
                          !strcmp(nm, "Torso")))    upper = nm;
        if (!full[0]  && (strstr(nm, "HumanoidRootPart") || !strcmp(nm, "Hips") ||
                          !strcmp(nm, "Root")))     full = nm;
    }
    if (!head[0]  && k > 0) head  = rows[0].name;              // the top one
    if (!upper[0] && k > 2) upper = rows[k / 4].name;
    if (!full[0]  && k > 2) full  = rows[k / 2].name;

    lg("\n    suggested rig-table row for Eclipse Aimbot game.cpp:\n");
    lg("      { Rig::NewGame, \"NewGame\", \"%s\", \"%s\", \"%s\", \"%s\" },\n",
           upper, head, upper, full);
    lg("      // fields are: id, label, probe, head, upper, full\n");
    lg("      // CHECK the heights above -- head should be the tallest and\n");
    lg("      // full should be near the middle of the body.\n");
}

// ---- run ---------------------------------------------------------------------------

extern "C" void fx_run() {
    lg("=== place %llu | DataModel '%s' | %llu children ===\n\n",
           (unsigned long long)fx_place(),
           fx_name(fx_dm()), (unsigned long long)fx_nchildren(fx_dm()));

    uint64_t players = fx_service("Players");
    uint64_t me      = fx_player();
    uint64_t ws      = fx_service("Workspace");
    lg("Players service: %llu   LocalPlayer: '%s'\n",
           (unsigned long long)fx_nchildren(players), fx_name(me));

    // ---- 1. is the stock route usable? --------------------------------------
    // fx_character() follows Player.ModelInstance. A game can point that
    // anywhere, so the test is not "is it non-null" but "does it hold parts".
    lg("\n--- 1. Player.ModelInstance (the stock route) ---\n");
    uint64_t chr = fx_character();
    if (!chr) {
        lg("  null -- not spawned, or the game does not use it\n");
    } else {
        int pc = part_count(chr);
        lg("  points at '%s' (%s), %llu children, %d positioned parts\n",
               fx_name(chr), fx_class(chr), (unsigned long long)fx_nchildren(chr), pc);
        if (pc < 3) {
            lg("  NOT A RIG -- this is a decoy or a state container.\n");
            lg("  first few children, to show what it really is:\n");
            for (uint64_t i = 0, n = fx_nchildren(chr); i < n && i < 6; i++) {
                uint64_t c = fx_child(chr, (int)i);
                if (c) lg("      %-20s %s\n", fx_name(c), fx_class(c));
            }
        } else {
            dump_rig(chr, "Players -> ModelInstance");
        }
    }

    // ---- 2. where else could rigs be? ---------------------------------------
    lg("\n--- 2. Workspace folders that might hold characters ---\n");
    const char* folders[] = { "Characters", "Entity", "Ragdolls", "Alive",
                              "Players", "Mobs", "NPCs" };
    int shown = 0;
    for (const char* f : folders) {
        uint64_t d = fx_find(ws, f);
        if (!d) continue;
        uint64_t n = fx_nchildren(d);
        lg("  Workspace.%-12s %llu children\n", f, (unsigned long long)n);
        for (uint64_t i = 0; i < n && shown < MAX_RIGS_SHOWN; i++) {
            uint64_t c = fx_child(d, (int)i);
            if (!c) continue;

            // The parts may hang off the container or off a sub-model, and
            // there may be MORE THAN ONE candidate. Bad Business has both a
            // .Body and a .Hitbox that track identically -- taking whichever
            // came first picked Hitbox, whose part names are randomised in some
            // sessions, so anything keyed to them breaks on rejoin.
            //
            // So collect every candidate, prefer a recognisable name, and print
            // the rest. A silent choice between two rigs is the kind of thing
            // that works today and mysteriously stops working tomorrow.
            uint64_t parts = (part_count(c) >= 3) ? c : 0;
            uint64_t alts[8];
            int nalt = 0;
            for (uint64_t j = 0, m = fx_nchildren(c); j < m && nalt < 8; j++) {
                uint64_t g = fx_child(c, (int)j);
                if (g && part_count(g) >= 3) alts[nalt++] = g;
            }
            // Preferred names first, in order.
            static const char* prefer[] = { "Body", "Rig", "Character", "Model" };
            for (const char* want : prefer) {
                for (int a = 0; a < nalt && !parts; a++)
                    if (strcmp(fx_name(alts[a]), want) == 0) parts = alts[a];
                if (parts) break;
            }
            if (!parts && nalt) parts = alts[0];
            if (!parts || part_count(parts) < 3) continue;

            if (nalt > 1) {
                lg("\n  NOTE: %d part-holding sub-models under '%s':", nalt, fx_name(c));
                for (int a = 0; a < nalt; a++)
                    lg(" %s(%d)", fx_name(alts[a]), part_count(alts[a]));
                lg("\n        using '%s'. If the others have randomised names,\n"
                       "        they are hitboxes -- aim at the stable one.\n", fx_name(parts));
            }

            char lbl[128];
            snprintf(lbl, sizeof lbl, "Workspace.%s.%s%s", f, fx_name(c),
                     parts == c ? "" : ".<sub-model>");
            dump_rig(parts, lbl);
            shown++;

            // Anything alongside the parts that might carry health or state.
            lg("\n    siblings of the parts (health / state / links):\n");
            for (uint64_t j = 0, m = fx_nchildren(c); j < m && j < 16; j++) {
                uint64_t g = fx_child(c, (int)j);
                if (!g || g == parts) continue;
                lg("      %-22s %s\n", fx_name(g), fx_class(g));
                const char* gc = fx_class(g);
                if (strstr(gc, "Value")) probe_value(g, fx_name(g));
            }
        }
    }
    if (!shown) lg("  no rig-shaped models found in those folders\n");

    // ---- 3. what is readable per player -------------------------------------
    lg("\n--- 3. per-player reads ---\n");
    uint64_t oTeam = fx_off("Player.Team");
    int teams_seen = 0;
    for (uint64_t i = 0, n = fx_nchildren(players); i < n && i < 8; i++) {
        uint64_t pl = fx_child(players, (int)i);
        if (!pl) continue;
        uint64_t team = (oTeam != (uint64_t)-1) ? fx_rd(pl + oTeam) : 0;
        if (team) teams_seen++;
        lg("  %-22s %s  team=%llx %s\n", fx_name(pl),
               pl == me ? "(me)" : "    ",
               (unsigned long long)team, team ? fx_name(team) : "(none)");
    }
    if (!teams_seen)
        lg("  Player.Team is 0 for everyone -- either no teams, or the game\n"
               "  runs its own. A team check is not possible from Player.Team.\n");

    // ---- 4. health, the hard one ---------------------------------------------
    lg("\n--- 4. health ---\n");
    uint64_t hum = chr ? fx_find(chr, "Humanoid") : 0;
    if (hum) {
        uint64_t oH = fx_off("Humanoid.Health"), oM = fx_off("Humanoid.MaxHealth");
        lg("  stock Humanoid found: %.1f / %.1f\n",
               oH != (uint64_t)-1 ? fx_rf(hum + oH) : -1.0f,
               oM != (uint64_t)-1 ? fx_rf(hum + oM) : -1.0f);
    } else {
        lg("  no Humanoid on the character.\n");
        // Look for a Health value near the rig instead.
        uint64_t chs = fx_find(ws, "Characters");
        uint64_t c0  = chs ? fx_child(chs, 0) : 0;
        uint64_t hv  = c0 ? fx_find(c0, "Health") : 0;
        if (hv) {
            lg("  found a Health object on the container:\n");
            probe_value(hv, "Health");
            scan_for_number(hv, "Health");
        } else {
            lg("  and no Health object alongside a rig either.\n");
        }
    }

    lg("\n=== done. If a rig was found, paste the suggested row into\n");
    lg("    veaim/src/game.cpp kRigs and rebuild. ===\n");
    lg_flush();
}
