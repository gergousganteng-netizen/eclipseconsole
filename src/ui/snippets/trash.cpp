// ---- trash ------------------------------------------------------------------
// Deleting moves a snippet to snippets\.trash instead of unlinking it, and the
// directory stays FLAT: a group becomes part of the filename, so two files
// called main.cpp in different groups do not overwrite one another.
//
// That flattening is not reversible on its own -- "movement_walkspeed.cpp"
// could equally be group `movement` file `walkspeed.cpp` or a loose file with
// an underscore in its name. So the original path is recorded alongside it.
// Anything trashed before this existed has no entry and restores to the top
// level, which is recoverable rather than correct, and the UI says so.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "host.h"
#include "trash.h"

std::string trash_dir(const std::string& snipdir)          { return snipdir + "\\.trash"; }
static std::string trash_index(const std::string& snipdir) { return trash_dir(snipdir) + "\\index.txt"; }

std::string flatten_name(const std::string& rel) {
    std::string flat = rel;
    for (char& c : flat) if (c == '/') c = '_';
    return flat;
}

// flat -> original relative path.
std::vector<std::pair<std::string, std::string>> trash_load_index(const std::string& snipdir) {
    std::vector<std::pair<std::string, std::string>> v;
    std::ifstream f(trash_index(snipdir), std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        v.push_back({ line.substr(0, tab), line.substr(tab + 1) });
    }
    return v;
}

static void trash_save_index(const std::string& snipdir,
                             const std::vector<std::pair<std::string, std::string>>& v) {
    std::ofstream o(trash_index(snipdir), std::ios::binary);
    for (auto& e : v) o << e.first << "\t" << e.second << "\n";
}

void trash_remember(const std::string& snipdir, const std::string& flat, const std::string& original) {
    CreateDirectoryW(widen(trash_dir(snipdir)).c_str(), nullptr);
    auto v = trash_load_index(snipdir);
    // Delete overwrites a previous copy of the same flat name, so the index has
    // to as well or it would describe the file that used to be there.
    for (auto& e : v) if (e.first == flat) { e.second = original; trash_save_index(snipdir, v); return; }
    v.push_back({ flat, original });
    trash_save_index(snipdir, v);
}

void trash_forget(const std::string& snipdir, const std::string& flat) {
    auto v = trash_load_index(snipdir);
    std::vector<std::pair<std::string, std::string>> out;
    for (auto& e : v) if (e.first != flat) out.push_back(e);
    trash_save_index(snipdir, out);
}

// A trash entry is a bare filename, never a path. Restricting it here means the
// restore and purge handlers cannot be talked into touching anything outside
// .trash, whatever the page sends.
bool safe_trash_name(const std::string& n) {
    if (n.empty() || n.size() > 240) return false;
    if (n == "index.txt") return false;
    if (n.find_first_of("\\/:*?\"<>|") != std::string::npos) return false;
    if (n.find("..") != std::string::npos) return false;
    return true;
}

void push_trash(const std::string& snipdir) {
    auto idx = trash_load_index(snipdir);
    auto original_of = [&](const std::string& flat) -> std::string {
        for (auto& e : idx) if (e.first == flat) return e.second;
        return "";
    };

    std::string j = "[";
    bool first = true;

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(widen(trash_dir(snipdir) + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::string n = narrow(fd.cFileName);
            if (n == "index.txt") continue;
            ULARGE_INTEGER sz{}; sz.LowPart = fd.nFileSizeLow; sz.HighPart = fd.nFileSizeHigh;
            ULARGE_INTEGER wt{};
            wt.LowPart  = fd.ftLastWriteTime.dwLowDateTime;
            wt.HighPart = fd.ftLastWriteTime.dwHighDateTime;
            // 100ns since 1601 -> ms since 1970, so the page can format it.
            unsigned long long ms = (wt.QuadPart - 116444736000000000ULL) / 10000ULL;

            if (!first) j += ",";
            first = false;
            j += "{\"n\":\"" + json_escape(n) +
                 "\",\"o\":\"" + json_escape(original_of(n)) +
                 "\",\"sz\":" + std::to_string(sz.QuadPart) +
                 ",\"t\":" + std::to_string(ms) + "}";
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    j += "]";
    ui_post("TRASH:" + j);
}
