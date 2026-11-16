// Pure logic for the right-shoulder holster - docs/SPEC-SHOULDER-HOLSTER-2026-09-29.md §1 (zone) and §2 (press).
// No OpenXR, no Windows, no logging, so src/src/tests/holster_logic_test.cpp compiles it on its own.
// holster.cpp owns everything with side effects (poses, R1, haptics, the log).
#pragma once

#include <cmath>

namespace dlvr {
namespace holster {

struct Vec3 { float x, y, z; };

// Same constant as ovr2xr.cpp's vr_grip block - one threshold for R1 and for the holster, not two.
constexpr float kGripThreshold = 0.5f;
// Leave the zone only past radius x this, so a hand resting on the edge cannot chatter enter/exit (and buzz).
constexpr float kExitScale = 1.2f;

// The palm's offset from the head in the HEAD-YAW frame: +X right, +Y up, +Z behind (OpenXR view axes).
// Only the head's yaw is removed - looking down or tilting must not move the shoulder (spec §1).
// Yaw extraction is the same formula as ovr2xr.cpp's vr_decouple (atan2 about +Y).
inline float HeadYaw(float qx, float qy, float qz, float qw)
{
    return std::atan2(2.0f * (qw * qy + qx * qz), 1.0f - 2.0f * (qy * qy + qx * qx));
}

// The same, with the yaw (radians, left positive) given - the body-lock's frame (2026-10-20).
inline Vec3 LocalOffsetYaw(const Vec3& origin, float yaw, const Vec3& palm)
{
    const float dx = palm.x - origin.x;
    const float dy = palm.y - origin.y;
    const float dz = palm.z - origin.z;
    const float c = std::cos(yaw);
    const float s = std::sin(yaw);
    // Ry(-yaw) * d
    return { dx * c - dz * s, dy, dx * s + dz * c };
}

inline Vec3 LocalOffset(const Vec3& head, float qx, float qy, float qz, float qw, const Vec3& palm)
{
    return LocalOffsetYaw(head, HeadYaw(qx, qy, qz, qw), palm);
}

// ===== BODY-LOCK (docs/superpowers/specs/2026-10-20-holster-body-lock-design.md) =====
// The right shoulder and both hips hang off the BODY, not the headset: a per-zone anchor (the neck point / the pelvis) plus an
// offset latched once in a body frame whose yaw is FacingStep's (a slow head average, squared up to the hand line only for a
// standing player's hands at rest - 2026-11-07; until then the line between the hands). LocalOffsetYaw(VirtualHead(...)) then
// feeds the unchanged Machine, so every zone centre keeps its value and meaning.

inline float WrapPi(float r)
{
    r = std::fmod(r, 6.28318531f);
    if (r > 3.14159265f)   { r -= 6.28318531f; }
    if (r <= -3.14159265f) { r += 6.28318531f; }
    return r;
}

// Ry(+yaw) * local - LocalOffsetYaw's inverse.
inline Vec3 ToWorldYaw(const Vec3& l, float yaw)
{
    const float c = std::cos(yaw), s = std::sin(yaw);
    return { l.x * c + l.z * s, l.y, -l.x * s + l.z * c };
}

// The real neck: eye - R n, R the FULL head rotation (n = head_neck_pivot_*, head frame). Moves with a lean, not a head turn.
inline Vec3 NeckPoint(const Vec3& eye, float x, float y, float z, float w, const Vec3& n)
{
    const float rx = (1 - 2 * (y * y + z * z)) * n.x + 2 * (x * y - z * w) * n.y + 2 * (x * z + y * w) * n.z;
    const float ry = 2 * (x * y + z * w) * n.x + (1 - 2 * (x * x + z * z)) * n.y + 2 * (y * z - x * w) * n.z;
    const float rz = 2 * (x * z - y * w) * n.x + 2 * (y * z + x * w) * n.y + (1 - 2 * (x * x + y * y)) * n.z;
    return { eye.x - rx, eye.y - ry, eye.z - rz };
}

// The inverse of the arm solve's tracking -> world map (boneblend.cpp: (ox c + oz s, oy, -ox s + oz c)).
inline Vec3 TrackingFromWorld(const Vec3& w, float c, float s)
{
    return { w.x * c - w.z * s, w.y, w.x * s + w.z * c };
}

// The body's yaw (HeadYaw's convention) from the left -> right hand line: the body's right is (cos, -sin) in x/z.
inline float HandLineYaw(const Vec3& l, const Vec3& r)
{
    return std::atan2(-(r.z - l.z), r.x - l.x);
}

// ===== THE FACING (2026-11-07, Jay: "Go with 6"; testers: "The right shoulder spot shifts sometimes when your body moves") =====
// The body's yaw is a SLOW average of the head's yaw (kDriftTauS), squared up to the hand line only when a STANDING player's
// hands hang at rest by their sides (RestPose, held kRestHoldS). The old rule followed the hand line within ~70 ms and swung the
// zones 11-19 cm (shoulder) / 25-43 cm (right hip) per second at p90 in seven seated captures; the drift alone 0.7-1.9 / 1.6-4.3
// (offline replay, STATE 2026-11-07). RestPose passed 0 seated samples in those seven captures (closest hand drop 0.506 m).
constexpr float kFacingClampR  = 70.0f * 0.0174532925f;   // the facing never sits further than this from the head (spec 10-20 §3.2.4)
constexpr float kDriftTauS     = 4.0f;                    // the head average's time constant
constexpr float kRestTauS      = 0.5f;                    // the square-up's time constant
constexpr double kRestHoldS    = 0.3;                     // the rest pose must hold this long first
constexpr float kRestDropM     = 0.55f;                   // both hands at least this far below the neck
constexpr float kRestForeAftM  = 0.20f;                   // each hand within this of the neck, fore / aft (head-yaw frame)
constexpr float kRestOutM      = 0.12f;                   // each hand at least this far out to its own side
constexpr float kRestSepM      = 0.25f;                   // the hands at least this far apart, horizontally
constexpr float kRestMaxSpeed  = 0.5f;                    // m/s, each hand
constexpr double kSpeedWindowS = 0.05;                    // speeds are measured over at least this long, never per poll
constexpr float kRestLineR     = 45.0f * 0.0174532925f;   // the hand line within this of the head
constexpr float kSteadyR       = 10.0f * 0.0174532925f;
constexpr double kSteadyS      = 0.5;
constexpr float kLatchHeadR    = 15.0f * 0.0174532925f;

struct Facing {
    bool   have = false;
    float  yaw = 0.0f;
    float  steadyRef = 0.0f;
    double steadySince = -1.0;
    double restSince = -1.0;      // the rest pose has held since (-1 = not in it)
    Vec3   prevL{ 0.0f, 0.0f, 0.0f }, prevR{ 0.0f, 0.0f, 0.0f };
    double prevAtS = -1.0;        // -1 = no speed reading yet
    float  speedL = 1e9f, speedR = 1e9f;   // unknown reads as too fast
    // FacingStepV2 only (2026-11-16): the follow after a held, hands-confirmed turn, and the hands' snapshot taken while the head was
    // last inside the dead band.
    bool   following = false;
    double outsideSince = -1.0;   // the head beyond the dead band since (-1 = inside)
    bool   snapOk = false;
    float  snapHead = 0.0f;
    float  snapL[3] = {}, snapR[3] = {};   // HandSnapOf: bearing (rad), horizontal radius (m), height above the neck (m)
    bool   lastConfirmed = false;          // the last step's body-turn confirmation (for the log)
    // v2.1 (2026-11-16, trace 20261116_1545_standing_posture): the head beyond the band continuously since (whatever the freezes);
    // standing + held kStandHoldS = a held turn.
    double farSince = -1.0;
    bool   lastLongHeld = false;
};
// Rest = squared up to the hand line; Drift = toward the head; Frozen = held for a hand in a zone / a placement; Limit = pulled
// to within kFacingClampR of the head this step.
enum class FacingWhy { Rest, Drift, Frozen, Limit };

// Held while a hand is at the shoulder (a draw - review I2: the left hand resting at the left hip must not let it through), or
// while exactly ONE hand is in a hip zone - a zone never moves under the hand in it. Both hands in both hip zones is a hip line
// and is not held. A squeezed grip no longer holds it (2026-11-07, Jay "Go ahead"): it only blocks the rest square-up.
inline bool FreezeFacing(bool shoulderIn, bool rightHipIn, bool leftHipIn)
{
    return shoulderIn || (rightHipIn != leftHipIn);
}

// A standing player's hands hanging at rest by their sides (positions only - FacingStep adds the speeds and the hold time).
inline bool RestPose(const Vec3& neck, float headYaw, const Vec3& l, const Vec3& r)
{
    const Vec3 L = LocalOffsetYaw(neck, headYaw, l);   // +X right, +Y up, +Z behind
    const Vec3 R = LocalOffsetYaw(neck, headYaw, r);
    if (-L.y < kRestDropM || -R.y < kRestDropM) { return false; }
    if (std::fabs(L.z) > kRestForeAftM || std::fabs(R.z) > kRestForeAftM) { return false; }
    if (L.x > -kRestOutM || R.x < kRestOutM) { return false; }
    const float dx = r.x - l.x, dz = r.z - l.z;
    if (dx * dx + dz * dz < kRestSepM * kRestSepM) { return false; }
    return std::fabs(WrapPi(HandLineYaw(l, r) - headYaw)) <= kRestLineR;
}

// Review I1: the eye the latch is taken from - the neck + n turned by the head's YAW only, so a head pitched or rolled at the latch
// moment bakes nothing into the zones (eye = neck + R n; level R = Ry(headYaw)).
inline Vec3 LevelEye(const Vec3& neck, float headYaw, const Vec3& n)
{
    const float c = std::cos(headYaw), s = std::sin(headYaw);
    return { neck.x + n.x * c + n.z * s, neck.y + n.y, neck.z - n.x * s + n.z * c };
}

// One step. neck = the real neck point (NeckPoint); freeze = FreezeFacing(...) or a placement in progress; squeezed = either grip
// past kGripThreshold (blocks the square-up only - a hand holding something is not a hand at rest).
inline FacingWhy FacingStep(Facing& f, const Vec3& neck, const Vec3& l, const Vec3& r, bool lOk, bool rOk, float headYaw, bool freeze,
                            bool squeezed, double nowS, float dtS)
{
    if (!f.have) {
        f.have = true;
        f.yaw = headYaw;
        f.steadyRef = headYaw;
        f.steadySince = nowS;
    }
    // The hands' speeds, over at least kSpeedWindowS (a per-poll difference at ~570 polls/s would be tracking noise / dt).
    if (!lOk || !rOk) {
        f.prevAtS = -1.0;
        f.speedL = f.speedR = 1e9f;
    } else if (f.prevAtS < 0.0) {
        f.prevL = l; f.prevR = r; f.prevAtS = nowS;
    } else if (nowS - f.prevAtS >= kSpeedWindowS) {
        const float inv = static_cast<float>(1.0 / (nowS - f.prevAtS));
        const float lx = l.x - f.prevL.x, ly = l.y - f.prevL.y, lz = l.z - f.prevL.z;
        const float rx = r.x - f.prevR.x, ry = r.y - f.prevR.y, rz = r.z - f.prevR.z;
        f.speedL = std::sqrt(lx * lx + ly * ly + lz * lz) * inv;
        f.speedR = std::sqrt(rx * rx + ry * ry + rz * rz) * inv;
        f.prevL = l; f.prevR = r; f.prevAtS = nowS;
    }
    const bool rest = lOk && rOk && !squeezed && f.speedL <= kRestMaxSpeed && f.speedR <= kRestMaxSpeed &&
                      RestPose(neck, headYaw, l, r);
    if (!rest) { f.restSince = -1.0; } else if (f.restSince < 0.0) { f.restSince = nowS; }

    FacingWhy why = FacingWhy::Frozen;
    if (!freeze) {
        const float dt = dtS > 0.0f ? dtS : 0.0f;
        if (rest && nowS - f.restSince >= kRestHoldS) {
            const float a = 1.0f - std::exp(-dt / kRestTauS);
            f.yaw = WrapPi(f.yaw + a * WrapPi(HandLineYaw(l, r) - f.yaw));
            why = FacingWhy::Rest;
        } else {
            const float a = 1.0f - std::exp(-dt / kDriftTauS);
            f.yaw = WrapPi(f.yaw + a * WrapPi(headYaw - f.yaw));
            why = FacingWhy::Drift;
        }
        const float off = WrapPi(f.yaw - headYaw);
        if (off > kFacingClampR || off < -kFacingClampR) {
            f.yaw = WrapPi(headYaw + (off > 0.0f ? kFacingClampR : -kFacingClampR));
            why = FacingWhy::Limit;
        }
    }
    if (std::fabs(WrapPi(f.yaw - f.steadyRef)) > kSteadyR) {
        f.steadyRef = f.yaw;
        f.steadySince = nowS;
    }
    return why;
}

inline void FacingReset(Facing& f) { f = Facing{}; }

inline bool FacingSteady(const Facing& f, double nowS) { return f.have && f.steadySince >= 0.0 && nowS - f.steadySince >= kSteadyS; }

struct Latch {
    bool have = false;
    Vec3 eyeFromAnchor{ 0.0f, 0.0f, 0.0f };
};

// Latch once: the eye's offset from the zone's anchor in the body frame, taken while the head faces the body - so at that moment
// the virtual head IS the eye and the zone sits exactly where today's head-relative zone does. holster.cpp passes LevelEye (review
// I1), so the pitch of the head at that moment is not baked in.
inline bool LatchTry(Latch& L, const Facing& f, double nowS, float headYaw, const Vec3& eye, const Vec3& anchor)
{
    if (L.have || !FacingSteady(f, nowS) || std::fabs(WrapPi(headYaw - f.yaw)) > kLatchHeadR) { return false; }
    L.eyeFromAnchor = LocalOffsetYaw(anchor, f.yaw, eye);
    L.have = true;
    return true;
}

inline Vec3 VirtualHead(const Vec3& anchor, float yaw, const Latch& L)
{
    const Vec3 o = ToWorldYaw(L.eyeFromAnchor, yaw);
    return { anchor.x + o.x, anchor.y + o.y, anchor.z + o.z };
}

// ===== THE FACING V2 + THE HIP POINT (2026-11-16; trace docs/traces/20261116_1450_hip_anchor_facing_v2.md) =====
// Jay: "We want them to stay exactly where they are, regardless of lean, crouch, player position" / "bulletproof". Built from RUN HC1
// (standing, backups/20261116_hipcap_run_hc1) and checked on 38 seated archive sessions + 48 gun sessions, offline (scratchpad hc1/:
// hc_facing4.py facing_v5, hc_cv.py anchor_v3 - this is a line-for-line port). HC1: today's facing swung 41.7 deg with the body still;
// v2 0.5 deg; hips within 150 mm of the belt on 48 / 49 grabs leave-one-step-out (today 31 / 49). Every constant below is the replay's.

// The head's pitch, rad, + = looking up (asin of the forward (-Z) vector's height).
inline float HeadPitch(float qx, float qy, float qz, float qw)
{
    float s = 2.0f * (qx * qw - qy * qz);
    if (s > 1.0f) { s = 1.0f; }
    if (s < -1.0f) { s = -1.0f; }
    return std::asin(s);
}

// A hand seen from the neck, horizontally: [0] bearing (HeadYaw's convention: forward = 0, left +), [1] radius m, [2] height m.
inline void HandSnapOf(const Vec3& neck, const Vec3& p, float out[3])
{
    const float dx = p.x - neck.x, dz = p.z - neck.z;
    out[0] = std::atan2(-dx, -dz);
    out[1] = std::sqrt(dx * dx + dz * dz);
    out[2] = p.y - neck.y;
}

constexpr float  kBandR         = 30.0f * 0.0174532925f;   // the head moves this far from the facing without moving it
constexpr double kBandHoldS     = 0.5;                     // ... and must stay beyond it this long before a confirmed turn is followed
constexpr float  kFollowTauS    = 0.4f;                    // the follow's time constant
constexpr float  kFollowDoneR   = 5.0f * 0.0174532925f;    // the follow ends this close to the head
constexpr float  kPitchFreezeR  = 35.0f * 0.0174532925f;   // a head pitched below -this holds the facing (looking at the body)
constexpr float  kConfirmShare  = 0.5f;                    // each hand turned round the neck by >= this x the head's turn
constexpr float  kConfirmRigidM = 0.05f;                   // ... keeping its horizontal radius and height within this
constexpr float  kConfirmAgreeR = 15.0f * 0.0174532925f;   // ... the two hands' turns within this of each other
constexpr float  kConfirmMaxM   = 0.35f;                   // ... both hands within this of the neck (an extended, aiming arm never counts)
constexpr float  kConfirmMinM   = 0.08f;                   // a hand nearer than this to the neck axis has no usable bearing

// One step of the v2 facing. zoneFreeze = FreezeFacing(...) (a hand at the shoulder / exactly one hip); otherFreeze = a placement in
// progress. Inside the band the facing holds; a head held beyond it for kBandHoldS is followed ONLY if both hands confirm a body turn
// (a confirmed turn also lifts the zone freeze); a head pitched below -kPitchFreezeR holds it; the standing rest square-up is v1's;
// there is no 70 deg clamp. Returns Rest / Drift (= following) / Frozen (= held).
// v2.1 (RUN HA1, standing + roomscale: 13 turns made with the hands busy were never confirmed, 91 s with the zones behind the
// player): STANDING, a head held beyond the band for kStandHoldS counts as a body turn with no hands needed. Seated it does not
// (offline: 15 of 38 seated sessions would follow long glances). HA1 replay: 91 -> 9 s stuck.
constexpr double kStandHoldS = 1.5;

inline FacingWhy FacingStepV2(Facing& f, const Vec3& neck, const Vec3& l, const Vec3& r, bool lOk, bool rOk, float headYaw,
                              float headPitch, bool zoneFreeze, bool otherFreeze, bool squeezed, double nowS, float dtS,
                              bool standing = false)
{
    if (!f.have) {
        f.have = true;
        f.yaw = headYaw;
        f.steadyRef = headYaw;
        f.steadySince = nowS;
    }
    // the hands' speeds (v1's rule, for the rest square-up)
    if (!lOk || !rOk) {
        f.prevAtS = -1.0;
        f.speedL = f.speedR = 1e9f;
    } else if (f.prevAtS < 0.0) {
        f.prevL = l; f.prevR = r; f.prevAtS = nowS;
    } else if (nowS - f.prevAtS >= kSpeedWindowS) {
        const float inv = static_cast<float>(1.0 / (nowS - f.prevAtS));
        const float lx = l.x - f.prevL.x, ly = l.y - f.prevL.y, lz = l.z - f.prevL.z;
        const float rx = r.x - f.prevR.x, ry = r.y - f.prevR.y, rz = r.z - f.prevR.z;
        f.speedL = std::sqrt(lx * lx + ly * ly + lz * lz) * inv;
        f.speedR = std::sqrt(rx * rx + ry * ry + rz * rz) * inv;
        f.prevL = l; f.prevR = r; f.prevAtS = nowS;
    }
    const float off = std::fabs(WrapPi(headYaw - f.yaw));
    float sl[3] = {}, sr[3] = {};
    bool handsOk = lOk && rOk;
    if (handsOk) {
        HandSnapOf(neck, l, sl);
        HandSnapOf(neck, r, sr);
        handsOk = sl[1] > kConfirmMinM && sr[1] > kConfirmMinM;
    }
    if (off <= kBandR && handsOk) {
        f.snapOk = true;
        f.snapHead = headYaw;
        for (int i = 0; i < 3; ++i) { f.snapL[i] = sl[i]; f.snapR[i] = sr[i]; }
    }
    bool confirmed = false;
    if (f.snapOk && handsOk && off > kBandR) {
        const float dh = WrapPi(headYaw - f.snapHead);
        if (std::fabs(dh) > 1e-3f) {
            const float dl = WrapPi(sl[0] - f.snapL[0]);
            const float dr = WrapPi(sr[0] - f.snapR[0]);
            confirmed = dl / dh >= kConfirmShare && dr / dh >= kConfirmShare;
            if (confirmed) {
                confirmed = sl[1] <= kConfirmMaxM && sr[1] <= kConfirmMaxM && f.snapL[1] <= kConfirmMaxM && f.snapR[1] <= kConfirmMaxM;
            }
            if (confirmed) {
                confirmed = std::fabs(sl[1] - f.snapL[1]) <= kConfirmRigidM && std::fabs(sr[1] - f.snapR[1]) <= kConfirmRigidM &&
                            std::fabs(sl[2] - f.snapL[2]) <= kConfirmRigidM && std::fabs(sr[2] - f.snapR[2]) <= kConfirmRigidM &&
                            std::fabs(WrapPi(dl - dr)) <= kConfirmAgreeR;
            }
        }
    }
    f.lastConfirmed = confirmed;
    if (off > kBandR) {
        if (f.farSince < 0.0) { f.farSince = nowS; }
    } else {
        f.farSince = -1.0;
    }
    const bool longHeld = standing && f.farSince >= 0.0 && nowS - f.farSince >= kStandHoldS;
    f.lastLongHeld = longHeld;
    const bool rest = lOk && rOk && !squeezed && f.speedL <= kRestMaxSpeed && f.speedR <= kRestMaxSpeed &&
                      RestPose(neck, headYaw, l, r);
    if (!rest) { f.restSince = -1.0; } else if (f.restSince < 0.0) { f.restSince = nowS; }

    const bool freeze = otherFreeze || headPitch < -kPitchFreezeR || (zoneFreeze && !(confirmed || longHeld));
    FacingWhy why = FacingWhy::Frozen;
    if (!freeze) {
        const float dt = dtS > 0.0f ? dtS : 0.0f;
        if (rest && nowS - f.restSince >= kRestHoldS) {
            const float a = 1.0f - std::exp(-dt / kRestTauS);
            f.yaw = WrapPi(f.yaw + a * WrapPi(HandLineYaw(l, r) - f.yaw));
            why = FacingWhy::Rest;
        } else if (f.following) {
            const float a = 1.0f - std::exp(-dt / kFollowTauS);
            f.yaw = WrapPi(f.yaw + a * WrapPi(headYaw - f.yaw));
            why = FacingWhy::Drift;
            if (off < kFollowDoneR) { f.following = false; f.outsideSince = -1.0; }
        } else if (off > kBandR) {
            if (f.outsideSince < 0.0) { f.outsideSince = nowS; }
            if (nowS - f.outsideSince >= kBandHoldS && (confirmed || longHeld)) { f.following = true; }
        } else {
            f.outsideSince = -1.0;
        }
    }
    if (std::fabs(WrapPi(f.yaw - f.steadyRef)) > kSteadyR) {
        f.steadyRef = f.yaw;
        f.steadySince = nowS;
    }
    return why;
}

// THE POSTURE (v2.1): STANDING once the eye has been above kStandEyeM for kPostureHoldS; SEATED once below kSeatEyeM for kPostureHoldS
// (the gap and the hold stop it flickering; a crouch only switches the held turn off while it lasts). No floor -> SEATED (the safe
// side: exactly the f365b5c0 facing). Eye height above the runtime's STAGE floor, metres.
constexpr float  kStandEyeM    = 1.38f;
constexpr float  kSeatEyeM     = 1.33f;
constexpr double kPostureHoldS = 2.0;

struct Posture {
    bool   standing = false;
    double aboveSince = -1.0, belowSince = -1.0;
};

// One step; returns true when the posture changed this step.
inline bool PostureStep(Posture& p, bool floorOk, float eyeAboveFloorM, double nowS)
{
    const bool was = p.standing;
    if (!floorOk) {
        p.standing = false;
        p.aboveSince = p.belowSince = -1.0;
        return was;
    }
    if (eyeAboveFloorM > kStandEyeM) {
        if (p.aboveSince < 0.0) { p.aboveSince = nowS; }
        if (nowS - p.aboveSince >= kPostureHoldS) { p.standing = true; }
    } else {
        p.aboveSince = -1.0;
    }
    if (eyeAboveFloorM < kSeatEyeM) {
        if (p.belowSince < 0.0) { p.belowSince = nowS; }
        if (nowS - p.belowSince >= kPostureHoldS) { p.standing = false; }
    } else {
        p.belowSince = -1.0;
    }
    return p.standing != was;
}

// THE HIP POINT. RUN HC1 (feet still): a lean leaves the belt ~room-fixed and pushed slightly AGAINST the neck; a squat drops it ~0.6 x
// the neck's drop and moves it back; sitting moves it with the neck, the neck ending BEHIND the body axis (a squat puts it ahead).
// In the REFERENCE frame (appSpace shifts undone - holster.cpp passes the neck and the facing through GetAppBase):
//   P   = the body axis (x/z): follows the neck (tau kHipCatchTauS) while the neck has not dropped more than kHipDipTolM below the
//         standing reference, holds while it has, and is dragged when the neck is further than kHipDragM from it;
//   ref = the standing neck height: rises with the neck at once; a drop past kHipSitDropM with the neck more than kHipSitBehindM
//         BEHIND P = sat down -> ref and P move to the neck;
//   resid = the drop a straight-torso lean of the neck's offset from P does not explain (a squat);
//   anchor = the hips raised by kHipTorsoM = (P - counter-balance + squat back-shift, ref - kHipSquatDrop x resid) - which IS the neck
//   point whenever the player stands upright with P caught up, so every stored hip centre keeps its meaning.
constexpr float kHipCatchTauS   = 0.25f;
constexpr float kHipDipTolM     = 0.04f;
constexpr float kHipDragM       = 0.35f;
constexpr float kHipSitDropM    = 0.15f;
constexpr float kHipSitBehindM  = 0.10f;
constexpr float kHipTorsoM      = 0.50f;   // HC1: neck -> belt 0.499 m upright (the rod in the lean's dip)
constexpr float kHipSquatDrop   = 0.6f;
constexpr float kHipSquatBack   = 0.2f;
constexpr float kHipCounterFwd  = 0.55f;
constexpr float kHipCounterSide = 0.75f;

struct HipPoint {
    bool  have = false;
    float px = 0.0f, pz = 0.0f;   // the body axis, reference frame
    float ref = 0.0f;             // the standing neck height, reference frame
};

inline Vec3 HipStep(HipPoint& h, const Vec3& neck, float facingYaw, float dtS)
{
    if (!h.have) {
        h.have = true;
        h.px = neck.x; h.pz = neck.z; h.ref = neck.y;
    }
    if (neck.y > h.ref) { h.ref = neck.y; }
    const float dt = dtS > 0.0f ? dtS : 0.0f;
    if (h.ref - neck.y <= kHipDipTolM && dt > 0.0f) {
        const float a = 1.0f - std::exp(-dt / kHipCatchTauS);
        h.px += (neck.x - h.px) * a;
        h.pz += (neck.z - h.pz) * a;
    }
    {
        const float ex = neck.x - h.px, ez = neck.z - h.pz;
        const float d = std::sqrt(ex * ex + ez * ez);
        if (d > kHipDragM) {
            const float k = (d - kHipDragM) / d;
            h.px += ex * k;
            h.pz += ez * k;
        }
    }
    Vec3 lb = LocalOffsetYaw({ h.px, 0.0f, h.pz }, facingYaw, neck);   // the neck in the body frame: x right, z behind
    if (h.ref - neck.y > kHipSitDropM && lb.z > kHipSitBehindM) {
        h.ref = neck.y;
        h.px = neck.x; h.pz = neck.z;
        lb = { 0.0f, 0.0f, 0.0f };
    }
    const float d = std::sqrt(lb.x * lb.x + lb.z * lb.z);
    const float dc = d < 0.95f * kHipTorsoM ? d : 0.95f * kHipTorsoM;
    const float dip = kHipTorsoM - std::sqrt(kHipTorsoM * kHipTorsoM - dc * dc);
    float resid = (h.ref - neck.y) - dip;
    if (resid < 0.0f) { resid = 0.0f; }
    const Vec3 hw = ToWorldYaw({ -kHipCounterSide * lb.x, 0.0f, -kHipCounterFwd * lb.z + kHipSquatBack * resid }, facingYaw);
    return { h.px + hw.x, h.ref - kHipSquatDrop * resid, h.pz + hw.z };
}

// Hip yield (spec 2026-10-26-roomscale-physical-crouch-design §2.4): a press starting in a hip zone with a body / severed limb under
// the hand goes to the body (Yes); Pending = the severed-limb reading is still due, decided on a later poll; No = the holster fires.
enum class Yield { No, Yes, Pending };

struct Input {
    bool  squeezeActive;  // GetControllerSqueeze(kRightHand) returned true this poll
    float squeeze;        // 0..1, meaningful only when squeezeActive
    bool  poseOk;         // head pose AND right grip pose both valid this poll
    Vec3  local;          // LocalOffset(), meaningful only when poseOk
    Yield yield = Yield::No;   // read only on an in-zone press edge and while Press::Pending (the hips' callers set it)
    // The crossbow's left-shoulder take (plan 2026-11-03-manual-reload-crossbow Task 4): read only on the press edge, through
    // LeftChestFires. Set only for the LEFT chest, LEFT hip and LEFT temple Machines (temple: Ruling T4-a); every other caller
    // leaves both false = today exactly.
    bool  takeLive = false;    // HolsterSetXbowTakeLive: manual reload on AND a crossbow in the right hand
    bool  takeZoneIn = false;  // the LEFT hand inside the mirrored shoulder zone (TakeZoneStep) this poll
};

struct Params {
    Vec3  centre;         // holster_center_x/y/z
    float radius;         // holster_radius (enter); exit = radius * kExitScale
};

struct Step {
    bool  suppressR1;     // do NOT OR R1 this poll - a HOLSTER press is active
    bool  pressHolster;   // grip went down IN the zone this poll
    bool  pressNormal;    // grip went down OUTSIDE the zone this poll
    bool  releaseSwitch;  // a HOLSTER press was released normally this poll -> fire the switch output
    bool  cancelled;      // a HOLSTER press ended because the squeeze action went inactive -> NO switch
    bool  entered;        // zone entered this poll
    bool  exited;         // zone left this poll, including by losing a pose
    bool  inZone;         // zone state after this poll
    float dist;           // |local - centre| in metres, -1 when !poseOk
    bool  yielded;        // this poll an in-zone press went to the body under the hand (Yield::Yes) - Normal, nothing fired
    bool  ownsGrip;       // a HOLSTER press is active (the loot machine must leave this grip alone); false while Pending
};

// Left hand (hip + temple): two Machines share the one left grip. If their EXIT spheres touch, one press could
// start in both zones and fire both taps - holster.cpp refuses the pair by name instead of picking one.
inline bool ZonesOverlap(const Params& a, const Params& b)
{
    const float dx = a.centre.x - b.centre.x;
    const float dy = a.centre.y - b.centre.y;
    const float dz = a.centre.z - b.centre.z;
    const float reach = (a.radius + b.radius) * kExitScale;
    return dx * dx + dy * dy + dz * dz < reach * reach;
}

// ===== THE CROSSBOW'S LEFT-SHOULDER TAKE ZONE (plan 2026-11-03-manual-reload-crossbow Task 4, spec 6b, Ruling T0-b) =====
// The right-shoulder zone mirrored in x for the LEFT hand: holster_center_x negated, the same y / z / radius - no new key.
inline Params MirrorX(const Params& p) { return { { -p.centre.x, p.centre.y, p.centre.z }, p.radius }; }

// The take zone's state after this poll: the shoulder's enter / leave hysteresis (enter inside r, leave past r * kExitScale).
// Not live, or no pose -> false, and the hysteresis starts again (a held "was" is dropped).
inline bool TakeZoneStep(bool live, bool was, bool poseOk, const Vec3& local, const Params& p)
{
    if (!live || !poseOk) { return false; }
    const float dx = local.x - p.centre.x, dy = local.y - p.centre.y, dz = local.z - p.centre.z;
    const float e = was ? p.radius * kExitScale : p.radius;
    return dx * dx + dy * dy + dz * dz <= e * e;
}

// The arbitration (Ruling T0-b): the mirrored shoulder and the left chest's EXIT spheres overlap by 8 mm, so ZonesOverlap reports the
// pair - it is NOT called on it. Instead, while the crossbow take is live and the left hand is in the shoulder zone, the take owns
// the press: the left-chest heal does not fire (and the left-hip pouch, which the Machine gates by the same rule).
inline bool LeftChestFires(bool chestIn, bool xbowTakeLive, bool shoulderIn) { return chestIn && !(xbowTakeLive && shoulderIn); }
// Ruling T4-a (review I1): the left TEMPLE (torch) is gated exactly like the chest - on the disk keys its enter sphere overlaps the
// mirrored shoulder's (0.183 m apart vs 0.25 m summed). The Machine applies the one rule to every Input that carries the take fields.
inline bool LeftTempleFires(bool templeIn, bool xbowTakeLive, bool shoulderIn) { return LeftChestFires(templeIn, xbowTakeLive, shoulderIn); }

// holster_hip_from_calib (Jay 2026-10-15): the hip zones at the BELT, placed from the calibration's down-pose resting hand
// (both arms straight down by the sides), measured in THIS file's frame (raw head, head yaw removed). Jay: "Go about 10-15cm
// UP from the resting hand by side pose in the calibration data. Don't go further forward by more than 4cm". Sideways stays
// where the hand hung. upCm is clamped to 10..15 and fwdCm to 0..4; +Z is BEHIND, so forward DECREASES z.
constexpr float kBeltUpMinCm = 10.0f;
constexpr float kBeltUpMaxCm = 15.0f;
constexpr float kBeltFwdMaxCm = 4.0f;
inline Vec3 BeltCentre(const Vec3& restHand, float upCm, float fwdCm)
{
    const float up = (upCm < kBeltUpMinCm ? kBeltUpMinCm : (upCm > kBeltUpMaxCm ? kBeltUpMaxCm : upCm)) * 0.01f;
    const float fwd = (fwdCm < 0.0f ? 0.0f : (fwdCm > kBeltFwdMaxCm ? kBeltFwdMaxCm : fwdCm)) * 0.01f;
    return { restHand.x, restHand.y + up, restHand.z - fwd };
}

// A resting hand read from dlvr\arm_fit.txt is used only if it is a hand hanging at ITS OWN side: 5-55 cm out on that side,
// 35-120 cm below the eyes, within 35 cm forward / back, every value finite. Anything else is refused by name in holster.cpp
// and the fixed holster_*hip_center_* apply.
inline bool RestHandPlausible(const Vec3& r, bool left)
{
    if (!std::isfinite(r.x) || !std::isfinite(r.y) || !std::isfinite(r.z)) { return false; }
    const float out = left ? -r.x : r.x;
    return out >= 0.05f && out <= 0.55f && r.y >= -1.20f && r.y <= -0.35f && r.z >= -0.35f && r.z <= 0.35f;
}

// Pending: an in-zone press waiting for the severed-limb reading - R1 / L1 withheld, nothing fired, the grip not owned.
enum class Press { Idle, Holster, Normal, Pending };

class Machine {
public:
    Step Update(const Input& in, const Params& p)
    {
        Step st{};

        // Zone (spec §1). Fails CLOSED: a missing pose is "not in the zone", so a holster press can never be
        // created from a pose we do not have.
        const bool wasIn = inZone_;
        if (in.poseOk) {
            const float dx = in.local.x - p.centre.x;
            const float dy = in.local.y - p.centre.y;
            const float dz = in.local.z - p.centre.z;
            st.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            inZone_ = wasIn ? (st.dist <= p.radius * kExitScale) : (st.dist < p.radius);
        } else {
            st.dist = -1.0f;
            inZone_ = false;
        }
        st.entered = !wasIn && inZone_;
        st.exited = wasIn && !inZone_;
        st.inZone = inZone_;

        // Press (spec §2). Decided on the press EDGE and latched for the whole press, wherever the hand goes.
        const bool down = in.squeezeActive && in.squeeze >= kGripThreshold;
        if (!primed_) {
            // First poll ever: a grip already held is a press we did not see start -> NORMAL, never suppressed.
            primed_ = true;
            state_ = down ? Press::Normal : Press::Idle;
        } else if (down && !down_) {
            // LeftChestFires(inZone_, false, x) == inZone_, so with the take fields at their defaults this is today's test.
            const bool fires = LeftChestFires(inZone_, in.takeLive, in.takeZoneIn);
            state_ = !fires ? Press::Normal
                   : in.yield == Yield::Yes ? Press::Normal
                   : in.yield == Yield::Pending ? Press::Pending : Press::Holster;
            st.yielded = fires && in.yield == Yield::Yes;   // a press the take owns did not yield to a body
            st.pressHolster = state_ == Press::Holster;
            st.pressNormal = state_ == Press::Normal;
        } else if (down && state_ == Press::Pending) {
            // the reading the press waited for: a body / limb takes it, nothing fires; else the holster fires now
            if (in.yield == Yield::Yes) {
                state_ = Press::Normal;
                st.yielded = true;
            } else if (in.yield == Yield::No) {
                state_ = Press::Holster;
                st.pressHolster = true;
            }
        } else if (!down && down_) {
            if (state_ == Press::Holster) {
                // A real release reads the squeeze below threshold; an INACTIVE action is not a release.
                if (in.squeezeActive) {
                    st.releaseSwitch = true;
                } else {
                    st.cancelled = true;
                }
            }
            state_ = Press::Idle;
        }
        down_ = down;
        st.suppressR1 = state_ == Press::Holster || state_ == Press::Pending;
        st.ownsGrip = state_ == Press::Holster;
        return st;
    }

    Press state() const { return state_; }

private:
    bool  primed_ = false;
    bool  down_   = false;
    bool  inZone_ = false;
    Press state_  = Press::Idle;
};

// Minimum-gap gate for human-facing side effects (the ENTER pulse, the ENTER/EXIT log lines). Review M1: a grip
// pose flapping behind the shoulder turns every dropout into EXIT + ENTER, i.e. a buzz and two log lines per
// frame. The zone STATE still changes every frame - only what the player feels and what the log prints is gated.
// Blocked events are counted so the log still reports how many there were.
class RateGate {
public:
    bool Allow(unsigned long long nowMs, unsigned long long minGapMs)
    {
        if (!armed_ || nowMs - last_ >= minGapMs) {
            armed_ = true;
            last_ = nowMs;
            return true;
        }
        ++skipped_;
        return false;
    }
    long skipped() const { return skipped_; }
    long TakeSkipped()
    {
        const long n = skipped_;
        skipped_ = 0;
        return n;
    }

private:
    bool armed_ = false;
    unsigned long long last_ = 0;
    long skipped_ = 0;
};

// The weapon-switch output (spec §4, Jay 2026-09-29: "right Dpad tap"; over half a second opens the wheel,
// under counts as a tap). A button held from Start() for durationMs, then released. A second Start() while
// active restarts it from the new time. TakeEnded() reports the release exactly once (for the log).
class TapPulse {
public:
    void Start(unsigned long long nowMs, unsigned long long durationMs)
    {
        until_ = nowMs + durationMs;
        active_ = true;
    }
    bool Active(unsigned long long nowMs) const { return active_ && nowMs < until_; }
    bool TakeEnded(unsigned long long nowMs)
    {
        if (active_ && nowMs >= until_) {
            active_ = false;
            return true;
        }
        return false;
    }

private:
    bool active_ = false;
    unsigned long long until_ = 0;
};

}  // namespace holster
}  // namespace dlvr
