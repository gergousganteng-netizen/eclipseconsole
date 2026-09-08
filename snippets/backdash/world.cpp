#include "bd.h"
#include <cmath>
#include <cstring>

// Resolve the character's parts fresh each call. fx_character() already tracks
// respawns, so this only has to find the pieces under it.
bool refresh_ptrs(Ptrs* out) {
    Ptrs p;
    p.character = fx_character();
    if (!p.character) { *out = p; return false; }

    p.humanoid = fx_part(p.character, "Humanoid");
    p.hrp      = fx_part(p.character, "HumanoidRootPart");
    if (!p.hrp) p.hrp = fx_part(p.character, "Torso");
    p.head     = fx_part(p.character, "Head");

    uint64_t oPrim = fx_off("BasePart.Primitive");
    if (p.hrp && oPrim != (uint64_t)-1) {
        uint64_t prim = fx_rd(p.hrp + oPrim);
        if (prim >= 0x10000 && prim <= 0x7ff000000000ULL) p.primitive = prim;
    }

    p.ok = p.humanoid && p.primitive;
    *out = p;
    return p.ok;
}

// 0..1. Returns 1 when it cannot be read, so a bad read never scales the dash
// down to nothing.
float hp_ratio(uint64_t humanoid) {
    uint64_t oH = fx_off("Humanoid.Health");
    uint64_t oM = fx_off("Humanoid.MaxHealth");
    if (!humanoid || oH == (uint64_t)-1 || oM == (uint64_t)-1) return 1.0f;
    float h = 0, m = 0;
    fx_rdblk(humanoid + oH, &h, 4);
    fx_rdblk(humanoid + oM, &m, 4);
    if (!(m > 0.0f) || !(h == h) || !(m == m)) return 1.0f;
    float r = h / m;
    return r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
}

bool read_rot(uint64_t primitive, float* rot9) {
    uint64_t oR = fx_off("Primitive.Rotation");
    if (!primitive || oR == (uint64_t)-1) return false;
    return fx_rdblk(primitive + oR, rot9, 36) == 36;
}

// Direction in world space from the character's rotation matrix. Up and down
// use world Y instead, since "up relative to your facing" is not what anyone
// means by an up dash.
bool dir_vector(int dir, const float* r, float* bx, float* bz, float* by, bool* vertical) {
    *bx = *bz = *by = 0.0f;
    *vertical = false;
    switch (dir) {
        case 1:  *bx = -r[2]; *bz = -r[8]; break;
        case 2:  *bx = -r[0]; *bz = -r[6]; break;
        case 3:  *bx =  r[0]; *bz =  r[6]; break;
        case 4:  *by =  1.0f; *vertical = true; return true;
        case 5:  *by = -1.0f; *vertical = true; return true;
        default: *bx =  r[2]; *bz =  r[8]; break;
    }
    float mag = std::sqrt((*bx) * (*bx) + (*bz) * (*bz));
    if (mag < 1e-6f) return false;
    *bx /= mag; *bz /= mag;
    return true;
}
