// conf.cpp -- parse the offsets conf into the offset table. Split out of
// eclipse_console.cpp now that the table (offsets.h) is behind accessors: this
// is the one writer that loads it from disk. It reaches the conf's declared
// version, which the attach staleness check owns, through a single forwarder.

#include <cstdint>
#include <cstdlib>   // strtoull
#include <fstream>
#include <string>

#include "eclipse_core.h"    // ec::state().version (the read-back)
#include "core_internal.h"   // core_set_conf_version (the write)
#include "offsets.h"         // offset_clear / offset_set / offset_size
#include "conf.h"

static void trim(std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) { s.clear(); return; }
    s = s.substr(a, b - a + 1);
}

size_t load_offsets(const std::string& path) {
    offset_clear();
    core_set_conf_version("");
    std::ifstream f(path);
    if (!f) return 0;
    std::string line;
    while (std::getline(f, line)) {
        // Keep the version note before the comment strip — it lives in a comment.
        size_t vp = line.find("client:");
        if (vp == std::string::npos) vp = line.find("Windows Roblox client:");
        if (vp != std::string::npos && ec::conf_version().empty()) {
            std::string v = line.substr(line.find(':', vp) + 1);
            trim(v);
            core_set_conf_version(v);
        }
        size_t h = line.find('#');
        if (h != std::string::npos) line = line.substr(0, h);
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        trim(k); trim(v);
        if (k.empty() || v.empty()) continue;
        offset_set(k.c_str(), strtoull(v.c_str(), nullptr, 0));
    }
    // Publishes everything above in one atomic store. Until this line readers
    // still see the empty table the clear installed.
    offset_commit();
    return offset_size();
}
