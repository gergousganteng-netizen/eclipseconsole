// tree.h -- the snippet file tree: page-relative paths and their
// validation, the .order file, and the directory scans behind the sidebar and
// the in-game panel's list. Split out of eclipse_ui.cpp; the semantics live
// with the code in tree.cpp.
//
// Every function takes the snippets directory rather than reading a global:
// the TU owns no state, and the caller owns which tree is live.
#pragma once

#include <string>
#include <vector>

std::string order_file(const std::string& snipdir);
bool safe_rel(const std::string& p);
std::string parent_of(const std::string& rel);
std::string leaf_of(const std::string& rel);
std::string win_path(const std::string& rel);
void ensure_group_dirs(const std::string& snipdir, const std::string& rel);
std::vector<std::string> read_order(const std::string& snipdir);
void scan_cpp(const std::string& dir, const std::string& prefix,
              std::vector<std::string>& out);
std::vector<std::string> list_groups(const std::string& snipdir);

// A fingerprint of the tree from directory metadata alone -- no file is opened.
// The watcher's blind poll compares it instead of rebuilding and shipping the
// whole corpus to find out nothing changed; see the note at the definition for
// what it covers and why missing a change is the direction that matters.
std::string snip_tree_stamp(const std::string& snipdir);
