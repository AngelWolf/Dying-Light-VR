// Pure logic for the two-handed grip, Phase 1 (firearms) and Phase 2 (melee, section 7) -docs/SPEC-TWO-HANDED-GRIP-2026-10-09.md.
// No OpenXR, no Windows, no logging, so src/src/tests/twohand_logic_test.cpp compiles it on its own.
// twohand.cpp owns everything with side effects (poses, L1, the log).
// Frames: OpenXR tracking space, +Y up, a controller's forward is its -Z.
#pragma once

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace dlvr {
namespace twohand {

struct V3 { float x, y, z; };
struct Q4 { float x, y, z, w; };
struct Pose { Q4 q; V3 p; };

// Same constant as ovr2xr.cpp's vr_grip block and holster_logic.h - one grip threshold, not two.
constexpr float kGripThreshold = 0.5f;
// Hands closer than this: the line between them is too short to aim by, so the gun keeps its last direction.
constexpr float kMinSpan = 0.08f;

inline V3 Add(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline V3 Sub(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline V3 Scale(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline float Len(V3 a) { return std::sqrt(Dot(a, a)); }
inline V3 Norm(V3 a)
{
    const float l = Len(a);
    return l > 1e-9f ? Scale(a, 1.0f / l) : V3{ 0.0f, 0.0f, 0.0f };
}

inline Q4 QMul(Q4 a, Q4 b)
{
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
             a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
inline Q4 QConj(Q4 q) { return { -q.x, -q.y, -q.z, q.w }; }
inline Q4 QNorm(Q4 q)
{
    const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return l > 1e-9f ? Q4{ q.x / l, q.y / l, q.z / l, q.w / l } : Q4{ 0.0f, 0.0f, 0.0f, 1.0f };
}
inline V3 QRotate(Q4 q, V3 v)
{
    const V3 u{ q.x, q.y, q.z };
    const V3 t = Scale(Cross(u, v), 2.0f);
    return Add(Add(v, Scale(t, q.w)), Cross(u, t));
}
inline float QAngleDeg(Q4 a, Q4 b)
{
    float d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    if (d > 1.0f) { d = 1.0f; }
    return 2.0f * std::acos(d) * 57.2957795f;
}
// Rotation whose X / Y / Z axes are the given orthonormal columns.
inline Q4 QFromBasis(V3 X, V3 Y, V3 Z)
{
    const float m00 = X.x, m01 = Y.x, m02 = Z.x;
    const float m10 = X.y, m11 = Y.y, m12 = Z.y;
    const float m20 = X.z, m21 = Y.z, m22 = Z.z;
    const float tr = m00 + m11 + m22;
    Q4 q;
    if (tr > 0.0f) {
        const float s = std::sqrt(tr + 1.0f) * 2.0f;
        q = { (m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s };
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q = { 0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s };
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q = { (m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s };
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q = { (m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s };
    }
    return QNorm(q);
}
inline Q4 Slerp(Q4 a, Q4 b, float t)
{
    if (t <= 0.0f) { return a; }
    if (t >= 1.0f) { return b; }
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0.0f) { b = { -b.x, -b.y, -b.z, -b.w }; d = -d; }
    if (d > 0.9995f) {
        return QNorm({ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t });
    }
    const float th = std::acos(d);
    const float s = std::sin(th);
    const float wa = std::sin((1.0f - t) * th) / s;
    const float wb = std::sin(t * th) / s;
    return { a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb };
}
// A rotation whose forward (-Z) is dir and whose up is `up` made perpendicular to it. False when they are parallel.
inline bool Frame(V3 dir, V3 up, Q4* out)
{
    const V3 back = Scale(Norm(dir), -1.0f);
    const V3 x0 = Cross(up, back);
    if (Len(x0) < 1e-3f) { return false; }
    const V3 x = Norm(x0);
    const V3 y = Cross(back, x);
    *out = QFromBasis(x, y, back);
    return true;
}

// ---- mesh names (MUST match tools/twohand_meshes.py norm) ----
// Lower-case, strip every trailing ".msh", then ONE trailing "_hq". A name that does not fit is REFUSED (out = ""),
// never truncated - a truncated name could match a different weapon.
inline bool NormMesh(const char* in, char* out, size_t n)
{
    if (!out || n < 2) { return false; }
    out[0] = 0;
    if (!in) { return false; }
    size_t len = 0;
    for (; in[len] && len + 1 < n; ++len) { out[len] = static_cast<char>(std::tolower(static_cast<unsigned char>(in[len]))); }
    if (in[len]) { out[0] = 0; return false; }
    out[len] = 0;
    while (len >= 4 && std::strcmp(out + len - 4, ".msh") == 0) { len -= 4; out[len] = 0; }
    if (len >= 3 && std::strcmp(out + len - 3, "_hq") == 0) { len -= 3; out[len] = 0; }
    return len > 0;
}

// ---- classification (spec section 2) ----
enum class Kind { None = 0, Firearm = 1, Melee = 2 };
struct Class { Kind kind; const char* why; };

inline bool InList(const char* s, const char* const* list, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) {
        if (list[i] && std::strcmp(s, list[i]) == 0) { return true; }
    }
    return false;
}
inline Class Classify(const char* cls, const char* meshNorm, const char* const* twoHanded, unsigned nTwo,
                      const char* const* ambiguous, unsigned nAmb)
{
    if (!cls || !cls[0]) { return { Kind::None, "item class unreadable" }; }
    if (std::strcmp(cls, "BowVis") == 0) { return { Kind::None, "the bow - separate work (Jay 2026-10-09), never grabbed" }; }
    if (std::strcmp(cls, "CrossbowVis") == 0) {
        return { Kind::None, "the crossbow - excluded (Jay 2026-10-09: too small for manual-reload tolerances)" };
    }
    const bool gun = std::strcmp(cls, "FireWeaponVis") == 0;
    const bool melee = std::strcmp(cls, "WeaponVis") == 0;
    if (!gun && !melee) { return { Kind::None, "not a WeaponVis / FireWeaponVis" }; }
    if (!meshNorm || !meshNorm[0]) { return { Kind::None, "mesh name unreadable - TwoHanded cannot be looked up" }; }
    if (InList(meshNorm, ambiguous, nAmb)) {
        return { Kind::None, "mesh is TwoHanded(true) AND (false) in the data - ambiguous until the engine reader (spec S1)" };
    }
    if (!InList(meshNorm, twoHanded, nTwo)) { return { Kind::None, "TwoHanded(false), or the mesh is not in the data table" }; }
    return gun ? Class{ Kind::Firearm, "two-handed firearm" } : Class{ Kind::Melee, "two-handed melee" };
}

// ---- the firearm grab zone: a capsule ahead of the right hand along its AIM ray ----
struct Zone { bool in; float along, off; };
inline Zone GunZone(V3 rPos, V3 aimFwd, V3 lPos, float dMin, float dMax, float radius)
{
    const V3 f = Norm(aimFwd);
    const V3 v = Sub(lPos, rPos);
    const float along = Dot(v, f);
    const float off = Len(Sub(v, Scale(f, along)));
    return { along >= dMin && along <= dMax && off <= radius, along, off };
}

// ---- the settle blend: t = 0 one-handed, 1 fully two-handed ----
struct Blend { double tEdge; float t0; int dir; };   // dir +1 grabbing, -1 releasing, 0 idle
inline float BlendAt(const Blend& b, double now, float slideS)
{
    if (b.dir == 0) { return b.t0; }
    if (!(slideS > 0.0f)) { return b.dir > 0 ? 1.0f : 0.0f; }
    const float d = static_cast<float>((now - b.tEdge) / slideS);
    const float t = b.t0 + (b.dir > 0 ? d : -d);
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

// ---- the press machine (HOLD, never toggle - Jay 2026-10-09) ----
enum class State { Free, Gripped, Releasing };
struct Input {
    bool   squeezeActive;   // GetControllerSqueeze(left) returned true this poll
    float  squeeze;         // 0..1, meaningful only when squeezeActive
    bool   holsterBusy;     // a left holster press is active (it owns this press)
    Kind   kind;            // the right hand's item
    bool   posesOk;         // both grip poses and the right aim pose valid this poll
    bool   inZone;          // GunZone(...).in, meaningful only when posesOk
    double now;             // seconds
    float  slideS;          // rig_two_hand_slide_s
    bool   meleeOn;         // rig_two_hand_melee - two-handed MELEE is grabbable (phase 2, spec section 7)
    unsigned long long item;  // the right item pointer (0 = none): a different item while held cancels (review I-1)
};
struct Step {
    bool grab;          // grabbed this poll (the caller latches the gun NOW)
    bool release;       // grip released this poll -> easing back
    bool cancelled;     // let go for a reason other than the button (item gone, squeeze inactive)
    bool refusedPress;  // a press that is NOT a grab - it reaches the game as L1
    bool suppressL1;    // do NOT OR L1 this poll
    bool resumed;       // with grab: it landed DURING the ease-back - keep the existing latch, do NOT latch anew (a new
                        // latch targets the controller's own orientation, so the hand would jump by t x the old offset)
    float t;            // the blend after this poll
    const char* why;    // the reason for grab / refusal / cancel, "" otherwise
};

class Machine {
public:
    Step Update(const Input& in)
    {
        Step st{};
        st.why = "";
        const bool down = in.squeezeActive && in.squeeze >= kGripThreshold;
        if (!primed_) {
            // First poll ever: a grip already held is a press we did not see start - never a grab.
            primed_ = true;
            down_ = down;
            st.t = T(in);
            return st;
        }
        if (state_ != State::Free &&
            (in.kind != held_ || in.item != heldItem_ || (held_ == Kind::Melee && !in.meleeOn))) {
            // The weapon is gone or changed (holstered, swapped - even for one of the same kind, dropped), or melee was switched
            // off: let go NOW, no blend.
            const bool changed = in.kind != held_ || in.item != heldItem_;
            Reset();
            down_ = down;
            st.cancelled = true;
            st.why = changed ? "the right hand no longer holds the two-handed weapon it gripped"
                             : "rig_two_hand_melee was switched off while gripping";
            return st;
        }
        if (state_ == State::Gripped && !in.squeezeActive) {
            // An inactive action is not a release, but we cannot keep holding on a button we cannot read.
            Start(State::Releasing, -1, in);
            st.cancelled = true;
            st.why = "the left squeeze action went inactive";
        } else if (down && !down_) {
            if (in.holsterBusy) { st.refusedPress = true; st.why = "a left holster took this press"; }
            else if (in.kind == Kind::Melee && !in.meleeOn) { st.refusedPress = true; st.why = "two-handed melee is off (rig_two_hand_melee=0)"; }
            else if (in.kind == Kind::None) { st.refusedPress = true; st.why = "the right hand holds no two-handed weapon"; }
            else if (!in.posesOk) { st.refusedPress = true; st.why = "a controller pose is missing"; }
            else if (!in.inZone) { st.refusedPress = true; st.why = "the left hand is outside the grab zone"; }
            else {
                st.resumed = state_ == State::Releasing;
                held_ = in.kind;
                heldItem_ = in.item;
                Start(State::Gripped, +1, in);
                st.grab = true;
                st.why = st.resumed ? "grabbed again during the ease-back - the hold resumes" : "grabbed";
            }
        } else if (!down && down_ && state_ == State::Gripped) {
            Start(State::Releasing, -1, in);
            st.release = true;
            st.why = "released";
        }
        down_ = down;
        st.t = T(in);
        if (state_ == State::Releasing && st.t <= 0.0f) { Reset(); }
        st.suppressL1 = state_ == State::Gripped;
        return st;
    }
    State state() const { return state_; }
    Blend blend() const { return blend_; }
    void Reset()
    {
        state_ = State::Free;
        blend_ = Blend{ 0.0, 0.0f, 0 };
        held_ = Kind::None;
        heldItem_ = 0;
    }
    Kind heldKind() const { return held_; }                 // the kind gripped (None when Free)
    unsigned long long heldItem() const { return heldItem_; }

private:
    float T(const Input& in) const { return BlendAt(blend_, in.now, in.slideS); }
    void Start(State s, int dir, const Input& in)
    {
        const float t = T(in);   // continue from where the blend is, so a re-grab mid-release does not jump
        state_ = s;
        blend_ = Blend{ in.now, t, dir };
    }
    bool  primed_ = false;
    bool  down_   = false;
    State state_  = State::Free;
    Blend blend_  = { 0.0, 0.0f, 0 };
    Kind  held_   = Kind::None;   // the kind gripped (a change while held cancels)
    unsigned long long heldItem_ = 0;   // the item gripped (a change while held cancels)
};

// ---- the gun: latched at the grab, aimed by the right wrist AND the hand line while gripped ----
// 2026-11-08 (Jay, RUN TH1: "very restrictive", "Hard to move, gun was flipping in the hands"): the right controller aims
// the gun exactly as it does one-handed, and the line between the hands pulls the barrel toward itself by a SHARE
// (rig_two_hand_aim_share; 1 = the hand line alone). The pull is a shortest-arc swing, which never adds a twist about the
// barrel, so the roll is always the right wrist's own - there is no up vector, and nothing in the maths can flip. The old
// rebuild took the roll from the right controller's up laid around the hand line, which turned ill-conditioned (up to
// 180 deg of roll over a few degrees of hand motion) whenever the line neared that up - the gun turned ~90 deg from the
// right controller, RUN TH1's peaks 90.9 / 90.5 / 84.3 deg.
// lineLocal : the hand line in the right controller's frame at the grab - where the line would be if only the wrist moved.
// lRel      : the left hand's rotation relative to the right hand at the grab - the drawn left hand stays on the gun.
// lastAxis / lastAngle : the last full swing, held while the line points back past the right hand (no unique shortest arc).
struct GunLatch { Q4 lRel; V3 lineLocal; V3 lastAxis; float lastAngle; };
// The hand line's share fades in with the span between the hands: none at kMinSpan, all of it from kFullSpan (a short line
// is noisy to aim by). Below kMinSpan the wrist aims alone - no hold, no snap when the hands part again.
constexpr float kFullSpan = 0.16f;
// cos of the angle past which the line counts as pointing back past the right hand (~170 deg): the last swing is held.
constexpr float kOppositeCos = -0.985f;
inline float SpanFade(float span)
{
    const float x = (span - kMinSpan) / (kFullSpan - kMinSpan);
    if (x <= 0.0f) { return 0.0f; }
    if (x >= 1.0f) { return 1.0f; }
    return x * x * (3.0f - 2.0f * x);
}
inline bool LatchGun(const Pose& r, const Pose& l, GunLatch* out)
{
    const V3 d = Sub(l.p, r.p);
    if (Len(d) < kMinSpan) { return false; }
    out->lineLocal = QRotate(QConj(r.q), Norm(d));
    out->lRel = QMul(QConj(r.q), l.q);
    out->lastAxis = V3{ 0.0f, 1.0f, 0.0f };
    out->lastAngle = 0.0f;
    return true;
}
// heldDir = the hands are under kMinSpan apart (the wrist aims alone this frame); heldUp = the line points back past the right
// hand (the last swing is held). failed is never set - kept so callers' counters stay one field each.
// share = the share applied this frame (the setting x the span fade); swingDeg = how far the line turned the gun from the wrist.
struct GunOut { Q4 r, l; bool heldDir, heldUp, failed; float share, swingDeg; };
// Positions are untouched - both hands stay at their controllers; only the orientations are rebuilt.
inline GunOut GunTargets(const Pose& r, const Pose& l, GunLatch* g, float share)
{
    GunOut o{};
    const V3 d = Sub(l.p, r.p);
    const float span = Len(d);
    const float s01 = share < 0.0f ? 0.0f : (share > 1.0f ? 1.0f : share);
    const float k = s01 * SpanFade(span);
    float ang = 0.0f;
    V3 axis = g->lastAxis;
    if (span < kMinSpan) {
        o.heldDir = true;
    } else {
        const V3 want = Norm(d);
        const V3 have = QRotate(r.q, g->lineLocal);
        const float c = Dot(have, want);
        const V3 x = Cross(have, want);
        const float sn = Len(x);
        if (c < kOppositeCos) {
            o.heldUp = true;
            ang = g->lastAngle;
        } else if (sn > 1e-7f) {
            axis = Scale(x, 1.0f / sn);
            ang = std::atan2(sn, c);
            g->lastAxis = axis;
            g->lastAngle = ang;
        } else {
            g->lastAngle = 0.0f;
        }
    }
    const float h = 0.5f * ang * k;
    const float sh = std::sin(h);
    const Q4 swing{ axis.x * sh, axis.y * sh, axis.z * sh, std::cos(h) };
    o.r = QNorm(QMul(swing, r.q));
    o.l = QNorm(QMul(o.r, g->lRel));
    o.share = k;
    o.swingDeg = ang * k * 57.2957795f;
    return o;
}

// ---- phase 1b: rigid transforms (row-major 3x3, basis in the COLUMNS, p' = R p + t - the snapshot / accessor layout) ----
struct Xf { float R[9]; float t[3]; };
inline void M3Mul(const float a[9], const float b[9], float out[9])
{
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out[3 * r + c] = a[3 * r] * b[c] + a[3 * r + 1] * b[3 + c] + a[3 * r + 2] * b[6 + c];
        }
    }
}
inline void M3T(const float a[9], float out[9])
{
    for (int r = 0; r < 3; ++r) { for (int c = 0; c < 3; ++c) { out[3 * r + c] = a[3 * c + r]; } }
}
inline Xf XfFrom34(const float m[12])
{
    return { { m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10] }, { m[3], m[7], m[11] } };
}
inline Xf XfMul(const Xf& a, const Xf& b)
{
    Xf o{};
    M3Mul(a.R, b.R, o.R);
    for (int r = 0; r < 3; ++r) { o.t[r] = a.R[3 * r] * b.t[0] + a.R[3 * r + 1] * b.t[1] + a.R[3 * r + 2] * b.t[2] + a.t[r]; }
    return o;
}
inline Xf XfInvRigid(const Xf& a)
{
    Xf o{};
    M3T(a.R, o.R);
    for (int r = 0; r < 3; ++r) { o.t[r] = -(o.R[3 * r] * a.t[0] + o.R[3 * r + 1] * a.t[1] + o.R[3 * r + 2] * a.t[2]); }
    return o;
}
// Gram-Schmidt on the columns: x normalised, y made perpendicular to x, z = x cross y. Strips scale (INV-136's item scale).
inline bool Ortho3(const float in[9], float out[9])
{
    V3 x{ in[0], in[3], in[6] }, y{ in[1], in[4], in[7] };
    if (Len(x) < 1e-6f) { return false; }
    x = Norm(x);
    y = Sub(y, Scale(x, Dot(x, y)));
    if (Len(y) < 1e-6f) { return false; }
    y = Norm(y);
    const V3 z = Cross(x, y);
    out[0] = x.x; out[3] = x.y; out[6] = x.z;
    out[1] = y.x; out[4] = y.y; out[7] = y.z;
    out[2] = z.x; out[5] = z.y; out[8] = z.z;
    return true;
}
inline void QToMat3(Q4 q, float m[9])
{
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y - z * w);     m[2] = 2 * (x * z + y * w);
    m[3] = 2 * (x * y + z * w);     m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z - x * w);
    m[6] = 2 * (x * z - y * w);     m[7] = 2 * (y * z + x * w);     m[8] = 1 - 2 * (x * x + y * y);
}
inline Q4 Mat3ToQ(const float m[9])
{
    return QFromBasis({ m[0], m[3], m[6] }, { m[1], m[4], m[7] }, { m[2], m[5], m[8] });
}
// holderl relative to the RIGHT snapshot frame (drawn basis + achieved handholder position) - a per-item constant, because
// ItemCtlPlace builds the gun from exactly that frame (boneblend.cpp:16545-16576).
inline bool LatchRel(const Xf& rightFrame, const Xf& holderlW, Xf* rel)
{
    Xf f = rightFrame;
    if (!Ortho3(rightFrame.R, f.R)) { return false; }
    *rel = XfMul(XfInvRigid(f), holderlW);
    return true;
}
struct PinOut { float cWorld[9]; float hhPos[3]; float wristPos[3]; };
// THIS frame's holderl = right frame now x rel. POSITION ONLY (INV-501, run 2: forcing the hand into holderl's AXES reversed the
// left hand - holderl's rotation is not a hand orientation; the game's own CorrectHandPosition moves only the POSITION onto
// HolderL, INV-295). The hand keeps the rotation the solve already has (cWorldIn = phase 1's grab-latched one); only the wrist
// target moves so that the hand AS DRAWN puts its l_handholder on holderl. SolveArmIk DRAWS hand = (cWorld x grip) x post, post =
// the authored rest (rig_grip_compose_rest) x the CorrectHandLocal dials (boneblend.cpp:13339-13346), so D = Ortho(cWorldIn x
// grip x post) and wrist = holderl - D x t_hh (the achieved-grip formula, :13421). out->cWorld = cWorldIn, unchanged.
// palm = the point the pin holds, as an offset from the handholder in the hand bone's frame (INV-502 (a): the handholder is
// the knuckle line; the palm centre is ~(-0.046, +0.004, -0.003) from it). {0,0,0} = pin the handholder itself.
inline bool PinLeftHand(const Xf& rightFrame, const Xf& rel, const float hh12[12], const float palm[3], const float grip[9],
                        const float post[9], const float cWorldIn[9], PinOut* out, const char** why)
{
    Xf f = rightFrame;
    if (!Ortho3(rightFrame.R, f.R)) { *why = "the right hand's drawn basis is degenerate"; return false; }
    // the grip calibration must be a rotation: |G G^T - I| small
    float gT[9], ggT[9];
    M3T(grip, gT);
    M3Mul(grip, gT, ggT);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            if (std::fabs(ggT[3 * r + c] - (r == c ? 1.0f : 0.0f)) > 0.02f) {
                *why = "the left grip calibration is not a pure rotation";
                return false;
            }
        }
    }
    const Xf hl = XfMul(f, rel);
    float postO[9], cg[9], drawn[9], hand[9];
    if (!Ortho3(post, postO)) { *why = "the hand's post-rotation (rest x dials) is degenerate"; return false; }
    M3Mul(cWorldIn, grip, cg);
    M3Mul(cg, postO, drawn);                     // the hand bone as the solve will DRAW it, from the rotation it already has
    if (!Ortho3(drawn, hand)) { *why = "the hand's drawn basis is degenerate"; return false; }
    for (int i = 0; i < 9; ++i) { out->cWorld[i] = cWorldIn[i]; }
    out->hhPos[0] = hl.t[0]; out->hhPos[1] = hl.t[1]; out->hhPos[2] = hl.t[2];
    const float tx = hh12[3] + palm[0], ty = hh12[7] + palm[1], tz = hh12[11] + palm[2];
    for (int r = 0; r < 3; ++r) {
        out->wristPos[r] = hl.t[r] - (hand[3 * r] * tx + hand[3 * r + 1] * ty + hand[3 * r + 2] * tz);
    }
    *why = "";
    return true;
}
// INV-502 (c): a point given in holderl's own frame, in world: hl.t + hl.R * p (the forend offset; the barrel self-check).
inline void ForendTarget(const Xf& hl, const float p[3], float out[3])
{
    for (int r = 0; r < 3; ++r) { out[r] = hl.t[r] + hl.R[3 * r] * p[0] + hl.R[3 * r + 1] * p[1] + hl.R[3 * r + 2] * p[2]; }
}
// The self-check: distance (m) between the barrel element predicted from holderl's world and the barrel's real world.
inline float ForendCheckErr(const Xf& hl, const float barrelHl[3], const float barrelWorld[3])
{
    float p[3];
    ForendTarget(hl, barrelHl, p);
    const float dx = p[0] - barrelWorld[0], dy = p[1] - barrelWorld[1], dz = p[2] - barrelWorld[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
// The HANDPOSE probe: a world hand rotation + palm point, expressed in a frame (the gun's holderl): q = quat(F.R^T * hand),
// p = F.R^T * (palm - F.t). F.R must be a rotation (the caller orthonormalises it).
inline void HandInFrame(const Xf& f, const float hand[9], const float palmW[3], float q[4], float p[3])
{
    float ft[9], rl[9];
    M3T(f.R, ft);
    M3Mul(ft, hand, rl);
    const Q4 qq = Mat3ToQ(rl);
    q[0] = qq.x; q[1] = qq.y; q[2] = qq.z; q[3] = qq.w;
    const float d[3] = { palmW[0] - f.t[0], palmW[1] - f.t[1], palmW[2] - f.t[2] };
    for (int r = 0; r < 3; ++r) { p[r] = ft[3 * r] * d[0] + ft[3 * r + 1] * d[1] + ft[3 * r + 2] * d[2]; }
}
// INV-506: the cWorld whose DRAWN hand (cWorld * grip * post) is frame * std. cWorld = frame * std * (grip * post)^T, with
// grip * post orthonormalised. False = a degenerate input (the caller keeps its own cWorld).
inline bool StandardCWorld(const float frame[9], Q4 std4, const float grip[9], const float post[9], float out[9])
{
    float gp[9], gpO[9], gpT[9], sm[9], fs[9];
    M3Mul(grip, post, gp);
    if (!Ortho3(gp, gpO)) { return false; }
    M3T(gpO, gpT);
    QToMat3(QNorm(std4), sm);
    M3Mul(frame, sm, fs);
    M3Mul(fs, gpT, out);
    return true;
}
// Rotation-matrix slerp (through quaternions; Slerp takes the short way).
inline void SlerpM3(const float a[9], const float b[9], float t, float out[9])
{
    float ao[9], bo[9];
    if (!Ortho3(a, ao) || !Ortho3(b, bo)) {
        for (int i = 0; i < 9; ++i) { out[i] = a[i]; }
        return;
    }
    QToMat3(Slerp(Mat3ToQ(ao), Mat3ToQ(bo), t), out);
}
// Jay 2026-10-09: the rifle grab area - a strip along holderl's z of half-length h, centred on holderl. d = the left grip point
// in holderl's frame. along = d.z clamped to [-h, h] (where on the strip the hand is); norm = the ellipsoidal distance from the
// strip, X over rx, Y and the end caps over ry; < 1 = inside.
inline void SlideZone(const float d[3], float h, float rx, float ry, float* along, float* norm)
{
    const float a = d[2] < -h ? -h : (d[2] > h ? h : d[2]);
    const float ex = d[0] / rx, ey = d[1] / ry, ez = (d[2] - a) / ry;
    *along = a;
    *norm = std::sqrt(ex * ex + ey * ey + ez * ez);
}
// The pin point offset by o in holderl's own frame: rel * translate(o) (INV-508 rifle palm offset).
inline Xf OffsetRel(const Xf& rel, const float o[3])
{
    Xf out = rel;
    for (int r = 0; r < 3; ++r) { out.t[r] = rel.t[r] + rel.R[3 * r] * o[0] + rel.R[3 * r + 1] * o[1] + rel.R[3 * r + 2] * o[2]; }
    return out;
}
// The pin point slid s along holderl's own z: rel * translate(0, 0, s).
inline Xf SlideRel(const Xf& rel, float s)
{
    Xf o = rel;
    for (int r = 0; r < 3; ++r) { o.t[r] = rel.t[r] + rel.R[3 * r + 2] * s; }
    return o;
}
// INV-507 fix 1: the pinned left arm's reach - `mapped` (rig_reach_scale + rig_reach_soft, for CONTROLLER targets) blended to
// `raw` (the target's true distance; a pin target is already in the rig's reach space) by the pin's t, clamped 0..1. The hard
// cap at the arm's length is applied after this by the caller, unchanged.
inline float PinnedReach(float mapped, float raw, float t)
{
    const float u = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return mapped + u * (raw - mapped);
}
// INV-507 fix 2: the point the grab zone tests - the DRAWN palm centre (achieved handholder + drawn * palm), so the player grabs
// where they SEE the hand; useDrawn false = the raw controller grip point.
inline void ZonePoint(bool useDrawn, const float achieved[3], const float drawn[9], const float palm[3], const float gripWorld[3],
                      float out[3])
{
    for (int r = 0; r < 3; ++r) {
        out[r] = useDrawn ? achieved[r] + drawn[3 * r] * palm[0] + drawn[3 * r + 1] * palm[1] + drawn[3 * r + 2] * palm[2]
                          : gripWorld[r];
    }
}
// INV-503 (b): the wrist overflow in effect. Not gripping: the normal one, capped at 60 (as ClampWristSwing always did).
// Gripping: the larger of the two, capped at 90 (flex 85 + 90 = 175 < 180, clear of D-RIG-4's degenerate axis).
inline float WristOverflowDeg(float normal, float twoHand, bool gripping)
{
    const float n = normal < 0.0f ? 0.0f : (normal > 60.0f ? 60.0f : normal);
    if (!gripping) { return n; }
    float g = twoHand > n ? twoHand : n;
    return g > 90.0f ? 90.0f : g;
}
// INV-502 (b) measurement: bin how far (deg) a wrist swing asked past its final (overflowed) clamp limit.
// Bins: (0,10) [10,20) [20,30) [30,45) [45,60) [60,90) [90,..). -1 = at or inside the limit.
constexpr int kExcessBins = 7;
inline int ExcessBin(float deg)
{
    if (!(deg > 0.0f)) { return -1; }
    static const float kEdge[kExcessBins - 1] = { 10.0f, 20.0f, 30.0f, 45.0f, 60.0f, 90.0f };
    for (int i = 0; i < kExcessBins - 1; ++i) {
        if (deg < kEdge[i]) { return i; }
    }
    return kExcessBins - 1;
}
// Point zone with hysteresis: enter below r, leave above 1.2 r (holster_logic.h's kExitScale).
struct SphereZoneState { bool in; };
inline bool SphereZone(SphereZoneState* s, const float p[3], const float c[3], float radius, float* dist)
{
    const float dx = p[0] - c[0], dy = p[1] - c[1], dz = p[2] - c[2];
    *dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    s->in = s->in ? (*dist <= radius * 1.2f) : (*dist < radius);
    return s->in;
}
// Minimum-gap gate for the near-grab hint pulse - holster_logic.h's RateGate, in this namespace.
class RateGate {
public:
    bool Allow(unsigned long long nowMs, unsigned long long minGapMs)
    {
        if (!armed_ || nowMs - last_ >= minGapMs) { armed_ = true; last_ = nowMs; return true; }
        return false;
    }

private:
    bool armed_ = false;
    unsigned long long last_ = 0;
};

// ======================= Phase 2: two-handed MELEE (spec section 7) =======================
// THE SHAFT AXIS u: the shaft runs along the MESH's +Y (INV-499), but on the axe / sledgehammer / bat the `holder` node is TILTED
// 10.82 deg about -X in the mesh, so in the Holder frame the runtime reads the shaft is (0, 0.9822, 0.1876) - NOT Holder +Y
// (tools/twohand_melee_pose.py emits it as kMeleeShaftAxis). Every "along the shaft" below takes u explicitly.
// The default floor of the melee grab strip along u: the deepest butt in the mesh data (sledgehammer -0.158 m, axe -0.104;
// INV-499). The runtime takes it from rig_two_hand_melee_butt_cm (Jay: "as long as we can adjust it").
constexpr float kMeleeButtY = -0.16f;

// Melee by the item desc's TwoHanded flag (INV-498, slot 176 - the table is ambiguous for 3 melee meshes, D-204); firearms keep
// phase 1's mesh table (verified, INV-509). Bow / crossbow are not WeaponVis and are refused by Classify, unchanged. The chainsaw
// is refused by name - Jay 2026-10-09: it has a different grab point and "will be its own case".
inline Class ClassifyWithDesc(const char* cls, const char* meshNorm, bool descOk, bool descTwoHanded,
                              const char* const* twoHanded, unsigned nTwo, const char* const* ambiguous, unsigned nAmb)
{
    if (cls && std::strcmp(cls, "WeaponVis") == 0) {
        if (meshNorm && std::strstr(meshNorm, "chainsaw")) {
            // Jay 2026-10-09: "its own case" (a different grab point) -> 2026-11-09: "Also need to add the second hands grip for the
            // chainsaw" - a FIREARM grip at its front hoop (holderl sits on the hoop's top bar; Jay's pose = its twohand_shotgun.inc row)
            return { Kind::Firearm, "the chainsaw - two-handed at its front hoop (Jay 2026-11-09)" };
        }
        if (!descOk) { return { Kind::None, "melee: the desc TwoHanded flag (slot 176) could not be read" }; }
        return descTwoHanded ? Class{ Kind::Melee, "two-handed melee (desc slot 176 set)" }
                             : Class{ Kind::None, "melee, TwoHanded(false) by the desc flag (slot 176)" };
    }
    return Classify(cls, meshNorm, twoHanded, nTwo, ambiguous, nAmb);
}
// INV-644: the game's own two-handed set is the ItemType - 15 TwoHandedHeavyStick, 16 TwoHandedCloseStick, 17 TwoHandedStick (OUR
// gamedll +6F12E0; desc slot 56, INV-571). The TwoHanded() flag misses rebar a/b/c, showel a/b and short_sword_c (type two-handed, flag
// false), so melee = the type OR the flag. Firearms, bow / crossbow and the chainsaw are decided by ClassifyWithDesc, unchanged; an
// unreadable type falls back to the flag alone.
inline bool IsTwoHandedItemType(int t) { return t >= 15 && t <= 17; }
inline Class ClassifyWithType(const char* cls, const char* meshNorm, bool descOk, bool descTwoHanded, bool typeOk, int itemType,
                              const char* const* twoHanded, unsigned nTwo, const char* const* ambiguous, unsigned nAmb)
{
    const Class c = ClassifyWithDesc(cls, meshNorm, descOk, descTwoHanded, twoHanded, nTwo, ambiguous, nAmb);
    if (c.kind != Kind::None || !typeOk || !IsTwoHandedItemType(itemType) || !cls || std::strcmp(cls, "WeaponVis") != 0 ||
        (meshNorm && std::strstr(meshNorm, "chainsaw"))) {
        return c;
    }
    // BETA (2026-10-28, beta_textscan hit): this reason ships as readable text (a %s value) - the INV reference is dropped here
    // (leak hardening); src/ keeps it.
    return { Kind::Melee, "two-handed melee by the ItemType (15..17) - the TwoHanded flag is clear or unreadable" };
}
// w expressed in frame f (f.R orthonormal): f.R^T (w - f.t).
inline void PointInFrame(const Xf& f, const float w[3], float out[3])
{
    const float d[3] = { w[0] - f.t[0], w[1] - f.t[1], w[2] - f.t[2] };
    for (int c = 0; c < 3; ++c) { out[c] = f.R[c] * d[0] + f.R[3 + c] * d[1] + f.R[6 + c] * d[2]; }
}
// Jay 2026-10-09, choice (a): the melee grab area = the shaft BELOW the right hand. d = the left grip point in the Holder frame,
// u = the shaft axis there (unit). along = d.u clamped to [yLo, yHi] (yHi below yLo collapses to yLo - never an inverted strip);
// norm = distance from that segment over r; < 1 = inside.
inline void MeleeZone(const float d[3], const float u[3], float yLo, float yHi, float r, float* along, float* norm)
{
    const float hi = yHi < yLo ? yLo : yHi;
    const float s = d[0] * u[0] + d[1] * u[1] + d[2] * u[2];
    const float a = s < yLo ? yLo : (s > hi ? hi : s);
    const float e[3] = { d[0] - a * u[0], d[1] - a * u[1], d[2] - a * u[2] };
    *along = a;
    *norm = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]) / r;
}
// The shaft for THIS weapon, from its left node in the Holder frame (holderO.R orthonormal). A left node >= 5 cm up gives the
// direction itself (u = its unit vector, slide = its length: INV-499 mesh 0.347-0.350 axe / sledgehammer); closer (the bat, 0)
// gives no direction, so u = uDefault (kMeleeShaftAxis), slide = its component along it. ok = u within 15 deg of uDefault and the
// node not below holder; otherwise this mesh's shaft is unknown and melee refuses it. offDeg = u's angle from uDefault.
struct ShaftCheck { float slide, offDeg; float u[3]; bool ok; };
inline ShaftCheck MeleeShaft(const Xf& holderO, const float leftNodeW[3], const float uDefault[3])
{
    float v[3];
    PointInFrame(holderO, leftNodeW, v);
    const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    ShaftCheck s{};
    if (len < 0.05f) {
        for (int i = 0; i < 3; ++i) { s.u[i] = uDefault[i]; }
        s.slide = v[0] * uDefault[0] + v[1] * uDefault[1] + v[2] * uDefault[2];
        s.offDeg = 0.0f;
        s.ok = s.slide >= -0.01f;
        return s;
    }
    for (int i = 0; i < 3; ++i) { s.u[i] = v[i] / len; }
    s.slide = len;
    float c = s.u[0] * uDefault[0] + s.u[1] * uDefault[1] + s.u[2] * uDefault[2];
    c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
    s.offDeg = std::acos(c) * 57.2957795f;
    s.ok = s.offDeg <= 15.0f;
    return s;
}
// INV-644: the shaft of a weapon with NO left node but a grip row (twohand_grip.inc): the row's own u (normalised), slide 0 (the bat's
// case - the left node at holder), offDeg = its angle from uDefault (logged; the golf club's untilted holder reads 10.8). ok = u usable.
inline ShaftCheck MeleeShaftFromRow(const float rowU[3], const float uDefault[3])
{
    ShaftCheck s{};
    const float len = std::sqrt(rowU[0] * rowU[0] + rowU[1] * rowU[1] + rowU[2] * rowU[2]);
    s.ok = len > 0.5f;
    for (int i = 0; i < 3; ++i) { s.u[i] = s.ok ? rowU[i] / len : uDefault[i]; }
    s.slide = 0.0f;
    float c = s.u[0] * uDefault[0] + s.u[1] * uDefault[1] + s.u[2] * uDefault[2];
    c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
    s.offDeg = std::acos(c) * 57.2957795f;
    return s;
}
// INV-644: the slide's sanity range for a weapon with a grip row. The game holds those weapons with the right handholder up to ~0.10 m
// along the shaft from Holder at rest (bats / short_sword_c +0.096 in every log, with a left node at holder = slide 0), so the plain
// range (-0.05 .. slide + 0.05) refused every bat slide (16 / 16 MELEE SLIDE REFUSED in the archived logs). With a row the upper end is
// at least kMeleeGripIdleMax + 0.05; a left node further up (the axes' 0.347) keeps its own.
constexpr float kMeleeGripIdleMax = 0.15f;
inline float MeleeSlideRangeTop(bool gripRow, float slide)
{
    return (gripRow && slide < kMeleeGripIdleMax) ? kMeleeGripIdleMax : slide;
}
// Run R5: move a point d metres toward the hand's fingers = along wrist -> handholder (hh, the handholder in the hand-bone frame; the
// wrist is that frame's origin). A handholder at the origin gives no direction: unchanged.
inline void TowardFingers(const float hh[3], float d, float p[3])
{
    const float len = std::sqrt(hh[0] * hh[0] + hh[1] * hh[1] + hh[2] * hh[2]);
    if (!(len > 1e-3f)) { return; }
    for (int i = 0; i < 3; ++i) { p[i] += hh[i] / len * d; }
}
// Run R6 live tuning: turn the weapon (relR = its basis in the hand-bone frame, row-major, columns = the weapon's axes; relT = its
// position there) about the handholder hh by rotations about the hand-bone x, then y, then z (degrees * k), and move it by off * k
// (metres, hand-bone frame). k = the grip's blend 0..1. Q = Rz Ry Rx; relR' = Q relR; relT' = hh + Q (relT - hh) + off k.
inline void GripTune(const float hh[3], const float off[3], const float degXYZ[3], float k, float relR[9], float relT[3])
{
    const float d2r = 0.01745329252f;
    const float ax = degXYZ[0] * k * d2r, ay = degXYZ[1] * k * d2r, az = degXYZ[2] * k * d2r;
    const float cx = std::cos(ax), sx = std::sin(ax), cy = std::cos(ay), sy = std::sin(ay), cz = std::cos(az), sz = std::sin(az);
    const float Rx[9] = { 1, 0, 0, 0, cx, -sx, 0, sx, cx };
    const float Ry[9] = { cy, 0, sy, 0, 1, 0, -sy, 0, cy };
    const float Rz[9] = { cz, -sz, 0, sz, cz, 0, 0, 0, 1 };
    float Ryx[9], Q[9], R2[9];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            Ryx[3 * r + c] = Ry[3 * r] * Rx[c] + Ry[3 * r + 1] * Rx[3 + c] + Ry[3 * r + 2] * Rx[6 + c];
        }
    }
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            Q[3 * r + c] = Rz[3 * r] * Ryx[c] + Rz[3 * r + 1] * Ryx[3 + c] + Rz[3 * r + 2] * Ryx[6 + c];
        }
    }
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            R2[3 * r + c] = Q[3 * r] * relR[c] + Q[3 * r + 1] * relR[3 + c] + Q[3 * r + 2] * relR[6 + c];
        }
    }
    const float v[3] = { relT[0] - hh[0], relT[1] - hh[1], relT[2] - hh[2] };
    for (int r = 0; r < 3; ++r) {
        relT[r] = hh[r] + Q[3 * r] * v[0] + Q[3 * r + 1] * v[1] + Q[3 * r + 2] * v[2] + off[r] * k;
    }
    for (int i = 0; i < 9; ++i) { relR[i] = R2[i]; }
}
// The right HANDHOLDER's position along the shaft u (Holder frame) from Holder, from the grip transform ItemCtlPlace uses (relR /
// relT = Holder in the right hand BONE frame: basis = drawn * relR, position = wrist + drawn * relT) and the handholder offset in
// that frame (hhT). 0 = the hand at holder (the swing pose); ~+0.35 = at the left node (the idle hold, INV-499).
inline bool MeleeHandAlong(const float relR[9], const float relT[3], const float hhT[3], const float u[3], float* a)
{
    float Ro[9];
    if (!Ortho3(relR, Ro)) { return false; }
    const float v[3] = { hhT[0] - relT[0], hhT[1] - relT[1], hhT[2] - relT[2] };
    float s = 0.0f;
    for (int r = 0; r < 3; ++r) { s += (Ro[3 * r] * u[0] + Ro[3 * r + 1] * u[1] + Ro[3 * r + 2] * u[2]) * v[r]; }   // (Ro u) . v
    *a = s;
    return true;
}
// A hold latched with the hand outside [-0.05, slide + 0.05] along the shaft (a grab mid-swing: the animation already slid it)
// does not slide - the weapon stays where it was gripped.
inline bool MeleeSlideInRange(float a, float slide) { return a >= -0.05f && a <= slide + 0.05f; }
// The ABSOLUTE slide (spec 7.1): the LATCHED transform moved a along the shaft (Ro u) so Holder comes to the handholder, blended
// from the LIVE transform by t. t <= 0 = live exactly; t >= 1 = latched + slide exactly, whatever the animation does to live.
// Between, the rotation is slerped (orthonormal - ItemCtlPlace's engine scale step re-normalises and re-scales after this).
inline void MeleeRel(const float liveR[9], const float liveT[3], const float latR[9], const float latT[3], const float u[3],
                     float a, float t, float outR[9], float outT[3])
{
    const float k = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    float Ro[9];
    if (!Ortho3(latR, Ro)) {
        for (int i = 0; i < 9; ++i) { outR[i] = liveR[i]; }
        for (int i = 0; i < 3; ++i) { outT[i] = liveT[i]; }
        return;
    }
    for (int r = 0; r < 3; ++r) {
        const float target = latT[r] + a * (Ro[3 * r] * u[0] + Ro[3 * r + 1] * u[1] + Ro[3 * r + 2] * u[2]);
        outT[r] = liveT[r] + k * (target - liveT[r]);
    }
    if (k >= 1.0f) { for (int i = 0; i < 9; ++i) { outR[i] = latR[i]; } }
    else if (k <= 0.0f) { for (int i = 0; i < 9; ++i) { outR[i] = liveR[i]; } }
    else { SlerpM3(liveR, latR, k, outR); }
}
// The S4 finger local with the runtime curl: base post-multiplied by deg about axis (the generated LEFT-local curl axis -
// tools/twohand_melee_pose.py derives it and checks it equal on every curling row). curls false (thumb) = base unchanged.
inline void CurlFinger(const float base[9], const float axis[3], float deg, bool curls, float out[9])
{
    if (!curls || deg == 0.0f) {
        for (int i = 0; i < 9; ++i) { out[i] = base[i]; }
        return;
    }
    const float an = deg * 0.0174532925f, c = std::cos(an), s = std::sin(an), k = 1.0f - c;
    const float x = axis[0], y = axis[1], z = axis[2];
    const float Rr[9] = { k * x * x + c,     k * x * y - s * z, k * x * z + s * y,
                          k * x * y + s * z, k * y * y + c,     k * y * z - s * x,
                          k * x * z - s * y, k * y * z + s * x, k * z * z + c };
    M3Mul(base, Rr, out);
}
// A finger pin's local while gripping: rotation slerped authored -> grip by t; translation authored (rigid bones).
// authored12 / out12 = the pin layout (3x4 row-major, basis in the columns, t at 3 / 7 / 11).
inline void FingerBlend(const float authored12[12], const float grip9[9], float t, float out12[12])
{
    float a9[9], r9[9];
    for (int r = 0; r < 3; ++r) { for (int c = 0; c < 3; ++c) { a9[3 * r + c] = authored12[4 * r + c]; } }
    if (t >= 1.0f) { for (int i = 0; i < 9; ++i) { r9[i] = grip9[i]; } }
    else if (t <= 0.0f) { for (int i = 0; i < 9; ++i) { r9[i] = a9[i]; } }
    else { SlerpM3(a9, grip9, t, r9); }
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) { out12[4 * r + c] = r9[3 * r + c]; }
        out12[4 * r + 3] = authored12[4 * r + 3];
    }
}
// INV-510 (Jay: the pickaxe grip "a little bit too low ... the shaft is slightly shorter"): how far the palm CENTRE may sit below
// the handle's end. 0.02 = the axe_d overhang Jay accepted in run 1 (palm -0.0986 vs butt -0.122 -> 0.024 inside; the
// sledgehammer 0.031) rounded down, so the two weapons he called good do not move.
constexpr float kMeleeButtMargin = 0.02f;
// The lift (metres up the shaft) that keeps the stacked left palm at least kMeleeButtMargin above this weapon's butt; both hands
// move up together by it (the weapon slides that much less). known false = no per-mesh butt (twohand_butt.inc): 0, never a guess.
inline float MeleeLift(bool known, float butt, float palmAlong)
{
    if (!known) { return 0.0f; }
    const float need = butt + kMeleeButtMargin - palmAlong;
    return need > 0.0f ? need : 0.0f;
}
// The grab strip's floor: this weapon's own butt when known, raised by the slider if the slider is higher (Jay: "as long as we
// can adjust it"); unknown = the slider alone.
inline float MeleeFloor(bool known, float butt, float slider)
{
    if (!known) { return slider; }
    return butt > slider ? butt : slider;
}
// INV-513: how deep the pinned palm point (twohand.cpp kPalmFromHandholder) sits above the palmar skin, along the hand's +z. 0: that
// point is ALREADY on the palmar surface (INV-502 (a): handholder (+0.0962, 0, -0.0238) + (-0.046, +0.004, -0.0027) = z -0.0265 = the
// palmar-surface p5). Measured on the skinned g5 / t5 hand, wn_axe_d: depth 0 -> palm skin 0.0014 m from the shaft surface (contact);
// the first guess 0.0238 left it 0.0195 m off.
constexpr float kMeleePalmDepth = 0.0f;
// The melee palm-centre target in the Holder frame: `along` up the shaft u, then (radius + palm depth) out from the axis along the
// hand's +z (zStd = the standard hand's z axis in the Holder frame) made perpendicular to u - so the palm, which faces -z, rests
// on the shaft surface. Run 3 (INV-513): a target on the -z side put the shaft on the BACK of the hand.
inline void MeleePalmTarget(const float u[3], float along, float radius, const float zStd[3], float out[3])
{
    const float d = zStd[0] * u[0] + zStd[1] * u[1] + zStd[2] * u[2];
    float zp[3] = { zStd[0] - d * u[0], zStd[1] - d * u[1], zStd[2] - d * u[2] };
    const float n = std::sqrt(zp[0] * zp[0] + zp[1] * zp[1] + zp[2] * zp[2]);
    const float k = n > 1e-6f ? (radius + kMeleePalmDepth) / n : 0.0f;
    for (int i = 0; i < 3; ++i) { out[i] = along * u[i] + k * zp[i]; }
}
// INV-514 (run 4, Jay "the left hand is still too far forward" - away from his body - "try 4cm"): the hand ROLLS rigidly about the
// shaft. A straight 4 cm shift drove all five fingers 14 mm into the shaft (toward the wrist) or lifted the palm off (any other
// way); a roll keeps fingers 1-4 out of the shaft at every angle tested (roll_check.py).
// The shaft radius at `deg` about the shaft from the standard palm direction, from a 12-entry table (every 30 deg, wraps).
inline float MeleeRadiusAt(const float r12[12], float deg)
{
    float d = std::fmod(deg, 360.0f);
    if (d < 0.0f) { d += 360.0f; }
    const float f = d / 30.0f;
    int i = static_cast<int>(f);
    if (i > 11) { i = 11; }
    const float k = f - static_cast<float>(i);
    return r12[i] + k * (r12[(i + 1) % 12] - r12[i]);
}
// v rotated by deg about the unit axis u (Rodrigues).
inline void RotateAboutAxis(const float v[3], const float u[3], float deg, float out[3])
{
    const float a = deg * 0.0174532925f, c = std::cos(a), s = std::sin(a);
    const float d = u[0] * v[0] + u[1] * v[1] + u[2] * v[2];
    const float cx[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
    for (int i = 0; i < 3; ++i) { out[i] = v[i] * c + cx[i] * s + u[i] * d * (1.0f - c); }
}
// The rotation of deg about the unit axis u, as a quaternion (left-multiply to roll a Holder-frame rotation about the shaft).
inline Q4 QAxisAngle(const float u[3], float deg)
{
    const float h = deg * 0.00872664626f, s = std::sin(h);
    return { u[0] * s, u[1] * s, u[2] * s, std::cos(h) };
}
// INV-515 (Jay: the handle should sit "across the base of the fingers ... wrapped round it"): the hand moves `cm` toward its WRIST
// (-x of the hand bone, xHand in the Holder frame), made perpendicular to the shaft u so the stack along the shaft is unchanged.
// Moving the hand that way carries the handle toward the finger bases (wrap_search2.py: 3.5 cm with curl 20 = 0 clipping).
inline void MeleeWristShift(const float xHand[3], const float u[3], float cm, float out[3])
{
    const float d = xHand[0] * u[0] + xHand[1] * u[1] + xHand[2] * u[2];
    const float p[3] = { xHand[0] - d * u[0], xHand[1] - d * u[1], xHand[2] - d * u[2] };
    const float n = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    const float k = n > 1e-6f ? -cm * 0.01f / n : 0.0f;
    for (int i = 0; i < 3; ++i) { out[i] = k * p[i]; }
}
inline float MaxAbsDiff(const float* a, const float* b, int n)
{
    float m = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float d = std::fabs(a[i] - b[i]);
        if (d > m) { m = d; }
    }
    return m;
}

}  // namespace twohand
}  // namespace dlvr
