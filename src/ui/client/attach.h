// attach.h -- the attach/identity layer: who we are attached to, the
// multi-instance state, and the attach/detach transitions. Split out of
// eclipse_ui.cpp; the semantics live with the code in attach.cpp.
//
// This TU OWNS the attach flags, the serialising mutex, the identity fields
// and the place-title cache -- nothing else can reach them. The message
// handlers and the liveness watcher in eclipse_ui.cpp drive it through these
// functions; state comes back out through push_state (host.h), which reads
// the identity through its getters.
#pragma once

#include <cstdint>
#include <string>

// The attach entry point. quiet = poll-driven retry; force = the user's
// Attach button; target_pid = a specific client picked from the modal.
void do_attach(bool quiet = false, bool force = false, uint32_t target_pid = 0);

// Manual "Refresh offsets" from Settings; runs on its own thread.
void refresh_offsets_async();

// One tick of the liveness watcher's attach work: attach when a client turns
// up, detach when it exits, re-resolve the DataModel, refresh the picker.
void attach_poll();

// The state halves of the DETACH / REATTACH / ATTACH_PID: messages; telling
// the page stays with the handlers in eclipse_ui.cpp.
void attach_user_detach();
void attach_reattach();
void attach_to_pid(uint32_t pid);

// One full attach look has happened. Consulted by reveal_window and
// hud_tick's no-client valve; see the flag's comment in attach.cpp.
bool attach_probed();

// The in-game panel header's identity. refresh_identity re-derives on a
// pid/DataModel change; the getters feed push_state.
void refresh_identity();
// The retry, with its own push. Call it from any periodic tick: it does
// nothing once the name is resolved, throttles itself while it is not, and
// pushes state only on the tick the name actually arrives. Called from BOTH
// the 1 Hz attach poll and the UI's ~30 Hz timer -- the poll alone left a
// visible beat of "pid N" after joining, because that is exactly the window
// where LocalPlayer does not exist yet.
void identity_tick();
std::string ident_user();
std::string ident_place();
// A resolved place title: cache it, and update the identity if it names the
// place we are in (the place-name lookup calls this from its worker thread).
void place_title_remember(uint64_t place_id, const std::string& name);
