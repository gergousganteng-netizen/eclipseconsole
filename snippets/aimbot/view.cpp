#include "aim.h"
#include <windows.h>

// The view matrix read and the perspective divide moved into the host as
// fx_screen. The reason they existed is unchanged and worth keeping: Roblox's
// OWN world-to-clip matrix is what the engine renders with, and rebuilding a
// projection from Camera.Rotation + FieldOfView + ViewportSize disagrees with
// the real render in any game that touches the camera pipeline from a script.
// Target selection ("closest to crosshair") then lands on players who visually
// were NOT closest.
//
// fx_screen keeps that and adds a cache: the matrix is re-read at most once a
// millisecond, so projecting every bone of every player costs one read rather
// than one per point.

// Find the attached Roblox client's window on the monitor. Multi-instance-
// safe: filtered by fx_pid so we always get OUR client, not "any Roblox".
static BOOL CALLBACK rbx_win_probe(HWND h, LPARAM lp) {
    struct Ctx { DWORD pid; HWND hwnd; int area; } *ctx = (Ctx*)lp;
    if (!IsWindowVisible(h)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != ctx->pid) return TRUE;
    RECT r;
    if (!GetWindowRect(h, &r)) return TRUE;
    int a = (r.right - r.left) * (r.bottom - r.top);
    if (a > ctx->area) { ctx->area = a; ctx->hwnd = h; }
    return TRUE;
}

bool get_rbx_window(int* x, int* y, int* w, int* h) {
    DWORD pid = (DWORD)fx_pid();
    if (!pid) return false;
    struct { DWORD pid; HWND hwnd; int area; } ctx = { pid, nullptr, 0 };
    EnumWindows(rbx_win_probe, (LPARAM)&ctx);
    if (!ctx.hwnd) return false;
    RECT client;
    if (!GetClientRect(ctx.hwnd, &client)) return false;
    POINT tl = { client.left, client.top };
    if (!ClientToScreen(ctx.hwnd, &tl)) return false;
    *x = tl.x;
    *y = tl.y;
    *w = client.right - client.left;
    *h = client.bottom - client.top;
    return *w > 0 && *h > 0;
}

uint64_t find_camera() {
    uint64_t ws = fx_service("Workspace");
    if (!ws) return 0;
    uint64_t n = fx_nchildren(ws);
    for (uint64_t i = 0; i < n; i++) {
        uint64_t c = fx_child(ws, (int)i);
        if (c && strcmp(fx_class(c), "Camera") == 0) return c;
    }
    return 0;
}

// Rotation is 9 floats at 0xd8, Position 3 floats at 0xfc, and 0xd8+36 == 0xfc
// exactly -- one CFrame the dumper split into two names. The basis vectors are
// the COLUMNS: right = (R0,R3,R6), up = (R1,R4,R7), back = (R2,R5,R8).
// Confirmed on a live camera, where col0 read y = 0.000 exactly, which is what a
// right vector must look like when the camera is not rolled.
bool camera_basis(float R[9], float3* pos, float* fov, float vp[2]) {
    uint64_t cam = find_camera();
    if (!cam) return false;

    uint64_t oRot = fx_off("Camera.Rotation"), oPos = fx_off("Camera.Position");
    uint64_t oFov = fx_off("Camera.FieldOfView"), oVp = fx_off("Camera.ViewportSize");
    if (oRot == (uint64_t)-1 || oPos == (uint64_t)-1 ||
        oFov == (uint64_t)-1 || oVp  == (uint64_t)-1) return false;

    if (fx_rdblk(cam + oRot, R, 36) != 36) return false;
    *pos = fx_rf3(cam + oPos);
    *fov = fx_rf(cam + oFov);
    if (fx_rdblk(cam + oVp, vp, 8) != 8) return false;
    return vp[0] >= 1.0f && vp[1] >= 1.0f;
}

// Unchanged in meaning: Roblox-window-local pixels, false when the point is
// behind the camera, clipped, or outside the frame. Add the window origin in
// the caller for monitor pixels, which is what the overlay pipe wants.
//
// The scale comes from the RENDER dimensions now instead of the window client
// rect. Same number, but from the same source as the matrix, so a resize can
// never leave the two describing different pictures.
bool project_to_screen(float3 w, float* sx, float* sy) {
    return fx_screen(w, sx, sy) == 1;   // visible, not merely projected
}

// How far off the wanted basis has to be before writing is worth it. Not zero:
// every write competes with the engine's own camera update, so one that changes
// nothing is pure interference -- and while DISARMED fx_wrblk logs each write it
// refuses, which buries the console.
static const float DEADZONE = 0.0008f;

// The only write in this project. Builds an orthonormal basis pointing at
// `world` and puts it back over Camera.Rotation.
//
// YAW comes from one origin and PITCH from another, and splitting them is the
// shift lock correction.
//
// Yaw, from the CHARACTER: under shift lock the camera sits about 1.75 studs to
// the RIGHT of the character, and the character's facing follows the camera's
// yaw. So a camera aimed exactly at a target leaves the CHARACTER aimed past it
// -- by atan(1.75 / distance), which is 5 degrees at 20 studs and worse as you
// close. Measuring the yaw from the character cancels that with no magic 1.75
// anywhere: whatever the offset is -- shift lock, a custom Humanoid.CameraOffset,
// first person, a game that moves the camera itself -- it is already the
// difference between two positions that are both read live.
//
// Pitch, from the CAMERA: taking both from the character is wrong in a way that
// is easy to miss. Character roots sit at hip height and so do their targets',
// so a look vector drawn between them is nearly flat, and setting the camera to
// it yanks the view down to horizontal however good the yaw is.
//
// What is left over is horizontal: the target sits slightly LEFT of the
// crosshair. That is not an error to fix, it is what being correctly aimed from
// a point that is not your eye looks like.
//
// `smoothing` is 0..1: the fraction of the way to turn this tick. 1.0 snaps.
// Lower is calmer but LAGS, which costs real accuracy against a moving target.
//
// Goes through fx_wrblk like every other write, so it honours the arm gate:
// disarmed, this logs what it would do and the camera does not move.
bool aim_from(float3 yawOrigin, float3 pitchOrigin, float3 world, float smoothing) {
    float R[9], vp[2], fov;
    float3 P;
    if (!camera_basis(R, &P, &fov, vp)) return false;

    float3 h = v_sub(world, yawOrigin);
    h.y = 0.0f;
    if (v_dot(h, h) < 1e-6f) return false;    // directly above or below us
    h = v_norm(h);

    float3 rel  = v_sub(world, pitchOrigin);
    float  flat = sqrtf(rel.x*rel.x + rel.z*rel.z);
    float  tanPitch = (flat > 1e-3f) ? (rel.y / flat) : 0.0f;

    // h is a unit HORIZONTAL vector, so raising it by tan(pitch) and
    // renormalising gives exactly that pitch without a trig call.
    float3 look = v_norm(float3{ h.x, tanPitch, h.z });
    float3 back = float3{ -look.x, -look.y, -look.z };

    // Right is horizontal by construction: cross(worldUp, back). Degenerate
    // when looking straight up or down, where "horizontal right" is undefined.
    float3 worldUp = { 0, 1, 0 };
    float3 right = v_cross(worldUp, back);
    if (v_dot(right, right) < 1e-6f) return false;
    right = v_norm(right);
    float3 up = v_cross(back, right);

    float want[9];
    want[0] = right.x; want[3] = right.y; want[6] = right.z;   // column 0
    want[1] = up.x;    want[4] = up.y;    want[7] = up.z;      // column 1
    want[2] = back.x;  want[5] = back.y;  want[8] = back.z;    // column 2

    if (smoothing < 1.0f) {
        for (int i = 0; i < 9; i++)
            want[i] = R[i] + (want[i] - R[i]) * smoothing;
        // Blending two rotation matrices componentwise does not stay
        // orthonormal, so re-derive the other two axes from the blended back
        // vector. Skipping this makes the view shear as it turns.
        float3 b2 = v_norm(float3{ want[2], want[5], want[8] });
        float3 r2 = v_cross(worldUp, b2);
        if (v_dot(r2, r2) < 1e-6f) return false;
        r2 = v_norm(r2);
        float3 u2 = v_cross(b2, r2);
        want[0] = r2.x; want[3] = r2.y; want[6] = r2.z;
        want[1] = u2.x; want[4] = u2.y; want[7] = u2.z;
        want[2] = b2.x; want[5] = b2.y; want[8] = b2.z;
    }

    bool moved = false;
    for (int i = 0; i < 9 && !moved; i++)
        if (fabsf(want[i] - R[i]) > DEADZONE) moved = true;
    if (!moved) return true;          // already there; nothing to say

    uint64_t cam  = find_camera();
    uint64_t oRot = fx_off("Camera.Rotation");
    if (!cam || oRot == (uint64_t)-1) return false;
    return fx_wrblk(cam + oRot, want, 36) == 36;
}

// Pure read. Answers the question the aim maths cannot answer about itself:
// after the engine has had its say, where is the camera actually pointing?
//
// If the yaw error stays large while aiming, no amount of arithmetic here is
// the problem -- the client is overwriting the rotation faster than it is being
// written.
bool aim_report(float3 charPos, float3 world, float* yawErrorDeg, float* sideOffset) {
    float R[9], vp[2], fov;
    float3 P;
    if (!camera_basis(R, &P, &fov, vp)) return false;

    // Column 2 is BACK, so forward is its negative. Flattened, because the
    // character only ever turns about the vertical axis.
    float3 fwd = v_norm(float3{ -R[2], 0.0f, -R[8] });
    float3 want = v_sub(world, charPos);
    want.y = 0.0f;
    if (v_dot(want, want) < 1e-6f) return false;
    want = v_norm(want);

    float d = v_dot(fwd, want);
    if (d >  1.0f) d =  1.0f;
    if (d < -1.0f) d = -1.0f;
    *yawErrorDeg = acosf(d) * 180.0f / 3.14159265f;

    // How far right of the character the eye sits, along the camera's own right
    // vector. This is the shift lock offset, measured -- not assumed.
    float3 eyeRel = v_sub(P, charPos);
    *sideOffset = eyeRel.x*R[0] + eyeRel.y*R[3] + eyeRel.z*R[6];
    return true;
}
