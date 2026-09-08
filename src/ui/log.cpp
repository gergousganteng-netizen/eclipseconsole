// ---- the UI log funnel --------------------------------------------------
// The coalescing queue, the sent history and the optional file, split out of
// eclipse_ui.cpp. This TU owns that state outright; the host facts it needs
// (is there a page, the UI-thread hop, delivery to both views) come in
// through ui_page_live / ui_dispatch_log_flush / ui_deliver_logmsg in
// host.h.

#include <atomic>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

#include "host.h"
#include "log.h"

// Log lines are COALESCED rather than posted one at a time.
//
// Per-line dispatch+eval is fine at human rates and collapses at machine
// ones: a disarmed snippet writing in a loop logs every intended write, each
// costing a queued UI item, a script eval, a DOM insert and a reflow -- and
// the worker-thread producer pays none of that, so it outruns the consumer
// permanently and the dispatch queue grows without bound. The queue is the
// real bug: STOP and ARM arrive through the SAME pump, so stopping a
// spamming snippet appeared to hang -- the click sat thousands of log lines
// back while the snippet kept logging.
//
// Two bounds: at most ONE flush in flight, so a click is at worst one flush
// from being handled; and the backlog is capped at the page's LOG_MAX --
// rendering lines only to be discarded is work nobody can see.
static std::mutex              g_logmux;
static std::deque<std::string> g_logq;           // "lvl|text", oldest first
static size_t                  g_logdrop = 0;    // lines the cap discarded
static std::atomic<bool>       g_logflush{false};
static const size_t            LOG_Q_MAX = 500;  // matches the page's LOG_MAX
// What has already been SENT, kept so a view that opens later can be handed
// the same history rather than starting blank. Distinct from g_logq, which is
// the pending batch and is drained on every flush. Same cap: the page keeps
// LOG_MAX lines, so sending more would only be trimmed at the far end.
static std::deque<std::string> g_loghist;        // "lvl|text", oldest first
static const size_t            LOG_HIST_MAX = 500;

void flush_logs() {
    // Cleared BEFORE the drain, never after. A producer appending in the window
    // between drain and clear would otherwise see a flush still pending, skip
    // scheduling one, and leave its line stranded until something else happened
    // to log. Clearing first can only ever cost one extra empty flush.
    g_logflush = false;

    std::deque<std::string> batch;
    size_t dropped = 0;
    {
        std::lock_guard<std::mutex> lk(g_logmux);
        batch.swap(g_logq);
        dropped = g_logdrop;
        g_logdrop = 0;
    }
    if (batch.empty() && !dropped) return;

    std::ostringstream j;
    j << "[";
    bool first = true;
    // First, because the lines it stands for are the oldest ones in the batch.
    // Said out loud rather than silently: a dry run that quietly skipped writes
    // would be lying about exactly the thing it exists to show.
    if (dropped) {
        char b[96];
        snprintf(b, sizeof b, "! %zu lines dropped - logging faster than the UI can draw", dropped);
        j << "{\"l\":\"warn\",\"t\":\"" << json_escape(b) << "\"}";
        first = false;
    }
    for (const std::string& e : batch) {
        size_t bar = e.find('|');
        std::string lvl = (bar == std::string::npos) ? "info" : e.substr(0, bar);
        std::string txt = (bar == std::string::npos) ? e     : e.substr(bar + 1);
        if (!first) j << ",";
        first = false;
        j << "{\"l\":\"" << json_escape(lvl) << "\",\"t\":\"" << json_escape(txt) << "\"}";
    }
    j << "]";
    // The panel is a view onto the same lines, not a second log. One funnel,
    // so the two can never show different histories.
    ui_deliver_logmsg("LOGS:" + j.str());

    // Keep the same lines for a page that has not loaded yet. A view opened
    // later -- the app frame on first entry into overlay mode -- would
    // otherwise start blank and only fill from the next line logged, which
    // reads as "the log did not transfer".
    {
        std::lock_guard<std::mutex> lk(g_logmux);
        for (const std::string& e : batch) {
            g_loghist.push_back(e);
            if (g_loghist.size() > LOG_HIST_MAX) g_loghist.pop_front();
        }
    }
}

// The backlog, in the wire format flush_logs uses. Built here rather than
// remembered as JSON so there is one encoder: a second one would drift the
// day a level or a field changed.
std::string log_history_json() {
    std::deque<std::string> hist;
    {
        std::lock_guard<std::mutex> lk(g_logmux);
        hist = g_loghist;
    }
    std::ostringstream j;
    j << "[";
    bool first = true;
    for (const std::string& e : hist) {
        size_t bar = e.find('|');
        std::string lvl = (bar == std::string::npos) ? "info" : e.substr(0, bar);
        std::string txt = (bar == std::string::npos) ? e     : e.substr(bar + 1);
        if (!first) j << ",";
        first = false;
        j << "{\"l\":\"" << json_escape(lvl) << "\",\"t\":\"" << json_escape(txt) << "\"}";
    }
    j << "]";
    return j.str();
}

// ---- optional log file ------------------------------------------------------
// Settings -> Console -> "Also write the log to a file". Opened in append mode
// and held open, because a snippet can log thousands of lines and reopening
// per line would dominate the cost of logging.
//
// Written from ui_log, which is called from ANY thread -- the snippet's, the
// watcher's, the attach poller's -- so the stream needs its own lock. It
// cannot borrow g_logmux: that one is held while the UI queue is manipulated,
// and file I/O under it would put a disk write on that path.
static std::mutex  g_logfile_mux;
static std::ofstream g_logfile;
void set_log_file(bool on) {
    std::lock_guard<std::mutex> lk(g_logfile_mux);
    if (!on) { if (g_logfile.is_open()) g_logfile.close(); return; }
    if (g_logfile.is_open()) return;
    g_logfile.open(appdata_home() + "\\console.log", std::ios::app);
}

void ui_log(const std::string& type, const std::string& text) {
    std::string lvl = "info";
    if (type == "ERROR") lvl = "err";
    else {
        size_t i = text.find_first_not_of(" ");
        char c = (i == std::string::npos) ? 0 : text[i];
        if      (c == 'x') lvl = "err";
        else if (c == '!') lvl = "warn";
        else if (c == '+') lvl = "ok";
    }
    std::string t = text;
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
    if (t.empty()) return;

    // Before the g_wv check, deliberately. Startup diagnostics are logged
    // before the WebView exists, and those are exactly the lines worth having
    // in a file when the window never appears.
    {
        std::lock_guard<std::mutex> lk(g_logfile_mux);
        if (g_logfile.is_open()) g_logfile << lvl << "  " << t << "\n" << std::flush;
    }

    if (!ui_page_live()) return;

    {
        std::lock_guard<std::mutex> lk(g_logmux);
        while (g_logq.size() >= LOG_Q_MAX) { g_logq.pop_front(); g_logdrop++; }
        g_logq.push_back(lvl + "|" + t);
    }
    // Only the thread that flips false->true schedules the flush, so however
    // many threads log and however fast, one flush is queued at a time.
    if (!g_logflush.exchange(true)) ui_dispatch_log_flush();
}

// LOGCLEAR's state half: the funnel's memory. The handler in eclipse_ui.cpp
// tells both views; dropping the history here is what makes the clear stick
// across replays.
void log_clear_history() {
    std::lock_guard<std::mutex> lk(g_logmux);
    g_loghist.clear();
}
