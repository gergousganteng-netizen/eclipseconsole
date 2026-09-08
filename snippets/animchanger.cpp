// Animation changer -- swap your character's walk/run/jump/idle animations.
//
// Ported from a driver-based version. Same idea, same asset IDs, same safety
// checks; the reads and writes go through fx_* instead of a kernel driver, so
// it honours the arm gate. DISARMED it logs the writes and changes nothing.
//
// Press K to cycle presets. Set PRESET below to pick the starting one.
//
// HOW IT WORKS. Your character holds a script called `Animate`, and under it a
// folder per state (idle, run, walk, jump, fall, climb, swim). Each folder
// holds Animation instances whose AnimationId is a STRING pointing at an asset.
// Overwrite that string and the game plays a different animation.
//
// THE STRING IS AN MSVC std::string, which is why write_anim_id is not a plain
// block write:
//
//     +0x00  16 bytes: the text ITSELF when it fits in 15 chars, otherwise a
//            pointer to a heap buffer
//     +0x10  length          (Misc.StringLength in the conf)
//     +0x18  capacity        (layout, not a conf value -- see below)
//
// Every asset URL here is ~47 chars, so both the old and the new string are the
// long kind, and the write goes INTO the existing heap buffer. That is only
// safe while the new text fits the old capacity, so the capacity is checked and
// the write is refused rather than truncated. Growing the string would mean
// allocating in the game's heap, which this does not do.
//
// 0x18 is the MSVC layout, not something the conf publishes. If a Roblox build
// ever ships with a different string implementation this is the line that
// breaks, and it will break by refusing writes, not by corrupting them.
//
// WHY THE PARENT CHECKS. Death and respawn free the old character, and every
// address below is a heap buffer belonging to one of its children. The original
// carried these checks and they are kept: resolve fresh each pass, confirm
// `Animate` still hangs off the character, and confirm each Animation still has
// a parent before writing into it. A freed instance reads as plausible garbage.

#include <chrono>
#include <cstring>
#include <cstdio>

const int PRESET   = 0;      // starting preset, see NAMES below
const int CYCLE_KEY = 'K';   // press to move to the next preset
const int PERIOD_MS = 100;   // how often to re-assert (the game can rewrite)

// The eight slots, in the order the presets list them.
struct Slot { const char* group; const char* child; };
static const Slot SLOTS[9] = {
    { "idle",  "Animation1" }, { "idle",  "Animation2" },
    { "run",   "RunAnim"    }, { "walk",  "WalkAnim"   },
    { "jump",  "JumpAnim"   }, { "fall",  "FallAnim"   },
    { "climb", "ClimbAnim"  }, { "swim",  "Swim"       },
    // 9th, R15 only, and currently used by NO preset -- every row leaves it
    // "" and an empty id means "leave this slot alone". Kept as the extension
    // point: a preset with two swim ids fills it in, and the ported ten keep
    // their original behaviour because the driver version never touched
    // swimidle either.
    { "swimidle", "SwimIdle" },
};
static const int N_SLOT = 9;

static const char* NAMES[] = {
    "zombie", "ninja", "robot", "zombie classic", "levitation", "stylish",
    "cartoony", "super hero", "elder", "toy", "old school",
};

// idle1, idle2, run, walk, jump, fall, climb, swim -- same order as SLOTS.
static const char* PRESETS[][9] = {
    { "10921344533","10921345304","616163682","616168032","616161997","616157476","616156119","616165109","" }, // zombie
    { "656117400","656118341","656118852","656121766","656117878","656115606","656114359","656119721","" },     // ninja
    { "616136790","616138447","616140816","616146177","616139451","616134815","616133594","616143378","" },     // robot
    { "616158929","616160636","616163682","616168032","616161997","616157476","616156119","616165109","" },     // zombie classic
    { "616006778","616008087","616010382","616013216","616008936","616005863","616003713","616011509","" },     // levitation
    { "616136790","616138447","616140816","616146177","616139451","616134815","616133594","616143378","" },     // stylish
    { "742637544","742638445","742638842","742640026","742637942","742637151","742636889","742639220","" },     // cartoony
    { "616111295","616113536","616117076","616122287","616115533","616118211","616120861","616119360","" },     // super hero
    { "845397899","845400520","845386501","845403856","845398858","845396048","845392038","845401742","" },     // elder
    { "782841498","782845736","782842708","782843345","782847020","782846423","782843869","782845186","" },     // toy
    { "5319828216","5319831086","5319844329","5319847204","5319841935","5319839762","5319816685","5319850266","" }, // old school
};
static const int N_PRESET = (int)(sizeof PRESETS / sizeof PRESETS[0]);

static uint64_t g_oLen = 0, g_oAnimId = 0, g_oParent = 0;

// Write an MSVC std::string in place. Returns false rather than writing a
// partial or oversized value -- a refused animation is a cosmetic miss, a
// corrupted string header is a crash in the game's own code.
static bool write_anim_id(uint64_t str_addr, const char* text, const char** why) {
    *why = nullptr;
    size_t len = strlen(text);
    if (len == 0 || len > 256) { *why = "absurd length"; return false; }

    uint32_t cur_len = fx_rd32(str_addr + g_oLen);
    uint64_t cur_ptr = fx_rd(str_addr);
    bool cur_long = (cur_len > 15);
    bool new_long = (len > 15);

    if (cur_long && cur_ptr > 0x10000) {
        if (new_long) {
            // +0x18 is the capacity. 0 means "unknown", and the original
            // treated that as permission -- kept, because a real allocation
            // always reports one and the length write below is bounded anyway.
            uint32_t cap = fx_rd32(str_addr + 0x18);
            if (cap > 0 && (uint32_t)(len + 1) > cap) {
                *why = "new id longer than the buffer it must fit";
                return false;
            }
            fx_wrblk(cur_ptr, text, len + 1);
        } else {
            // Long -> short: the text moves back INLINE, over the pointer.
            char inl[16] = {0};
            memcpy(inl, text, len + 1);
            fx_wrblk(str_addr, inl, 16);
        }
        uint32_t n = (uint32_t)len;
        fx_wrblk(str_addr + g_oLen, &n, 4);
        return true;
    }

    // Currently short. Only another short string fits; growing it would need an
    // allocation in the game's heap, which is not something this does.
    if (new_long) { *why = "current id is short, cannot grow it in place"; return false; }
    char inl[16] = {0};
    memcpy(inl, text, len + 1);
    fx_wrblk(str_addr, inl, 16);
    uint32_t n = (uint32_t)len;
    fx_wrblk(str_addr + g_oLen, &n, 4);
    return true;
}

static uint64_t now_ms() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(
               steady_clock::now().time_since_epoch()).count();
}

static bool parented(uint64_t inst) {
    if (!inst) return false;
    uint64_t p = fx_rd(inst + g_oParent);
    return p >= 0x10000 && p <= 0x7ff000000000ULL;
}

extern "C" void fx_run() {
    g_oLen    = fx_off("Misc.StringLength");
    g_oAnimId = fx_off("Misc.AnimationId");
    g_oParent = fx_off("Instance.Parent");
    if (g_oLen == (uint64_t)-1 || g_oAnimId == (uint64_t)-1 || g_oParent == (uint64_t)-1) {
        fx_log("x missing an offset (Misc.StringLength / Misc.AnimationId / Instance.Parent)");
        return;
    }

    int preset = PRESET;
    if (preset < 0 || preset >= N_PRESET) preset = 0;
    fx_logf("anim: %s  (press %c to cycle)%s", NAMES[preset], CYCLE_KEY,
            fx_armed() ? "" : "   [disarmed: dry-run only]");

    int last_ok = -1;

    // THE RESPAWN BURST, and why it exists.
    //
    // Other players only see the change after you reset. The reason is not
    // really a server cache: the Animate script reads each AnimationId ONCE and
    // loads it into an AnimationTrack, and that loaded track is what
    // replicates. Writing memory directly bypasses the property setter, so
    // Roblox's Changed signal never fires and the script never learns to
    // reload. A respawn is the only thing that makes it read again.
    //
    // So the one lever we have is TIMING: get our ids in before the script
    // reads them. On the generation edge -- a new character -- re-apply flat
    // out for a short window instead of waiting out PERIOD_MS, because the
    // script is loading its tracks in exactly that window and a 100ms nap is
    // enough to lose the race.
    //
    // We cannot force a reload. That means calling into Lua, and this console
    // does not do that by design.
    const int   BURST_MS = 1500;   // how long to hammer after a respawn
    const int   BURST_GAP = 4;     // ms between passes during the burst
    unsigned    gen       = fx_gen();
    uint64_t    burst_end = now_ms() + BURST_MS;   // treat the start as a spawn

    while (fx_running()) {
        if (fx_key_pressed(CYCLE_KEY) && fx_input_ok()) {
            preset = (preset + 1) % N_PRESET;
            last_ok = -1;                       // re-report on the new one
            fx_logf("anim: %s", NAMES[preset]);
            // ACTION FIRST, and a WARN rather than an error: nothing failed,
            // there is just something you have to do. Said on the CHANGE only
            // -- repeating it every pass would be the noise this style exists
            // to avoid.
            fx_log("! reset for others to see it - they load your animation at spawn");
            // A preset change cannot reach other players until you reset, but
            // burst anyway: if you reset right after, the ids are already in.
            burst_end = now_ms() + BURST_MS;
        }

        // NEW CHARACTER. fx_gen moves when the cached character/parts are
        // invalidated, which is the respawn we are racing.
        unsigned g = fx_gen();
        if (g != gen) {
            gen = g;
            last_ok = -1;
            burst_end = now_ms() + BURST_MS;
            fx_log("  respawn - re-applying now");
        }

        // RESOLVED FRESH every pass, never cached across the loop: a respawn
        // frees the old character and every address below belongs to one of
        // its children.
        uint64_t chr = fx_character();
        uint64_t animate = chr ? fx_find(chr, "Animate") : 0;
        // The model may have been rebuilt between those two calls.
        if (!animate || fx_rd(animate + g_oParent) != chr) {
            if (!fx_sleep(PERIOD_MS)) break;
            continue;
        }

        int ok = 0;
        const char* miss = nullptr;   // one example is enough to explain a gap
        const char* why  = nullptr;
        int want = 0;                 // slots this preset actually asks for
        for (int i = 0; i < N_SLOT; i++) {
            const char* id = PRESETS[preset][i];
            if (!id || !*id) continue;            // preset leaves this one alone
            want++;

            uint64_t group = fx_find(animate, SLOTS[i].group);
            if (!group) { miss = SLOTS[i].group; why = "folder not on this rig"; continue; }
            uint64_t anim = fx_find(group, SLOTS[i].child);
            if (!anim)           { miss = SLOTS[i].child; why = "not on this rig"; continue; }
            if (!parented(anim)) { miss = SLOTS[i].child; why = "instance is gone"; continue; }

            // TWO ID FORMS, and the choice is forced by arithmetic, not taste.
            // The buffer we must fit is the one the game already allocated --
            // 47 bytes on this rig. The URL form is 32 bytes of prefix plus the
            // id, so a 15-digit id needs 48 with its terminator and CANNOT fit.
            // rbxassetid:// is 13 bytes of prefix, so it always does. The
            // engine accepts either.
            //
            // The URL is tried first only so the ten ported presets keep the
            // exact bytes the driver version wrote; anything that does not fit
            // falls back rather than being refused.
            char url[96], alt[96];
            snprintf(url, sizeof url, "http://www.roblox.com/asset/?id=%s", id);
            snprintf(alt, sizeof alt, "rbxassetid://%s", id);
            const char* w = nullptr;
            if (write_anim_id(anim + g_oAnimId, url, &w)) { ok++; continue; }
            if (write_anim_id(anim + g_oAnimId, alt, &w)) { ok++; continue; }
            miss = SLOTS[i].child; why = w ? w : "refused";
        }

        // Reported on CHANGE only. This loop re-asserts several times a second
        // and a line per pass would bury everything else in the console.
        if (ok != last_ok) {
            last_ok = ok;
            if (ok == 0)
                fx_logf("! no animation slots written - %s: %s",
                        miss ? miss : "?", why ? why : "unknown");
            else if (ok < want)
                fx_logf("  %s: %d/%d slots set  (%s: %s)", NAMES[preset], ok, want,
                        miss ? miss : "?", why ? why : "unknown");
            else
                fx_logf("  %s: %d/%d slots set", NAMES[preset], ok, want);
        }

        // Fast while the burst is live, lazy the rest of the time. Idle cost
        // matters: this runs for the whole session and only the moments right
        // after a spawn are a race.
        if (!fx_sleep(now_ms() < burst_end ? BURST_GAP : PERIOD_MS)) break;
    }
}
