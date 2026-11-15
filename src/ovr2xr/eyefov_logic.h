#pragma once
// One frustum for both eyes (INV-597; docs/BETA-FINDINGS-2026-10-19.md 7f; Jay, 2026-10-19: "Build it.").
//
// With symmetric_fov=2 GameEyeFov symmetrises EACH eye from its own real FOV. On a headset whose eyes are mirror images
// (Quest 3, PSVR2) the two results are identical; on one whose eyes are not (Steam Frame: eye0 1.7260, eye1 1.6938) they
// differ, and the AFW rebuild - which draws the held eye with the FRESH eye's projection (dlssflat.cpp AfwRebuildRun) - puts
// every rebuilt frame 1.9 % off in zoom. Handing both eyes the frustum that contains both makes the rebuild's one-projection
// assumption true on every headset. A superset, so no edge is lost; the narrower eye renders a little beyond its lens.

namespace dlvr::eyefov {

struct Tans { float l, r, u, d; };

inline float Max(float a, float b) { return a > b ? a : b; }

// The smallest frustum containing both eyes' frustums. Symmetric in its arguments, so each eye computing
// Shared(own, other) lands on the identical frustum.
inline Tans Shared(const Tans& a, const Tans& b)
{
    return Tans{ Max(a.l, b.l), Max(a.r, b.r), Max(a.u, b.u), Max(a.d, b.d) };
}

inline bool Differ(const Tans& a, const Tans& b)
{
    return a.l != b.l || a.r != b.r || a.u != b.u || a.d != b.d;
}

}  // namespace dlvr::eyefov
