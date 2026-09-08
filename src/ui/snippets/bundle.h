// bundle.h -- bundle export / import: one plain-text file carrying the
// snippets tree, the settings and the offsets conf, for handing a working
// setup to someone else. Split out of eclipse_ui.cpp; the format and the
// trust argument live with the code in bundle.cpp.
//
// Every function takes the snippets directory and the owner window rather
// than reading globals: the TU owns no state, and the caller owns which tree
// is live and which window the file dialog belongs to.
#pragma once

#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// `files` is the exact list the page ticked, relative to the snippets root;
// `settings` is the page's own k=v block, passed through untouched.
void do_export(HWND owner, const std::string& snipdir,
               const std::vector<std::string>& files, bool doSettings,
               bool offsets, const std::string& settings);
void do_import(HWND owner, const std::string& snipdir);
