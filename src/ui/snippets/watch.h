// watch.h -- the on-disk snippet watcher. Split out of eclipse_ui.cpp;
// why it exists and the notification-plus-poll shape live with the code in
// watch.cpp.
//
// The TU owns its thread handle and run flag. It is NOT the liveness watcher:
// that one (watch_client in eclipse_ui.cpp) is welded to the attach state and
// stays there, with its own flag.
#pragma once

#include <string>

void snipwatch_start(const std::string& snipdir);
void snipwatch_stop();
