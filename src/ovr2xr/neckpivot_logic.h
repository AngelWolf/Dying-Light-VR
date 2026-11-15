// Pure logic for head_neck_pivot's arc (SPEC-HEAD-NECK-PIVOT-2026-10-01). No OpenXR, no Windows, no logging, so
// src/src/tests/neckpivot_logic_test.cpp compiles it on its own. ovr2xr.cpp NeckPivotCompute owns the use.
//
// The reported head is eye - (R - B) n: R = the head's rotation, B = the rotation the arc is measured from, n = neck -> eye in
// the head frame. B = identity (today, decouple off): the whole arc goes, because the engine's own eye lever puts it back from
// the head orientation it is given (INV-322). B = the head's own yaw (decouple on, 2026-10-20): the engine is given a YAW-FREE
// head, so its lever never adds the yaw arc back - removing it left the reported head 2 |n_xz| sin(yaw/2) off (~17 cm at 180
// deg from the playspace's forward; Jay: "essentially my forearms only" after a 180 + recentre). Only the pitch / roll arc goes.
#pragma once

namespace dlvr {
namespace neckpivot {

// The rotation matrix of unit quaternion (x, y, z, w), row-major.
inline void QuatToMat(const float q[4], float R[9])
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    R[0] = 1 - 2 * (y * y + z * z); R[1] = 2 * (x * y - z * w);     R[2] = 2 * (x * z + y * w);
    R[3] = 2 * (x * y + z * w);     R[4] = 1 - 2 * (x * x + z * z); R[5] = 2 * (y * z - x * w);
    R[6] = 2 * (x * z - y * w);     R[7] = 2 * (y * z + x * w);     R[8] = 1 - 2 * (x * x + y * y);
}

// out = (R(q) - R(base)) n. base = identity -> today's (R - I) n.
inline void ArcOffset(const float q[4], const float base[4], const float n[3], float out[3])
{
    float R[9], B[9];
    QuatToMat(q, R);
    QuatToMat(base, B);
    for (int r = 0; r < 3; ++r) {
        out[r] = (R[r * 3 + 0] - B[r * 3 + 0]) * n[0] + (R[r * 3 + 1] - B[r * 3 + 1]) * n[1] + (R[r * 3 + 2] - B[r * 3 + 2]) * n[2];
    }
}

}  // namespace neckpivot
}  // namespace dlvr
