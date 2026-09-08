#include "bd.h"
#include <chrono>
#include <cstring>
#include <string>

static double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Roblox std::string: [ptr][size][capacity], with the bytes stored inline when
// capacity is under 16 rather than behind the pointer.
static bool read_rbx_string(uint64_t addr, std::string* out) {
    unsigned char raw[0x20];
    if (fx_rdblk(addr, raw, sizeof raw) != sizeof raw) return false;
    uint64_t sz, cap;
    std::memcpy(&sz,  raw + 0x10, 8);
    std::memcpy(&cap, raw + 0x18, 8);
    if (sz > 512 || cap > 0xFFFFFFull) return false;
    if (sz == 0) { out->clear(); return true; }

    std::string tmp;
    tmp.resize((size_t)sz);
    if (cap >= 16) {
        uint64_t ptr;
        std::memcpy(&ptr, raw, 8);
        if (!ptr || fx_rdblk(ptr, &tmp[0], (size_t)sz) != (size_t)sz) return false;
    } else {
        std::memcpy(&tmp[0], raw, (size_t)sz);
    }
    out->swap(tmp);
    return true;
}

// ---- HP gate ----------------------------------------------------------------
// Open normally; a drop in health shuts it for g.hp_gate_secs. Every further hit
// pushes that out again, so it stays shut for as long as you are being hit
// rather than for a fixed window from the first blow.
static float  g_last_hp   = -1.0f;
static double g_damage_ts = -1.0;

// ---- sound gate -------------------------------------------------------------
// A WINDOW opened by detection, not a mirror of whether the sound is playing.
//
// g.gate_secs starts the moment a matching Sound appears and the gate shuts when
// it runs out -- even if the sound is still going. A sound that plays for four
// seconds does not buy four seconds of dashing.
//
// Re-opening needs ANOTHER sound, and "another" is tracked by instance address
// rather than by presence: Roblox makes a fresh Sound per play, so a new
// address is a new play. The address is forgotten as soon as the instance goes,
// so a reused address still counts as new next time. Without that, the same
// long-running instance could re-trigger its own window forever.
static uint64_t g_gate_inst = 0;      // the instance that opened the window
static double   g_gate_ts   = -1.0;   // when it opened

static bool sound_matches(uint64_t inst, uint64_t oId) {
    if (std::strcmp(fx_class(inst), "Sound") != 0) return false;
    std::string id;
    if (!read_rbx_string(inst + oId, &id)) return false;
    return id.find(g.gate_sound) != std::string::npos;
}

// The matching instance, or 0.
static uint64_t find_gate_sound(uint64_t root) {
    if (!root || !g.gate_sound[0]) return 0;
    uint64_t oId = fx_off("Sound.SoundId");
    if (oId == (uint64_t)-1) return 0;

    // Two levels. A full walk every tick costs more than it finds -- game
    // sounds are parented to the character or to a part on it.
    uint64_t n = fx_nchildren(root);
    for (uint64_t i = 0; i < n; i++) {
        uint64_t c = fx_child(root, (int)i);
        if (!c) continue;
        if (sound_matches(c, oId)) return c;
        uint64_t m = fx_nchildren(c);
        for (uint64_t j = 0; j < m; j++) {
            uint64_t g = fx_child(c, (int)j);
            if (g && sound_matches(g, oId)) return g;
        }
    }
    return 0;
}

void gates_tick(const Ptrs& p) {
    if (!p.ok) return;

    uint64_t oH = fx_off("Humanoid.Health");
    if (oH != (uint64_t)-1) {
        float hp = 0;
        if (fx_rdblk(p.humanoid + oH, &hp, 4) == 4 && hp == hp) {
            if (g_last_hp >= 0.0f && hp < g_last_hp - 0.01f) g_damage_ts = now_s();
            g_last_hp = hp;
        }
    }

    if (g.use_sound_gate && g.gate_sound[0]) {
        uint64_t snd = find_gate_sound(p.character);
        if (!snd) {
            g_gate_inst = 0;                  // gone; the next one counts as new
        } else if (snd != g_gate_inst) {
            g_gate_inst = snd;                // a different play: open a window
            g_gate_ts   = now_s();
        }
        // Same instance as last time: nothing happens. The window it opened
        // expires on its own.
    }
}

// With no sound configured the gate cannot be judged, so it stays open rather
// than blocking everything.
bool sound_gate_open() {
    if (!g.use_sound_gate || !g.gate_sound[0]) return true;
    if (g_gate_ts < 0.0) return false;        // nothing has fired yet
    return now_s() - g_gate_ts < g.gate_secs;
}

// Seconds left in the window, 0 when shut. Worth showing: with g.cooldown at 0
// this is the only thing limiting how often a dash can happen.
double sound_gate_left() {
    if (!g.use_sound_gate || !g.gate_sound[0] || g_gate_ts < 0.0) return 0.0;
    double left = g.gate_secs - (now_s() - g_gate_ts);
    return left > 0.0 ? left : 0.0;
}

bool hp_gate_open() {
    if (!g.use_hp_gate) return true;
    if (g_damage_ts < 0.0) return true;
    return now_s() - g_damage_ts >= g.hp_gate_secs;
}

double last_damage_age() {
    return g_damage_ts < 0.0 ? -1.0 : now_s() - g_damage_ts;
}
