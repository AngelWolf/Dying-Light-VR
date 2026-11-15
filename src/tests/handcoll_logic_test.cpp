// Standalone unit test for src/src/ovr2xr/handcoll_logic.h - NOT part of the DLL build.
// Live-enemy hand collision: docs/superpowers/specs/2026-10-30-live-enemy-hand-collision-design.md, plan Task 2. Exit code = failures.
#include "handcoll_logic.h"

#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

using dlvr::handcoll::Admit;
using dlvr::handcoll::BuildLocalShape;
using dlvr::handcoll::ContactEdge;
using dlvr::handcoll::LocalShape;
using dlvr::handcoll::NearestSurface;
using dlvr::handcoll::PoseShape;
using dlvr::handcoll::ScaleToBone;
using dlvr::handcoll::ShapeKind;
using dlvr::handcoll::StopTarget;
using dlvr::handcoll::WorldShape;

static bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// mtx34 row-major: row r = [4r..4r+2], translation [4r+3].
static const float kIdentity[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };

static WorldShape Sphere(float x, float y, float z, float r)
{
    WorldShape s{};
    s.kind = ShapeKind::Sphere;
    s.a[0] = s.b[0] = x; s.a[1] = s.b[1] = y; s.a[2] = s.b[2] = z;
    s.r = r;
    return s;
}

static void sphere_from_extents()
{
    const float mn[3] = { -0.1f, -0.1f, -0.1f }, mx[3] = { 0.1f, 0.1f, 0.1f };
    LocalShape s{};
    CHECK(BuildLocalShape(ShapeKind::Sphere, mn, mx, 0.8f, &s));
    CHECK(s.kind == ShapeKind::Sphere);
    CHECK(Near(s.r, (0.2f * 0.8f + 0.01f) / 2.0f));   // 0.085
    CHECK(Near(s.c[0], 0.0f) && Near(s.c[1], 0.0f) && Near(s.c[2], 0.0f));
    CHECK(Near(s.halfLen, 0.0f));
}

static void capsule_from_extents()
{
    // sides 0.1 / 0.4 / 0.12 about centre (0.01, 0.02, 0.03)
    const float mn[3] = { 0.01f - 0.05f, 0.02f - 0.2f, 0.03f - 0.06f }, mx[3] = { 0.01f + 0.05f, 0.02f + 0.2f, 0.03f + 0.06f };
    LocalShape s{};
    CHECK(BuildLocalShape(ShapeKind::Capsule, mn, mx, 1.0f, &s));
    CHECK(s.kind == ShapeKind::Capsule);
    CHECK(s.axis == 1);
    CHECK(Near(s.r, 0.065f));                                 // max(0.11, 0.13) / 2
    CHECK(Near(s.halfLen, (0.41f - 0.065f + 0.08f) / 2.0f));  // 0.2125
    CHECK(Near(s.c[0], 0.01f) && Near(s.c[1], 0.02f) && Near(s.c[2], 0.03f));
}

static void capsule_scale_divides_length()
{
    const float mn[3] = { -0.05f, -0.2f, -0.06f }, mx[3] = { 0.05f, 0.2f, 0.06f };
    LocalShape s{};
    CHECK(BuildLocalShape(ShapeKind::Capsule, mn, mx, 0.7f, &s));
    const float dx = 0.1f * 0.7f + 0.01f, dy = 0.4f * 0.7f + 0.01f, dz = 0.12f * 0.7f + 0.01f;
    const float r = (dx > dz ? dx : dz) / 2.0f;
    CHECK(s.axis == 1);
    CHECK(Near(s.r, r));
    CHECK(Near(s.halfLen, ((dy - r) / 0.7f + 0.08f) / 2.0f));
}

static void capsule_axis_ties_go_x_then_y()
{
    // the engine's order (+3B0280): m == dX -> X, else m == dY -> Y, else Z
    const float mn[3] = { -0.1f, -0.1f, -0.05f }, mx[3] = { 0.1f, 0.1f, 0.05f };
    LocalShape s{};
    CHECK(BuildLocalShape(ShapeKind::Capsule, mn, mx, 1.0f, &s));
    CHECK(s.axis == 0);
    const float mz[3] = { -0.05f, -0.1f, -0.1f }, xz[3] = { 0.05f, 0.1f, 0.1f };
    CHECK(BuildLocalShape(ShapeKind::Capsule, mz, xz, 1.0f, &s));
    CHECK(s.axis == 1);
}

static void capsule_length_never_negative()
{
    // a near-cube: max(m - r, 0) keeps L at the engine's +0.08
    const float mn[3] = { -0.1f, -0.1f, -0.1f }, mx[3] = { 0.1f, 0.1f, 0.1f };
    LocalShape s{};
    CHECK(BuildLocalShape(ShapeKind::Capsule, mn, mx, 1.0f, &s));
    const float m = 0.21f, r = 0.105f;
    CHECK(Near(s.halfLen, ((m - r) / 1.0f + 0.08f) / 2.0f));
    CHECK(s.halfLen > 0.0f);
}

static void bad_scale_refused()
{
    const float mn[3] = { -0.1f, -0.1f, -0.1f }, mx[3] = { 0.1f, 0.1f, 0.1f };
    LocalShape s{};
    CHECK(!BuildLocalShape(ShapeKind::Sphere, mn, mx, 0.0f, &s));
    CHECK(!BuildLocalShape(ShapeKind::Capsule, mn, mx, -1.0f, &s));
    const float nanv = std::nanf("");
    const float bad[3] = { nanv, 0.1f, 0.1f };
    CHECK(!BuildLocalShape(ShapeKind::Sphere, mn, bad, 1.0f, &s));
}

static void scale_to_bone_applies_column_scale()
{
    // world 3x3 = diag(2, 1, 0.5): the engine's N^T * M maps a point by the per-axis scale
    const float m[12] = { 2, 0, 0, 5, 0, 1, 0, 6, 0, 0, 0.5f, 7 };
    const float mn[3] = { -0.1f, -0.2f, -0.4f }, mx[3] = { 0.1f, 0.2f, 0.4f };
    float omn[3], omx[3];
    CHECK(ScaleToBone(m, mn, mx, omn, omx));
    CHECK(Near(omn[0], -0.2f) && Near(omn[1], -0.2f) && Near(omn[2], -0.2f));
    CHECK(Near(omx[0], 0.2f) && Near(omx[1], 0.2f) && Near(omx[2], 0.2f));
    // a pure rotation leaves the box as it was
    const float rot[12] = { 0, 0, 1, 0, 0, 1, 0, 0, -1, 0, 0, 0 };
    CHECK(ScaleToBone(rot, mn, mx, omn, omx));
    CHECK(Near(omn[0], -0.1f) && Near(omn[1], -0.2f) && Near(omn[2], -0.4f));
    CHECK(Near(omx[0], 0.1f) && Near(omx[1], 0.2f) && Near(omx[2], 0.4f));
}

static void pose_identity_keeps_local()
{
    LocalShape s{};
    s.kind = ShapeKind::Capsule; s.c[0] = 0.1f; s.c[1] = 0.2f; s.c[2] = 0.3f; s.axis = 2; s.r = 0.05f; s.halfLen = 0.2f;
    WorldShape w{};
    PoseShape(s, kIdentity, &w);
    CHECK(w.kind == ShapeKind::Capsule);
    CHECK(Near(w.a[0], 0.1f) && Near(w.a[1], 0.2f) && Near(w.a[2], 0.1f));
    CHECK(Near(w.b[0], 0.1f) && Near(w.b[1], 0.2f) && Near(w.b[2], 0.5f));
    CHECK(Near(w.r, 0.05f));
}

static void pose_translates_and_rotates()
{
    // 90 deg about y: x -> -z, z -> x (row-major R = [[0,0,1],[0,1,0],[-1,0,0]]), translated by (1, 2, 3)
    const float m[12] = { 0, 0, 1, 1, 0, 1, 0, 2, -1, 0, 0, 3 };
    LocalShape s{};
    s.kind = ShapeKind::Capsule; s.c[0] = 0.5f; s.axis = 0; s.r = 0.1f; s.halfLen = 0.25f;
    WorldShape w{};
    PoseShape(s, m, &w);
    // centre (0.5,0,0) -> (0,0,-0.5) + t = (1, 2, 2.5); axis x -> (0,0,-1)
    CHECK(Near(w.a[0], 1.0f) && Near(w.a[1], 2.0f) && Near(w.a[2], 2.75f));
    CHECK(Near(w.b[0], 1.0f) && Near(w.b[1], 2.0f) && Near(w.b[2], 2.25f));
    // a scaled bone: the rotation is normalised, the translation kept
    const float ms[12] = { 0, 0, 2, 1, 0, 2, 0, 2, -2, 0, 0, 3 };
    PoseShape(s, ms, &w);
    CHECK(Near(w.a[2], 2.75f) && Near(w.b[2], 2.25f));
}

static void stop_on_sphere_surface()
{
    const float S[3] = { 0, 0, 0 }, T[3] = { 0, 0, 1 };
    const WorldShape sh = Sphere(0, 0, 0.8f, 0.1f);
    float out[3];
    int hit = -1;
    CHECK(StopTarget(S, T, &sh, 1, out, &hit));
    CHECK(hit == 0);
    CHECK(Near(out[0], 0.0f) && Near(out[1], 0.0f) && Near(out[2], 0.699f));
}

static void stop_on_capsule_side()
{
    // a vertical capsule at x = 0.5, from y -0.3 to 0.3, r 0.1; reach along +x at y = 0
    WorldShape c{};
    c.kind = ShapeKind::Capsule;
    c.a[0] = 0.5f; c.a[1] = -0.3f; c.a[2] = 0.0f;
    c.b[0] = 0.5f; c.b[1] = 0.3f;  c.b[2] = 0.0f;
    c.r = 0.1f;
    const float S[3] = { 0, 0, 0 }, T[3] = { 0.8f, 0, 0 };
    float out[3];
    CHECK(StopTarget(S, T, &c, 1, out, nullptr));
    CHECK(Near(out[0], 0.399f) && Near(out[1], 0.0f) && Near(out[2], 0.0f));
    // the same reach over the cap end: y = 0.35, enters the top cap sphere (centre y 0.3): x = 0.5 - sqrt(0.01 - 0.0025)
    const float S2[3] = { 0, 0.35f, 0 }, T2[3] = { 0.8f, 0.35f, 0 };
    CHECK(StopTarget(S2, T2, &c, 1, out, nullptr));
    CHECK(Near(out[0], 0.5f - std::sqrt(0.0075f) - 0.001f) && Near(out[1], 0.35f));
}

static void no_entry_returns_target_unchanged()
{
    const float S[3] = { 0, 0, 0 }, T[3] = { 0, 0, 0.5f };
    const WorldShape sh = Sphere(0, 0, 0.8f, 0.1f);
    float out[3];
    CHECK(!StopTarget(S, T, &sh, 1, out, nullptr));
    CHECK(out[0] == T[0] && out[1] == T[1] && out[2] == T[2]);
    // a miss to the side
    const WorldShape side = Sphere(0.5f, 0, 0.3f, 0.1f);
    CHECK(!StopTarget(S, T, &side, 1, out, nullptr));
}

static void hand_past_body_stays_at_entry()
{
    const float S[3] = { 0, 0, 0 }, T[3] = { 0, 0, 1.2f };   // beyond the far side (0.9)
    const WorldShape sh = Sphere(0, 0, 0.8f, 0.1f);
    float out[3];
    CHECK(StopTarget(S, T, &sh, 1, out, nullptr));
    CHECK(Near(out[2], 0.699f));
}

static void nearest_of_two_shapes_wins()
{
    const float S[3] = { 0, 0, 0 }, T[3] = { 0, 0, 1.0f };
    const WorldShape sh[2] = { Sphere(0, 0, 0.8f, 0.1f), Sphere(0, 0, 0.5f, 0.05f) };
    float out[3];
    int hit = -1;
    CHECK(StopTarget(S, T, sh, 2, out, &hit));
    CHECK(hit == 1);
    CHECK(Near(out[2], 0.449f));
}

static void shoulder_inside_shape_ignored()
{
    const float S[3] = { 0, 0, 0 }, T[3] = { 0, 0, 1.0f };
    const WorldShape sh[2] = { Sphere(0, 0, 0.05f, 0.2f), Sphere(0, 0, 0.8f, 0.1f) };   // the first holds S
    float out[3];
    int hit = -1;
    CHECK(StopTarget(S, T, sh, 2, out, &hit));
    CHECK(hit == 1);
    CHECK(Near(out[2], 0.699f));
    // only the containing shape: no stop at all
    CHECK(!StopTarget(S, T, sh, 1, out, nullptr));
    CHECK(out[2] == T[2]);
}

// The reach (push_grab_reach_m): a fingertip a few cm short of a surface counts for the push / grab.
static void reach_sphere_within_reach()
{
    const float S[3] = { 1, 0, 0 }, p[3] = { 0.13f, 0, 0 };
    const WorldShape sh = Sphere(0, 0, 0, 0.1f);
    float gap = -1, surf[3] = {}, nrm[3] = {}, nearest = -1;
    int hit = -1;
    CHECK(NearestSurface(S, p, &sh, 1, 0.05f, &gap, &hit, surf, nrm, &nearest));
    CHECK(hit == 0);
    CHECK(Near(gap, 0.03f) && Near(nearest, 0.03f));
    CHECK(Near(surf[0], 0.1f) && Near(surf[1], 0.0f) && Near(surf[2], 0.0f));
    CHECK(Near(nrm[0], 1.0f) && Near(nrm[1], 0.0f) && Near(nrm[2], 0.0f));
}

static void reach_beyond_reach_refused_but_nearest_reported()
{
    const float S[3] = { 1, 0, 0 }, p[3] = { 0.2f, 0, 0 };
    const WorldShape sh = Sphere(0, 0, 0, 0.1f);
    float gap = -1, surf[3] = {}, nrm[3] = {}, nearest = -1;
    int hit = 7;
    CHECK(!NearestSurface(S, p, &sh, 1, 0.05f, &gap, &hit, surf, nrm, &nearest));
    CHECK(hit == -1);
    CHECK(Near(nearest, 0.1f));
}

static void reach_capsule_side_normal()
{
    const float S[3] = { 0, 0, 1 }, p[3] = { 0, 0.1f, 0.08f };
    WorldShape sh{};
    sh.kind = ShapeKind::Capsule;
    sh.a[1] = -0.2f;
    sh.b[1] = 0.2f;
    sh.r = 0.05f;
    float gap = -1, surf[3] = {}, nrm[3] = {};
    int hit = -1;
    CHECK(NearestSurface(S, p, &sh, 1, 0.05f, &gap, &hit, surf, nrm, nullptr));
    CHECK(Near(gap, 0.03f));
    CHECK(Near(surf[0], 0.0f) && Near(surf[1], 0.1f) && Near(surf[2], 0.05f));
    CHECK(Near(nrm[0], 0.0f) && Near(nrm[1], 0.0f) && Near(nrm[2], 1.0f));
}

static void reach_nearest_gap_of_two_wins()
{
    const float S[3] = { 0, 0, 0 }, p[3] = { 0, 0, 0.5f };
    const WorldShape sh[2] = { Sphere(0, 0, 0.6f, 0.06f), Sphere(0, 0, 0.55f, 0.03f) };   // gaps 0.04 and 0.02
    float gap = -1, surf[3] = {}, nrm[3] = {};
    int hit = -1;
    CHECK(NearestSurface(S, p, sh, 2, 0.05f, &gap, &hit, surf, nrm, nullptr));
    CHECK(hit == 1);
    CHECK(Near(gap, 0.02f));
    CHECK(Near(nrm[2], -1.0f));
}

static void reach_ignores_shape_holding_shoulder()
{
    const float S[3] = { 0, 0, 0 }, p[3] = { 0, 0, 0.23f };
    const WorldShape sh = Sphere(0, 0, 0.05f, 0.2f);   // holds S; p is 0.03 outside it
    float gap = -1, surf[3] = {}, nrm[3] = {}, nearest = 5;
    int hit = -1;
    CHECK(!NearestSurface(S, p, &sh, 1, 0.05f, &gap, &hit, surf, nrm, &nearest));
    CHECK(nearest < 0.0f);
}

static void reach_zero_is_off()
{
    const float S[3] = { 1, 0, 0 }, p[3] = { 0.1001f, 0, 0 };
    const WorldShape sh = Sphere(0, 0, 0, 0.1f);
    float gap = -1, surf[3] = {}, nrm[3] = {}, nearest = -1;
    int hit = -1;
    CHECK(!NearestSurface(S, p, &sh, 1, 0.0f, &gap, &hit, surf, nrm, &nearest));
    CHECK(Near(nearest, 0.0001f));
    CHECK(!NearestSurface(S, p, nullptr, 0, 0.05f, &gap, &hit, surf, nrm, &nearest));
    CHECK(nearest < 0.0f);
}

static void release_returns_target_unchanged()
{
    const float S[3] = { 0, 0, 0 }, T[3] = { 0.1f, 0.2f, 0.3f };
    float out[3] = { 9, 9, 9 };
    CHECK(!StopTarget(S, T, nullptr, 0, out, nullptr));
    CHECK(out[0] == T[0] && out[1] == T[1] && out[2] == T[2]);
}

static void one_pulse_per_touch()
{
    ContactEdge e;
    int edges = 0;
    double t = 0.0;
    for (int f = 0; f < 30; ++f) {
        const bool in = (f % 5) != 3;   // a 1-frame dropout every 5 frames (11 ms each)
        if (e.Update(in, t)) { ++edges; }
        t += 0.011;
    }
    CHECK(edges == 1);
}

static void rearm_after_release()
{
    ContactEdge e;
    int edges = 0;
    double t = 0.0;
    for (int f = 0; f < 10; ++f) { if (e.Update(true, t)) { ++edges; } t += 0.011; }
    for (int f = 0; f < 19; ++f) { if (e.Update(false, t)) { ++edges; } t += 0.011; }   // 0.209 s out
    for (int f = 0; f < 10; ++f) { if (e.Update(true, t)) { ++edges; } t += 0.011; }
    CHECK(edges == 2);
}

// Review F2: the tracked set is the NEAREST enemies, not the first ones the level query happened to return.
static void admit_fills_free_slot()
{
    const float d[6] = { 0.5f };
    int r = -2;
    CHECK(Admit(d, 1, 6, 2.0f, &r));
    CHECK(r == -1);   // a free slot: nothing replaced
}

static void admit_replaces_farthest_when_nearer()
{
    const float d[3] = { 1.0f, 2.0f, 1.5f };
    int r = -2;
    CHECK(Admit(d, 3, 3, 0.8f, &r));
    CHECK(r == 1);    // the 2.0 m one goes
}

static void admit_refuses_when_farther_than_all()
{
    const float d[3] = { 1.0f, 2.0f, 1.5f };
    int r = -2;
    CHECK(!Admit(d, 3, 3, 2.5f, &r));
    CHECK(r == -1);
}

static void admit_tie_keeps_existing()
{
    const float d[2] = { 1.0f, 2.0f };
    int r = -2;
    CHECK(!Admit(d, 2, 2, 2.0f, &r));   // no churn on an equal distance
    CHECK(r == -1);
}

static void admit_unknown_distance_is_farthest()
{
    // a tracked enemy whose distance could not be read (< 0) is the first to give way
    const float d[3] = { 1.0f, -1.0f, 1.5f };
    int r = -2;
    CHECK(Admit(d, 3, 3, 1.9f, &r));
    CHECK(r == 1);
}

int main()
{
    admit_fills_free_slot();
    admit_replaces_farthest_when_nearer();
    admit_refuses_when_farther_than_all();
    admit_tie_keeps_existing();
    admit_unknown_distance_is_farthest();
    sphere_from_extents();
    capsule_from_extents();
    capsule_scale_divides_length();
    capsule_axis_ties_go_x_then_y();
    capsule_length_never_negative();
    bad_scale_refused();
    scale_to_bone_applies_column_scale();
    pose_identity_keeps_local();
    pose_translates_and_rotates();
    stop_on_sphere_surface();
    stop_on_capsule_side();
    no_entry_returns_target_unchanged();
    hand_past_body_stays_at_entry();
    nearest_of_two_shapes_wins();
    shoulder_inside_shape_ignored();
    release_returns_target_unchanged();
    reach_sphere_within_reach();
    reach_beyond_reach_refused_but_nearest_reported();
    reach_capsule_side_normal();
    reach_nearest_gap_of_two_wins();
    reach_ignores_shape_holding_shoulder();
    reach_zero_is_off();
    one_pulse_per_touch();
    rearm_after_release();
    if (g_fail == 0) { std::printf("handcoll_logic_test: ALL PASS\n"); }
    else { std::printf("handcoll_logic_test: %d FAILURE(S)\n", g_fail); }
    return g_fail;
}
