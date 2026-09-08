// Stage F: field offsets, by signature rather than by anchor string.
//
// The earlier stages proved the root can be derived from nothing, so this one
// takes the DataModel as given and runs in seconds instead of sweeping 3.6 GB.
// Everything below it is derived and then graded against the conf.
//
// The signature that does most of the work is the CFrame: Roblox stores a 3x3
// rotation matrix immediately followed by a vec3 position. Nine floats that are
// orthonormal with determinant +1 is not something random memory produces, and
// the position is simply the twelve bytes after it -- so one pattern yields
// Camera.Rotation, Camera.Position, Primitive.Rotation and Primitive.Position.
//
// BasePart.Primitive then falls out of consistency: the pointer field that
// leads to a CFrame-bearing object FOR MANY DIFFERENT PARTS is the real one. A
// single part could coincide; forty agreeing on the same offset could not.

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <map>
#include <string>
#include <vector>

static void L(const char* fmt, ...) {
    char b[512];
    va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    fx_log(b);
}

static uint64_t qw(const unsigned char* p, size_t o) {
    uint64_t v; memcpy(&v, p + o, 8); return v;
}
static float fl(const unsigned char* p, size_t o) {
    float v; memcpy(&v, p + o, 4); return v;
}

// A std::string read that copes with both storage forms, as the console does.
static bool rd_string(uint64_t at, std::string& out) {
    unsigned char raw[0x20];
    if (fx_rdblk(at, raw, sizeof raw) != sizeof raw) return false;
    uint64_t sz = qw(raw, 0x10), cap = qw(raw, 0x18);
    if (sz == 0 || sz > 128 || cap < sz) return false;
    char buf[129] = {};
    if (cap >= 16) {
        uint64_t p = qw(raw, 0);
        if (!fx_ptr_ok(p) || fx_rdblk(p, buf, (size_t)sz) != (size_t)sz) return false;
    } else {
        memcpy(buf, raw, (size_t)sz);
    }
    for (size_t i = 0; i < sz; i++) if (buf[i] < 32 || buf[i] > 126) return false;
    out.assign(buf, (size_t)sz);
    return true;
}

// Is there a rotation matrix at p+o? Orthonormal, right-handed, and not the
// zero or identity-by-accident case that uninitialised memory can fake.
static bool is_rotation(const unsigned char* p, size_t o) {
    float m[9];
    for (int i = 0; i < 9; i++) {
        m[i] = fl(p, o + (size_t)i * 4);
        if (!std::isfinite(m[i]) || std::fabs(m[i]) > 1.001f) return false;
    }
    for (int r = 0; r < 3; r++) {
        float len = m[r*3]*m[r*3] + m[r*3+1]*m[r*3+1] + m[r*3+2]*m[r*3+2];
        if (std::fabs(len - 1.0f) > 1e-3f) return false;
    }
    float d01 = m[0]*m[3] + m[1]*m[4] + m[2]*m[5];
    float d02 = m[0]*m[6] + m[1]*m[7] + m[2]*m[8];
    float d12 = m[3]*m[6] + m[4]*m[7] + m[5]*m[8];
    if (std::fabs(d01) > 1e-3f || std::fabs(d02) > 1e-3f || std::fabs(d12) > 1e-3f)
        return false;
    float det = m[0]*(m[4]*m[8] - m[5]*m[7])
              - m[1]*(m[3]*m[8] - m[5]*m[6])
              + m[2]*(m[3]*m[7] - m[4]*m[6]);
    return std::fabs(det - 1.0f) < 1e-3f;
}

static bool is_position(const unsigned char* p, size_t o) {
    for (int i = 0; i < 3; i++) {
        float v = fl(p, o + (size_t)i * 4);
        if (!std::isfinite(v) || std::fabs(v) > 100000.0f) return false;
    }
    return true;
}

// A CFrame is the rotation with the position right behind it.
static bool find_cframe(const unsigned char* buf, size_t len, size_t& at) {
    for (size_t o = 0; o + 48 <= len; o += 4)
        if (is_rotation(buf, o) && is_position(buf, o + 36)) { at = o; return true; }
    return false;
}

struct Ctx { uint64_t cd, cn, cs, ce, pa; };

static std::string class_of(const Ctx& c, uint64_t inst) {
    uint64_t d = fx_rd(inst + c.cd);
    std::string s;
    if (!d || !rd_string(d + c.cn, s)) {
        uint64_t p = d ? fx_rd(d + c.cn) : 0;      // ClassName is a pointer here
        if (!p || !rd_string(p, s)) return "";
    }
    return s;
}

static void children(const Ctx& c, uint64_t inst, std::vector<uint64_t>& out) {
    uint64_t cont = fx_rd(inst + c.cs);
    if (!fx_ptr_ok(cont)) return;
    uint64_t b = fx_rd(cont), e = fx_rd(cont + c.ce);
    if (!fx_ptr_ok(b) || e <= b || (e - b) % 0x10) return;
    uint64_t n = (e - b) / 0x10;
    if (n > 4000) n = 4000;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t ch = fx_rd(b + i * 0x10);
        if (fx_ptr_ok(ch)) out.push_back(ch);
    }
}

static void grade(const char* key, uint64_t got) {
    uint64_t want = fx_off(key);
    L("  %-28s derived %-8llx conf %-8llx  %s", key,
      (unsigned long long)got, (unsigned long long)want,
      got == want ? "MATCH" : (want == (uint64_t)-1 ? "(not in conf)" : "DIFFER"));
}

extern "C" void fx_run() {
    uint64_t dm = fx_dm();
    if (!dm) { L("not attached"); return; }
    Ctx c{};

    // ---- ClassDescriptor / ClassName, from the one class name we know -------
    unsigned char obj[0x200];
    fx_rdblk(dm, obj, sizeof obj);
    for (uint64_t j = 0; j <= 0x40 && !c.cd; j += 8) {
        uint64_t d = qw(obj, (size_t)j);
        if (!fx_ptr_ok(d)) continue;
        for (uint64_t k = 0; k <= 0x40; k += 8) {
            std::string s;
            uint64_t p = fx_rd(d + k);
            if ((fx_ptr_ok(p) && rd_string(p, s) && s == "DataModel") ||
                (rd_string(d + k, s) && s == "DataModel")) {
                c.cd = j; c.cn = k; break;
            }
        }
    }
    if (!c.cd && c.cd != 0) { L("could not locate the class name"); return; }

    // ---- children / parent, by the round trip -------------------------------
    for (uint64_t cs = 0x40; cs <= 0x100 && !c.cs; cs += 8) {
        uint64_t cont = qw(obj, (size_t)cs);
        if (!fx_ptr_ok(cont)) continue;
        unsigned char cb[0x40];
        if (fx_rdblk(cont, cb, sizeof cb) != sizeof cb) continue;
        uint64_t b = qw(cb, 0);
        if (!fx_ptr_ok(b)) continue;
        for (uint64_t ce = 8; ce <= 0x38; ce += 8) {
            uint64_t e = qw(cb, (size_t)ce);
            if (e <= b || (e - b) % 0x10) continue;
            uint64_t n = (e - b) / 0x10;
            if (n < 3 || n > 4000) continue;
            uint64_t c0 = fx_rd(b), c1 = fx_rd(b + 0x10);
            for (uint64_t pa = 0x40; pa <= 0xf8; pa += 8)
                if (fx_rd(c0 + pa) == dm && fx_rd(c1 + pa) == dm) {
                    c.cs = cs; c.ce = ce; c.pa = pa; break;
                }
            if (c.cs) break;
        }
    }
    L("shape:  desc +%llx  name +%llx  children +%llx  end +%llx  parent +%llx",
      (unsigned long long)c.cd, (unsigned long long)c.cn,
      (unsigned long long)c.cs, (unsigned long long)c.ce, (unsigned long long)c.pa);
    if (!c.cs) { L("stop: no children shape"); return; }

    // ---- Workspace and Camera, by class, then by pointer-back ---------------
    std::vector<uint64_t> svc;
    children(c, dm, svc);
    uint64_t ws = 0;
    for (uint64_t s : svc) if (class_of(c, s) == "Workspace") ws = s;
    if (!ws) { L("stop: no Workspace among %zu services", svc.size()); return; }

    uint64_t dm_ws = (uint64_t)-1;
    unsigned char dmbuf[0x800];
    fx_rdblk(dm, dmbuf, sizeof dmbuf);
    for (size_t o = 0; o + 8 <= sizeof dmbuf; o += 8)
        if (qw(dmbuf, o) == ws) { dm_ws = o; break; }

    std::vector<uint64_t> wsk;
    children(c, ws, wsk);
    uint64_t cam = 0;
    for (uint64_t k : wsk) if (class_of(c, k) == "Camera") cam = k;

    uint64_t ws_cam = (uint64_t)-1;
    std::vector<unsigned char> wsbuf(0x1000);
    fx_rdblk(ws, wsbuf.data(), wsbuf.size());
    if (cam)
        for (size_t o = 0; o + 8 <= wsbuf.size(); o += 8)
            if (qw(wsbuf.data(), o) == cam) { ws_cam = o; break; }

    L("Workspace %llx   Camera %llx   (%zu services, %zu workspace children)",
      (unsigned long long)ws, (unsigned long long)cam, svc.size(), wsk.size());

    // ---- the camera CFrame --------------------------------------------------
    uint64_t cam_rot = (uint64_t)-1, cam_pos = (uint64_t)-1;
    if (cam) {
        unsigned char cb[0x400];
        if (fx_rdblk(cam, cb, sizeof cb) == sizeof cb) {
            size_t at;
            if (find_cframe(cb, sizeof cb, at)) {
                cam_rot = at; cam_pos = at + 36;
                L("camera CFrame at +%llx   pos %.1f %.1f %.1f",
                  (unsigned long long)at, fl(cb, at + 36), fl(cb, at + 40), fl(cb, at + 44));
            }
        }
    }

    // ---- BasePart.Primitive, by agreement across many parts -----------------
    // Collect parts breadth-first from the Workspace, two levels down, which is
    // enough to reach model children without walking the whole tree.
    std::vector<uint64_t> parts;
    for (uint64_t k : wsk) {
        std::string cl = class_of(c, k);
        if (cl == "Part" || cl == "MeshPart" || cl == "UnionOperation" ||
            cl == "SpawnLocation" || cl == "Seat" || cl == "TrussPart" ||
            cl == "WedgePart" || cl == "CornerWedgePart")
            parts.push_back(k);
        if (parts.size() > 60) break;
        if (cl == "Model" || cl == "Folder") {
            std::vector<uint64_t> gk;
            children(c, k, gk);
            for (uint64_t g : gk) {
                std::string gc = class_of(c, g);
                if (gc == "Part" || gc == "MeshPart" || gc == "UnionOperation" ||
                    gc == "WedgePart" || gc == "Seat")
                    parts.push_back(g);
            }
        }
    }
    L("parts sampled: %zu", parts.size());

    std::map<uint64_t, int> ptr_votes;      // BasePart -> Primitive offset
    std::map<uint64_t, int> cf_votes;       // CFrame offset inside Primitive
    for (uint64_t p : parts) {
        unsigned char pb[0x300];
        if (fx_rdblk(p, pb, sizeof pb) != sizeof pb) continue;
        for (size_t o = 0; o + 8 <= sizeof pb; o += 8) {
            uint64_t t = qw(pb, o);
            if (!fx_ptr_ok(t)) continue;
            unsigned char tb[0x220];
            if (fx_rdblk(t, tb, sizeof tb) != sizeof tb) continue;
            size_t at;
            if (find_cframe(tb, sizeof tb, at)) {
                ptr_votes[o]++;
                cf_votes[at]++;
            }
        }
    }
    uint64_t prim = (uint64_t)-1; int pbest = 0;
    for (const auto& kv : ptr_votes) if (kv.second > pbest) { pbest = kv.second; prim = kv.first; }
    uint64_t prot = (uint64_t)-1; int cbest = 0;
    for (const auto& kv : cf_votes) if (kv.second > cbest) { cbest = kv.second; prot = kv.first; }
    L("Primitive pointer: +%llx agreed by %d/%zu parts", (unsigned long long)prim,
      pbest, parts.size());
    L("Primitive CFrame:  +%llx agreed by %d", (unsigned long long)prot, cbest);
    for (const auto& kv : ptr_votes)
        if (kv.second > 1 && kv.first != prim)
            L("   also: +%llx (%d votes)", (unsigned long long)kv.first, kv.second);

    // ---- grade --------------------------------------------------------------
    L("--- graded against the conf ---");
    grade("Instance.ClassDescriptor", c.cd);
    grade("Instance.ClassName",       c.cn);
    grade("Instance.ChildrenStart",   c.cs);
    grade("Instance.ChildrenEnd",     c.ce);
    grade("Instance.Parent",          c.pa);
    grade("DataModel.Workspace",      dm_ws);
    grade("Workspace.CurrentCamera",  ws_cam);
    grade("Camera.Rotation",          cam_rot);
    grade("Camera.Position",          cam_pos);
    grade("BasePart.Primitive",       prim);
    grade("Primitive.Rotation",       prot);
    grade("Primitive.Position",       prot == (uint64_t)-1 ? prot : prot + 36);
}
