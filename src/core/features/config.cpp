// config.cpp -- persisted snippet settings.
//
// A snippet's tuned dials (SMOOTHING, a switch fraction, a dwell time) are
// recompiled constants: changing one means editing source, rebuilding, and
// losing the value you were comparing against. This is the key/value store
// that lets a snippet keep them instead.
//
// WHERE, AND WHY NOT NEXT TO THE SNIPPETS. The file is
// %APPDATA%\EclipseConsole\settings.conf -- a SIBLING of the snippets folder,
// never inside it. snippets\ holds the user's own work and nothing but their
// own actions may write there; a settings file the console rewrites on its own
// schedule has no business in that tree.
//
// FORMAT is deliberately the offsets conf's: `key = value`, one per line, '#'
// comments, and unknown lines preserved on rewrite is NOT attempted -- the file
// is ours to rewrite wholesale. It is plain text on purpose, because the fastest
// way to fix a dial you have tuned into a corner is to open it and type.
//
// ONE FLAT NAMESPACE, shared by every snippet. Prefixing is the caller's job
// ("aimbot.smoothing", not "smoothing"). A per-snippet namespace was considered
// and dropped: a run's identity is its label, projects have several files under
// one label, and the CLI path has no label at all -- so the automatic answer
// would have been wrong in three places, each silently. An explicit key is
// always right and always visible in the file.
//
// WRITES ARE RATE-LIMITED, NOT DEFERRED. A snippet storing a value every tick
// at 60 Hz would otherwise rewrite the file 60 times a second. Sets land in
// memory immediately (so a read-back is always the value just written) and the
// file is rewritten at most once a second, plus once more at run end so the
// last value is never the one that got dropped.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "fx.h"
#include "eclipse_core.h"     // ec::self_dir
#include "core_internal.h"    // config_flush

namespace {

std::mutex  g_mux;
// Ordered, so a rewritten file is stable and diffable rather than reordering
// itself on every save.
std::map<std::string, std::string> g_kv;
bool        g_loaded = false;
bool        g_dirty  = false;
DWORD       g_last_write = 0;

const DWORD WRITE_INTERVAL_MS = 1000;

std::string cfg_path() {
    char appdata[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
    std::string dir = (n && n < MAX_PATH) ? std::string(appdata) + "\\EclipseConsole"
                                          : ec::self_dir();
    return dir + "\\settings.conf";
}

void trim(std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) { s.clear(); return; }
    size_t b = s.find_last_not_of(" \t\r\n");
    s = s.substr(a, b - a + 1);
}

// Load once. A missing file is not an error -- it is the ordinary state before
// anything has ever been saved.
void load_locked() {
    if (g_loaded) return;
    g_loaded = true;
    FILE* f = fopen(cfg_path().c_str(), "rb");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        std::string l = line;
        size_t hash = l.find('#');
        if (hash != std::string::npos) l = l.substr(0, hash);
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string k = l.substr(0, eq), v = l.substr(eq + 1);
        trim(k); trim(v);
        if (!k.empty()) g_kv[k] = v;
    }
    fclose(f);
}

// Rewrite wholesale. Via a temp file and MoveFileEx so an interrupted write
// cannot leave a truncated settings file behind -- losing every dial at once
// is a worse failure than losing the last change.
void save_locked() {
    std::string path = cfg_path();
    std::string tmp  = path + ".tmp";

    // The directory may not exist yet on a fresh install where no snippet has
    // been saved. Creating it is ours to do: this is not the snippets tree.
    size_t slash = path.find_last_of("\\/");
    if (slash != std::string::npos) CreateDirectoryA(path.substr(0, slash).c_str(), nullptr);

    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    fprintf(f, "# Eclipse Console -- snippet settings (fx_cfg_*).\n"
               "# Rewritten by the console; edit freely, it is read back on the\n"
               "# next fx_cfg_ call after a run starts.\n\n");
    for (const auto& kv : g_kv)
        fprintf(f, "%-40s = %s\n", kv.first.c_str(), kv.second.c_str());
    fclose(f);

    MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    g_dirty = false;
    g_last_write = GetTickCount();
}

// Called after every set. Coalesces: in memory now, on disk at most once a
// second. config_flush() covers the tail.
void touch_locked() {
    g_dirty = true;
    DWORD now = GetTickCount();
    if (!g_last_write || (now - g_last_write) >= WRITE_INTERVAL_MS) save_locked();
}

// A value may not contain a newline: the format is one pair per line, and a
// value that broke that would come back as a different key next load. Replaced
// rather than refused -- silently storing nothing would be worse.
std::string sanitise(const char* v) {
    std::string s = v ? v : "";
    for (char& c : s) if (c == '\n' || c == '\r') c = ' ';
    return s;
}

}  // namespace

// ---- exports ---------------------------------------------------------------

FX_API double fx_cfg_num(const char* key, double fallback) {
    if (!key || !*key) return fallback;
    std::lock_guard<std::mutex> lk(g_mux);
    load_locked();
    auto it = g_kv.find(key);
    if (it == g_kv.end() || it->second.empty()) return fallback;
    // strtod, checked: a key holding text must read as the fallback rather
    // than as 0.0, which would look exactly like a deliberately-zeroed dial.
    const char* s = it->second.c_str();
    char* end = nullptr;
    double v = strtod(s, &end);
    if (end == s) return fallback;
    return v;
}

FX_API void fx_cfg_set_num(const char* key, double value) {
    if (!key || !*key) return;
    char buf[64];
    // %.17g round-trips a double exactly; the trailing noise is the price of
    // reading back what you wrote.
    snprintf(buf, sizeof buf, "%.17g", value);
    std::lock_guard<std::mutex> lk(g_mux);
    load_locked();
    g_kv[key] = buf;
    touch_locked();
}

FX_API int fx_cfg_str(const char* key, char* out, int max, const char* fallback) {
    if (!out || max <= 0) return 0;
    std::string v;
    {
        std::lock_guard<std::mutex> lk(g_mux);
        load_locked();
        auto it = key && *key ? g_kv.find(key) : g_kv.end();
        v = it != g_kv.end() ? it->second : (fallback ? fallback : "");
    }
    int n = (int)v.size();
    if (n > max - 1) n = max - 1;
    memcpy(out, v.data(), (size_t)n);
    out[n] = 0;
    return n;
}

FX_API void fx_cfg_set_str(const char* key, const char* value) {
    if (!key || !*key) return;
    std::string v = sanitise(value);
    std::lock_guard<std::mutex> lk(g_mux);
    load_locked();
    g_kv[key] = v;
    touch_locked();
}

FX_API int fx_cfg_has(const char* key) {
    if (!key || !*key) return 0;
    std::lock_guard<std::mutex> lk(g_mux);
    load_locked();
    return g_kv.find(key) != g_kv.end() ? 1 : 0;
}

// ---- host side (core_internal.h) -------------------------------------------

void config_flush(void) {
    std::lock_guard<std::mutex> lk(g_mux);
    if (g_dirty) save_locked();
}
