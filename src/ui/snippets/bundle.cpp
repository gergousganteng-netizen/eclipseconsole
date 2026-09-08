#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>       // the export/import file dialogs

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "eclipse_core.h"

#include "bundle.h"
#include "host.h"
#include "log.h"
#include "tree.h"

// ---- bundle export / import -------------------------------------------------
// One plain-text file carrying any of: the snippets tree, the settings, and
// the offsets conf. Meant for handing a working setup to someone else.
//
// NOT a zip. Everything in scope is text, a zip would mean a dependency, and
// a bundle you can open and read is one you can trust before running it --
// which matters when the thing being shared is code someone else will compile.
//
// Length-prefixed rather than delimiter-separated, so a snippet containing
// what looks like a header cannot corrupt the parse:
//
//   # eclipse-bundle 1
//   [settings]
//   editor.fontSize=14
//   [file] snippets/radar.cpp 15829
//   <exactly 15829 bytes>
//   [file] conf/offsets_windows.conf 12043
//   <exactly 12043 bytes>
//   [end]
static const char* kBundleMagic = "# eclipse-bundle 1";

// Every file under `dir`, as paths relative to it. Not just .cpp: .project
// marks a folder as a project and .order holds the sidebar arrangement, and a
// tree that arrived without them would be subtly not the tree that was sent.
static void walk_files(const std::string& dir, const std::string& prefix,
                       std::vector<std::string>& out) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(widen(dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string n = narrow(fd.cFileName);
        if (n == "." || n == "..") continue;
        if (n == ".trash") continue;              // deleted work is not shared
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            walk_files(dir + "\\" + n, prefix + n + "/", out);
        else
            out.push_back(prefix + n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static bool pick_file(HWND owner, bool saving, std::string& path) {
    wchar_t buf[MAX_PATH] = L"eclipse-bundle.txt";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner   = owner;
    ofn.lpstrFilter = L"Eclipse bundle (*.txt)\0*.txt\0All files\0*.*\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrDefExt = L"txt";
    ofn.Flags = saving ? (OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST)
                       : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);
    if (!saving) buf[0] = 0;
    BOOL ok = saving ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (!ok) return false;
    path = narrow(buf);
    return true;
}

static void append_blob(std::ostream& o, const std::string& body,
                        const std::string& rel) {
    o << "[file] " << rel << " " << body.size() << "\n";
    o.write(body.data(), (std::streamsize)body.size());
    o << "\n";
}

static void append_file(std::ostream& o, const std::string& disk,
                        const std::string& rel) {
    std::ifstream f(disk, std::ios::binary);
    if (!f) return;
    std::string body((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    append_blob(o, body, rel);
}

// `files` is the exact list the page ticked, relative to the snippets root.
// `settings` is its own k=v block, passed through untouched -- the host does
// not know or care what the keys mean.
void do_export(HWND owner, const std::string& snipdir,
               const std::vector<std::string>& files, bool doSettings,
               bool offsets, const std::string& settings) {
    std::string path;
    if (!pick_file(owner, true, path)) return;    // cancelled

    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    if (!o) { ui_log("ERROR", "x could not write " + path); return; }
    o << kBundleMagic << "\n";

    int nfiles = 0;
    if (doSettings && !settings.empty()) {
        o << "[settings]\n" << settings;
        if (settings.back() != '\n') o << "\n";
    }

    for (const std::string& r : files) {
        if (r.find("..") != std::string::npos) continue;
        std::string disk = snipdir + "\\" + win_path(r);
        if (GetFileAttributesW(widen(disk).c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        append_file(o, disk, "snippets/" + r);
        nfiles++;
    }

    // The markers the page does not model as files, added for any folder that
    // actually contributed something. Without the .project marker a project
    // arrives as a plain group and stops compiling as one unit -- the same
    // files, silently not the same thing.
    if (!files.empty()) {
        std::vector<std::string> dirs;
        for (const std::string& r : files) {
            size_t s = r.rfind('/');
            if (s == std::string::npos) continue;
            std::string d = r.substr(0, s);
            if (std::find(dirs.begin(), dirs.end(), d) == dirs.end()) dirs.push_back(d);
        }
        for (const std::string& d : dirs) {
            std::string m = snipdir + "\\" + win_path(d) + "\\.project";
            if (GetFileAttributesW(widen(m).c_str()) != INVALID_FILE_ATTRIBUTES) {
                append_file(o, m, "snippets/" + d + "/.project");
                nfiles++;
            }
        }
        // The sidebar arrangement, FILTERED to what was actually sent.
        //
        // This used to ship .order verbatim, which disclosed the name of every
        // snippet and project in the sidebar -- including ones since deleted,
        // whose names linger in the file -- even when the export was a single
        // snippet. Measured on a real export: one file sent, twenty-four names
        // leaked.
        //
        // The recipient could not use those lines either. Names that do not
        // exist are skipped when .order is read, so every extra line was pure
        // disclosure with no effect. On a fresh install, where nothing is there
        // to protect it, they were written to their disk verbatim.
        std::string ord = snipdir + "\\.order";
        std::ifstream of(ord, std::ios::binary);
        if (of) {
            // Worth mentioning: each exported file, plus every ancestor folder
            // of one. A group needs its own line -- that is what carries its
            // position among its siblings, not the lines of its children.
            std::vector<std::string> keep;
            for (const std::string& r : files) {
                keep.push_back(r);
                for (size_t s = r.find('/'); s != std::string::npos; s = r.find('/', s + 1))
                    keep.push_back(r.substr(0, s));
            }

            std::string filtered, line;
            while (std::getline(of, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                    line.pop_back();
                if (line.empty()) continue;
                if (std::find(keep.begin(), keep.end(), line) != keep.end())
                    filtered += line + "\n";
            }
            // Nothing to say rather than an empty file: a zero-length .order is
            // a file the far side still has to accept and skip.
            if (!filtered.empty()) {
                append_blob(o, filtered, "snippets/.order");
                nfiles++;
            }
        }
    }

    if (offsets) {
        std::string conf = ec::self_dir() + "\\conf\\offsets_windows.conf";
        append_file(o, conf, "conf/offsets_windows.conf");
        nfiles++;
    }
    o << "[end]\n";
    o.close();

    char b[64];
    snprintf(b, sizeof b, "+ exported %d file%s to ", nfiles, nfiles == 1 ? "" : "s");
    ui_log("STATUS", std::string(b) + path);
}

void do_import(HWND owner, const std::string& snipdir) {
    std::string path;
    if (!pick_file(owner, false, path)) return;

    std::ifstream f(path, std::ios::binary);
    if (!f) { ui_log("ERROR", "x could not read " + path); return; }
    std::string all((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    if (all.compare(0, strlen(kBundleMagic), kBundleMagic) != 0) {
        ui_log("ERROR", "x not an Eclipse bundle - wrong header");
        return;
    }

    size_t p = all.find('\n');
    if (p == std::string::npos) return;
    p++;

    std::string settings;
    int added = 0;
    std::vector<std::string> skipped;

    while (p < all.size()) {
        size_t nl = all.find('\n', p);
        if (nl == std::string::npos) break;
        std::string line = all.substr(p, nl - p);
        while (!line.empty() && line.back() == '\r') line.pop_back();
        p = nl + 1;

        if (line == "[end]") break;
        if (line == "[settings]") {
            // Runs to the next [ at the start of a line.
            while (p < all.size() && all[p] != '[') {
                size_t e = all.find('\n', p);
                if (e == std::string::npos) e = all.size();
                settings += all.substr(p, e - p) + "\n";
                p = e + 1;
            }
            continue;
        }
        if (line.compare(0, 7, "[file] ") == 0) {
            size_t sp = line.rfind(' ');
            if (sp == std::string::npos || sp < 7) break;
            std::string rel = line.substr(7, sp - 7);
            size_t len = (size_t)atoll(line.c_str() + sp + 1);
            if (p + len > all.size()) { ui_log("ERROR", "x bundle truncated"); break; }
            std::string body = all.substr(p, len);
            p += len;
            if (p < all.size() && all[p] == '\n') p++;

            // Where it lands. Only two roots are accepted, and a path is
            // rebuilt from its parts rather than trusted -- a bundle is a file
            // from someone else, and "snippets/../../windows/system32/..." is
            // exactly the shape of thing that would otherwise work.
            std::string dest;
            if      (rel.compare(0, 9, "snippets/") == 0) dest = snipdir + "\\" + win_path(rel.substr(9));
            else if (rel == "conf/offsets_windows.conf")  dest = ec::self_dir() + "\\conf\\offsets_windows.conf";
            else continue;
            if (rel.find("..") != std::string::npos) continue;

            if (GetFileAttributesW(widen(dest).c_str()) != INVALID_FILE_ATTRIBUTES) {
                skipped.push_back(rel);
                continue;
            }
            if (rel.compare(0, 9, "snippets/") == 0) ensure_group_dirs(snipdir, rel.substr(9));
            std::ofstream w(dest, std::ios::binary | std::ios::trunc);
            if (w) { w.write(body.data(), (std::streamsize)body.size()); added++; }
            continue;
        }
    }

    char b[96];
    snprintf(b, sizeof b, "+ imported %d file%s", added, added == 1 ? "" : "s");
    ui_log("STATUS", b);
    if (!skipped.empty()) {
        snprintf(b, sizeof b, "! %d already present, left alone:", (int)skipped.size());
        ui_log("STATUS", b);
        for (const std::string& s : skipped) ui_log("STATUS", "    " + s);
    }
    // The page owns the settings, so they go back to it rather than being
    // applied here.
    if (!settings.empty()) ui_post("IMPORTSETTINGS:" + settings);
    push_snippets();
}
