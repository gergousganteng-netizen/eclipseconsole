// health.cpp -- the offset health check. Split out of eclipse_console.cpp now
// that reads (read.h) and the offset table (offsets.h) are published: this file
// is almost entirely reads and value assertions, so it moved the moment its
// read/offset dependencies stopped being file-private. It reaches core state
// read-only through ec::state() and core_attached(); it writes nothing. The
// dense per-check reasoning travels verbatim.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "fx.h"
#include "eclipse_core.h"
#include "core_internal.h"   // core_attached (the "is a client here" check)
#include "read.h"            // rd_raw, rd64, read_name, class_of
#include "offsets.h"         // off_of

// The offset "not found" sentinel, value-identical to off_of's return for an
// absent key (matches read.cpp / offsets.cpp, which keep their own copy).
static const uint64_t BAD = (uint64_t)-1;

namespace ec {

// ---- offset health check ----------------------------------------------------
// See the comment on ec::health_check in eclipse_core.h for why this exists.
// Every check here asserts a RELATIONSHIP, not merely that a read returned
// something -- a wrong offset almost always returns something.

namespace {

struct HC {
    std::vector<HealthCheck>* out;

    void pass(const char* n, const std::string& d, const char* o = "") {
        out->push_back({ HealthCheck::Pass, n, d, o });
    }
    void fail(const char* n, const std::string& d, const char* o = "") {
        out->push_back({ HealthCheck::Fail, n, d, o });
    }
    void skip(const char* n, const std::string& d) {
        out->push_back({ HealthCheck::Skip, n, d, "" });
    }
    // The common shape: assert a condition, report the value either way.
    void want(bool ok, const char* n, const std::string& d, const char* o) {
        if (ok) pass(n, d, o); else fail(n, d, o);
    }
};

std::string f2(double v) {
    char b[48];
    snprintf(b, sizeof b, "%.2f", v);
    return b;
}

}  // namespace

std::vector<HealthCheck> health_check() {
    std::vector<HealthCheck> r;
    HC hc{ &r };

    if (!core_attached()) { hc.fail("attached", "no client", ""); return r; }
    hc.pass("attached", "pid " + std::to_string(state().pid), "");

    if (!state().noffsets) { hc.fail("offsets loaded", "none", ""); return r; }
    hc.pass("offsets loaded", std::to_string(state().noffsets) + " from " + conf_version(), "");

    if (state().offsets_stale) {
        hc.fail("conf matches client",
                "conf is " + conf_version() + ", client is " + client_version(),
                "ClientVersion");
        return r;   // everything below would be measuring the wrong build
    }
    hc.pass("conf matches client", client_version(), "");

    // ---- DataModel -----------------------------------------------------------
    uint64_t dm = fx_dm();
    if (!dm) { hc.fail("DataModel resolves", "null", "FakeDataModel.Pointer, .RealDataModel"); return r; }
    std::string dmc = class_of(dm);
    hc.want(dmc == "DataModel", "DataModel class", dmc.empty() ? "(empty)" : dmc,
            "Instance.ClassDescriptor, ClassDescriptor.ClassName");

    uint64_t nkids = fx_nchildren(dm);
    // A real DataModel has dozens of services. Zero means the walk is broken;
    // a huge number means the pointers are stale and FX_MAX_CHILDREN caught it.
    hc.want(nkids >= 10 && nkids <= 4096, "DataModel children",
            std::to_string(nkids), "Instance.ChildrenStart, .ChildrenEnd");

    // ---- names ---------------------------------------------------------------
    // Two separate paths. A SHORT name is stored inline in the string; a name
    // of 16+ characters lives behind the data pointer. The NameContainer change
    // broke both at once, but a future change could break only one -- and
    // finding a long name at all is the point of the walk.
    std::string dmn;
    read_name(dm, dmn, 128);
    hc.want(!dmn.empty(), "name (short)", dmn.empty() ? "(empty)" : dmn,
            "Instance.Name, Instance.NameContainer");

    {
        std::string longest;
        for (uint64_t i = 0; i < nkids && i < 256; i++) {
            uint64_t ch = fx_child(dm, (int)i);
            if (!ch) continue;
            std::string n;
            read_name(ch, n, 128);
            if (n.size() > longest.size()) longest = n;
        }
        if (longest.size() >= 16)
            hc.pass("name (heap, 16+ chars)", longest, "Instance.Name");
        else if (!longest.empty())
            hc.skip("name (heap, 16+ chars)", "no child name that long; longest was '" + longest + "'");
        else
            hc.fail("name (heap, 16+ chars)", "no child names readable at all", "Instance.Name");
    }

    // ---- services ------------------------------------------------------------
    const char* svcs[] = { "Workspace", "Players", "Lighting" };
    for (const char* s : svcs) {
        uint64_t v = fx_service(s);
        hc.want(v != 0, s, v ? "resolved" : "not found",
                "Instance.ClassDescriptor, ClassDescriptor.ClassName");
    }

    uint64_t me = fx_player();
    if (!me) {
        hc.skip("LocalPlayer", "not in a place yet");
    } else {
        std::string n;
        read_name(me, n, 64);
        hc.want(!n.empty(), "LocalPlayer", n.empty() ? "(no name)" : n, "Player.LocalPlayer");
    }

    // ---- character geometry ---------------------------------------------------
    uint64_t chr = fx_character();
    if (!chr) {
        hc.skip("character", "not spawned - join a place and respawn to check these");
    } else {
        uint64_t head = fx_find(chr, "Head");
        uint64_t root = fx_find(chr, "HumanoidRootPart");
        uint64_t hum  = fx_find(chr, "Humanoid");

        uint64_t oPrim = off_of("BasePart.Primitive"), oPos = off_of("Primitive.Position");
        if (!head || !root || oPrim == BAD || oPos == BAD) {
            hc.fail("head above torso", "parts not found", "BasePart.Primitive, Primitive.Position");
        } else {
            float3 hp = fx_rf3(rd64(head + oPrim) + oPos);
            float3 rp = fx_rf3(rd64(root + oPrim) + oPos);
            float dy = hp.y - rp.y;
            // A head sits above its own root part, by roughly a stud or two on
            // every rig. A wrong Primitive.Position makes this nonsense long
            // before it makes anything crash.
            hc.want(dy > 0.3f && dy < 4.0f, "head above torso",
                    f2(dy) + " studs", "BasePart.Primitive, Primitive.Position");
        }

        if (!hum) {
            hc.skip("humanoid", "no Humanoid in the character");
        } else {
            uint64_t oH = off_of("Humanoid.Health"), oM = off_of("Humanoid.MaxHealth");
            uint64_t oW = off_of("Humanoid.Walkspeed");
            if (oH != BAD && oM != BAD) {
                float h = fx_rf(hum + oH), m = fx_rf(hum + oM);
                hc.want(m > 0.0f && h >= 0.0f && h <= m + 0.5f, "health <= maxhealth",
                        f2(h) + " / " + f2(m), "Humanoid.Health, Humanoid.MaxHealth");
            }
            if (oW != BAD) {
                float w = fx_rf(hum + oW);
                hc.want(w >= 0.0f && w < 500.0f, "walkspeed sane",
                        f2(w), "Humanoid.Walkspeed");
            }
        }
    }

    // ---- camera ----------------------------------------------------------------
    uint64_t ws = fx_service("Workspace"), cam = 0;
    if (ws) {
        uint64_t n = fx_nchildren(ws);
        for (uint64_t i = 0; i < n && !cam; i++) {
            uint64_t c = fx_child(ws, (int)i);
            if (c && class_of(c) == "Camera") cam = c;
        }
    }
    if (!cam) {
        hc.skip("camera", "no Camera in Workspace");
        return r;
    }

    uint64_t oRot = off_of("Camera.Rotation"), oCp = off_of("Camera.Position");
    uint64_t oFov = off_of("Camera.FieldOfView"), oVp = off_of("Camera.ViewportSize");

    float R[9] = {0};
    if (oRot == BAD || !rd_raw(cam + oRot, R, 36)) {
        hc.fail("camera rotation readable", "no", "Camera.Rotation");
        return r;
    }

    // Orthonormality is the strongest single assertion available here: three
    // unit-length columns and a determinant of +1. Garbage passes neither, and
    // this is the field the aimbot WRITES -- a wrong offset there corrupts the
    // client rather than merely misreading it.
    float c0 = sqrtf(R[0]*R[0] + R[3]*R[3] + R[6]*R[6]);
    float c1 = sqrtf(R[1]*R[1] + R[4]*R[4] + R[7]*R[7]);
    float c2 = sqrtf(R[2]*R[2] + R[5]*R[5] + R[8]*R[8]);
    float det = R[0]*(R[4]*R[8] - R[5]*R[7])
              - R[1]*(R[3]*R[8] - R[5]*R[6])
              + R[2]*(R[3]*R[7] - R[4]*R[6]);
    bool ortho = fabsf(c0-1) < 0.01f && fabsf(c1-1) < 0.01f &&
                 fabsf(c2-1) < 0.01f && fabsf(det-1) < 0.01f;
    hc.want(ortho, "camera rotation orthonormal",
            "|cols| " + f2(c0) + " " + f2(c1) + " " + f2(c2) + ", det " + f2(det),
            "Camera.Rotation");

    if (oFov != BAD) {
        float fov = fx_rf(cam + oFov);
        // RADIANS, not degrees -- 1.22 is the default 70 deg. Anything above pi
        // means the field moved or the units changed.
        hc.want(fov > 0.1f && fov < 3.2f, "fov in radians",
                f2(fov) + " rad (" + f2(fov * 57.2958f) + " deg)", "Camera.FieldOfView");
    }

    float vp[2] = {0, 0};
    if (oVp != BAD && rd_raw(cam + oVp, vp, 8)) {
        hc.want(vp[0] >= 320.0f && vp[1] >= 240.0f && vp[0] <= 16384.0f && vp[1] <= 16384.0f,
                "viewport size", f2(vp[0]) + " x " + f2(vp[1]), "Camera.ViewportSize");
    }

    // ---- projection -------------------------------------------------------------
    // The end-to-end check: a point 20 studs down the camera's forward axis must
    // land at the middle of the screen. This is the one that proves the view
    // matrix is REAL rather than merely readable -- it ties VisualEngine to
    // Camera.* and to the viewport in a single assertion.
    uint64_t vePtr = off_of("VisualEngine.Pointer"), veVm = off_of("VisualEngine.ViewMatrix");
    float m[16] = {0};
    uint64_t ve = (vePtr == BAD) ? 0 : rd64(fx_base() + vePtr);
    if (!ve || veVm == BAD || !rd_raw(ve + veVm, m, 64)) {
        hc.fail("view matrix readable", "no", "VisualEngine.Pointer, .ViewMatrix");
        return r;
    }
    if (oCp == BAD || vp[0] < 1.0f) {
        hc.skip("forward point projects to centre", "need Camera.Position and a viewport");
        return r;
    }

    float3 eye = fx_rf3(cam + oCp);
    // Column 2 of the basis is "back", so forward is its negation.
    float3 fwd{ -R[2], -R[5], -R[8] };
    float3 t{ eye.x + fwd.x*20, eye.y + fwd.y*20, eye.z + fwd.z*20 };
    float cx = m[0]*t.x + m[1]*t.y + m[2]*t.z  + m[3];
    float cy = m[4]*t.x + m[5]*t.y + m[6]*t.z  + m[7];
    float cw = m[12]*t.x + m[13]*t.y + m[14]*t.z + m[15];
    if (cw <= 1e-4f) {
        hc.fail("forward point projects to centre", "point came out behind the camera",
                "VisualEngine.ViewMatrix");
    } else {
        float px = (vp[0] * 0.5f) * (1.0f + cx / cw);
        float py = (vp[1] * 0.5f) * (1.0f - cy / cw);
        float ex = fabsf(px - vp[0] * 0.5f), ey = fabsf(py - vp[1] * 0.5f);
        // 2% of the viewport. Generous enough for float noise, far tighter than
        // any wrong offset would land.
        bool ok = ex < vp[0] * 0.02f && ey < vp[1] * 0.02f;
        hc.want(ok, "forward point projects to centre",
                f2(px) + "," + f2(py) + " vs centre " + f2(vp[0]*0.5f) + "," + f2(vp[1]*0.5f),
                "VisualEngine.ViewMatrix, Camera.Rotation, Camera.Position");
    }

    return r;
}

}  // namespace ec
