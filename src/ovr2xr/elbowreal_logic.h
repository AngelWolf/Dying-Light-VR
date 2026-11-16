// elbowreal_logic.h - THE REAL-ARM ELBOW (rig_elbow_pole_mode=2), pure maths: no globals, no LOG, no allocation.
// docs/SPEC-ELBOW-REAL-ARM-2026-10-25.md. The Python twin is tools/armcap_realarm.py (solve_swivel, ORDER=lex) - it is the
// reference, replayed on captures, and generates the golden vectors in elbowreal_golden.inc (src/src/tests/elbowreal_logic_test.cpp).
//
// The elbow is chosen by a SWIVEL phi about the shoulder-wrist line (phi 0 = the body rule's pole t), RANKED (Jay 2026-10-25):
//   1. the REAL wrist fits - the swing between the forearm and handReal = handW . R(X -> v), v = the calibrated forearm-in-hand
//      (arm_fit.txt), inside the shipped wrist clamp's limits;
//   2. then the forearm's roll fits - the solve's own twist (forearm vs handW, the one rig_twist_chain_max_deg caps) within
//      rotLimitDeg of the measured neutral;
//   3. then the swivel nearest the LIFTED TARGET (with a 15 / 20 deg hysteresis toward the previous solve's swivel).
// 2026-10-29 (docs/superpowers/specs/2026-10-29-elbow-smooth-outward-design.md; twin solve_swivel_v2): the target is the body
// rule's swivel lifted toward the most-out elbow by liftDeg (rig_elbow_out_deg) x gunW, never past it; it is returned exactly when
// it fits; otherwise the grid choice is refined continuously (bisection to the fit boundary toward the target, or a parabolic
// step on the wrist excess where nothing fits), so the swivel is no longer a 5-deg grid point. FilterSwivel low-passes what the
// caller applies. At liftDeg 0 the choice is the 10-25 build's up to one grid step, toward the body rule.
// Matrices are row-major 3x3 with the basis in the COLUMNS (as boneblend.cpp and tools/armcap_lib.py). Internally double, so
// the 5-deg grid's ties resolve as the Python twin's do.
#pragma once

#include <cmath>

namespace dlvr {
namespace elbowreal {

// The solve's twist that means "forearm neutral", per arm [0] R, [1] L, degrees - MEASURED, not assumed: the median of the
// solve's own twist over reach-forward holds where the body rule's real wrist fits (tools/armcap_realarm.py neutral, 8 captures,
// backups/20261025_realarm_gate; the forearm aimed at the reachable wrist as rig_forearm_aim_achieved=1 does; R n 8012,
// L n 10817; class-median spread 22.4 / 15.1 deg).
constexpr float kNeutralDeg[2] = { -0.1f, -12.8f };

struct Limits { float posA, negA, posB, negB; };   // degrees: axisA + / -, axisB + / - (the wrist clamp's, already side-mapped)

struct Inputs {
    float S[3], T[3], dir[3], A, L1, t[3];   // shoulder, wrist target, unit S->T, shoulder angle (rad), upper-arm length, body-rule pole
    float prevPhiDeg;
    bool  havePrev;
    float handW[9], v[3];                    // the commanded hand (cWorld . grip) and the calibrated forearm-in-hand
    float neutralDeg, rotLimitDeg;
    Limits lim;
    float liftDeg;                           // rig_elbow_out_deg, clamped 0..kLiftMaxDeg
    float out[3];                            // the body's own-side OUT axis (unit, world)
    float gunW;                              // GunFade(): 1 = no fade, 0 = the lift is off
    // 2026-11-08 THE ELBOW ZONE (Jay: "basically need to set limits"; offline replay of 22 captures, face 0.20 m + swing 60 deg
    // chosen). Both 0 / haveCam false = off: the search is byte-for-byte the 2026-10-29 one. A candidate's ZONE VIOLATION is
    // (faceMinM - |E - cam|) when the elbow is nearer the camera than faceMinM, plus 0.01 per degree its swivel sits past
    // swingMaxDeg from the lifted target; only the least-violating candidates (0 = inside the zone) are ranked, and the ranking
    // among them is unchanged. Twin: tools/elbow_zone_replay.py (solve_zone).
    float cam[3];                            // element 8's world position this solve (the camera, INV-209)
    bool  haveCam;
    float faceMinM;                          // rig_elbow_face_min_cm / 100; 0 = no face limit
    float swingMaxDeg;                       // rig_elbow_swing_max_deg; 0 = no swing cap
};

// The 2026-10-29 constants - twin LIFT_MAX_DEG / EXT_LO / EXT_SPAN / FLIP_SPAN / REFINE_STEPS / FILTER_TAU_S / GUN_* (tools/
// armcap_realarm.py). Offline gate backups/20261029_elbow_outward_gate (ALL BARS PASS with these values).
constexpr float kLiftMaxDeg = 45.0f;
constexpr double kExtLo = 0.10, kExtSpan = 0.20;     // the lift fades where OUT lies along the arm (extent R below kExtLo + kExtSpan)
constexpr double kFlipSpanDeg = 30.0;                // and where the body rule's elbow is within this of the most-IN swivel
constexpr int kRefineSteps = 8;
constexpr int kWalkMax = 36;                         // the refine's walk toward the target: at most 180 deg in 5-deg steps
constexpr float kFilterTauS = 0.09f;                 // FilterSwivel's time constant (spec 8.5: 0.08-0.10 s)
constexpr float kGunHdFar = 0.45f, kGunHdNear = 0.30f, kGunYLo = -0.30f, kGunYHi = -0.15f;   // m, controller vs head

struct Result {
    float pole[3];
    float phiDeg;
    float phiTargetDeg;                      // the lifted target the choice was measured from
    float wristExBefore, wristExAfter, twistExBefore, twistExAfter;   // degrees past the limits, at phi 0 and at the chosen phi
    bool  fitted;    // both the real wrist and the forearm roll fit at the chosen phi
    bool  refused;   // no usable handReal (bad v / non-finite hand): pole = t, phi 0
    int   evals;
    // the zone (all false / -1 when it is off)
    bool  zoneTargetOut;   // the lifted target itself was outside the zone, so the zone decided this solve
    bool  zoneFallback;    // no candidate was inside the zone: the least violation was taken
    float faceDistM;       // the chosen elbow's distance from the camera, m (-1 = no camera / not evaluated)
};

namespace detail {

inline void Cross(const double a[3], const double b[3], double o[3])
{
    o[0] = a[1]*b[2] - a[2]*b[1]; o[1] = a[2]*b[0] - a[0]*b[2]; o[2] = a[0]*b[1] - a[1]*b[0];
}
inline double Dot(const double a[3], const double b[3]) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
inline double Len(const double a[3]) { return std::sqrt(Dot(a, a)); }
inline void Nrm(double a[3])
{
    const double l = Len(a);
    if (l > 1e-12) { a[0] /= l; a[1] /= l; a[2] /= l; } else { a[0] = a[1] = a[2] = 0.0; }
}
inline void MulMM(const double a[9], const double b[9], double o[9])
{
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) { o[r*3+c] = a[r*3+0]*b[0*3+c] + a[r*3+1]*b[1*3+c] + a[r*3+2]*b[2*3+c]; }
    }
}
inline void Transpose(const double a[9], double o[9])
{
    o[0] = a[0]; o[1] = a[3]; o[2] = a[6]; o[3] = a[1]; o[4] = a[4]; o[5] = a[7]; o[6] = a[2]; o[7] = a[5]; o[8] = a[8];
}
inline double WrapDeg(double a)
{
    if (!std::isfinite(a)) { return a; }                       // never spin on inf (review finding 1)
    if (a > 540.0 || a < -540.0) { a = std::fmod(a, 360.0); }
    while (a > 180.0) { a -= 360.0; }
    while (a < -180.0) { a += 360.0; }
    return a;
}
inline double AbsD(double a) { return a < 0.0 ? -a : a; }

// armcap_lib.quat - the standard branchy conversion, (x, y, z, w).
inline void Quat(const double m[9], double q[4])
{
    const double tr = m[0] + m[4] + m[8];
    if (tr > 0.0) {
        const double s = std::sqrt(tr + 1.0) * 2.0;
        q[0] = (m[7]-m[5])/s; q[1] = (m[2]-m[6])/s; q[2] = (m[3]-m[1])/s; q[3] = 0.25*s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        const double s = std::sqrt(1.0 + m[0] - m[4] - m[8]) * 2.0;
        q[0] = 0.25*s; q[1] = (m[1]+m[3])/s; q[2] = (m[2]+m[6])/s; q[3] = (m[7]-m[5])/s;
    } else if (m[4] > m[8]) {
        const double s = std::sqrt(1.0 + m[4] - m[0] - m[8]) * 2.0;
        q[0] = (m[1]+m[3])/s; q[1] = 0.25*s; q[2] = (m[5]+m[7])/s; q[3] = (m[2]-m[6])/s;
    } else {
        const double s = std::sqrt(1.0 + m[8] - m[0] - m[4]) * 2.0;
        q[0] = (m[2]+m[6])/s; q[1] = (m[5]+m[7])/s; q[2] = 0.25*s; q[3] = (m[3]-m[1])/s;
    }
}

// cA / sA = cos / sin of the shoulder angle, passed in so the 73-evaluation search computes them once (review finding 3).
inline void ForearmBasisCS(const double S[3], const double T[3], const double dir[3], const double pole[3], double cA, double sA,
                           double L1, double E[3], double M[9])
{
    double ax[3]; Cross(dir, pole, ax); Nrm(ax);
    double axd[3]; Cross(ax, dir, axd);
    double F[3];
    for (int k = 0; k < 3; ++k) { E[k] = S[k] + (dir[k]*cA + axd[k]*sA) * L1; F[k] = T[k] - E[k]; }
    Nrm(F);
    const double k1 = Dot(ax, F);
    double c1[3] = { ax[0] - F[0]*k1, ax[1] - F[1]*k1, ax[2] - F[2]*k1 }; Nrm(c1);
    double c2[3]; Cross(F, c1, c2);
    M[0] = F[0]; M[1] = c1[0]; M[2] = c2[0];
    M[3] = F[1]; M[4] = c1[1]; M[5] = c2[1];
    M[6] = F[2]; M[7] = c1[2]; M[8] = c2[2];
}

inline void ForearmBasisD(const double S[3], const double T[3], const double dir[3], const double pole[3], double A, double L1,
                          double E[3], double M[9])
{
    ForearmBasisCS(S, T, dir, pole, std::cos(A), std::sin(A), L1, E, M);
}

// armcap_lib.wrist_swing: rel = m^T . hand; twist about X taken off on the left (Rx(-tw) . rel); swing as a rotation vector.
inline void WristSwingD(const double m[9], const double hand[9], double* tw, double* a, double* b)
{
    double mt[9], rel[9]; Transpose(m, mt); MulMM(mt, hand, rel);
    double q[4]; Quat(rel, q);
    const double kPi = 3.14159265358979323846;
    double t = 2.0 * std::atan2(q[0], q[3]);
    if (t > kPi) { t -= 2.0*kPi; }
    if (t < -kPi) { t += 2.0*kPi; }
    const double c = std::cos(-t), s = std::sin(-t);
    const double rx[9] = { 1, 0, 0, 0, c, -s, 0, s, c };
    double sw[9]; MulMM(rx, rel, sw);
    double cc = (sw[0] + sw[4] + sw[8] - 1.0) * 0.5;
    if (cc > 1.0) { cc = 1.0; }
    if (cc < -1.0) { cc = -1.0; }
    const double ang = std::acos(cc);
    const double rv[3] = { sw[7]-sw[5], sw[2]-sw[6], sw[3]-sw[1] };
    const double rl = Len(rv);
    *tw = t;
    if (ang < 1e-4 || rl < 1e-6) { *a = 0.0; *b = 0.0; return; }
    *a = rv[1]/rl*ang; *b = rv[2]/rl*ang;
}

inline double ExcessDegD(double a, double b, const Limits& lim)
{
    const double d2r = 3.14159265358979323846 / 180.0;
    const double pa = lim.posA*d2r, na = lim.negA*d2r, pb = lim.posB*d2r, nb = lim.negB*d2r;
    double e = 0.0;
    if (a - pa > e) { e = a - pa; }
    if (-a - na > e) { e = -a - na; }
    if (b - pb > e) { e = b - pb; }
    if (-b - nb > e) { e = -b - nb; }
    return e / d2r;
}

inline bool HandRealBasisD(const float handW[9], const float v[3], double out[9])
{
    for (int k = 0; k < 9; ++k) { if (!std::isfinite(handW[k])) { return false; } }
    double vn[3] = { v[0], v[1], v[2] };
    if (!std::isfinite(vn[0]) || !std::isfinite(vn[1]) || !std::isfinite(vn[2])) { return false; }
    const double vl = Len(vn);
    if (!(vl > 0.5)) { return false; }
    vn[0] /= vl; vn[1] /= vl; vn[2] /= vl;
    if (vn[0] < 0.5) { return false; }      // more than 60 deg off the hand's X: implausible
    const double x[3] = { 1.0, 0.0, 0.0 };
    double ax[3]; Cross(x, vn, ax);
    const double s = Len(ax), c = Dot(x, vn);
    double R[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    if (s >= 1e-9) {
        const double k[3] = { ax[0]/s, ax[1]/s, ax[2]/s };
        const double K[9] = { 0, -k[2], k[1], k[2], 0, -k[0], -k[1], k[0], 0 };
        double KK[9]; MulMM(K, K, KK);
        const double ang = std::atan2(s, c), sa = std::sin(ang), ca = 1.0 - std::cos(ang);
        for (int i = 0; i < 9; ++i) { R[i] = ((i == 0 || i == 4 || i == 8) ? 1.0 : 0.0) + sa*K[i] + ca*KK[i]; }
    }
    double hw[9];
    for (int i = 0; i < 9; ++i) { hw[i] = handW[i]; }
    MulMM(hw, R, out);
    return true;
}

}  // namespace detail

inline void ForearmBasis(const float S[3], const float T[3], const float dir[3], const float pole[3], float A, float L1, float outE[3],
                         float outM[9])
{
    double s[3], t[3], d[3], p[3], E[3], M[9];
    for (int k = 0; k < 3; ++k) { s[k] = S[k]; t[k] = T[k]; d[k] = dir[k]; p[k] = pole[k]; }
    detail::ForearmBasisD(s, t, d, p, A, L1, E, M);
    for (int k = 0; k < 3; ++k) { outE[k] = static_cast<float>(E[k]); }
    for (int k = 0; k < 9; ++k) { outM[k] = static_cast<float>(M[k]); }
}

inline void WristSwing(const float m[9], const float hand[9], float* twistRad, float* aRad, float* bRad)
{
    double md[9], hd[9], tw, a, b;
    for (int k = 0; k < 9; ++k) { md[k] = m[k]; hd[k] = hand[k]; }
    detail::WristSwingD(md, hd, &tw, &a, &b);
    *twistRad = static_cast<float>(tw); *aRad = static_cast<float>(a); *bRad = static_cast<float>(b);
}

inline float ExcessDeg(float aRad, float bRad, const Limits& lim) { return static_cast<float>(detail::ExcessDegD(aRad, bRad, lim)); }

inline bool HandRealBasis(const float handW[9], const float v[3], float out[9])
{
    double o[9];
    if (!detail::HandRealBasisD(handW, v, o)) { return false; }
    for (int k = 0; k < 9; ++k) { out[k] = static_cast<float>(o[k]); }
    return true;
}

inline float Smoothstep(float a, float b, float x)   // 0 at a, 1 at b (either order), cubic between - twin smoothstep
{
    if (a == b) { return x >= b ? 1.0f : 0.0f; }
    float u = (x - a) / (b - a);
    if (!(u > 0.0f)) { u = 0.0f; }
    if (u > 1.0f) { u = 1.0f; }
    return u * u * (3.0f - 2.0f * u);
}

// 1 = no fade. The RIGHT arm with a firearm held: fades to 0 as the controller comes up to the head (Jay 2026-10-29: "Fade it out
// as the gun comes up"). ctlRelHead = controller minus head position, app space (metres).
inline float GunFade(bool fireHeldRight, bool isRight, const float ctlRelHead[3])
{
    if (!isRight || !fireHeldRight) { return 1.0f; }
    const float hd = std::sqrt(ctlRelHead[0]*ctlRelHead[0] + ctlRelHead[1]*ctlRelHead[1] + ctlRelHead[2]*ctlRelHead[2]);
    const float w = 1.0f - Smoothstep(kGunHdFar, kGunHdNear, hd) * Smoothstep(kGunYLo, kGunYHi, ctlRelHead[1]);
    return std::isfinite(w) ? w : 1.0f;
}

// The applied swivel follows the chosen one with time constant tauS, the short way round; a gap (dt > 0.25 s), no history or a
// bad dt starts from the chosen swivel (the pole memory's own gap rule).
inline float FilterSwivel(float appliedDeg, float chosenDeg, float dt, float tauS, bool have)
{
    using detail::WrapDeg;
    if (!have || !(dt <= 0.25f) || !(dt >= 0.0f) || !std::isfinite(appliedDeg) || !(tauS > 1e-4f)) {
        return static_cast<float>(WrapDeg(chosenDeg));
    }
    const double k = 1.0 - std::exp(-static_cast<double>(dt) / static_cast<double>(tauS));
    return static_cast<float>(WrapDeg(appliedDeg + WrapDeg(static_cast<double>(chosenDeg) - appliedDeg) * k));
}

// The held pole memory (boneblend s_epPrevB) holds LAST frame's FINAL pole, which carries the swivel mode 2 drew; rotate it back
// about the arm by that swivel so the next t is mode 1's own held pole (final review finding 1: without it the lift and the filter
// feed back inside the held cone and ratchet - twin unrotate_held). Swivel convention as SolveSwivel: +a maps t -> t cos a +
// (dir x t) sin a. prevP perpendicular to dir, unit; dir unit.
inline void UnrotateHeld(const float prevP[3], const float dir[3], float drawnDeg, float out[3])
{
    const float a = drawnDeg * 0.01745329252f, ca = std::cos(a), sa = std::sin(a);
    const float q[3] = { dir[1]*prevP[2] - dir[2]*prevP[1], dir[2]*prevP[0] - dir[0]*prevP[2], dir[0]*prevP[1] - dir[1]*prevP[0] };
    for (int k = 0; k < 3; ++k) { out[k] = prevP[k]*ca - q[k]*sa; }
}

inline Result SolveSwivel(const Inputs& in)
{
    using namespace detail;
    Result r{};
    for (int k = 0; k < 3; ++k) { r.pole[k] = in.t[k]; }
    r.phiDeg = 0.0f;
    r.phiTargetDeg = 0.0f;
    // Review finding 1: every geometric input finite, or refuse (a NaN basis reads as "fits" in ExcessDegD's > tests).
    for (int k = 0; k < 3; ++k) {
        if (!std::isfinite(in.S[k]) || !std::isfinite(in.T[k]) || !std::isfinite(in.dir[k]) || !std::isfinite(in.t[k]) ||
            !std::isfinite(in.out[k])) {
            r.refused = true; return r;
        }
    }
    if (!std::isfinite(in.liftDeg) || !std::isfinite(in.gunW)) { r.refused = true; return r; }
    r.zoneTargetOut = false; r.zoneFallback = false; r.faceDistM = -1.0f;
    const bool faceOn = in.haveCam && in.faceMinM > 0.0f && std::isfinite(in.faceMinM) &&
                        std::isfinite(in.cam[0]) && std::isfinite(in.cam[1]) && std::isfinite(in.cam[2]);
    const bool swingOn = in.swingMaxDeg > 0.0f && std::isfinite(in.swingMaxDeg);
    const bool zoneOn = faceOn || swingOn;
    if (!std::isfinite(in.A) || !std::isfinite(in.L1) || !std::isfinite(in.prevPhiDeg) || !std::isfinite(in.neutralDeg) ||
        !std::isfinite(in.rotLimitDeg)) {
        r.refused = true; return r;
    }
    double hR[9];
    if (!HandRealBasisD(in.handW, in.v, hR)) { r.refused = true; return r; }
    double S[3], T[3], d[3], t[3], hW[9];
    for (int k = 0; k < 3; ++k) { S[k] = in.S[k]; T[k] = in.T[k]; d[k] = in.dir[k]; t[k] = in.t[k]; }
    for (int k = 0; k < 9; ++k) { hW[k] = in.handW[k]; }
    double q[3]; Cross(d, t, q);
    const double d2r = 3.14159265358979323846 / 180.0;
    const double cA = std::cos(static_cast<double>(in.A)), sA = std::sin(static_cast<double>(in.A));
    // the 72-point grid's cos / sin, computed once per process (review finding 3); index 72 is phi 0 for the first evaluation
    struct Grid { double c[73], s[73]; Grid() { for (int k = 0; k < 72; ++k) { const double p = (-180.0 + 5.0*k) * (3.14159265358979323846 / 180.0); c[k] = std::cos(p); s[k] = std::sin(p); } c[72] = 1.0; s[72] = 0.0; } };
    static const Grid g;
    struct Cand { double phi, wex, tex, c, s, zv, fd; };   // zv = zone violation (0 = inside), fd = elbow to camera (m, -1 none)
    double phiT = 0.0;   // the lifted target, set below; the swing cap measures from it
    auto evcs = [&](double c, double s, double phiDeg) -> Cand {
        const double cand[3] = { t[0]*c + q[0]*s, t[1]*c + q[1]*s, t[2]*c + q[2]*s };
        double E[3], M[9]; ForearmBasisCS(S, T, d, cand, cA, sA, in.L1, E, M);
        double tw, a, b, tw2, ar, br;
        WristSwingD(M, hW, &tw, &a, &b);
        WristSwingD(M, hR, &tw2, &ar, &br);
        double tex = AbsD(WrapDeg(tw / d2r - in.neutralDeg)) - in.rotLimitDeg;
        if (tex < 0.0) { tex = 0.0; }
        double zv = 0.0, fd = -1.0;
        if (faceOn) {
            const double ec[3] = { E[0] - in.cam[0], E[1] - in.cam[1], E[2] - in.cam[2] };
            fd = Len(ec);
            if (fd < in.faceMinM) { zv += static_cast<double>(in.faceMinM) - fd; }
        }
        if (swingOn) {
            const double over = AbsD(WrapDeg(phiDeg - phiT)) - static_cast<double>(in.swingMaxDeg);
            if (over > 0.0) { zv += over * 0.01; }
        }
        return Cand{ phiDeg, ExcessDegD(ar, br, in.lim), tex, c, s, zv, fd };
    };
    auto ev = [&](double phiDeg) -> Cand { return evcs(std::cos(phiDeg * d2r), std::sin(phiDeg * d2r), phiDeg); };
    auto take = [&](const Cand& b, bool fitted) {
        for (int k = 0; k < 3; ++k) { r.pole[k] = static_cast<float>(t[k]*b.c + q[k]*b.s); }
        r.phiDeg = static_cast<float>(WrapDeg(b.phi));
        r.wristExAfter = static_cast<float>(b.wex);
        r.twistExAfter = static_cast<float>(b.tex);
        r.fitted = fitted;
        r.faceDistM = static_cast<float>(b.fd);
    };
    // the lifted target (twin lift_target): the elbow offset for swivel phi is t cos + q sin, so its OUT is R cos(phi - star)
    double lift = in.liftDeg < 0.0f ? 0.0 : (in.liftDeg > kLiftMaxDeg ? kLiftMaxDeg : static_cast<double>(in.liftDeg));
    lift *= in.gunW < 0.0f ? 0.0 : (in.gunW > 1.0f ? 1.0 : static_cast<double>(in.gunW));
    {
        const double ov[3] = { in.out[0], in.out[1], in.out[2] };
        const double oa = Dot(t, ov), ob = Dot(q, ov), R = std::sqrt(oa*oa + ob*ob);
        if (lift > 0.0 && R >= 1e-9) {
            const double star = std::atan2(ob, oa) / d2r;
            double wExt = (R - kExtLo) / kExtSpan;
            wExt = wExt < 0.0 ? 0.0 : (wExt > 1.0 ? 1.0 : wExt);
            double wFlip = (180.0 - AbsD(star)) / kFlipSpanDeg;
            wFlip = wFlip < 0.0 ? 0.0 : (wFlip > 1.0 ? 1.0 : wFlip);
            const double lim = lift * wExt * wFlip;
            if (star != 0.0) { phiT = std::copysign(AbsD(star) < lim ? AbsD(star) : lim, star); }
        }
    }
    r.phiTargetDeg = static_cast<float>(phiT);
    const Cand cT = (phiT == 0.0) ? evcs(g.c[72], g.s[72], 0.0) : ev(phiT);
    const Cand c0 = (phiT == 0.0) ? cT : evcs(g.c[72], g.s[72], 0.0);
    r.wristExBefore = static_cast<float>(c0.wex); r.twistExBefore = static_cast<float>(c0.tex);
    r.evals = (phiT == 0.0) ? 1 : 2;
    if (cT.wex == 0.0 && cT.tex == 0.0 && cT.zv == 0.0) {
        r.wristExAfter = 0.0f; r.twistExAfter = 0.0f; r.fitted = true;
        r.faceDistM = static_cast<float>(cT.fd);
        if (phiT != 0.0) { take(cT, true); }                       // the lifted target fits: exactly it
        return r;                                                   // lift 0 and the body rule fits: t untouched
    }
    r.zoneTargetOut = zoneOn && cT.zv > 0.0;
    // the grid, plus (zone on only - the twin's candidate set) the lifted target itself as index 72
    Cand cs[73];
    for (int k = 0; k < 72; ++k) { cs[k] = evcs(g.c[k], g.s[k], -180.0 + 5.0 * k); }
    r.evals += 72;
    const int nC = zoneOn ? 73 : 72;
    if (zoneOn) { cs[72] = cT; }
    // THE ZONE: only the least-violating candidates (0 = inside) take part; zone off = every candidate (zmin 0, all zv 0)
    double zmin = 0.0;
    if (zoneOn) {
        zmin = 1e30;
        for (int k = 0; k < nC; ++k) { if (cs[k].zv < zmin) { zmin = cs[k].zv; } }
        r.zoneFallback = zmin > 0.0;
    }
    auto ok = [&](const Cand& x) { return !zoneOn || x.zv <= zmin + 1e-9; };
    const double kTie = 0.5, kNear = 15.0, kSlack = 20.0;
    // "better(a, b)": key (|wrap(phi - ref)|, phi) - the Python min() key
    auto keyLess = [&](const Cand& a, const Cand& b, double ref) {
        const double ka = AbsD(WrapDeg(a.phi - ref)), kb = AbsD(WrapDeg(b.phi - ref));
        return ka < kb || (ka == kb && a.phi < b.phi);
    };
    bool anyW = false;
    for (int k = 0; k < nC; ++k) { if (ok(cs[k]) && cs[k].wex == 0.0) { anyW = true; break; } }
    int best = -1;
    if (anyW) {
        bool anyBoth = false;
        double minTex = 1e30;
        for (int k = 0; k < nC; ++k) {
            if (!ok(cs[k]) || cs[k].wex != 0.0) { continue; }
            if (cs[k].tex == 0.0) { anyBoth = true; }
            if (cs[k].tex < minTex) { minTex = cs[k].tex; }
        }
        auto inPool = [&](const Cand& x) {
            return ok(x) && x.wex == 0.0 && (anyBoth ? (x.tex == 0.0) : (x.tex <= minTex + kTie));
        };
        for (int k = 0; k < nC; ++k) { if (inPool(cs[k]) && (best < 0 || keyLess(cs[k], cs[best], phiT))) { best = k; } }
        if (in.havePrev) {
            const double lim = AbsD(WrapDeg(cs[best].phi - phiT)) + kSlack;
            int nb = -1;
            for (int k = 0; k < nC; ++k) {
                if (!inPool(cs[k])) { continue; }
                if (AbsD(WrapDeg(cs[k].phi - in.prevPhiDeg)) > kNear || AbsD(WrapDeg(cs[k].phi - phiT)) > lim) { continue; }
                if (nb < 0 || keyLess(cs[k], cs[nb], in.prevPhiDeg)) { nb = k; }
            }
            if (nb >= 0) { best = nb; }
        }
        // continuous refine (final review finding 2): from best, WALK toward the target in steps of <= 5 deg while the pool holds,
        // then bisect the first step that leaves it - the choice rides the near boundary of best's own region, so a held best no
        // longer sits on its grid point until the hysteresis lets go, and the side the hysteresis chose is kept (the walk stops at
        // the region's edge). Reaching the target itself (in the tie pool, not fully fitting) returns it exactly.
        Cand keep = cs[best];
        const double dT = WrapDeg(phiT - cs[best].phi);
        if (dT != 0.0) {
            double pos = cs[best].phi;
            for (int w = 0; w < kWalkMax; ++w) {
                const double rem = WrapDeg(phiT - pos);
                if (AbsD(rem) < 1e-9) { break; }
                const Cand e = ev(pos + std::copysign(AbsD(rem) < 5.0 ? AbsD(rem) : 5.0, dT));
                ++r.evals;
                if (inPool(e)) { pos = e.phi; keep = e; continue; }
                double lo = pos, hi = e.phi;
                for (int i = 0; i < kRefineSteps; ++i) {
                    const Cand m = ev(0.5 * (lo + hi));
                    ++r.evals;
                    if (inPool(m)) { lo = m.phi; keep = m; } else { hi = m.phi; }
                }
                break;
            }
        }
        take(keep, anyBoth);
        return r;
    }
    double lo = 1e30;
    for (int k = 0; k < nC; ++k) { if (ok(cs[k]) && cs[k].wex < lo) { lo = cs[k].wex; } }
    double loT = 1e30;
    for (int k = 0; k < nC; ++k) { if (ok(cs[k]) && cs[k].wex <= lo + kTie && cs[k].tex < loT) { loT = cs[k].tex; } }
    const double ref = in.havePrev ? in.prevPhiDeg : phiT;
    for (int k = 0; k < nC; ++k) {
        if (!(ok(cs[k]) && cs[k].wex <= lo + kTie && cs[k].tex <= loT + kTie)) { continue; }
        if (best < 0 || keyLess(cs[k], cs[best], ref)) { best = k; }
    }
    if (best < 0) { r.refused = true; return r; }              // no candidate chosen (non-finite path): pole = t (finding 1)
    // a parabolic step on the wrist excess through best and its two grid neighbours, clamped to +-5 deg, kept only if no worse
    // (and, zone on, still inside the zone); it can land inside a fit no grid point reached, and then says so. Not for the
    // target (index 72), which has no grid neighbours.
    Cand keep = cs[best];
    if (best < 72) {
        const double em = cs[(best + 71) % 72].wex, ep = cs[(best + 1) % 72].wex, den = em - 2.0 * cs[best].wex + ep;
        if (den > 1e-9) {
            double off = 2.5 * (em - ep) / den;
            off = off < -5.0 ? -5.0 : (off > 5.0 ? 5.0 : off);
            const Cand x = ev(cs[best].phi + off);
            ++r.evals;
            if (ok(x) && (x.wex < keep.wex || (x.wex == keep.wex && x.tex <= keep.tex))) { keep = x; }
        }
    }
    take(keep, keep.wex == 0.0 && keep.tex == 0.0);
    return r;
}

// Moves w toward 1 (on) or 0 (off) at 1/seconds per second; dt clamped to [0, 0.25] (the pole memory's own gap rule).
inline float FadeStep(float w, bool on, float dt, float seconds)
{
    if (!(dt >= 0.0f)) { dt = 0.0f; }
    if (dt > 0.25f) { dt = 0.25f; }
    const float step = (seconds > 1e-4f) ? dt / seconds : 1.0f;
    w += on ? step : -step;
    if (w < 0.0f) { w = 0.0f; }
    if (w > 1.0f) { w = 1.0f; }
    return w;
}

// ===== THE FINAL-ELBOW FACE LIMIT (Jay 2026-11-16: "Elbow ... in view whilst grabbing holsters" -> "Yes"; trace
// docs/traces/20261116_1800_elbow_face_final.md). SolveSwivel's face limit (INV-708) is faded out by the short-arm hold and absent from
// the mode-1 fallbacks (STATE 2026-11-08 (4) "NOT COVERED"); SolveArmIk applies this to the elbow it is about to place, on every path.
// v turned by deg about the UNIT axis k (Rodrigues, right-handed). deg 0 returns v bit-identical.
inline void TurnAbout(const float v[3], const float k[3], float deg, float out[3])
{
    if (deg == 0.0f) { out[0] = v[0]; out[1] = v[1]; out[2] = v[2]; return; }
    const double a = static_cast<double>(deg) * (3.14159265358979323846 / 180.0);
    const double c = std::cos(a), s = std::sin(a);
    const double kv = static_cast<double>(k[0])*v[0] + static_cast<double>(k[1])*v[1] + static_cast<double>(k[2])*v[2];
    const double cx = static_cast<double>(k[1])*v[2] - static_cast<double>(k[2])*v[1];
    const double cy = static_cast<double>(k[2])*v[0] - static_cast<double>(k[0])*v[2];
    const double cz = static_cast<double>(k[0])*v[1] - static_cast<double>(k[1])*v[0];
    out[0] = static_cast<float>(v[0]*c + cx*s + k[0]*kv*(1.0 - c));
    out[1] = static_cast<float>(v[1]*c + cy*s + k[1]*kv*(1.0 - c));
    out[2] = static_cast<float>(v[2]*c + cz*s + k[2]*kv*(1.0 - c));
}

// THE IN-LIMIT (Jay 2026-11-16: "Just stop the swing in to the torso. No ones going to intentionally try touch their elbows together, so
// the elbow doesn't need to go that far in bar two hand grips or the bow"; trace docs/traces/20261116_1745_elbow_in_limit.md): the elbow
// sits no further than this inboard of its OWN shoulder, sideways in the body frame. 0.10 m = Claude's pick (the shoulder is ~0.17-0.20 m
// off the midline in the arm captures), not a measured preference - one constant.
constexpr float kElbowInMaxM = 0.10f;

// The smallest turn (degrees, 1-degree steps out to +-180, + before - at equal size) of the upper-arm direction U about the shoulder-hand
// line dir (unit) that puts the elbow E = S + U*L1 inside BOTH optional limits: |E - cam| >= faceMinM (on when faceMinM > 0) and
// outAxis . (E - S) >= outMinM (on when haveOut; outAxis = the arm's own-side out, unit). 0 when it already is inside, when neither limit
// is on, or when an input is not finite. When no turn clears both, the turn of the least total shortfall (strictly less than no turn -
// an all-equal circle keeps 0). *faceDistOut = that elbow's camera distance (-1 when the face limit is off / not evaluated); *outOut =
// its outAxis . (E - S). COST: one evaluation when inside; at most 361 turned points when not.
inline float ElbowClearTurnDeg(const float S[3], const float U[3], const float dir[3], float L1, const float cam[3], float faceMinM,
                               const float outAxis[3], float outMinM, bool haveOut, float* faceDistOut, float* outOut)
{
    if (faceDistOut) { *faceDistOut = -1.0f; }
    if (outOut) { *outOut = 0.0f; }
    const bool faceOn = faceMinM > 0.0f && std::isfinite(faceMinM);
    const bool outOn = haveOut && std::isfinite(outMinM);
    if (!faceOn && !outOn) { return 0.0f; }
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(S[i]) || !std::isfinite(U[i]) || !std::isfinite(dir[i])) { return 0.0f; }
        if (faceOn && !std::isfinite(cam[i])) { return 0.0f; }
        if (outOn && !std::isfinite(outAxis[i])) { return 0.0f; }
    }
    if (!(L1 > 0.0f) || !std::isfinite(L1)) { return 0.0f; }
    struct Ev { double short_, face, out; };
    auto at = [&](float deg) {
        float u[3];
        TurnAbout(U, dir, deg, u);
        const double e[3] = { static_cast<double>(u[0])*L1, static_cast<double>(u[1])*L1, static_cast<double>(u[2])*L1 };   // E - S
        Ev r{ 0.0, -1.0, 0.0 };
        if (faceOn) {
            const double ex = S[0] + e[0] - cam[0], ey = S[1] + e[1] - cam[1], ez = S[2] + e[2] - cam[2];
            r.face = std::sqrt(ex*ex + ey*ey + ez*ez);
            if (r.face < faceMinM) { r.short_ += static_cast<double>(faceMinM) - r.face; }
        }
        r.out = e[0]*outAxis[0] + e[1]*outAxis[1] + e[2]*outAxis[2];
        if (outOn && r.out < outMinM) { r.short_ += static_cast<double>(outMinM) - r.out; }
        return r;
    };
    auto put = [&](const Ev& r) {
        if (faceDistOut) { *faceDistOut = faceOn ? static_cast<float>(r.face) : -1.0f; }
        if (outOut) { *outOut = static_cast<float>(r.out); }
    };
    const Ev e0 = at(0.0f);
    put(e0);
    if (e0.short_ <= 0.0) { return 0.0f; }
    float bestDeg = 0.0f;
    Ev best = e0;
    for (int k = 1; k <= 180; ++k) {
        for (int sg = 0; sg < 2; ++sg) {
            const float deg = sg == 0 ? static_cast<float>(k) : -static_cast<float>(k);
            if (k == 180 && sg == 1) { continue; }   // +180 and -180 are the same turn
            const Ev r = at(deg);
            if (r.short_ <= 0.0) { put(r); return deg; }
            if (r.short_ < best.short_ - 1e-6) { best = r; bestDeg = deg; }
        }
    }
    put(best);
    return bestDeg;
}

// The face limit alone (the in-limit off): kept for its callers / tests - the same search as ElbowClearTurnDeg with haveOut false.
inline float FaceClearTurnDeg(const float S[3], const float U[3], const float dir[3], float L1, const float cam[3], float minM,
                              float* distOut)
{
    if (!(minM > 0.0f)) { if (distOut) { *distOut = -1.0f; } return 0.0f; }
    const float none[3] = { 0.0f, 0.0f, 0.0f };
    float out = 0.0f;
    return ElbowClearTurnDeg(S, U, dir, L1, cam, minM, none, 0.0f, false, distOut, &out);
}

}  // namespace elbowreal
}  // namespace dlvr
