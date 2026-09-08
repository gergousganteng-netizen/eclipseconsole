// ------------------------------ launching Roblox -----------------------------
//
// The console cannot do anything until a client exists, and the watcher already
// attaches by itself once one appears -- so all this has to do is start one.
//
// It does NOT hunt for a bootstrapper and run it. The `roblox-player:` protocol
// handler is by definition whichever launcher the user actually installed --
// Froststrap, Bloxstrap, Fishstrap or stock Roblox register themselves as it --
// so handing the URI to the shell launches the RIGHT one without this having to
// know any of their install paths. Path detection is kept only to LABEL the
// button and as a fallback if the protocol is not registered at all.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <string>

#include "host.h"
#include "log.h"
#include "launch.h"

// Expands %VARS% in place. Returns empty when a variable is missing, so a
// caller cannot accidentally test a literal "%LOCALAPPDATA%\..." path.
static std::string expand_env(const std::string& in) {
    char out[MAX_PATH * 2];
    DWORD n = ExpandEnvironmentStringsA(in.c_str(), out, sizeof out);
    if (!n || n > sizeof out) return "";
    std::string s(out);
    return s.find('%') == std::string::npos ? s : "";
}

static bool file_exists(const std::string& p) {
    return !p.empty() && GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// The newest RobloxPlayerBeta.exe under the stock install. Each client build
// gets its own version-<hash> directory and old ones are left behind, so
// "newest by write time" is the only way to pick the current one.
static std::string find_stock_roblox() {
    std::string dir = expand_env("%LOCALAPPDATA%\\Roblox\\Versions");
    if (dir.empty()) return "";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return "";
    std::string best;
    FILETIME newest{};
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue;
        std::string exe = dir + "\\" + fd.cFileName + "\\RobloxPlayerBeta.exe";
        WIN32_FILE_ATTRIBUTE_DATA ad;
        if (!GetFileAttributesExA(exe.c_str(), GetFileExInfoStandard, &ad)) continue;
        if (best.empty() || CompareFileTime(&ad.ftLastWriteTime, &newest) > 0) {
            newest = ad.ftLastWriteTime;
            best = exe;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return best;
}

// Bootstrappers first, stock last. A machine with Froststrap installed also has
// a stock Roblox directory, and naming the bootstrapper is the honest label.
Launcher detect_launcher() {
    static std::string path;          // storage for the returned pointer
    static const struct { const char* label; const char* p; } kBoots[] = {
        { "Froststrap", "%LOCALAPPDATA%\\FrostStrap\\FrostStrap.exe" },
        { "Froststrap", "%LOCALAPPDATA%\\Froststrap\\Froststrap.exe" },
        { "Bloxstrap",  "%LOCALAPPDATA%\\Bloxstrap\\Bloxstrap.exe"   },
        { "Fishstrap",  "%LOCALAPPDATA%\\Fishstrap\\Fishstrap.exe"   },
    };
    for (const auto& b : kBoots) {
        std::string full = expand_env(b.p);
        if (file_exists(full)) { path = full; return { b.label, path.c_str() }; }
    }
    path = find_stock_roblox();
    if (!path.empty()) return { "Roblox", path.c_str() };
    return { "", "" };
}

void launch_roblox() {
    Launcher l = detect_launcher();

    // The protocol handler, which is whatever the user actually installed.
    // launchmode:play with no place is what a bootstrapper's own shortcut
    // does: it opens the app rather than joining anything.
    // Announced BEFORE the spawn. Logging afterwards let the watchdog notice
    // the new client and print "waiting for RobloxCrashHandler" FIRST, so the
    // console read as though it started waiting before you pressed launch.
    ui_log("STATUS", l.label[0]
           ? std::string("launching Roblox via ") + l.label + " - will attach on its own"
           : std::string("launching Roblox - will attach on its own"));

    HINSTANCE rc = ShellExecuteA(nullptr, "open", "roblox-player:1+launchmode:play",
                                 nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)rc > 32) return;

    // Nothing registered for the protocol. Fall back to running the launcher
    // directly, which still works and is worth trying before giving up.
    if (l.path[0]) {
        HINSTANCE rc2 = ShellExecuteA(nullptr, "open", l.path, nullptr, nullptr, SW_SHOWNORMAL);
        if ((INT_PTR)rc2 > 32) {
            ui_log("STATUS", std::string("launching ") + l.label + " directly");
            return;
        }
    }
    ui_log("ERROR", "x could not launch Roblox - no roblox-player handler and no launcher found");
}
