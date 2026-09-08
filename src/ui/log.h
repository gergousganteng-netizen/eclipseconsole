// log.h -- the UI log funnel: the coalescing queue, the sent history and
// the optional file. Split out of eclipse_ui.cpp; the design notes (why lines
// are coalesced, why the file has its own lock) live with the code in
// log.cpp.
//
// This TU OWNS the queue, the history and the file stream -- nothing else can
// reach them. The host side stays in eclipse_ui.cpp behind three functions in
// host.h: page presence, the UI-thread hop that runs flush_logs, and
// delivery of one funnel message to BOTH views.
#pragma once

#include <string>

void ui_log(const std::string& type, const std::string& text);
void set_log_file(bool on);
void flush_logs();                 // UI thread only: the dispatch target
std::string log_history_json();    // the backlog, in flush_logs' wire format
// LOGCLEAR's state half: drops the history so the clear sticks across
// replays. Telling both views stays with the handler in eclipse_ui.cpp.
void log_clear_history();
