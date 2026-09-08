// Blink -- teleport 7 studs BEHIND your facing, on a hotkey.
//
// Press G. Writes Primitive.Position, so it honours the arm gate: disarmed you
// get the dry-run line and nothing moves.
//
// Everything load-bearing here was already measured in your own snippets; this
// is the small version of it, not a new idea:
//
//   * BEHIND is +column3 of the rotation matrix. backdash/world.cpp's
//     dir_vector has forward as (-r[2], -r[8]) -- Roblox's LookVector is the
//     NEGATED third column -- so behind is (+r[2], +r[8]).
//   * FLATTENED to XZ, dropping r[5], for the same reason it does: looking at
//     the sky would otherwise send you up instead of back.
//   * VELOCITY IS ZEROED FIRST. clicktp/move.cpp: the other order leaves one
//     physics step at the new spot carrying the old speed, "enough to be flung
//     straight back off it".
//   * ONE WRITE IS NOT ENOUGH. The solver steps into any gap, so the position
//     is re-asserted for a ~100ms hold, SPINNING on fx_sleep(0) rather than
//     pacing -- measured there at zero drift across 434,252 writes, because the
//     engine cannot step faster than we rewrite.
//   * GROUNDED ONLY, and that is a known dead end rather than an oversight:
//     while the assembly is airborne the solver keeps its own transform and
//     puts you back on the original trajectory the moment the writes stop.
//     clicktp/move.cpp lists what was ruled out (EvaluateStateMachine,
//     PlatformStand, and the rest) so it is not re-measured here.
//
// extern "C" on fx_run is required, not decoration: the host resolves the entry
// point with GetProcAddress("fx_run"), and C++ mangling makes that miss.

#include <chrono>
#include <cmath>

const int   KEY      = 'E';
const float DIST     = 18.0f;   // studs
const int   HOLD_MS  = 100;    // re-assert window
const int   COOLDOWN = 250;    // ms, so a held key is not a hundred blinks

static uint64_t now_ms() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(
               steady_clock::now().time_since_epoch()).count();
}

extern "C" void fx_run() {
    uint64_t oPrim = fx_off("BasePart.Primitive");
    uint64_t oRot  = fx_off("Primitive.Rotation");
    uint64_t oPos  = fx_off("Primitive.Position");
    uint64_t oVel  = fx_off("Primitive.AssemblyLinearVelocity");
    if (oPrim == (uint64_t)-1 || oRot == (uint64_t)-1 || oPos == (uint64_t)-1) {
        fx_log("x missing an offset (BasePart.Primitive / Primitive.Rotation / .Position)");
        return;
    }

    fx_logf("blink ready - press %c to jump %.0f studs back%s",
            KEY, DIST, fx_armed() ? "" : "  (disarmed: dry-run only)");

    uint64_t last = 0;

    while (fx_running()) {
        // Key FIRST, gate second: fx_key_pressed consumes one press per call,
        // so testing fx_input_ok first would eat presses made while alt-tabbed
        // and lose the next real one. fx.h says so at the gate's definition.
        if (fx_key_pressed(KEY) && fx_input_ok()) {
            uint64_t t = now_ms();
            if (t - last < (uint64_t)COOLDOWN) { if (!fx_sleep(8)) break; continue; }

            uint64_t ch   = fx_character();
            uint64_t hrp  = ch ? fx_part(ch, "HumanoidRootPart") : 0;
            uint64_t prim = 0;
            if (hrp) {
                uint64_t p = fx_rd(hrp + oPrim);
                // Same plausibility bound backdash uses -- a stale root gives a
                // pointer-shaped number that is not a Primitive.
                if (p >= 0x10000 && p <= 0x7ff000000000ULL) prim = p;
            }
            if (!prim) { fx_log("! no character"); if (!fx_sleep(8)) break; continue; }

            float r[9];
            if (fx_rdblk(prim + oRot, r, 36) != 36) {
                fx_log("! could not read rotation");
                if (!fx_sleep(8)) break;
                continue;
            }
            // Behind, flattened to the ground plane.
            float bx = r[2], bz = r[8];
            float mag = std::sqrt(bx * bx + bz * bz);
            if (mag < 1e-6f) {
                fx_log("! facing is degenerate");
                if (!fx_sleep(8)) break;
                continue;
            }
            bx /= mag; bz /= mag;

            float3 from = fx_rf3(prim + oPos);
            float3 to{ from.x + bx * DIST, from.y, from.z + bz * DIST };

            last = t;
            fx_logf("blink %.1f,%.1f,%.1f -> %.1f,%.1f,%.1f",
                    from.x, from.y, from.z, to.x, to.y, to.z);

            // Hold it. Velocity before position, every pass.
            float3 zero{ 0, 0, 0 };
            uint64_t end = now_ms() + (uint64_t)HOLD_MS;
            while (fx_running() && now_ms() < end) {
                if (oVel != (uint64_t)-1) fx_wrblk(prim + oVel, &zero, 12);
                fx_wrblk(prim + oPos, &to, 12);
                if (!fx_sleep(0)) break;   // spin, do not pace
            }
        }
        if (!fx_sleep(8)) break;
    }
}
