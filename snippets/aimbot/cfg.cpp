#include "aim.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// Settings, persisted through fx_cfg.
//
// The DIALS THEMSELVES still live in main.cpp with the comments that explain
// them -- this file only moves values in and out. That is deliberate: the
// reasoning for why SWITCH_DWELL_MS is 25 belongs next to the code that uses
// it, not next to a serialiser.
//
// STORAGE is %APPDATA%\EclipseConsole\settings.conf, shared with every other
// snippet, which is why every key here is prefixed "aimbot.". Writes are
// coalesced by the host (about one file write a second, plus one at run end),
// so saving on every panel change costs nothing.
//
// DEFAULTS ARE WHATEVER main.cpp WAS ALREADY SET TO. The panel starting with
// different numbers than the file had would silently retune the aim the first
// time it opened -- so the fallback passed to fx_cfg_num is the live value,
// captured before anything is loaded.

namespace {

struct Dial {
    const char* key;      // without the "aimbot." prefix
    float*      f;        // exactly one of these is set
    double*     d;
    unsigned*   u;
    bool*       b;
    double      lo, hi;   // clamped on the way in, from file OR panel
    int*        i = nullptr;   // enums; last so the rows above stay readable
};

// Captured on first use, so "reset" restores what the source shipped rather
// than whatever happens to be loaded.
double g_default[16];
bool   g_defaults_taken = false;

Dial* table(int& n) {
    static Dial t[] = {
        { "smoothing",   &SMOOTHING,   nullptr, nullptr, nullptr, 0.01, 1.0 },
        { "maxDist",     &MAX_DIST,    nullptr, nullptr, nullptr, 10.0, 5000.0 },
        { "lead",        &LEAD_SECONDS,nullptr, nullptr, nullptr, 0.0,  0.5 },
        { "switchFrac",  &SWITCH_FRACTION, nullptr, nullptr, nullptr, 0.1, 1.0 },
        { "switchDwell", nullptr, &SWITCH_DWELL_MS, nullptr, nullptr, 0.0, 2000.0 },
        { "aimHz",       nullptr, nullptr, &AIM_HZ, nullptr, 30.0, 1000.0 },
        { "espHz",       nullptr, nullptr, &ESP_HZ, nullptr, 10.0, 240.0 },
        { "teamCheck",   nullptr, nullptr, nullptr, &TEAM_CHECK, 0.0, 1.0 },
        { "priority",    nullptr, nullptr, nullptr, nullptr, 0.0, 1.0, &TARGET_PRIORITY },
        { "pickRadius",  &PICK_RADIUS, nullptr, nullptr, nullptr, 0.0, 2000.0 },
    };
    n = (int)(sizeof t / sizeof t[0]);
    return t;
}

double dial_get(const Dial& d) {
    if (d.f) return *d.f;
    if (d.d) return *d.d;
    if (d.u) return (double)*d.u;
    if (d.i) return (double)*d.i;
    return *d.b ? 1.0 : 0.0;
}

// Clamped here rather than in the panel, because settings.conf is editable by
// hand too and an aimHz of 0 would divide by zero in fx_pace.
void dial_set(const Dial& d, double v) {
    if (v < d.lo) v = d.lo;
    if (v > d.hi) v = d.hi;
    if      (d.f) *d.f = (float)v;
    else if (d.d) *d.d = v;
    else if (d.u) *d.u = (unsigned)(v + 0.5);
    else if (d.i) *d.i = (int)(v + 0.5);
    else          *d.b = v >= 0.5;
}

void take_defaults() {
    if (g_defaults_taken) return;
    int n; Dial* t = table(n);
    for (int i = 0; i < n; i++) g_default[i] = dial_get(t[i]);
    g_defaults_taken = true;
}

std::string full_key(const char* k) { return std::string("aimbot.") + k; }

}  // namespace

void cfg_load() {
    take_defaults();
    int n; Dial* t = table(n);
    for (int i = 0; i < n; i++)
        dial_set(t[i], fx_cfg_num(full_key(t[i].key).c_str(), g_default[i]));
}

void cfg_save() {
    int n; Dial* t = table(n);
    for (int i = 0; i < n; i++)
        fx_cfg_set_num(full_key(t[i].key).c_str(), dial_get(t[i]));
}

// false = key we do not know, so the caller can say so instead of silently
// accepting a typo from a hand-edited file.
bool cfg_set(const std::string& key, double v) {
    take_defaults();
    int n; Dial* t = table(n);
    for (int i = 0; i < n; i++) {
        if (key == t[i].key) {
            dial_set(t[i], v);
            fx_cfg_set_num(full_key(t[i].key).c_str(), dial_get(t[i]));
            return true;
        }
    }
    return false;
}

void cfg_reset() {
    take_defaults();
    int n; Dial* t = table(n);
    for (int i = 0; i < n; i++) {
        dial_set(t[i], g_default[i]);
        fx_cfg_set_num(full_key(t[i].key).c_str(), dial_get(t[i]));
    }
}

// The values as the panel sees them. Clamping happens before this, so what the
// panel shows is always what the aim is actually using -- a field that snapped
// back on the next push would look like the UI ignoring you.
std::string cfg_json() {
    char b[64];
    std::string s = "{";
    int n; Dial* t = table(n);
    for (int i = 0; i < n; i++) {
        if (i) s += ",";
        s += "\"";
        s += t[i].key;
        s += "\":";
        snprintf(b, sizeof b, "%.4g", dial_get(t[i]));
        s += b;
    }
    return s + "}";
}
