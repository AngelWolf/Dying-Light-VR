// Standalone unit test for src/src/ovr2xr/twohand_logic.h - NOT part of the DLL build.
// docs/superpowers/plans/2026-10-09-two-handed-grip.md Task 2. Exit code = number of failures.
#include "twohand_logic.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace dlvr::twohand;

namespace {
#include "twohand_meshes.inc"
}
namespace {
#include "twohand_melee.inc"
}
namespace {
#include "twohand_grip.inc"
}

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
static bool NearV(V3 a, V3 b, float eps = 1e-3f) { return Near(a.x, b.x, eps) && Near(a.y, b.y, eps) && Near(a.z, b.z, eps); }
static bool SameRot(Q4 a, Q4 b, float deg = 0.05f) { return QAngleDeg(a, b) <= deg; }
static Q4 AxisAngle(V3 axis, float deg)
{
    const float l = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    const float h = deg * 0.00872664626f;
    const float s = std::sin(h) / l;
    return { axis.x * s, axis.y * s, axis.z * s, std::cos(h) };
}
static const Q4 kId{ 0, 0, 0, 1 };

// ---- mesh names ----
static void norm_mesh_strips_case_msh_and_hq()
{
    char o[96];
    CHECK(NormMesh("WN_Rifle_A_HQ.msh", o, sizeof(o)) && !std::strcmp(o, "wn_rifle_a"));
    CHECK(NormMesh("weapon_hammer_big_a_thug.msh.msh", o, sizeof(o)) && !std::strcmp(o, "weapon_hammer_big_a_thug"));
    CHECK(NormMesh("wn_electric_guitar_a_hq.msh", o, sizeof(o)) && !std::strcmp(o, "wn_electric_guitar_a"));
    CHECK(NormMesh("hand_r.msh", o, sizeof(o)) && !std::strcmp(o, "hand_r"));
    CHECK(!NormMesh("", o, sizeof(o)));
    CHECK(!NormMesh(nullptr, o, sizeof(o)));
    char tiny[4];
    CHECK(!NormMesh("wn_rifle_a.msh", tiny, sizeof(tiny)) && tiny[0] == 0);   // does not fit: refused, not truncated
}

// ---- classification ----
static const char* const kT[] = { "wn_bow_composite_a", "wn_rifle_a", "wn_scythe_a", nullptr };
static const char* const kA[] = { "wn_baseballbat_a", nullptr };
static Kind K(const char* cls, const char* mesh) { return Classify(cls, mesh, kT, 3, kA, 1).kind; }
static void classify_rules()
{
    CHECK(K("FireWeaponVis", "wn_rifle_a") == Kind::Firearm);
    CHECK(K("WeaponVis", "wn_scythe_a") == Kind::Melee);
    CHECK(K("BowVis", "wn_bow_composite_a") == Kind::None);        // in the table, refused by CLASS
    CHECK(K("CrossbowVis", "wn_rifle_a") == Kind::None);           // refused by CLASS whatever the mesh
    CHECK(K("FireWeaponVis", "wn_pistol_a") == Kind::None);        // TwoHanded(false)
    CHECK(K("WeaponVis", "wn_baseballbat_a") == Kind::None);       // ambiguous in the data
    CHECK(K("SomethingVis", "wn_rifle_a") == Kind::None);
    CHECK(K("FireWeaponVis", nullptr) == Kind::None);
    CHECK(K(nullptr, "wn_rifle_a") == Kind::None);
    CHECK(Classify("BowVis", "wn_bow_composite_a", kT, 3, kA, 1).why[0] != 0);   // every refusal NAMES its reason
}
static void generated_table_selects_rifles_not_pistols()
{
    CHECK(Classify("FireWeaponVis", "wn_rifle_a", kTwoHandedMeshes, kTwoHandedMeshesN,
                   kTwoHandedAmbiguous, kTwoHandedAmbiguousN).kind == Kind::Firearm);
    CHECK(Classify("FireWeaponVis", "wn_pistol_a", kTwoHandedMeshes, kTwoHandedMeshesN,
                   kTwoHandedAmbiguous, kTwoHandedAmbiguousN).kind == Kind::None);
}

// ---- grab zone ----
static void gun_zone_bounds()
{
    const V3 r{ 0, 0, 0 }, f{ 0, 0, -1 };
    CHECK(GunZone(r, f, { 0.00f, -0.05f, -0.30f }, 0.10f, 0.60f, 0.08f).in);
    CHECK(!GunZone(r, f, { 0.00f, -0.09f, -0.30f }, 0.10f, 0.60f, 0.08f).in);   // too far off the line
    CHECK(!GunZone(r, f, { 0.00f, 0.00f, -0.05f }, 0.10f, 0.60f, 0.08f).in);    // too close to the right hand
    CHECK(!GunZone(r, f, { 0.00f, 0.00f, -0.70f }, 0.10f, 0.60f, 0.08f).in);    // past the end
    CHECK(!GunZone(r, f, { 0.00f, 0.00f, 0.30f }, 0.10f, 0.60f, 0.08f).in);     // behind the right hand
    const Zone z = GunZone(r, f, { 0.03f, 0.04f, -0.25f }, 0.10f, 0.60f, 0.08f);
    CHECK(Near(z.along, 0.25f) && Near(z.off, 0.05f));
}

// ---- gun latch / targets ----
static Pose P(Q4 q, V3 p) { return { q, p }; }
static void no_jump_at_grab()
{
    const Q4 rq = AxisAngle({ 0.3f, 1, 0.2f }, 40.0f), lq = AxisAngle({ 1, 0, 0.5f }, -25.0f);
    const Pose r = P(rq, { 0.1f, 1.2f, -0.2f }), l = P(lq, { 0.15f, 1.15f, -0.55f });
    const float shares[] = { 0.0f, 0.5f, 1.0f };
    for (float share : shares) {
        GunLatch g{};
        CHECK(LatchGun(r, l, &g));
        const GunOut o = GunTargets(r, l, &g, share);
        CHECK(!o.failed && SameRot(o.r, rq) && SameRot(o.l, lq));
    }
}
static void left_hand_up_steers_the_gun()
{
    GunLatch g{};
    CHECK(LatchGun(P(kId, { 0, 0, 0 }), P(kId, { 0, 0, -0.4f }), &g));
    const GunOut o = GunTargets(P(kId, { 0, 0, 0 }), P(kId, { 0, 0.2f, -0.4f }), &g, 1.0f);
    const V3 fwd = QRotate(o.r, { 0, 0, -1 });
    const float n = std::sqrt(0.2f * 0.2f + 0.4f * 0.4f);
    CHECK(NearV(fwd, { 0, 0.2f / n, -0.4f / n }));
}
static void left_keeps_its_grab_rotation_on_the_gun()
{
    const Q4 lq0 = AxisAngle({ 0, 0, 1 }, 70.0f);
    GunLatch g{};
    CHECK(LatchGun(P(kId, { 0, 0, 0 }), P(lq0, { 0, 0, -0.4f }), &g));
    const GunOut o = GunTargets(P(kId, { 0, 0, 0 }), P(kId, { 0.1f, 0.1f, -0.4f }), &g, 0.5f);   // left controller turned away
    CHECK(SameRot(o.l, QMul(o.r, lq0)));                                                          // drawn left = gun * grab offset
}
static void right_roll_rolls_the_gun()
{
    GunLatch g{};
    CHECK(LatchGun(P(kId, { 0, 0, 0 }), P(kId, { 0, 0, -0.4f }), &g));
    const Q4 roll = AxisAngle({ 0, 0, -1 }, 30.0f);                                         // roll about the hand line
    const GunOut o = GunTargets(P(roll, { 0, 0, 0 }), P(kId, { 0, 0, -0.4f }), &g, 1.0f);
    CHECK(SameRot(o.r, roll));
}
// 2026-11-08: below kMinSpan the wrist aims alone (no held direction, no snap when the hands part).
static void hands_together_wrist_aims_alone()
{
    GunLatch g{};
    const Q4 rq = AxisAngle({ 0, 1, 0 }, 15.0f);
    CHECK(LatchGun(P(kId, { 0, 0, 0 }), P(kId, { 0, 0.2f, -0.4f }), &g));
    const GunOut b = GunTargets(P(rq, { 0, 0, 0 }), P(kId, { 0.03f, -0.02f, -0.03f }), &g, 1.0f);   // 4 cm apart
    CHECK(b.heldDir && SameRot(b.r, rq) && Near(b.share, 0.0f));
}
static void latch_refuses_hands_together()
{
    GunLatch g{};
    CHECK(!LatchGun(P(kId, { 0, 0, 0 }), P(kId, { 0, 0, -0.05f }), &g));
}
// RUN TH1 (Jay: "gun was flipping in the hands"): the old aim took the roll from the right controller's up laid around the hand
// line, which spun the gun when the line neared that up. Sweep the left hand from straight ahead up over the top and down behind
// the head (0..150 deg in 0.5 deg steps) with the right wrist still, from several wrist attitudes: every step must turn the gun by
// about the step (a pure swing) - no frame may jump - and the gun's twist about its own barrel must not change.
static float Twist(Q4 q, V3 axisLocal)   // twist angle of q about axisLocal (deg, signed), swing-twist split
{
    const float d = q.x * axisLocal.x + q.y * axisLocal.y + q.z * axisLocal.z;
    Q4 t{ axisLocal.x * d, axisLocal.y * d, axisLocal.z * d, q.w };
    t = QNorm(t);
    return 2.0f * std::atan2(t.x * axisLocal.x + t.y * axisLocal.y + t.z * axisLocal.z, t.w) * 57.2957795f;
}
static void no_flip_when_the_line_passes_the_wrist_up()
{
    const Q4 wrists[] = { kId, AxisAngle({ 0, 0, -1 }, 35.0f), AxisAngle({ 1, 0.4f, 0 }, -50.0f), AxisAngle({ 0.2f, 1, 0.1f }, 80.0f) };
    for (const Q4 rq : wrists) {
        const float shares[] = { 1.0f, 0.5f };
        for (float share : shares) {
            GunLatch g{};
            const V3 ahead = QRotate(rq, { 0, 0, -0.4f }), up = QRotate(rq, { 0, 0.4f, 0 });
            CHECK(LatchGun(P(rq, { 0, 0, 0 }), P(kId, ahead), &g));
            Q4 prev = rq;
            float worst = 0.0f;
            for (int i = 1; i <= 300; ++i) {
                const float a = 0.5f * static_cast<float>(i) * 0.0174532925f;
                const V3 lp = Add(Scale(ahead, std::cos(a)), Scale(up, std::sin(a)));
                const GunOut o = GunTargets(P(rq, { 0, 0, 0 }), P(kId, lp), &g, share);
                const float step = QAngleDeg(prev, o.r);
                if (step > worst) { worst = step; }
                // the line's pull carries no twist: the gun's barrel-local rotation from the wrist is a pure swing
                const Q4 rel = QMul(QConj(rq), o.r);
                CHECK(std::fabs(Twist(rel, { 0, 0, -1 })) < 0.5f);
                prev = o.r;
            }
            CHECK(worst <= 0.5f * share + 0.05f);
        }
    }
}
static void share_turns_the_gun_part_way()
{
    // the left hand swung 40 deg to the right about the right grip, wrist still
    const float a = 40.0f * 0.0174532925f;
    const V3 lp{ 0.4f * std::sin(a), 0.0f, -0.4f * std::cos(a) };
    const float want[] = { 0.0f, 10.0f, 20.0f, 40.0f };
    const float share[] = { 0.0f, 0.25f, 0.5f, 1.0f };
    for (int i = 0; i < 4; ++i) {
        GunLatch g{};
        CHECK(LatchGun(P(kId, { 0, 0, 0 }), P(kId, { 0, 0, -0.4f }), &g));
        const GunOut o = GunTargets(P(kId, { 0, 0, 0 }), P(kId, lp), &g, share[i]);
        CHECK(Near(QAngleDeg(o.r, kId), want[i], 0.05f) && Near(o.swingDeg, want[i], 0.05f));
        const V3 fwd = QRotate(o.r, { 0, 0, -1 });
        CHECK(fwd.x >= -1e-4f && Near(fwd.y, 0.0f));                                     // turned toward the left hand, level
    }
    // the wrist still aims: wrist turned 20 deg left, line unchanged in the world -> share 0.5 leaves half the wrist's turn
    GunLatch g{};
    CHECK(LatchGun(P(kId, { 0, 0, 0 }), P(kId, { 0, 0, -0.4f }), &g));
    const GunOut o = GunTargets(P(AxisAngle({ 0, 1, 0 }, 20.0f), { 0, 0, 0 }), P(kId, { 0, 0, -0.4f }), &g, 0.5f);
    CHECK(Near(QAngleDeg(o.r, kId), 10.0f, 0.05f));
}
static void hands_coming_together_fade_without_a_snap()
{
    const float a = 30.0f * 0.0174532925f;
    const V3 dir{ std::sin(a), 0.0f, -std::cos(a) };
    GunLatch g{};
    CHECK(LatchGun(P(kId, { 0, 0, 0 }), P(kId, { 0, 0, -0.4f }), &g));
    Q4 prev{};
    float worst = 0.0f;
    for (int i = 0; i <= 60; ++i) {                                  // 0.35 m down to 0.05 m in 5 mm steps
        const float span = 0.35f - 0.005f * static_cast<float>(i);
        const GunOut o = GunTargets(P(kId, { 0, 0, 0 }), P(kId, Scale(dir, span)), &g, 1.0f);
        if (i > 0) { const float s = QAngleDeg(prev, o.r); if (s > worst) { worst = s; } }
        prev = o.r;
        if (span >= kFullSpan) { CHECK(Near(QAngleDeg(o.r, kId), 30.0f, 0.05f)); }
        if (span < kMinSpan) { CHECK(SameRot(o.r, kId)); }
    }
    CHECK(worst < 3.5f);   // smoothstep over 8 cm: at most 1.5 x 30 deg x 5 mm / 80 mm = 2.8 deg a step
}
static void line_pointing_back_holds_the_last_pull()
{
    GunLatch g{};
    CHECK(LatchGun(P(kId, { 0, 0, 0 }), P(kId, { 0, 0, -0.4f }), &g));
    const GunOut a = GunTargets(P(kId, { 0, 0, 0 }), P(kId, { 0.39f, 0, -0.09f }), &g, 1.0f);   // ~77 deg right
    const GunOut b = GunTargets(P(kId, { 0, 0, 0 }), P(kId, { 0.01f, 0, 0.4f }), &g, 1.0f);     // straight behind
    CHECK(!a.heldUp && b.heldUp && SameRot(a.r, b.r) && std::isfinite(b.r.w));
}

// ---- press machine ----
static Input In(float squeeze, Kind k, bool inZone, double now, bool holsterBusy = false, bool active = true)
{
    Input i{};
    i.squeezeActive = active;
    i.squeeze = squeeze;
    i.holsterBusy = holsterBusy;
    i.kind = k;
    i.posesOk = true;
    i.inZone = inZone;
    i.now = now;
    i.slideS = 0.15f;
    i.meleeOn = false;
    i.item = 1;
    return i;
}
static void grab_hold_release_blend()
{
    Machine m;
    Step s = m.Update(In(0.0f, Kind::Firearm, true, 0.00));                 // prime
    s = m.Update(In(0.9f, Kind::Firearm, true, 1.00));                       // press in zone
    CHECK(s.grab && s.suppressL1 && m.state() == State::Gripped && Near(s.t, 0.0f));
    s = m.Update(In(0.9f, Kind::Firearm, false, 1.075));                     // held, hand left the zone: still gripped
    CHECK(!s.grab && s.suppressL1 && m.state() == State::Gripped && Near(s.t, 0.5f));
    s = m.Update(In(0.9f, Kind::Firearm, false, 1.50));
    CHECK(Near(s.t, 1.0f));
    s = m.Update(In(0.1f, Kind::Firearm, false, 2.00));                      // release
    CHECK(s.release && !s.suppressL1 && m.state() == State::Releasing && Near(s.t, 1.0f));
    s = m.Update(In(0.1f, Kind::Firearm, false, 2.075));
    CHECK(Near(s.t, 0.5f) && m.state() == State::Releasing);
    s = m.Update(In(0.1f, Kind::Firearm, false, 2.20));
    CHECK(Near(s.t, 0.0f) && m.state() == State::Free);
}
static void press_outside_zone_is_refused_and_l1_passes()
{
    Machine m;
    m.Update(In(0.0f, Kind::Firearm, false, 0.0));
    const Step s = m.Update(In(0.9f, Kind::Firearm, false, 1.0));
    CHECK(s.refusedPress && !s.grab && !s.suppressL1 && s.why[0] != 0);
}
static void press_outside_then_enter_is_not_grab()
{
    Machine m;
    m.Update(In(0.0f, Kind::Firearm, false, 0.0));
    m.Update(In(0.9f, Kind::Firearm, false, 1.0));
    const Step s = m.Update(In(0.9f, Kind::Firearm, true, 1.1));             // held grip drifts into the zone
    CHECK(!s.grab && !s.suppressL1 && m.state() == State::Free);
}
static void first_poll_held_is_not_grab()
{
    Machine m;
    Step s = m.Update(In(0.9f, Kind::Firearm, true, 0.0));
    CHECK(!s.grab && !s.suppressL1 && m.state() == State::Free);
    s = m.Update(In(0.9f, Kind::Firearm, true, 0.1));
    CHECK(!s.grab && m.state() == State::Free);
}
static void holster_busy_refuses_grab()
{
    Machine m;
    m.Update(In(0.0f, Kind::Firearm, true, 0.0));
    const Step s = m.Update(In(0.9f, Kind::Firearm, true, 1.0, /*holsterBusy=*/true));
    CHECK(s.refusedPress && !s.grab && !s.suppressL1);
}
static void melee_and_none_refused()
{
    Machine m;
    m.Update(In(0.0f, Kind::Melee, true, 0.0));
    Step s = m.Update(In(0.9f, Kind::Melee, true, 1.0));
    CHECK(s.refusedPress && !s.grab);
    Machine n;
    n.Update(In(0.0f, Kind::None, true, 0.0));
    s = n.Update(In(0.9f, Kind::None, true, 1.0));
    CHECK(s.refusedPress && !s.grab);
}
static void kind_change_while_gripped_cancels()
{
    Machine m;
    m.Update(In(0.0f, Kind::Firearm, true, 0.0));
    m.Update(In(0.9f, Kind::Firearm, true, 1.0));
    const Step s = m.Update(In(0.9f, Kind::None, true, 1.5));                // gun holstered while gripping
    CHECK(s.cancelled && !s.suppressL1 && m.state() == State::Free && Near(s.t, 0.0f));
}
static void squeeze_inactive_releases()
{
    Machine m;
    m.Update(In(0.0f, Kind::Firearm, true, 0.0));
    m.Update(In(0.9f, Kind::Firearm, true, 1.0));
    const Step s = m.Update(In(0.9f, Kind::Firearm, true, 1.5, false, /*active=*/false));
    CHECK(s.cancelled && !s.suppressL1 && m.state() == State::Releasing);
}
static void regrab_during_release_continues_from_current_t()
{
    Machine m;
    m.Update(In(0.0f, Kind::Firearm, true, 0.0));
    m.Update(In(0.9f, Kind::Firearm, true, 1.0));
    m.Update(In(0.1f, Kind::Firearm, true, 2.0));                            // release at t = 1
    const Step s = m.Update(In(0.9f, Kind::Firearm, true, 2.075));           // re-grab half way out
    CHECK(s.grab && Near(s.t, 0.5f) && m.state() == State::Gripped);
    // Final review: a re-grab mid-release must KEEP the old latch (resumed) - a new latch targets the controller's own
    // orientation and the drawn hand would jump by t x the old two-hand offset.
    CHECK(s.resumed);
}
static void fresh_grab_is_not_resumed()
{
    Machine m;
    m.Update(In(0.0f, Kind::Firearm, true, 0.0));
    const Step s = m.Update(In(0.9f, Kind::Firearm, true, 1.0));
    CHECK(s.grab && !s.resumed);                                           // a grab from Free latches anew
}
static void zero_slide_is_instant()
{
    Blend b{ 1.0, 0.0f, +1 };
    CHECK(Near(BlendAt(b, 1.0, 0.0f), 1.0f));
    b = Blend{ 1.0, 1.0f, -1 };
    CHECK(Near(BlendAt(b, 1.0, 0.0f), 0.0f));
}

// ---- phase 1b: the pin ----
static Xf MakeXf(Q4 q, V3 t)
{
    Xf x{};
    QToMat3(q, x.R);
    x.t[0] = t.x; x.t[1] = t.y; x.t[2] = t.z;
    return x;
}
static bool NearM3(const float* a, const float* b, float eps = 1e-3f)
{
    for (int i = 0; i < 9; ++i) { if (std::fabs(a[i] - b[i]) > eps) { return false; } }
    return true;
}
static void quat_mat_round_trip()
{
    const Q4 q = AxisAngle({ 0.2f, 1, -0.4f }, 63.0f);
    float m[9];
    QToMat3(q, m);
    const Q4 back = QFromBasis({ m[0], m[3], m[6] }, { m[1], m[4], m[7] }, { m[2], m[5], m[8] });
    CHECK(SameRot(q, back));
}
static void ortho_strips_scale()
{
    float m[9];
    QToMat3(AxisAngle({ 1, 2, 3 }, 40.0f), m);
    float s[9];
    for (int r = 0; r < 3; ++r) { for (int c = 0; c < 3; ++c) { s[3 * r + c] = m[3 * r + c] * 1.37f; } }
    float o[9];
    CHECK(Ortho3(s, o) && NearM3(o, m));
}
static void rigid_inverse_round_trip()
{
    const Xf a = MakeXf(AxisAngle({ 0, 1, 1 }, 30.0f), { 0.3f, -1.0f, 2.0f });
    const Xf i = XfMul(XfInvRigid(a), a);
    const float I[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    CHECK(NearM3(i.R, I) && Near(i.t[0], 0) && Near(i.t[1], 0) && Near(i.t[2], 0));
}
static void latch_then_pin_lands_handholder_on_holderl()
{
    // right frame at the latch, the gun's holderl then; later the right frame moves - holderl must move rigidly with it.
    const Xf right0 = MakeXf(AxisAngle({ 0, 1, 0 }, 20.0f), { 1.0f, 1.2f, 0.5f });
    const Xf hl0 = XfMul(right0, MakeXf(AxisAngle({ 1, 0, 0 }, 15.0f), { 0.02f, 0.08f, 0.32f }));
    Xf rel{};
    CHECK(LatchRel(right0, hl0, &rel));
    const Xf right1 = MakeXf(AxisAngle({ 0.3f, 1, 0 }, -35.0f), { 0.7f, 1.4f, 0.9f });
    const Xf hl1 = XfMul(right1, rel);                       // where holderl IS on frame 1
    // authored l_handholder local, ROTATED 30 deg about Z so the test can tell hhR^T from hhR (the identity rotation could not)
    float hh12[12] = { 0.866025f, -0.5f, 0, 0.1f,  0.5f, 0.866025f, 0, 0.0f,  0, 0, 1, -0.025f };
    float grip[9];
    QToMat3(AxisAngle({ 0, 0, 1 }, 90.0f), grip);
    // Final review 2026-10-09: SolveArmIk DRAWS hand = (cWorld * grip) * post, post = the authored rest (rig_grip_compose_rest,
    // ~77 deg) x the CorrectHandLocal dials - post-multiplied (boneblend.cpp:13339-13346). Model it with a non-identity post.
    float post[9];
    QToMat3(QMul(AxisAngle({ 1, 0, 0 }, 77.0f), AxisAngle({ 0, 1, 0 }, 8.0f)), post);
    // INV-501 (run 2): holderl's ROTATION is not a hand orientation (the hand came out reversed). The pin is POSITION ONLY:
    // the hand keeps the rotation the solve already has (phase 1's grab-latched one); only the wrist target moves.
    float cIn[9];
    QToMat3(AxisAngle({ 0.4f, -1, 0.2f }, 52.0f), cIn);
    PinOut p{};
    const char* why = "";
    const float noPalm[3] = { 0, 0, 0 };
    CHECK(PinLeftHand(right1, rel, hh12, noPalm, grip, post, cIn, &p, &why));
    CHECK(NearM3(p.cWorld, cIn));                              // the hand's rotation passes through UNTOUCHED
    // the hand DRAWN from that rotation: hand = cWorld * grip * post
    float cg[9], hand[9];
    M3Mul(cIn, grip, cg);
    M3Mul(cg, post, hand);
    CHECK(Near(p.hhPos[0], hl1.t[0]) && Near(p.hhPos[1], hl1.t[1]) && Near(p.hhPos[2], hl1.t[2]));
    // and the WRIST target puts the drawn handholder there: wrist + hand * t_hh == holderl (the achieved-grip formula, :13421)
    const float tx = hh12[3], ty = hh12[7], tz = hh12[11];
    for (int r = 0; r < 3; ++r) {
        const float hhPosFromWrist = p.wristPos[r] + hand[3 * r] * tx + hand[3 * r + 1] * ty + hand[3 * r + 2] * tz;
        CHECK(Near(hhPosFromWrist, hl1.t[r]));
    }
}
static void pin_refuses_non_orthonormal_grip()
{
    const Xf right = MakeXf(kId, { 0, 1, 0 });
    Xf rel{};
    CHECK(LatchRel(right, MakeXf(kId, { 0, 0, 0.3f }), &rel));
    float hh12[12] = { 1, 0, 0, 0.1f, 0, 1, 0, 0, 0, 0, 1, 0 };
    const float sheared[9] = { 1, 0.4f, 0, 0, 1, 0, 0, 0, 1 };
    PinOut p{};
    const char* why = "";
    const float Ipost[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    const float noPalm[3] = { 0, 0, 0 };
    CHECK(!PinLeftHand(right, rel, hh12, noPalm, sheared, Ipost, Ipost, &p, &why) && why[0] != 0);
}
static void pin_refuses_degenerate_inputs()
{
    Xf zero{};
    Xf rel{};
    CHECK(!LatchRel(zero, MakeXf(kId, { 0, 0, 0 }), &rel));
    const Xf right = MakeXf(kId, { 0, 1, 0 });
    CHECK(LatchRel(right, MakeXf(kId, { 0, 0, 0.3f }), &rel));
    float hh12[12] = { 1, 0, 0, 0.1f, 0, 1, 0, 0, 0, 0, 1, 0 };
    const float I[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    PinOut p{};
    const char* why = "";
    const float noPalm[3] = { 0, 0, 0 };
    CHECK(!PinLeftHand(zero, rel, hh12, noPalm, I, I, I, &p, &why) && why[0] != 0);
}
static void sphere_zone_hysteresis()
{
    SphereZoneState s{};
    const float c[3] = { 0, 0, 0 };
    float d = 0;
    const float a[3] = { 0.11f, 0, 0 }, b[3] = { 0.09f, 0, 0 }, e[3] = { 0.115f, 0, 0 }, f[3] = { 0.125f, 0, 0 };
    CHECK(!SphereZone(&s, a, c, 0.10f, &d));       // 0.11 >= 0.10: not entered
    CHECK(SphereZone(&s, b, c, 0.10f, &d));        // enter
    CHECK(SphereZone(&s, e, c, 0.10f, &d));        // 0.115 <= 0.12: stays in
    CHECK(!SphereZone(&s, f, c, 0.10f, &d) && Near(d, 0.125f));
}
// INV-502 (a): the pin holds the PALM centre, not the knuckle-line handholder. palm = (palm - handholder) in the hand bone's
// frame; the wrist target must put wrist + hand * (t_hh + palm) on holderl, and the handholder ends up hand * (-palm) off it.
static void palm_offset_lands_palm_on_holderl()
{
    const Xf right0 = MakeXf(AxisAngle({ 0, 1, 0 }, 20.0f), { 1.0f, 1.2f, 0.5f });
    const Xf hl0 = XfMul(right0, MakeXf(AxisAngle({ 1, 0, 0 }, 15.0f), { 0.02f, 0.08f, 0.32f }));
    Xf rel{};
    CHECK(LatchRel(right0, hl0, &rel));
    const Xf right1 = MakeXf(AxisAngle({ 0.3f, 1, 0 }, -35.0f), { 0.7f, 1.4f, 0.9f });
    const Xf hl1 = XfMul(right1, rel);
    float hh12[12] = { 1, 0, 0, 0.0962f,  0, 1, 0, 0.0f,  0, 0, 1, -0.0238f };
    float grip[9], post[9], cIn[9];
    QToMat3(AxisAngle({ 0, 0, 1 }, 90.0f), grip);
    QToMat3(QMul(AxisAngle({ 1, 0, 0 }, 77.0f), AxisAngle({ 0, 1, 0 }, 8.0f)), post);
    QToMat3(AxisAngle({ 0.4f, -1, 0.2f }, 52.0f), cIn);
    const float palm[3] = { -0.0460f, 0.0040f, -0.0027f };
    PinOut p{};
    const char* why = "";
    CHECK(PinLeftHand(right1, rel, hh12, palm, grip, post, cIn, &p, &why));
    CHECK(NearM3(p.cWorld, cIn));
    float cg[9], hand[9];
    M3Mul(cIn, grip, cg);
    M3Mul(cg, post, hand);
    for (int r = 0; r < 3; ++r) {
        const float px = hh12[3] + palm[0], py = hh12[7] + palm[1], pz = hh12[11] + palm[2];
        const float palmW = p.wristPos[r] + hand[3 * r] * px + hand[3 * r + 1] * py + hand[3 * r + 2] * pz;
        CHECK(Near(palmW, hl1.t[r]));
        const float hhW = p.wristPos[r] + hand[3 * r] * hh12[3] + hand[3 * r + 1] * hh12[7] + hand[3 * r + 2] * hh12[11];
        const float want = hl1.t[r] - (hand[3 * r] * palm[0] + hand[3 * r + 1] * palm[1] + hand[3 * r + 2] * palm[2]);
        CHECK(Near(hhW, want));
    }
    // and the offset really moved it: the handholder is |palm| (~4.6 cm) off holderl, not on it
    float dx = 0, d2 = 0;
    for (int r = 0; r < 3; ++r) {
        dx = p.wristPos[r] + hand[3 * r] * hh12[3] + hand[3 * r + 1] * hh12[7] + hand[3 * r + 2] * hh12[11] - hl1.t[r];
        d2 += dx * dx;
    }
    CHECK(Near(std::sqrt(d2), 0.04625f, 2e-4f));
}
// INV-502 (c): the forend offset is in holderl's own frame; the self-check predicts the barrel element from holderl's world.
static void forend_offset_and_self_check()
{
    const Xf hl = MakeXf(AxisAngle({ 0.2f, 1, -0.3f }, 40.0f), { 0.5f, 1.1f, -0.2f });
    const float off[3] = { -0.0202f, 0.0396f, 0.0777f };
    float t[3];
    ForendTarget(hl, off, t);
    float want[3];
    for (int r = 0; r < 3; ++r) { want[r] = hl.t[r] + hl.R[3 * r] * off[0] + hl.R[3 * r + 1] * off[1] + hl.R[3 * r + 2] * off[2]; }
    CHECK(Near(t[0], want[0]) && Near(t[1], want[1]) && Near(t[2], want[2]));
    // |offset| survives the rotation: 0.0895 m
    const float dx = t[0] - hl.t[0], dy = t[1] - hl.t[1], dz = t[2] - hl.t[2];
    CHECK(Near(std::sqrt(dx * dx + dy * dy + dz * dz), 0.0895f, 5e-4f));
    // self-check: the barrel where the table says -> error ~0; the barrel predicted with the TRANSPOSED rotation -> large error
    const float barrelHl[3] = { -0.1235f, 0.2870f, 0.3951f };
    float bw[3];
    ForendTarget(hl, barrelHl, bw);
    CHECK(ForendCheckErr(hl, barrelHl, bw) < 1e-4f);
    Xf hlT = hl;
    M3T(hl.R, hlT.R);
    float bwT[3];
    ForendTarget(hlT, barrelHl, bwT);
    CHECK(ForendCheckErr(hl, barrelHl, bwT) > 0.05f);
}
// The HANDPOSE probe (Jay 2026-10-09: the last run-6 grip is the standard): the drawn left hand's rotation and palm point,
// expressed in the gun's holderl frame, must come back exactly as put in.
static void hand_in_frame_round_trip()
{
    const Xf f = MakeXf(AxisAngle({ 0.3f, -1, 0.5f }, 63.0f), { 0.4f, 1.3f, -0.6f });
    float rl[9];
    QToMat3(AxisAngle({ 1, 0.2f, -0.4f }, 110.0f), rl);      // the hand in the gun frame (what the probe must report)
    const float pl[3] = { 0.012f, -0.034f, 0.051f };
    float hw[9], pw[3];
    M3Mul(f.R, rl, hw);
    ForendTarget(f, pl, pw);
    float q[4], p[3];
    HandInFrame(f, hw, pw, q, p);
    float back[9];
    QToMat3(Q4{ q[0], q[1], q[2], q[3] }, back);
    CHECK(NearM3(back, rl));
    CHECK(Near(p[0], pl[0]) && Near(p[1], pl[1]) && Near(p[2], pl[2]));
    CHECK(Near(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3], 1.0f));
}
// INV-506 standard pose: the cWorld that makes the DRAWN hand (cWorld * grip * post) equal frame * standard.
static void standard_cworld_draws_the_standard()
{
    float frame[9], grip[9], post[9], want[9];
    QToMat3(AxisAngle({ 0.2f, 1, -0.4f }, 37.0f), frame);
    QToMat3(AxisAngle({ 0, 0, 1 }, 90.0f), grip);
    QToMat3(QMul(AxisAngle({ 1, 0, 0 }, 77.0f), AxisAngle({ 0, 1, 0 }, 8.0f)), post);
    const Q4 std4{ 0.2015f, -0.5707f, 0.7529f, 0.2587f };
    float out[9];
    CHECK(StandardCWorld(frame, std4, grip, post, out));
    float og[9], drawn[9], sm[9];
    M3Mul(out, grip, og);
    M3Mul(og, post, drawn);
    QToMat3(QNorm(std4), sm);
    M3Mul(frame, sm, want);
    CHECK(NearM3(drawn, want));
}
static void slerp_m3_ends_and_middle()
{
    float a[9], b[9], o[9];
    QToMat3(AxisAngle({ 0, 1, 0 }, 10.0f), a);
    QToMat3(AxisAngle({ 0, 1, 0 }, 70.0f), b);
    SlerpM3(a, b, 0.0f, o);
    CHECK(NearM3(o, a));
    SlerpM3(a, b, 1.0f, o);
    CHECK(NearM3(o, b));
    SlerpM3(a, b, 0.5f, o);
    float m[9];
    QToMat3(AxisAngle({ 0, 1, 0 }, 40.0f), m);
    CHECK(NearM3(o, m));
}
// Jay 2026-10-09: the rifle grab area is a strip along the handguard (holderl frame z), half-length h, X radius rx, Y/end radius
// ry; `along` = where on the strip the hand is (clamped), `norm` < 1 inside.
static void slide_zone_strip()
{
    float along = 9, norm = 9;
    const float on[3] = { 0, 0, 0.05f };
    SlideZone(on, 0.065f, 0.12f, 0.10f, &along, &norm);
    CHECK(Near(along, 0.05f) && Near(norm, 0.0f));
    const float past[3] = { 0, 0, 0.165f };                   // 10 cm past the front end: on the end cap's edge
    SlideZone(past, 0.065f, 0.12f, 0.10f, &along, &norm);
    CHECK(Near(along, 0.065f) && Near(norm, 1.0f));
    const float back[3] = { 0, 0, -0.30f };
    SlideZone(back, 0.065f, 0.12f, 0.10f, &along, &norm);
    CHECK(Near(along, -0.065f) && norm > 1.0f);
    const float side[3] = { 0.12f, 0, 0.0f };                 // X uses the wider radius
    SlideZone(side, 0.065f, 0.12f, 0.10f, &along, &norm);
    CHECK(Near(norm, 1.0f));
    const float up[3] = { 0, 0.10f, -0.03f };                 // Y uses ry
    SlideZone(up, 0.065f, 0.12f, 0.10f, &along, &norm);
    CHECK(Near(norm, 1.0f) && Near(along, -0.03f));
    const float zero[3] = { 0.05f, 0, 0.04f };                // h = 0: a point zone again (today's behaviour)
    SlideZone(zero, 0.0f, 0.10f, 0.10f, &along, &norm);
    CHECK(Near(along, 0.0f) && Near(norm, std::sqrt(0.05f * 0.05f + 0.04f * 0.04f) / 0.10f));
}
// INV-508: the pin point offset by a vector in holderl's frame (the rifle standard palm offset): XfMul(rel, translate(o)).
static void offset_rel_in_holderl_frame()
{
    const Xf rel = MakeXf(AxisAngle({ -0.2f, 1, 0.4f }, 33.0f), { 0.31f, 0.1f, -0.02f });
    const float o[3] = { 0.020f, -0.012f, 0.0f };
    const Xf want = XfMul(rel, MakeXf(kId, { o[0], o[1], o[2] }));
    const Xf got = OffsetRel(rel, o);
    CHECK(Near(got.t[0], want.t[0]) && Near(got.t[1], want.t[1]) && Near(got.t[2], want.t[2]));
    CHECK(NearM3(got.R, rel.R));
}
// The pin point slid along holderl's z by s: the same as XfMul(rel, translate(0,0,s)).
static void slide_rel_moves_along_holderl_z()
{
    const Xf rel = MakeXf(AxisAngle({ 0.3f, 1, 0.2f }, 48.0f), { 0.3f, 0.09f, 0.01f });
    const Xf want = XfMul(rel, MakeXf(kId, { 0, 0, 0.042f }));
    const Xf got = SlideRel(rel, 0.042f);
    CHECK(Near(got.t[0], want.t[0]) && Near(got.t[1], want.t[1]) && Near(got.t[2], want.t[2]));
    CHECK(NearM3(got.R, rel.R));
}
// INV-507 fix 1: the pinned left arm's reach distance - the reach-mapped one blended to the raw one by the pin's t (the target
// is already in the rig's reach space).
static void pinned_reach_blend()
{
    CHECK(Near(PinnedReach(0.45f, 0.50f, 0.0f), 0.45f));
    CHECK(Near(PinnedReach(0.45f, 0.50f, 1.0f), 0.50f));
    CHECK(Near(PinnedReach(0.45f, 0.50f, 0.5f), 0.475f));
    CHECK(Near(PinnedReach(0.45f, 0.50f, 1.7f), 0.50f));   // t clamped
}
// INV-507 fix 2: the grab-zone point is the DRAWN palm centre (achieved handholder + drawn basis x palm offset).
static void zone_point_is_the_drawn_palm()
{
    float drawn[9];
    QToMat3(AxisAngle({ 0.1f, 1, 0.3f }, 70.0f), drawn);
    const float achieved[3] = { 0.4f, 1.2f, -0.3f }, palm[3] = { -0.046f, 0.004f, -0.0027f }, grip[3] = { 9, 9, 9 };
    float out[3];
    ZonePoint(true, achieved, drawn, palm, grip, out);
    for (int r = 0; r < 3; ++r) {
        CHECK(Near(out[r], achieved[r] + drawn[3 * r] * palm[0] + drawn[3 * r + 1] * palm[1] + drawn[3 * r + 2] * palm[2]));
    }
    ZonePoint(false, achieved, drawn, palm, grip, out);
    CHECK(Near(out[0], 9) && Near(out[1], 9) && Near(out[2], 9));
}
// INV-503 (b) fix: the wrist overflow while two-handing - never below the normal one, capped at 90 (flex 85+90 = 175 < 180,
// clear of D-RIG-4's degenerate axis); not gripping = the normal overflow, capped at 60 as before.
static void two_hand_overflow_pick()
{
    CHECK(Near(WristOverflowDeg(60.0f, 80.0f, false), 60.0f));
    CHECK(Near(WristOverflowDeg(60.0f, 80.0f, true), 80.0f));
    CHECK(Near(WristOverflowDeg(60.0f, 30.0f, true), 60.0f));    // never LESS than the normal overflow
    CHECK(Near(WristOverflowDeg(60.0f, 150.0f, true), 90.0f));   // capped
    CHECK(Near(WristOverflowDeg(75.0f, 0.0f, false), 60.0f));    // the old cap stays for the normal path
    CHECK(Near(WristOverflowDeg(-5.0f, -5.0f, true), 0.0f));
}
// INV-502 (b) measurement: how far past the (overflowed) wrist limit the solve asked, binned.
static void wrist_excess_bins()
{
    CHECK(ExcessBin(0.0f) == -1);       // at or inside the limit: not clamped, not binned
    CHECK(ExcessBin(-3.0f) == -1);
    CHECK(ExcessBin(0.5f) == 0);
    CHECK(ExcessBin(9.99f) == 0);
    CHECK(ExcessBin(10.0f) == 1);
    CHECK(ExcessBin(29.0f) == 2);
    CHECK(ExcessBin(30.0f) == 3);
    CHECK(ExcessBin(44.9f) == 3);
    CHECK(ExcessBin(59.0f) == 4);
    CHECK(ExcessBin(75.0f) == 5);
    CHECK(ExcessBin(90.0f) == 6);
    CHECK(ExcessBin(170.0f) == kExcessBins - 1);
}

// ---- phase 2: two-handed melee (spec section 7) ----
static void classify_melee_by_desc_flag()
{
    CHECK(ClassifyWithDesc("WeaponVis", "wn_axe_a", true, true, kT, 3, kA, 1).kind == Kind::Melee);
    CHECK(ClassifyWithDesc("WeaponVis", "wn_scythe_a", true, false, kT, 3, kA, 1).kind == Kind::None);      // desc wins over the table
    CHECK(ClassifyWithDesc("WeaponVis", "wn_baseballbat_a", true, true, kT, 3, kA, 1).kind == Kind::Melee); // table-ambiguous, desc decides
    CHECK(ClassifyWithDesc("WeaponVis", "wn_axe_a", false, true, kT, 3, kA, 1).kind == Kind::None);         // unreadable flag: refused
    CHECK(ClassifyWithDesc("FireWeaponVis", "wn_rifle_a", true, false, kT, 3, kA, 1).kind == Kind::Firearm); // guns keep the table
    CHECK(ClassifyWithDesc("BowVis", "wn_bow_composite_a", true, true, kT, 3, kA, 1).kind == Kind::None);
    CHECK(ClassifyWithDesc("CrossbowVis", "wn_rifle_a", true, true, kT, 3, kA, 1).kind == Kind::None);
    // Jay 2026-11-09: "Also need to add the second hands grip for the chainsaw" - its own case is now a FIREARM grip (at its hoop,
    // holderl; the row is twohand_shotgun.inc's), whatever its desc flag says
    CHECK(ClassifyWithDesc("WeaponVis", "wn_chainsaw_a", true, true, kT, 3, kA, 1).kind == Kind::Firearm);
    CHECK(ClassifyWithDesc("WeaponVis", "wn_chainsaw_a", true, false, kT, 3, kA, 1).kind == Kind::Firearm);
    CHECK(ClassifyWithDesc("WeaponVis", "wn_chainsaw_a", false, false, kT, 3, kA, 1).kind == Kind::Firearm);
}
// INV-644: the grip table and its two helpers.
static void grip_table_rows_are_sane()
{
    const volatile unsigned rowsN = kMeleeGripsN;   // volatile: a compile-time constant here trips C4127
    CHECK(rowsN >= 52);
    unsigned bad = 0;
    for (unsigned i = 0; i < kMeleeGripsN; ++i) {
        const MeleeGrip& g = kMeleeGrips[i];
        const float len = std::sqrt(g.u[0] * g.u[0] + g.u[1] * g.u[1] + g.u[2] * g.u[2]);
        // the left palm below the right hand, and both above the handle end (the polearms' left is 41 cm down - still above)
        // radius: all 12 measured (> 0) or all -1 (not measured) - never a mix, never a zero
        bool radOk = true;
        for (int k = 0; k < 12; ++k) {
            radOk = radOk && (g.radius[0] > 0.0f ? g.radius[k] > 0.0f : g.radius[k] == -1.0f);
        }
        if (!(g.left < g.right) || !(g.left > g.butt) || std::fabs(len - 1.0f) > 1e-3f || !radOk) {
            std::printf("  bad row %s: right %+.3f left %+.3f butt %+.3f |u| %.4f\n", g.mesh, g.right, g.left, g.butt, len);
            ++bad;
        }
    }
    CHECK(bad == 0);
    bool axe = false, guitar = false, stage = false;
    for (unsigned i = 0; i < kMeleeGripsN; ++i) {
        axe = axe || std::strcmp(kMeleeGrips[i].mesh, "wn_axe_d") == 0;
        guitar = guitar || std::strcmp(kMeleeGrips[i].mesh, "wn_electric_guitar_a") == 0;
        stage = stage || std::strcmp(kMeleeGrips[i].mesh, "wn_axe_e_dm_b") == 0;
    }
    CHECK(axe && guitar && stage);
    unsigned guitars = 0, fingersElsewhere = 0;
    for (unsigned i = 0; i < kMeleeGripsN; ++i) {   // run R4: the guitar's neck measured (0.035 m); run R5: it and its stages 5 cm
        const bool g = std::strncmp(kMeleeGrips[i].mesh, "wn_electric_guitar_a", 20) == 0;
        if (g) {
            ++guitars;
            // the base neck 0.035; each stage measured from its own mesh (_dm_b's neck sits off the axis: 0.013 on the palm side)
            CHECK(std::strcmp(kMeleeGrips[i].mesh, "wn_electric_guitar_a") != 0 || kMeleeGrips[i].radius[0] > 0.03f);
            CHECK(kMeleeGrips[i].radius[0] > 0.0f);
            CHECK(kMeleeGrips[i].fingers == 0.0f);   // run R6: the 5 cm reverted - the live tune sliders set it instead
        } else if (kMeleeGrips[i].fingers != 0.0f) {
            ++fingersElsewhere;
        }
    }
    CHECK(guitars == 3 && fingersElsewhere == 0);   // base + _dm_a + _dm_b; no weapon moved by the fingers column
    // run R7 bake: the guitar's three rows carry Jay's hold (x +0.03 m, turn x -3.6 deg); no other row is tuned
    unsigned tunedGuitar = 0, tunedOther = 0;
    for (unsigned i = 0; i < kMeleeGripsN; ++i) {
        const float* tv = kMeleeGrips[i].tune;
        const bool tuned = tv[0] != 0.0f || tv[1] != 0.0f || tv[2] != 0.0f || tv[3] != 0.0f || tv[4] != 0.0f || tv[5] != 0.0f;
        if (std::strncmp(kMeleeGrips[i].mesh, "wn_electric_guitar_a", 20) == 0) {
            tunedGuitar += (std::fabs(tv[0] - 0.03f) < 1e-6f && std::fabs(tv[3] + 3.6f) < 1e-5f && tv[1] == 0.0f && tv[2] == 0.0f
                            && tv[4] == 0.0f && tv[5] == 0.0f) ? 1u : 0u;
        } else if (tuned) {
            ++tunedOther;
        }
    }
    CHECK(tunedGuitar == 3 && tunedOther == 0);
    // run R6 GripTune: all zero = unchanged; a 90 deg turn about z at the handholder; an offset scaled by the blend
    {
        float R[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        float T[3] = { 0.2f, 0.0f, 0.0f };
        const float hhz[3] = { 0.1f, 0.0f, 0.0f }, zero3[3] = { 0, 0, 0 };
        GripTune(hhz, zero3, zero3, 1.0f, R, T);
        CHECK(T[0] == 0.2f && T[1] == 0.0f && R[0] == 1.0f && R[4] == 1.0f);
        const float rz[3] = { 0.0f, 0.0f, 90.0f };
        GripTune(hhz, zero3, rz, 1.0f, R, T);   // (0.2,0,0) about (0.1,0,0) by +90 about z -> (0.1, 0.1, 0); x axis -> +y
        CHECK(std::fabs(T[0] - 0.1f) < 1e-5f && std::fabs(T[1] - 0.1f) < 1e-5f && std::fabs(R[3] - 1.0f) < 1e-5f);
        float T2[3] = { 0, 0, 0 };
        float R2[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        const float off[3] = { 0.04f, 0.0f, 0.0f };
        GripTune(hhz, off, zero3, 0.5f, R2, T2);
        CHECK(std::fabs(T2[0] - 0.02f) < 1e-6f);
    }
    float p[3] = { 0.0f, 0.0f, 0.0f };
    const float hh[3] = { 0.0962f, 0.0f, -0.0238f };
    TowardFingers(hh, 0.05f, p);
    CHECK(std::fabs(std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) - 0.05f) < 1e-5f && p[0] > 0.0f);
    float q[3] = { 1.0f, 2.0f, 3.0f };
    const float none[3] = { 0.0f, 0.0f, 0.0f };
    TowardFingers(none, 0.05f, q);
    CHECK(q[0] == 1.0f && q[1] == 2.0f && q[2] == 3.0f);
}
static void melee_shaft_from_row()
{
    const float golf[3] = { 0.0f, 1.0f, 0.0f };
    const ShaftCheck g = MeleeShaftFromRow(golf, kMeleeShaftAxis);
    CHECK(g.ok && g.slide == 0.0f && std::fabs(g.offDeg - 10.8f) < 0.2f);   // the golf club's untilted holder
    const ShaftCheck a = MeleeShaftFromRow(kMeleeShaftAxis, kMeleeShaftAxis);
    CHECK(a.ok && a.offDeg < 0.1f);
    const float zero[3] = { 0.0f, 0.0f, 0.0f };
    CHECK(!MeleeShaftFromRow(zero, kMeleeShaftAxis).ok);
    // the slide range: a bat (slide 0) held at +0.096 slides with a row, not without; the axes keep their own 0.347
    CHECK(!MeleeSlideInRange(0.096f, MeleeSlideRangeTop(false, 0.0f)));
    CHECK(MeleeSlideInRange(0.096f, MeleeSlideRangeTop(true, 0.0f)));
    CHECK(MeleeSlideRangeTop(true, 0.347f) == 0.347f);
    CHECK(!MeleeSlideInRange(0.25f, MeleeSlideRangeTop(true, 0.0f)));
}
// INV-644: melee = the ItemType (15..17) OR the flag; the flag path and everything that is not WeaponVis unchanged.
static void classify_melee_by_item_type()
{
    CHECK(IsTwoHandedItemType(15) && IsTwoHandedItemType(16) && IsTwoHandedItemType(17));
    CHECK(!IsTwoHandedItemType(13) && !IsTwoHandedItemType(14) && !IsTwoHandedItemType(18) && !IsTwoHandedItemType(-1));
    CHECK(ClassifyWithType("WeaponVis", "wn_rebar_a", true, false, true, 15, kT, 3, kA, 1).kind == Kind::Melee);   // flag false, type
    CHECK(ClassifyWithType("WeaponVis", "wn_short_sword_c", true, false, true, 16, kT, 3, kA, 1).kind == Kind::Melee);
    CHECK(ClassifyWithType("WeaponVis", "wn_axe_a", false, false, true, 15, kT, 3, kA, 1).kind == Kind::Melee);    // flag unreadable
    CHECK(ClassifyWithType("WeaponVis", "wn_axe_b_twohanded_hr", true, true, true, 13, kT, 3, kA, 1).kind == Kind::Melee); // flag only
    CHECK(ClassifyWithType("WeaponVis", "wn_sickle_d", true, false, true, 13, kT, 3, kA, 1).kind == Kind::None);   // one-handed Stick
    CHECK(ClassifyWithType("WeaponVis", "wn_rebar_a", true, false, false, 15, kT, 3, kA, 1).kind == Kind::None);   // type unreadable:
                                                                                                                   // the flag alone
    CHECK(ClassifyWithType("WeaponVis", "wn_chainsaw_a", true, false, true, 15, kT, 3, kA, 1).kind == Kind::Firearm); // 2026-11-09
    CHECK(ClassifyWithType("BowVis", "wn_bow_composite_a", true, true, true, 16, kT, 3, kA, 1).kind == Kind::None);
    CHECK(ClassifyWithType("FireWeaponVis", "wn_rifle_a", true, false, true, 21, kT, 3, kA, 1).kind == Kind::Firearm);
    CHECK(ClassifyWithType("FireWeaponVis", "wn_pistol_a", true, false, true, 16, kT, 3, kA, 1).kind == Kind::None); // not WeaponVis
}
static Input InM(float squeeze, bool inZone, double now, bool meleeOn = true)
{
    Input i = In(squeeze, Kind::Melee, inZone, now);
    i.meleeOn = meleeOn;
    return i;
}
static void melee_grabs_when_on()
{
    Machine m;
    m.Update(InM(0.0f, true, 0.0));
    const Step s = m.Update(InM(0.9f, true, 1.0));
    CHECK(s.grab && s.suppressL1 && m.state() == State::Gripped);
    const Step r = m.Update(InM(0.0f, true, 2.0));
    CHECK(r.release && !r.suppressL1);
}
static void melee_refused_when_off_or_outside()
{
    Machine m;
    m.Update(InM(0.0f, true, 0.0, false));
    Step s = m.Update(InM(0.9f, true, 1.0, false));
    CHECK(s.refusedPress && !s.grab && std::strstr(s.why, "rig_two_hand_melee=0") != nullptr);
    Machine n;
    n.Update(InM(0.0f, false, 0.0));
    s = n.Update(InM(0.9f, false, 1.0));
    CHECK(s.refusedPress && !s.grab);
}
static void swap_kind_while_gripped_cancels()
{
    Machine m;
    m.Update(InM(0.0f, true, 0.0));
    m.Update(InM(0.9f, true, 1.0));
    const Step s = m.Update(In(0.9f, Kind::Firearm, true, 1.5));   // the melee weapon swapped for a rifle mid-grip
    CHECK(s.cancelled && !s.suppressL1 && m.state() == State::Free);
}
static void swap_same_kind_while_gripped_cancels()
{
    // review I-1: axe -> sledgehammer (Melee -> Melee) or rifle -> rifle with no empty-slot poll between: a different ITEM
    Machine m;
    m.Update(InM(0.0f, true, 0.0));
    m.Update(InM(0.9f, true, 1.0));
    CHECK(m.heldItem() == 1 && m.heldKind() == Kind::Melee);
    Input sw = InM(0.9f, true, 1.5);
    sw.item = 2;
    const Step s = m.Update(sw);
    CHECK(s.cancelled && !s.suppressL1 && m.state() == State::Free && m.heldKind() == Kind::None);
    Machine r;
    r.Update(In(0.0f, Kind::Firearm, true, 0.0));
    r.Update(In(0.9f, Kind::Firearm, true, 1.0));
    Input sr = In(0.9f, Kind::Firearm, true, 1.5);
    sr.item = 7;
    const Step t = r.Update(sr);
    CHECK(t.cancelled && r.state() == State::Free);
}
static void melee_off_mid_grip_lets_go()
{
    Machine m;
    m.Update(InM(0.0f, true, 0.0));
    m.Update(InM(0.9f, true, 1.0));
    const Step s = m.Update(InM(0.9f, true, 1.5, false));         // rig_two_hand_melee unticked while gripping
    CHECK(s.cancelled && !s.suppressL1 && m.state() == State::Free);
}
static void melee_zone_strip_below_the_right_hand()
{
    const float u[3] = { 0.0f, 1.0f, 0.0f };
    float along = 0.0f, norm = 0.0f;
    const float below[3] = { 0.02f, 0.10f, 0.0f };      // on the shaft, 10 cm above holder; right hand at +0.35
    MeleeZone(below, u, kMeleeButtY, 0.35f, 0.10f, &along, &norm);
    CHECK(Near(along, 0.10f) && norm < 1.0f);
    const float head[3] = { 0.0f, 0.60f, 0.0f };        // up at the head, above the right hand
    MeleeZone(head, u, kMeleeButtY, 0.35f, 0.10f, &along, &norm);
    CHECK(Near(along, 0.35f) && norm > 1.0f);
    const float wide[3] = { 0.15f, 0.0f, 0.0f };        // beside the shaft, too far out
    MeleeZone(wide, u, kMeleeButtY, 0.35f, 0.10f, &along, &norm);
    CHECK(norm > 1.0f);
    const float pastButt[3] = { 0.0f, -0.30f, 0.0f };
    MeleeZone(pastButt, u, kMeleeButtY, 0.35f, 0.10f, &along, &norm);
    CHECK(Near(along, kMeleeButtY) && norm > 1.0f);
    const float low[3] = { 0.0f, -0.10f, 0.0f };        // right hand BELOW the butt: the strip collapses to its floor, never inverts
    MeleeZone(low, u, kMeleeButtY, -0.30f, 0.10f, &along, &norm);
    CHECK(Near(along, kMeleeButtY) && norm < 1.0f);
    const float lowFar[3] = { 0.0f, 0.20f, 0.0f };
    MeleeZone(lowFar, u, kMeleeButtY, -0.30f, 0.10f, &along, &norm);
    CHECK(norm > 1.0f);
    // the TILTED shaft (the real one in the Holder frame): a point 0.2 m along it is on the strip, distance 0
    const float* t = kMeleeShaftAxis;
    const float onTilt[3] = { 0.2f * t[0], 0.2f * t[1], 0.2f * t[2] };
    MeleeZone(onTilt, t, kMeleeButtY, 0.35f, 0.10f, &along, &norm);
    CHECK(Near(along, 0.20f) && norm < 0.01f);
}
static void point_in_frame_round_trip()
{
    Xf f{};
    QToMat3(AxisAngle({ 0.3f, 1.0f, -0.2f }, 50.0f), f.R);
    f.t[0] = 1.0f; f.t[1] = 2.0f; f.t[2] = 3.0f;
    const float local[3] = { 0.1f, -0.2f, 0.3f };
    float w[3], back[3];
    ForendTarget(f, local, w);
    PointInFrame(f, w, back);
    CHECK(Near(back[0], 0.1f) && Near(back[1], -0.2f) && Near(back[2], 0.3f));
}
static void melee_shaft_check()
{
    const Xf h{ { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, { 0, 0, 0 } };
    const float axeL[3] = { 0.0f, 0.3408f, 0.0651f };   // the axe's holderl in its Holder frame (mesh data)
    ShaftCheck s = MeleeShaft(h, axeL, kMeleeShaftAxis);
    CHECK(s.ok && Near(s.slide, 0.347f) && s.offDeg < 1.0f);
    CHECK(Near(s.u[0], kMeleeShaftAxis[0]) && Near(s.u[1], kMeleeShaftAxis[1]) && Near(s.u[2], kMeleeShaftAxis[2]));
    const float bat[3] = { 0.0f, 0.0f, 0.0f };          // the bat: holderl == holder, no direction -> the shared axis
    s = MeleeShaft(h, bat, kMeleeShaftAxis);
    CHECK(s.ok && Near(s.slide, 0.0f) && Near(s.u[1], kMeleeShaftAxis[1]));
    const float side[3] = { 0.10f, 0.20f, 0.0f };       // left node ~27 deg off the shaft: axis unknown, refused
    s = MeleeShaft(h, side, kMeleeShaftAxis);
    CHECK(!s.ok);
    const float down[3] = { 0.0f, -0.30f, -0.06f };     // left node BELOW holder: refused
    s = MeleeShaft(h, down, kMeleeShaftAxis);
    CHECK(!s.ok);
}
static void melee_rel_is_absolute()
{
    // rel = the Holder frame in the hand bone frame (turned 90 deg about X); the shaft u is TILTED in the Holder frame (the real
    // axis). Holder sits 0.30 m DOWN the shaft from the handholder (the idle hold, INV-499); handholder at (0.1, 0, 0).
    const float* u = kMeleeShaftAxis;
    float R[9];
    QToMat3(AxisAngle({ 1, 0, 0 }, 90.0f), R);
    float s[3];   // the shaft in the hand frame = R u
    for (int r = 0; r < 3; ++r) { s[r] = R[3 * r] * u[0] + R[3 * r + 1] * u[1] + R[3 * r + 2] * u[2]; }
    const float hh[3] = { 0.10f, 0.0f, 0.0f };
    const float T[3] = { hh[0] - 0.30f * s[0], hh[1] - 0.30f * s[1], hh[2] - 0.30f * s[2] };
    float a = 0.0f;
    CHECK(MeleeHandAlong(R, T, hh, u, &a) && Near(a, 0.30f));
    float oR[9], oT[3];
    MeleeRel(R, T, R, T, u, a, 0.0f, oR, oT);           // t = 0: the live transform, untouched
    CHECK(Near(oT[0], T[0]) && Near(oT[1], T[1]) && Near(oT[2], T[2]));
    MeleeRel(R, T, R, T, u, a, 1.0f, oR, oT);           // t = 1: Holder at the handholder along the shaft
    float a1 = 1.0f;
    CHECK(MeleeHandAlong(oR, oT, hh, u, &a1) && Near(a1, 0.0f));
    // an attack moves the LIVE rel 0.2 m along the shaft while gripped: the gripped placement does not move
    const float Tatk[3] = { T[0] + 0.20f * s[0], T[1] + 0.20f * s[1], T[2] + 0.20f * s[2] };
    float oR2[9], oT2[3];
    MeleeRel(R, Tatk, R, T, u, a, 1.0f, oR2, oT2);
    CHECK(Near(oT2[0], oT[0]) && Near(oT2[1], oT[1]) && Near(oT2[2], oT[2]));
    MeleeRel(R, T, R, T, u, a, 0.5f, oR, oT);           // halfway: halfway along the shaft
    float ah = 0.0f;
    CHECK(MeleeHandAlong(oR, oT, hh, u, &ah) && Near(ah, 0.15f));
}
static void melee_slide_range()
{
    CHECK(MeleeSlideInRange(0.347f, 0.347f));            // the idle hold on the axe
    CHECK(MeleeSlideInRange(0.0f, 0.0f));                // the bat
    CHECK(!MeleeSlideInRange(-0.20f, 0.347f));           // grabbed mid-swing past holder: no slide this hold
    CHECK(!MeleeSlideInRange(0.50f, 0.347f));            // further up than the left node: no slide
}
static void melee_curl_matches_the_generator()
{
    static_assert(kMeleeFingersN == 16, "the 16 left finger pins");
    for (unsigned i = 0; i < kMeleeFingersN; ++i) {
        float g[9];
        CurlFinger(kMeleeFingers[i].grip0, kMeleeCurlAxis, 30.0f, kMeleeFingers[i].curls, g);
        CHECK(MaxAbsDiff(g, kMeleeFingers[i].grip30, 9) < 2e-3f);
    }
}
static void melee_std_and_palm_sane()
{
    const float n = std::sqrt(kMeleeLeftStdQ[0] * kMeleeLeftStdQ[0] + kMeleeLeftStdQ[1] * kMeleeLeftStdQ[1] +
                              kMeleeLeftStdQ[2] * kMeleeLeftStdQ[2] + kMeleeLeftStdQ[3] * kMeleeLeftStdQ[3]);
    CHECK(Near(n, 1.0f));
    const float* u = kMeleeShaftAxis;
    const float* p = kMeleeLeftPalm0;
    const float al = p[0] * u[0] + p[1] * u[1] + p[2] * u[2];
    const float off2 = p[0] * p[0] + p[1] * p[1] + p[2] * p[2] - al * al;
    CHECK(off2 < 0.08f * 0.08f);
}
static void melee_lift_keeps_the_left_hand_on_short_handles()
{
    // INV-510 numbers: palm target along the shaft = -0.0086 - 0.09 = -0.0986. Pickaxe butt -0.091 -> lift; axe_d -0.122 and
    // sledgehammer_d -0.130 (Jay: fine) -> no lift.
    const float palm = -0.0986f;
    CHECK(Near(MeleeLift(true, -0.091f, palm), -0.091f + kMeleeButtMargin - palm));
    CHECK(MeleeLift(true, -0.091f, palm) > 0.02f);
    CHECK(Near(MeleeLift(true, -0.122f, palm), 0.0f));
    CHECK(Near(MeleeLift(true, -0.130f, palm), 0.0f));
    CHECK(Near(MeleeLift(false, -0.091f, palm), 0.0f));          // no table row: no lift, never a guess
}
static void melee_floor_from_the_table_and_slider()
{
    CHECK(Near(MeleeFloor(true, -0.091f, -0.16f), -0.091f));      // the weapon's own butt
    CHECK(Near(MeleeFloor(true, -0.30f, -0.16f), -0.16f));        // the slider can still raise the floor
    CHECK(Near(MeleeFloor(false, 0.0f, -0.16f), -0.16f));         // no table row: the slider
}
static void melee_palm_target_rests_the_palm_on_the_shaft()
{
    // INV-513: the palm faces -z of the hand, so its CENTRE sits (radius + depth) out along +z (made perpendicular to the shaft)
    const float* u = kMeleeShaftAxis;
    const float zStd[3] = { 0.30f, 0.20f, 0.93f };   // not perpendicular to u on purpose
    float p[3];
    MeleePalmTarget(u, -0.0986f, 0.0151f, zStd, p);
    const float a = p[0] * u[0] + p[1] * u[1] + p[2] * u[2];
    CHECK(Near(a, -0.0986f));                                                      // along the shaft: as asked
    const float q[3] = { p[0] - a * u[0], p[1] - a * u[1], p[2] - a * u[2] };
    CHECK(Near(std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]), 0.0151f + kMeleePalmDepth));   // radius + palm depth off the axis
    CHECK(q[0] * zStd[0] + q[1] * zStd[1] + q[2] * zStd[2] > 0.0f);               // on the +z (back-of-palm) side: palm faces the shaft
}
static void melee_roll_about_the_shaft()
{
    // INV-514: radius table every 30 deg, linear between, wraps at 360; negative angles wrap too
    const float r[12] = { 0.010f, 0.020f, 0.030f, 0.040f, 0.050f, 0.060f, 0.070f, 0.080f, 0.090f, 0.100f, 0.110f, 0.120f };
    CHECK(Near(MeleeRadiusAt(r, 0.0f), 0.010f));
    CHECK(Near(MeleeRadiusAt(r, 45.0f), 0.025f));
    CHECK(Near(MeleeRadiusAt(r, 345.0f), 0.065f));                 // between 330 (0.120) and 360 = 0 (0.010)
    CHECK(Near(MeleeRadiusAt(r, -15.0f), 0.065f));
    // rolling a vector about u keeps it perpendicular / length and the rolled quaternion turns the hand's z the same way
    const float* u = kMeleeShaftAxis;
    const float v[3] = { 1.0f, 0.0f, 0.0f };
    float o[3];
    RotateAboutAxis(v, u, 60.0f, o);
    CHECK(Near(o[0] * u[0] + o[1] * u[1] + o[2] * u[2], 0.0f) && Near(std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]), 1.0f));
    CHECK(Near(o[0], 0.5f));                                         // cos 60
    const Q4 q = QMul(QAxisAngle(u, 60.0f), Q4{ 0, 0, 0, 1 });
    const V3 qr = QRotate(q, V3{ 1.0f, 0.0f, 0.0f });
    CHECK(Near(qr.x, o[0]) && Near(qr.y, o[1]) && Near(qr.z, o[2]));
}
static void melee_wrist_shift_moves_the_handle_onto_the_finger_bases()
{
    // INV-515: the hand moves `cm` toward its WRIST (-x of the hand), made perpendicular to the shaft so the stack is unchanged
    const float* u = kMeleeShaftAxis;
    const float xHand[3] = { -0.274f, -0.004f, 0.962f };
    float d[3];
    MeleeWristShift(xHand, u, 3.5f, d);
    CHECK(Near(d[0] * u[0] + d[1] * u[1] + d[2] * u[2], 0.0f));                       // not along the shaft
    CHECK(Near(std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), 0.035f));          // 3.5 cm
    CHECK(d[0] * xHand[0] + d[1] * xHand[1] + d[2] * xHand[2] < 0.0f);                // toward the wrist (against the fingers)
    MeleeWristShift(xHand, u, 0.0f, d);
    CHECK(Near(d[0], 0.0f) && Near(d[1], 0.0f) && Near(d[2], 0.0f));
}
static void finger_blend_endpoints()
{
    const float auth[12] = { 1, 0, 0, 0.03f, 0, 1, 0, 0.01f, 0, 0, 1, -0.02f };
    float grip[9];
    QToMat3(AxisAngle({ 0, 1, 0 }, 60.0f), grip);
    float out[12];
    FingerBlend(auth, grip, 0.0f, out);
    CHECK(MaxAbsDiff(out, auth, 12) < 1e-5f);
    FingerBlend(auth, grip, 1.0f, out);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) { CHECK(Near(out[4 * r + c], grip[3 * r + c], 1e-4f)); }
        CHECK(Near(out[4 * r + 3], auth[4 * r + 3], 1e-6f));   // translation stays authored (rigid bones)
    }
    FingerBlend(auth, grip, 0.5f, out);
    const float h[9] = { out[0], out[1], out[2], out[4], out[5], out[6], out[8], out[9], out[10] };
    CHECK(Near(QAngleDeg(Mat3ToQ(h), kId), 30.0f, 0.1f));
}

int main()
{
    norm_mesh_strips_case_msh_and_hq();
    classify_rules();
    generated_table_selects_rifles_not_pistols();
    gun_zone_bounds();
    no_jump_at_grab();
    left_hand_up_steers_the_gun();
    left_keeps_its_grab_rotation_on_the_gun();
    right_roll_rolls_the_gun();
    hands_together_wrist_aims_alone();
    latch_refuses_hands_together();
    no_flip_when_the_line_passes_the_wrist_up();
    share_turns_the_gun_part_way();
    hands_coming_together_fade_without_a_snap();
    line_pointing_back_holds_the_last_pull();
    grab_hold_release_blend();
    press_outside_zone_is_refused_and_l1_passes();
    press_outside_then_enter_is_not_grab();
    first_poll_held_is_not_grab();
    holster_busy_refuses_grab();
    melee_and_none_refused();
    kind_change_while_gripped_cancels();
    squeeze_inactive_releases();
    regrab_during_release_continues_from_current_t();
    fresh_grab_is_not_resumed();
    quat_mat_round_trip();
    ortho_strips_scale();
    rigid_inverse_round_trip();
    latch_then_pin_lands_handholder_on_holderl();
    pin_refuses_non_orthonormal_grip();
    pin_refuses_degenerate_inputs();
    sphere_zone_hysteresis();
    zero_slide_is_instant();
    palm_offset_lands_palm_on_holderl();
    wrist_excess_bins();
    forend_offset_and_self_check();
    two_hand_overflow_pick();
    hand_in_frame_round_trip();
    standard_cworld_draws_the_standard();
    slerp_m3_ends_and_middle();
    slide_zone_strip();
    slide_rel_moves_along_holderl_z();
    pinned_reach_blend();
    offset_rel_in_holderl_frame();
    zone_point_is_the_drawn_palm();
    classify_melee_by_desc_flag();
    classify_melee_by_item_type();
    grip_table_rows_are_sane();
    melee_shaft_from_row();
    melee_grabs_when_on();
    melee_refused_when_off_or_outside();
    swap_kind_while_gripped_cancels();
    swap_same_kind_while_gripped_cancels();
    melee_off_mid_grip_lets_go();
    melee_zone_strip_below_the_right_hand();
    point_in_frame_round_trip();
    melee_shaft_check();
    melee_rel_is_absolute();
    melee_slide_range();
    melee_curl_matches_the_generator();
    melee_std_and_palm_sane();
    finger_blend_endpoints();
    melee_lift_keeps_the_left_hand_on_short_handles();
    melee_floor_from_the_table_and_slider();
    melee_palm_target_rests_the_palm_on_the_shaft();
    melee_roll_about_the_shaft();
    melee_wrist_shift_moves_the_handle_onto_the_finger_bases();
    std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail;
}
