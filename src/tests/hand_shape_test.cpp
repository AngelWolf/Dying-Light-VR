// Standalone unit test for src/src/ovr2xr/hand_shape.h + the GENERATED hand_shape_table.inc - NOT part of the DLL build.
// The solid HAND (Jay 2026-11-02: "consider the players hand & fingers"). Exit code = number of failures.
// Compile: cl /nologo /EHsc /W4 /WX /std:c++17 /I src\src\ovr2xr src\src\tests\hand_shape_test.cpp
#include "hand_shape.h"

#include "gun_shapes.h"
#include "gunshape_logic.h"

#include <cmath>
#include <cstdio>
#include <limits>

using namespace dlvr::gunshape;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

namespace {

const float kI9[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
bool Near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

void Rest(float jm[kHandJoints][12])
{
    for (int j = 0; j < kHandJoints; ++j) { for (int k = 0; k < 12; ++k) { jm[j][k] = kHandRestJoint[j][k]; } }
}

// boneblend.cpp's FK walks the list in order: every parent must come before its child, and joint 0 is the root.
void TableParentsPrecedeChildren()
{
    CHECK(kHandParent[0] == -1);
    for (int j = 1; j < kHandJoints; ++j) { CHECK(kHandParent[j] >= 0 && kHandParent[j] < j); }
}

void TableSpheresSane()
{
    const volatile int n = kHandSpheres;   // volatile: not a constant expression (C4127)
    CHECK(n > 50);
    for (int s = 0; s < kHandSpheres; ++s) {
        const HandSphereDef& h = kHandSphereDefs[s];
        CHECK(h.j < kHandJoints);
        CHECK(h.r >= 0.003f && h.r <= 0.040f);
        CHECK(std::fabs(h.p[0]) < 0.25f && std::fabs(h.p[1]) < 0.25f && std::fabs(h.p[2]) < 0.25f);
    }
}

// The rest joints and the bone lengths come from the same fit: they must agree (the runtime's FK check compares live to these).
void RestJointsMatchBoneLengths()
{
    for (int j = 1; j < kHandJoints; ++j) {
        const int p = kHandParent[j];
        const float d[3] = { kHandRestJoint[j][3] - kHandRestJoint[p][3], kHandRestJoint[j][7] - kHandRestJoint[p][7],
                             kHandRestJoint[j][11] - kHandRestJoint[p][11] };
        CHECK(Near(std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), kHandBoneLen[j], 0.0002f));
    }
    // joint 0 is l_hand itself: identity
    CHECK(kHandRestJoint[0][0] > 0.999f && kHandRestJoint[0][5] > 0.999f && kHandRestJoint[0][10] > 0.999f);
    CHECK(Near(kHandRestJoint[0][3], 0, 1e-5f) && Near(kHandRestJoint[0][7], 0, 1e-5f) && Near(kHandRestJoint[0][11], 0, 1e-5f));
}

// A sphere moves with ITS joint, and only its joint's spheres move.
void SpheresFollowTheirJoint()
{
    float jm[kHandJoints][12];
    Rest(jm);
    static float off0[kHandSpheres][3], r0[kHandSpheres], off1[kHandSpheres][3], r1[kHandSpheres];
    CHECK(HandSpheresFromJoints(kI9, jm, off0, r0));
    const int moved = 7;   // l_finger13, the index finger's distal joint
    jm[moved][3] += 0.01f;
    CHECK(HandSpheresFromJoints(kI9, jm, off1, r1));
    int nMoved = 0;
    for (int s = 0; s < kHandSpheres; ++s) {
        const bool mine = kHandSphereDefs[s].j == moved;
        const float dx = off1[s][0] - off0[s][0], dy = off1[s][1] - off0[s][1], dz = off1[s][2] - off0[s][2];
        if (mine) {
            ++nMoved;
            CHECK(Near(dx, 0.01f, 1e-6f) && Near(dy, 0, 1e-6f) && Near(dz, 0, 1e-6f));
        } else {
            CHECK(dx == 0 && dy == 0 && dz == 0);
        }
        CHECK(r0[s] == kHandSphereDefs[s].r);
    }
    CHECK(nMoved > 0);   // the index tip has spheres
}

// A joint's ROTATION turns its spheres about it (the finger curls): rotate l_finger13 90 deg about its own Z.
void SpheresTurnWithTheirJoint()
{
    float jm[kHandJoints][12];
    Rest(jm);
    static float off0[kHandSpheres][3], r0[kHandSpheres], off1[kHandSpheres][3], r1[kHandSpheres];
    CHECK(HandSpheresFromJoints(kI9, jm, off0, r0));
    const int j = 7;
    // new rotation = old x Rz(90): columns (c1, -c0, c2)
    float c0[3] = { jm[j][0], jm[j][4], jm[j][8] }, c1[3] = { jm[j][1], jm[j][5], jm[j][9] };
    for (int r = 0; r < 3; ++r) { jm[j][4 * r + 0] = c1[r]; jm[j][4 * r + 1] = -c0[r]; }
    CHECK(HandSpheresFromJoints(kI9, jm, off1, r1));
    for (int s = 0; s < kHandSpheres; ++s) {
        if (kHandSphereDefs[s].j != j) { continue; }
        // the distance from the joint is kept; the point moved unless it lies on the joint's Z axis
        const float* t = &jm[j][0];
        const float d0 = std::sqrt((off0[s][0] - t[3]) * (off0[s][0] - t[3]) + (off0[s][1] - t[7]) * (off0[s][1] - t[7]) +
                                   (off0[s][2] - t[11]) * (off0[s][2] - t[11]));
        const float d1 = std::sqrt((off1[s][0] - t[3]) * (off1[s][0] - t[3]) + (off1[s][1] - t[7]) * (off1[s][1] - t[7]) +
                                   (off1[s][2] - t[11]) * (off1[s][2] - t[11]));
        CHECK(Near(d0, d1, 1e-5f));
        const float p = kHandSphereDefs[s].p[0] * kHandSphereDefs[s].p[0] + kHandSphereDefs[s].p[1] * kHandSphereDefs[s].p[1];
        if (p > 1e-6f) {
            const float mv = std::fabs(off1[s][0] - off0[s][0]) + std::fabs(off1[s][1] - off0[s][1]) + std::fabs(off1[s][2] - off0[s][2]);
            CHECK(mv > 1e-4f);
        }
    }
}

// The hand's world rotation turns every offset.
void HandRotationApplied()
{
    float jm[kHandJoints][12];
    Rest(jm);
    static float off0[kHandSpheres][3], r0[kHandSpheres], off1[kHandSpheres][3], r1[kHandSpheres];
    CHECK(HandSpheresFromJoints(kI9, jm, off0, r0));
    const float Rz[9] = { 0, -1, 0, 1, 0, 0, 0, 0, 1 };   // 90 deg about Z, axes as columns
    CHECK(HandSpheresFromJoints(Rz, jm, off1, r1));
    for (int s = 0; s < kHandSpheres; ++s) {
        CHECK(Near(off1[s][0], -off0[s][1], 1e-6f) && Near(off1[s][1], off0[s][0], 1e-6f) && Near(off1[s][2], off0[s][2], 1e-6f));
    }
}

void NonFiniteRefuses()
{
    float jm[kHandJoints][12];
    Rest(jm);
    static float off[kHandSpheres][3], r[kHandSpheres];
    jm[3][7] = std::numeric_limits<float>::quiet_NaN();
    CHECK(!HandSpheresFromJoints(kI9, jm, off, r));
    Rest(jm);
    float R[9] = { 1, 0, 0, 0, 1, 0, 0, 0, std::numeric_limits<float>::infinity() };
    CHECK(!HandSpheresFromJoints(R, jm, off, r));
}

// THE BOX GRID (a cost fix) must not change a single answer: the real hand on real guns, from many directions and hand rotations,
// with the grid and without - the same stop, the same first sphere, the same "started inside". Both kinds of call: a sweep that
// touches, a free move, and a start inside.
void GridMatchesPlain()
{
    const char* rows[] = { "wn_pistol_b", "wn_rifle_rust", "wn_shisha_gun" };
    const float id[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
    static float off[kHandSpheres][3], r[kHandSpheres];
    int compared = 0, stopped = 0, insideN = 0;
    for (const char* name : rows) {
        const GunShapeRow* row = FindGunShapeRow(name);
        CHECK(row != nullptr);
        if (!row) { continue; }
        BoxGrid grid;
        CHECK(BuildBoxGrid(row->shape, &grid));
        float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
        for (int k = 0; k < row->shape.nBoxes; ++k) {
            for (int a = 0; a < 3; ++a) {
                const float lo = row->shape.boxes[k].c[a] - row->shape.boxes[k].half[a], hi = row->shape.boxes[k].c[a] + row->shape.boxes[k].half[a];
                if (lo < mn[a]) { mn[a] = lo; }
                if (hi > mx[a]) { mx[a] = hi; }
            }
        }
        const float c[3] = { 0.5f * (mn[0] + mx[0]), 0.5f * (mn[1] + mx[1]), 0.5f * (mn[2] + mx[2]) };
        const HandBody plain[1] = { { &row->shape, id, nullptr } };
        const HandBody gridded[1] = { { &row->shape, id, &grid } };
        for (int rot = 0; rot < 6; ++rot) {
            // six hand rotations: identity and 90 / 180 deg turns about each axis
            const float R[6][9] = { { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, { 1, 0, 0, 0, 0, -1, 0, 1, 0 }, { 0, 0, 1, 0, 1, 0, -1, 0, 0 },
                                    { 0, -1, 0, 1, 0, 0, 0, 0, 1 }, { -1, 0, 0, 0, -1, 0, 0, 0, 1 }, { 1, 0, 0, 0, -1, 0, 0, 0, -1 } };
            CHECK(HandSpheresFromJoints(R[rot], kHandRestJoint, off, r));
            float oc[3] = { 0, 0, 0 };
            for (int s = 0; s < kHandSpheres; ++s) { for (int a = 0; a < 3; ++a) { oc[a] += off[s][a] / kHandSpheres; } }
            for (int d = 0; d < 6; ++d) {
                float dv[3] = { 0, 0, 0 };
                dv[d / 2] = (d % 2) ? 1.0f : -1.0f;
                for (int depth = 0; depth < 3; ++depth) {
                    // aim the hand's centre at the gun's centre (touching sweep), past it by depth (start inside for depth 2), or alongside
                    const float to[3] = { c[0] - oc[0] + dv[0] * 0.02f * depth, c[1] - oc[1] + dv[1] * 0.02f * depth, c[2] - oc[2] + dv[2] * 0.02f * depth };
                    const float from[3] = { to[0] - 0.35f * dv[0], to[1] - 0.35f * dv[1], to[2] - 0.35f * dv[2] };
                    const float* f = depth == 2 ? to : from;   // depth 2: start where it ends (inside) - the push-out path
                    float o1[3], o2[3];
                    HandHit h1{}, h2{};
                    const bool s1 = StopHandOnBodies(plain, 1, off, r, kHandSpheres, f, to, false, o1, &h1);
                    const bool s2 = StopHandOnBodies(gridded, 1, off, r, kHandSpheres, f, to, false, o2, &h2);
                    ++compared;
                    stopped += s1 ? 1 : 0;
                    insideN += h1.startedInside ? 1 : 0;
                    CHECK(s1 == s2);
                    CHECK(h1.startedInside == h2.startedInside);
                    CHECK(Near(o1[0], o2[0], 1e-5f) && Near(o1[1], o2[1], 1e-5f) && Near(o1[2], o2[2], 1e-5f));
                    if (!h1.startedInside) { CHECK(h1.sphere == h2.sphere); }   // push-out: which sphere is first is order-dependent
                }
            }
        }
    }
    std::printf("  grid vs plain: %d calls compared, %d stopped, %d started inside\n", compared, stopped, insideN);
    CHECK(stopped > compared / 2);   // the comparison must exercise stops, not only free moves
    CHECK(insideN > 0);
}

}  // namespace

int main()
{
    TableParentsPrecedeChildren();
    TableSpheresSane();
    RestJointsMatchBoneLengths();
    SpheresFollowTheirJoint();
    SpheresTurnWithTheirJoint();
    HandRotationApplied();
    NonFiniteRefuses();
    GridMatchesPlain();
    std::printf("hand_shape_test: %d spheres, %d failure(s)\n", kHandSpheres, g_fail);
    return g_fail;
}
