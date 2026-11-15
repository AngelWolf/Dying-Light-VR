// Standalone unit test for src/src/ovr2xr/headfollow_logic.h - NOT part of the DLL build.
// docs/SPEC-BODY-FOLLOWS-HEAD-2026-10-18.md §3.2 (the fold). Exit code = number of failures.
#include "headfollow_logic.h"

#include <cmath>
#include <cstdio>

using namespace dlvr::headfollow;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// The player's four yaw fields and the camera total the engine renders: fields + addend (spec §3.2). The camera
// total must equal the head yaw on every update, whatever the switch or the look owner - that is "the view is
// unchanged".
struct Sim {
    State s;
    float fields = 0.0f;
    Step  Run(unsigned long long obj, float h, bool fold)
    {
        const Step st = Update(s, obj, h, fold);
        fields += st.addToFields;
        return st;
    }
    float Cam(const Step& st) const { return fields + st.addend; }
};

static void folding_moves_body_not_camera()
{
    Sim m;
    Step st = m.Run(1, 10.0f, true);
    CHECK(Near(st.addToFields, 10.0f));
    CHECK(Near(st.addend, 0.0f));
    CHECK(Near(m.Cam(st), 10.0f));
    st = m.Run(1, 25.0f, true);
    CHECK(Near(st.addToFields, 15.0f));
    CHECK(Near(st.addend, 0.0f));
    CHECK(Near(m.Cam(st), 25.0f));
}

static void off_is_todays_behaviour()
{
    Sim m;
    const Step st = m.Run(1, 40.0f, false);
    CHECK(Near(st.addToFields, 0.0f));
    CHECK(Near(st.addend, 40.0f));
}

static void toggle_edges_do_not_move_camera()
{
    Sim m;
    Step st = m.Run(1, 30.0f, true);
    CHECK(Near(m.Cam(st), 30.0f));
    st = m.Run(1, 30.0f, false);   // switched off: the body keeps what it absorbed
    CHECK(Near(st.addToFields, 0.0f));
    CHECK(Near(st.addend, 0.0f));
    CHECK(Near(m.Cam(st), 30.0f));
    st = m.Run(1, 50.0f, false);
    CHECK(Near(st.addend, 20.0f));
    CHECK(Near(m.Cam(st), 50.0f));
    st = m.Run(1, 50.0f, true);    // switched on again
    CHECK(Near(st.addToFields, 20.0f));
    CHECK(Near(st.addend, 0.0f));
    CHECK(Near(m.Cam(st), 50.0f));
}

static void resume_folds_outstanding_once()
{
    Sim m;
    m.Run(1, 0.0f, true);
    for (float h = 0.0f; h <= 90.0f; h += 30.0f) {   // rope / cutscene / vehicle / grab owns the look
        const Step st = m.Run(1, h, false);
        CHECK(Near(st.addToFields, 0.0f));
        CHECK(Near(m.Cam(st), h));
    }
    Step st = m.Run(1, 90.0f, true);
    CHECK(Near(st.addToFields, 90.0f));
    CHECK(Near(st.addend, 0.0f));
    st = m.Run(1, 90.0f, true);
    CHECK(Near(st.addToFields, 0.0f));
    CHECK(Near(st.addend, 0.0f));
}

static void new_object_resets_fold()
{
    Sim m;
    m.Run(1, 60.0f, true);
    CHECK(Near(m.s.folded, 60.0f));
    // A new PlayerDI after a load / death / sleep has its own fields, holding none of the old fold.
    m.fields = 0.0f;
    Step st = m.Run(2, 60.0f, true);
    CHECK(Near(st.addToFields, 60.0f));
    CHECK(Near(st.addend, 0.0f));
    st = m.Run(2, 60.0f, false);
    CHECK(Near(st.addend, 0.0f));
    CHECK(m.s.obj == 2);
}

// Same direction: equal modulo 360 (the engine renders +4460 through sin/cos; a whole turn is invisible).
static bool SameDir(float a, float b)
{
    const float d = std::fmod(std::fabs(a - b), 360.0f);
    return d < 1e-3f || d > 360.0f - 1e-3f;
}

// h is NOT accumulated: it comes from YawOnly (camhook.cpp) = atan2 -> (-180, 180], so a player facing the playspace's
// back direction crosses 179.9 -> -179.9. Each update must fold the SHORT way (+0.2), never -359.8 into the fields.
static void fold_crosses_180_the_short_way()
{
    Sim m;
    const float hs[4]  = { 170.0f, 179.0f, -179.0f, -170.0f };
    const float add[4] = { 170.0f, 9.0f, 2.0f, 9.0f };
    for (int i = 0; i < 4; ++i) {
        const Step st = m.Run(1, hs[i], true);
        CHECK(Near(st.addToFields, add[i]));
        CHECK(SameDir(m.Cam(st), hs[i]));
        CHECK(std::fabs(st.addend) < 1e-3f);
    }
    CHECK(Near(m.fields, 190.0f));   // the body turned 190 deg in total, continuously
}

// Head tremor at the back direction: alternating 179.9 / -179.9 must fold +-0.2, never +-359.8.
static void tremor_at_180_folds_small_steps()
{
    Sim m;
    m.Run(1, 179.9f, true);
    for (int i = 0; i < 6; ++i) {
        const float h = (i % 2 == 0) ? -179.9f : 179.9f;
        const Step st = m.Run(1, h, true);
        CHECK(std::fabs(st.addToFields) < 0.3f);
        CHECK(SameDir(m.Cam(st), h));
    }
}

// Paused across the crossing (rope, cutscene): the camera addend stays the short way too, and the resume fold is small.
static void paused_across_180_stays_short()
{
    Sim m;
    m.Run(1, 170.0f, true);
    Step st = m.Run(1, -170.0f, false);
    CHECK(Near(st.addend, 20.0f));
    CHECK(SameDir(m.Cam(st), -170.0f));
    st = m.Run(1, -170.0f, true);
    CHECK(Near(st.addToFields, 20.0f));
    CHECK(std::fabs(st.addend) < 1e-3f);
    CHECK(SameDir(m.Cam(st), -170.0f));
}

// ===== The arm solve's body-facing yaw - docs/SPEC-BODY-FACING-YAW-2026-10-19.md §6.1 / §6.4 =====
static bool NearR(float a, float b) { return std::fabs(a - b) < 1e-3f; }

// The verdict waits for a SETTLED fold (final review 2026-10-19): one step = one solve; hold the same values long enough.
static FacingVerdict Hold(FacingCheck& c, unsigned long long obj, float folded, float r, bool ready, int n = 25)
{
    FacingVerdict v = FacingVerdict::Waiting;
    for (int i = 0; i < n; ++i) { v = FacingCheckStep(c, obj, folded, r, ready); }
    return v;
}
static FacingVerdict HoldS(FacingCheck& c, unsigned long long obj, float folded, float r, bool ready, bool stick, int n = 25)
{
    FacingVerdict v = FacingVerdict::Waiting;
    for (int i = 0; i < n; ++i) { v = FacingCheckStep(c, obj, folded, r, ready, stick); }
    return v;
}

// Final review: mid-turn the low-passed root yaw lags the fold (~6 deg at 90 deg/s), so a verdict taken the moment the fold
// passes 30 deg can REFUSE a correct sign. It must wait until the fold has held still.
static void check_defers_while_turning()
{
    FacingCheck c;
    FacingCheckStep(c, 1, 0.0f, 0.0f, true);
    Hold(c, 1, 0.0f, 0.0f, true);   // C1: a settled baseline before the turn
    float lp = 0.0f;
    for (int i = 1; i <= 40; ++i) {                  // a 1 deg-per-solve turn; the root LP trails it
        const float folded = static_cast<float>(i);
        lp += 0.15f * (BodyFacingDeltaRad(folded) - lp);
        CHECK(FacingCheckStep(c, 1, folded, lp, true) == FacingVerdict::Waiting);
    }
    for (int i = 0; i < 40; ++i) {                   // the turn has stopped; the LP catches up
        lp += 0.15f * (BodyFacingDeltaRad(40.0f) - lp);
        FacingCheckStep(c, 1, 40.0f, lp, true);
    }
    CHECK(c.verdict == FacingVerdict::Verified);
}

static void delta_sign()
{
    CHECK(Near(BodyFacingDeltaRad(90.0f), -1.5707963f));
    CHECK(Near(BodyFacingDeltaRad(-30.0f), 0.5235988f));
    CHECK(BodyFacingDeltaRad(0.0f) == 0.0f);   // exactly: Δ = 0 keeps the solve bit-identical (spec G3)
}

// Switch turned off mid-session: folded freezes and stays applied (the body stays turned).
static void delta_frozen_fold_stays_applied()
{
    FacingCheck c;
    CHECK(c.verdict == FacingVerdict::Waiting);
    CHECK(Near(AppliedDeltaRad(c, 45.0f), -0.7853982f));
}

static void check_verified()
{
    FacingCheck c;
    CHECK(FacingCheckStep(c, 1, 0.0f, 0.30f, true) == FacingVerdict::Waiting);
    CHECK(Hold(c, 1, 0.0f, 0.30f, true) == FacingVerdict::Waiting);   // C1: a settled baseline
    CHECK(FacingCheckStep(c, 1, 20.0f, 0.30f - 0.349f, true) == FacingVerdict::Waiting);   // < 30 deg folded since latch
    CHECK(Hold(c, 1, 40.0f, 0.30f - 0.698f, true) == FacingVerdict::Verified);
    CHECK(Near(c.d, -0.6981317f));
    CHECK(NearR(c.dR, -0.698f));
}

static void check_refused_opposite_sign()
{
    FacingCheck c;
    FacingCheckStep(c, 1, 0.0f, 0.0f, true);
    Hold(c, 1, 0.0f, 0.0f, true);   // C1: the baseline is taken only once the fold and the root have settled
    CHECK(Hold(c, 1, 40.0f, 0.698f, true) == FacingVerdict::Refused);
    CHECK(Near(AppliedDeltaRad(c, 40.0f), -0.6981317f));   // 2026-10-25 (B): the verdict is log-only, the delta stays applied
}

static void check_refused_magnitude()
{
    FacingCheck c;
    FacingCheckStep(c, 1, 0.0f, 0.0f, true);
    Hold(c, 1, 0.0f, 0.0f, true);   // C1: the baseline is taken only once the fold and the root have settled
    CHECK(Hold(c, 1, 40.0f, -0.35f, true) == FacingVerdict::Refused);   // 19.9 deg off > 10
}

static void check_refused_is_sticky()
{
    FacingCheck c;
    FacingCheckStep(c, 1, 0.0f, 0.0f, true);
    Hold(c, 1, 0.0f, 0.0f, true);   // C1: the baseline is taken only once the fold and the root have settled
    CHECK(Hold(c, 1, 40.0f, 0.698f, true) == FacingVerdict::Refused);
    CHECK(Hold(c, 1, 80.0f, -1.396f, true) == FacingVerdict::Refused);
}

// ===== Clean sample (docs/superpowers/specs/2026-10-20-body-facing-check-clean-sample-design.md) =====
// Stick turn mid-window (run 1's shape): the root moves 90 deg more than the fold - must re-latch, never REFUSE.
static void check_stick_turn_relatches()
{
    FacingCheck c;
    FacingCheckStep(c, 1, 0.0f, 0.0f, true);
    CHECK(HoldS(c, 1, 60.0f, -1.047f - 1.571f, true, true) == FacingVerdict::Waiting);   // stick held the whole time
    CHECK(c.relatchStick >= 25);
    CHECK(Hold(c, 1, 60.0f, -1.047f - 1.571f, true) == FacingVerdict::Waiting);          // stick released: moved 0 since re-latch
    CHECK(Hold(c, 1, 100.0f, -1.047f - 1.571f - 0.698f, true) == FacingVerdict::Verified); // a clean 40 deg fold
}

// Run 2's numbers: fold settled at 63 deg, root still catching up (and moving) - must wait, then verify.
static void check_waits_for_root_settle()
{
    FacingCheck c;
    FacingCheckStep(c, 1, -3.0f, 0.0f, true);
    Hold(c, 1, -3.0f, 0.0f, true);   // C1: a settled baseline before the head turn
    float lp = 0.0f;
    const float target = BodyFacingDeltaRad(-66.1f - -3.0f);   // +1.101 rad
    for (int i = 0; i < 25; ++i) {                              // fold held; root crawls toward it, > 0.1 deg per step
        lp += 0.02f * (target - lp);
        CHECK(FacingCheckStep(c, 1, -66.1f, lp, true) == FacingVerdict::Waiting);
    }
    CHECK(c.heldByRoot > 0);
    for (int i = 0; i < 400 && c.verdict == FacingVerdict::Waiting; ++i) {
        lp += 0.15f * (target - lp);
        FacingCheckStep(c, 1, -66.1f, lp, true);
    }
    CHECK(c.verdict == FacingVerdict::Verified);
}

// The check stays: fold and root both settled, root opposite -> REFUSED (logged). Since 2026-10-25 (B) the delta stays applied.
static void check_clean_disagreement_still_refuses()
{
    FacingCheck c;
    FacingCheckStep(c, 1, 0.0f, 0.0f, true);
    Hold(c, 1, 0.0f, 0.0f, true);   // C1: the baseline is taken only once the fold and the root have settled
    CHECK(Hold(c, 1, 40.0f, 0.698f, true) == FacingVerdict::Refused);
    CHECK(Near(AppliedDeltaRad(c, 40.0f), -0.6981317f));
}

// A root jittering 0.2 deg per step never settles: WAITING forever, the delta stays applied.
static void check_root_never_settles_stays_waiting()
{
    FacingCheck c;
    FacingCheckStep(c, 1, 0.0f, 0.0f, true);
    for (int i = 0; i < 1000; ++i) {
        const float r = -0.698f + ((i & 1) ? 0.0035f : 0.0f);
        CHECK(FacingCheckStep(c, 1, 40.0f, r, true) == FacingVerdict::Waiting);
    }
    CHECK(Near(AppliedDeltaRad(c, 40.0f), -0.6981317f));
}

// No stick (fed as turning) -> never decides, delta applied.
static void check_stick_held_throughout_stays_waiting()
{
    FacingCheck c;
    FacingCheckStep(c, 1, 0.0f, 0.0f, true, true);
    CHECK(HoldS(c, 1, 90.0f, 0.698f, true, true, 500) == FacingVerdict::Waiting);   // would REFUSE if judged
    CHECK(Near(AppliedDeltaRad(c, 90.0f), -1.5707963f));
}

// Root settled at the seam, alternating +179.98 / -179.98 deg -> counts as steady (wrapped steps of 0.045 deg; unwrapped they
// would be 359.95 deg).
static void check_root_settles_across_seam()
{
    FacingCheck c;
    FacingCheckStep(c, 1, 0.0f, 3.1412f, true);                 // r0 just under +pi
    for (int i = 0; i < 25; ++i) {
        const float r = (i & 1) ? -3.1412f : 3.1412f;            // 0.045 deg apart across the seam
        FacingCheckStep(c, 1, 30.5f, r, true);
    }
    CHECK(c.rootSteady >= 20);
}

// Final review C1: the stick is released while the root is still turning (EMA lag + INV-20's root lag). The baseline must wait
// for the root to settle, or the drift after release lands in dR and a correct head fold is REFUSED.
static void check_baseline_waits_for_root_after_stick()
{
    FacingCheck c;
    Hold(c, 1, 0.0f, 0.0f, true);                                // a settled start
    HoldS(c, 1, 0.0f, 0.0f, true, true, 5);                      // stick turning; released with the root at 0 ...
    float lp = 0.0f;
    for (int i = 0; i < 60; ++i) {                               // ... but the root keeps turning the stick's way to -0.5 rad
        lp += 0.15f * (-0.5f - lp);
        FacingCheckStep(c, 1, 0.0f, lp, true);
    }
    const float target = -0.5f + BodyFacingDeltaRad(40.0f);    // then a clean 40 deg head fold, root following it exactly
    for (int i = 0; i < 200 && c.verdict == FacingVerdict::Waiting; ++i) {
        lp += 0.15f * (target - lp);
        FacingCheckStep(c, 1, 40.0f, lp, true);
    }
    CHECK(c.verdict == FacingVerdict::Verified);
}

// A new player object resets the counters with the check.
static void check_new_object_resets_counters()
{
    FacingCheck c;
    FacingCheckStep(c, 1, 0.0f, 0.0f, true);
    HoldS(c, 1, 10.0f, 0.0f, true, true);
    CHECK(c.relatchStick > 0);
    FacingCheckStep(c, 2, 0.0f, 0.0f, true);
    CHECK(c.relatchStick == 0 && c.heldByRoot == 0 && c.rootSteady == 0);
}

static void check_waits_without_root()
{
    FacingCheck c;
    CHECK(FacingCheckStep(c, 1, 0.0f, 99.0f, false) == FacingVerdict::Waiting);
    CHECK(FacingCheckStep(c, 1, 60.0f, 99.0f, false) == FacingVerdict::Waiting);
    CHECK(!c.latched);
    CHECK(FacingCheckStep(c, 1, 60.0f, 1.0f, true) == FacingVerdict::Waiting);
    CHECK(c.latched);
    CHECK(Near(c.folded0, 60.0f));
    CHECK(Near(c.r0, 1.0f));
}

// A new player object (death / respawn / level change): re-latch, never compare across bodies.
static void check_reset_relatches()
{
    FacingCheck c;
    FacingCheckStep(c, 1, 0.0f, 0.0f, true);
    Hold(c, 1, 0.0f, 0.0f, true);   // C1: the baseline is taken only once the fold and the root have settled
    CHECK(Hold(c, 1, 40.0f, -0.698f, true) == FacingVerdict::Verified);
    CHECK(FacingCheckStep(c, 2, 0.0f, 5.0f, true) == FacingVerdict::Waiting);
    CHECK(c.latched);
    CHECK(Near(c.folded0, 0.0f));
    CHECK(Near(c.r0, 5.0f));
}

// folded wraps the short way at the playspace's back; the root yaw wraps at +-pi. Neither may read as a 360 step.
static void check_wrap_across_180()
{
    FacingCheck c;
    FacingCheckStep(c, 1, 170.0f, 3.0f, true);
    Hold(c, 1, 170.0f, 3.0f, true);   // C1: a settled baseline
    float r = 3.0f - 0.5236f + 6.2831853f;   // the root yaw a turn of +30 deg later, as an unwrapped value
    while (r > 3.14159265f) { r -= 6.2831853f; }
    CHECK(Hold(c, 1, -160.0f, r, true) == FacingVerdict::Verified);
    CHECK(NearR(c.d, -0.5236f));
    CHECK(NearR(c.dR, -0.5236f));
}

// A turn near 180 deg puts d and dR either side of +-pi: fold +178 with the root 4 deg past it reads dR = +178 deg against
// d = -178 deg - the same direction, 4 deg apart. The comparison must be wrapped, or this REFUSES a correct reading.
static void check_turn_near_180_compares_wrapped()
{
    const float folds[2] = { 178.0f, -178.0f };
    const float errs[2]  = { -4.0f, 4.0f };
    for (int k = 0; k < 2; ++k) {
        FacingCheck c;
        Hold(c, 1, 0.0f, 0.0f, true, 30);   // a settled baseline
        const float r = WrapPi(BodyFacingDeltaRad(folds[k]) + errs[k] * 0.01745329252f);
        CHECK(Hold(c, 1, folds[k], r, true, 30) == FacingVerdict::Verified);
    }
    // The safety still holds at the seam: a root 20 deg off a 178 deg fold refuses.
    FacingCheck c;
    Hold(c, 1, 0.0f, 0.0f, true, 30);
    CHECK(Hold(c, 1, 178.0f, WrapPi(BodyFacingDeltaRad(178.0f) - 20.0f * 0.01745329252f), true, 30) == FacingVerdict::Refused);
}

// ===== The reported head POSITION under the fold - docs/SPEC-HEAD-POSITION-FOLD-2026-10-19.md §3 =====
static void pos_zero_is_identity()
{
    float x = 0.3f, z = -0.2f;
    ReportedHeadPosForFold(0.0f, x, z);
    CHECK(x == 0.3f);   // exactly: folded 0 -> the report is bit-identical (spec G2)
    CHECK(z == -0.2f);
}

// OpenXR +yaw about +Y turns forward (0, 0, -1) to the left (-1, 0, 0).
static void pos_plus90_forward_to_left()
{
    float x = 0.0f, z = -1.0f;
    ReportedHeadPosForFold(90.0f, x, z);
    CHECK(Near(x, -1.0f));
    CHECK(Near(z, 0.0f));
}

static void pos_half_turn()
{
    float x = 0.3f, z = -0.2f;
    ReportedHeadPosForFold(180.0f, x, z);
    CHECK(Near(x, -0.3f));
    CHECK(Near(z, 0.2f));
}

static void pos_inverse()
{
    float x = 0.3f, z = -0.2f;
    ReportedHeadPosForFold(37.0f, x, z);
    ReportedHeadPosForFold(-37.0f, x, z);
    CHECK(Near(x, 0.3f));
    CHECK(Near(z, -0.2f));
}

// ===== TORSO FROM CONTROLLERS - docs/superpowers/specs/2026-10-29-torso-from-controllers-design.md §4.2-4.4 =====

static bool NearDeg(float a, float b) { return std::fabs(a - b) < 0.01f; }

// Both grips at the same point, `dist` metres from the neck (at the origin) along OpenXR yaw `yawDeg` (h's convention).
static TorsoIn HandsAt(float yawDeg, float dist, float headDeg, float dt = 1.0f)
{
    const float r = yawDeg * 0.01745329252f;
    const float x = -dist * std::sin(r), z = -dist * std::cos(r);
    TorsoIn in{};
    in.lX = in.rX = x;
    in.lZ = in.rZ = z;
    in.haveL = in.haveR = true;
    in.headYawXrDeg = headDeg;
    in.dt = dt;
    return in;   // neck at the origin, eyes 10 cm above it, both hands at chest height (30 cm below the neck)
}

// One hand at OpenXR yaw `yawDeg`, `dist` metres out at chest height.
static void HandAt(TorsoIn& in, bool right, float yawDeg, float dist, float y = -0.30f)
{
    const float r = yawDeg * 0.01745329252f;
    (right ? in.rX : in.lX) = -dist * std::sin(r);
    (right ? in.rZ : in.lZ) = -dist * std::cos(r);
    (right ? in.rY : in.lY) = y;
}

// Converge the filter: steps far longer than its time constant.
static TorsoOut Settle(TorsoState& s, unsigned long long obj, const TorsoIn& in)
{
    TorsoOut o{};
    for (int i = 0; i < 20; ++i) { o = TorsoGapStep(s, obj, in); }
    return o;
}

static void torso_yaw_convention()
{
    CHECK(NearDeg(TorsoYawXrDeg(0.0f, -1.0f), 0.0f));
    CHECK(NearDeg(TorsoYawXrDeg(-1.0f, 0.0f), 90.0f));
    CHECK(NearDeg(TorsoYawXrDeg(1.0f, 0.0f), -90.0f));
}

static void gap_zero_hands_in_front()
{
    TorsoState s;
    const TorsoOut o = TorsoGapStep(s, 1, HandsAt(0.0f, 0.4f, 0.0f));
    CHECK(NearDeg(o.gapXrDeg, 0.0f));
    CHECK(Near(o.weight, 1.0f));
}

static void gap_follows_hands_left()
{
    TorsoState s;
    const TorsoOut o = TorsoGapStep(s, 1, HandsAt(30.0f, 0.4f, 0.0f));
    CHECK(NearDeg(o.gapXrDeg, 30.0f));
}

static void head_turn_alone_keeps_body()
{
    TorsoState s;
    Settle(s, 1, HandsAt(0.0f, 0.4f, 0.0f));
    const TorsoOut o = Settle(s, 1, HandsAt(0.0f, 0.4f, 20.0f));
    CHECK(NearDeg(o.gapXrDeg, -20.0f));
    const float look0 = 37.0f;   // the field-convention facing does not move when only the head turned (flip off)
    CHECK(NearDeg(FacingTarget(look0 + 20.0f, o.gapXrDeg, look0), look0));
}

static void clamp_at_max_gap()
{
    TorsoState s;
    const TorsoOut o = TorsoGapStep(s, 1, HandsAt(70.0f, 0.4f, 0.0f));
    CHECK(NearDeg(o.gapXrDeg, 40.0f));
    CHECK(o.clamped);
}

static void wrap_across_180()
{
    TorsoState s;
    const TorsoOut o = TorsoGapStep(s, 1, HandsAt(-170.0f, 0.4f, 170.0f));
    CHECK(NearDeg(o.gapXrDeg, 20.0f));
}

static void hold_when_hands_close()
{
    TorsoState s;
    Settle(s, 1, HandsAt(30.0f, 0.4f, 0.0f));
    const TorsoOut o1 = TorsoGapStep(s, 1, HandsAt(-30.0f, 0.05f, 0.0f));
    CHECK(o1.gapXrDeg == 30.0f || NearDeg(o1.gapXrDeg, 30.0f));
    CHECK(o1.held);
    const TorsoOut o2 = Settle(s, 1, HandsAt(-30.0f, 0.15f, 0.0f));   // weight 0.5: half way from the held +30 to -30
    CHECK(NearDeg(o2.gapXrDeg, 0.0f));
}

static void missing_hand_holds()
{
    TorsoState s;
    Settle(s, 1, HandsAt(30.0f, 0.4f, 0.0f));
    TorsoIn in = HandsAt(-30.0f, 0.4f, 0.0f);
    in.haveR = false;   // Run T1 rule (Jay: "Ignore a hand at the head" - one hand counts -> it decides): the left hand alone
    const TorsoOut o = Settle(s, 1, in);
    TorsoState ref;     // the same input with the right hand never there at all
    const TorsoOut r = Settle(ref, 1, in);
    CHECK(NearDeg(o.gapXrDeg, r.gapXrDeg));
    CHECK(o.hands == 1);
    in.haveL = false;   // both lost: hold
    const TorsoOut o2 = TorsoGapStep(s, 1, in);
    CHECK(NearDeg(o2.gapXrDeg, o.gapXrDeg));
    CHECK(Near(o2.weight, 0.0f));
}

static void filter_time_constant()
{
    TorsoState s;
    Settle(s, 1, HandsAt(0.0f, 0.4f, 0.0f));
    const TorsoOut o = TorsoGapStep(s, 1, HandsAt(20.0f, 0.4f, 0.0f, 0.08f));
    CHECK(NearDeg(o.gapXrDeg, 20.0f * (1.0f - std::exp(-1.0f))));
}

static void jump_bypasses_filter()
{
    TorsoState s;
    Settle(s, 1, HandsAt(-30.0f, 0.4f, 0.0f));
    const TorsoOut o = TorsoGapStep(s, 1, HandsAt(40.0f, 0.4f, 0.0f, 0.011f));   // a 70 deg change > 60: no filter
    CHECK(NearDeg(o.gapXrDeg, 40.0f));
}

static void new_object_resets()
{
    TorsoState s;
    Settle(s, 1, HandsAt(30.0f, 0.4f, 0.0f));
    const TorsoOut o = TorsoGapStep(s, 2, HandsAt(0.0f, 0.4f, 0.0f, 0.011f));
    CHECK(NearDeg(o.gapXrDeg, 0.0f));
}

static void nonfinite_input_holds()
{
    TorsoState s;
    Settle(s, 1, HandsAt(10.0f, 0.4f, 0.0f));
    TorsoIn in = HandsAt(-30.0f, 0.4f, 0.0f);
    in.lX = std::nanf("");   // a NaN hand does not count; the right hand alone decides
    const TorsoOut o = Settle(s, 1, in);
    CHECK(std::isfinite(o.gapXrDeg));
    TorsoIn only = in;       // the same input with the left hand simply absent
    only.haveL = false;
    TorsoState ref;
    CHECK(NearDeg(o.gapXrDeg, Settle(ref, 1, only).gapXrDeg));
    CHECK(o.hands == 2);
    in.rX = std::nanf("");   // both NaN: hold, finite
    const TorsoOut o2 = TorsoGapStep(s, 1, in);
    CHECK(std::isfinite(o2.gapXrDeg));
    CHECK(NearDeg(o2.gapXrDeg, o.gapXrDeg));
}

static void facing_target_continuous()
{
    CHECK(NearDeg(FacingTarget(10.0f, 5.0f, 725.0f), 735.0f));
    CHECK(NearDeg(FacingTarget(-170.0f, -15.0f, 170.0f), 175.0f));   // -185 == 175: +5 the short way, never -355
}

static void stick_rotation_sign()
{
    float x = 0.0f, y = 1.0f;
    StickForGap(30.0f, x, y);   // body 30 deg LEFT of the look: the stick turns right to walk along the look
    CHECK(Near(x, 0.5f));
    CHECK(Near(y, 0.8660254f));
    float x0 = 0.3f, y0 = -0.7f;
    StickForGap(0.0f, x0, y0);
    CHECK(x0 == 0.3f && y0 == -0.7f);
}

// Final review C1: a head-only turn at frame rate must not move the body even for a moment (the filter is on the torso's
// heading, so the gap follows -h instantly).
static void head_turn_transient_keeps_body()
{
    TorsoState s;
    Settle(s, 1, HandsAt(0.0f, 0.4f, 0.0f));
    for (int i = 1; i <= 10; ++i) {
        const float h = 2.0f * static_cast<float>(i);
        const TorsoOut o = TorsoGapStep(s, 1, HandsAt(0.0f, 0.4f, h, 0.011f));
        CHECK(NearDeg(o.gapXrDeg, -h));
    }
}

// Final review C2: hands close in (no reading) - the torso HOLDS its heading while the head turns; past the clamp the head
// pulls it round, and it stays where it was pulled.
static void hands_close_head_turn_keeps_body()
{
    TorsoState s;
    Settle(s, 1, HandsAt(0.0f, 0.4f, 0.0f));
    CHECK(NearDeg(TorsoGapStep(s, 1, HandsAt(0.0f, 0.05f, 30.0f, 0.011f)).gapXrDeg, -30.0f));
    const TorsoOut pulled = TorsoGapStep(s, 1, HandsAt(0.0f, 0.05f, 50.0f, 0.011f));
    CHECK(NearDeg(pulled.gapXrDeg, -40.0f));
    CHECK(pulled.clamped);
    CHECK(NearDeg(TorsoGapStep(s, 1, HandsAt(0.0f, 0.05f, 0.0f, 0.011f)).gapXrDeg, 10.0f));   // heading 50 - 40 = 10
}

// Final review I5: a hand midpoint BEHIND the neck (a hand at a back holster) is no reading - no 80 deg flip.
static void hand_behind_holds()
{
    TorsoState s;
    Settle(s, 1, HandsAt(0.0f, 0.4f, 0.0f));
    const TorsoOut o = TorsoGapStep(s, 1, HandsAt(170.0f, 0.4f, 0.0f, 0.011f));
    CHECK(NearDeg(o.gapXrDeg, 0.0f));
    CHECK(Near(o.weight, 0.0f));
    CHECK(NearDeg(TorsoGapStep(s, 1, HandsAt(-170.0f, 0.4f, 0.0f, 0.011f)).gapXrDeg, 0.0f));
}

// Run T1 (Jay: "Ignore a hand at the head"): a gun at the eye puts that grip next to the neck, so the midpoint followed the OTHER
// hand and the torso sat on the 40 deg limit. A hand within 25 cm of the eyes does not count; the other hand alone decides.
static void hand_at_head_ignored()
{
    TorsoState s;
    TorsoIn in = HandsAt(0.0f, 0.4f, 0.0f);
    HandAt(in, false, 20.0f, 0.4f);              // left hand forward-left at the chest
    in.rX = 0.03f; in.rY = 0.08f; in.rZ = -0.06f;   // right hand at the eyes (a gun at the eye)
    const TorsoOut o = TorsoGapStep(s, 1, in);
    TorsoIn only = in;                              // the same input with the right hand absent
    only.haveR = false;
    TorsoState ref;
    CHECK(NearDeg(o.gapXrDeg, TorsoGapStep(ref, 1, only).gapXrDeg));
    CHECK(o.hands == 1);
    CHECK(Near(o.weight, 1.0f));
}

// A hand hanging at the side (more than 40 cm below the neck, within 25 cm of it horizontally) does not count either.
static void hand_hanging_ignored()
{
    TorsoState s;
    TorsoIn in = HandsAt(0.0f, 0.4f, 0.0f);
    HandAt(in, false, 90.0f, 0.15f, -0.55f);   // left hand hanging at the left side
    HandAt(in, true, -10.0f, 0.4f);           // right hand forward
    const TorsoOut o = TorsoGapStep(s, 1, in);
    TorsoIn only = in;                        // the same input with the left hand absent
    only.haveL = false;
    TorsoState ref;
    CHECK(NearDeg(o.gapXrDeg, TorsoGapStep(ref, 1, only).gapXrDeg));
    CHECK(o.hands == 2);
}

// Neither hand counts: the torso HOLDS its heading (the head still pulls it past 40).
static void no_usable_hand_holds()
{
    TorsoState s;
    Settle(s, 1, HandsAt(10.0f, 0.4f, 0.0f));
    TorsoIn in = HandsAt(0.0f, 0.4f, 0.0f, 0.011f);
    in.rX = 0.03f; in.rY = 0.08f; in.rZ = -0.06f;   // gun at the eye
    HandAt(in, false, 90.0f, 0.15f, -0.55f);        // other hand hanging
    const TorsoOut o = TorsoGapStep(s, 1, in);
    CHECK(NearDeg(o.gapXrDeg, 10.0f));
    CHECK(o.held);
}

// The T1 case end to end: gun at the eye, the other hand forward - a small glance leaves the body still (gap moves by -h).
static void gun_at_eye_glance_keeps_body()
{
    TorsoState s;
    TorsoIn in = HandsAt(0.0f, 0.4f, 0.0f);
    in.rX = 0.03f; in.rY = 0.08f; in.rZ = -0.06f;
    in.lX = -kTorsoShoulderHalfM;   // the support hand in front of its own shoulder (run T2's pose), not the neck
    Settle(s, 1, in);
    for (int i = 1; i <= 8; ++i) {
        TorsoIn g = in;
        g.headYawXrDeg = 2.0f * static_cast<float>(i);
        g.dt = 0.011f;
        const TorsoOut o = TorsoGapStep(s, 1, g);
        CHECK(NearDeg(o.gapXrDeg, -g.headYawXrDeg));
        CHECK(!o.clamped);
    }
}

// Run T2 (Jay: "Yes. Leave the limit at 40 for now"): a single counted hand sits in front of its own SHOULDER, not the neck - read
// from the neck it pulled the torso ~20 deg to its side (T2 22:35:53-22:36:13 pinned at +40). Measured from the shoulder
// (neck -+ kTorsoShoulderHalfM along the torso's right), a hand straight ahead of its shoulder reads straight ahead.
static void single_hand_from_its_shoulder()
{
    TorsoState s;
    TorsoIn in = HandsAt(0.0f, 0.4f, 0.0f);
    in.rX = 0.03f; in.rY = 0.08f; in.rZ = -0.06f;      // right hand at the eyes: dropped
    in.lX = -kTorsoShoulderHalfM; in.lZ = -0.45f;       // left hand straight ahead of the left shoulder
    const TorsoOut o = TorsoGapStep(s, 1, in);
    CHECK(NearDeg(o.gapXrDeg, 0.0f));
    CHECK(o.hands == 1);
    TorsoState s2;
    TorsoIn in2 = HandsAt(0.0f, 0.4f, 0.0f);
    in2.lX = 0.0f; in2.lY = 0.08f; in2.lZ = -0.05f;    // left hand at the face: dropped
    in2.rX = kTorsoShoulderHalfM; in2.rZ = -0.45f;      // right hand straight ahead of the right shoulder
    const TorsoOut o2 = TorsoGapStep(s2, 1, in2);
    CHECK(NearDeg(o2.gapXrDeg, 0.0f));
    CHECK(o2.hands == 2);
}

// The shoulder offset follows the torso's own heading: torso turned 90 deg left, the left hand straight ahead of the left shoulder
// still reads the torso's heading.
static void shoulder_offset_turns_with_torso()
{
    TorsoState s;
    TorsoIn in = HandsAt(90.0f, 0.45f, 90.0f);
    Settle(s, 1, in);                                    // torso and head at +90 (facing -x)
    in.rX = -0.03f; in.rY = 0.08f; in.rZ = 0.0f;         // right hand at the eyes
    // facing -x: right axis = -z, so the left shoulder is at +z; straight ahead of it = (-0.45, +0.18)
    in.lX = -0.45f; in.lZ = kTorsoShoulderHalfM;
    const TorsoOut o = Settle(s, 1, in);
    CHECK(NearDeg(o.gapXrDeg, 0.0f));
}

// The 5 s line's counters (Jay: split the causes exactly): how far the 40 deg limit pulled the torso this step, and which hands counted.
static void clamp_pull_is_reported()
{
    TorsoState s;
    Settle(s, 1, HandsAt(0.0f, 0.4f, 0.0f));
    const TorsoOut a = TorsoGapStep(s, 1, HandsAt(0.0f, 0.05f, 30.0f, 0.011f));   // hands close: hold, 30 < 40, no pull
    CHECK(Near(a.pulledDeg, 0.0f));
    CHECK(a.hands == 0);
    const TorsoOut b = TorsoGapStep(s, 1, HandsAt(0.0f, 0.05f, 50.0f, 0.011f));   // 50 > 40: pulled 10 toward the head
    CHECK(NearDeg(b.pulledDeg, 10.0f));
    const TorsoOut c = TorsoGapStep(s, 1, HandsAt(0.0f, 0.4f, 0.0f, 0.011f));
    CHECK(c.hands == 3);
}

static void stick_gap_refused_when_stale()
{
    CHECK(StickGapFresh(1000, 950, true));
    CHECK(!StickGapFresh(1000, 900, true));
    CHECK(!StickGapFresh(1000, 990, false));
    CHECK(!StickGapFresh(1000, 0, true));
}

int main()
{
    torso_yaw_convention();
    gap_zero_hands_in_front();
    gap_follows_hands_left();
    head_turn_alone_keeps_body();
    clamp_at_max_gap();
    wrap_across_180();
    hold_when_hands_close();
    missing_hand_holds();
    filter_time_constant();
    jump_bypasses_filter();
    new_object_resets();
    nonfinite_input_holds();
    facing_target_continuous();
    stick_rotation_sign();
    stick_gap_refused_when_stale();
    head_turn_transient_keeps_body();
    hands_close_head_turn_keeps_body();
    hand_behind_holds();
    hand_at_head_ignored();
    hand_hanging_ignored();
    no_usable_hand_holds();
    gun_at_eye_glance_keeps_body();
    single_hand_from_its_shoulder();
    shoulder_offset_turns_with_torso();
    clamp_pull_is_reported();
    pos_zero_is_identity();
    pos_plus90_forward_to_left();
    pos_half_turn();
    pos_inverse();
    delta_sign();
    delta_frozen_fold_stays_applied();
    check_verified();
    check_refused_opposite_sign();
    check_refused_magnitude();
    check_refused_is_sticky();
    check_stick_turn_relatches();
    check_waits_for_root_settle();
    check_clean_disagreement_still_refuses();
    check_root_never_settles_stays_waiting();
    check_stick_held_throughout_stays_waiting();
    check_root_settles_across_seam();
    check_new_object_resets_counters();
    check_baseline_waits_for_root_after_stick();
    check_waits_without_root();
    check_reset_relatches();
    check_wrap_across_180();
    check_turn_near_180_compares_wrapped();
    check_defers_while_turning();
    folding_moves_body_not_camera();
    off_is_todays_behaviour();
    toggle_edges_do_not_move_camera();
    resume_folds_outstanding_once();
    new_object_resets_fold();
    fold_crosses_180_the_short_way();
    tremor_at_180_folds_small_steps();
    paused_across_180_stays_short();
    std::printf("%s (%d failure(s))\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail;
}
