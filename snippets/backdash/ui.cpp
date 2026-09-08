#include "bd.h"
#include <cstdio>
#include <string>

static void esc(const std::string& in, std::string& out) {
    for (char c : in) {
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c >= 32)   out += c;
    }
}

// The page is a real file now, not a string. ui.inc is gone: with a settings
// panel the page needs its own stylesheet and script, and none of that can be
// reached from a NavigateToString page.
std::string ui_page_path() {
    return std::string(fx_snippets_dir()) + "\\backdash\\ui.html";
}

std::string ui_state_js(const Ptrs& p, double fps, int dashes, bool busy,
                        const std::string& status) {
    char b[640];
    double age  = last_damage_age();
    double shut = (g.use_hp_gate && age >= 0.0 && age < g.hp_gate_secs)
                ? (g.hp_gate_secs - age) : 0.0;

    std::string s = "U({";
    snprintf(b, sizeof b,
             "\"dashes\":%d,\"fps\":%.0f,\"armed\":%d,\"busy\":%d,"
             "\"hpOn\":%d,\"hpOpen\":%d,\"hpFor\":%.2f,"
             "\"sndOn\":%d,\"sndOpen\":%d,\"sndFor\":%.2f,\"sndWin\":%.2f,"
             "\"cd\":%.2f,\"cdLeft\":%.2f,\"ok\":%d,\"hp\":%.3f,",
             dashes, fps, fx_armed() ? 1 : 0, busy ? 1 : 0,
             g.use_hp_gate ? 1 : 0, hp_gate_open() ? 1 : 0, shut,
             (g.use_sound_gate && !g.gate_sound.empty()) ? 1 : 0,
             sound_gate_open() ? 1 : 0, sound_gate_left(), g.gate_secs,
             g.cooldown, dash_cooldown_left(), p.ok ? 1 : 0,
             p.ok ? (double)hp_ratio(p.humanoid) : 0.0);
    s += b;
    s += "\"cfg\":" + cfg_json() + ",\"status\":\"";
    esc(status, s);
    s += "\"})";
    return s;
}
