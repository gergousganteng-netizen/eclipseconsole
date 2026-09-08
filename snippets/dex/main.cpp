// dex -- a live Instance-tree explorer for the running client.
//
// Roblox Studio's Explorer, read from outside the process: navigate the tree,
// select a node, and see every offset the conf knows for its class with the
// live bytes at each one. That last part is the reason this exists rather than
// just being a nicer inspect.cpp -- it is the view you want when hunting an
// offset, and Studio cannot show it.
//
// READ ONLY. It never writes, never hooks, never touches the VM. There WAS a
// write path here -- a Value editor, a typed field write and a freeze list that
// rewrote held addresses every tick -- and it was taken out on purpose once this
// became a tool for reading how games work. Do not put it back without deciding
// you want it; see the note at the bottom of this comment.
//
// WHY THIS ONE IS DIFFERENT FROM THE LAST DEX
//
// There was a dex.cpp in this project. It was 75 KB, and it was abandoned as
// "buggy UI, never produced the result I wanted, very slow, weird logic". Three
// design rules here come straight out of that post-mortem:
//
//   1. THE SNIPPET OWNS ALL STATE. Which nodes are expanded, what is selected,
//      what the rows are -- all of it lives in C++, and the page is a dumb
//      renderer handed a finished list of rows. "Weird logic" is what happens
//      when tree state is split across two languages that disagree about it.
//
//   2. NOTHING WALKS THE WHOLE TREE. Expansion is lazy: a node costs one
//      fx_nchildren to know whether to draw a twisty, and its children are only
//      read when it is actually open. The portkit calls this _cheap_kids and it
//      is the single most important decision in the design. Search is the one
//      exception and it is hard-bounded.
//
//   3. NO SCANNING THE HEAP. The old one had a whole-address-space value
//      scanner and that is where "very slow" came from. Cheat Engine already
//      does that properly. What this has instead sweeps only the bytes of
//      instances the tree already knows about, so every hit arrives named as
//      instance+offset -- see the scanning section of dex.h.
//
// The write path stays out for a reason worth keeping in front of you: a field
// editor needs the conf to be right about where a field lives, and when it is
// wrong you are putting four bytes through the middle of some other member.
// That is worth having deliberately, in a tool built to do it -- not as a side
// effect of a browser you opened to read something.

#include "dex.h"

#include <cstdarg>
#include <cstdio>
#include <string>

static void lg(const char* fmt, ...) {
    char b[512];
    va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    fx_log(b);
}

extern "C" void fx_run() {
    // NOT an "are you in a game" check, and the message must not pretend it is.
    // The client holds a DataModel at the MENU too -- named LuaApp, replaced on
    // join and leave -- so a non-zero dm says nothing about being in a place,
    // and the menu tree is a perfectly legitimate thing to open this on. Zero
    // means the console has no DataModel at all: not attached, or the pointer
    // chain through FakeDataModel.Pointer did not resolve. antifling.cpp gates
    // on fx_place() instead, which IS the in-a-game question.
    if (!fx_dm()) {
        lg("no DataModel -- check the console is attached.");
        return;
    }

    State st;
    st.root = fx_dm();
    st.expanded.insert(st.root);       // the root is open on arrival
    st.sel = st.root;

    fx_window_open("Dex -- instance explorer", 1100, 720);
    std::string page = std::string(fx_snippets_dir()) + "\\dex\\ui.html";
    fx_window_navigate(page.c_str());

    lg("dex: open. Click a twisty to expand, a row to inspect.");
    lg("  the tree is read lazily -- nothing walks the whole DataModel.");

    unsigned gen = fx_gen();
    std::string last;          // only push when something actually changed
    int since_refresh = 0;

    while (fx_running()) {
        // The world changed underneath us: every cached pointer is suspect, so
        // drop the expansion set rather than draw a tree of freed nodes.
        if (fx_gen() != gen) {
            gen = fx_gen();
            uint64_t dm = fx_dm();
            if (dm && dm != st.root) {
                st.root = dm;
                st.expanded.clear();
                st.expanded.insert(st.root);
                st.sel = st.root;
                lg("  world changed -- tree reset");
            }
        }

        // Drain every pending message before redrawing, so a burst of clicks
        // costs one repaint rather than one each.
        bool dirty = false;
        for (;;) {
            const char* m = fx_window_get_msg();
            if (!m || !*m) break;
            if (handle_msg(st, m)) dirty = true;
        }

        // A periodic redraw as well, because the VALUES in the detail pane are
        // live -- a health field should tick down while you watch it. Two
        // separate rates on purpose: input is polled every tick so a click is
        // picked up within one, values refresh about twice a second because
        // that is as fast as anyone reads them.
        if (++since_refresh >= 15) { since_refresh = 0; dirty = true; }

        if (dirty) {
            std::string js = build_js(st);
            if (js != last) { fx_window_eval(js.c_str()); last = js; }
        }

        // 100 ms here meant a click waited up to a tenth of a second to be even
        // NOTICED, on top of the redraw -- which read as the window freezing.
        if (!fx_sleep(33)) break;
    }

    fx_window_close();
    lg("dex: closed.");
}
