// Watch snippets/ and re-send the list when anything changes on disk.
//
// Without this, editing a file in another editor -- or dropping one in, or
// making a folder -- shows nothing until the app is restarted. That is easy to
// hit and gives no clue what is wrong: the build compiles from disk, so it picks
// the change up immediately, while the sidebar and editor keep showing the old
// text. The two disagreeing silently is worse than either being stale.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <string>

#include "host.h"
#include "tree.h"
#include "watch.h"

static std::atomic<bool> g_snipwatch_run{false};
static HANDLE            g_snipwatch_thread = nullptr;
static std::string       g_snipwatch_dir;      // fixed at snipwatch_start

static DWORD WINAPI watch_snippets(LPVOID) {
    HANDLE h = FindFirstChangeNotificationW(
        widen(g_snipwatch_dir).c_str(), TRUE,    // TRUE: include subdirectories
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME |
        FILE_NOTIFY_CHANGE_DIR_NAME);
    if (h == INVALID_HANDLE_VALUE) return 0;

    // Notification-driven + polling fallback.
    // FindFirstChangeNotification occasionally misses a fast write or a
    // temp-file-rename save (some editors do that). Without a fallback the
    // sidebar goes stale until the app is restarted -- a real bug the user
    // hit repeatedly. The poll catches whatever the notification misses
    // within POLL_MS. This comment used to call push_snippets idempotent
    // because "the page's editor-reload gate short-circuits" -- true for the
    // EDITOR, false for the DOM: every identical push still re-rendered the
    // sidebar and tab strip, destroying the element under the pointer (hover
    // lost on a 2s beat, the drag rail flickering with the pointer still).
    // The page still drops byte-identical SNIPPETS bodies (app.js,
    // lastSnippetsBody -- the page-side twin of overlay.cpp's g_hud_last).
    // That guard is why the re-send was harmless to LOOK at; it was never
    // free, which is what the poll gate below is for.
    const DWORD POLL_MS = 2000;

    // The tree as of the last push. The POLL compares against it; the
    // NOTIFICATION does not, because a notification is positive evidence that
    // something changed and is not worth second-guessing with metadata.
    std::string pushed;

    while (g_snipwatch_run) {
        DWORD r = WaitForSingleObject(h, POLL_MS);
        bool notified = (r == WAIT_OBJECT_0);
        if (r == WAIT_OBJECT_0) {
            // Rearm BEFORE settling. A change-notification handle is not
            // reset by the wait -- only FindNextChangeNotification resets
            // it. Waiting on it again without rearming returns instantly
            // every time, so the settle loop below would spin forever.
            FindNextChangeNotification(h);
            // Settle burst: one save often raises several notifications; a
            // compile writes into the folder too. Absorbing them keeps
            // push_snippets from firing three or four times per save.
            while (g_snipwatch_run && WaitForSingleObject(h, 250) == WAIT_OBJECT_0)
                FindNextChangeNotification(h);
        }
        // A BLIND POLL ASKS THE CHEAP QUESTION FIRST. push_snippets is not a
        // listing: it opens every snippet, reads its whole text, JSON-escapes
        // it and ships the corpus to the page AND the in-game panel. Doing
        // that unconditionally every 2s cost ~200ms of CPU per beat (measured
        // 2026-09-06: two threads co-firing, ~9.5% of one core sustained), and
        // every byte of it was discarded again by the page's identical-body
        // guard. snip_tree_stamp answers from directory metadata alone --
        // enumeration, no file opened.
        //
        // The page-side drop stays: it still covers the pushes this cannot
        // see (a user action pushes without telling the watcher, so the next
        // poll finds a difference and pushes once more -- harmless, and the
        // safe direction to fail in).
        if (!g_snipwatch_run) break;
        std::string now = snip_tree_stamp(g_snipwatch_dir);
        if (notified || now != pushed) {
            pushed = now;
            push_snippets();
        }
    }
    FindCloseChangeNotification(h);
    return 0;
}

void snipwatch_start(const std::string& snipdir) {
    g_snipwatch_dir = snipdir;
    g_snipwatch_run = true;
    g_snipwatch_thread = CreateThread(nullptr, 0, watch_snippets, nullptr, 0, nullptr);
}

void snipwatch_stop() {
    g_snipwatch_run = false;
    if (g_snipwatch_thread) {
        // Its wait is bounded at POLL_MS (2000ms), 250ms inside the settle
        // burst, so it notices the cleared flag within one wait; 3000ms covers
        // the worst case with margin.
        WaitForSingleObject(g_snipwatch_thread, 3000);
        CloseHandle(g_snipwatch_thread);
        g_snipwatch_thread = nullptr;
    }
}
