#include "bd.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
// WIN32_LEAN_AND_MEAN: windows.h is the most expensive header this project
// includes -- measured 2026-09-07 on the bundled g++, +2222ms over an empty
// TU at -O2, against +520 for <fstream>, +365 for <atomic>, +275 for
// <thread> and nothing at all for <string>/<chrono>/<cstdio> (fx.h already
// pulls those in). LEAN drops the parts nothing here uses -- OLE, RPC,
// winsock, the multimedia and shell APIs -- and measured 319-491ms back per
// file. Delete it only if something here starts needing one of those.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>    // MoveFileExA, for the atomic save below

Cfg g;

// settings.ini beside the project. Not .cpp/.h/.inc, so it stays out of the
// sidebar and out of export bundles -- which is right: these are your dials on
// your machine, not part of the program someone else would receive.
std::string cfg_path() {
    return std::string(fx_snippets_dir()) + "\\backdash\\settings.ini";
}

static void esc_json(const std::string& in, std::string& out) {
    for (char c : in) {
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c >= 32)   out += c;
    }
}

// One key=value per line. Unknown keys are ignored rather than rejected, so a
// file written by an older or newer build still loads what it can.
bool cfg_set(const std::string& k, const std::string& v) {
    double d = atof(v.c_str());
    int    i = atoi(v.c_str());
    bool   b = (v == "1" || v == "true");

    if      (k == "key_fps")        g.key_fps        = i;
    else if (k == "key_dash")       g.key_dash       = i;
    else if (k == "dash_dir")       g.dash_dir       = i;
    else if (k == "power_min")      g.power_min      = d;
    else if (k == "power_max")      g.power_max      = d;
    else if (k == "hold_secs")      g.hold_secs      = d;
    else if (k == "hold_hz")        g.hold_hz        = i;
    else if (k == "do_jump")        g.do_jump        = b;
    else if (k == "jump_delay")     g.jump_delay     = d;
    else if (k == "cooldown")       g.cooldown       = d;
    else if (k == "use_sound_gate") g.use_sound_gate = b;
    else if (k == "use_hp_gate")    g.use_hp_gate    = b;
    else if (k == "gate_sound")     g.gate_sound     = v;
    else if (k == "gate_secs")      g.gate_secs      = d;
    else if (k == "hp_gate_secs")   g.hp_gate_secs   = d;
    else if (k == "fps_low")        g.fps_low        = d;
    else if (k == "fps_high")       g.fps_high       = d;
    else return false;

    // Clamped here rather than in the panel, because the file is editable by
    // hand too and a hold_hz of 0 would divide by zero in fx_pace.
    if (g.hold_hz    < 1)     g.hold_hz    = 1;
    if (g.hold_hz    > 4000)  g.hold_hz    = 4000;
    if (g.hold_secs  < 0.01)  g.hold_secs  = 0.01;
    if (g.hold_secs  > 5.0)   g.hold_secs  = 5.0;
    if (g.cooldown   < 0.0)   g.cooldown   = 0.0;
    if (g.gate_secs  < 0.01)  g.gate_secs  = 0.01;
    if (g.dash_dir   < 0 || g.dash_dir > 5) g.dash_dir = 0;
    return true;
}

void cfg_reset() { g = Cfg(); }

void cfg_load() {
    std::ifstream f(cfg_path());
    if (!f) return;                       // no file yet: defaults stand
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        cfg_set(line.substr(0, eq), line.substr(eq + 1));
    }
}

// Written to a temp file and RENAMED over the real one, never straight into it.
//
// A plain ofstream(trunc) empties settings.ini BEFORE it writes a byte, so
// anything that interrupts the write -- a crash, the console killed for a
// rebuild -- leaves an empty file and every dial gone. The rename is atomic:
// an interrupted save leaves the PREVIOUS settings untouched, which is the
// difference between losing one edit and losing the whole tuning session.
void cfg_save() {
    std::string path = cfg_path();
    std::string tmp  = path + ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) return;
        o << "# backdash settings - edited from the panel, safe to edit by hand\n";
        o << "key_fps="        << g.key_fps        << "\n"
          << "key_dash="       << g.key_dash       << "\n"
          << "dash_dir="       << g.dash_dir       << "\n"
          << "power_min="      << g.power_min      << "\n"
          << "power_max="      << g.power_max      << "\n"
          << "hold_secs="      << g.hold_secs      << "\n"
          << "hold_hz="        << g.hold_hz        << "\n"
          << "do_jump="        << (g.do_jump ? 1 : 0)        << "\n"
          << "jump_delay="     << g.jump_delay     << "\n"
          << "cooldown="       << g.cooldown       << "\n"
          << "use_sound_gate=" << (g.use_sound_gate ? 1 : 0) << "\n"
          << "use_hp_gate="    << (g.use_hp_gate ? 1 : 0)    << "\n"
          << "gate_sound="     << g.gate_sound     << "\n"
          << "gate_secs="      << g.gate_secs      << "\n"
          << "hp_gate_secs="   << g.hp_gate_secs   << "\n"
          << "fps_low="        << g.fps_low        << "\n"
          << "fps_high="       << g.fps_high       << "\n";
    }   // closed HERE: MoveFileEx cannot replace a file we still hold open
    MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::string cfg_json() {
    char b[512];
    std::string s = "{";
    snprintf(b, sizeof b,
             "\"key_fps\":%d,\"key_dash\":%d,\"dash_dir\":%d,"
             "\"power_min\":%.3g,\"power_max\":%.3g,"
             "\"hold_secs\":%.3g,\"hold_hz\":%d,"
             "\"do_jump\":%d,\"jump_delay\":%.3g,\"cooldown\":%.3g,"
             "\"use_sound_gate\":%d,\"use_hp_gate\":%d,"
             "\"gate_secs\":%.3g,\"hp_gate_secs\":%.3g,"
             "\"fps_low\":%.4g,\"fps_high\":%.4g,\"gate_sound\":\"",
             g.key_fps, g.key_dash, g.dash_dir,
             g.power_min, g.power_max, g.hold_secs, g.hold_hz,
             g.do_jump ? 1 : 0, g.jump_delay, g.cooldown,
             g.use_sound_gate ? 1 : 0, g.use_hp_gate ? 1 : 0,
             g.gate_secs, g.hp_gate_secs, g.fps_low, g.fps_high);
    s += b;
    esc_json(g.gate_sound, s);
    s += "\"}";
    return s;
}
