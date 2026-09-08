#include "bd.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

static double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static double g_last_fire = -1e9;

double dash_cooldown_left() {
    double left = g.cooldown - (now_s() - g_last_fire);
    return left > 0.0 ? left : 0.0;
}

// Decide. Cheap, and stays on the main loop so the refusal reason appears the
// instant you press the key rather than after a thread has been started.
bool dash_begin(const Ptrs& p, DashPlan* plan, std::string* why) {
    if (!p.ok) { *why = "no character"; return false; }

    double left = dash_cooldown_left();
    if (left > 0.0) {
        char b[48];
        snprintf(b, sizeof b, "cooldown %.1fs", left);
        *why = b;
        return false;
    }
    if (!sound_gate_open()) { *why = "sound gate shut"; return false; }
    if (!hp_gate_open())    { *why = "hp gate shut";    return false; }

    float rot[9];
    if (!read_rot(p.primitive, rot)) { *why = "no rotation"; return false; }
    if (!dir_vector(g.dash_dir, rot, &plan->bx, &plan->bz, &plan->by, &plan->vertical)) {
        *why = "rotation ~0";
        return false;
    }

    plan->vel_off = fx_off("Primitive.AssemblyLinearVelocity");
    if (plan->vel_off == (uint64_t)-1) { *why = "no velocity offset"; return false; }
    plan->jump_off  = g.do_jump ? fx_off("Humanoid.Jump") : (uint64_t)-1;
    plan->primitive = p.primitive;
    plan->humanoid  = p.humanoid;
    plan->gen       = fx_gen();
    // Snapshotted, not read live. The panel can now change these while a burst
    // is in flight, and a burst that changed length or rate halfway through
    // would be neither the old setting nor the new one.
    plan->hold_secs  = g.hold_secs;
    plan->hold_hz    = g.hold_hz;
    plan->do_jump    = g.do_jump;
    plan->jump_delay = g.jump_delay;

    float ratio = hp_ratio(p.humanoid);
    plan->power = g.power_min + (g.power_max - g.power_min) * ratio;
    g_last_fire = now_s();

    char b[64];
    snprintf(b, sizeof b, "dash (v %.0f, hp %.0f%%)", plan->power, ratio * 100.0);
    *why = b;
    return true;
}

// Hold the velocity. Once is not enough -- the engine recomputes velocity every
// frame, so a single write is overwritten before it moves anything. Same reason
// noclip rewrites its bit.
//
// Runs on a worker, but a JOINED one: the snippet's DLL is unloaded when fx_run
// returns, and a detached thread still executing this code would be running
// freed memory.
void dash_run(const DashPlan& plan) {
    if (plan.do_jump && plan.jump_off != (uint64_t)-1) {
        uint8_t one = 1;
        double until = now_s() + 0.08;
        while (now_s() < until && fx_running()) {
            if (fx_gen() != plan.gen) return;   // died: humanoid is freed
            fx_wrblk(plan.humanoid + plan.jump_off, &one, 1);
            if (!fx_sleep(8)) return;
        }
        if (!fx_sleep((unsigned)(plan.jump_delay * 1000.0))) return;
    }

    double end = now_s() + plan.hold_secs;
    while (now_s() < end && fx_running()) {
        // Cheap, and it is the difference between ending a dash and writing
        // 12 bytes a millisecond into a heap block the client has since
        // handed to something else.
        if (fx_gen() != plan.gen) return;
        float cur[3] = { 0, 0, 0 };
        fx_rdblk(plan.primitive + plan.vel_off, cur, 12);
        float v[3];
        if (plan.vertical) { v[0] = cur[0];                    v[1] = plan.by * (float)plan.power; v[2] = cur[2]; }
        else               { v[0] = plan.bx * (float)plan.power; v[1] = cur[1];                    v[2] = plan.bz * (float)plan.power; }
        fx_wrblk(plan.primitive + plan.vel_off, v, 12);
        if (!fx_pace((unsigned)plan.hold_hz)) break;
    }
}
