// trash.h -- the snippet trash. Split out of eclipse_ui.cpp; the semantics
// (flat directory, the index, why restore needs it) live with the code in
// trash.cpp.
//
// Every function takes the snippets directory rather than reading a global:
// the TU owns no state, and the caller owns which tree is live.
#pragma once

#include <string>
#include <utility>
#include <vector>

std::string trash_dir(const std::string& snipdir);
std::string flatten_name(const std::string& rel);
std::vector<std::pair<std::string, std::string>> trash_load_index(const std::string& snipdir);
void trash_remember(const std::string& snipdir, const std::string& flat, const std::string& original);
void trash_forget(const std::string& snipdir, const std::string& flat);
bool safe_trash_name(const std::string& n);
void push_trash(const std::string& snipdir);
