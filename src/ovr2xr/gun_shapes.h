// gun_shapes.h - the generated per-gun solid shapes (gun_shapes_table.inc, tools/gun_shapes.py) and their lookup by mesh name.
// Solid guns plan Task 2. Header-only; included ONCE per translation unit that needs the table (the table's arrays are `static`, so
// each including .cpp gets its own copy - memory const-table-pointers-across-tus: never pass a row pointer across TUs, pass the
// mesh name).
#pragma once

#include "gunshape_logic.h"

#include <cctype>
#include <cstring>

namespace dlvr {
namespace gunshape {

struct GunShapeRow {
    const char* mesh;        // normalised: lower case, no ".msh", no trailing "_hq" (twohand_logic.h NormMesh)
    const char* frameNode;   // the skeleton root whose world matrix poses the boxes (lower case)
    GunShape    shape;
    Obb         mag;         // the magazine in the MAG BONE's (magBone's) REST frame; axis[0] = insertion direction
    bool        hasMag;
    const char* const* parts;   // the mesh's geometry nodes (lower case), index = Obb::part; the runtime reads each one's hidden
    int         nParts;         // flag by element name and sets GunShape::hiddenParts
    float       magRest34[12];  // the mag bone's REST pose in the model frame (row-major 3x4; zero without a mag). Its translation
                                // ([3] [7] [11]) is the runtime's frame self-check: the live mag bone in frameNode's frame must
                                // sit there while the mag is seated; the whole pose is the seated-mag test's (gun_shapes_test)
    const char* magBone;        // the mag bone (rifle plan Task 4): the first of bone_magazine / bone_clip the mesh has (pistol / SMG:
                                // bone_magazine; rifles: bone_clip); "" = neither. mag, the well and magRest34 are in THIS bone's frame
};

#include "gun_shapes_table.inc"

namespace shapes_detail {
// The same rule as twohand_logic.h NormMesh: lower case, strip every trailing ".msh", then ONE trailing "_hq". False = empty / too long.
inline bool Norm(const char* in, char* out, size_t n)
{
    if (!out || n < 2) { return false; }
    out[0] = 0;
    if (!in) { return false; }
    size_t len = 0;
    for (; in[len] && len + 1 < n; ++len) { out[len] = static_cast<char>(std::tolower(static_cast<unsigned char>(in[len]))); }
    if (in[len]) { out[0] = 0; return false; }
    out[len] = 0;
    while (len >= 4 && std::strcmp(out + len - 4, ".msh") == 0) { len -= 4; out[len] = 0; }
    if (len >= 3 && std::strcmp(out + len - 3, "_hq") == 0) { len -= 3; out[len] = 0; }
    return len > 0;
}
}  // namespace shapes_detail

// The row for a mesh name in any spelling the game uses ("wn_pistol_b_hq.msh", "WN_PISTOL_B"); nullptr = no row.
inline const GunShapeRow* FindGunShapeRow(const char* mesh)
{
    char k[96];
    if (!shapes_detail::Norm(mesh, k, sizeof(k))) { return nullptr; }
    for (int i = 0; i < kGunShapeRowCount; ++i) {
        if (std::strcmp(kGunShapeRows[i].mesh, k) == 0) { return &kGunShapeRows[i]; }
    }
    return nullptr;
}

inline const GunShape* FindGunShape(const char* mesh)
{
    const GunShapeRow* r = FindGunShapeRow(mesh);
    return r ? &r->shape : nullptr;
}

// The held-mag well rail (magrail_logic.h, Jay 2026-11-02 (4)): how far the SEATED mag travels out of the well until its TOP clears
// the well's open end - its centre's depth along the channel's into-gun axis plus its box's reach along that axis, at magRest34
// (the model frame, the channel's frame). False = no well / no mag / non-finite / not inside (<= 0).
inline bool SeatedMagClearM(const GunShapeRow& r, float* clearM)
{
    using namespace detail;
    if (!r.hasMag || !r.shape.hasWell || !Finite12(r.magRest34)) { return false; }
    float n[9];
    UnitRot(r.magRest34, n);
    Obb m = r.mag;
    ToWorldPt(n, r.magRest34, r.mag.c, m.c);
    for (int i = 0; i < 3; ++i) { ToWorldDir(n, r.mag.axis[i], m.axis[i]); }
    const Channel& ch = r.shape.well;
    const float rel[3] = { m.c[0] - ch.origin[0], m.c[1] - ch.origin[1], m.c[2] - ch.origin[2] };
    const float v = Dot(rel, ch.axis) + Radius(m, ch.axis);
    if (!Finite(v) || !(v > 0.0f)) { return false; }
    *clearM = v;
    return true;
}

}  // namespace gunshape
}  // namespace dlvr
