// offsets_fetch.h -- fetching a matching offsets conf for the attached
// client's version. Split out of eclipse_ui.cpp; the guards (declared-version
// check, plausible-count floor, temp-file write) live with the code in
// offsets_fetch.cpp.
#pragma once

#include <cstdint>
#include <string>

int fetch_offsets_for(const std::string& version);
// `conf` is the path the caller attaches with (eclipse_ui.cpp's g_conf); a
// parameter rather than a global so this TU owns no state.
void apply_fetched_offsets(const std::string& version, uint32_t pid,
                           int n, bool was_stale, const std::string& conf);
