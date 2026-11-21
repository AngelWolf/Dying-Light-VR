// Standalone unit test for src/src/ovr2xr/holster_logic.h - NOT part of the DLL build.
// docs/superpowers/plans/2026-09-29-shoulder-holster.md Task 1. Exit code = number of failures.
#include "holster_logic.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>

using namespace dlvr::holster;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
static bool NearV(const Vec3& a, const Vec3& b) { return Near(a.x, b.x) && Near(a.y, b.y) && Near(a.z, b.z); }

struct Q { float x, y, z, w; };
static Q AxisAngle(float ax, float ay, float az, float deg)
{
    const float h = deg * 0.00872664626f;  // half angle in radians
    const float s = std::sin(h);
    return { ax * s, ay * s, az * s, std::cos(h) };
}
static Q Mul(const Q& a, const Q& b)
{
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
             a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
static Vec3 Off(const Q& q, const Vec3& worldRel)
{
    const Vec3 head{ 1.0f, 1.7f, -2.0f };  // arbitrary non-zero head position
    const Vec3 palm{ head.x + worldRel.x, head.y + worldRel.y, head.z + worldRel.z };
    return LocalOffset(head, q.x, q.y, q.z, q.w, palm);
}

// ===== Body-lock (docs/superpowers/plans/2026-10-20-holster-body-lock.md Task 1) =====
static const float kD2R = 0.0174532925f;
static Vec3 ToWorld(const Vec3& local, float yawDeg)   // the inverse of LocalOffsetYaw's Ry(-yaw)
{
    const float c = std::cos(yawDeg * kD2R), s = std::sin(yawDeg * kD2R);
    return { local.x * c + local.z * s, local.y, -local.x * s + local.z * c };
}
static Vec3 Add(const Vec3& a, const Vec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }

static void facing_matches_head_yaw_convention()
{
    const Q q = AxisAngle(0, 1, 0, 40.0f);
    CHECK(Near(HeadYaw(q.x, q.y, q.z, q.w) / kD2R, 40.0f, 1e-3f));
    const Vec3 eye{ 0.3f, 1.6f, -0.2f };
    const Vec3 l = Add(eye, ToWorld({ -0.25f, -0.6f, 0.0f }, 40.0f));
    const Vec3 r = Add(eye, ToWorld({ 0.25f, -0.6f, 0.0f }, 40.0f));
    CHECK(Near(HandLineYaw(l, r) / kD2R, 40.0f, 1e-3f));
    // and LocalOffsetYaw at that yaw puts the right hand on +X
    const Vec3 lr = LocalOffsetYaw(eye, 40.0f * kD2R, r);
    CHECK(Near(lr.x, 0.25f) && Near(lr.y, -0.6f) && Near(lr.z, 0.0f));
}

static void tracking_world_round_trip()
{
    for (float deg : { 0.0f, 73.0f, -160.0f }) {
        const float c = std::cos(deg * kD2R), s = std::sin(deg * kD2R);
        const Vec3 o{ 0.12f, -0.55f, 0.31f };
        const Vec3 w{ o.x * c + o.z * s, o.y, -o.x * s + o.z * c };   // boneblend.cpp T formula
        CHECK(NearV(TrackingFromWorld(w, c, s), o));
    }
}

static void neck_point_ignores_head_rotation()
{
    const Vec3 neck{ 0.4f, 1.45f, -0.3f };
    const Vec3 n{ 0.008f, 0.112f, -0.084f };
    for (float yaw : { 0.0f, 90.0f, 180.0f }) {
        const Q q = Mul(AxisAngle(0, 1, 0, yaw), AxisAngle(1, 0, 0, -35.0f));
        // eye = neck + R n (R from q, via LocalOffset's inverse is not needed: build R n directly)
        const float x = q.x, y = q.y, z = q.z, w = q.w;
        const Vec3 rn{ (1 - 2 * (y * y + z * z)) * n.x + 2 * (x * y - z * w) * n.y + 2 * (x * z + y * w) * n.z,
                       2 * (x * y + z * w) * n.x + (1 - 2 * (x * x + z * z)) * n.y + 2 * (y * z - x * w) * n.z,
                       2 * (x * z - y * w) * n.x + 2 * (y * z + x * w) * n.y + (1 - 2 * (x * x + y * y)) * n.z };
        const Vec3 eye = Add(neck, rn);
        CHECK(NearV(NeckPoint(eye, q.x, q.y, q.z, q.w, n), neck));
    }
}

static void both_hands_in_zones_updates()
{
    // (shoulder, right hip, left hip) - a squeezed grip is no longer a freeze (2026-11-07), only a block on the rest square-up
    CHECK(!FreezeFacing(false, true, true));
    CHECK(FreezeFacing(false, true, false));
    CHECK(FreezeFacing(false, false, true));
    CHECK(!FreezeFacing(false, false, false));
}

// Review I2 (2026-10-20): the left hand resting in the left hip + the right hand reaching the shoulder is a DRAW, not a hip line.
static void shoulder_reach_freezes_with_left_hand_at_hip()
{
    CHECK(FreezeFacing(true, false, true));
    CHECK(FreezeFacing(true, false, false));
    CHECK(FreezeFacing(true, true, true));
}

// Review I1 (2026-10-20): the latch is taken from the LEVEL-head eye, so looking down at the latch moment bakes in no pitch.
static void latch_from_level_eye_ignores_pitch()
{
    const Vec3 n{ 0.008f, 0.112f, -0.084f };
    const Vec3 neck{ 0.3f, 1.45f, -0.2f };
    const float yaw = 30.0f;
    const Q qLevel = AxisAngle(0, 1, 0, yaw);
    const Q qDown = Mul(qLevel, AxisAngle(1, 0, 0, -45.0f));
    const auto eyeOf = [&](const Q& q) {   // eye = neck + R n
        const Vec3 zero{ 0.0f, 0.0f, 0.0f };
        const Vec3 m = NeckPoint(zero, q.x, q.y, q.z, q.w, n);   // = -R n
        return Vec3{ neck.x - m.x, neck.y - m.y, neck.z - m.z };
    };
    const Vec3 eyeLevel = eyeOf(qLevel), eyeDown = eyeOf(qDown);
    CHECK(!NearV(eyeLevel, eyeDown));                                     // the control: pitch DOES move the raw eye
    const float hyDown = HeadYaw(qDown.x, qDown.y, qDown.z, qDown.w);
    CHECK(NearV(LevelEye(NeckPoint(eyeDown, qDown.x, qDown.y, qDown.z, qDown.w, n), hyDown, n), eyeLevel));
    CHECK(NearV(LevelEye(neck, yaw * kD2R, n), eyeLevel));
}

// ===== The facing (2026-11-07): a slow head average, squared up to the hand line only for a standing player's hands at rest =====
static const Vec3 kNeck{ 0.3f, 1.45f, -0.2f };
static const float kDt = 0.011f;

// Hands relative to the neck, in a body frame at bodyYawDeg: x out to the side, y below the neck (negative), z fore / aft.
static void HandsAt(float bodyYawDeg, float sideX, float dropY, float foreZ, Vec3& l, Vec3& r)
{
    l = Add(kNeck, ToWorld({ -sideX, dropY, foreZ }, bodyYawDeg));
    r = Add(kNeck, ToWorld({ sideX, dropY, foreZ }, bodyYawDeg));
}
// Seated / in play: hands up in front (0.25 m below the neck, 0.30 m forward) - never a rest pose.
static void HandsUp(Vec3& l, Vec3& r) { HandsAt(0.0f, 0.20f, -0.25f, -0.30f, l, r); }

// Steps the facing for `secs` with the head at headDeg and the given hands, returns the last reason.
static FacingWhy Run(Facing& f, double& t, float secs, float headDeg, const Vec3& l, const Vec3& r, bool freeze = false,
                     bool squeezed = false)
{
    FacingWhy w = FacingWhy::Frozen;
    for (int i = 0; i * kDt < secs; ++i, t += kDt) {
        w = FacingStep(f, kNeck, l, r, true, true, headDeg * kD2R, freeze, squeezed, t, kDt);
    }
    return w;
}

static void drift_follows_the_head_slowly()
{
    Facing f; double t = 0.0; Vec3 l, r; HandsUp(l, r);
    Run(f, t, 0.5f, 0.0f, l, r);
    CHECK(Near(f.yaw, 0.0f, 1e-4f));
    CHECK(Run(f, t, 1.0f, 60.0f, l, r) == FacingWhy::Drift);
    CHECK(Near(f.yaw / kD2R, 60.0f * (1.0f - std::exp(-1.0f / kDriftTauS)), 1.0f));   // ~13 deg after 1 s, not 60
    Run(f, t, 15.0f, 60.0f, l, r);
    CHECK(Near(f.yaw / kD2R, 60.0f, 1.5f));                                            // and arrives
}

static void quick_look_barely_moves_it()
{
    Facing f; double t = 0.0; Vec3 l, r; HandsUp(l, r);
    Run(f, t, 1.0f, 0.0f, l, r);
    Run(f, t, 0.3f, 60.0f, l, r);   // a glance
    Run(f, t, 0.3f, 0.0f, l, r);
    CHECK(std::fabs(f.yaw / kD2R) < 5.0f);
    CHECK(std::fabs(f.yaw / kD2R) > 0.5f);   // the control: it did move a little, so the drift is running
}

static void the_hands_no_longer_steer_it()
{
    // The old rule followed the hand line: a reach / swing turning it to 50 deg moved every zone. Now: no.
    Facing f; double t = 0.0; Vec3 l, r;
    HandsAt(50.0f, 0.20f, -0.25f, -0.30f, l, r);
    Run(f, t, 3.0f, 0.0f, l, r);
    CHECK(Near(f.yaw, 0.0f, 1e-4f));
}

static void limit_pulls_it_to_70_off_the_head()
{
    Facing f; double t = 0.0; Vec3 l, r; HandsUp(l, r);
    Run(f, t, 1.0f, 0.0f, l, r);
    CHECK(FacingStep(f, kNeck, l, r, true, true, 120.0f * kD2R, false, false, t, kDt) == FacingWhy::Limit);
    CHECK(Near(f.yaw / kD2R, 50.0f, 0.5f));   // 120 - 70, never left behind the player
    t += kDt;
    CHECK(FacingStep(f, kNeck, l, r, true, true, -120.0f * kD2R, false, false, t, kDt) == FacingWhy::Limit);   // the seam way
    CHECK(Near(f.yaw / kD2R, -50.0f, 0.5f));
}

static void a_hand_in_a_zone_holds_it()
{
    Facing f; double t = 0.0; Vec3 l, r; HandsUp(l, r);
    Run(f, t, 1.0f, 0.0f, l, r);
    CHECK(Run(f, t, 2.0f, 60.0f, l, r, /*freeze=*/true) == FacingWhy::Frozen);
    CHECK(Near(f.yaw, 0.0f, 1e-4f));
    CHECK(Run(f, t, 0.1f, 120.0f, l, r, true) == FacingWhy::Frozen);   // not even the limit moves a zone under the hand
    CHECK(Near(f.yaw, 0.0f, 1e-4f));
}

static void a_squeezed_grip_does_not_hold_the_drift()
{
    Facing f; double t = 0.0; Vec3 l, r; HandsUp(l, r);
    Run(f, t, 1.0f, 0.0f, l, r);
    CHECK(Run(f, t, 1.0f, 60.0f, l, r, false, /*squeezed=*/true) == FacingWhy::Drift);
    CHECK(f.yaw / kD2R > 10.0f);
}

static void standing_rest_squares_it_up()
{
    Facing f; double t = 0.0; Vec3 l, r;
    HandsAt(30.0f, 0.22f, -0.70f, 0.0f, l, r);   // arms hanging, the body turned 30 deg from the head
    CHECK(RestPose(kNeck, 0.0f, l, r));
    CHECK(Run(f, t, 0.2f, 0.0f, l, r) == FacingWhy::Drift);   // < kRestHoldS: not yet
    CHECK(Run(f, t, 2.5f, 0.0f, l, r) == FacingWhy::Rest);
    CHECK(Near(f.yaw / kD2R, 30.0f, 1.0f));
    CHECK(Run(f, t, 1.0f, 0.0f, l, r, false, /*squeezed=*/true) == FacingWhy::Drift);   // holding something: not at rest
}

static void rest_pose_misses_seated_and_each_condition()
{
    Vec3 l, r;
    HandsAt(0.0f, 0.22f, -0.70f, 0.0f, l, r);
    CHECK(RestPose(kNeck, 0.0f, l, r));                         // the control
    HandsUp(l, r);
    CHECK(!RestPose(kNeck, 0.0f, l, r));                        // seated / in play
    HandsAt(0.0f, 0.22f, -0.45f, -0.30f, l, r);
    CHECK(!RestPose(kNeck, 0.0f, l, r));                        // seated, hands in the lap
    HandsAt(0.0f, 0.22f, -0.51f, 0.0f, l, r);
    CHECK(!RestPose(kNeck, 0.0f, l, r));                        // drop 0.51 < 0.55 (seated max seen: 0.506)
    HandsAt(0.0f, 0.22f, -0.70f, -0.25f, l, r);
    CHECK(!RestPose(kNeck, 0.0f, l, r));                        // 0.25 forward
    HandsAt(0.0f, 0.22f, -0.70f, 0.25f, l, r);
    CHECK(!RestPose(kNeck, 0.0f, l, r));                        // 0.25 behind
    HandsAt(0.0f, 0.10f, -0.70f, 0.0f, l, r);
    CHECK(!RestPose(kNeck, 0.0f, l, r));                        // only 0.10 out (and 0.20 apart)
    HandsAt(0.0f, 0.22f, -0.70f, 0.0f, l, r);
    CHECK(!RestPose(kNeck, 0.0f, r, l));                        // hands crossed: each on the wrong side
    HandsAt(50.0f, 0.22f, -0.70f, 0.0f, l, r);
    CHECK(!RestPose(kNeck, 0.0f, l, r));                        // hand line 50 deg off the head
}

static void moving_hands_are_not_at_rest()
{
    Facing f; double t = 0.0;
    FacingWhy last = FacingWhy::Frozen;
    for (int i = 0; i < 300; ++i, t += kDt) {
        Vec3 l, r;
        HandsAt(0.0f, 0.22f, -0.70f, 0.12f * std::sin(i * 0.2f), l, r);   // swinging ~2.2 m/s peak, always inside the pose box
        CHECK(RestPose(kNeck, 0.0f, l, r));                                // the control: only the speed can refuse it
        last = FacingStep(f, kNeck, l, r, true, true, 0.0f, false, false, t, kDt);
        CHECK(last != FacingWhy::Rest);
    }
    CHECK(last == FacingWhy::Drift);
}

static void seam_short_way()
{
    Facing f; double t = 0.0; Vec3 l, r; HandsUp(l, r);
    Run(f, t, 1.0f, 170.0f, l, r);
    float minAbs = 1000.0f;
    for (int i = 0; i < 1000; ++i, t += kDt) {
        FacingStep(f, kNeck, l, r, true, true, -170.0f * kD2R, false, false, t, kDt);
        const float a = std::fabs(f.yaw / kD2R);
        if (a < minAbs) { minAbs = a; }
    }
    CHECK(minAbs > 160.0f);                       // never swung through 0
    CHECK(Near(f.yaw / kD2R, -170.0f, 3.0f));     // the control: it did get there
}

static void latch_is_identity_at_latch()
{
    Facing f;
    Vec3 l, r;
    HandsUp(l, r);
    const float yaw = 25.0f;
    const Q q = AxisAngle(0, 1, 0, yaw);
    const Vec3 eye{ 0.2f, 1.6f, 0.1f };
    const Vec3 anchor{ 0.25f, 1.0f, 0.15f };
    Latch L;
    double t = 0.0;
    Run(f, t, 0.2f, yaw, l, r);
    CHECK(!LatchTry(L, f, t, yaw * kD2R, eye, anchor));                  // < 0.5 s steady
    Run(f, t, 0.7f, yaw, l, r);
    Latch L2;
    CHECK(!LatchTry(L2, f, t, (yaw + 20.0f) * kD2R, eye, anchor));       // head 20 deg off the body
    CHECK(LatchTry(L, f, t, yaw * kD2R, eye, anchor));
    const Vec3 palm{ 0.5f, 1.1f, -0.3f };
    const Vec3 a = LocalOffsetYaw(VirtualHead(anchor, f.yaw, L), f.yaw, palm);
    const Vec3 b = LocalOffset(eye, q.x, q.y, q.z, q.w, palm);
    CHECK(Near(a.x, b.x, 1e-3f) && Near(a.y, b.y, 1e-3f) && Near(a.z, b.z, 1e-3f));
}

static void virtual_head_turns_with_body()
{
    Latch L;
    L.have = true;
    L.eyeFromAnchor = { 0.0f, 0.6f, -0.1f };
    const Vec3 anchor{ 1.0f, 1.0f, 1.0f };
    const Vec3 v = VirtualHead(anchor, 90.0f * kD2R, L);
    const Vec3 want = Add(anchor, ToWorld(L.eyeFromAnchor, 90.0f));
    CHECK(NearV(v, want));
}

static void identity_head_passes_offset_through()
{
    const Q q{ 0, 0, 0, 1 };
    CHECK(NearV(Off(q, { 0.2f, -0.1f, 0.1f }), { 0.2f, -0.1f, 0.1f }));
}
static void yaw90_rotates_offset()
{
    // +90 deg about +Y faces the head toward world -X; its RIGHT is world -Z.
    const Q q = AxisAngle(0, 1, 0, 90.0f);
    CHECK(NearV(Off(q, { 0.0f, 0.0f, -0.2f }), { 0.2f, 0.0f, 0.0f }));
}
static void pitch_does_not_move_offset()
{
    const Q q = AxisAngle(1, 0, 0, -60.0f);  // looking down 60 deg
    CHECK(NearV(Off(q, { 0.15f, -0.15f, 0.10f }), { 0.15f, -0.15f, 0.10f }));
}
static void yaw_and_pitch_combined()
{
    // yaw +90 then pitch -30 (intrinsic). World offset = Ry(90) * (0.15, -0.15, 0.10) = (0.10, -0.15, -0.15).
    const Q q = Mul(AxisAngle(0, 1, 0, 90.0f), AxisAngle(1, 0, 0, -30.0f));
    CHECK(NearV(Off(q, { 0.10f, -0.15f, -0.15f }), { 0.15f, -0.15f, 0.10f }));
}

static const Params kP{ { 0.15f, -0.15f, 0.10f }, 0.15f };
static Input At(float squeeze, bool poseOk, const Vec3& local, bool squeezeActive = true)
{
    return { squeezeActive, squeeze, poseOk, local };
}
static const Vec3 kIn{ 0.15f, -0.15f, 0.10f };   // dist 0 - in the zone
static const Vec3 kOut{ 0.40f, -0.50f, -0.30f }; // far outside

static void press_in_zone_suppresses_then_switches_once()
{
    Machine m;
    Step s = m.Update(At(0.0f, true, kOut), kP);           // prime, grip up
    CHECK(!s.suppressR1);
    s = m.Update(At(0.0f, true, kIn), kP);                 // enter, grip up
    CHECK(s.entered && s.inZone && m.state() == Press::Idle);
    s = m.Update(At(0.9f, true, kIn), kP);                 // press in zone
    CHECK(s.pressHolster && !s.pressNormal && s.suppressR1 && m.state() == Press::Holster);
    s = m.Update(At(0.9f, true, kIn), kP);                 // held
    CHECK(s.suppressR1 && !s.releaseSwitch);
    s = m.Update(At(0.1f, true, kIn), kP);                 // release
    CHECK(s.releaseSwitch && !s.cancelled && !s.suppressR1 && m.state() == Press::Idle);
    s = m.Update(At(0.1f, true, kIn), kP);                 // no second switch
    CHECK(!s.releaseSwitch && !s.suppressR1);
}
// ===== Hip yield (docs/superpowers/plans/2026-10-27-roomscale-physical-crouch.md Task 4, spec §2.4) =====
static Input AtY(float squeeze, const Vec3& local, Yield y)
{
    Input in = At(squeeze, true, local);
    in.yield = y;
    return in;
}
static void yield_in_zone_is_normal()
{
    Machine m;
    m.Update(At(0.0f, true, kIn), kP);
    m.Update(At(0.0f, true, kIn), kP);
    Step s = m.Update(AtY(0.9f, kIn, Yield::Yes), kP);     // a body under the hand at the press edge
    CHECK(s.pressNormal && !s.pressHolster && !s.suppressR1 && !s.ownsGrip && s.yielded && m.state() == Press::Normal);
    s = m.Update(AtY(0.9f, kIn, Yield::No), kP);           // held: the yield is not re-read
    CHECK(!s.suppressR1 && !s.ownsGrip && !s.pressHolster && m.state() == Press::Normal);
    s = m.Update(At(0.1f, true, kIn), kP);                 // release: nothing fired, nothing to switch
    CHECK(!s.releaseSwitch && !s.cancelled && m.state() == Press::Idle);
}
static void yield_outside_zone_changes_nothing()
{
    Machine m;
    m.Update(At(0.0f, true, kOut), kP);
    Step s = m.Update(AtY(0.9f, kOut, Yield::Yes), kP);
    CHECK(s.pressNormal && !s.yielded && !s.suppressR1 && !s.ownsGrip && m.state() == Press::Normal);
    m.Update(At(0.0f, true, kOut), kP);
    s = m.Update(AtY(0.9f, kOut, Yield::Pending), kP);     // Pending outside the zone is not a hip press either
    CHECK(s.pressNormal && !s.yielded && !s.suppressR1 && m.state() == Press::Normal);
}
static void yield_after_edge_keeps_holster()
{
    Machine m;
    m.Update(At(0.0f, true, kIn), kP);
    m.Update(At(0.0f, true, kIn), kP);
    Step s = m.Update(AtY(0.9f, kIn, Yield::No), kP);
    CHECK(s.pressHolster && s.suppressR1 && s.ownsGrip && !s.yielded);
    s = m.Update(AtY(0.9f, kIn, Yield::Yes), kP);          // a body arriving later does not take a fired press back
    CHECK(!s.pressNormal && !s.yielded && s.suppressR1 && s.ownsGrip && m.state() == Press::Holster);
    s = m.Update(At(0.1f, true, kIn), kP);
    CHECK(s.releaseSwitch);
}
static void pending_then_yield()
{
    Machine m;
    m.Update(At(0.0f, true, kIn), kP);
    m.Update(At(0.0f, true, kIn), kP);
    Step s = m.Update(AtY(0.9f, kIn, Yield::Pending), kP); // no corpse, the limb reading is due
    CHECK(s.suppressR1 && !s.ownsGrip && !s.pressHolster && !s.pressNormal && !s.yielded && m.state() == Press::Pending);
    s = m.Update(AtY(0.9f, kIn, Yield::Pending), kP);      // still waiting
    CHECK(s.suppressR1 && !s.ownsGrip && !s.pressHolster && m.state() == Press::Pending);
    s = m.Update(AtY(0.9f, kIn, Yield::Yes), kP);          // a limb under the hand
    CHECK(s.yielded && !s.pressHolster && !s.suppressR1 && !s.ownsGrip && m.state() == Press::Normal);
    s = m.Update(At(0.1f, true, kIn), kP);
    CHECK(!s.releaseSwitch && !s.cancelled);
}
static void pending_then_holster()
{
    Machine m;
    m.Update(At(0.0f, true, kIn), kP);
    m.Update(At(0.0f, true, kIn), kP);
    Step s = m.Update(AtY(0.9f, kIn, Yield::Pending), kP);
    CHECK(!s.pressHolster && m.state() == Press::Pending);
    s = m.Update(AtY(0.9f, kOut, Yield::No), kP);          // nothing came (hand moved meanwhile): the holster fires now
    CHECK(s.pressHolster && s.ownsGrip && s.suppressR1 && !s.yielded && m.state() == Press::Holster);
    s = m.Update(AtY(0.9f, kOut, Yield::No), kP);          // fires once
    CHECK(!s.pressHolster && s.suppressR1);
    s = m.Update(At(0.1f, true, kOut), kP);
    CHECK(s.releaseSwitch);
}
static void pending_release_cancels()
{
    Machine m;
    m.Update(At(0.0f, true, kIn), kP);
    m.Update(At(0.0f, true, kIn), kP);
    m.Update(AtY(0.9f, kIn, Yield::Pending), kP);
    Step s = m.Update(AtY(0.1f, kIn, Yield::Pending), kP); // let go before the reading came
    CHECK(!s.pressHolster && !s.releaseSwitch && !s.cancelled && !s.suppressR1 && m.state() == Press::Idle);
    Machine m2;                                            // the squeeze action going inactive while Pending: the same
    m2.Update(At(0.0f, true, kIn), kP);
    m2.Update(At(0.0f, true, kIn), kP);
    m2.Update(AtY(0.9f, kIn, Yield::Pending), kP);
    s = m2.Update(At(0.0f, true, kIn, false), kP);
    CHECK(!s.pressHolster && !s.releaseSwitch && !s.cancelled && !s.suppressR1 && m2.state() == Press::Idle);
}
static void no_yield_is_today()
{
    Machine m;                                             // Input's yield defaults to No: a holster press owns the grip
    m.Update(At(0.0f, true, kIn), kP);
    m.Update(At(0.0f, true, kIn), kP);
    Step s = m.Update(At(0.9f, true, kIn), kP);
    CHECK(s.pressHolster && s.ownsGrip && s.suppressR1 && !s.yielded);
    s = m.Update(At(0.1f, true, kIn), kP);
    CHECK(s.releaseSwitch && !s.ownsGrip);
}

static void normal_press_entering_zone_stays_normal()
{
    Machine m;
    m.Update(At(0.0f, true, kOut), kP);
    Step s = m.Update(At(0.9f, true, kOut), kP);           // jump outside the zone
    CHECK(s.pressNormal && !s.suppressR1 && m.state() == Press::Normal);
    s = m.Update(At(0.9f, true, kIn), kP);                 // held grip drifts into the zone
    CHECK(s.entered && !s.suppressR1 && m.state() == Press::Normal);
    s = m.Update(At(0.0f, true, kIn), kP);                 // release
    CHECK(!s.releaseSwitch && !s.suppressR1);
}
static void holster_press_leaving_zone_stays_latched()
{
    Machine m;
    m.Update(At(0.0f, true, kIn), kP);
    m.Update(At(0.0f, true, kIn), kP);
    Step s = m.Update(At(0.9f, true, kIn), kP);
    CHECK(s.pressHolster);
    s = m.Update(At(0.9f, true, kOut), kP);                // arm comes back while still held
    CHECK(s.exited && s.suppressR1);
    s = m.Update(At(0.0f, true, kOut), kP);
    CHECK(s.releaseSwitch);
}
static void pose_lost_mid_holster_press_stays_latched()
{
    Machine m;
    m.Update(At(0.0f, true, kIn), kP);
    Step s = m.Update(At(0.9f, true, kIn), kP);
    CHECK(s.pressHolster);
    s = m.Update(At(0.9f, false, kIn), kP);                // tracking lost behind the shoulder
    CHECK(s.exited && !s.inZone && Near(s.dist, -1.0f) && s.suppressR1);
    s = m.Update(At(0.0f, false, kIn), kP);                // released while still untracked
    CHECK(s.releaseSwitch && !s.suppressR1);
}
static void squeeze_inactive_cancels_without_switch()
{
    Machine m;
    m.Update(At(0.0f, true, kIn), kP);
    Step s = m.Update(At(0.9f, true, kIn), kP);
    CHECK(s.pressHolster);
    s = m.Update(At(0.9f, true, kIn, /*squeezeActive=*/false), kP);
    CHECK(s.cancelled && !s.releaseSwitch && !s.suppressR1 && m.state() == Press::Idle);
}
static void grip_down_on_first_poll_is_normal()
{
    Machine m;
    Step s = m.Update(At(0.9f, true, kIn), kP);            // first ever poll, grip already held, in zone
    CHECK(!s.pressHolster && !s.pressNormal && !s.suppressR1 && m.state() == Press::Normal);
    s = m.Update(At(0.0f, true, kIn), kP);
    CHECK(!s.releaseSwitch);
}
static void hysteresis_no_chatter()
{
    // centre (0.15,-0.15,0.10), r 0.15, exit 0.18. Move along +X from the centre.
    auto x = [](float d) { return Vec3{ 0.15f + d, -0.15f, 0.10f }; };
    Machine m;
    Step s = m.Update(At(0.0f, true, x(0.16f)), kP);
    CHECK(!s.inZone);                                      // 0.16 >= 0.15: not entered
    s = m.Update(At(0.0f, true, x(0.14f)), kP);
    CHECK(s.entered && s.inZone);
    s = m.Update(At(0.0f, true, x(0.17f)), kP);
    CHECK(!s.exited && s.inZone);                          // 0.17 <= 0.18: stays in
    s = m.Update(At(0.0f, true, x(0.19f)), kP);
    CHECK(s.exited && !s.inZone);
    s = m.Update(At(0.0f, true, x(0.16f)), kP);
    CHECK(!s.entered && !s.inZone);                        // must come back under 0.15
}
static void no_pose_press_is_normal()
{
    Machine m;
    m.Update(At(0.0f, false, kIn), kP);
    Step s = m.Update(At(0.9f, false, kIn), kP);           // fails closed: never a holster press
    CHECK(s.pressNormal && !s.suppressR1);
}

static void rate_gate_limits_flapping()
{
    // Review M1: a pose flapping behind the shoulder must not re-buzz / re-log every frame.
    RateGate g;
    CHECK(g.Allow(1000, 500));                             // first event always passes
    CHECK(!g.Allow(1011, 500));                            // next frame: blocked
    CHECK(!g.Allow(1499, 500));
    CHECK(g.skipped() == 2);
    CHECK(g.Allow(1500, 500));                             // gap elapsed: passes, skipped count handed over
    CHECK(g.TakeSkipped() == 2 && g.skipped() == 0);
}

static void tap_pulse_holds_for_duration_then_ends_once()
{
    // The weapon-switch output: D-pad right held for tapMs, well under the game's ~0.5 s hold (wheel).
    TapPulse t;
    CHECK(!t.Active(1000));
    t.Start(1000, 100);
    CHECK(t.Active(1000));
    CHECK(t.Active(1099));
    CHECK(!t.TakeEnded(1099));
    CHECK(!t.Active(1100));                                // 100 ms elapsed: released
    CHECK(t.TakeEnded(1100));                              // end reported exactly once
    CHECK(!t.TakeEnded(1200));
}
static void tap_pulse_restart_while_active_extends()
{
    TapPulse t;
    t.Start(1000, 100);
    t.Start(1050, 100);                                    // a second switch before the first tap ended
    CHECK(t.Active(1120));
    CHECK(!t.Active(1150));
}

// ---- left hand: hip + temple (two Machines on the one left grip, fire on the press edge) ----
static const Params kHip{ { -0.20f, -0.60f, 0.00f }, 0.15f };
static const Params kHead{ { -0.12f, 0.00f, 0.00f }, 0.10f };
static const Vec3 kAtHip{ -0.20f, -0.60f, 0.00f };
static const Vec3 kAtHead{ -0.12f, 0.00f, 0.00f };
static const Vec3 kNeither{ 0.30f, -0.30f, -0.40f };

static void zones_overlap_uses_exit_radii()
{
    // Exit spheres touch when centre distance < (ra + rb) * kExitScale.
    const Params a{ { 0.0f, 0.0f, 0.0f }, 0.10f };
    CHECK(ZonesOverlap(a, Params{ { 0.23f, 0.0f, 0.0f }, 0.10f }));   // 0.23 < 0.24
    CHECK(!ZonesOverlap(a, Params{ { 0.25f, 0.0f, 0.0f }, 0.10f }));  // 0.25 >= 0.24
    CHECK(!ZonesOverlap(kHip, kHead));                                 // the shipped guesses are disjoint
}
static void left_press_at_hip_fires_hip_only()
{
    Machine hip, head;
    hip.Update(At(0.0f, true, kAtHip), kHip);
    head.Update(At(0.0f, true, kAtHip), kHead);
    const Step h = hip.Update(At(0.9f, true, kAtHip), kHip);
    const Step t = head.Update(At(0.9f, true, kAtHip), kHead);
    CHECK(h.pressHolster && !t.pressHolster && t.pressNormal);
    CHECK(h.suppressR1 && !t.suppressR1);                  // L1 suppressed = either machine suppressing
}
static void left_press_at_head_fires_head_only()
{
    Machine hip, head;
    hip.Update(At(0.0f, true, kAtHead), kHip);
    head.Update(At(0.0f, true, kAtHead), kHead);
    const Step h = hip.Update(At(0.9f, true, kAtHead), kHip);
    const Step t = head.Update(At(0.9f, true, kAtHead), kHead);
    CHECK(t.pressHolster && !h.pressHolster && h.pressNormal);
}
static void left_press_elsewhere_fires_nothing_keeps_l1()
{
    Machine hip, head;
    hip.Update(At(0.0f, true, kNeither), kHip);
    head.Update(At(0.0f, true, kNeither), kHead);
    const Step h = hip.Update(At(0.9f, true, kNeither), kHip);
    const Step t = head.Update(At(0.9f, true, kNeither), kHead);
    CHECK(!h.pressHolster && !t.pressHolster && !h.suppressR1 && !t.suppressR1);
}
static void left_fire_on_grip_happens_once_per_press()
{
    // On-grip: the tap fires on pressHolster. Holding must not re-fire; the release is not a second fire.
    Machine hip;
    hip.Update(At(0.0f, true, kAtHip), kHip);
    int fires = 0;
    Step s = hip.Update(At(0.9f, true, kAtHip), kHip);
    fires += s.pressHolster ? 1 : 0;
    s = hip.Update(At(0.9f, true, kAtHip), kHip);
    fires += s.pressHolster ? 1 : 0;
    CHECK(s.suppressR1);
    s = hip.Update(At(0.0f, true, kAtHip), kHip);
    fires += s.pressHolster ? 1 : 0;
    CHECK(fires == 1 && !s.suppressR1);
}

// ---- right hand: shoulder + right hip (Jay 2026-10-04), two Machines on the one right grip ----
static const Params kRHip{ { 0.20f, -0.60f, 0.00f }, 0.15f };
static const Vec3 kAtRHip{ 0.20f, -0.60f, 0.00f };

static void right_shipped_zones_are_disjoint()
{
    CHECK(!ZonesOverlap(kP, kRHip));
}
static void right_press_at_hip_fires_hip_only()
{
    Machine shoulder, rhip;
    shoulder.Update(At(0.0f, true, kAtRHip), kP);
    rhip.Update(At(0.0f, true, kAtRHip), kRHip);
    const Step s = shoulder.Update(At(0.9f, true, kAtRHip), kP);
    const Step r = rhip.Update(At(0.9f, true, kAtRHip), kRHip);
    CHECK(r.pressHolster && !s.pressHolster && s.pressNormal);
    CHECK(r.suppressR1 && !s.suppressR1);                  // R1 withheld = either machine suppressing
}
static void right_press_at_shoulder_fires_shoulder_only()
{
    Machine shoulder, rhip;
    shoulder.Update(At(0.0f, true, kIn), kP);
    rhip.Update(At(0.0f, true, kIn), kRHip);
    const Step s = shoulder.Update(At(0.9f, true, kIn), kP);
    const Step r = rhip.Update(At(0.9f, true, kIn), kRHip);
    CHECK(s.pressHolster && !r.pressHolster && r.pressNormal);
}
static void right_press_elsewhere_fires_nothing_keeps_r1()
{
    Machine shoulder, rhip;
    shoulder.Update(At(0.0f, true, kOut), kP);
    rhip.Update(At(0.0f, true, kOut), kRHip);
    const Step s = shoulder.Update(At(0.9f, true, kOut), kP);
    const Step r = rhip.Update(At(0.9f, true, kOut), kRHip);
    CHECK(!s.pressHolster && !r.pressHolster && !s.suppressR1 && !r.suppressR1);
}

// ---- holster_hip_from_calib (Jay 2026-10-15): the hip zone at the BELT, from the calibration's down-pose resting hand ----
// Jay's numbers: "10-15cm UP from the resting hand by side pose", "Don't go further forward by more than 4cm".
static void belt_centre_lifts_and_moves_forward()
{
    const Vec3 rest{ 0.25f, -0.80f, 0.02f };
    // +Y up, +Z BEHIND: 12.5 cm up -> y + 0.125; 3 cm forward -> z - 0.03. x (sideways) is the resting hand's.
    CHECK(NearV(BeltCentre(rest, 12.5f, 3.0f), { 0.25f, -0.675f, -0.01f }));
}
static void belt_centre_clamps_to_jays_ranges()
{
    const Vec3 rest{ -0.25f, -0.80f, 0.0f };
    CHECK(Near(BeltCentre(rest, 30.0f, 0.0f).y, -0.65f));    // up stops at 15 cm
    CHECK(Near(BeltCentre(rest, 0.0f, 0.0f).y, -0.70f));     // and starts at 10 cm
    CHECK(Near(BeltCentre(rest, 12.5f, 10.0f).z, -0.04f));   // forward stops at 4 cm
    CHECK(Near(BeltCentre(rest, 12.5f, -5.0f).z, 0.0f));     // never backward
    CHECK(Near(BeltCentre(rest, 12.5f, 0.0f).x, -0.25f));    // sideways untouched
}
static void belt_forward_is_toward_the_face()
{
    // The deliberate miss this guards: +Z is BEHIND (OpenXR view axes), so "forward" must DECREASE z.
    const Vec3 rest{ 0.20f, -0.80f, 0.0f };
    CHECK(BeltCentre(rest, 12.5f, 4.0f).z < rest.z);
}
static void rest_hand_plausible_selects_the_right_set()
{
    // Accepted: a hand hanging at its own side, below the eyes, near the body.
    CHECK(RestHandPlausible({ 0.22f, -0.80f, 0.03f }, /*left=*/false));
    CHECK(RestHandPlausible({ -0.22f, -0.80f, 0.03f }, /*left=*/true));
    // Refused (each must MISS): the wrong side, too high, too low, too far forward / back, too far out, not finite.
    CHECK(!RestHandPlausible({ -0.22f, -0.80f, 0.03f }, false));
    CHECK(!RestHandPlausible({ 0.22f, -0.80f, 0.03f }, true));
    CHECK(!RestHandPlausible({ 0.22f, -0.20f, 0.03f }, false));
    CHECK(!RestHandPlausible({ 0.22f, -1.30f, 0.03f }, false));
    CHECK(!RestHandPlausible({ 0.22f, -0.80f, -0.45f }, false));
    CHECK(!RestHandPlausible({ 0.22f, -0.80f, 0.45f }, false));
    CHECK(!RestHandPlausible({ 0.70f, -0.80f, 0.03f }, false));
    CHECK(!RestHandPlausible({ std::nanf(""), -0.80f, 0.03f }, false));
    CHECK(!RestHandPlausible({ 0.22f, std::nanf(""), 0.03f }, false));
}
static void calibrated_belt_zones_clear_the_shoulder_and_temple()
{
    // A typical standing rest hand (arm 0.60 m, shoulder ~0.19 m below the eye), at the defaults: the belt zones must not
    // touch the shoulder (right) or temple (left) zone, or the overlap refusal would switch both zones off.
    const Params rbelt{ BeltCentre({ 0.24f, -0.79f, 0.02f }, 12.5f, 0.0f), 0.15f };
    const Params lbelt{ BeltCentre({ -0.24f, -0.79f, 0.02f }, 12.5f, 0.0f), 0.15f };
    CHECK(!ZonesOverlap(kP, rbelt));
    CHECK(!ZonesOverlap(kHead, lbelt));
}

// ===== The crossbow's left-shoulder take zone (plan 2026-11-03-manual-reload-crossbow Task 4, Ruling T0-b) =====
// The left-chest zone at its live values (dlvr\ovr2xr.txt :1323-1335, spec 6b).
static const Params kLChest{ { -0.12f, -0.30f, -0.12f }, 0.08f };
static Input AtTake(float squeeze, const Vec3& local, bool takeLive, bool takeZoneIn)
{
    Input in = At(squeeze, true, local);
    in.takeLive = takeLive;
    in.takeZoneIn = takeZoneIn;
    return in;
}
static void MirroredShoulderCentre()
{
    const Params m = MirrorX(kP);
    CHECK(Near(m.centre.x, -0.15f));   // x negated
    CHECK(Near(m.centre.y, kP.centre.y) && Near(m.centre.z, kP.centre.z) && Near(m.radius, kP.radius));
    CHECK(!Near(m.centre.x, kP.centre.x));   // the miss: the mirror really moved it (not the right shoulder again)
    // The mirrored centre is in the zone, the RIGHT shoulder's centre is not (0.30 m away, past the 0.18 exit).
    CHECK(TakeZoneStep(true, false, true, { -0.15f, -0.15f, 0.10f }, m));
    CHECK(!TakeZoneStep(true, false, true, kIn, m));
}
static void LeftChestHealsOutsideShoulderZone()
{
    // The rule: the hand in the chest zone heals whenever the take does not own the press.
    CHECK(LeftChestFires(true, false, false));
    CHECK(LeftChestFires(true, true, false));    // crossbow out, hand NOT in the shoulder zone -> heal as today
    CHECK(LeftChestFires(true, false, true));    // shoulder flag without the crossbow take -> heal
    CHECK(!LeftChestFires(false, true, false));  // not in the chest zone -> nothing
    // The Machine: crossbow out (take live), hand at the chest, not in the shoulder zone -> the chest press fires.
    const Vec3 atChest{ -0.12f, -0.30f, -0.12f };
    Machine chest;
    chest.Update(AtTake(0.0f, atChest, true, false), kLChest);
    chest.Update(AtTake(0.0f, atChest, true, false), kLChest);
    const Step s = chest.Update(AtTake(0.9f, atChest, true, false), kLChest);
    CHECK(s.pressHolster && !s.pressNormal && s.suppressR1 && s.ownsGrip && chest.state() == Press::Holster);
}
static void ShoulderPressNeverHeals()
{
    CHECK(!LeftChestFires(true, true, true));    // take live + shoulder zone -> the chest does not fire
    // The Machine: even with the hand inside the chest zone (the 8 mm exit overlap), a press while the take owns it is NORMAL -
    // nothing fired, L1 not withheld, no release action, the grip not owned.
    const Vec3 atChest{ -0.12f, -0.30f, -0.12f };
    Machine chest;
    chest.Update(AtTake(0.0f, atChest, true, true), kLChest);
    Step s = chest.Update(AtTake(0.0f, atChest, true, true), kLChest);
    CHECK(s.inZone);   // the control: the hand IS in the chest zone, so only the take rule can stop it
    s = chest.Update(AtTake(0.9f, atChest, true, true), kLChest);
    CHECK(s.pressNormal && !s.pressHolster && !s.suppressR1 && !s.ownsGrip && !s.yielded && chest.state() == Press::Normal);
    s = chest.Update(AtTake(0.9f, atChest, false, false), kLChest);   // the take ending mid-press does not fire it late
    CHECK(!s.pressHolster && !s.suppressR1 && chest.state() == Press::Normal);
    s = chest.Update(AtTake(0.1f, atChest, false, false), kLChest);
    CHECK(!s.releaseSwitch && !s.cancelled && chest.state() == Press::Idle);
    // The next press, take no longer live, heals as today.
    s = chest.Update(AtTake(0.9f, atChest, false, false), kLChest);
    CHECK(s.pressHolster && chest.state() == Press::Holster);
}
static void NotLiveNoShoulderZone()
{
    const Params m = MirrorX(kP);
    const Vec3 c{ -0.15f, -0.15f, 0.10f };
    CHECK(!TakeZoneStep(false, false, true, c, m));   // not live: never in, even at the centre
    CHECK(!TakeZoneStep(false, true, true, c, m));    // not live: a held "was" is dropped too
    CHECK(TakeZoneStep(true, false, true, c, m));     // the control: live at the same point -> in
    CHECK(!TakeZoneStep(true, true, false, c, m));    // no pose -> out (fails closed)
    // Hysteresis as the shoulder: 0.16 m out is past the enter radius (0.15) but inside the exit radius (0.18).
    const Vec3 edge{ -0.15f + 0.16f, -0.15f, 0.10f };
    CHECK(!TakeZoneStep(true, false, true, edge, m));
    CHECK(TakeZoneStep(true, true, true, edge, m));
    const Vec3 past{ -0.15f + 0.19f, -0.15f, 0.10f };
    CHECK(!TakeZoneStep(true, true, true, past, m));
}
// Ruling T4-a (review I1): the left TEMPLE (torch) is gated exactly like the chest - its enter sphere overlaps the mirrored shoulder.
static Step TemplePress(bool takeLive, bool takeZoneIn)
{
    const Vec3 atHead{ -0.12f, 0.00f, 0.00f };
    Machine head;
    head.Update(AtTake(0.0f, atHead, takeLive, takeZoneIn), kHead);
    head.Update(AtTake(0.0f, atHead, takeLive, takeZoneIn), kHead);
    return head.Update(AtTake(0.9f, atHead, takeLive, takeZoneIn), kHead);
}
static void TempleSilentInShoulderZoneWhileTakeLive()
{
    // The overlap is real on the disk keys: temple (-0.12, 0, 0) r 0.10 vs the mirrored shoulder (-0.15, -0.15, 0.10) r 0.15.
    const Vec3 between{ -0.135f, -0.08f, 0.05f };
    CHECK(TakeZoneStep(true, false, true, between, MirrorX(kP)));
    Machine probe;
    CHECK(probe.Update(At(0.0f, true, between), kHead).inZone);   // the control: one point inside both enter spheres
    CHECK(!LeftTempleFires(true, true, true));
    CHECK(LeftTempleFires(true, true, false) && LeftTempleFires(true, false, true) && !LeftTempleFires(false, false, false));
    // Take live + hand in the shoulder zone -> the temple does not fire (no torch tap, L1 not withheld).
    const Step s = TemplePress(true, true);
    CHECK(s.inZone && s.pressNormal && !s.pressHolster && !s.suppressR1 && !s.ownsGrip);
    // Outside the shoulder zone with the take live, and with the take not live -> the torch fires as today.
    CHECK(TemplePress(true, false).pressHolster);
    CHECK(TemplePress(false, true).pressHolster);
    CHECK(TemplePress(false, false).pressHolster);
}

// ===== The facing v2 + the hip point (2026-11-16, trace 20261116_1450_hip_anchor_facing_v2) =====
// Steps v2 for `secs`; the hands are placed by `bodyDeg` (they turn with the body, not the head) at side / drop / fore from the neck.
static void RunV2(Facing& f, double& t, float secs, float headDeg, float bodyDeg, float side, float drop, float fore,
                  float pitchDeg = 0.0f)
{
    Vec3 l, r;
    HandsAt(bodyDeg, side, drop, fore, l, r);
    for (int i = 0; i * kDt < secs; ++i, t += kDt) {
        FacingStepV2(f, kNeck, l, r, true, true, headDeg * kD2R, pitchDeg * kD2R, false, false, false, t, kDt);
    }
}

static void v2_holds_inside_the_band()
{
    Facing f; double t = 0.0;
    RunV2(f, t, 0.5f, 0.0f, 0.0f, 0.2f, -0.30f, -0.10f);
    RunV2(f, t, 10.0f, 25.0f, 0.0f, 0.2f, -0.30f, -0.10f);          // a 25 deg glance held 10 s
    CHECK(Near(f.yaw, 0.0f, 1e-4f));
}

static void v2_head_only_turn_is_not_followed_and_not_clamped()
{
    Facing f; double t = 0.0;
    RunV2(f, t, 0.5f, 0.0f, 0.0f, 0.2f, -0.30f, -0.10f);
    RunV2(f, t, 5.0f, 60.0f, 0.0f, 0.2f, -0.30f, -0.10f);           // head 60 deg, hands stay with the body
    CHECK(Near(f.yaw, 0.0f, 1e-4f));
    RunV2(f, t, 5.0f, 120.0f, 0.0f, 0.2f, -0.30f, -0.10f);          // past v1's 70 deg clamp: still held
    CHECK(Near(f.yaw, 0.0f, 1e-4f));
}

static void v2_body_turn_confirmed_by_the_hands_is_followed()
{
    Facing f; double t = 0.0;
    RunV2(f, t, 0.5f, 0.0f, 0.0f, 0.2f, -0.30f, -0.10f);
    for (int k = 1; k <= 6; ++k) { RunV2(f, t, 0.08f, 10.0f * k, 10.0f * k, 0.2f, -0.30f, -0.10f); }   // the body turns 60
    RunV2(f, t, 3.0f, 60.0f, 60.0f, 0.2f, -0.30f, -0.10f);
    CHECK(Near(f.yaw / kD2R, 60.0f, 6.0f));
}

static void v2_extended_or_looking_down_never_follows()
{
    Facing f; double t = 0.0;
    RunV2(f, t, 0.5f, 0.0f, 0.0f, 0.15f, -0.20f, -0.50f);           // hands out in front (aiming): radius > 0.35 m
    for (int k = 1; k <= 6; ++k) { RunV2(f, t, 0.08f, 10.0f * k, 10.0f * k, 0.15f, -0.20f, -0.50f); }
    RunV2(f, t, 3.0f, 60.0f, 60.0f, 0.15f, -0.20f, -0.50f);
    CHECK(Near(f.yaw, 0.0f, 1e-4f));
    Facing g; double u = 0.0;
    RunV2(g, u, 0.5f, 0.0f, 0.0f, 0.2f, -0.30f, -0.10f);
    for (int k = 1; k <= 6; ++k) { RunV2(g, u, 0.08f, 10.0f * k, 10.0f * k, 0.2f, -0.30f, -0.10f, -50.0f); }
    RunV2(g, u, 3.0f, 60.0f, 60.0f, 0.2f, -0.30f, -0.10f, -50.0f);  // looking down (pitch -50): held
    CHECK(Near(g.yaw, 0.0f, 1e-4f));
}

static void head_pitch_sign()
{
    const Q down = AxisAngle(1.0f, 0.0f, 0.0f, -40.0f);   // pitched down about +X
    CHECK(Near(HeadPitch(down.x, down.y, down.z, down.w) / kD2R, -40.0f, 0.01f));
}

static Vec3 HipRun(HipPoint& h, float secs, const Vec3& neck)
{
    Vec3 a{ 0, 0, 0 };
    for (int i = 0; i * kDt < secs; ++i) { a = HipStep(h, neck, 0.0f, kDt); }
    return a;
}

static void hip_point_upright_is_the_neck()
{
    HipPoint h;
    CHECK(NearV(HipRun(h, 1.0f, kNeck), kNeck));
}

static void hip_point_lean_holds_the_hips()
{
    HipPoint h;
    HipRun(h, 1.0f, kNeck);
    const Vec3 lean{ kNeck.x, kNeck.y - 0.09f, kNeck.z - 0.20f };   // 20 cm forward, 9 cm down (HC1's lean)
    const Vec3 a = HipRun(h, 2.0f, lean);
    CHECK(std::fabs(a.y - kNeck.y) < 0.05f);                         // the hips barely drop (the neck dropped 9 cm)
    CHECK(a.z > kNeck.z);                                            // and push back, against the neck (+Z behind)
}

static void hip_point_squat_drops_and_moves_back()
{
    HipPoint h;
    HipRun(h, 1.0f, kNeck);
    const Vec3 squat{ kNeck.x, kNeck.y - 0.60f, kNeck.z - 0.15f };
    const Vec3 a = HipRun(h, 2.0f, squat);
    CHECK(a.y < kNeck.y - 0.25f && a.y > kNeck.y - 0.45f);           // ~0.6 x the unexplained drop, not the neck's 0.6
    CHECK(a.z > kNeck.z + 0.10f);                                    // moved back
}

static void hip_point_sit_recentres_and_step_catches_up()
{
    HipPoint h;
    HipRun(h, 1.0f, kNeck);
    const Vec3 sit{ kNeck.x, kNeck.y - 0.45f, kNeck.z + 0.28f };     // down, the neck BEHIND the body axis
    CHECK(NearV(HipRun(h, 1.0f, sit), sit));
    HipPoint s;
    HipRun(s, 1.0f, kNeck);
    const Vec3 step{ kNeck.x, kNeck.y, kNeck.z - 0.60f };           // a real step: same height
    const Vec3 a = HipRun(s, 2.0f, step);
    CHECK(std::fabs(a.x - step.x) < 0.01f && std::fabs(a.y - step.y) < 0.01f && std::fabs(a.z - step.z) < 0.01f);
}

// v2.1 (trace 20261116_1545_standing_posture): a head held past the band with the hands OUT (busy) - followed only when standing.
static void v21_held_turn_followed_only_standing()
{
    for (int standing = 0; standing < 2; ++standing) {
        Facing f; double t = 0.0;
        Vec3 l, r;
        HandsAt(0.0f, 0.15f, -0.20f, -0.60f, l, r);   // hands out in front, 0.62 m: never a hands confirmation
        for (int i = 0; i * kDt < 0.5f; ++i, t += kDt) {
            FacingStepV2(f, kNeck, l, r, true, true, 0.0f, 0.0f, false, false, false, t, kDt, standing != 0);
        }
        for (int i = 0; i * kDt < 4.0f; ++i, t += kDt) {
            FacingStepV2(f, kNeck, l, r, true, true, 90.0f * kD2R, 0.0f, false, false, false, t, kDt, standing != 0);
        }
        if (standing) { CHECK(Near(f.yaw / kD2R, 90.0f, 6.0f)); } else { CHECK(Near(f.yaw, 0.0f, 1e-4f)); }
    }
}

// Direction-free zones (spec 2026-11-20-holster-direction-free-design, trace 20261120_1400): a hand on the zone's ring at ANY bearing
// round the anchor is at the zone centre; off the ring by d it is d away; near the axis the fallback yaw is used.
static void align_hand_on_ring_any_bearing()
{
    const Vec3 anchor{ 0.3f, 1.5f, -0.2f };
    const Vec3 off{ 0.20f, -0.60f, 0.10f };          // a right-back hip offset, body frame (+X right, +Z behind)
    for (int k = 0; k < 8; ++k) {
        const float yawHand = (-170.0f + 45.0f * k) * kD2R;
        const Vec3 w = ToWorldYaw(off, yawHand);
        const Vec3 palm{ anchor.x + w.x, anchor.y + w.y, anchor.z + w.z };
        const float y = AlignYawToHand(anchor, off, palm, 0.0f);
        CHECK(NearV(LocalOffsetYaw(anchor, y, palm), off));
        const Vec3 up{ palm.x, palm.y + 0.05f, palm.z };   // 5 cm above the ring: 5 cm from the centre
        const Vec3 lu = LocalOffsetYaw(anchor, AlignYawToHand(anchor, off, up, 0.0f), up);
        CHECK(Near(std::sqrt((lu.x - off.x) * (lu.x - off.x) + (lu.y - off.y) * (lu.y - off.y) + (lu.z - off.z) * (lu.z - off.z)), 0.05f, 1e-4f));
    }
}
static void align_near_axis_uses_fallback()
{
    const Vec3 anchor{ 0.0f, 1.5f, 0.0f };
    CHECK(Near(AlignYawToHand(anchor, { 0.2f, -0.6f, 0.1f }, { 0.01f, 0.9f, 0.01f }, 0.7f), 0.7f, 1e-6f));   // hand on the axis
    CHECK(Near(AlignYawToHand(anchor, { 0.01f, -0.3f, 0.01f }, { 0.3f, 1.2f, 0.0f }, -0.4f), -0.4f, 1e-6f));  // zone on the axis
}
static void shoulder_front_of_face_excluded()
{
    const Vec3 neck{ 0.0f, 1.5f, 0.0f };
    CHECK(InFrontOfFace(neck, 0.0f, { 0.0f, 1.5f, -0.3f }, kShoulderFrontR));             // straight ahead
    const Vec3 at50 = ToWorldYaw({ 0.0f, 0.0f, -0.3f }, 50.0f * kD2R), at70 = ToWorldYaw({ 0.0f, 0.0f, -0.3f }, 70.0f * kD2R);
    CHECK(InFrontOfFace(neck, 0.0f, { at50.x, 1.5f, at50.z }, kShoulderFrontR));         // 50 deg off the head: in front
    CHECK(!InFrontOfFace(neck, 0.0f, { at70.x, 1.5f, at70.z }, kShoulderFrontR));        // 70 deg: not
    CHECK(!InFrontOfFace(neck, 0.0f, { 0.25f, 1.6f, 0.05f }, kShoulderFrontR));          // over the right shoulder: not
    CHECK(!InFrontOfFace(neck, 0.0f, { 0.01f, 1.6f, 0.01f }, kShoulderFrontR));          // on the axis: no bearing, not excluded
}
// Final review I1 (2026-11-20, trace 20261120_1510): lidhand's LEFT hand tests the shoulder zone mirrored (-centre.x). The frame must
// be aligned with the MIRRORED target (holster.cpp HolsterZoneLocal mirrorX), as the poll's chain does it: VirtualHead + LocalOffsetYaw.
// Aligned with the right shoulder's own target, a left hand on the left ring reads ~2 x 0.19 m from the mirrored centre (never in).
static void align_mirrored_shoulder_for_the_left_hand()
{
    const Vec3 anchor{ 0.0f, 1.5f, 0.0f };
    Latch L; L.have = true; L.eyeFromAnchor = { 0.008f, 0.112f, -0.090f };   // Jay's learned n, roughly
    const Vec3 c{ 0.193f, -0.089f, 0.019f }, cm{ -c.x, c.y, c.z };          // the right shoulder's centre and its mirror
    const Vec3 tR{ L.eyeFromAnchor.x + c.x, L.eyeFromAnchor.y + c.y, L.eyeFromAnchor.z + c.z };
    const Vec3 tM{ L.eyeFromAnchor.x + cm.x, L.eyeFromAnchor.y + cm.y, L.eyeFromAnchor.z + cm.z };
    for (int k = 0; k < 8; ++k) {
        const float yawBody = (-170.0f + 45.0f * k) * kD2R;
        const Vec3 w = ToWorldYaw(tM, yawBody);
        const Vec3 palm{ anchor.x + w.x, anchor.y + w.y, anchor.z + w.z };   // the left hand on the left-shoulder ring
        const float yM = AlignYawToHand(anchor, tM, palm, 0.0f);
        CHECK(NearV(LocalOffsetYaw(VirtualHead(anchor, yM, L), yM, palm), cm));
        const float yR = AlignYawToHand(anchor, tR, palm, 0.0f);
        const Vec3 lr = LocalOffsetYaw(VirtualHead(anchor, yR, L), yR, palm);
        const float dx = lr.x - cm.x, dy = lr.y - cm.y, dz = lr.z - cm.z;
        CHECK(std::sqrt(dx * dx + dy * dy + dz * dz) > 0.15f);               // the bug: outside the 0.15 m radius
    }
}

// RUN HP2 (2026-11-20, trace 20261120_1325_lookdown_limit): a real turn, then a look DOWN for the holster - the look-down hold left
// the facing 100-173 deg behind. Past kLookDownLimitR (70) the hold lets go; within it a look at the belt still holds.
static void lookdown_past_limit_follows_standing()
{
    Facing f; double t = 0.0;
    Vec3 l, r;
    HandsAt(0.0f, 0.15f, -0.20f, -0.60f, l, r);   // hands out in front: never a hands confirmation
    for (int i = 0; i * kDt < 0.5f; ++i, t += kDt) {
        FacingStepV2(f, kNeck, l, r, true, true, 0.0f, 0.0f, false, false, false, t, kDt, true);
    }
    for (int i = 0; i * kDt < 4.0f; ++i, t += kDt) {     // turned 100 deg AND looking down at -50
        FacingStepV2(f, kNeck, l, r, true, true, 100.0f * kD2R, -50.0f * kD2R, false, false, false, t, kDt, true);
    }
    CHECK(Near(f.yaw / kD2R, 100.0f, 6.0f));
}
static void lookdown_within_limit_still_holds_standing()
{
    Facing f; double t = 0.0;
    Vec3 l, r;
    HandsAt(0.0f, 0.15f, -0.20f, -0.60f, l, r);
    for (int i = 0; i * kDt < 0.5f; ++i, t += kDt) {
        FacingStepV2(f, kNeck, l, r, true, true, 0.0f, 0.0f, false, false, false, t, kDt, true);
    }
    for (int i = 0; i * kDt < 4.0f; ++i, t += kDt) {     // a 60 deg look at the belt, pitched -50: the hold stays
        FacingStepV2(f, kNeck, l, r, true, true, 60.0f * kD2R, -50.0f * kD2R, false, false, false, t, kDt, true);
    }
    CHECK(Near(f.yaw, 0.0f, 1e-4f));
}
static void lookdown_past_limit_seated_needs_hands()
{
    Facing f; double t = 0.0;
    Vec3 l, r;
    HandsAt(0.0f, 0.15f, -0.20f, -0.60f, l, r);
    for (int i = 0; i * kDt < 0.5f; ++i, t += kDt) {
        FacingStepV2(f, kNeck, l, r, true, true, 0.0f, 0.0f, false, false, false, t, kDt, false);
    }
    for (int i = 0; i * kDt < 4.0f; ++i, t += kDt) {     // seated, hands not confirming: no follow, as before
        FacingStepV2(f, kNeck, l, r, true, true, 100.0f * kD2R, -50.0f * kD2R, false, false, false, t, kDt, false);
    }
    CHECK(Near(f.yaw, 0.0f, 1e-4f));
}

static void v21_posture_hysteresis()
{
    Posture p; double t = 0.0;
    CHECK(!PostureStep(p, true, 1.60f, t)); CHECK(!p.standing);           // not yet: needs 2 s above
    t = 2.1;  PostureStep(p, true, 1.60f, t); CHECK(p.standing);
    t = 2.5;  PostureStep(p, true, 1.35f, t); CHECK(p.standing);          // inside the gap: no change
    t = 3.0;  PostureStep(p, true, 1.00f, t); CHECK(p.standing);          // a crouch starts
    t = 4.0;  PostureStep(p, true, 1.55f, t); CHECK(p.standing);          // back up within 2 s: still standing
    t = 5.0;  PostureStep(p, true, 1.10f, t);
    t = 7.1;  PostureStep(p, true, 1.10f, t); CHECK(!p.standing);         // 2 s below: seated
    t = 8.0;  PostureStep(p, false, 0.0f, t); CHECK(!p.standing);         // no floor: seated
}

// Plan 2026-11-20-holster-pivot-posture-rings Task 2: the posture lines from the player's own calibration.
static void posture_lines_jay_standing()
{
    const PostureLines l = PostureLinesFrom(true, 1.67f, true);
    CHECK(Near(l.standEyeM, 1.379f, 0.005f)); CHECK(Near(l.seatEyeM, 1.329f, 0.005f)); CHECK(l.source == 1);
}
static void posture_lines_jay_seated()
{
    const PostureLines l = PostureLinesFrom(true, 1.14f, false);
    CHECK(Near(l.standEyeM, 1.379f, 0.005f)); CHECK(Near(l.seatEyeM, 1.330f, 0.005f)); CHECK(l.source == 2);
}
static void posture_lines_short_stander()
{
    const PostureLines l = PostureLinesFrom(true, 1.35f, true);
    CHECK(Near(l.standEyeM, 1.115f, 0.005f)); CHECK(Near(l.seatEyeM, 1.075f, 0.005f));
    Posture p; double t = 0.0;                                             // a 1.35 m standing eye now reads STANDING
    PostureStep(p, true, 1.35f, t, l); t = 2.1; PostureStep(p, true, 1.35f, t, l); CHECK(p.standing);
}
static void posture_lines_fallback()
{
    const PostureLines l = PostureLinesFrom(false, 0.0f, true);
    CHECK(Near(l.standEyeM, 1.38f, 1e-6f)); CHECK(Near(l.seatEyeM, 1.33f, 1e-6f)); CHECK(l.source == 0);
}
static void posture_lines_implausible()
{
    const PostureLines a = PostureLinesFrom(true, 2.5f, true);
    const PostureLines b = PostureLinesFrom(true, 0.4f, false);
    CHECK(a.source == 0 && Near(a.standEyeM, 1.38f, 1e-6f));
    CHECK(b.source == 0 && Near(b.seatEyeM, 1.33f, 1e-6f));
}

int main()
{
    align_hand_on_ring_any_bearing();
    align_near_axis_uses_fallback();
    shoulder_front_of_face_excluded();
    align_mirrored_shoulder_for_the_left_hand();
    lookdown_past_limit_follows_standing();
    lookdown_within_limit_still_holds_standing();
    lookdown_past_limit_seated_needs_hands();
    posture_lines_jay_standing();
    posture_lines_jay_seated();
    posture_lines_short_stander();
    posture_lines_fallback();
    posture_lines_implausible();
    v21_held_turn_followed_only_standing();
    v21_posture_hysteresis();
    v2_holds_inside_the_band();
    v2_head_only_turn_is_not_followed_and_not_clamped();
    v2_body_turn_confirmed_by_the_hands_is_followed();
    v2_extended_or_looking_down_never_follows();
    head_pitch_sign();
    hip_point_upright_is_the_neck();
    hip_point_lean_holds_the_hips();
    hip_point_squat_drops_and_moves_back();
    hip_point_sit_recentres_and_step_catches_up();
    belt_centre_lifts_and_moves_forward();
    belt_centre_clamps_to_jays_ranges();
    belt_forward_is_toward_the_face();
    rest_hand_plausible_selects_the_right_set();
    calibrated_belt_zones_clear_the_shoulder_and_temple();
    right_shipped_zones_are_disjoint();
    right_press_at_hip_fires_hip_only();
    right_press_at_shoulder_fires_shoulder_only();
    right_press_elsewhere_fires_nothing_keeps_r1();
    zones_overlap_uses_exit_radii();
    left_press_at_hip_fires_hip_only();
    left_press_at_head_fires_head_only();
    left_press_elsewhere_fires_nothing_keeps_l1();
    left_fire_on_grip_happens_once_per_press();
    tap_pulse_holds_for_duration_then_ends_once();
    tap_pulse_restart_while_active_extends();
    rate_gate_limits_flapping();
    facing_matches_head_yaw_convention();
    tracking_world_round_trip();
    neck_point_ignores_head_rotation();
    both_hands_in_zones_updates();
    shoulder_reach_freezes_with_left_hand_at_hip();
    latch_from_level_eye_ignores_pitch();
    drift_follows_the_head_slowly();
    quick_look_barely_moves_it();
    the_hands_no_longer_steer_it();
    limit_pulls_it_to_70_off_the_head();
    a_hand_in_a_zone_holds_it();
    a_squeezed_grip_does_not_hold_the_drift();
    standing_rest_squares_it_up();
    rest_pose_misses_seated_and_each_condition();
    moving_hands_are_not_at_rest();
    seam_short_way();
    latch_is_identity_at_latch();
    virtual_head_turns_with_body();
    identity_head_passes_offset_through();
    yaw90_rotates_offset();
    pitch_does_not_move_offset();
    yaw_and_pitch_combined();
    press_in_zone_suppresses_then_switches_once();
    normal_press_entering_zone_stays_normal();
    holster_press_leaving_zone_stays_latched();
    pose_lost_mid_holster_press_stays_latched();
    squeeze_inactive_cancels_without_switch();
    grip_down_on_first_poll_is_normal();
    hysteresis_no_chatter();
    no_pose_press_is_normal();
    yield_in_zone_is_normal();
    yield_outside_zone_changes_nothing();
    yield_after_edge_keeps_holster();
    pending_then_yield();
    pending_then_holster();
    pending_release_cancels();
    no_yield_is_today();
    MirroredShoulderCentre();
    LeftChestHealsOutsideShoulderZone();
    ShoulderPressNeverHeals();
    NotLiveNoShoulderZone();
    TempleSilentInShoulderZoneWhileTakeLive();
    std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail;
}
