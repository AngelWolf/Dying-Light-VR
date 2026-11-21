// Pure logic: the per-player neck pivot learned in ordinary play - spec docs/superpowers/specs/2026-11-20-holster-pivot-posture-rings-design.md
// §5.1, plan docs/superpowers/plans/2026-11-20-holster-pivot-posture-rings.md Task 1. No OpenXR, no Windows, no logging, so
// src/src/tests/pivotfit_logic_test.cpp compiles it on its own. holster.cpp owns the sampling, the latch shift and the file.
//
// The head's eye midpoint is P + R n (P the neck pivot, fixed while the body is still; n the pivot -> eye offset in the head frame).
// Over a 3 s window of head poses in the REFERENCE frame (appSpace shifts undone - roomscale moves appSpace itself) the least squares
// fit of P and n is INV-321's method; offline on RUNS BC1 / BC2 (backups/20261120_run_bc2/step4_pivot_fit.py) its window medians
// landed within 3-5 mm of Jay's measured n.x / n.z. A window counts only when the head turned about one point (rms < kRmsM) AND it
// observes the component: n.x / n.z need a yaw sweep, n.y a pitch sweep (a yaw-only window cannot see n.y). The estimate is the MEDIAN
// of the accepted windows (single windows are noisy); it replaces the shipped n only after kYawNeed / kPitchNeed windows, never
// outside the plausibility box, in steps of at least kCommitM, at most once per kCommitMinS.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dlvr {
namespace pivotfit {

constexpr double kWindowS    = 3.0;
constexpr double kStepS      = 0.5;
constexpr float  kRmsM       = 0.005f;
constexpr float  kSweepRad   = 20.0f * 0.0174532925f;
constexpr int    kYawNeed    = 40;     // accepted yaw windows before n.x / n.z leave the shipped value
constexpr int    kPitchNeed  = 30;     // accepted pitch windows before n.y does. Spec said 10 (builder's call); RAISED 2026-11-20
                                       // (ledger ruling T1-R2): INV-321's nod run read n.y 66..138 mm over 12 windows and the BC1
                                       // replay committed 131 at 13 - ten nod windows cannot pin n.y. Until 30, n.y stays shipped.
constexpr float  kCommitM    = 0.003f;
constexpr double kCommitMinS = 60.0;
constexpr float  kBoxMin[3]  = { -0.040f, 0.060f, -0.140f };
constexpr float  kBoxMax[3]  = { 0.040f, 0.180f, -0.040f };
constexpr int    kRingCap    = 256;    // > 4 s of samples at the caller's ~30 Hz
constexpr int    kValCap     = 4096;   // accepted values kept per component (oldest overwritten)
constexpr int    kMinSamples = 20;

// One head pose, REFERENCE frame: eye midpoint (m) and the head rotation, row-major 3x3.
struct Sample {
    double t;
    float  eye[3];
    float  R[9];
};

struct WindowFit {
    bool  ok;
    float n[3];
    float rmsM;
    float yawSweepRad;
    float pitchSweepRad;
};

// Yaw (HeadYaw's convention: forward = -Z is 0, left +) and pitch (+ up) of R's forward (-column 2).
inline float YawOf(const float R[9]) { return std::atan2(R[2], R[8]); }
inline float PitchOf(const float R[9])
{
    const float s = -R[5];
    return std::asin(s > 1.0f ? 1.0f : (s < -1.0f ? -1.0f : s));
}

inline bool Solve6(double A[6][7], double out[6])
{
    for (int c = 0; c < 6; ++c) {
        int p = c;
        for (int r = c + 1; r < 6; ++r) { if (std::fabs(A[r][c]) > std::fabs(A[p][c])) { p = r; } }
        if (std::fabs(A[p][c]) < 1e-12) { return false; }
        if (p != c) { for (int k = 0; k < 7; ++k) { std::swap(A[c][k], A[p][k]); } }
        for (int r = 0; r < 6; ++r) {
            if (r == c) { continue; }
            const double f = A[r][c] / A[c][c];
            for (int k = c; k < 7; ++k) { A[r][k] -= f * A[c][k]; }
        }
    }
    for (int i = 0; i < 6; ++i) { out[i] = A[i][6] / A[i][i]; }
    return true;
}

// Least squares eye_i = P + R_i n over the window; a tiny ridge keeps an unobserved direction of n finite (it is never used then).
inline WindowFit FitWindow(const Sample* s, int count)
{
    WindowFit w{ false, { 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f, 0.0f };
    if (!s || count < kMinSamples) { return w; }
    double A[6][7] = {};
    for (int i = 0; i < count; ++i) {
        for (int r = 0; r < 3; ++r) {
            const double a[6] = { r == 0 ? 1.0 : 0.0, r == 1 ? 1.0 : 0.0, r == 2 ? 1.0 : 0.0, s[i].R[3 * r], s[i].R[3 * r + 1], s[i].R[3 * r + 2] };
            for (int j = 0; j < 6; ++j) {
                A[j][6] += a[j] * s[i].eye[r];
                for (int k = 0; k < 6; ++k) { A[j][k] += a[j] * a[k]; }
            }
        }
    }
    for (int j = 3; j < 6; ++j) { A[j][j] += 1e-6 * count; }
    double x[6];
    if (!Solve6(A, x)) { return w; }
    double ss = 0.0;
    for (int i = 0; i < count; ++i) {
        for (int r = 0; r < 3; ++r) {
            const double e = s[i].eye[r] - (x[r] + s[i].R[3 * r] * x[3] + s[i].R[3 * r + 1] * x[4] + s[i].R[3 * r + 2] * x[5]);
            ss += e * e;
        }
    }
    const float y0 = YawOf(s[0].R);
    float yMin = 0.0f, yMax = 0.0f, pMin = 1e9f, pMax = -1e9f;
    for (int i = 0; i < count; ++i) {
        const float d = std::remainder(YawOf(s[i].R) - y0, 6.28318531f);
        yMin = std::min(yMin, d);
        yMax = std::max(yMax, d);
        const float p = PitchOf(s[i].R);
        pMin = std::min(pMin, p);
        pMax = std::max(pMax, p);
    }
    w.ok = true;
    for (int k = 0; k < 3; ++k) { w.n[k] = static_cast<float>(x[3 + k]); }
    w.rmsM = static_cast<float>(std::sqrt(ss / count));
    w.yawSweepRad = yMax - yMin;
    w.pitchSweepRad = pMax - pMin;
    return w;
}

struct State {
    Sample ring[kRingCap];
    int    ringHead = 0, ringCount = 0;
    float  val[3][kValCap];
    int    valCount[3] = { 0, 0, 0 }, valNext[3] = { 0, 0, 0 };
    int    tried = 0, rigid = 0, acceptedYaw = 0, acceptedPitch = 0, refused = 0, commits = 0;
    double nextFitAt = -1.0, lastCommitAt = -1.0;
    float  n[3] = { 0.0f, 0.0f, 0.0f };          // the value in use (shipped until committed)
    float  shipped[3] = { 0.0f, 0.0f, 0.0f };
    WindowFit last{ false, { 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f, 0.0f };   // the last window fitted (for the log)
    float  refusedMedian[3] = { 0.0f, 0.0f, 0.0f };                   // the last out-of-box candidate (for the log)
};

inline void Reset(State& s, const float shipped[3])
{
    s.ringHead = s.ringCount = 0;
    for (int k = 0; k < 3; ++k) { s.valCount[k] = s.valNext[k] = 0; s.n[k] = s.shipped[k] = shipped[k]; }
    s.tried = s.rigid = s.acceptedYaw = s.acceptedPitch = s.refused = s.commits = 0;
    s.nextFitAt = s.lastCommitAt = -1.0;
    s.last = WindowFit{ false, { 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f, 0.0f };
}

inline void PushVal(State& s, int k, float v)
{
    s.val[k][s.valNext[k]] = v;
    s.valNext[k] = (s.valNext[k] + 1) % kValCap;
    if (s.valCount[k] < kValCap) { ++s.valCount[k]; }
}

inline float MedianOf(const State& s, int k)
{
    static thread_local float tmp[kValCap];
    const int c = s.valCount[k];
    std::memcpy(tmp, s.val[k], sizeof(float) * static_cast<size_t>(c));
    std::nth_element(tmp, tmp + c / 2, tmp + c);
    return tmp[c / 2];
}

// out = each component's median of the accepted windows, or the value in use where none was accepted.
inline void Medians(const State& s, float out[3])
{
    for (int k = 0; k < 3; ++k) { out[k] = s.valCount[k] > 0 ? MedianOf(s, k) : s.n[k]; }
}

enum class Event { None, Accepted, Committed, RefusedOutOfBox };

// One sample (the caller decimates to ~30 Hz). Every kStepS the last kWindowS is fitted; see the file header for the rules.
inline Event Step(State& s, const Sample& x)
{
    s.ring[s.ringHead] = x;
    s.ringHead = (s.ringHead + 1) % kRingCap;
    if (s.ringCount < kRingCap) { ++s.ringCount; }
    if (s.nextFitAt < 0.0) { s.nextFitAt = x.t + kWindowS; }
    if (x.t < s.nextFitAt) { return Event::None; }
    s.nextFitAt = x.t + kStepS;
    static thread_local Sample win[kRingCap];
    int c = 0;
    for (int i = s.ringCount; i >= 1; --i) {   // oldest first
        const Sample& y = s.ring[(s.ringHead - i + kRingCap) % kRingCap];
        if (y.t >= x.t - kWindowS) { win[c++] = y; }
    }
    if (c < kMinSamples || win[c - 1].t - win[0].t < 0.8 * kWindowS) { return Event::None; }
    ++s.tried;
    const WindowFit w = FitWindow(win, c);
    s.last = w;
    if (!w.ok || w.rmsM > kRmsM) { return Event::None; }
    ++s.rigid;
    const bool yaw = w.yawSweepRad >= kSweepRad;
    const bool pitch = w.pitchSweepRad >= kSweepRad;
    if (!yaw && !pitch) { return Event::None; }
    if (yaw) { PushVal(s, 0, w.n[0]); PushVal(s, 2, w.n[2]); ++s.acceptedYaw; }
    if (pitch) { PushVal(s, 1, w.n[1]); PushVal(s, 2, w.n[2]); ++s.acceptedPitch; }
    if (s.acceptedYaw < kYawNeed) { return Event::Accepted; }
    float cand[3] = { MedianOf(s, 0), s.n[1], MedianOf(s, 2) };
    if (s.acceptedPitch >= kPitchNeed) { cand[1] = MedianOf(s, 1); }
    for (int k = 0; k < 3; ++k) {
        if (cand[k] < kBoxMin[k] || cand[k] > kBoxMax[k]) {
            ++s.refused;
            for (int j = 0; j < 3; ++j) { s.refusedMedian[j] = cand[j]; }
            return Event::RefusedOutOfBox;
        }
    }
    bool differs = false;
    for (int k = 0; k < 3; ++k) { if (std::fabs(cand[k] - s.n[k]) >= kCommitM) { differs = true; } }
    if (!differs || (s.lastCommitAt >= 0.0 && x.t - s.lastCommitAt < kCommitMinS)) { return Event::Accepted; }
    for (int k = 0; k < 3; ++k) { s.n[k] = cand[k]; }
    s.lastCommitAt = x.t;
    ++s.commits;
    return Event::Committed;
}

// What to add to every latch.eyeFromAnchor (facing frame) at a commit from n0 to n1 so the virtual head does not move:
// the neck point moves by -Rq (n1 - n0), so the latch takes Ry(-facing) Rq (n1 - n0). Ry(+y) maps l -> (l.x c + l.z s, l.y, -l.x s + l.z c).
inline void LatchShift(float facingYaw, const float Rq[9], const float n0[3], const float n1[3], float out[3])
{
    const float d0 = n1[0] - n0[0], d1 = n1[1] - n0[1], d2 = n1[2] - n0[2];
    const float w[3] = { Rq[0] * d0 + Rq[1] * d1 + Rq[2] * d2, Rq[3] * d0 + Rq[4] * d1 + Rq[5] * d2, Rq[6] * d0 + Rq[7] * d1 + Rq[8] * d2 };
    const float c = std::cos(facingYaw), s = std::sin(facingYaw);
    out[0] = w[0] * c - w[2] * s;
    out[1] = w[1];
    out[2] = w[0] * s + w[2] * c;
}

// The file line for one headset: headset=<name>|n=<x> <y> <z>|yaw=<k>|pitch=<k>. A '|' or line break in the name is written as ' '.
inline void CleanName(const char* in, char* out, int cap)
{
    int i = 0;
    for (; in && in[i] && i < cap - 1; ++i) { out[i] = (in[i] == '|' || in[i] == '\n' || in[i] == '\r') ? ' ' : in[i]; }
    out[i] = '\0';
}

inline int Serialize(const State& s, const char* headset, char* buf, int cap)
{
    char name[128];
    CleanName(headset, name, static_cast<int>(sizeof(name)));
    const int n = std::snprintf(buf, static_cast<size_t>(cap), "headset=%s|n=%.5f %.5f %.5f|yaw=%d|pitch=%d", name, s.n[0], s.n[1], s.n[2],
                                s.acceptedYaw, s.acceptedPitch);
    return (n > 0 && n < cap) ? n : -1;
}

// True only for THIS headset's line.
inline bool Parse(const char* line, const char* headset, float n[3], int counts[2])
{
    if (!line) { return false; }
    char name[128];
    CleanName(headset, name, static_cast<int>(sizeof(name)));
    const size_t nl = std::strlen(name);
    if (std::strncmp(line, "headset=", 8) != 0 || std::strncmp(line + 8, name, nl) != 0 || line[8 + nl] != '|') { return false; }
    float a = 0.0f, b = 0.0f, c = 0.0f;
    int y = 0, p = 0;
#if defined(_MSC_VER)
    if (sscanf_s(line + 8 + nl, "|n=%f %f %f|yaw=%d|pitch=%d", &a, &b, &c, &y, &p) != 5) { return false; }
#else
    if (std::sscanf(line + 8 + nl, "|n=%f %f %f|yaw=%d|pitch=%d", &a, &b, &c, &y, &p) != 5) { return false; }
#endif
    n[0] = a; n[1] = b; n[2] = c;
    counts[0] = y; counts[1] = p;
    return true;
}

}  // namespace pivotfit
}  // namespace dlvr
