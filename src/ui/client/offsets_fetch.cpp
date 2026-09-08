// ---- offsets fetch ----------------------------------------------------------
// When the attached client is a version the conf is not for, try to fetch a
// matching one. The source publishes a C++ header per version:
//
//   namespace Offsets {
//       inline std::string ClientVersion = "version-145f189a6a974303";
//       namespace Camera {
//            inline constexpr uintptr_t FieldOfView = 0x140;
//       }
//   }
//
// Turned into the conf's Class.Field = 0xN form. NOTHING is written unless the
// parse succeeds AND the header names the version we asked for AND it yields a
// plausible number of offsets: a 404 page, a JS shell or a captive-portal
// interstitial all arrive as a 200 with a body, and any of them would
// otherwise overwrite a working conf with nothing.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "eclipse_core.h"
#include "host.h"
#include "placename.h"
#include "log.h"
#include "offsets_fetch.h"

static const int kMinPlausibleOffsets = 300;   // a real dump is ~390

static std::string parse_offsets_header(const std::string& src,
                                        const std::string& wantVersion,
                                        int& count) {
    count = 0;
    std::string ns, out, declared;

    size_t p = 0;
    while (p <= src.size()) {
        size_t nl = src.find('\n', p);
        std::string line = src.substr(p, nl == std::string::npos
                                          ? std::string::npos : nl - p);
        if (nl == std::string::npos) p = src.size() + 1; else p = nl + 1;

        size_t a = line.find_first_not_of(" \t\r");
        if (a == std::string::npos) continue;
        line = line.substr(a);

        // The version the file says it is for. Checked rather than trusted:
        // a stale mirror serving the wrong dump is exactly the failure this
        // whole feature exists to prevent.
        size_t cv = line.find("ClientVersion");
        if (cv != std::string::npos) {
            size_t q1 = line.find('"', cv);
            size_t q2 = (q1 == std::string::npos) ? std::string::npos
                                                  : line.find('"', q1 + 1);
            if (q2 != std::string::npos) declared = line.substr(q1 + 1, q2 - q1 - 1);
            continue;
        }

        if (line.compare(0, 10, "namespace ") == 0) {
            std::string n = line.substr(10);
            size_t e = n.find_first_of(" \t{");
            if (e != std::string::npos) n = n.substr(0, e);
            // The outer wrapper is not part of a key.
            ns = (n == "Offsets") ? "" : n;
            continue;
        }
        if (line[0] == '}') { continue; }

        size_t up = line.find("uintptr_t ");
        if (up == std::string::npos || ns.empty()) continue;
        size_t eq = line.find('=', up);
        if (eq == std::string::npos) continue;

        std::string field = line.substr(up + 10, eq - (up + 10));
        while (!field.empty() && (field.back() == ' ' || field.back() == '\t')) field.pop_back();
        std::string val = line.substr(eq + 1);
        size_t vs = val.find_first_not_of(" \t");
        size_t ve = val.find_first_of(";,/ \t\r", vs == std::string::npos ? 0 : vs);
        if (vs == std::string::npos) continue;
        val = val.substr(vs, ve == std::string::npos ? std::string::npos : ve - vs);
        if (val.empty() || field.empty()) continue;

        out += ns + "." + field + " = " + val + "\n";
        count++;
    }

    if (declared != wantVersion) { count = -1; return ""; }
    return out;
}

// Offsets written on success, 0 on any failure.
//
// Returns the COUNT rather than a bool so the caller can report it in the same
// line as whether running was actually re-enabled -- which it can only know
// after re-attaching, not from here.
int fetch_offsets_for(const std::string& version) {
    // A manual refresh can ask for the version we are already on -- theo
    // republishes a dump when fields are added, which is how Instance.
    // NameContainer arrived. Announcing "updating X to X, running is blocked"
    // for that would be both wrong and alarming.
    if (ec::state().offsets_stale)
        ui_log("STATUS", "! Updating offsets for " + ec::conf_version() +
                         " to " + version + " - running is blocked");
    else
        ui_log("STATUS", "refreshing offsets for " + version + "...");

    std::wstring path = L"/" + widen(version) + L"/offsets.hpp";
    std::string body = http_get(L"offsets.imtheo.lol", path);
    if (body.empty()) {
        ui_log("ERROR", "x no offsets published for " + version + " (nothing returned)");
        return 0;
    }

    int n = 0;
    std::string conf = parse_offsets_header(body, version, n);
    if (n < 0) {
        ui_log("ERROR", "x the file served for " + version + " is for a different version");
        return 0;
    }
    if (n < kMinPlausibleOffsets) {
        char b[128];
        snprintf(b, sizeof b, "x only %d offsets parsed - not a usable dump, keeping the current conf", n);
        ui_log("ERROR", b);
        return 0;
    }

    // Written only now that it is known good, and to a temp file first so a
    // failure part-way through cannot leave a half-written conf in place of a
    // working one.
    std::string dest = ec::self_dir() + "\\conf\\offsets_windows.conf";
    std::string tmp  = dest + ".new";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) { ui_log("ERROR", "x could not write the conf"); return 0; }
        o << "# offsets_windows.conf - FETCHED from offsets.imtheo.lol\n"
          << "#\n"
          << "# Windows Roblox client: " << version << "\n"
          << "# " << n << " offsets\n"
          << "#\n"
          << "# Key format is Class.Field, matching fx_off(\"Instance.Parent\").\n\n"
          << conf;
    }
    DeleteFileA(dest.c_str());
    if (!MoveFileA(tmp.c_str(), dest.c_str())) {
        ui_log("ERROR", "x could not replace the conf");
        return 0;
    }

    // The success line is the CALLER's to write: whether running is enabled
    // again depends on the re-attach, which has not happened yet.
    return n;
}

// Everything that has to follow a successful fetch, shared by the automatic
// path at attach time and the manual one from Settings.
//
// push_offsets() is the part that is easy to miss. The page is sent the offset
// table ONCE, on READY, so without this the palette keeps listing the offsets
// that were on disk at startup -- including, after an update, values that no
// longer exist. The conf on disk and the table you can search would disagree
// with nothing to say so.
void apply_fetched_offsets(const std::string& version, uint32_t pid,
                           int n, bool was_stale, const std::string& conf) {
    ec::attach(conf, true, pid);         // redo with the right ones
    push_offsets();
    // Only now can this claim anything about running, because only the
    // re-attach clears offsets_stale.
    char b[192];
    snprintf(b, sizeof b, "+ fetched %d offsets for %s%s", n, version.c_str(),
             (was_stale && !ec::state().offsets_stale) ? " - running is enabled" : "");
    ui_log("STATUS", b);
}
