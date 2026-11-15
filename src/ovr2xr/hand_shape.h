// hand_shape.h - the LEFT hand's spheres placed on its live joints (Jay 2026-11-02: "consider the players hand & fingers"). The table
// is GENERATED (tools/hand_shape.py -> hand_shape_table.inc, fitted to the game's own skinned hand mesh); this places it. Header-only,
// no game reads - used by gunshape.cpp (live joints) and by the offline checks (the rest joints).
#pragma once

#include "hand_shape_table.inc"

#include <cmath>

namespace dlvr {
namespace gunshape {

// jm[j] = joint j's matrix in l_hand's own frame (row-major 3x4: rotation with axes as COLUMNS, translation [3] [7] [11]; m), in
// kHandJointNames order (jm[0] = identity). R9 = the hand's world rotation (row-major 3x3, axes as COLUMNS - SolveArmIk's handBasisW
// convention). Writes, per sphere, its centre minus the wrist (world) and its radius. False on a non-finite input. The rotation
// columns are used as given (FK of the game's locals - unit in our reads; a scaled bone would scale its spheres' offsets).
inline bool HandSpheresFromJoints(const float R9[9], const float jm[kHandJoints][12], float off[kHandSpheres][3], float r[kHandSpheres])
{
    for (int k = 0; k < 9; ++k) { if (!std::isfinite(R9[k])) { return false; } }
    for (int j = 0; j < kHandJoints; ++j) {
        for (int k = 0; k < 12; ++k) { if (!std::isfinite(jm[j][k])) { return false; } }
    }
    for (int s = 0; s < kHandSpheres; ++s) {
        const HandSphereDef& h = kHandSphereDefs[s];
        const float* m = jm[h.j];
        float l[3];
        for (int row = 0; row < 3; ++row) { l[row] = m[row * 4] * h.p[0] + m[row * 4 + 1] * h.p[1] + m[row * 4 + 2] * h.p[2] + m[row * 4 + 3]; }
        for (int row = 0; row < 3; ++row) { off[s][row] = R9[row * 3] * l[0] + R9[row * 3 + 1] * l[1] + R9[row * 3 + 2] * l[2]; }
        r[s] = h.r;
    }
    return true;
}

}  // namespace gunshape
}  // namespace dlvr
