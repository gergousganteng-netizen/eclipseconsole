// ---- snippet file tree --------------------------------------------------
// The filesystem layer under the sidebar and the in-game panel's list: paths
// relative to snippets/, the .order file, and the directory scans. Split out
// of eclipse_ui.cpp. push_snippets/push_hud_snippets stayed behind -- they are
// the page-message producers and belong with the host's ui_post/hud_push.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>
#include <vector>

#include "host.h"
#include "tree.h"

// Sidebar order. The filesystem has no notion of one -- FindFirstFile just
// returns directory order -- so a custom arrangement has to be stored. A plain
// list of filenames in snippets/.order does it without renaming anything, so
// the snippets stay ordinary .cpp files that --run still accepts. The leading
// dot also keeps it out of the *.cpp glob below.
std::string order_file(const std::string& snipdir) { return snipdir + "\\.order"; }

// A snippet path relative to snippets/: either "name.cpp" or "group/name.cpp".
//
// These arrive from the page and are turned into filesystem paths, so this is
// the only thing between a crafted name and a write outside snippets/.
//
// Nesting is BOUNDED, not forbidden: up to kMaxDepth (8) segments, per the
// rule below. This comment used to say "exactly one level is allowed", which
// was true before groups nested and then survived the change that made them
// nest -- it was three lines above a comment saying the opposite. Measured
// before rewriting: "a/b/c/d/e/f/g/h" is accepted and a ninth segment is
// rejected, as are "../x", "a/../b", "a//b", "a/./b", "/a", "a/", "",
// "C:\x", and trailing "." or " " on any segment.
bool safe_rel(const std::string& p) {
    if (p.empty() || p.size() > 200) return false;
    // Backslash is excluded deliberately: '/' is the only separator we accept,
    // so there is exactly one spelling to validate.
    if (p.find_first_of("\\:*?\"<>|") != std::string::npos) return false;
    if (p.front() == '/' || p.back() == '/') return false;

    // Groups nest, so any number of separators is allowed -- but bounded. An
    // unbounded depth is an unbounded recursion in the scanner and an unbounded
    // path length on disk, and nothing legitimate needs more than this.
    const int kMaxDepth = 8;

    int segments = 0;
    for (size_t start = 0; start <= p.size(); ) {
        size_t end = p.find('/', start);
        std::string seg = p.substr(start, end == std::string::npos ? std::string::npos
                                                                   : end - start);
        // Empty rejects "a//b"; "." and ".." reject traversal. Those are the
        // whole of the security argument -- every segment is a plain name, so no
        // path can climb out of snippets/ however deeply it nests.
        if (seg.empty() || seg == "." || seg == "..") return false;
        // Windows silently strips these, so "foo." and "foo" would collide.
        if (seg.back() == ' ' || seg.back() == '.') return false;
        if (++segments > kMaxDepth) return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

// Everything before the last '/', i.e. the group a path sits in. "" for root.
std::string parent_of(const std::string& rel) {
    size_t slash = rel.rfind('/');
    return slash == std::string::npos ? std::string() : rel.substr(0, slash);
}

// Everything after the last '/'.
std::string leaf_of(const std::string& rel) {
    size_t slash = rel.rfind('/');
    return slash == std::string::npos ? rel : rel.substr(slash + 1);
}

// Same path, spelled the way the Win32 file APIs want it.
std::string win_path(const std::string& rel) {
    std::string s = rel;
    for (char& c : s) if (c == '/') c = '\\';
    return s;
}

// Create every directory leading up to `rel`, which is a path relative to
// snippets/. Groups nest, so "a/b/c.cpp" needs both a and a\b -- CreateDirectory
// makes one level at a time and fails on a missing parent.
void ensure_group_dirs(const std::string& snipdir, const std::string& rel) {
    std::string sofar = snipdir;
    size_t start = 0;
    for (;;) {
        size_t slash = rel.find('/', start);
        if (slash == std::string::npos) break;       // the last segment is the file
        sofar += "\\" + rel.substr(start, slash - start);
        CreateDirectoryW(widen(sofar).c_str(), nullptr);
        start = slash + 1;
    }
}

std::vector<std::string> read_order(const std::string& snipdir) {
    std::vector<std::string> out;
    std::ifstream f(order_file(snipdir));
    std::string line;
    bool first = true;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        // Strip a UTF-8 BOM off the first line. Notepad writes one by default,
        // as does PowerShell's `Set-Content -Encoding UTF8` -- and the failure is
        // silent: the first entry simply stops matching any filename, so that
        // one snippet quietly falls to the bottom of the list and nothing says
        // why.
        if (first) {
            first = false;
            if (line.size() >= 3 && (unsigned char)line[0] == 0xEF
                                 && (unsigned char)line[1] == 0xBB
                                 && (unsigned char)line[2] == 0xBF)
                line.erase(0, 3);
        }
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

// Snippets are plain .cpp files in snippets/ — the same files the CLI takes
// with --run, so nothing is locked inside the UI.
// The .cpp files directly in `dir`, each prefixed so the result is a path
// relative to snippets/.
// Headers count as sources here. A project shares declarations through one, and
// leaving it out of the list meant the file existed, compiled, and could not be
// opened -- editable only by leaving the app.
static bool is_source(const std::string& name) {
    size_t dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot);
    for (char& c : ext) c = (char)tolower((unsigned char)c);
    // .html/.css/.js are here so a project can SHIP a real UI. They are not
    // compiled -- run_project globs *.cpp -- but being "source" is what puts
    // them in the sidebar and, more importantly, in an export bundle. A page
    // loaded with fx_window_navigate lives in files like these, and without
    // this the project would arrive at someone else with no interface at all.
    // Precedent: .h and .inc are already listed and are not runnable either.
    return ext == ".cpp" || ext == ".h" || ext == ".hpp" || ext == ".inc" ||
           ext == ".html" || ext == ".css" || ext == ".js";
}

void scan_cpp(const std::string& dir, const std::string& prefix,
              std::vector<std::string>& out) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(widen(dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string n = narrow(fd.cFileName);
        if (is_source(n)) out.push_back(prefix + n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Every group under snippets/, at any depth, as paths relative to it
// ("movement", "movement/experiments"). Dot-directories are skipped so .trash
// does not show up as one. Sorted, so a parent always precedes its children
// and the page can build the tree in one pass.
static void walk_groups(const std::string& dir, const std::string& prefix,
                        std::vector<std::string>& out, int depth) {
    if (depth > 8) return;                       // matches safe_rel's cap
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(widen(dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        // A junction or symlink can point back up the tree, and the depth cap
        // alone would only turn that into a slow crawl rather than stopping it.
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        std::string n = narrow(fd.cFileName);
        if (n.empty() || n[0] == '.') continue;  // ".", "..", ".trash"
        std::string rel = prefix + n;
        out.push_back(rel);
        walk_groups(dir + "\\" + n, rel + "/", out, depth + 1);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::vector<std::string> list_groups(const std::string& snipdir) {
    std::vector<std::string> groups;
    walk_groups(snipdir, "", groups, 1);
    std::sort(groups.begin(), groups.end());
    return groups;
}

// One directory's worth of fingerprint: path, size, last-write time per source
// file. FindFirstFileW hands all three back with the name, so this costs a
// directory enumeration and NOTHING ELSE -- no file is opened.
static void stamp_dir(const std::string& dir, const std::string& prefix,
                      std::vector<std::string>& out) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(widen(dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string n = narrow(fd.cFileName);
        if (!is_source(n)) continue;
        char b[64];
        snprintf(b, sizeof b, "|%llu|%llu",
                 (unsigned long long)fd.nFileSizeLow |
                     ((unsigned long long)fd.nFileSizeHigh << 32),
                 (unsigned long long)fd.ftLastWriteTime.dwLowDateTime |
                     ((unsigned long long)fd.ftLastWriteTime.dwHighDateTime << 32));
        out.push_back(prefix + n + b);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// A cheap fingerprint of everything push_snippets would send.
//
// WHY: push_snippets is not a listing. It opens every snippet, reads the whole
// source text, JSON-escapes it and ships the entire corpus to the page AND the
// in-game panel. The watcher's blind 2s poll did that unconditionally -- 200ms
// of CPU every 2.1s, measured, all of it dropped again by the page's
// identical-body guard. This is what the poll asks instead.
//
// Covers what a change can touch: each source file's path, size and write time
// (so an edit, an add, a delete or a rename all move it), the group list (so a
// new empty folder is seen), which groups carry a .project marker, and .order's
// own size and time. A content edit that preserved BOTH size and 100ns write
// time would slip past, which is not a thing a real save does -- and the
// notification path does not consult this at all, so a genuine save is pushed
// on its own evidence.
//
// FAILS OPEN: user actions push directly without updating the watcher's copy,
// so the next poll simply sees a difference and pushes once more. A wasted push
// is the old behaviour; a missed one would be a stale sidebar.
std::string snip_tree_stamp(const std::string& snipdir) {
    std::vector<std::string> parts;
    stamp_dir(snipdir, "", parts);
    for (const std::string& g : list_groups(snipdir)) {
        stamp_dir(snipdir + "\\" + win_path(g), g + "/", parts);
        std::string marker = snipdir + "\\" + win_path(g) + "\\.project";
        bool proj = GetFileAttributesW(widen(marker).c_str()) != INVALID_FILE_ATTRIBUTES;
        parts.push_back("<" + g + (proj ? "/.project>" : ">"));
    }
    WIN32_FILE_ATTRIBUTE_DATA ad{};
    if (GetFileAttributesExW(widen(order_file(snipdir)).c_str(),
                             GetFileExInfoStandard, &ad)) {
        char b[80];
        snprintf(b, sizeof b, "<.order|%lu|%llu>", ad.nFileSizeLow,
                 (unsigned long long)ad.ftLastWriteTime.dwLowDateTime |
                     ((unsigned long long)ad.ftLastWriteTime.dwHighDateTime << 32));
        parts.push_back(b);
    }
    // SORTED, because FindFirstFileW's order is not contractual. An order flip
    // would only cost a redundant push, but there is no reason to pay it.
    std::sort(parts.begin(), parts.end());
    std::string s;
    for (const std::string& p : parts) { s += p; s += '\n'; }
    return s;
}
