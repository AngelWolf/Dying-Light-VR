// The arm fit, stage 1 - a guided calibration that MEASURES each arm's length and shoulder pivot (SPEC-ARM-FIT-2026-10-02,
// INV-545; Jay 2026-10-12: "Equal percent, go ahead with stage 1"). The result goes to dlvr\arm_fit.txt and the log; stage 2
// (rig_arm_fit, ArmFitScale below, INV-546) reads it and lengthens the drawn arm.
//
// WHY GUIDED (INV-545 (5)): a fit from ordinary play ranged 0.58-0.72 m on the same person; the LABELLED fit (straight-arm
// poses, tools/armcap_shoulder.py on armcap_run1) gave 0.603 / 0.593 m with a residual median 7-13 mm. This reproduces that
// tool in game: the same frame (the wrist target T and the rig's shoulder S relative to the eye anchor, undone by the body
// yaw, the left arm mirrored), the same pose rules (|T-S| >= 0.45 m and the direction within the tool's cones), the same
// balance (at most 150 samples per pose) and the same algebraic sphere fit.
//
// COST: ArmFitWanted() is one volatile read per solve. Only while a calibration runs (about 18 s): a lock and a copy per
// accepted sample (time-throttled to one per 5 ms per arm), and two local-matrix reads per arm per calibration (the bone sum).
#include "armfit.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include "../common/log.h"
#include "calibraw_logic.h"   // build B: RawMatches / ForearmInHand for the raw store
#include "headpose.h"         // GetEyeAboveFloor - the calibration's eye height (plan 2026-11-20-holster-pivot-posture-rings Task 3)

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

namespace dlvr {
namespace {

// Poses 0-3 feed the arm's sphere fit. Poses 4-5 (Jay 2026-10-12: "Yes to both, set it to 0 and build it") are the SHOULDER
// TOUCH, measure-only: a palm flat on top of the opposite shoulder locates the player's REAL shoulder relative to the view, to
// set beside the in-game shoulder - measured from the view as it is (with cam_eye_forward / cam_eye_up_mm) and from the
// character's own eye point (without) - so the offsets' share of "the shoulders look lower than mine" is read, not guessed.
// Pose 4 (2026-10-13, Jay: "Go ahead with 1 and 2"; INV-553): the RELAXED side pose - measure-only, NOT in the sphere fit. In
// play his straight arm at shoulder height reached 95-97% of the stretched side pose's |T - S|, so the anatomical length (S1) is
// taken from this pose instead.
enum Pose { kFwd = 0, kSide = 1, kUp = 2, kDown = 3, kSideRelax = 4, kTouchR = 5, kTouchL = 6, kPoses = 7 };
constexpr int kFitPoses = 4;
const char* const kPoseName[kPoses] = { "forward", "side", "up", "down", "side relaxed", "touch right shoulder",
                                        "touch left shoulder" };
const char* const kPosePrompt[kPoses] = {
    "BOTH ARMS STRAIGHT OUT IN FRONT, elbows locked",
    "BOTH ARMS STRAIGHT OUT TO THE SIDES, elbows locked",
    "BOTH ARMS STRAIGHT UP, elbows locked",
    "BOTH ARMS STRAIGHT DOWN BY YOUR SIDES, elbows locked",
    "BOTH ARMS OUT TO THE SIDES AT SHOULDER HEIGHT - straight but RELAXED, don't push",
    "LEFT HAND FLAT ON TOP OF YOUR RIGHT SHOULDER, right arm relaxed down",
    "RIGHT HAND FLAT ON TOP OF YOUR LEFT SHOULDER, left arm relaxed down",
};
constexpr ULONGLONG kReadyMs = 2500;   // get into the pose
constexpr ULONGLONG kHoldMs  = 2000;   // collect
constexpr ULONGLONG kPoseMs  = kReadyMs + kHoldMs;
constexpr ULONGLONG kTotalMs = kPoseMs * kPoses;
constexpr int       kMinPerPose = 30;  // accepted samples per arm per pose, or no result
constexpr int       kFitPerPose = 150; // armcap_shoulder.py's balance
constexpr float     kMinReach   = 0.45f;

// hv = the arm's direction (S -> T, unit) in the HAND's frame for this sample (hok = the hand basis was given) - on a straight
// arm that is the forearm's direction in the hand frame (2026-10-13, INV-558).
struct Sample { float t[3]; float s[3]; float hv[3]; bool hok; };

// The shoulder touch, per TOUCHED shoulder (0 = right, 1 = left), every vector in that shoulder's own body frame (left mirrored)
// relative to the view (V) and to the character's eye point before the view offsets (E): the touching hand's grip point and the
// touched side's in-game shoulder (the solve's S = the drawn upper-arm origin, INV-547 run 3: 0.0 mm apart).
struct TouchData {
    std::vector<float> gripV[3], gripE[3], shV[3], shE[3];
};

struct ArmData {
    std::vector<Sample> pose[kPoses];
    int   rejHand = 0, rejHeld = 0, rejShort = 0, rejDir = 0;
    ULONGLONG lastAccept = 0;
    float boneSum = -1.0f;
    bool  firstSaid = false;
    int   verdict = 0;            // the menu's live per-arm line: 0 none yet, 1 in the pose, 2 not extended, 3 wrong direction, 4 no hand
    ULONGLONG verdictAt = 0;
};

// Stage 2: the fitted arm lengths in use (pivot -> wrist, metres), from dlvr\arm_fit.txt at the first ArmFitScale call and
// replaced by every completed calibration. Under g_lock.
bool          g_fitLoaded = false;
bool          g_fitHave = false;
float         g_fitLen[2] = {};
float         g_fitPiv[2][3] = {};   // stage 3c: the pivot (fwd, up, OUT) per arm; valid only with g_fitPivHave
bool          g_fitPivHave = false;
float         g_fitAnat[2] = {};     // S1: the ANATOMICAL length per arm (side pose, |T - S| from the in-game shoulder)
bool          g_fitAnatHave = false;
float         g_fitFh[2][3] = {};    // the forearm's direction in the hand frame per arm (INV-558)
bool          g_fitFhHave[2] = { false, false };
// holster_hip_from_calib (Jay 2026-10-15): the down pose's resting GRIP point per arm, holsters' frame (see RawSample::hl),
// from the file's r_/l_rest_hand_*_m lines or the last calibration. A file written before 2026-10-15 has none (recalibrate).
float         g_fitRest[2][3] = {};
bool          g_fitRestHave[2] = { false, false };
char          g_fitWhy[200] = "not read yet";
LONG          g_fitSeq = 0;      // bumped on every change, so ArmFitScale re-logs

SRWLOCK       g_lock = SRWLOCK_INIT;
volatile LONG g_running = 0;      // 1 while collecting
volatile LONG g_bonesWanted[2] = {};
ULONGLONG     g_start = 0;        // GetTickCount64 at start (under g_lock)
LONG          g_run = 0;          // calibration counter, for the log
ArmData       g_arm[2];
TouchData     g_touch[2];
char          g_result1[160] = "Not calibrated yet.";
char          g_result2[200] = "";
// Build A (calibration screen): the UNTIMED step mode. 0 = the timed seven-pose run (ArmFitStart), 1 = step mode.
volatile LONG g_stepMode = 0;
volatile LONG g_stepPose = -1;      // the pose on screen; -1 = none yet
volatile LONG g_stepCollect = 0;    // 1 while the grips are held and the read runs
constexpr int kStepTarget = 150;    // = calibscreen_logic.h kTarget; one pose's read is done at this many per arm
// Per arm: samples NOT stored this read because the OTHER arm was out of the pose (the A3 pause). Reset at each read start;
// reported on the DONE / DISCARDED lines (run 2: the pause was shown but never logged). Under g_lock.
int           g_stepPaused[2] = { 0, 0 };
// Build B: the RAW store (calibraw_logic.h samples from the render thread - the main-menu path, and the in-level run's
// side-by-side). t and hb in the body frame. Under g_lock.
// hl = the GRIP point's offset from the RAW head in the holsters' frame (holster::LocalOffset: head yaw removed, +X right +Y up
// +Z behind) - hlOk false when the caller had none. Only the down pose's are used: holster_hip_from_calib's resting hand.
struct RawSample { float t[3]; float hb[9]; float hl[3]; bool hlOk; };
std::vector<RawSample> g_raw[2][kFitPoses];
int           g_rawVerdict[2] = { 0, 0 };        // as ArmData::verdict: 1 OK, 2 not extended, 3 wrong direction, 4 not tracked
ULONGLONG     g_rawVerdictAt[2] = { 0, 0 };
int           g_rawRejShort[2] = { 0, 0 }, g_rawRejDir[2] = { 0, 0 };
bool          g_rawFirstSaid[2] = { false, false };
volatile LONG g_stepSource = 0;                  // 0 = in-level (+ raw side by side), 1 = raw only (the main menu)
// Build C (Jay 2026-11-15): 1 = this step run is SEATED - pose 3 is "down and out at 45 deg" (MatchesPose / RawMatches case 4) and
// the run measures no rest hand (FinishWith keeps the loaded one, Jay: "Keep previous hips"). 0 = standing / the timed run.
volatile LONG g_stepSeated = 0;
// Plan 2026-11-20-holster-pivot-posture-rings Task 3 (trace 20261120_1230_calib_eye_floor): the eye's height above the STAGE floor at
// each accepted RIGHT-arm step sample of this run (one per accepted pair), and the loaded / last calibration's median + posture.
// Under g_lock.
std::vector<float> g_eyeFloorRun;
float         g_fitEyeFloor = 0.0f;
bool          g_fitEyeFloorHave = false;
bool          g_fitEyeFloorStanding = true;

// The pose's name for the log - pose 3 of a seated step run is the 45 deg pose.
const char* StepPoseName(int p)
{
    if (p == kDown && g_stepSeated) { return "down 45 out (seated)"; }
    return (p >= 0 && p < kPoses) ? kPoseName[p] : "?";
}

// armcap_shoulder.py body(): fwd = v.(-sin, 0, -cos), up = v.y, out = (v.x cos - v.z sin), mirrored for the left arm.
void BodyFrame(const float v[3], float yaw, float sg, float o[3])
{
    const float sby = sinf(yaw), cby = cosf(yaw);
    o[0] = v[0] * -sby + v[2] * -cby;
    o[1] = v[1];
    o[2] = (v[0] * cby + v[2] * -sby) * sg;
}

// INV-548 / Jay 2026-10-12 ("yes fix the side-pose check"): the arm is tested against the pose the prompt ASKS for, within
// ~37 deg (cos 0.80) of its axis, not sorted into four tight cones (cos 0.90 / 0.93) - the cones are taken from the rig's S,
// which sits ~10 cm below the player's shoulder, so a level arm already reads ~10 deg off and a slightly raised left arm
// failed "side" 202 / 204 times. The sphere fit needs extended arms, not exact directions; the reach test still rejects a
// bent arm. Returns 1 = in the pose, -1 = outside its cone, -2 = reach under kMinReach.
// Build C (Jay 2026-11-15, spec section 2 seated set "B"): seated45 = a SEATED step run's 4th pose, arms down and OUT at 45 deg -
// the axis (fwd 0, up -0.7071, out +0.7071), the same cone and reach test.
int MatchesPose(const float tb[3], int want, bool seated45)
{
    const float d = sqrtf(tb[0] * tb[0] + tb[1] * tb[1] + tb[2] * tb[2]);
    if (d < kMinReach) { return -2; }
    float c = 0.0f;
    switch (want) {
        case kFwd:  c = tb[0] / d; break;
        case kSide: c = tb[2] / d; break;
        case kUp:   c = tb[1] / d; break;
        case kDown: c = seated45 ? (tb[2] - tb[1]) * 0.70710678f / d : -tb[1] / d; break;
        case kSideRelax: c = tb[2] / d; break;
        default:    return -1;
    }
    return (c >= 0.80f) ? 1 : -1;
}

int Classify(const float tb[3])
{
    const float d = sqrtf(tb[0] * tb[0] + tb[1] * tb[1] + tb[2] * tb[2]);
    if (d < kMinReach) { return -2; }
    if (tb[0] / d > 0.90f) { return kFwd; }
    if (tb[2] / d > 0.90f) { return kSide; }
    if (tb[1] / d > 0.90f) { return kUp; }
    if (tb[1] / d < -0.93f) { return kDown; }
    return -1;
}

// Algebraic sphere fit: |p|^2 = 2 c.p + k, radius = sqrt(k + |c|^2). Normal equations, Gaussian elimination, partial pivot.
bool SphereFit(const std::vector<const float*>& pts, float c[3], float* r)
{
    double M[4][5] = {};
    for (const float* p : pts) {
        const double a[4] = { 2.0 * p[0], 2.0 * p[1], 2.0 * p[2], 1.0 };
        const double b = static_cast<double>(p[0]) * p[0] + static_cast<double>(p[1]) * p[1] + static_cast<double>(p[2]) * p[2];
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) { M[i][j] += a[i] * a[j]; }
            M[i][4] += a[i] * b;
        }
    }
    for (int i = 0; i < 4; ++i) {
        int piv = i;
        for (int rr = i + 1; rr < 4; ++rr) { if (fabs(M[rr][i]) > fabs(M[piv][i])) { piv = rr; } }
        if (fabs(M[piv][i]) < 1e-12) { return false; }
        if (piv != i) { for (int cc = 0; cc < 5; ++cc) { const double t = M[i][cc]; M[i][cc] = M[piv][cc]; M[piv][cc] = t; } }
        for (int rr = 0; rr < 4; ++rr) {
            if (rr == i) { continue; }
            const double f = M[rr][i] / M[i][i];
            for (int cc = i; cc < 5; ++cc) { M[rr][cc] -= f * M[i][cc]; }
        }
    }
    double x[4];
    for (int i = 0; i < 4; ++i) { x[i] = M[i][4] / M[i][i]; }
    const double r2 = x[3] + x[0] * x[0] + x[1] * x[1] + x[2] * x[2];
    if (!(r2 > 0.0)) { return false; }
    c[0] = static_cast<float>(x[0]); c[1] = static_cast<float>(x[1]); c[2] = static_cast<float>(x[2]);
    *r = static_cast<float>(sqrt(r2));
    return true;
}

float Pct(std::vector<float> v, float p)
{
    if (v.empty()) { return -1.0f; }
    std::sort(v.begin(), v.end());
    size_t i = static_cast<size_t>(p * static_cast<float>(v.size() - 1) + 0.5f);
    if (i >= v.size()) { i = v.size() - 1; }
    return v[i];
}

struct ArmResult {
    bool  ok = false;
    char  why[160] = "";
    float len = 0.0f, c[3] = {}, sMean[3] = {}, resMed = 0.0f, resP90 = 0.0f, boneSum = -1.0f;
    int   used = 0;
    // S1 (2026-10-13): the ANATOMICAL length - the median |T - S| over the SIDE pose, S = the in-game shoulder (at the player's
    // width, INV-548). Out to the side the girdle adds no reach, so this is the arm from the shoulder joint. -1 = not measured.
    float anat = -1.0f, anatP10 = -1.0f, anatP90 = -1.0f;
    // INV-558: the forearm in the hand frame (median of the straight-arm samples' hv, renormalised) and the angular spread.
    float fh[3] = {}; bool fhOk = false; int fhN = 0; float fhMedDeg = -1.0f, fhP90Deg = -1.0f;
};

ArmResult FitArm(int side)
{
    ArmResult R;
    const ArmData& a = g_arm[side];
    for (int p = 0; p < kFitPoses; ++p) {   // the four straight-arm poses only; the touch poses are measured separately
        if (static_cast<int>(a.pose[p].size()) < kMinPerPose) {
            _snprintf_s(R.why, sizeof(R.why), _TRUNCATE, "the %s pose collected %d accepted samples (needs %d)",
                        StepPoseName(p), static_cast<int>(a.pose[p].size()), kMinPerPose);
            return R;
        }
    }
    std::vector<const float*> pts;
    double sm[3] = {};
    for (int p = 0; p < kFitPoses; ++p) {
        const std::vector<Sample>& v = a.pose[p];
        const size_t step = (v.size() / kFitPerPose) > 1 ? (v.size() / kFitPerPose) : 1;
        int taken = 0;
        for (size_t i = 0; i < v.size() && taken < kFitPerPose; i += step, ++taken) {
            pts.push_back(v[i].t);
            for (int k = 0; k < 3; ++k) { sm[k] += v[i].s[k]; }
        }
    }
    R.used = static_cast<int>(pts.size());
    for (int k = 0; k < 3; ++k) { R.sMean[k] = static_cast<float>(sm[k] / static_cast<double>(pts.size())); }
    if (!SphereFit(pts, R.c, &R.len)) {
        _snprintf_s(R.why, sizeof(R.why), _TRUNCATE, "the sphere fit was degenerate over %d samples", R.used);
        return R;
    }
    std::vector<float> res;
    res.reserve(pts.size());
    for (const float* p : pts) {
        const float dx = p[0] - R.c[0], dy = p[1] - R.c[1], dz = p[2] - R.c[2];
        res.push_back(fabsf(sqrtf(dx * dx + dy * dy + dz * dz) - R.len) * 1000.0f);
    }
    R.resMed = Pct(res, 0.5f);
    R.resP90 = Pct(res, 0.9f);
    R.boneSum = a.boneSum;
    // INV-558: the forearm's direction in the hand frame, from every straight-arm (fit-pose) sample that carried a hand basis.
    {
        std::vector<float> c[3];
        for (int p = 0; p < kFitPoses; ++p) {
            for (const Sample& sm : a.pose[p]) {
                if (!sm.hok) { continue; }
                for (int k = 0; k < 3; ++k) { c[k].push_back(sm.hv[k]); }
            }
        }
        R.fhN = static_cast<int>(c[0].size());
        if (R.fhN >= kMinPerPose) {
            float v[3] = { Pct(c[0], 0.5f), Pct(c[1], 0.5f), Pct(c[2], 0.5f) };
            const float vl = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (vl > 0.3f) {
                for (int k = 0; k < 3; ++k) { R.fh[k] = v[k] / vl; }
                std::vector<float> ang;
                ang.reserve(c[0].size());
                for (size_t i = 0; i < c[0].size(); ++i) {
                    float dd = c[0][i] * R.fh[0] + c[1][i] * R.fh[1] + c[2][i] * R.fh[2];
                    if (dd > 1.0f) { dd = 1.0f; }
                    if (dd < -1.0f) { dd = -1.0f; }
                    ang.push_back(acosf(dd) * 57.29578f);
                }
                R.fhMedDeg = Pct(ang, 0.5f);
                R.fhP90Deg = Pct(ang, 0.9f);
                R.fhOk = true;
            }
        }
    }
    // S1's length from the RELAXED side pose (INV-553); fewer than kMinPerPose samples = not measured (-1, the arm keeps the fitted
    // length and the log says RECALIBRATE) - never the stretched side pose as a stand-in.
    if (static_cast<int>(a.pose[kSideRelax].size()) >= kMinPerPose) {
        std::vector<float> an;
        an.reserve(a.pose[kSideRelax].size());
        for (const Sample& sm : a.pose[kSideRelax]) {
            const float dx = sm.t[0] - sm.s[0], dy = sm.t[1] - sm.s[1], dz = sm.t[2] - sm.s[2];
            an.push_back(sqrtf(dx * dx + dy * dy + dz * dz));
        }
        R.anat = Pct(an, 0.5f);
        R.anatP10 = Pct(an, 0.1f);
        R.anatP90 = Pct(an, 0.9f);
    }
    if (R.len < 0.40f || R.len > 0.90f) {
        _snprintf_s(R.why, sizeof(R.why), _TRUNCATE, "the fitted arm length %.3f m is outside 0.40..0.90", R.len);
        return R;
    }
    if (R.resMed > 25.0f) {
        _snprintf_s(R.why, sizeof(R.why), _TRUNCATE,
                    "the fit residual median %.1f mm is over 25 (the poses were not held straight or still)", R.resMed);
        return R;
    }
    R.ok = true;
    return R;
}

// Build B: the RAW store's fit - FitArm's sphere fit, balance and limits over g_raw (t only), then the forearm-in-hand from
// the STAND-IN shoulder (stand-in -> T in each sample's hand frame - the same origin the in-level S -> T uses, B1's mean of the
// in-game S). Changed 2026-10-14 (Jay: "Go ahead with the stand-in origin change") from the fitted PIVOT, which failed the 2 deg
// check by 8.12 / 8.84 deg (B run 2 [CALIBEQ]): S sits 5-11 cm from the pivot (INV-555), turning the direction 5-10 deg.
// No S, no anatomical length, no bone sum (no player at the main menu): sMean = the stand-in (B1), boneSum -1. Caller holds g_lock.
ArmResult FitRaw(int side)
{
    ArmResult R;
    for (int p = 0; p < kFitPoses; ++p) {
        if (static_cast<int>(g_raw[side][p].size()) < kMinPerPose) {
            _snprintf_s(R.why, sizeof(R.why), _TRUNCATE, "the %s pose collected %d raw samples (needs %d)",
                        StepPoseName(p), static_cast<int>(g_raw[side][p].size()), kMinPerPose);
            return R;
        }
    }
    std::vector<const float*> pts;
    for (int p = 0; p < kFitPoses; ++p) {
        const std::vector<RawSample>& v = g_raw[side][p];
        const size_t step = (v.size() / kFitPerPose) > 1 ? (v.size() / kFitPerPose) : 1;
        int taken = 0;
        for (size_t i = 0; i < v.size() && taken < kFitPerPose; i += step, ++taken) { pts.push_back(v[i].t); }
    }
    R.used = static_cast<int>(pts.size());
    for (int k = 0; k < 3; ++k) { R.sMean[k] = calibraw::kStandinS[k]; }
    if (!SphereFit(pts, R.c, &R.len)) {
        _snprintf_s(R.why, sizeof(R.why), _TRUNCATE, "the raw sphere fit was degenerate over %d samples", R.used);
        return R;
    }
    std::vector<float> res;
    res.reserve(pts.size());
    for (const float* p : pts) {
        const float dx = p[0] - R.c[0], dy = p[1] - R.c[1], dz = p[2] - R.c[2];
        res.push_back(fabsf(sqrtf(dx * dx + dy * dy + dz * dz) - R.len) * 1000.0f);
    }
    R.resMed = Pct(res, 0.5f);
    R.resP90 = Pct(res, 0.9f);
    {
        std::vector<float> c[3];
        std::vector<float> hv;
        for (int p = 0; p < kFitPoses; ++p) {
            for (const RawSample& sm : g_raw[side][p]) {
                float v[3];
                if (!calibraw::ForearmInHand(sm.hb, sm.t, calibraw::kStandinS, v)) { continue; }
                for (int k = 0; k < 3; ++k) { c[k].push_back(v[k]); hv.push_back(v[k]); }
            }
        }
        R.fhN = static_cast<int>(c[0].size());
        if (R.fhN >= kMinPerPose) {
            float v[3] = { Pct(c[0], 0.5f), Pct(c[1], 0.5f), Pct(c[2], 0.5f) };
            const float vl = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (vl > 0.3f) {
                for (int k = 0; k < 3; ++k) { R.fh[k] = v[k] / vl; }
                std::vector<float> ang;
                ang.reserve(R.fhN);
                for (int i = 0; i < R.fhN; ++i) {
                    float dd = hv[i * 3] * R.fh[0] + hv[i * 3 + 1] * R.fh[1] + hv[i * 3 + 2] * R.fh[2];
                    if (dd > 1.0f) { dd = 1.0f; }
                    if (dd < -1.0f) { dd = -1.0f; }
                    ang.push_back(acosf(dd) * 57.29578f);
                }
                R.fhMedDeg = Pct(ang, 0.5f);
                R.fhP90Deg = Pct(ang, 0.9f);
                R.fhOk = true;
            }
        }
    }
    if (R.len < 0.40f || R.len > 0.90f) {
        _snprintf_s(R.why, sizeof(R.why), _TRUNCATE, "the raw fitted arm length %.3f m is outside 0.40..0.90", R.len);
        return R;
    }
    if (R.resMed > 25.0f) {
        _snprintf_s(R.why, sizeof(R.why), _TRUNCATE,
                    "the raw fit residual median %.1f mm is over 25 (the poses were not held straight or still)", R.resMed);
        return R;
    }
    R.ok = true;
    return R;
}

bool ArmFitPath(char* out, size_t cch)
{
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&ArmFitPath), &self);
    const DWORD n = GetModuleFileNameA(self, out, static_cast<DWORD>(cch));
    if (n == 0 || n >= cch) { return false; }
    char* slash = strrchr(out, '\\');
    if (!slash) { return false; }
    slash[1] = '\0';
    strcat_s(out, cch, "dlvr\\");
    CreateDirectoryA(out, nullptr);
    strcat_s(out, cch, "arm_fit.txt");
    return true;
}

// Stage 2: read r_len_m / l_len_m from dlvr\arm_fit.txt. Caller holds g_lock exclusively. Both arms or neither.
void LoadFit()
{
    g_fitLoaded = true;
    g_fitHave = false;
    g_fitRestHave[0] = g_fitRestHave[1] = false;
    g_fitEyeFloorHave = false;   // Task 3
    char path[MAX_PATH] = {};
    FILE* f = nullptr;
    if (!ArmFitPath(path, sizeof(path)) || fopen_s(&f, path, "r") != 0 || !f) {
        _snprintf_s(g_fitWhy, sizeof(g_fitWhy), _TRUNCATE, "no calibration file (%s) - calibrate in VR Comfort",
                    path[0] ? path : "dlvr path not built");
        ++g_fitSeq;
        return;
    }
    float len[2] = { -1.0f, -1.0f };
    float piv[2][3] = { { -9.0f, -9.0f, -9.0f }, { -9.0f, -9.0f, -9.0f } };   // -9 = not in the file
    float anat[2] = { -1.0f, -1.0f };                                           // -1 = not in the file
    float rest[2][3] = {};
    int restN[2] = { 0, 0 };   // how many of the three rest-hand lines were read per arm (all three or none)
    float eyeFloor = -1.0f;    // Task 3: eye_floor_m (-1 = not in the file)
    int eyeFloorPosture = -1;  // Task 3: eye_floor_posture (1 standing, 0 seated, -1 not in the file)
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        float v = 0.0f;
        for (int s = 0; s < 2; ++s) {
            static const char* const kRestKey[2][3] = {
                { "r_rest_hand_x_m=%f", "r_rest_hand_y_m=%f", "r_rest_hand_z_m=%f" },
                { "l_rest_hand_x_m=%f", "l_rest_hand_y_m=%f", "l_rest_hand_z_m=%f" } };
            for (int k = 0; k < 3; ++k) {
                if (sscanf_s(line, kRestKey[s][k], &v) == 1) { rest[s][k] = v; ++restN[s]; }
            }
        }
        if (sscanf_s(line, "r_len_m=%f", &v) == 1) { len[0] = v; }
        if (sscanf_s(line, "l_len_m=%f", &v) == 1) { len[1] = v; }
        if (sscanf_s(line, "r_pivot_fwd_m=%f", &v) == 1) { piv[0][0] = v; }
        if (sscanf_s(line, "r_pivot_up_m=%f", &v) == 1)  { piv[0][1] = v; }
        if (sscanf_s(line, "r_pivot_out_m=%f", &v) == 1) { piv[0][2] = v; }
        if (sscanf_s(line, "l_pivot_fwd_m=%f", &v) == 1) { piv[1][0] = v; }
        if (sscanf_s(line, "l_pivot_up_m=%f", &v) == 1)  { piv[1][1] = v; }
        if (sscanf_s(line, "l_pivot_out_m=%f", &v) == 1) { piv[1][2] = v; }
        if (sscanf_s(line, "r_anat_len_m=%f", &v) == 1)  { anat[0] = v; }
        if (sscanf_s(line, "l_anat_len_m=%f", &v) == 1)  { anat[1] = v; }
        if (sscanf_s(line, "eye_floor_m=%f", &v) == 1)   { eyeFloor = v; }   // Task 3
        if (strncmp(line, "eye_floor_posture=seated", 24) == 0)   { eyeFloorPosture = 0; }
        if (strncmp(line, "eye_floor_posture=standing", 26) == 0) { eyeFloorPosture = 1; }
        float fx = 0.0f, fy = 0.0f, fz = 0.0f;
        for (int s = 0; s < 2; ++s) {
            const char* key = s ? "l_forearm_in_hand=%f %f %f" : "r_forearm_in_hand=%f %f %f";
            if (sscanf_s(line, key, &fx, &fy, &fz) == 3) {
                const float fl = sqrtf(fx * fx + fy * fy + fz * fz);
                g_fitFhHave[s] = fl > 0.9f && fl < 1.1f;
                if (g_fitFhHave[s]) { g_fitFh[s][0] = fx / fl; g_fitFh[s][1] = fy / fl; g_fitFh[s][2] = fz / fl; }
            }
        }
    }
    fclose(f);
    // Task 3: both lines or neither (holster_logic.h PostureLinesFrom checks plausibility). Said once per load.
    g_fitEyeFloorHave = eyeFloor > 0.0f && eyeFloorPosture >= 0;
    g_fitEyeFloor = g_fitEyeFloorHave ? eyeFloor : 0.0f;
    g_fitEyeFloorStanding = eyeFloorPosture != 0;
    // [PROBE: ARMFIT] (Task 3 - the loaded calibration's eye height, once per load)
    LOG("boneblend: [ARMFIT] EYE FLOOR loaded: %s%.3f m (%s)%s", g_fitEyeFloorHave ? "" : "NONE in the file - ",
        g_fitEyeFloorHave ? eyeFloor : 0.0f, g_fitEyeFloorHave ? (g_fitEyeFloorStanding ? "calibrated standing" : "calibrated seated") : "-",
        g_fitEyeFloorHave ? "" : " | the holsters use the 1.38 / 1.33 m lines until you recalibrate");
    // holster_hip_from_calib: an arm's resting hand counts only with all three lines (holster.cpp checks it is plausible).
    for (int s = 0; s < 2; ++s) {
        g_fitRestHave[s] = restN[s] == 3;
        for (int k = 0; k < 3; ++k) { g_fitRest[s][k] = rest[s][k]; }
    }
    // S1: both anatomical lengths or neither; each within 0.35..0.85 m. Files written before S1 have none (recalibrate).
    g_fitAnatHave = anat[0] >= 0.35f && anat[0] <= 0.85f && anat[1] >= 0.35f && anat[1] <= 0.85f;
    g_fitAnat[0] = anat[0];
    g_fitAnat[1] = anat[1];
    // Stage 3c: the pivot is usable only if all six numbers were read and each lies within 1 m of the eye anchor.
    g_fitPivHave = true;
    for (int s = 0; s < 2; ++s) {
        for (int k = 0; k < 3; ++k) {
            if (!(piv[s][k] > -1.0f && piv[s][k] < 1.0f)) { g_fitPivHave = false; }
            g_fitPiv[s][k] = piv[s][k];
        }
    }
    for (int s = 0; s < 2; ++s) {
        if (!(len[s] >= 0.40f && len[s] <= 0.90f)) {
            _snprintf_s(g_fitWhy, sizeof(g_fitWhy), _TRUNCATE, "%s %s_len_m is %.4f (missing or outside 0.40..0.90)", path,
                        s ? "l" : "r", len[s]);
            ++g_fitSeq;
            return;
        }
    }
    g_fitLen[0] = len[0];
    g_fitLen[1] = len[1];
    g_fitHave = true;
    _snprintf_s(g_fitWhy, sizeof(g_fitWhy), _TRUNCATE, "from %s", path);
    ++g_fitSeq;
    // The mod menu's status line (ArmFitStatus) shows a SAVED calibration too, not only one completed this session - it read
    // "Not calibrated yet." with a valid file loaded (Jay 2026-10-14, build D run 2). A run this session overwrites it as before.
    _snprintf_s(g_result1, sizeof(g_result1), _TRUNCATE, "Saved calibration: right arm %.3f m, left arm %.3f m.", len[0], len[1]);
    _snprintf_s(g_result2, sizeof(g_result2), _TRUNCATE, "%s", RigArmFit ? "Your arm length is applied (rig_arm_fit on)."
                                                                          : "Not applied: rig_arm_fit is off.");
}

// The shoulder touch's report, per touched shoulder: medians, body frame (fwd, up, OUT; the left mirrored), metres.
// ⚠ The palm rests ON TOP of the shoulder: it sits above the joint centre by palm + flesh + bone, an amount NOT measured here -
// the UP difference carries it; FWD and OUT do not (much). The offsets' share (view minus eye point) is exact either way.
void ReportTouch()
{
    for (int t = 0; t < 2; ++t) {
        const TouchData& td = g_touch[t];
        const char* sn = t ? "LEFT" : "RIGHT";
        const int ng = static_cast<int>(td.gripV[0].size()), ns = static_cast<int>(td.shV[0].size());
        if (ng < kMinPerPose || ns < kMinPerPose) {
            // [PROBE: ARMFIT] (the shoulder touch - one line per touched shoulder per calibration, a result or the reason not)
            LOG("boneblend: [ARMFIT] SHOULDER TOUCH %s run %ld: *** NO RESULT *** - %d palm samples, %d in-game shoulder "
                "samples (needs %d each; the palm is rejected while the hand axis is missing or a pin holds it).",
                sn, g_run, ng, ns, kMinPerPose);
            continue;
        }
        float gv[3], ge[3], sv[3], se[3];
        for (int k = 0; k < 3; ++k) {
            gv[k] = Pct(td.gripV[k], 0.5f); ge[k] = Pct(td.gripE[k], 0.5f);
            sv[k] = Pct(td.shV[k], 0.5f);   se[k] = Pct(td.shE[k], 0.5f);
        }
        LOG("boneblend: [ARMFIT] SHOULDER TOUCH %s run %ld (%d palm / %d shoulder samples; fwd, up, OUT in m): YOUR palm on the "
            "shoulder from the view %+.3f %+.3f %+.3f | the IN-GAME shoulder joint from the view %+.3f %+.3f %+.3f -> palm minus "
            "in-game %+.3f %+.3f %+.3f | from the character's eye point WITHOUT the view offsets: palm %+.3f %+.3f %+.3f, in-game "
            "%+.3f %+.3f %+.3f | the view offsets moved the in-game shoulder by %+.3f %+.3f %+.3f relative to the view",
            sn, g_run, ng, ns, gv[0], gv[1], gv[2], sv[0], sv[1], sv[2], gv[0] - sv[0], gv[1] - sv[1], gv[2] - sv[2],
            ge[0], ge[1], ge[2], se[0], se[1], se[2], sv[0] - se[0], sv[1] - se[1], sv[2] - se[2]);
    }
}

bool FinishWith(ArmResult R[2], bool raw);

// Called once, by whichever thread first sees the collection time end (g_running 1 -> 0 under g_lock).
bool Finish()
{
    ReportTouch();   // measure-only; never gates the arm fit below
    ArmResult R[2] = { FitArm(0), FitArm(1) };
    return FinishWith(R, false);
}

// Build B: log + write + apply a result - the timed / in-level run's (raw = false) or the main-menu RAW fit's (raw = true;
// FitRaw). The same RESULT / FOREARM / file block either way. Caller holds g_lock exclusively.
bool FinishWith(ArmResult R[2], bool raw)
{
    const char* sn[2] = { "R", "L" };
    for (int s = 0; s < 2; ++s) {
        const ArmData& a = g_arm[s];
        if (raw) {
            // [PROBE: ARMFIT] (build B - the RAW store's counts, the main-menu path)
            LOG("boneblend: [ARMFIT %s] run %ld collected (RAW, main menu): forward %d, side %d, up %d, %s %d accepted | "
                "rejected while held: reach under %.2f m %d, outside the pose cone %d",
                sn[s], g_run, static_cast<int>(g_raw[s][0].size()), static_cast<int>(g_raw[s][1].size()),
                static_cast<int>(g_raw[s][2].size()), StepPoseName(kDown), static_cast<int>(g_raw[s][3].size()), kMinReach,
                g_rawRejShort[s],
                g_rawRejDir[s]);
        } else {
        // [PROBE: ARMFIT] gate=none (the VR Comfort tab's "Calibrate my arms" button)  site=HEADSET  log-only=no
        LOG("boneblend: [ARMFIT %s] run %ld collected: forward %d, side %d, up %d, %s %d, side relaxed %d accepted | rejected: no "
            "hand axis %d, held by a pin %d, reach under %.2f m %d, outside every pose cone %d | bone sum read %.4f m (-1 = never "
            "read)",
            sn[s], g_run, static_cast<int>(a.pose[0].size()), static_cast<int>(a.pose[1].size()),
            static_cast<int>(a.pose[2].size()), StepPoseName(kDown), static_cast<int>(a.pose[3].size()),
            static_cast<int>(a.pose[kSideRelax].size()), a.rejHand, a.rejHeld, kMinReach, a.rejShort, a.rejDir, a.boneSum);
        }
        if (R[s].ok) {
            const float k = (R[s].boneSum > 0.1f) ? R[s].len / R[s].boneSum : -1.0f;
            LOG("boneblend: [ARMFIT %s] RESULT run %ld: arm (pivot -> wrist) %.4f m | pivot from the eye anchor (fwd, up, OUT) "
                "%+.4f %+.4f %+.4f m | the rig's shoulder S (mean) %+.4f %+.4f %+.4f -> S minus pivot %+.4f %+.4f %+.4f | "
                "residual median %.1f mm, p90 %.1f mm over %d samples | k = arm / bone sum = %.4f (-1 = no bone sum)",
                sn[s], g_run, R[s].len, R[s].c[0], R[s].c[1], R[s].c[2], R[s].sMean[0], R[s].sMean[1], R[s].sMean[2],
                R[s].sMean[0] - R[s].c[0], R[s].sMean[1] - R[s].c[1], R[s].sMean[2] - R[s].c[2],
                R[s].resMed, R[s].resP90, R[s].used, k);
            // [PROBE: ARMFIT] (S1 - the anatomical length from the side pose, logged with every RESULT)
            // [PROBE: ARMFIT] (INV-558 - the forearm's direction in the hand frame, with every RESULT)
            if (R[s].fhOk) {
                LOG("boneblend: [ARMFIT %s] FOREARM IN HAND run %ld: %+.3f %+.3f %+.3f (unit, hand frame) over %d straight-arm samples, "
                    "spread median %.1f deg, p90 %.1f deg. Used for the bow's drawing elbow when rig_bow_elbow_from_hand=1.",
                    sn[s], g_run, R[s].fh[0], R[s].fh[1], R[s].fh[2], R[s].fhN, R[s].fhMedDeg, R[s].fhP90Deg);
            } else {
                LOG("boneblend: [ARMFIT %s] FOREARM IN HAND run %ld: *** NOT MEASURED *** - %d straight-arm samples carried a hand basis "
                    "(needs %d). The bow's drawing elbow keeps the fixed steer.", sn[s], g_run, R[s].fhN, kMinPerPose);
            }
            if (R[s].anat > 0.0f) {
                LOG("boneblend: [ARMFIT %s] ANATOMICAL run %ld: RELAXED side pose |T - S| median %.4f m (p10 %.4f, p90 %.4f) over "
                    "%d samples = %.1f cm %s the fitted arm %.4f m. Used by the arm fit when rig_arm_fit_anatomical=1 (now %d).",
                    sn[s], g_run, R[s].anat, R[s].anatP10, R[s].anatP90, static_cast<int>(g_arm[s].pose[kSideRelax].size()),
                    fabsf(R[s].anat - R[s].len) * 100.0f, R[s].anat < R[s].len ? "SHORTER than" : "LONGER than", R[s].len,
                    RigArmFitAnatomical ? 1 : 0);
            } else {
                LOG("boneblend: [ARMFIT %s] ANATOMICAL run %ld: *** NOT MEASURED *** - the relaxed side pose collected %d accepted "
                    "samples (needs %d). The file gets no anatomical length; with rig_arm_fit_anatomical=1 the arm keeps the fitted "
                    "length (logged as RECALIBRATE).", sn[s], g_run, static_cast<int>(g_arm[s].pose[kSideRelax].size()),
                    kMinPerPose);
            }
        } else {
            LOG("boneblend: [ARMFIT %s] *** NO RESULT run %ld *** - %s. dlvr\\arm_fit.txt is NOT written by this run.",
                sn[s], g_run, R[s].why);
        }
    }
    // holster_hip_from_calib (Jay 2026-10-15): the resting hand = the median GRIP point of the DOWN pose's accepted raw samples
    // (calibscreen.cpp feeds the raw store on BOTH paths - the main menu and in level), in the holsters' frame. The timed
    // research run has no raw store, so it measures none. Written to the file only with the rest of a complete result.
    float restHand[2][3] = {};
    bool restOk[2] = { false, false };
    // Build C (Jay 2026-11-15: "Keep previous hips"): a SEATED run has no straight-down pose, so it measures no rest hand - each
    // arm KEEPS the loaded calibration's (the file's lines, or the last calibration this launch). None loaded = NOT MEASURED, the
    // fixed hip centre as before. Kept values are written back below, so the next seated run keeps them again.
    const bool seated = g_stepSeated != 0;
    if (seated) {
        if (!g_fitLoaded) { LoadFit(); }   // caller holds g_lock exclusively (LoadFit's contract)
        for (int s = 0; s < 2; ++s) {
            if (g_fitRestHave[s]) {
                for (int k = 0; k < 3; ++k) { restHand[s][k] = g_fitRest[s][k]; }
                restOk[s] = true;
                // [PROBE: ARMFIT] (Build C - the seated run's kept rest hand, one line per arm per calibration)
                LOG("boneblend: [ARMFIT %s] REST HAND run %ld: SEATED run - not measured; KEPT from the loaded calibration "
                    "%+.3f %+.3f %+.3f m (+X right, +Y up, +Z behind). Places the hip holster when holster_hip_from_calib=1.",
                    sn[s], g_run, restHand[s][0], restHand[s][1], restHand[s][2]);
            } else {
                LOG("boneblend: [ARMFIT %s] REST HAND run %ld: *** NOT MEASURED *** - SEATED run and the loaded calibration has "
                    "no rest hand to keep. The file gets no rest hand; holster_hip_from_calib keeps the fixed hip centre for this "
                    "side.", sn[s], g_run);
            }
        }
    }
    for (int s = 0; s < 2 && !seated; ++s) {
        std::vector<float> c[3];
        for (const RawSample& sm : g_raw[s][kDown]) {
            if (!sm.hlOk) { continue; }
            for (int k = 0; k < 3; ++k) { c[k].push_back(sm.hl[k]); }
        }
        const int n = static_cast<int>(c[0].size());
        if (n >= kMinPerPose) {
            for (int k = 0; k < 3; ++k) { restHand[s][k] = Pct(c[k], 0.5f); }
            restOk[s] = true;
            // [PROBE: ARMFIT] (holster_hip_from_calib - the down pose's resting hand, one line per arm per calibration)
            LOG("boneblend: [ARMFIT %s] REST HAND run %ld: the down pose's grip point from the RAW head, head yaw removed "
                "(+X right, +Y up, +Z behind) %+.3f %+.3f %+.3f m, median of %d samples (x p10..p90 %+.3f..%+.3f, y %+.3f..%+.3f, "
                "z %+.3f..%+.3f). Places the hip holster when holster_hip_from_calib=1.",
                sn[s], g_run, restHand[s][0], restHand[s][1], restHand[s][2], n, Pct(c[0], 0.1f), Pct(c[0], 0.9f),
                Pct(c[1], 0.1f), Pct(c[1], 0.9f), Pct(c[2], 0.1f), Pct(c[2], 0.9f));
        } else {
            LOG("boneblend: [ARMFIT %s] REST HAND run %ld: *** NOT MEASURED *** - the down pose carried %d raw grip samples "
                "(needs %d; the timed research run takes none). The file gets no rest hand; holster_hip_from_calib keeps the "
                "fixed hip centre for this side.", sn[s], g_run, n, kMinPerPose);
        }
    }
    // Task 3 (plan 2026-11-20-holster-pivot-posture-rings, spec §5.2): the eye's height above the STAGE floor in the posture this run
    // was taken in - the median of the accepted right-arm samples, only with >= kMinPerPose readings (no floor = none, said here).
    float eyeFloorMed = 0.0f;
    const bool eyeFloorOk = g_stepMode && static_cast<int>(g_eyeFloorRun.size()) >= kMinPerPose;
    if (eyeFloorOk) {
        std::vector<float> e = g_eyeFloorRun;
        std::nth_element(e.begin(), e.begin() + e.size() / 2, e.end());
        eyeFloorMed = e[e.size() / 2];
    }
    // [PROBE: ARMFIT] (Task 3 - the calibration's eye height above the floor, one line per calibration)
    LOG("boneblend: [ARMFIT] EYE FLOOR run %ld: %s - %s readings %d (needs %d)%s", g_run,
        eyeFloorOk ? "MEASURED" : "*** NOT MEASURED ***", seated ? "seated" : "standing", static_cast<int>(g_eyeFloorRun.size()),
        kMinPerPose, eyeFloorOk ? "" : " - no floor reported while calibrating (or a timed run); the holsters keep the 1.38 / 1.33 m lines");
    if (eyeFloorOk) {
        LOG("boneblend: [ARMFIT] EYE FLOOR run %ld: median %.3f m above the floor (%s) -> the holsters' standing / seated lines are "
            "set from it", g_run, eyeFloorMed, seated ? "seated" : "standing");
    }
    if (R[0].ok && R[1].ok) {
        char path[MAX_PATH] = {};
        FILE* f = nullptr;
        if (ArmFitPath(path, sizeof(path)) && fopen_s(&f, path, "w") == 0 && f) {
            char when[32] = "?";
            const time_t now = time(nullptr);
            struct tm lt = {};
            if (localtime_s(&lt, &now) == 0) { strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &lt); }
            fprintf(f, "# DyingLightVR arm fit - written by the in-game arm calibration.\n"
                       "# A BODY MEASUREMENT of the player on this machine. Read by rig_arm_fit (r_len_m / l_len_m).\n"
                       "# Frame: metres, relative to the eye anchor, body yaw undone, OUT = away from the body's centre.\n"
                       "# EXCEPT *_rest_hand_*_m (the down pose's grip point, for holster_hip_from_calib): from the RAW head,\n"
                       "# head yaw removed, +X right +Y up +Z behind - the holsters' own frame.\n"
                       "# posture = the calibration's pose set (seated: the 4th pose is down and out at 45 deg, the rest hand\n"
                       "# is kept from the calibration before it). Not read back.\n"
                       "when=%s\nposture=%s\n", when, !g_stepMode ? "timed" : (seated ? "seated" : "standing"));
            for (int s = 0; s < 2; ++s) {
                const char* p = s ? "l" : "r";
                fprintf(f, "%s_len_m=%.4f\n%s_pivot_fwd_m=%.4f\n%s_pivot_up_m=%.4f\n%s_pivot_out_m=%.4f\n%s_resid_med_mm=%.1f\n"
                           "%s_bone_sum_m=%.4f\n%s_anat_len_m=%.4f\n",
                        p, R[s].len, p, R[s].c[0], p, R[s].c[1], p, R[s].c[2], p, R[s].resMed, p, R[s].boneSum,
                        p, R[s].anat);
                if (R[s].fhOk) {
                    fprintf(f, "%s_forearm_in_hand=%.4f %.4f %.4f\n", p, R[s].fh[0], R[s].fh[1], R[s].fh[2]);
                }
                if (restOk[s]) {
                    // NOT the eye-anchor body frame above: the RAW head, head yaw removed, +X right +Y up +Z behind.
                    fprintf(f, "%s_rest_hand_x_m=%.4f\n%s_rest_hand_y_m=%.4f\n%s_rest_hand_z_m=%.4f\n", p, restHand[s][0], p,
                            restHand[s][1], p, restHand[s][2]);
                }
            }
            if (eyeFloorOk) {   // Task 3: read back by LoadFit -> ArmFitEyeFloor -> the holsters' posture lines
                fprintf(f, "eye_floor_m=%.4f\neye_floor_posture=%s\n", eyeFloorMed, seated ? "seated" : "standing");
            }
            fclose(f);
            LOG("boneblend: [ARMFIT] run %ld WROTE %s", g_run, path);
        } else {
            LOG("boneblend: [ARMFIT] run %ld *** COULD NOT WRITE *** %s - the result above is in the log only.", g_run,
                path[0] ? path : "(the dlvr path could not be built)");
        }
        // Stage 2: the new lengths apply at once (the solve picks them up through ArmFitScale; g_fitSeq makes it log).
        g_fitLen[0] = R[0].len;
        g_fitLen[1] = R[1].len;
        for (int s = 0; s < 2; ++s) {
            for (int k = 0; k < 3; ++k) { g_fitPiv[s][k] = R[s].c[k]; }
            g_fitAnat[s] = R[s].anat;
            g_fitFhHave[s] = R[s].fhOk;
            for (int k = 0; k < 3; ++k) { g_fitFh[s][k] = R[s].fh[k]; }
            g_fitRestHave[s] = restOk[s];
            for (int k = 0; k < 3; ++k) { g_fitRest[s][k] = restHand[s][k]; }
        }
        g_fitPivHave = true;
        g_fitAnatHave = R[0].anat >= 0.35f && R[0].anat <= 0.85f && R[1].anat >= 0.35f && R[1].anat <= 0.85f;
        g_fitEyeFloorHave = eyeFloorOk;   // Task 3: a calibration without a floor reading drops the old one (it described another run)
        g_fitEyeFloor = eyeFloorMed;
        g_fitEyeFloorStanding = !seated;
        g_fitHave = true;
        g_fitLoaded = true;
        _snprintf_s(g_fitWhy, sizeof(g_fitWhy), _TRUNCATE, "from the calibration just completed (run %ld)", g_run);
        ++g_fitSeq;
        _snprintf_s(g_result1, sizeof(g_result1), _TRUNCATE, "Last calibration: right arm %.3f m, left arm %.3f m.",
                    R[0].len, R[1].len);
        _snprintf_s(g_result2, sizeof(g_result2), _TRUNCATE, "Fit error (median) %.0f / %.0f mm. %s", R[0].resMed,
                    R[1].resMed, RigArmFit ? "Your arm length is applied (rig_arm_fit on)."
                                           : "Not applied: rig_arm_fit is off.");
    } else {
        _snprintf_s(g_result1, sizeof(g_result1), _TRUNCATE, "Calibration did not complete - nothing was saved.");
        _snprintf_s(g_result2, sizeof(g_result2), _TRUNCATE, "%s%s", R[0].ok ? "Left: " : "Right: ",
                    R[0].ok ? R[1].why : R[0].why);
    }
    return R[0].ok && R[1].ok;
}

// Ends the collection if its time is up. Returns the elapsed ms (valid while running).
ULONGLONG Advance()
{
    if (!g_running) { return 0; }
    AcquireSRWLockExclusive(&g_lock);
    ULONGLONG el = 0;
    if (g_running) {
        el = GetTickCount64() - g_start;
        if (!g_stepMode && el >= kTotalMs) {   // a step run (calibration screen) has no timer
            InterlockedExchange(&g_running, 0);
            Finish();
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return el;
}

}  // namespace

// [DEP: rig_arm_fit] required-for=SPEC-ARM-FIT stage 2 - the arm bones at the player's measured length (RigArmFit; Jay
//      2026-10-12: "Yes, set it to 1.0 for calibrated players, go ahead"). On + a valid dlvr\arm_fit.txt: upper arm and
//      forearm scaled by the same k = fitted length / the live bone sum, the hand counter-scaled, the solve's lengths = live
//      x k, rig_reach_scale treated as 1.0. On + no file: nothing changes. tree=INERT (off) / LOADBEARING when on with a
//      calibration (it sets every drawn arm bone's length and the reach cap). HEADSET. Was DEFAULT OFF - Tier C until Jay verifies.
//      SHIPPED ON since 2026-10-12 - Tier A (INV-547: "Looks natural"; "Weapons and bow looked fine"; "Set the new values to ship").
//      Uncalibrated players are unchanged (no dlvr\arm_fit.txt = k 1, the settings' lengths, rig_reach_scale as set).
bool RigArmFit = true;

bool ArmFitScale(int side, float liveUpper, float liveFore, float* k)
{
    *k = 1.0f;
    if (side < 0 || side > 1) { return false; }
    static int  s_state[2] = { -1, -1 };   // 0 no calibration, 1 bones unreadable, 2 applied, 3 applied but clamped
    static LONG s_seq[2] = { -1, -1 };
    static float s_k[2] = {};
    bool  have = false;
    float len = 0.0f;
    LONG  seq = 0;
    char  why[200] = {};
    if (!g_fitLoaded) {
        AcquireSRWLockExclusive(&g_lock);
        if (!g_fitLoaded) { LoadFit(); }
        ReleaseSRWLockExclusive(&g_lock);
    }
    AcquireSRWLockShared(&g_lock);
    have = g_fitHave;
    len = g_fitLen[side];
    seq = g_fitSeq;
    if (s_seq[side] != seq) { strcpy_s(why, sizeof(why), g_fitWhy); }
    // S1 (rig_arm_fit_anatomical): the arm at the ANATOMICAL length when one was measured; the roll supplies the rest.
    const bool anatOn = RigArmFitAnatomical && g_fitAnatHave;
    const float fittedLen = len;
    if (anatOn) { len = g_fitAnat[side]; }
    ReleaseSRWLockShared(&g_lock);
    static int s_anat[2] = { -1, -1 };   // 0 off, 1 wanted but none measured (recalibrate), 2 applied
    const int anatState = !RigArmFitAnatomical ? 0 : (anatOn ? 2 : 1);
    if (have && s_anat[side] != anatState) {
        s_anat[side] = anatState;
        s_seq[side] = -2;   // force the LENGTH line below to re-log with the length now in use
        // [PROBE: ARMFIT] (S1 - which length the arm uses, on every change)
        LOG("boneblend: [ARMFIT %s] S1 LENGTH SOURCE: %s (fitted %.4f m%s).", side ? "L" : "R",
            anatState == 0 ? "the FITTED arm (rig_arm_fit_anatomical=0)"
                           : (anatState == 2 ? "the ANATOMICAL arm (rig_arm_fit_anatomical=1)"
                                             : "*** the FITTED arm - rig_arm_fit_anatomical=1 but this calibration has NO anatomical "
                                               "length (written before S1): RECALIBRATE ***"),
            fittedLen, anatState == 2 ? "" : ", anatomical not used");
    }

    int state = 0;
    float kk = 1.0f;
    const float sum = liveUpper + liveFore;
    if (!have) {
        state = 0;
    } else if (!(liveUpper > 0.10f && liveFore > 0.10f && sum < 1.2f)) {
        state = 1;
    } else {
        kk = len / sum;
        state = 2;
        if (kk < 0.85f) { kk = 0.85f; state = 3; }
        if (kk > 1.25f) { kk = 1.25f; state = 3; }
    }
    const bool changed = s_state[side] != state || s_seq[side] != seq || (state >= 2 && fabsf(kk - s_k[side]) > 0.002f);
    if (changed) {
        s_state[side] = state;
        s_seq[side] = seq;
        s_k[side] = kk;
        const char* sn = side ? "L" : "R";
        // [PROBE: ARMFIT] (stage 2 - what the solve applies; logged on every change of answer, never per solve)
        if (state == 0) {
            LOG("boneblend: [ARMFIT %s] LENGTH NOT APPLIED - rig_arm_fit=1 but %s. The arm keeps the model's length and "
                "rig_reach_scale is used as set.", sn, why[0] ? why : "no calibration");
        } else if (state == 1) {
            LOG("boneblend: [ARMFIT %s] LENGTH NOT APPLIED - the live bone lengths could not be read (upper %.4f, fore %.4f "
                "m). The arm keeps the model's length and rig_reach_scale is used as set.", sn, liveUpper, liveFore);
        } else {
            LOG("boneblend: [ARMFIT %s] LENGTH APPLIED: k %.4f%s = %s %.4f m / live bone sum %.4f m (upper %.4f, fore "
                "%.4f) -> solve lengths %.4f + %.4f m; reach scale 1.00 (rig_reach_scale ignored while calibrated). Source: %s",
                sn, kk, state == 3 ? " (CLAMPED to 0.85..1.25)" : "", anatOn ? "ANATOMICAL" : "fitted", len, sum, liveUpper,
                liveFore, liveUpper * kk, liveFore * kk, why[0] ? why : "unchanged");
        }
    }
    *k = kk;
    return state >= 2;
}

// [DEP: rig_arm_fit_follow_cm] required-for=SPEC-ARM-FIT stage 3a - the shoulder following the reach (RigArmFitFollowCm; Jay
//      2026-10-12: "Follow the reach first"). INV-547: with the arm at his length he still reaches 5-15% past it (the girdle
//      comes forward) and the left's fitted pivot sits 4-7 cm forward of the rig's S. When |T - S| exceeds the calibrated arm
//      (armU + armF) the solve's S moves toward T by the excess, capped at this many cm, and the DRAWN shoulder moves by the same
//      world offset (added to the upper arm's local translation at the four restore sites). tree=INERT at 0 / LOADBEARING when
//      on with rig_arm_fit applied (it moves S and the drawn shoulder). Needs rig_restore_e48_len / rig_restore_left_len (the
//      writes it rides). HEADSET. DEFAULT 0 - Tier C until Jay verifies.
float RigArmFitFollowCm = 0.0f;
// [DEP: rig_arm_fit_straight_cm] required-for=SPEC-ARM-FIT stage 4 - a straight arm at full reach (RigArmFitStraightCm; Jay
//      2026-10-12: "Stage 4"). Baseline run 3 (INV-547, tools/arm_fit_lockout.py): at his full forward stretch the elbow still sits
//      41-93 mm off the shoulder-wrist line - rig_reach_soft eases from 85% and rig_reach_margin_mm stops the arm 5 mm short. With
//      this > 0 AND rig_arm_fit applying a calibration: no ease (the hand 1:1 up to the arm's length), the margin at most 0.5 mm, and
//      over the last N cm before full reach the used reach is blended (smoothstep) to full, so the arm reads straight when the
//      player's does; the hand ends at most N cm beyond the controller there. NOT for the two-handed pin or the bow's string grab
//      (both keep their own reach - INV-507, the bow's lock). tree=INERT at 0 / LOADBEARING when on with a calibration (it sets dc).
//      Not an easing curve "in place of" the arm fit (D-172): the fit stays; this only removes the bend at its end. HEADSET.
//      Was DEFAULT 0 - Tier C until Jay verifies.
//      SHIPPED 4 since 2026-10-13 - Tier A (INV-551 "solid"; with the shoulder roll on, Jay tried 1 / 3 / 4 live: "4 has the least
//      snap"; "Ship the roll at 8 and the band at 4"). Rigging tab, group "Arm fit".
float RigArmFitStraightCm = 4.0f;

namespace {
volatile LONG      g_shSeq[2] = {};
volatile LONG      g_shOn[2] = {};
float              g_shD[2][3] = {};
volatile LONG64    g_shStamp[2] = {};
}  // namespace

void ArmFitShoulderPublish(int side, const float local3[3], float moveM, float capM)
{
    if (side < 0 || side > 1) { return; }
    const bool on = moveM > 1e-4f;
    if (!on && !g_shOn[side]) { return; }   // already off: nothing to publish
    InterlockedIncrement(&g_shSeq[side]);   // odd: writing
    for (int k = 0; k < 3; ++k) { g_shD[side][k] = on ? local3[k] : 0.0f; }
    InterlockedExchange64(&g_shStamp[side], static_cast<LONG64>(GetTickCount64()));
    InterlockedExchange(&g_shOn[side], on ? 1 : 0);
    InterlockedIncrement(&g_shSeq[side]);   // even: done
    // The log: the first engagement per arm, then each new maximum in 1 cm steps - bounded (cap / 1 cm lines), never per solve.
    static float s_max[2] = {};
    static bool  s_first[2] = {};
    if (on && (!s_first[side] || moveM > s_max[side] + 0.01f)) {
        const bool first = !s_first[side];
        s_first[side] = true;
        if (moveM > s_max[side]) { s_max[side] = moveM; }
        // [PROBE: ARMFIT] (stage 3a - the shoulder follow; first engagement and each new 1 cm maximum, per arm)
        LOG("boneblend: [ARMFIT %s] SHOULDER FOLLOW %s: the shoulder moved %.1f mm toward the hand (cap %.0f mm) - local "
            "offset %+.4f %+.4f %+.4f m added to the upper arm's translation",
            side ? "L" : "R", first ? "FIRST ENGAGED" : "new maximum", moveM * 1000.0f, capM * 1000.0f, local3[0],
            local3[1], local3[2]);
    }
}

bool ArmFitShoulderDelta(int side, float out3[3])
{
    if (side < 0 || side > 1 || !g_shOn[side]) { return false; }
    for (int tries = 0; tries < 4; ++tries) {
        const LONG s0 = g_shSeq[side];
        if (s0 & 1) { continue; }
        const float a = g_shD[side][0], b = g_shD[side][1], c = g_shD[side][2];
        const LONG64 st = g_shStamp[side];
        if (g_shSeq[side] != s0) { continue; }
        if (static_cast<LONG64>(GetTickCount64()) - st > 200) { return false; }   // the solve stopped publishing
        out3[0] = a; out3[1] = b; out3[2] = c;
        return true;
    }
    return false;
}

// [DEP: rig_arm_fit_shoulder_cm] required-for=SPEC-ARM-FIT stage 3c - the shoulder ROLLING with the reach (RigArmFitShoulderCm;
//      Jay 2026-10-13: "We'll try B. That would be the most natural in terms of real movement", "Go with ii & 0.5"). The snap at
//      full reach (INV-551 add. 2) is the rig's reach running ahead of the player's because the rig's S is not at his pivot
//      (straight_run1: S 25-34 mm behind, 51-70 mm below, 77-100 mm outboard of the fitted pivot). With this > 0 AND rig_arm_fit
//      applying a calibration (with a pivot): the clavicle is held at the fixed melee pose (rig_clavicle_hold's machinery and flag
//      handshake - so rig_clavicle_hold must be on) and rotated about its own origin, toward T, by the angle that moves S by
//      w(e) x (|T - S| - |T - pivot|), w = smoothstep from e 0.5 to 1.0 (e = |T - pivot| / fitted length; VRArmIK starts at half
//      reach), capped at this many cm and 33 deg (VRArmIK's limit). Never backwards: side and hanging reaches (the rig behind his)
//      get 0. Off while a bow is held (INV-544, verified arms) and for an arm the two-handed pin / bow string places.
//      tree=INERT at 0 / LOADBEARING when on with a calibration (it sets the clavicle, S and the drawn shoulder). Needs
//      rig_clavicle_hold, rig_arm_fit, a dlvr\arm_fit.txt with pivots. HEADSET. Was DEFAULT 0 - Tier C until Jay verifies.
//      SHIPPED 8 since 2026-10-13 - Tier A (INV-552: "Shoulder movement seemed natural", "Bow/guns/two hand grip/ melee all felt
//      fine"; "Ship the roll at 8 and the band at 4"). Rigging tab, group "Arm fit". Uncalibrated players: nothing changes (no
//      pivot = the roll and its no-gun clavicle hold stay off).
//      2026-10-13 (A): the no-gun clavicle HOLD stays on while a bow is out (only the roll's rotation stands down) - releasing it on
//      the swap to the bow snapped the right shoulder 70-102 mm (bowhand_run2).
float RigArmFitShoulderCm = 8.0f;

// [DEP: rig_arm_fit_anatomical] required-for=S1 - the arm at the player's ANATOMICAL length (RigArmFitAnatomical; Jay 2026-10-13:
//      "Go ahead" on S1). Out to the side the arm was 5.5-7.6 cm too long (INV-552 side bend; len_by_dir: |T - S| at his full side
//      stretch 0.902-0.907 R / 0.870 L of the fitted arm): the fitted length is the reach around an effective pivot that includes
//      the girdle. With this on AND a calibration that measured it (the side pose's median |T - S|, written as *_anat_len_m): the
//      arm bones are scaled to that length, and the shoulder roll covers the rest forward / up (its target becomes |T - P| x
//      anatomical / fitted) for EVERY arm - the bow's and the pinned one too, which would otherwise fall short. No anatomical
//      length (older file) = the fitted arm, logged. tree=INERT off / LOADBEARING on with a calibration (the arm's length + the
//      roll's target). Needs rig_arm_fit, a recalibration, rig_arm_fit_shoulder_cm > 0 (else forward reaches fall short).
//      HEADSET. DEFAULT OFF - Tier C until Jay verifies.
bool RigArmFitAnatomical = false;
// [DEP: rig_arm_fit_shoulder_max_deg] required-for=S1 - the shoulder roll's clavicle angle limit (RigArmFitShoulderMaxDeg). 33 =
//      VRArmIK's limit (stage 3c as verified, INV-552). S1 replay on straight_run1 (s1_replay.py): at 33 the LEFT forward / up roll
//      is LIMITED 68-100% (the arm straight at his ~0.95); at 45 limited 0% both arms. Clamped 10..60 at use. tree=LOADBEARING with
//      the roll on (it bounds the clavicle's rotation). HEADSET. DEFAULT 33 (= the verified stage 3c behaviour).
float RigArmFitShoulderMaxDeg = 33.0f;

bool ArmFitForearmInHand(int side, float v[3])
{
    if (side < 0 || side > 1) { return false; }
    if (!g_fitLoaded) {
        AcquireSRWLockExclusive(&g_lock);
        if (!g_fitLoaded) { LoadFit(); }
        ReleaseSRWLockExclusive(&g_lock);
    }
    AcquireSRWLockShared(&g_lock);
    const bool ok = g_fitFhHave[side];
    if (ok) { for (int k = 0; k < 3; ++k) { v[k] = g_fitFh[side][k]; } }
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

bool ArmFitPivot(int side, float pivotBody[3], float* lenM, float* rigLenM)
{
    if (side < 0 || side > 1 || !RigArmFit) { return false; }
    if (!g_fitLoaded) {
        AcquireSRWLockExclusive(&g_lock);
        if (!g_fitLoaded) { LoadFit(); }
        ReleaseSRWLockExclusive(&g_lock);
    }
    AcquireSRWLockShared(&g_lock);
    const bool ok = g_fitHave && g_fitPivHave;
    if (ok) {
        for (int k = 0; k < 3; ++k) { pivotBody[k] = g_fitPiv[side][k]; }
        *lenM = g_fitLen[side];
        *rigLenM = (RigArmFitAnatomical && g_fitAnatHave) ? g_fitAnat[side] : g_fitLen[side];
    }
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

void ArmFitRollNote(int side, float wantM, float gotM, float deg, float ext, float capM, bool limited)
{
    if (side < 0 || side > 1) { return; }
    // Bounded: the first engagement per arm, then each new maximum in 1 cm steps (cap / 1 cm lines), never per solve.
    static float s_max[2] = {};
    static bool  s_first[2] = {};
    static volatile LONG s_evalSaid[2] = {};
    if (!InterlockedExchange(&s_evalSaid[side], 1)) {
        // [PROBE: ARMFIT] (stage 3c - the FIRST evaluation per arm, logged whatever it found, so "on but never engaged" is visible)
        LOG("boneblend: [ARMFIT %s] SHOULDER ROLL FIRST EVALUATION: player's extension %.2f, asked %.1f mm, moved %.1f mm, "
            "clavicle %.1f deg (cap %.0f mm). Below extension 0.50, or with the rig's reach behind the player's, 0 is correct.",
            side ? "L" : "R", ext, wantM * 1000.0f, gotM * 1000.0f, deg, capM * 1000.0f);
    }
    if (gotM > 1e-4f && (!s_first[side] || gotM > s_max[side] + 0.01f)) {
        const bool first = !s_first[side];
        s_first[side] = true;
        if (gotM > s_max[side]) { s_max[side] = gotM; }
        // [PROBE: ARMFIT] (stage 3c - the shoulder roll; first engagement and each new 1 cm maximum, per arm)
        LOG("boneblend: [ARMFIT %s] SHOULDER ROLL %s: the clavicle rolled %.1f deg, the shoulder moved %.1f mm toward the hand "
            "(asked %.1f mm, cap %.0f mm%s) at the player's extension %.2f",
            side ? "L" : "R", first ? "FIRST ENGAGED" : "new maximum", deg, gotM * 1000.0f, wantM * 1000.0f, capM * 1000.0f,
            limited ? ", LIMITED by the clavicle angle limit (rig_arm_fit_shoulder_max_deg) or the geometry" : "", ext);
    }
}

void ArmFitStart()
{
    AcquireSRWLockExclusive(&g_lock);
    for (int s = 0; s < 2; ++s) {
        g_arm[s] = ArmData();
        g_touch[s] = TouchData();
        InterlockedExchange(&g_bonesWanted[s], 1);
        // The timed run feeds no raw store; cleared so its Finish cannot write a PREVIOUS screen run's down-pose resting
        // hand (holster_hip_from_calib) as this run's. Nothing else in the timed run reads g_raw.
        for (int p = 0; p < kFitPoses; ++p) { g_raw[s][p].clear(); }
    }
    g_eyeFloorRun.clear();   // Task 3: the timed run takes no eye heights; never write a previous run's
    ++g_run;
    g_start = GetTickCount64();
    InterlockedExchange(&g_stepSeated, 0);   // Build C: a timed run is never seated; ArmFitStartStep sets it after this
    InterlockedExchange(&g_running, 1);
    LOG("boneblend: [ARMFIT] START run %ld - seven poses (forward, side, up, down for the fit; then the side RELAXED for the "
        "anatomical length; then the palm on the right, then the left shoulder - measured only), %llu ms to get into each then "
        "%llu ms held. "
        "rig_arm_fit=%d: a complete result is %s.", g_run, kReadyMs, kHoldMs, RigArmFit ? 1 : 0,
        RigArmFit ? "applied at once" : "saved but not applied");
    ReleaseSRWLockExclusive(&g_lock);
}

void ArmFitCancel()
{
    AcquireSRWLockExclusive(&g_lock);
    if (g_running) {
        InterlockedExchange(&g_running, 0);
        LOG("boneblend: [ARMFIT] run %ld CANCELLED by the player after %llu ms - nothing written.", g_run,
            GetTickCount64() - g_start);
        _snprintf_s(g_result1, sizeof(g_result1), _TRUNCATE, "Calibration cancelled - nothing was saved.");
        g_result2[0] = '\0';
    }
    InterlockedExchange(&g_stepMode, 0);
    InterlockedExchange(&g_stepPose, -1);
    InterlockedExchange(&g_stepCollect, 0);
    InterlockedExchange(&g_stepSeated, 0);
    ReleaseSRWLockExclusive(&g_lock);
}

void ArmFitStartStep(bool seated)
{
    ArmFitStart();   // resets both arms, the touch data and the bone-sum request; logs START
    AcquireSRWLockExclusive(&g_lock);
    for (int s = 0; s < 2; ++s) {   // build B: the raw store starts empty with every run
        for (int p = 0; p < kFitPoses; ++p) { g_raw[s][p].clear(); }
        g_rawVerdict[s] = 0; g_rawVerdictAt[s] = 0; g_rawRejShort[s] = 0; g_rawRejDir[s] = 0; g_rawFirstSaid[s] = false;
    }
    g_eyeFloorRun.clear();   // Task 3
    InterlockedExchange(&g_stepPose, -1);
    InterlockedExchange(&g_stepCollect, 0);
    InterlockedExchange(&g_stepSeated, seated ? 1 : 0);   // Build C - before g_stepMode, so no sample sees the wrong posture
    InterlockedExchange(&g_stepMode, 1);
    // [PROBE: ARMFIT] gate=none (the calibration screen)  site=HEADSET  log-only=no
    LOG("boneblend: [ARMFIT] run %ld is a STEP run (calibration screen), %s: the four fit poses, no timer - the START line above "
        "names the timed run's poses; this run collects only forward, side, up, %s, %d accepted samples per arm each.%s",
        g_run, seated ? "SEATED" : "STANDING", seated ? "down 45 out" : "down", kStepTarget,
        seated ? " Seated: no rest hand is measured - the hips keep the loaded calibration's (Jay: \"Keep previous hips\")." : "");
    ReleaseSRWLockExclusive(&g_lock);
}

void ArmFitBeginPose(int pose)
{
    if (pose < 0 || pose >= kFitPoses) { return; }
    InterlockedExchange(&g_stepCollect, 0);
    InterlockedExchange(&g_stepPose, pose);
    LOG("boneblend: [ARMFIT] run %ld STEP pose %d (%s) on screen", g_run, pose, StepPoseName(pose));
}

void ArmFitCollect(int mode)
{
    AcquireSRWLockExclusive(&g_lock);
    const LONG p = g_stepPose;
    if (g_stepMode && p >= 0 && p < kFitPoses) {
        if (mode == 1) {
            g_stepPaused[0] = g_stepPaused[1] = 0;
            InterlockedExchange(&g_stepCollect, 1);
            LOG("boneblend: [ARMFIT] run %ld STEP pose %s READ STARTED (both grips held)", g_run, StepPoseName(p));
        } else if (mode == 0) {
            InterlockedExchange(&g_stepCollect, 0);
            const int dr = static_cast<int>(g_arm[0].pose[p].size()), dl = static_cast<int>(g_arm[1].pose[p].size());
            g_arm[0].pose[p].clear();
            g_arm[1].pose[p].clear();
            const int rr = static_cast<int>(g_raw[0][p].size()), rl = static_cast<int>(g_raw[1][p].size());
            g_raw[0][p].clear();
            g_raw[1][p].clear();
            LOG("boneblend: [ARMFIT] run %ld STEP pose %s DISCARDED (grips released) - %d right / %d left samples dropped "
                "(raw %d / %d) | paused (the other arm out of the pose): right %d, left %d solves", g_run, StepPoseName(p), dr, dl,
                rr, rl, g_stepPaused[0], g_stepPaused[1]);
        } else {
            InterlockedExchange(&g_stepCollect, 0);
            LOG("boneblend: [ARMFIT] run %ld STEP pose %s DONE - right %d, left %d accepted (raw %d / %d) | paused (the other arm "
                "out of the pose): right %d, left %d solves", g_run, StepPoseName(p), static_cast<int>(g_arm[0].pose[p].size()),
                static_cast<int>(g_arm[1].pose[p].size()), static_cast<int>(g_raw[0][p].size()),
                static_cast<int>(g_raw[1][p].size()), g_stepPaused[0], g_stepPaused[1]);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void ArmFitPoseProgress(int* nR, int* nL, int* vR, int* vL)
{
    *nR = *nL = *vR = *vL = 0;
    AcquireSRWLockShared(&g_lock);
    const LONG p = g_stepPose;
    const ULONGLONG now = GetTickCount64();
    if (g_stepSource == 1) {   // build B, the main menu: the RAW store and the RAW verdicts (no solve runs there)
        if (g_stepMode && p >= 0 && p < kFitPoses) {
            *nR = static_cast<int>(g_raw[0][p].size());
            *nL = static_cast<int>(g_raw[1][p].size());
        }
        if (g_rawVerdictAt[0] && now - g_rawVerdictAt[0] < 300) { *vR = g_rawVerdict[0]; }
        if (g_rawVerdictAt[1] && now - g_rawVerdictAt[1] < 300) { *vL = g_rawVerdict[1]; }
        ReleaseSRWLockShared(&g_lock);
        return;
    }
    if (g_stepMode && p >= 0 && p < kFitPoses) {
        *nR = static_cast<int>(g_arm[0].pose[p].size());
        *nL = static_cast<int>(g_arm[1].pose[p].size());
    }
    if (g_arm[0].verdictAt && now - g_arm[0].verdictAt < 300) { *vR = g_arm[0].verdict; }
    if (g_arm[1].verdictAt && now - g_arm[1].verdictAt < 300) { *vL = g_arm[1].verdict; }
    ReleaseSRWLockShared(&g_lock);
}

void ArmFitSetStepSource(int source)
{
    InterlockedExchange(&g_stepSource, source == 1 ? 1 : 0);
    LOG("boneblend: [ARMFIT] step source = %s", source == 1 ? "RAW only (the main menu - no solve runs)"
                                                            : "IN-LEVEL (the solve) + RAW side by side (the CALIBEQ lines at the end)");
}

void ArmFitNoteRaw(int side, const float T[3], const float handB[9], const float* holsterLocal)
{
    if (side < 0 || side > 1 || !g_stepMode || !g_running) { return; }
    const LONG want = g_stepPose;
    if (want < 0 || want >= kFitPoses) { return; }
    // Task 3: read BEFORE g_lock (GetEyeAboveFloor enters the frame Guard; g_lock is never held around it). Right arm only.
    float eyeFloorM = 0.0f;
    const bool eyeFloorOk = side == 0 && GetEyeAboveFloor(&eyeFloorM);
    AcquireSRWLockExclusive(&g_lock);
    const ULONGLONG now = GetTickCount64();
    const int m = calibraw::RawMatches(T, (want == kDown && g_stepSeated) ? 4 : static_cast<int>(want));   // Build C: 4 = seated 45
    g_rawVerdict[side] = (m == 1) ? 1 : (m == -2 ? 2 : 3);
    g_rawVerdictAt[side] = now;
    if (!g_rawFirstSaid[side]) {
        g_rawFirstSaid[side] = true;
        // [PROBE: ARMFIT] (build B - the first RAW sample of each arm, logged whatever it is)
        LOG("boneblend: [ARMFIT %s] FIRST RAW SAMPLE run %ld (pose %s): T body frame %+.3f %+.3f %+.3f | vs the stand-in shoulder "
            "%+.3f %+.3f %+.3f -> %s (1 = in the pose, -1 wrong direction, -2 reach under %.2f m)",
            side ? "L" : "R", g_run, StepPoseName(want), T[0], T[1], T[2], calibraw::kStandinS[0], calibraw::kStandinS[1],
            calibraw::kStandinS[2], m == 1 ? "1" : (m == -2 ? "-2" : "-1"), kMinReach);
    }
    if (g_stepCollect) {
        const int o = side ^ 1;
        const bool otherOk = g_rawVerdict[o] == 1 && g_rawVerdictAt[o] && now - g_rawVerdictAt[o] < 300;
        std::vector<RawSample>& v = g_raw[side][want];
        if (m == 1 && otherOk && static_cast<int>(v.size()) < kStepTarget) {
            RawSample sm;
            for (int k = 0; k < 3; ++k) { sm.t[k] = T[k]; }
            for (int k = 0; k < 9; ++k) { sm.hb[k] = handB[k]; }
            sm.hlOk = holsterLocal != nullptr;
            for (int k = 0; k < 3; ++k) { sm.hl[k] = sm.hlOk ? holsterLocal[k] : 0.0f; }
            v.push_back(sm);
            if (eyeFloorOk) { g_eyeFloorRun.push_back(eyeFloorM); }   // Task 3
        } else if (m == -2) {
            ++g_rawRejShort[side];
        } else if (m != 1) {
            ++g_rawRejDir[side];
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void ArmFitNoteRawUntracked(int side)
{
    if (side < 0 || side > 1 || !g_stepMode) { return; }
    AcquireSRWLockExclusive(&g_lock);
    g_rawVerdict[side] = 4;   // not tracked (Review Focus 1: never a stale pose)
    g_rawVerdictAt[side] = GetTickCount64();
    ReleaseSRWLockExclusive(&g_lock);
}

bool ArmFitHaveCalibration()
{
    AcquireSRWLockExclusive(&g_lock);
    if (!g_fitLoaded) { LoadFit(); }
    const bool have = g_fitHave;
    ReleaseSRWLockExclusive(&g_lock);
    return have;
}

int ArmFitRestHand(int side, float out[3], long* seq)
{
    if (side < 0 || side > 1) { return 0; }
    if (!g_fitLoaded) {
        AcquireSRWLockExclusive(&g_lock);
        if (!g_fitLoaded) { LoadFit(); }   // once per launch: one small file read
        ReleaseSRWLockExclusive(&g_lock);
    }
    AcquireSRWLockShared(&g_lock);
    int r = 0;
    if (!g_fitHave) {
        r = 0;
    } else if (!g_fitRestHave[side]) {
        r = -1;
    } else {
        for (int k = 0; k < 3; ++k) { out[k] = g_fitRest[side][k]; }
        r = 1;
    }
    if (seq) { *seq = g_fitSeq; }
    ReleaseSRWLockShared(&g_lock);
    return r;
}

bool ArmFitEyeFloor(float* metres, bool* calibratedStanding)
{
    if (!g_fitLoaded) {
        AcquireSRWLockExclusive(&g_lock);
        if (!g_fitLoaded) { LoadFit(); }   // once per launch, as ArmFitRestHand
        ReleaseSRWLockExclusive(&g_lock);
    }
    AcquireSRWLockShared(&g_lock);
    const bool have = g_fitEyeFloorHave;
    if (have) {
        if (metres) { *metres = g_fitEyeFloor; }
        if (calibratedStanding) { *calibratedStanding = g_fitEyeFloorStanding; }
    }
    ReleaseSRWLockShared(&g_lock);
    return have;
}

int ArmFitStepFinish(char* why, size_t n)
{
    AcquireSRWLockExclusive(&g_lock);
    int r = -1;
    if (g_stepMode && g_running) {
        InterlockedExchange(&g_running, 0);
        InterlockedExchange(&g_stepCollect, 0);
        if (g_stepSource == 1) {
            // Build B, the main menu: the RAW fit is the result (the in-level store is empty - no solve ran).
            ArmResult R[2] = { FitRaw(0), FitRaw(1) };
            r = FinishWith(R, true) ? 1 : -1;
        } else {
            // In level: today's result, then the RAW fit of the same holds, side by side (ruling B2: forearm-in-hand
            // within 2 deg - Jay 2026-10-14 "2 degrees is fine").
            ArmResult L[2] = { FitArm(0), FitArm(1) };
            ArmResult W[2] = { FitRaw(0), FitRaw(1) };
            ReportTouch();
            r = FinishWith(L, false) ? 1 : -1;
            for (int s = 0; s < 2; ++s) {
                const char* sn = s ? "L" : "R";
                if (!L[s].ok) {
                    // [PROBE: CALIBEQ] gate=none (an in-level calibration-screen run)  site=HEADSET  log-only=yes
                    LOG("boneblend: [CALIBEQ %s] run %ld: NO COMPARISON - the in-level fit refused (%s)", sn, g_run, L[s].why);
                    continue;
                }
                if (!W[s].ok) {
                    // [PROBE: CALIBEQ] gate=none (an in-level calibration-screen run)  site=HEADSET  log-only=yes
                    LOG("boneblend: [CALIBEQ %s] run %ld: raw: NO FIT (%s) - no comparison", sn, g_run, W[s].why);
                    continue;
                }
                const float dc[3] = { W[s].c[0] - L[s].c[0], W[s].c[1] - L[s].c[1], W[s].c[2] - L[s].c[2] };
                const float dPiv = sqrtf(dc[0] * dc[0] + dc[1] * dc[1] + dc[2] * dc[2]) * 1000.0f;
                float ang = -1.0f;
                if (L[s].fhOk && W[s].fhOk) {
                    float dd = L[s].fh[0] * W[s].fh[0] + L[s].fh[1] * W[s].fh[1] + L[s].fh[2] * W[s].fh[2];
                    if (dd > 1.0f) { dd = 1.0f; }
                    if (dd < -1.0f) { dd = -1.0f; }
                    ang = acosf(dd) * 57.29578f;
                }
                // [PROBE: CALIBEQ] gate=none (an in-level calibration-screen run)  site=HEADSET  log-only=yes
                LOG("boneblend: [CALIBEQ %s] run %ld: in-level len %.4f / raw %.4f (d %+.1f mm) | pivot |d| %.1f mm | "
                    "forearm-in-hand angle %.2f deg (pass <= 2.00, Jay 2026-10-14) -> %s | in-level fh %+.3f %+.3f %+.3f (n %d) "
                    "raw fh %+.3f %+.3f %+.3f (n %d) | fit samples in-level %d, raw %d",
                    sn, g_run, L[s].len, W[s].len, (W[s].len - L[s].len) * 1000.0f, dPiv, ang,
                    ang < 0.0f ? "NO VERDICT (a forearm-in-hand was not measured)" : (ang <= 2.0f ? "PASS" : "FAIL"),
                    L[s].fh[0], L[s].fh[1], L[s].fh[2], L[s].fhN, W[s].fh[0], W[s].fh[1], W[s].fh[2], W[s].fhN, L[s].used,
                    W[s].used);
            }
        }
    }
    InterlockedExchange(&g_stepMode, 0);
    InterlockedExchange(&g_stepPose, -1);
    InterlockedExchange(&g_stepSeated, 0);   // Build C: read by FinishWith above; cleared with the step mode
    _snprintf_s(why, n, _TRUNCATE, "%s %s", g_result1, g_result2);
    ReleaseSRWLockExclusive(&g_lock);
    return r;
}

bool ArmFitWanted() { return g_running != 0; }

bool ArmFitNeedsBones(int side) { return side >= 0 && side < 2 && g_bonesWanted[side] != 0; }

void ArmFitNote(int side, const float anchor[3], const float anchorRaw[3], const float S[3], const float T[3],
                const float wristBack[3], float bodyYaw, bool handOk, bool held, float boneSum, const float* handB)
{
    if (side < 0 || side > 1) { return; }
    const ULONGLONG el = Advance();
    if (!g_running) { return; }
    // Step mode (build A): the screen decides the pose and the hold; no timer. Timed mode: unchanged.
    const bool step = g_stepMode != 0;
    if (step && (g_stepPose < 0 || g_stepPose >= kFitPoses)) { return; }
    const ULONGLONG inPose = step ? (g_stepCollect ? kReadyMs : 0) : el % kPoseMs;
    const int want = step ? static_cast<int>(g_stepPose) : static_cast<int>(el / kPoseMs);
    AcquireSRWLockExclusive(&g_lock);
    ArmData& a = g_arm[side];
    if (boneSum > 0.0f) {
        a.boneSum = boneSum;
        InterlockedExchange(&g_bonesWanted[side], 0);
    }
    // The shoulder touch (poses 4-5): no cone and no reach test - the touching arm is bent across the body by design. The
    // touching hand's GRIP point (T + wristBack = the controller's grip, as the solve built T) and the touched side's S, both in
    // the touched shoulder's own frame, from the view and from the eye point before the offsets.
    if (g_running && (want == kTouchR || want == kTouchL) && inPose >= kReadyMs) {
        const ULONGLONG now = GetTickCount64();
        const int touched = (want == kTouchR) ? 0 : 1;
        const float sg = touched ? -1.0f : 1.0f;
        if (now - a.lastAccept >= 5) {
            TouchData& td = g_touch[touched];
            float o[3];
            if (side != touched) {
                if (!handOk) { ++a.rejHand; }
                else if (held) { ++a.rejHeld; }
                else {
                    const float g[3] = { T[0] + wristBack[0], T[1] + wristBack[1], T[2] + wristBack[2] };
                    const float gv[3] = { g[0] - anchor[0], g[1] - anchor[1], g[2] - anchor[2] };
                    const float ge[3] = { g[0] - anchorRaw[0], g[1] - anchorRaw[1], g[2] - anchorRaw[2] };
                    BodyFrame(gv, bodyYaw, sg, o);
                    for (int k = 0; k < 3; ++k) { td.gripV[k].push_back(o[k]); }
                    BodyFrame(ge, bodyYaw, sg, o);
                    for (int k = 0; k < 3; ++k) { td.gripE[k].push_back(o[k]); }
                    a.lastAccept = now;
                }
            } else {
                const float sv[3] = { S[0] - anchor[0], S[1] - anchor[1], S[2] - anchor[2] };
                const float se[3] = { S[0] - anchorRaw[0], S[1] - anchorRaw[1], S[2] - anchorRaw[2] };
                BodyFrame(sv, bodyYaw, sg, o);
                for (int k = 0; k < 3; ++k) { td.shV[k].push_back(o[k]); }
                BodyFrame(se, bodyYaw, sg, o);
                for (int k = 0; k < 3; ++k) { td.shE[k].push_back(o[k]); }
                a.lastAccept = now;
            }
        }
    }
    // Evaluated through the WHOLE pose window so the menu's per-arm line can say "in the pose / not" while the player is still
    // getting into it; samples are stored, and rejections counted, only during the hold (inPose >= kReadyMs).
    const bool holding = inPose >= kReadyMs;
    if (g_running && (want < kFitPoses || want == kSideRelax)) {
        const ULONGLONG now = GetTickCount64();
        if (now - a.lastAccept >= 5) {
            const float sg = side ? -1.0f : 1.0f;
            const float vt[3] = { T[0] - anchor[0], T[1] - anchor[1], T[2] - anchor[2] };
            const float vs[3] = { S[0] - anchor[0], S[1] - anchor[1], S[2] - anchor[2] };
            Sample sm;
            BodyFrame(vt, bodyYaw, sg, sm.t);
            BodyFrame(vs, bodyYaw, sg, sm.s);
            // INV-558: the arm's direction (S -> T) in the hand's own frame - handB^T . f (handB: columns = axes, row-major).
            sm.hok = false;
            sm.hv[0] = sm.hv[1] = sm.hv[2] = 0.0f;
            if (handB && handOk) {
                float fw[3] = { T[0] - S[0], T[1] - S[1], T[2] - S[2] };
                const float fl = sqrtf(fw[0] * fw[0] + fw[1] * fw[1] + fw[2] * fw[2]);
                if (fl > 1e-4f) {
                    for (int k = 0; k < 3; ++k) { fw[k] /= fl; }
                    for (int c = 0; c < 3; ++c) { sm.hv[c] = handB[c] * fw[0] + handB[3 + c] * fw[1] + handB[6 + c] * fw[2]; }
                    sm.hok = std::isfinite(sm.hv[0]) && std::isfinite(sm.hv[1]) && std::isfinite(sm.hv[2]);
                }
            }
            const float rel[3] = { sm.t[0] - sm.s[0], sm.t[1] - sm.s[1], sm.t[2] - sm.s[2] };
            const int cls = Classify(rel);          // logged on the first sample only (the old sort, for comparison)
            const int fit = MatchesPose(rel, want, step && g_stepSeated != 0); // the acceptance test (Build C: seated 45)
            if (!a.firstSaid) {
                a.firstSaid = true;
                // [PROBE: ARMFIT] (the first sample of each arm, logged whatever it is - the header is in Finish)
                LOG("boneblend: [ARMFIT %s] FIRST SAMPLE run %ld (pose %s): T-S body frame %+.3f %+.3f %+.3f (|%.3f| m), "
                    "class %d (-2 = reach under %.2f, -1 = no pose cone), hand axis %d, held %d",
                    side ? "L" : "R", g_run, StepPoseName(want), rel[0], rel[1], rel[2],
                    sqrtf(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]), cls, kMinReach, handOk ? 1 : 0,
                    held ? 1 : 0);
            }
            if (!handOk || held) {
                if (holding) { if (!handOk) { ++a.rejHand; } else { ++a.rejHeld; } }
                a.verdict = 4;
            } else if (fit == -2) {
                if (holding) { ++a.rejShort; }
                a.verdict = 2;
            } else if (fit != 1) {
                if (holding) { ++a.rejDir; }
                a.verdict = 3;
            } else {
                // Step mode (A3): store only while the OTHER arm is in the pose too (fresh verdict OK) - an arm out of the
                // pose PAUSES the read for both - and stop at kStepTarget. Timed mode: as before.
                const ArmData& o = g_arm[side ^ 1];
                const bool otherOk = !step || (o.verdict == 1 && o.verdictAt && now - o.verdictAt < 300);
                const bool room = !step || static_cast<int>(a.pose[want].size()) < kStepTarget;
                if (holding && otherOk && room) {
                    a.pose[want].push_back(sm);
                    a.lastAccept = now;
                } else if (holding && !otherOk && room) {
                    ++g_stepPaused[side];
                }
                a.verdict = 1;
            }
            a.verdictAt = now;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

bool ArmFitStatus(char* line1, size_t n1, char* line2, size_t n2)
{
    // Load the saved file once if nothing has yet (calib_screen_auto=0 leaves it to the rig's first use), so the status line
    // below can name it. COST: one unlocked bool read per menu frame after the first load (a stale false = one extra locked call).
    if (!g_fitLoaded) { ArmFitHaveCalibration(); }
    const ULONGLONG el = Advance();
    AcquireSRWLockShared(&g_lock);
    const bool running = g_running != 0;
    if (running) {
        const int p = static_cast<int>(el / kPoseMs);
        const ULONGLONG inPose = el % kPoseMs;
        const int pp = p < kPoses ? p : kPoses - 1;
        _snprintf_s(line1, n1, _TRUNCATE, "%d of %d: %s", pp + 1, static_cast<int>(kPoses), kPosePrompt[pp]);
        // Per-arm live verdict for the four fitted poses (INV-548: the left's "side" was failing with nothing to say why).
        char vv[2][40] = { "", "" };
        const bool armPose = pp < kFitPoses || pp == kSideRelax;   // the poses with a per-arm verdict
        if (armPose) {
            const ULONGLONG nowS = GetTickCount64();
            static const char* const kV[5] = { "--", "OK", "not fully extended", "wrong direction", "not tracked" };
            for (int s = 0; s < 2; ++s) {
                const int v = (g_arm[s].verdictAt && nowS - g_arm[s].verdictAt < 300) ? g_arm[s].verdict : 0;
                _snprintf_s(vv[s], sizeof(vv[s]), _TRUNCATE, "%s", kV[v >= 0 && v <= 4 ? v : 0]);
            }
        }
        if (inPose < kReadyMs) {
            if (armPose) {
                _snprintf_s(line2, n2, _TRUNCATE, "Get into position... %.1f s   right arm: %s | left arm: %s",
                            static_cast<double>(kReadyMs - inPose) / 1000.0, vv[0], vv[1]);
            } else {
                _snprintf_s(line2, n2, _TRUNCATE, "Get into position... %.1f s", static_cast<double>(kReadyMs - inPose) / 1000.0);
            }
        } else if (armPose) {
            _snprintf_s(line2, n2, _TRUNCATE, "HOLD STILL... %.1f s   right arm: %s (%d) | left arm: %s (%d)",
                        static_cast<double>(kPoseMs - inPose) / 1000.0, vv[0], static_cast<int>(g_arm[0].pose[pp].size()),
                        vv[1], static_cast<int>(g_arm[1].pose[pp].size()));
        } else {
            _snprintf_s(line2, n2, _TRUNCATE, "HOLD STILL... %.1f s", static_cast<double>(kPoseMs - inPose) / 1000.0);
        }
    } else {
        _snprintf_s(line1, n1, _TRUNCATE, "%s", g_result1);
        _snprintf_s(line2, n2, _TRUNCATE, "%s", g_result2);
    }
    ReleaseSRWLockShared(&g_lock);
    return running;
}

}  // namespace dlvr
