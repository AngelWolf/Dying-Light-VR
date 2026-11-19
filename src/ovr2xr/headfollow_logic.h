// Pure logic for "body follows head" - docs/SPEC-BODY-FOLLOWS-HEAD-2026-10-18.md §3.2 (the fold).
// No OpenXR, no Windows, no logging, so src/src/tests/headfollow_logic_test.cpp compiles it on its own.
// boneblend.cpp (camyaw::Hook) owns everything with side effects: it adds addToFields to the player's four yaw
// fields before the engine's chase (snap's writer) and puts addend into +4460 after it.
#pragma once

#include <cmath>

namespace dlvr {
namespace headfollow {

// folded = degrees of head yaw already moved into the four fields of player object obj.
struct State {
    float              folded = 0.0f;
    unsigned long long obj    = 0;
};

struct Step {
    float addToFields;   // add to +4460 / +4468 / +4588 / +4592 before the chase (0 = write nothing)
    float addend;        // the camera-only head yaw camyaw adds to +4460 after the chase
};

// Into (-180, 180] - the range h itself comes in (YawOnly in camhook.cpp is atan2 with w = cos >= 0).
inline float Wrap180(float d)
{
    d = std::fmod(d, 360.0f);
    if (d > 180.0f)   { d -= 360.0f; }
    if (d <= -180.0f) { d += 360.0f; }
    return d;
}

// h = this update's head yaw (degrees, flip already applied); fold = the switch is on AND LookController owns the look.
// h is NOT accumulated - it wraps at +-180 when the player faces the playspace's back direction - so every difference is
// taken the SHORT way: the fields (which the engine accumulates unwrapped) move continuously, never by +-360 in one
// update. The camera total (fields + addend) therefore points where h points (equal modulo 360) on every path, so the
// view never moves because of this; only the body does. Switch off with nothing folded: addend == h exactly, as before.
inline Step Update(State& s, unsigned long long obj, float h, bool fold)
{
    if (obj != s.obj) {   // a new PlayerDI (load, death, sleep) holds none of the old fold
        s.obj = obj;
        s.folded = 0.0f;
    }
    Step st{ 0.0f, 0.0f };
    if (fold) {
        st.addToFields = Wrap180(h - s.folded);
        s.folded = h;
    }
    st.addend = Wrap180(h - s.folded);
    return st;
}

// ===== The arm solve's BODY-FACING yaw - docs/SPEC-BODY-FACING-YAW-2026-10-19.md =====
// The solve's own yaw (bodyYaw) is the OpenXR->game MAPPING yaw; the body the engine turned sits `folded` degrees away from
// it. §6.1: +d degrees in the player's yaw fields is -d * pi/180 in the solve's convention (a READING - §6.4 checks it).

// Into (-pi, pi].
inline float WrapPi(float r)
{
    r = std::fmod(r, 6.28318531f);
    if (r > 3.14159265f)   { r -= 6.28318531f; }
    if (r <= -3.14159265f) { r += 6.28318531f; }
    return r;
}

inline float BodyFacingDeltaRad(float foldedDeg)
{
    return -foldedDeg * 0.01745329252f;
}

enum class FacingVerdict : int { Waiting = 0, Verified = 1, Refused = 2 };

// The self-check against the solve's root-derived yaw (an independent source that turns with the engine body).
// dR / d = the radians the verdict was decided on.
struct FacingCheck {
    bool               latched = false;
    float              folded0 = 0.0f, r0 = 0.0f;
    FacingVerdict      verdict = FacingVerdict::Waiting;
    float              dR = 0.0f, d = 0.0f;
    unsigned long long obj = 0;
    float              lastFolded = 0.0f;   // the previous step's folded, for the settled test
    int                steady = 0;          // consecutive steps the fold held within kFacingCheckSteadyDeg
    // Clean sample (2026-10-20, docs/superpowers/specs/2026-10-20-body-facing-check-clean-sample-design.md):
    float              lastLpRoot = 0.0f;   // the previous step's low-passed root yaw, for the root-settled test
    int                rootSteady = 0;      // consecutive steps the root held within kFacingCheckRootSteadyRad
    long               relatchStick = 0;    // steps that re-latched because the right stick was turning (this player object)
    long               heldByRoot = 0;      // steps a verdict was held back because the root had not settled (this player object)
    bool               armed = false;       // the baseline (folded0, r0) was taken with the fold AND the root settled (final review C1)
};

constexpr float kFacingCheckMinFoldDeg = 30.0f;          // spec §6.4: a verdict needs this much folded since the latch
constexpr float kFacingCheckTolRad     = 0.17453293f;    // 10 deg
// Final review 2026-10-19: mid-turn the low-passed root yaw trails the fold (~6 deg at 90 deg/s with the 0.15 / solve EMA),
// plus a possible one-update order lag (hy37) - deciding then REFUSES a correct sign. Decide only once the fold has held
// still: 20 steps (solves) within 0.1 deg each leaves the EMA within 0.85^20 = 3.9 % of the turn.
constexpr float kFacingCheckSteadyDeg   = 0.1f;
constexpr int   kFacingCheckSteadySteps = 20;
// Clean sample (2026-10-20): decide only on a CLEAN sample. The root also moves for things the fold does not contain - its own
// lag behind the camera (INV-20) and smooth stick turns - and a verdict taken on such a sample REFUSED a correct delta (6 of 14
// archived verdicts), pinning the elbow axis to the playspace start. So a stick turn re-latches, and the verdict also waits for
// the ROOT to hold within 0.1 deg per step for kFacingCheckSteadySteps. The delta stays applied while waiting.
constexpr float kFacingCheckRootSteadyRad = 0.0017453293f;   // 0.1 deg per step

// obj = the player object `folded` belongs to; lpRootYawRad = the low-passed root-derived yaw; rootReady = that yaw exists
// (the solve's K is calibrated and the root read succeeded); stickTurning = the right stick is turning the character (or no
// stick could be read). Changes are compared, never absolutes.
inline FacingVerdict FacingCheckStep(FacingCheck& c, unsigned long long obj, float foldedDeg, float lpRootYawRad, bool rootReady,
                                     bool stickTurning = false)
{
    if (obj != c.obj) {
        c = FacingCheck{};
        c.obj = obj;
    }
    if (c.verdict != FacingVerdict::Waiting || !rootReady) { return c.verdict; }
    if (!c.latched) {
        c.latched    = true;
        c.folded0    = foldedDeg;
        c.r0         = lpRootYawRad;
        c.lastFolded = foldedDeg;
        c.steady     = 0;
        c.lastLpRoot = lpRootYawRad;
        c.rootSteady = 0;
        c.armed      = false;
        return c.verdict;
    }
    if (stickTurning) {   // the stick turns the root, not the fold: start the window again from here
        c.folded0    = foldedDeg;
        c.r0         = lpRootYawRad;
        c.lastFolded = foldedDeg;
        c.lastLpRoot = lpRootYawRad;
        c.steady     = 0;
        c.rootSteady = 0;
        c.armed      = false;
        ++c.relatchStick;
        return c.verdict;
    }
    c.steady     = (std::fabs(Wrap180(foldedDeg - c.lastFolded)) <= kFacingCheckSteadyDeg) ? c.steady + 1 : 0;
    c.lastFolded = foldedDeg;
    c.rootSteady = (std::fabs(WrapPi(lpRootYawRad - c.lastLpRoot)) <= kFacingCheckRootSteadyRad) ? c.rootSteady + 1 : 0;
    c.lastLpRoot = lpRootYawRad;
    // Final review C1: the baseline itself must be clean. Until the fold AND the root have both held for kFacingCheckSteadySteps,
    // keep re-seating it here - a root still drifting from a released stick (or from the first latch) would otherwise land in dR.
    if (!c.armed) {
        c.folded0 = foldedDeg;
        c.r0      = lpRootYawRad;
        if (c.steady >= kFacingCheckSteadySteps && c.rootSteady >= kFacingCheckSteadySteps) { c.armed = true; }
        return c.verdict;
    }
    const float moved = Wrap180(foldedDeg - c.folded0);
    if (std::fabs(moved) < kFacingCheckMinFoldDeg || c.steady < kFacingCheckSteadySteps) { return c.verdict; }
    if (c.rootSteady < kFacingCheckSteadySteps) {
        ++c.heldByRoot;
        return c.verdict;
    }
    c.d  = BodyFacingDeltaRad(moved);
    c.dR = WrapPi(lpRootYawRad - c.r0);
    // Wrapped (2026-10-20): a turn near 180 deg puts d and dR either side of +-pi, where a raw sign test and difference REFUSE
    // a correct reading. |moved| >= 30 against a 10 deg tolerance, so an opposite-direction root still refuses.
    c.verdict = (std::fabs(WrapPi(c.dR - c.d)) <= kFacingCheckTolRad) ? FacingVerdict::Verified : FacingVerdict::Refused;
    return c.verdict;
}

// ===== The reported head POSITION under the fold - docs/SPEC-HEAD-POSITION-FOLD-2026-10-19.md §3 =====
// gamedll +C08100 rotates the head position we report by the BODY frame (INV-306), which the fold turned by an OpenXR yaw of
// -folded; pre-rotating the report by OpenXR yaw +folded about +Y cancels it. Y-up: x' = x cos + z sin, z' = -x sin + z cos.
// folded 0 -> cos 1, sin 0 exactly -> x and z unchanged bit for bit.
inline void ReportedHeadPosForFold(float foldedDeg, float& x, float& z)
{
    const float t = foldedDeg * 0.01745329252f;
    const float c = std::cos(t), s = std::sin(t);
    const float nx = x * c + z * s;
    const float nz = -x * s + z * c;
    x = nx;
    z = nz;
}

// ===== HAND-TWIST VIEW HOLD - docs/superpowers/specs/2026-11-19-hand-twist-view-hold-design.md §4 / §8b, INV-748 =====
// The camera base (PlayerFppVis+4112) is the eye point in BODY-LOCAL coordinates (INV-747). Torso from controllers turns the body
// by the hand twist G on top of the look; the head term is cancelled by the fold above, but two levers still turn with the body:
//   a = HSPINE1's body-local position minus the head term (L + R - S, INV-306) - unpitched;
//   v = the eye lever below HSPINE1 plus the face offset (eye - HSPINE1 + d1) - pitched in place by C06C60 (INV-310), P (body-local).
// c = (Y(G) - I) a + (P Y(G) P^T - I) v puts both where a body WITHOUT the twist would have them. Y(G) is EXACTLY the rotation
// ReportedHeadPosForFold applies (the verified S-61 sign, same units: G = Wrap180(after - look), the term PublishFolded adds), on
// body-local x/z about local +Y (the root's own up, INV-262 / R3). G == 0 -> c == 0 exactly. Row-major 3x3, m[3*r + c].
inline void FoldYawMat(float gDeg, float m[9])
{
    const float t = gDeg * 0.01745329252f;
    const float c = std::cos(t), s = std::sin(t);
    m[0] = c;    m[1] = 0.0f; m[2] = s;      // x' = x c + z s   (ReportedHeadPosForFold)
    m[3] = 0.0f; m[4] = 1.0f; m[5] = 0.0f;
    m[6] = -s;   m[7] = 0.0f; m[8] = c;      // z' = -x s + z c
}

inline void Mat3MulVecHt(const float m[9], const float v[3], float o[3])
{
    o[0] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2];
    o[1] = m[3] * v[0] + m[4] * v[1] + m[5] * v[2];
    o[2] = m[6] * v[0] + m[7] * v[1] + m[8] * v[2];
}

// P = I (or nullptr) when C06C60 did not pitch the basis the eye was read from.
inline void HandTwistViewCorrection(float gDeg, const float a[3], const float v[3], const float* P, float c[3])
{
    if (gDeg == 0.0f) { c[0] = c[1] = c[2] = 0.0f; return; }
    float Y[9];
    FoldYawMat(gDeg, Y);
    float ya[3];
    Mat3MulVecHt(Y, a, ya);
    float yv[3];
    if (P) {
        // P Y P^T v
        float pt[3];
        pt[0] = P[0] * v[0] + P[3] * v[1] + P[6] * v[2];
        pt[1] = P[1] * v[0] + P[4] * v[1] + P[7] * v[2];
        pt[2] = P[2] * v[0] + P[5] * v[1] + P[8] * v[2];
        float ypt[3];
        Mat3MulVecHt(Y, pt, ypt);
        Mat3MulVecHt(P, ypt, yv);
    } else {
        Mat3MulVecHt(Y, v, yv);
    }
    c[0] = (ya[0] - a[0]) + (yv[0] - v[0]);
    c[1] = (ya[1] - a[1]) + (yv[1] - v[1]);
    c[2] = (ya[2] - a[2]) + (yv[2] - v[2]);
}

// The delta is ALWAYS applied (2026-10-25, Jay: "Go with B"). The verdict is LOG ONLY: a REFUSED used to drop the delta to 0 for the
// player object, which pinned the elbow axis to the playspace start - both elbows went up once the player faced ~100+ deg away
// (rig item 3). In 957 of 1,302 refused windows (fold >= 60) the root followed the body, i.e. the delta was right; 5 followed the
// mapping yaw. The check still decides and logs, so a genuinely wrong delta stays visible.
inline float AppliedDeltaRad(const FacingCheck& c, float foldedDeg)
{
    (void)c;
    return BodyFacingDeltaRad(foldedDeg);
}

// ===== TORSO FROM CONTROLLERS - docs/superpowers/specs/2026-10-29-torso-from-controllers-design.md §4.2-4.4 =====
// The body (root) faces the real body judged from the two grips; the head keeps the view and the look. All in OpenXR LOCAL
// x / z and h's yaw convention WITHOUT the decouple flip (boneblend applies the flip once, where it writes the fields).

constexpr float              kTorsoMaxGapDeg    = 40.0f;   // never further from the head: under the sprint stick angle (INV-653)
constexpr float              kTorsoReachMinM    = 0.10f;   // grip midpoint closer to the neck than this: no reading, hold
constexpr float              kTorsoReachFullM   = 0.20f;   // full weight from here
constexpr float              kTorsoTauS         = 0.08f;   // hand-tremor filter on the gap only (the head is never filtered)
constexpr float              kTorsoJumpDeg      = 60.0f;   // a larger change in one step (snap, teleport, recentre) skips the filter
constexpr unsigned long long kTorsoStickStaleMs = 100;

// The torso's HEADING (OpenXR yaw, LOCAL) is what is filtered and held - never the gap - so a head-only turn moves the gap by
// exactly -h at once and the body does not move (final review C1 / C2, spec §2).
struct TorsoState {
    float              yaw     = 0.0f;   // the filtered torso heading, degrees
    float              heldYaw = 0.0f;   // the last heading taken at full weight (or where the clamp pulled it)
    bool               have    = false;
    unsigned long long obj     = 0;
};

constexpr float kTorsoBehindDeg = 90.0f;   // a hand midpoint further round than this from the head is no reading (final review I5)

struct TorsoIn {
    float neckX = 0.0f, neckZ = 0.0f;                          // the neck point (eye - R n) x / z
    float lX = 0.0f, lZ = 0.0f, rX = 0.0f, rZ = 0.0f;          // the grips x / z
    bool  haveL = false, haveR = false;
    float headYawXrDeg = 0.0f;   // 2 atan2(sy.y, sy.w) of the stripped head, degrees, no flip
    float dt = 0.0f;             // seconds since this object's previous step
    // Run T1 (Jay: "Ignore a hand at the head"): heights and the eyes, to drop a hand at the head or hanging at the side.
    float neckY = 0.0f;
    float eyeX = 0.0f, eyeY = 0.10f, eyeZ = 0.0f;              // the eye midpoint (the head pose position)
    float lY = -0.30f, rY = -0.30f;                            // the grips y
};

constexpr float kTorsoHeadNearM   = 0.25f;   // a grip this close to the eyes (a gun at the eye, a hand at the face) does not count
constexpr float kTorsoHangBelowM  = 0.40f;   // a grip this far below the neck ...
constexpr float kTorsoHangReachM  = 0.25f;   // ... and this close to it horizontally is hanging at the side: does not count

// Does this grip say anything about where the torso faces? (Run T1, Jay: "Ignore a hand at the head".)
inline bool TorsoHandUsable(const TorsoIn& in, float x, float y, float z)
{
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) { return false; }
    const float ex = x - in.eyeX, ey = y - in.eyeY, ez = z - in.eyeZ;
    if (ex * ex + ey * ey + ez * ez < kTorsoHeadNearM * kTorsoHeadNearM) { return false; }
    const float hx = x - in.neckX, hz = z - in.neckZ;
    if (in.neckY - y > kTorsoHangBelowM && hx * hx + hz * hz < kTorsoHangReachM * kTorsoHangReachM) { return false; }
    return true;
}

struct TorsoOut {
    float gapXrDeg;      // where the body faces relative to the head, + = left (OpenXR yaw)
    float rawGapXrDeg;   // this step's clamped reading (= the held gap when weight is 0)
    float weight;        // 0..1 reliability of the reading
    bool  clamped;
    bool  held;          // weight 0: the gap was held
    int   hands = 0;         // the grips this step's reading used: 0 none (held), 1 left, 2 right, 3 both
    float pulledDeg = 0.0f;  // how far the 40 deg limit pulled the torso heading this step (0 = not clamped)
};

constexpr float kTorsoShoulderHalfM = 0.18f;   // a single counted grip is measured from ITS shoulder: the neck -+ this along the
                                               // torso's right (run T2: from the neck it read ~20 deg to its own side)

// The yaw of a horizontal direction in h's convention: forward (-z) is 0, left (-x) is +90.
inline float TorsoYawXrDeg(float vx, float vz)
{
    return std::atan2(-vx, -vz) * 57.2957795f;
}

inline TorsoOut TorsoGapStep(TorsoState& s, unsigned long long obj, const TorsoIn& in)
{
    if (obj != s.obj) {   // a new PlayerDI holds none of the old torso
        s = TorsoState{};
        s.obj = obj;
    }
    TorsoOut o{ 0.0f, 0.0f, 0.0f, false, false };
    const float h = std::isfinite(in.headYawXrDeg) ? in.headYawXrDeg : 0.0f;
    if (!s.have) { s.yaw = s.heldYaw = h; }   // nothing read yet: the body starts under the head
    // Both usable grips -> their midpoint; one -> that grip alone (a pistol held out to the side still twists the torso); none -> hold.
    const bool useL = in.haveL && TorsoHandUsable(in, in.lX, in.lY, in.lZ);
    const bool useR = in.haveR && TorsoHandUsable(in, in.rX, in.rY, in.rZ);
    const int  nUse = (useL ? 1 : 0) + (useR ? 1 : 0);
    const float px = nUse == 2 ? 0.5f * (in.lX + in.rX) : (useL ? in.lX : in.rX);
    const float pz = nUse == 2 ? 0.5f * (in.lZ + in.rZ) : (useL ? in.lZ : in.rZ);
    // The reference: the neck for both grips (the two shoulder offsets cancel), that grip's own SHOULDER for one - along the torso's
    // current right (forward (-sin y, -cos y) -> right (cos y, -sin y), x / z).
    float ox = in.neckX, oz = in.neckZ;
    if (nUse == 1) {
        const float yr = s.yaw * 0.01745329252f;
        const float side = useL ? -kTorsoShoulderHalfM : kTorsoShoulderHalfM;
        ox += side * std::cos(yr);
        oz += side * -std::sin(yr);
    }
    const float vx = px - ox;
    const float vz = pz - oz;
    const float len = std::sqrt(vx * vx + vz * vz);
    const bool usable = nUse > 0 && std::isfinite(len) && std::isfinite(in.headYawXrDeg);
    float w = usable ? (len - kTorsoReachMinM) / (kTorsoReachFullM - kTorsoReachMinM) : 0.0f;
    if (w < 0.0f) { w = 0.0f; }
    if (w > 1.0f) { w = 1.0f; }
    const float measured = w > 0.0f ? TorsoYawXrDeg(vx, vz) : s.heldYaw;
    if (w > 0.0f && std::fabs(Wrap180(measured - h)) > kTorsoBehindDeg) { w = 0.0f; }   // behind the neck: no reading
    const float target = w > 0.0f ? Wrap180(s.heldYaw + w * Wrap180(measured - s.heldYaw)) : s.heldYaw;
    const float step = Wrap180(target - s.yaw);
    if (!s.have || std::fabs(step) > kTorsoJumpDeg) {
        s.yaw = target;
    } else {
        const float dt = (std::isfinite(in.dt) && in.dt > 0.0f) ? in.dt : 0.0f;
        s.yaw = Wrap180(s.yaw + (1.0f - std::exp(-dt / kTorsoTauS)) * step);
    }
    s.have = true;
    if (w >= 1.0f) { s.heldYaw = s.yaw; }
    float gap = Wrap180(s.yaw - h);
    if (gap > kTorsoMaxGapDeg || gap < -kTorsoMaxGapDeg) {   // the head pulls the torso round, and it stays where it was pulled
        gap = gap > 0.0f ? kTorsoMaxGapDeg : -kTorsoMaxGapDeg;
        const float before = s.yaw;
        s.yaw = s.heldYaw = Wrap180(h + gap);
        o.clamped = true;
        o.pulledDeg = std::fabs(Wrap180(s.yaw - before));
    }
    o.hands = w > 0.0f ? (useL ? 1 : 0) + (useR ? 2 : 0) : 0;
    o.gapXrDeg = gap;
    o.rawGapXrDeg = w > 0.0f ? Wrap180(measured - h) : gap;
    o.weight = w;
    o.held = w <= 0.0f;
    return o;
}

// ===== NECK HOLD (trace 20261119_1430, Jay 2026-11-19: "hold the root while headturning till it hits the range the body is meant to
// turn"; range 40 deg + re-centre while walking: "Yes.") - replaces the hand twist (removed, trace 20261119_1400). The body's HEADING
// (OpenXR yaw, h's convention, no flip) holds while the head turns; the gap (body - head) is clamped to +-kNeckRangeDeg, the head pulling
// the body round past it; while walking the body eases back under the head; a stick turn never changes h, so the body turns with the
// stick at once (the gap is kept). Reuses the hand twist's limit and jump test.
constexpr float kNeckRangeDeg      = kTorsoMaxGapDeg;   // 40: under the sprint stick angle (INV-653)
constexpr float kNeckRecentreTauS  = 0.30f;             // walking: the body back under the head (my call, a constant)

struct NeckHoldState {
    float              yaw  = 0.0f;   // the body's heading, degrees (OpenXR, LOCAL)
    float              lastH = 0.0f;  // the head yaw at the previous step
    bool               have = false;
    unsigned long long obj  = 0;
};

struct NeckOut {
    float gapXrDeg = 0.0f;   // where the body faces relative to the head, + = left (OpenXR yaw)
    bool  clamped  = false;  // at the range this step (the head pulled the body)
    bool  recentring = false;
    bool  reset    = false;  // a new object / a head-yaw jump put the body under the head
};

inline NeckOut NeckHoldStep(NeckHoldState& s, unsigned long long obj, float headYawXrDeg, bool walking, float dt)
{
    NeckOut o;
    const float h = std::isfinite(headYawXrDeg) ? headYawXrDeg : s.lastH;
    if (obj != s.obj || !s.have || std::fabs(Wrap180(h - s.lastH)) > kTorsoJumpDeg) {   // new player / recentre / snap
        s.obj = obj;
        s.yaw = h;
        s.have = true;
        o.reset = true;
    } else if (walking) {
        const float d = (std::isfinite(dt) && dt > 0.0f) ? dt : 0.0f;
        s.yaw = Wrap180(s.yaw + (1.0f - std::exp(-d / kNeckRecentreTauS)) * Wrap180(h - s.yaw));
        o.recentring = true;
    }
    s.lastH = h;
    float gap = Wrap180(s.yaw - h);
    if (gap > kNeckRangeDeg || gap < -kNeckRangeDeg) {   // the head pulls the body round, and it stays where it was pulled
        gap = gap > 0.0f ? kNeckRangeDeg : -kNeckRangeDeg;
        s.yaw = Wrap180(h + gap);
        o.clamped = true;
    }
    o.gapXrDeg = gap;
    return o;
}

// The facing to write: look + gap, reached from the engine's unwrapped facing the short way (never a 360 step).
inline float FacingTarget(float lookDeg, float gapFieldDeg, float facingNowDeg)
{
    return facingNowDeg + Wrap180(lookDeg + gapFieldDeg - facingNowDeg);
}

// ===== WALK FOLLOWS THE LOOK - docs/superpowers/specs/2026-11-18-walk-follows-look-design.md §2 / §3a, INV-652 addenda 1-3 =====
// The player walk (gamedll +BA2B20) hands +ED2040 the world walk vector w = M3x3 * l, M = the root's 3x4 (mtx34 rows of 4:
// left column m[0] m[4] m[8], up m[1] m[5] m[9], forward m[2] m[6] m[10]; +EBAA90 builds forward = (cos f, 0, sin f)), l = the
// local walk vector (player+4616, local +X left, +Y up, +Z forward). WalkOnLook rebuilds the same vector on the LOOK yaw L
// (degrees, the same units as the facing f): left (sin L, 0, -cos L), up (0, 1, 0), forward (cos L, 0, sin L).
// It refuses (keep w) unless every input is finite, the root is upright within kWalkUprightCos and w == M3x3 * l within
// max(kWalkCheckAbs, kWalkCheckRel * |w|) - the self-check that our model of the engine's root is right for this call.
constexpr float kWalkUprightCos = 0.99984770f;   // cos 1 deg
constexpr float kWalkCheckAbs   = 0.001f;        // 1 mm/s
constexpr float kWalkCheckRel   = 0.005f;        // 0.5 % of |w|

enum WalkWhy { kWalkFixed = 0, kWalkNonFinite = 1, kWalkNotUpright = 2, kWalkCheckFailed = 3 };

struct WalkOut {
    int   why;       // WalkWhy
    float checkErr;  // |w - M3x3 l| (m/s); -1 when not computed
    float turnDeg;   // the root's forward yaw minus L, wrapped (how far walking was turned back); 0 when not computed
    float w[3];      // the vector to send: w_L when why == kWalkFixed, else the incoming w unchanged
};

inline WalkOut WalkOnLook(const float m[12], const float l[3], const float w[3], float lookDeg)
{
    WalkOut o{ kWalkNonFinite, -1.0f, 0.0f, { w[0], w[1], w[2] } };
    for (int i = 0; i < 12; ++i) {
        if (!std::isfinite(m[i])) { return o; }
    }
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(l[i]) || !std::isfinite(w[i])) { return o; }
    }
    if (!std::isfinite(lookDeg)) { return o; }
    const float upLen = std::sqrt(m[1] * m[1] + m[5] * m[5] + m[9] * m[9]);
    if (!(upLen > 1e-6f) || m[5] / upLen < kWalkUprightCos) {
        o.why = kWalkNotUpright;
        return o;
    }
    const float fx = m[0] * l[0] + m[1] * l[1] + m[2] * l[2];
    const float fy = m[4] * l[0] + m[5] * l[1] + m[6] * l[2];
    const float fz = m[8] * l[0] + m[9] * l[1] + m[10] * l[2];
    const float ex = fx - w[0], ey = fy - w[1], ez = fz - w[2];
    o.checkErr = std::sqrt(ex * ex + ey * ey + ez * ez);
    const float wLen = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    const float tol = kWalkCheckAbs > kWalkCheckRel * wLen ? kWalkCheckAbs : kWalkCheckRel * wLen;
    if (!(o.checkErr <= tol)) {
        o.why = kWalkCheckFailed;
        return o;
    }
    const float rootDeg = std::atan2(m[10], m[2]) * 57.2957795f;
    o.turnDeg = Wrap180(rootDeg - lookDeg);
    const float r = lookDeg * 0.01745329252f;
    const float c = std::cos(r), s = std::sin(r);
    o.w[0] = l[0] * s + l[2] * c;
    o.w[1] = l[1];
    o.w[2] = -l[0] * c + l[2] * s;
    o.why = kWalkFixed;
    return o;
}

// A published torso gap is used only if a FOLDING update wrote it under kTorsoStickStaleMs ago (walk follows the look's gate).
inline bool StickGapFresh(unsigned long long nowMs, unsigned long long pubMs, bool pubFolding)
{
    return pubMs != 0 && pubFolding && nowMs >= pubMs && nowMs - pubMs < kTorsoStickStaleMs;
}

}  // namespace headfollow
}  // namespace dlvr
