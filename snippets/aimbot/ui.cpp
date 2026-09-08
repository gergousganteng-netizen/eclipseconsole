#include "aim.h"
#include <cstdio>
#include <string>

// The settings panel's host side: where the page lives, and the one JS call
// that pushes state into it.
//
// A real file rather than a NavigateToString string, so the page can have its
// own stylesheet and script -- backdash learned that the hard way and this
// follows it.

static void esc(const std::string& in, std::string& out) {
    for (char c : in) {
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c >= 32)   out += c;
    }
}

std::string ui_page_path() {
    return std::string(fx_snippets_dir()) + "\\aimbot\\ui.html";
}

// One call, one object. The panel re-renders from whatever it is handed rather
// than tracking deltas, so a dropped push costs a frame of staleness and
// nothing else.
std::string ui_state_js(const char* target, int nTargets, bool aiming) {
    char b[256];
    std::string s = "U({";
    snprintf(b, sizeof b, "\"armed\":%d,\"aiming\":%d,\"targets\":%d,\"target\":\"",
             fx_armed() ? 1 : 0, aiming ? 1 : 0, nTargets);
    s += b;
    esc(target ? target : "", s);
    s += "\",\"cfg\":";
    s += cfg_json();
    s += "})";
    return s;
}
