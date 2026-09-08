#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "attach.h"
#include "host.h"
#include "placename.h"

// ---- place name lookup via Roblox web API ----------------------------------
//
// The DataModel exposes a PlaceId but not the human-readable game title.
// Roblox stores that on their web API. Two public GETs and it's back:
//
//   1) apis.roblox.com/universes/v1/places/<placeId>/universe  -> universeId
//   2) games.roblox.com/v1/games?universeIds=<uid>              -> name
//
// We do this HOST-side rather than as page fetch(): WebView2's `file://`
// origin gets CORS-blocked at Roblox's endpoints, silently, with no useful
// error surfaced. WinHTTP has no CORS layer to fight.
//
// Each lookup runs on a detached thread so the UI thread is never blocked
// on the ~500 ms round trip. Result comes back through ui_post as
// "PLACE_NAME:<placeId>|<name>" -- the page caches by placeId.

// The GET itself lives in core/features/net.cpp, which fx_http_get and
// fx_avatar_url also use -- one implementation, so a timeout or proxy fix
// lands everywhere instead of in whichever copy got opened. This wrapper is
// kept because its callers (place name, offsets fetch) speak wide strings.
bool core_http_get(const char* host, const char* path, std::string& out);

std::string http_get(const wchar_t* host, const std::wstring& path) {
    std::string h, p;
    for (const wchar_t* c = host; c && *c; c++) h += (char)*c;
    for (wchar_t c : path) p += (char)c;
    std::string body;
    core_http_get(h.c_str(), p.c_str(), body);
    return body;
}

// Extract a JSON numeric value that appears right after a given key. Not a
// general parser -- Roblox's specific response shape lets us cheat.
static uint64_t json_number(const std::string& body, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    size_t p = body.find(needle);
    if (p == std::string::npos) return 0;
    p += needle.size();
    while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) p++;
    return strtoull(body.c_str() + p, nullptr, 10);
}

// Same shape for a string value. Handles the two common escapes; anything
// else stays as-is (rare in a game title).
static std::string json_string(const std::string& body, const std::string& key) {
    std::string needle = "\"" + key + "\":\"";
    size_t p = body.find(needle);
    if (p == std::string::npos) return "";
    p += needle.size();
    std::string out;
    for (; p < body.size() && body[p] != '"'; p++) {
        if (body[p] == '\\' && p + 1 < body.size()) {
            char c = body[p + 1];
            if      (c == '"' || c == '\\' || c == '/') out += c;
            else if (c == 'n' || c == 'r' || c == 't') out += ' ';
            else                                        out += c;
            p++;
        } else out += body[p];
    }
    return out;
}

static void send_place_name(uint64_t place_id, const std::string& name) {
    // "PLACE_NAME:<pid>|<name>" -- '|' separator is fine here; game titles
    // contain many things but a pipe is rare enough, and the page splits on
    // the first one.
    std::ostringstream m;
    m << "PLACE_NAME:" << place_id << "|" << name;
    ui_post(m.str());
    // Kept here too: the page caches these for the picker, and the panel needs
    // the same answer without asking the network again.
    if (!name.empty()) place_title_remember(place_id, name);
}

// Kick off the two-step lookup on a background thread. Duplicate requests
// for the same place_id are cheap for us; the page already caches, so we
// won't be asked twice in practice.
void lookup_place_name_async(uint64_t place_id) {
    std::thread([place_id]() {
        std::wstring path = L"/universes/v1/places/" + std::to_wstring(place_id) + L"/universe";
        std::string r1 = http_get(L"apis.roblox.com", path);
        uint64_t uid = json_number(r1, "universeId");
        if (!uid) { send_place_name(place_id, ""); return; }

        std::wstring gpath = L"/v1/games?universeIds=" + std::to_wstring(uid);
        std::string r2 = http_get(L"games.roblox.com", gpath);
        std::string name = json_string(r2, "name");
        send_place_name(place_id, name);
    }).detach();
}
