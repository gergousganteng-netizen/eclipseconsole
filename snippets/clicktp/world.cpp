#include "ctp.h"
// WIN32_LEAN_AND_MEAN: windows.h is the most expensive header this project
// includes -- measured 2026-09-07 on the bundled g++, +2222ms over an empty
// TU at -O2, against +520 for <fstream>, +365 for <atomic>, +275 for
// <thread> and nothing at all for <string>/<chrono>/<cstdio> (fx.h already
// pulls those in). LEAN drops the parts nothing here uses -- OLE, RPC,
// winsock, the multimedia and shell APIs -- and measured 319-491ms back per
// file. Delete it only if something here starts needing one of those.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstring>

// ---- the Roblox window ------------------------------------------------------
// fx_screen reports CLIENT-AREA pixels, so the cursor has to be converted into
// the same space before the two can be compared.

static BOOL CALLBACK win_probe(HWND h, LPARAM lp) {
    struct Ctx { DWORD pid; HWND hwnd; int area; } *c = (Ctx*)lp;
    if (!IsWindowVisible(h)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid) return TRUE;
    RECT r;
    if (!GetWindowRect(h, &r)) return TRUE;
    int a = (r.right - r.left) * (r.bottom - r.top);
    if (a > c->area) { c->area = a; c->hwnd = h; }
    return TRUE;
}

bool cursor_px(float* x, float* y) {
    DWORD pid = (DWORD)fx_pid();
    if (!pid) return false;
    struct { DWORD pid; HWND hwnd; int area; } c = { pid, nullptr, 0 };
    EnumWindows(win_probe, (LPARAM)&c);
    if (!c.hwnd) return false;

    POINT p;
    if (!GetCursorPos(&p)) return false;
    if (!ScreenToClient(c.hwnd, &p)) return false;
    *x = (float)p.x;
    *y = (float)p.y;
    return true;
}

// ---- the character ----------------------------------------------------------

bool refresh_ptrs(Ptrs* out) {
    Ptrs p;
    p.character = fx_character();
    if (!p.character) { *out = p; return false; }
    p.humanoid = fx_part(p.character, "Humanoid");
    p.hrp      = fx_part(p.character, "HumanoidRootPart");
    if (!p.hrp) p.hrp = fx_part(p.character, "Torso");
    if (!p.hrp) p.hrp = fx_part(p.character, "UpperTorso");

    uint64_t oPrim = fx_off("BasePart.Primitive");
    if (p.hrp && oPrim != (uint64_t)-1) {
        uint64_t prim = fx_rd(p.hrp + oPrim);
        if (prim >= 0x10000 && prim <= 0x7ff000000000ULL) p.primitive = prim;
    }
    p.ok = p.hrp && p.primitive;
    *out = p;
    return p.ok;
}

// ---- the cursor ray ---------------------------------------------------------

static uint64_t find_camera() {
    uint64_t ws = fx_service("Workspace");
    if (!ws) return 0;
    uint64_t n = fx_nchildren(ws);
    if (n > 4096) n = 4096;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t c = fx_child(ws, (int)i);
        if (c && strcmp(fx_class(c), "Camera") == 0) return c;
    }
    return 0;
}

// The ray through a client-area pixel.
//
// IT DOES NOT USE Camera.FieldOfView, and that is deliberate: that is one of the
// conf fields whose name does not mean what it suggests, and a projection built
// on a misread FOV is wrong by a factor nobody notices until the destination is
// consistently short. Instead the scale is CALIBRATED against fx_screen, which
// uses the engine's own world-to-clip matrix and is therefore right by
// construction -- project two points whose camera-space offsets are known, and
// read the scale straight off where they land.
//
// Then it CHECKS ITSELF: a point down the finished ray is projected back, and
// must land on the pixel that was asked for. If it does not, this refuses rather
// than teleporting you somewhere plausible-looking and wrong. That check has
// teeth -- get the forward sign or the aspect handling wrong and it fails by
// hundreds of pixels.
bool camera_ray(float sx, float sy, float3* origin, float3* dir, std::string* why) {
    uint64_t cam = find_camera();
    if (!cam) { *why = "no camera"; return false; }

    uint64_t oRot = fx_off("Camera.Rotation"), oPos = fx_off("Camera.Position");
    if (oRot == (uint64_t)-1 || oPos == (uint64_t)-1) { *why = "no camera offsets"; return false; }

    float R[9];
    if (fx_rdblk(cam + oRot, R, 36) != 36) { *why = "camera unreadable"; return false; }
    float3 eye = fx_rf3(cam + oPos);

    // Columns are the basis vectors, and column 2 is BACK -- the same layout
    // lockon and backdash already rely on.
    float3 right{ R[0], R[3], R[6] };
    float3 up   { R[1], R[4], R[7] };
    float3 fwd  { -R[2], -R[5], -R[8] };

    float W = 0, H = 0;
    if (!fx_viewport(&W, &H) || W < 1 || H < 1) { *why = "no viewport"; return false; }

    // Calibration probes at a fixed depth. 10 studs of offset at 100 of depth
    // is well inside any sane frustum, so both land on screen.
    const float D = 100.0f, O = 10.0f;
    float cxp = 0, cyp = 0, rxp = 0, ryp = 0, uxp = 0, uyp = 0;
    float3 pc = v_add(eye, v_mul(fwd, D));
    if (!fx_screen(pc, &cxp, &cyp)) { *why = "camera basis points backwards"; return false; }
    if (!fx_screen(v_add(pc, v_mul(right, O)), &rxp, &ryp)) { *why = "probe off frame"; return false; }
    if (!fx_screen(v_add(pc, v_mul(up,    O)), &uxp, &uyp)) { *why = "probe off frame"; return false; }

    // ndc of each probe. The centre probe should be ~0,0; the others give the
    // studs-per-ndc scale on each axis directly.
    float ndc_rx = 2.0f * rxp / W - 1.0f;
    float ndc_uy = 1.0f - 2.0f * uyp / H;
    if (fabsf(ndc_rx) < 1e-4f || fabsf(ndc_uy) < 1e-4f) { *why = "degenerate projection"; return false; }
    float kx = (O / D) / ndc_rx;
    float ky = (O / D) / ndc_uy;

    float ndx = 2.0f * sx / W - 1.0f;
    float ndy = 1.0f - 2.0f * sy / H;
    float3 d = v_norm(v_add(fwd, v_add(v_mul(right, ndx * kx), v_mul(up, ndy * ky))));
    if (v_len(d) < 0.5f) { *why = "degenerate ray"; return false; }

    // Self-check: does a point on the ray project back onto the pixel we asked
    // about? Two pixels of slack for the float round trip.
    float bx = 0, by = 0;
    if (!fx_screen(v_add(eye, v_mul(d, D)), &bx, &by)) { *why = "ray check failed to project"; return false; }
    float err = sqrtf((bx - sx) * (bx - sx) + (by - sy) * (by - sy));
    if (err > 2.0f) {
        char b[96];
        snprintf(b, sizeof b, "ray is %.0f px off the cursor", err);
        *why = b;
        return false;
    }

    *origin = eye;
    *dir = d;
    return true;
}

// ---- the body ---------------------------------------------------------------

bool measure_body(const Ptrs& p, Body* out) {
    Body b;
    if (!p.ok) { *out = b; return false; }

    float3 root{};
    if (!fx_pos(p.hrp, &root)) { *out = b; return false; }

    uint64_t parts[64];
    int n = fx_parts(parts, 64);
    if (n <= 0) { *out = b; return false; }

    uint64_t oPrim = fx_off("BasePart.Primitive"), oSize = fx_off("Primitive.Size");
    uint64_t oRot  = fx_off("Primitive.Rotation");
    if (oPrim == (uint64_t)-1 || oSize == (uint64_t)-1 || oRot == (uint64_t)-1) { *out = b; return false; }

    float3 lo{ 1e9f, 1e9f, 1e9f }, hi{ -1e9f, -1e9f, -1e9f };
    int used = 0;
    for (int i = 0; i < n; i++) {
        float3 c{};
        if (!fx_pos(parts[i], &c)) continue;
        uint64_t prim = fx_rd(parts[i] + oPrim);
        if (prim < 0x10000 || prim > 0x7ff000000000ULL) continue;
        float3 sz{}; float rot[9];
        if (fx_rdblk(prim + oSize, &sz, 12) != 12) continue;
        if (fx_rdblk(prim + oRot, rot, 36) != 36) continue;
        if (!(sz.x > 0 && sz.y > 0 && sz.z > 0)) continue;

        // World-space half extent of a rotated box: |R| times the half size.
        float hx = 0.5f * (fabsf(rot[0])*sz.x + fabsf(rot[1])*sz.y + fabsf(rot[2])*sz.z);
        float hy = 0.5f * (fabsf(rot[3])*sz.x + fabsf(rot[4])*sz.y + fabsf(rot[5])*sz.z);
        float hz = 0.5f * (fabsf(rot[6])*sz.x + fabsf(rot[7])*sz.y + fabsf(rot[8])*sz.z);

        if (c.x - hx < lo.x) lo.x = c.x - hx;
        if (c.y - hy < lo.y) lo.y = c.y - hy;
        if (c.z - hz < lo.z) lo.z = c.z - hz;
        if (c.x + hx > hi.x) hi.x = c.x + hx;
        if (c.y + hy > hi.y) hi.y = c.y + hy;
        if (c.z + hz > hi.z) hi.z = c.z + hz;
        used++;
    }
    if (used == 0) { *out = b; return false; }

    b.half = float3{ (hi.x-lo.x)*0.5f, (hi.y-lo.y)*0.5f, (hi.z-lo.z)*0.5f };
    float3 mid{ (hi.x+lo.x)*0.5f, (hi.y+lo.y)*0.5f, (hi.z+lo.z)*0.5f };
    b.centre_off = v_sub(mid, root);
    b.foot_drop  = root.y - lo.y;
    b.ok = true;
    *out = b;
    return true;
}

// Is there room for the body with its ROOT at dest?
//
// Six rays inward at the body's centre, one per face. A ray that stops INSIDE
// the box means something is occupying the space. This is deliberately built on
// fx_raycast rather than a new host overlap test: six rays cost ~0.02 ms and
// answer the question well enough to refuse a bad spot.
//
// Conservative in the safe direction -- the box is the body's full extent, so a
// destination that only just fits may be refused. That beats materialising you
// inside a wall, which is one of the two ways a teleport gets undone.
bool room_at(const Body& b, float3 dest, float3* blocker) {
    if (!b.ok) return true;                       // cannot measure -> do not block
    float3 c = v_add(dest, b.centre_off);
    const float dirs[6][3] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
    float half[3] = { b.half.x, b.half.y, b.half.z };

    for (int i = 0; i < 6; i++) {
        float3 d{ dirs[i][0], dirs[i][1], dirs[i][2] };
        float reach = fabsf(d.x)*half[0] + fabsf(d.y)*half[1] + fabsf(d.z)*half[2];
        float start = reach + 6.0f;
        float3 from = v_sub(c, v_mul(d, start));
        float3 hit{};
        if (!fx_raycast(from, d, start, &hit, nullptr)) continue;
        // Inside the destination box?
        if (fabsf(hit.x - c.x) <= half[0] + 0.05f &&
            fabsf(hit.y - c.y) <= half[1] + 0.05f &&
            fabsf(hit.z - c.z) <= half[2] + 0.05f) {
            if (blocker) *blocker = hit;
            return false;
        }
    }
    return true;
}
