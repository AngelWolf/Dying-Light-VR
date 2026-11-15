// Standalone unit test for src/src/ovr2xr/bladeline_logic.h - NOT part of the DLL build.
// docs/superpowers/specs/2026-10-26-blade-length-hit-line-design.md §4.2-4.3 (the line along the drawn weapon) and §7 (revision 2:
// the oh_ boxes, the minimum reach). Exit code = number of failures.
#include "bladeline_logic.h"

#include <cmath>
#include <cstdio>
#include <limits>

using namespace dlvr::bladeline;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static bool Near(float a, float b, float eps) { return std::fabs(a - b) < eps; }
static bool Near3(const float* p, float x, float y, float z, float eps) { return Near(p[0], x, eps) && Near(p[1], y, eps) && Near(p[2], z, eps); }

// g_getWorld 3x4 layout: rows, translation at 3 / 7 / 11.
static void Identity(float m[12])
{
    const float id[12] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0 };
    for (int i = 0; i < 12; ++i) { m[i] = id[i]; }
}

static const BladeRow kAxe = { "wn_axe_e", { 0.0f, 1.0f, 0.0f }, 0.744f, 0, 0 };

static void find_row_basics()
{
    const BladeRow rows[2] = { { "wn_axe_e", { 0, 1, 0 }, 0.744f, 0, 0 }, { "wn_long_knife_e", { 0, 1, 0 }, 0.427f, 0, 0 } };
    CHECK(FindRow("wn_long_knife_e", rows, 2) == 1);
    CHECK(FindRow("wn_axe_e", rows, 2) == 0);
    CHECK(FindRow("wn_nothing", rows, 2) == -1);
    CHECK(FindRow(nullptr, rows, 2) == -1);
    CHECK(FindRow("", rows, 2) == -1);
}

static void shaft_world_identity_and_rotation()
{
    float m[12];
    Identity(m);
    const float d[3] = { 0.0f, 0.6f, 0.8f };
    float o[3];
    ShaftWorld(m, d, o);
    CHECK(Near(o[0], 0.0f, 1e-5f) && Near(o[1], 0.6f, 1e-5f) && Near(o[2], 0.8f, 1e-5f));
    // 90 deg about Y (rows): x' = z, z' = -x
    const float ry[12] = { 0, 0, 1, 0,  0, 1, 0, 0,  -1, 0, 0, 0 };
    const float dx[3] = { 1.0f, 0.0f, 0.0f };
    ShaftWorld(ry, dx, o);
    CHECK(Near(o[0], 0.0f, 1e-5f) && Near(o[1], 0.0f, 1e-5f) && Near(o[2], -1.0f, 1e-5f));
}

static void segment_one_handed_axe()
{
    float m[12];
    Identity(m);
    const float grip[3] = { 0.0f, 0.363f, 0.0f };
    float s[3], e[3], along = -1.0f;
    CHECK(Segment(m, kAxe, grip, 0.10f, 0.0f, s, e, &along));
    CHECK(Near(s[1], 0.363f, 1e-5f) && Near(e[1], 0.844f, 1e-5f) && Near(along, 0.363f, 1e-5f));
}

static void segment_two_handed_axe()
{
    float m[12];
    Identity(m);
    const float grip[3] = { 0.0f, 0.013f, 0.0f };
    float s[3], e[3], along = -1.0f;
    CHECK(Segment(m, kAxe, grip, 0.10f, 0.0f, s, e, &along));
    CHECK(Near(s[1], 0.013f, 1e-5f) && Near(e[1], 0.844f, 1e-5f));
}

static void segment_grip_behind_holder_starts_at_holder()
{
    float m[12];
    Identity(m);
    m[3] = 1.0f; m[7] = 2.0f; m[11] = 3.0f;   // Holder away from the origin
    const float grip[3] = { 1.0f, 1.95f, 3.0f };
    float s[3], e[3], along = -1.0f;
    CHECK(Segment(m, kAxe, grip, 0.10f, 0.0f, s, e, &along));
    CHECK(Near(along, 0.0f, 1e-6f) && Near(s[0], 1.0f, 1e-5f) && Near(s[1], 2.0f, 1e-5f) && Near(s[2], 3.0f, 1e-5f));
    CHECK(Near(e[1], 2.844f, 1e-5f));
}

static void segment_grip_past_tip_clamped()
{
    float m[12];
    Identity(m);
    const float grip[3] = { 0.0f, 0.9f, 0.0f };
    float s[3], e[3], along = -1.0f;
    CHECK(Segment(m, kAxe, grip, 0.10f, 0.0f, s, e, &along));
    CHECK(Near(along, 0.734f, 1e-5f) && Near(s[1], 0.734f, 1e-5f) && Near(e[1], 0.844f, 1e-5f));
}

static void segment_margin_bounds()
{
    float m[12];
    Identity(m);
    const float grip[3] = { 0.0f, 0.0f, 0.0f };
    float s[3], e[3], along = 0.0f;
    CHECK(Segment(m, kAxe, grip, 0.0f, 0.0f, s, e, &along) && Near(e[1], 0.744f, 1e-5f));
    CHECK(Segment(m, kAxe, grip, 0.30f, 0.0f, s, e, &along) && Near(e[1], 1.044f, 1e-5f));
}

static void segment_tilted_holder()
{
    // The tool's convention: dirLocal = R^T (0,1,0) with R the Holder's rotation in mesh space; the engine's Holder world =
    // the weapon root's world x R. With the root at identity, the Holder world rotation IS R, and dirW must come out (0,1,0).
    const float a = 10.82f * 3.14159265f / 180.0f;
    const float c = std::cos(a), sn = std::sin(a);
    const float R[12] = { 1, 0, 0, 0,  0, c, -sn, 0,  0, sn, c, 0 };   // about X, rows
    const BladeRow r = { "tilted", { R[4], R[5], R[6] }, 0.5f, 0, 0 };  // R's second ROW = R^T (0,1,0)
    const float grip[3] = { 0.0f, 0.2f, 0.0f };
    float s[3], e[3], along = 0.0f;
    CHECK(Segment(R, r, grip, 0.0f, 0.0f, s, e, &along));
    CHECK(Near(e[0], 0.0f, 1e-4f) && Near(e[1], 0.5f, 1e-4f) && Near(e[2], 0.0f, 1e-4f));
}

static void pick_pair_falls_back_when_either_side_lacks_a_row()
{
    Seg a{ true, { 0, 0, 0 }, { 0, 0.8f, 0 } };
    Seg b{ false, { 9, 9, 9 }, { 9, 9, 9 } };
    const float ag[3] = { 0, 0, 0 }, ad[3] = { 0, 0, -1 }, bg[3] = { 1, 0, 0 }, bd[3] = { 0, 0, -1 };
    Seg oa{}, ob{};
    PickPair(a, b, ag, ad, bg, bd, 1.5f, &oa, &ob);
    CHECK(!oa.blade && !ob.blade);
    CHECK(Near(oa.e[2], -1.5f, 1e-5f) && Near(ob.s[0], 1.0f, 1e-5f) && Near(ob.e[2], -1.5f, 1e-5f));
}

static void pick_pair_mixed_is_fallback()
{
    Seg a{ false, {}, {} };
    Seg b{ true, { 0, 0, 0 }, { 0, 0.8f, 0 } };
    const float g[3] = { 0, 0, 0 }, d[3] = { 1, 0, 0 };
    Seg oa{}, ob{};
    PickPair(a, b, g, d, g, d, 2.0f, &oa, &ob);
    CHECK(!oa.blade && !ob.blade && Near(ob.e[0], 2.0f, 1e-5f));
    Seg c{ true, { 0, 0, 0 }, { 0, 0.5f, 0 } };
    PickPair(b, c, g, d, g, d, 2.0f, &oa, &ob);
    CHECK(oa.blade && ob.blade && Near(oa.e[1], 0.8f, 1e-6f) && Near(ob.e[1], 0.5f, 1e-6f));
}

static void nonfinite_refuses()
{
    float m[12];
    Identity(m);
    m[5] = std::numeric_limits<float>::quiet_NaN();
    const float grip[3] = { 0, 0, 0 };
    float s[3], e[3], along = 0.0f;
    CHECK(!Segment(m, kAxe, grip, 0.1f, 0.0f, s, e, &along));
}

// ===== Revision 2 (spec §7) =====

static void box_two_edges()
{
    float m[12];
    Identity(m);
    const BladeBox b = { { 0.0f, 0.3f, 0.0f }, { 0.0f, 0.2f, 0.0f }, { 0.0f, 0.0f, 0.04f } };
    float s[2][3], e[2][3];
    CHECK(BoxSegments(m, b, s, e) == 2);
    CHECK(Near3(s[0], 0.0f, 0.1f, 0.04f, 1e-5f) && Near3(e[0], 0.0f, 0.5f, 0.04f, 1e-5f));
    CHECK(Near3(s[1], 0.0f, 0.1f, -0.04f, 1e-5f) && Near3(e[1], 0.0f, 0.5f, -0.04f, 1e-5f));
}

static void box_thin_is_one_segment()
{
    float m[12];
    Identity(m);
    float s[2][3], e[2][3];
    const BladeBox thin = { { 0.0f, 0.3f, 0.0f }, { 0.0f, 0.2f, 0.0f }, { 0.0f, 0.0f, 0.01f } };   // authored 2|w| = 0.02
    CHECK(BoxSegments(m, thin, s, e) == 1);
    CHECK(Near3(s[0], 0.0f, 0.1f, 0.0f, 1e-5f) && Near3(e[0], 0.0f, 0.5f, 0.0f, 1e-5f));
    const BladeBox zero = { { 0.0f, 0.3f, 0.0f }, { 0.0f, 0.2f, 0.0f }, { 0.0f, 0.0f, 0.0f } };
    CHECK(BoxSegments(m, zero, s, e) == 1);
    CHECK(Near3(s[0], 0.0f, 0.1f, 0.0f, 1e-5f) && Near3(e[0], 0.0f, 0.5f, 0.0f, 1e-5f));
    const BladeBox edge = { { 0.0f, 0.3f, 0.0f }, { 0.0f, 0.2f, 0.0f }, { 0.0f, 0.0f, 0.025f } };  // 2|w| = 0.05, not < 0.05
    CHECK(BoxSegments(m, edge, s, e) == 2);
    CHECK(Near(s[0][2], 0.025f, 1e-5f) && Near(s[1][2], -0.025f, 1e-5f));
}

static void box_padding_long_axis()
{
    float m[12];
    Identity(m);
    const BladeBox b = { { 0.0f, 0.3f, 0.0f }, { 0.0f, 0.01f, 0.0f }, { 0.0f, 0.0f, 0.0f } };
    float s[2][3], e[2][3];
    CHECK(BoxSegments(m, b, s, e) == 1);
    CHECK(Near(s[0][1], 0.275f, 1e-5f) && Near(e[0][1], 0.325f, 1e-5f));
}

static void box_world_transform()
{
    // Holder translated (1, 2, 3) and yawed 90 deg about Y (rows: x' = z, z' = -x).
    const float m[12] = { 0, 0, 1, 1,  0, 1, 0, 2,  -1, 0, 0, 3 };
    const BladeBox b = { { 0.1f, 0.3f, 0.0f }, { 0.0f, 0.2f, 0.0f }, { 0.04f, 0.0f, 0.0f } };
    float s[2][3], e[2][3];
    CHECK(BoxSegments(m, b, s, e) == 2);
    // segment 0 = c - l + w = (0.14, 0.1, 0) -> world (1 + 0, 2 + 0.1, 3 - 0.14)
    CHECK(Near3(s[0], 1.0f, 2.1f, 2.86f, 1e-5f) && Near3(e[0], 1.0f, 2.5f, 2.86f, 1e-5f));
    // segment 1 = c - l - w = (0.06, 0.1, 0) -> (1, 2.1, 2.94)
    CHECK(Near3(s[1], 1.0f, 2.1f, 2.94f, 1e-5f) && Near3(e[1], 1.0f, 2.5f, 2.94f, 1e-5f));
}

static const BladeBox kThree[3] = {
    { { 0.0f, 0.3f, 0.0f }, { 0.0f, 0.2f, 0.0f }, { 0.0f, 0.0f, 0.04f } },   // 2 segments
    { { 0.0f, 0.6f, 0.0f }, { 0.0f, 0.1f, 0.0f }, { 0.0f, 0.0f, 0.0f } },    // 1
    { { 0.0f, 0.7f, 0.0f }, { 0.0f, 0.0f, 0.1f }, { 0.0f, 0.05f, 0.0f } },   // 2
};

static void row_segments_concatenates_and_caps()
{
    float m[12];
    Identity(m);
    const BladeRow r = { "three", { 0, 1, 0 }, 0.8f, 0, 3 };
    float s[kMaxBoxSegs][3], e[kMaxBoxSegs][3];
    CHECK(RowSegments(m, r, kThree, 3, s, e, kMaxBoxSegs) == 5);
    CHECK(Near3(s[0], 0.0f, 0.1f, 0.04f, 1e-5f));    // box 0 segment 0
    CHECK(Near3(s[2], 0.0f, 0.5f, 0.0f, 1e-5f));     // box 1 (the only one)
    CHECK(Near3(s[3], 0.0f, 0.75f, -0.1f, 1e-5f));   // box 2 segment 0 = c - l + w
    const BladeRow r1 = { "one", { 0, 1, 0 }, 0.8f, 1, 1 };
    CHECK(RowSegments(m, r1, kThree, 3, s, e, kMaxBoxSegs) == 1 && Near3(s[0], 0.0f, 0.5f, 0.0f, 1e-5f));
}

static void row_segments_caps_at_max()
{
    float m[12];
    Identity(m);
    const BladeRow r = { "three", { 0, 1, 0 }, 0.8f, 0, 3 };
    float s[kMaxBoxSegs][3], e[kMaxBoxSegs][3];
    CHECK(RowSegments(m, r, kThree, 3, s, e, 3) == 3);
    const BladeRow past = { "past", { 0, 1, 0 }, 0.8f, 2, 5 };      // box0 + boxN > nBoxes: clamped to box 2 only
    CHECK(RowSegments(m, past, kThree, 3, s, e, kMaxBoxSegs) == 2);
    const BladeRow none = { "none", { 0, 1, 0 }, 0.8f, 7, 2 };      // box0 past the array: nothing read
    CHECK(RowSegments(m, none, kThree, 3, s, e, kMaxBoxSegs) == 0);
    const BladeRow zero = { "zero", { 0, 1, 0 }, 0.8f, 0, 0 };
    CHECK(RowSegments(m, zero, kThree, 3, s, e, kMaxBoxSegs) == 0);
}

static void box_pair_needs_same_row_both_ok()
{
    BoxFrame a{ true, 4, {} }, b{ true, 4, {} };
    CHECK(BoxPairOk(a, b));
    b.row = 5;
    CHECK(!BoxPairOk(a, b));
    b.row = 4;
    b.ok = false;
    CHECK(!BoxPairOk(a, b));
    BoxFrame c{ true, -1, {} }, d{ true, -1, {} };
    CHECK(!BoxPairOk(c, d));
}

static void segment_min_reach_extends_short_weapon()
{
    float m[12];
    Identity(m);
    const BladeRow claw = { "claw", { 0, 1, 0 }, 0.10f, 0, 0 };
    const float grip[3] = { 0.0f, 0.0f, 0.0f };
    float s[3], e[3], along = -1.0f;
    CHECK(Segment(m, claw, grip, 0.10f, 0.65f, s, e, &along));
    CHECK(Near(along, 0.0f, 1e-6f) && Near(e[1], 0.65f, 1e-5f));
}

static void segment_min_reach_not_shorter()
{
    float m[12];
    Identity(m);
    const float grip[3] = { 0.0f, 0.013f, 0.0f };
    float s[3], e[3], along = -1.0f;
    CHECK(Segment(m, kAxe, grip, 0.10f, 0.65f, s, e, &along));
    CHECK(Near(e[1], 0.844f, 1e-5f));
    const float high[3] = { 0.0f, 0.363f, 0.0f };   // one-handed: hand + 0.65 = 1.013 > tip + margin 0.844
    CHECK(Segment(m, kAxe, high, 0.10f, 0.65f, s, e, &along) && Near(e[1], 1.013f, 1e-5f));
}

static void segment_min_reach_off()
{
    float m[12];
    Identity(m);
    const float grip[3] = { 0.0f, 0.2f, 0.0f };
    float s[3], e[3], along = -1.0f;
    CHECK(Segment(m, kAxe, grip, 0.0f, 0.0f, s, e, &along) && Near(e[1], 0.744f, 1e-5f));
}

static void nonfinite_box_refuses()
{
    float m[12];
    Identity(m);
    BladeBox b = { { 0.0f, 0.3f, 0.0f }, { 0.0f, 0.2f, 0.0f }, { 0.0f, 0.0f, 0.04f } };
    b.c[1] = std::numeric_limits<float>::quiet_NaN();
    float s[2][3], e[2][3];
    CHECK(BoxSegments(m, b, s, e) == 0);
    BladeBox ok = { { 0.0f, 0.3f, 0.0f }, { 0.0f, 0.2f, 0.0f }, { 0.0f, 0.0f, 0.04f } };
    m[0] = std::numeric_limits<float>::infinity();
    CHECK(BoxSegments(m, ok, s, e) == 0);
}

// ===== §9 - the capsule and the factor restore =====

static void capsule_extends_both_ends()
{
    float s[3] = { 0.0f, 0.363f, 0.0f }, e[3] = { 0.0f, 1.013f, 0.0f };
    CHECK(Capsule(s, e, 0.30f, 0.20f));
    CHECK(Near3(s, 0.0f, 0.063f, 0.0f, 1e-5f));          // 0.30 back toward the elbow
    CHECK(Near3(e, 0.0f, 1.213f, 0.0f, 1e-5f));          // 0.20 further past the end
}

static void capsule_zero_is_unchanged_and_degenerate_refused()
{
    float s[3] = { 1.0f, 2.0f, 3.0f }, e[3] = { 1.0f, 2.5f, 3.0f };
    CHECK(Capsule(s, e, 0.0f, 0.0f));
    CHECK(Near3(s, 1.0f, 2.0f, 3.0f, 1e-6f) && Near3(e, 1.0f, 2.5f, 3.0f, 1e-6f));
    float p[3] = { 1.0f, 1.0f, 1.0f }, q[3] = { 1.0f, 1.0f, 1.0f };
    CHECK(!Capsule(p, q, 0.3f, 0.2f));                    // zero length: refused, untouched
    CHECK(Near3(p, 1.0f, 1.0f, 1.0f, 1e-6f));
}

static void thick_copies_offset_across_the_swing_plane()
{
    // frame a: the line along +Y; frame b: the same line moved +X (the swing). Plane normal = dir x motion = Y x X = -Z.
    const Seg a{ true, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    const Seg b{ true, { 0.1f, 0.0f, 0.0f }, { 0.1f, 1.0f, 0.0f } };
    float as[2][3], ae[2][3], bs[2][3], be[2][3];
    CHECK(ThickCopies(a, b, 0.20f, as, ae, bs, be) == 2);
    CHECK(Near3(as[0], 0.0f, 0.0f, -0.20f, 1e-5f) && Near3(ae[0], 0.0f, 1.0f, -0.20f, 1e-5f));
    CHECK(Near3(bs[0], 0.1f, 0.0f, -0.20f, 1e-5f) && Near3(be[0], 0.1f, 1.0f, -0.20f, 1e-5f));
    CHECK(Near3(as[1], 0.0f, 0.0f, 0.20f, 1e-5f) && Near3(be[1], 0.1f, 1.0f, 0.20f, 1e-5f));
}

static void thick_copies_none_without_motion_or_radius()
{
    const Seg a{ true, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    float as[2][3], ae[2][3], bs[2][3], be[2][3];
    CHECK(ThickCopies(a, a, 0.20f, as, ae, bs, be) == 0);   // no motion: no plane
    const Seg b{ true, { 0.1f, 0.0f, 0.0f }, { 0.1f, 1.0f, 0.0f } };
    CHECK(ThickCopies(a, b, 0.0f, as, ae, bs, be) == 0);    // radius 0: off
    const Seg c{ false, { 0.1f, 0.0f, 0.0f }, { 0.1f, 1.0f, 0.0f } };
    CHECK(ThickCopies(a, c, 0.20f, as, ae, bs, be) == 0);   // either frame off the blade line: none
}

static void restored_factor_is_the_games_own_line()
{
    const float g[3] = { 0.0f, 0.0f, 0.0f };
    const float c[3] = { 0.0f, 0.5f, 0.0f };
    CHECK(Near(RestoredFactor(0.1f, c, g, 2.0f), 0.75f, 1e-5f));     // 1 - 0.5 / 2.0
    const float far[3] = { 0.0f, 3.0f, 0.0f };
    CHECK(Near(RestoredFactor(0.1f, far, g, 2.0f), 0.0f, 1e-6f));    // clamped
    CHECK(Near(RestoredFactor(0.1f, g, g, 2.0f), 1.0f, 1e-6f));
    CHECK(Near(RestoredFactor(1.0f, c, g, 2.0f), 1.0f, 1e-6f));      // the default 1.0 (not computed) is left
    CHECK(Near(RestoredFactor(0.3f, c, g, 0.0f), 0.3f, 1e-6f));      // no reach: left
}

int main()
{
    find_row_basics();
    shaft_world_identity_and_rotation();
    segment_one_handed_axe();
    segment_two_handed_axe();
    segment_grip_behind_holder_starts_at_holder();
    segment_grip_past_tip_clamped();
    segment_margin_bounds();
    segment_tilted_holder();
    pick_pair_falls_back_when_either_side_lacks_a_row();
    pick_pair_mixed_is_fallback();
    nonfinite_refuses();
    box_two_edges();
    box_thin_is_one_segment();
    box_padding_long_axis();
    box_world_transform();
    row_segments_concatenates_and_caps();
    row_segments_caps_at_max();
    box_pair_needs_same_row_both_ok();
    segment_min_reach_extends_short_weapon();
    segment_min_reach_not_shorter();
    segment_min_reach_off();
    nonfinite_box_refuses();
    capsule_extends_both_ends();
    capsule_zero_is_unchanged_and_degenerate_refused();
    thick_copies_offset_across_the_swing_plane();
    thick_copies_none_without_motion_or_radius();
    restored_factor_is_the_games_own_line();
    std::printf("bladeline_logic_test: 27 tests, %d failure(s)\n", g_fail);
    return g_fail;
}
