// conf.h -- parsing the offsets conf file. Split out of eclipse_console.cpp; the
// offset table it fills lives in offsets.cpp, the version it extracts lives in
// the attach State. One function, called by attach() and preload_offsets().
#pragma once
#include <cstddef>
#include <string>

// Load the conf at `path`: clear and repopulate the offset table, pull the
// client version out of the header comment, and return the number of offsets.
size_t load_offsets(const std::string& path);
