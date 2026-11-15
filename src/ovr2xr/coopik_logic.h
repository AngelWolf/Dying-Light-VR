// coopik_logic.h - the co-op arm-pose channel's PURE remote-pose solve (spec docs/superpowers/specs/2026-11-13-coop-arm-pose-channel-
// design.md section 8, option B; trace docs/traces/20261113_2000_coopik_logic.md). No Windows, no game memory, no globals, no threads -
// unit-tested by src/src/tests/coopik_logic_test.cpp (cl, not part of the DLL).
//
// Conventions: 3x3 rotations row-major m[r*3+c] (columns = the bone's axes in its parent / world); 3x4 matrices row-major with the
// translation at [3] [7] [11] (the layout boneblend's accessors read); quaternions crossing this interface are x y z w (the sample's).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#include "coopnet_logic.h"       // coop::RotYVec - the solve's own yaw
#include "fingertrack_logic.h"   // ft::QuatFromMat / ft::MatFromQuat (w x y z - used only inside BlendLocal)

namespace dlvr {
namespace coopik {

// ---- spec 8.2: which hand is ours (the sample's handState, written by cooppose.cpp CoopPoseCaptureLocal) ----
constexpr uint8_t kKindMask   = 0x03;   // 0 empty / 1 WeaponVis (melee) / 2 any other item / 3 not known
constexpr uint8_t kTwoHand    = 0x04;   // LEFT: gripping a two-handed weapon
constexpr uint8_t kReloadLock = 0x08;   // LEFT: a manual-reload step owns the hand
constexpr uint8_t kNoData     = 0x80;   // the sender had no solve for this hand
constexpr uint8_t kNoRot      = 0x40;   // protocol 3: hand[3..6] is the controller's rotation, not the hand bone's (no grip yet)
constexpr uint8_t kNoElbow    = 0x20;   // protocol 4: elbow[h] carries no bend direction (straight arm / not published)
static_assert(kNoData == coop::kHandNoDataBit, "coop::Unpack's no-data bit (B5 validation) must be the receiver's");
static_assert(kNoElbow == coop::kHandNoElbowBit, "coop::Unpack's no-elbow bit must be the receiver's");

// INV-736 build (Jay "Yes, it should" - both hands always follow the controllers on the partner's screen): the receiver's per-hand gate.
inline bool HandHasData(uint8_t hs) { return (hs & kNoData) == 0; }

// spec 8.2's original free-hand-only rule. No caller since the INV-736 build (HandHasData replaced it); kept with its tests.
inline bool HandIsOurs(uint8_t hs, bool isLeft)
{
    if (hs & kNoData) { return false; }
    if ((hs & kKindMask) != 0) { return false; }
    if (isLeft && (hs & (kTwoHand | kReloadLock))) { return false; }
    return true;
}

// ---- small vector helpers ----
inline float Dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline float Len3(const float a[3]) { return std::sqrt(Dot3(a, a)); }
inline void Cross3(const float a[3], const float b[3], float o[3])
{
    const float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x; o[1] = y; o[2] = z;
}
inline bool Norm3(float v[3])
{
    const float l = Len3(v);
    if (!(l > 1e-8f)) { return false; }   // also rejects NaN
    v[0] /= l; v[1] /= l; v[2] /= l;
    return true;
}
// Any unit vector perpendicular to unit d.
inline void AnyPerp(const float d[3], float o[3])
{
    const float up[3] = { 0.0f, 1.0f, 0.0f }, x[3] = { 1.0f, 0.0f, 0.0f };
    Cross3(d, up, o);
    if (!Norm3(o)) { Cross3(d, x, o); Norm3(o); }
}

// ---- spec 8.4: the target. T = rootPos + Ry(rootYaw) x (headRel + handOff) ----
inline void TargetWorld(const float rootPos[3], float rootYaw, const float headRel[3], const float handOff[3], float out[3])
{
    const float v[3] = { headRel[0] + handOff[0], headRel[1] + handOff[1], headRel[2] + handOff[2] };
    float r[3];
    coop::RotYVec(rootYaw, v, r);
    out[0] = rootPos[0] + r[0]; out[1] = rootPos[1] + r[1]; out[2] = rootPos[2] + r[2];
}

// ---- two-bone IK: shoulder S, lengths lu / lf, target T, pole = which side the elbow goes. Reach clamped to 0.999 of full length.
// False only for degenerate input (a length <= 0, T on S); outputs untouched then.
inline bool TwoBoneIk(const float S[3], float lu, float lf, const float T[3], const float pole[3], float elbow[3], float hand[3])
{
    if (!(lu > 1e-5f) || !(lf > 1e-5f)) { return false; }
    float dir[3] = { T[0] - S[0], T[1] - S[1], T[2] - S[2] };
    float d = Len3(dir);
    if (!(d > 1e-6f)) { return false; }
    dir[0] /= d; dir[1] /= d; dir[2] /= d;
    const float dMax = (lu + lf) * 0.999f;
    const float dMin = std::fabs(lu - lf) * 1.001f + 1e-5f;
    d = d > dMax ? dMax : (d < dMin ? dMin : d);
    const float a = (lu * lu - lf * lf + d * d) / (2.0f * d);
    const float h2 = lu * lu - a * a;
    const float h = h2 > 0.0f ? std::sqrt(h2) : 0.0f;
    const float pd = Dot3(pole, dir);
    float p[3] = { pole[0] - dir[0] * pd, pole[1] - dir[1] * pd, pole[2] - dir[2] * pd };
    if (!Norm3(p)) { AnyPerp(dir, p); }
    for (int i = 0; i < 3; ++i) {
        hand[i]  = S[i] + dir[i] * d;
        elbow[i] = S[i] + dir[i] * a + p[i] * h;
    }
    return true;
}

// ---- out = Rmin(from -> to) x R: turn a bone's world rotation so its `from` direction points along `to`, by the smallest rotation
// (keeps the game's roll about the bone; needs no bone-axis convention). False for a zero direction.
inline bool SwingTo(const float R[9], const float from[3], const float to[3], float out[9])
{
    float f[3] = { from[0], from[1], from[2] }, t[3] = { to[0], to[1], to[2] };
    if (!Norm3(f) || !Norm3(t)) { return false; }
    const float c = Dot3(f, t);
    float k[3];
    Cross3(f, t, k);
    const float s = Len3(k);
    float M[9];
    if (s < 1e-7f) {
        if (c > 0.0f) { std::memcpy(out, R, sizeof(float) * 9); return true; }
        float p[3];
        AnyPerp(f, p);   // 180 deg about p: 2 p p^T - I
        for (int r = 0; r < 3; ++r) { for (int q = 0; q < 3; ++q) { M[r * 3 + q] = 2.0f * p[r] * p[q] - (r == q ? 1.0f : 0.0f); } }
    } else {
        k[0] /= s; k[1] /= s; k[2] /= s;
        const float K[9] = { 0.0f, -k[2], k[1], k[2], 0.0f, -k[0], -k[1], k[0], 0.0f };
        float K2[9];
        for (int r = 0; r < 3; ++r) {
            for (int q = 0; q < 3; ++q) { K2[r * 3 + q] = K[r * 3] * K[q] + K[r * 3 + 1] * K[3 + q] + K[r * 3 + 2] * K[6 + q]; }
        }
        for (int i = 0; i < 9; ++i) { M[i] = (i % 4 == 0 ? 1.0f : 0.0f) + s * K[i] + (1.0f - c) * K2[i]; }
    }
    float o[9];
    for (int r = 0; r < 3; ++r) {
        for (int q = 0; q < 3; ++q) { o[r * 3 + q] = M[r * 3] * R[q] + M[r * 3 + 1] * R[3 + q] + M[r * 3 + 2] * R[6 + q]; }
    }
    std::memcpy(out, o, sizeof(o));
    return true;
}

// ---- 3x4 local <-> world. General inverse (a scaled parent is allowed - INV-11). ----
inline bool Inverse3(const float m[9], float inv[9])
{
    const float c0 = m[4] * m[8] - m[5] * m[7], c1 = m[5] * m[6] - m[3] * m[8], c2 = m[3] * m[7] - m[4] * m[6];
    const float det = m[0] * c0 + m[1] * c1 + m[2] * c2;
    if (!(std::fabs(det) > 1e-12f)) { return false; }
    const float id = 1.0f / det;
    inv[0] = c0 * id; inv[1] = (m[2] * m[7] - m[1] * m[8]) * id; inv[2] = (m[1] * m[5] - m[2] * m[4]) * id;
    inv[3] = c1 * id; inv[4] = (m[0] * m[8] - m[2] * m[6]) * id; inv[5] = (m[2] * m[3] - m[0] * m[5]) * id;
    inv[6] = c2 * id; inv[7] = (m[1] * m[6] - m[0] * m[7]) * id; inv[8] = (m[0] * m[4] - m[1] * m[3]) * id;
    return true;
}
inline void Rot34(const float m[12], float r[9])
{
    r[0] = m[0]; r[1] = m[1]; r[2] = m[2]; r[3] = m[4]; r[4] = m[5]; r[5] = m[6]; r[6] = m[8]; r[7] = m[9]; r[8] = m[10];
}
inline void WorldFromLocal(const float parent[12], const float local[12], float world[12])
{
    float o[12];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            o[r * 4 + c] = parent[r * 4] * local[c] + parent[r * 4 + 1] * local[4 + c] + parent[r * 4 + 2] * local[8 + c];
        }
        o[r * 4 + 3] += parent[r * 4 + 3];
    }
    std::memcpy(world, o, sizeof(o));
}
inline bool LocalFromWorld(const float parent[12], const float world[12], float local[12])
{
    float pr[9], inv[9];
    Rot34(parent, pr);
    if (!Inverse3(pr, inv)) { return false; }
    const float d[3] = { world[3] - parent[3], world[7] - parent[7], world[11] - parent[11] };
    float o[12];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            o[r * 4 + c] = inv[r * 3] * world[c] + inv[r * 3 + 1] * world[4 + c] + inv[r * 3 + 2] * world[8 + c];
        }
        o[r * 4 + 3] = inv[r * 3] * d[0] + inv[r * 3 + 1] * d[1] + inv[r * 3 + 2] * d[2];
    }
    std::memcpy(local, o, sizeof(o));
    return true;
}

// ---- blend the game's local toward ours: w <= 0 = the game's bytes, w >= 1 = ours, else slerp rotation + lerp translation ----
inline void BlendLocal(const float game[12], const float ours[12], float w, float out[12])
{
    if (!(w > 0.0f)) { std::memcpy(out, game, sizeof(float) * 12); return; }
    if (w >= 1.0f) { std::memcpy(out, ours, sizeof(float) * 12); return; }
    float ga[9], ob[9], qa[4], qb[4];
    Rot34(game, ga);
    Rot34(ours, ob);
    ft::QuatFromMat(ga, qa);
    ft::QuatFromMat(ob, qb);
    float dot = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
    if (dot < 0.0f) { for (float& v : qb) { v = -v; } dot = -dot; }
    float ka, kb;
    if (dot > 0.9995f) {
        ka = 1.0f - w; kb = w;
    } else {
        const float th = std::acos(dot), sn = std::sin(th);
        ka = std::sin((1.0f - w) * th) / sn; kb = std::sin(w * th) / sn;
    }
    float q[4];
    for (int i = 0; i < 4; ++i) { q[i] = ka * qa[i] + kb * qb[i]; }
    const float ql = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (float& v : q) { v /= ql; }
    float m[9];
    ft::MatFromQuat(q, m);
    out[0] = m[0]; out[1] = m[1]; out[2]  = m[2];
    out[4] = m[3]; out[5] = m[4]; out[6]  = m[5];
    out[8] = m[6]; out[9] = m[7]; out[10] = m[8];
    out[3]  = game[3]  + (ours[3]  - game[3])  * w;
    out[7]  = game[7]  + (ours[7]  - game[7])  * w;
    out[11] = game[11] + (ours[11] - game[11]) * w;
}

// ---- spec 8.6: the head. q (x y z w) = the headset's turn away from the head's neutral; its angle is clamped to maxRad, then split
// into n equal turns about the same axis (one per neck / neck1 / head bone). Identity in -> identity out.
inline void HeadSplit(const float q[4], int n, float maxRad, float part[4])
{
    float x = q[0], y = q[1], z = q[2], w = q[3];
    if (w < 0.0f) { x = -x; y = -y; z = -z; w = -w; }   // the same turn (double cover)
    if (w > 1.0f) { w = 1.0f; }
    const float s = std::sqrt(1.0f - w * w);
    if (n < 1 || !(s > 1e-7f)) { part[0] = 0.0f; part[1] = 0.0f; part[2] = 0.0f; part[3] = 1.0f; return; }
    float ang = 2.0f * std::acos(w);
    if (ang > maxRad) { ang = maxRad; }
    const float a = ang / static_cast<float>(n);
    const float hs = std::sin(0.5f * a) / s;
    part[0] = x * hs; part[1] = y * hs; part[2] = z * hs; part[3] = std::cos(0.5f * a);
}

// ---- quaternions, x y z w (Hamilton product; QMul(a, b) = apply b first, then a) ----
inline void QMul(const float a[4], const float b[4], float o[4])
{
    const float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    const float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    const float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    const float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    o[0] = x; o[1] = y; o[2] = z; o[3] = w;
}
inline void QConj(const float q[4], float o[4]) { o[0] = -q[0]; o[1] = -q[1]; o[2] = -q[2]; o[3] = q[3]; }
// a turn of `ang` rad about axis 0 / 1 / 2 (X / Y / Z)
inline void QAxis(int axis, float ang, float o[4])
{
    o[0] = 0.0f; o[1] = 0.0f; o[2] = 0.0f; o[3] = std::cos(0.5f * ang);
    o[axis] = std::sin(0.5f * ang);
}
inline void QYaw(float ang, float o[4]) { QAxis(1, ang, o); }   // = coop::RotYQuat's turn
inline void QRotate(const float q[4], const float v[3], float o[3])
{
    const float p[4] = { v[0], v[1], v[2], 0.0f };
    float c[4], t[4], r[4];
    QConj(q, c);
    QMul(q, p, t);
    QMul(t, c, r);
    o[0] = r[0]; o[1] = r[1]; o[2] = r[2];
}
// 3x3 row-major from a unit quaternion (x y z w) - the same formula boneblend's cWorld uses (SolveArmIk, `cq`).
inline void MatFromQ(const float q[4], float m[9])
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y - z * w);     m[2] = 2 * (x * z + y * w);
    m[3] = 2 * (x * y + z * w);     m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z - x * w);
    m[6] = 2 * (x * z - y * w);     m[7] = 2 * (y * z + x * w);     m[8] = 1 - 2 * (x * x + y * y);
}

// 3x3 row-major rotation -> unit quaternion x y z w (ft::QuatFromMat gives w x y z).
inline void QFromMat(const float m[9], float q[4])
{
    float wq[4];
    ft::QuatFromMat(m, wq);
    const float l = std::sqrt(wq[0] * wq[0] + wq[1] * wq[1] + wq[2] * wq[2] + wq[3] * wq[3]);
    const float k = l > 1e-8f ? 1.0f / l : 1.0f;
    q[0] = wq[1] * k; q[1] = wq[2] * k; q[2] = wq[3] * k; q[3] = wq[0] * k;
}

// INV-736 (a): the sender sends the hand BONE's world rotation in the body frame, b = Ry(-rootYaw) x W (x y z w). The receiver's world
// rotation for that bone = Ry(rootYaw_here) x b - the TPP hand bone has the FPP one's authored frame, so no further offset.
inline void HandWorldRot(float rootYaw, const float bodyQ[4], float m[9])
{
    float qy[4], q[4];
    QYaw(rootYaw, qy);
    QMul(qy, bodyQ, q);
    MatFromQ(q, m);
}

// ---- item 5 of the 2026-11-14 proposal (trace 20261115_1735_coop_arms_world): the elbow's BEND DIRECTION ----
// The unit vector from the shoulder->wrist line to the elbow: (E - S) with its component along (W - S) removed. A direction, not a
// position - the partner's TPP arm is not the sender's FPP arm. False (out untouched) when the elbow is less than `minM` off the line
// (a straight arm has no bend side) or the line is degenerate.
inline bool BendDir(const float S[3], const float E[3], const float W[3], float minM, float out[3])
{
    float d[3] = { W[0] - S[0], W[1] - S[1], W[2] - S[2] };
    if (!Norm3(d)) { return false; }
    const float e[3] = { E[0] - S[0], E[1] - S[1], E[2] - S[2] };
    const float k = Dot3(e, d);
    float p[3] = { e[0] - d[0] * k, e[1] - d[1] * k, e[2] - d[2] * k };
    const float l = Len3(p);
    if (!(l >= minM) || !(l > 1e-8f)) { return false; }
    out[0] = p[0] / l; out[1] = p[1] / l; out[2] = p[2] / l;
    return true;
}

// The receiver's pole for one arm. Sources, in order: the sent direction (body frame, turned by the sender's yaw) when the sample has
// one; else this body's last good sent direction (`held`, body frame); else the caller's rule pole, left in `pole`. A sent direction
// updates `held`. Returns 0 sent / 1 held / 2 rule.
inline int ChooseElbowPole(bool sentOk, const float sent[3], float rootYaw, bool* heldOk, float held[3], float pole[3])
{
    if (sentOk) {
        held[0] = sent[0]; held[1] = sent[1]; held[2] = sent[2];
        *heldOk = true;
        coop::RotYVec(rootYaw, sent, pole);
        return 0;
    }
    if (*heldOk) {
        coop::RotYVec(rootYaw, held, pole);
        return 1;
    }
    return 2;
}

// The bend check: the angle (deg) between the bend direction a placed arm has (shoulder S, elbow E, hand H) and the pole it was
// given, both taken perpendicular to S->H. -1 when either has no perpendicular part.
inline float BendAngleDeg(const float S[3], const float E[3], const float H[3], const float pole[3])
{
    float got[3];
    if (!BendDir(S, E, H, 1e-6f, got)) { return -1.0f; }
    const float Sp[3] = { S[0] + pole[0], S[1] + pole[1], S[2] + pole[2] };
    float want[3];
    if (!BendDir(S, Sp, H, 1e-6f, want)) { return -1.0f; }
    float c = Dot3(got, want);
    c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
    return std::acos(c) * 57.2957795f;
}

// INV-736 (c): with the head-yaw decouple active the engine is told a yaw-free head (C1), and body-follows-head moves `folded` degrees of
// the head's yaw into the look fields (headfollow_logic.h: folded = h, h = 2 atan2(stripped.y, stripped.w) in degrees, negated when the
// decouple's flip is latched - boneblend.cpp camyaw). So the look the game shows (and sends) carries OpenXR yaw `FoldedLookYawRad` on top
// of the reported head; folded 0 -> 0 -> today's formula exactly.
inline float FoldedLookYawRad(float foldedDeg, bool flip)
{
    return (flip ? -foldedDeg : foldedDeg) * 0.01745329252f;
}
// The head the look shows = qY(yaw) x reported (x y z w).
inline void LookWithFold(const float reported[4], float yawRad, float out[4])
{
    float qy[4];
    QYaw(yawRad, qy);
    QMul(qy, reported, out);
}

// ---- spec 8.8: the look. The yaw + pitch part of an OpenXR head (forward = -Z, Y up): L = qY(yaw) x qX(pitch). Roll dropped.
inline void LookPart(const float q[4], float L[4])
{
    const float fwd0[3] = { 0.0f, 0.0f, -1.0f };
    float f[3];
    QRotate(q, fwd0, f);
    const float fy = f[1] > 1.0f ? 1.0f : (f[1] < -1.0f ? -1.0f : f[1]);
    const float yaw = std::atan2(-f[0], -f[2]);
    const float pitch = std::asin(fy);
    float qy[4], qx[4];
    QYaw(yaw, qy);
    QAxis(0, pitch, qx);
    QMul(qy, qx, L);
}
// D = real x inverse(LookPart(reported)), then into the body frame: qY(d) D qY(-d), d = bodyYaw - rootYaw (the sample's frame).
// "The head beyond what the game's look shows": the decouple's stripped yaw + the roll (spec 8.8).
inline void HeadBeyondLook(const float real[4], const float reported[4], float d, float out[4])
{
    float L[4], Li[4], D[4], qd[4], qdi[4], t[4];
    LookPart(reported, L);
    QConj(L, Li);
    QMul(real, Li, D);
    QYaw(d, qd);
    QYaw(-d, qdi);
    QMul(qd, D, t);
    QMul(t, qdi, out);
}

}  // namespace coopik
}  // namespace dlvr
