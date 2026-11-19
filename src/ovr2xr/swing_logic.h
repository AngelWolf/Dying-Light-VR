// Pure logic for the VR swing-triggered melee attack - docs/SPEC-VR-SWING-ATTACK-2026-09-29.md §2.1 (detector)
// and §4 (blade direction). No OpenXR, no Windows, no logging, so src/src/tests/swing_logic_test.cpp compiles it
// on its own. swing.cpp owns everything with side effects (poses, the game calls, the log).
#pragma once

#include <atomic>
#include <cmath>

namespace dlvr {
namespace swing {

struct Vec3 { float x, y, z; };

inline bool  Finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
inline Vec3  Sub(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float Len(const Vec3& v) { return std::sqrt(Dot(v, v)); }
inline Vec3  Normalised(const Vec3& v)
{
    const float l = Len(v);
    return l > 1e-6f ? Vec3{ v.x / l, v.y / l, v.z / l } : v;
}

// The blade axis in the GRIP's own frame (spec §4): OpenXR grip -Z is "the ray that goes through the center of the
// tube formed by your non-thumb fingers, in the direction of little finger to thumb" (semantic_paths.adoc), tilted
// about grip +X by pitchDeg. Rx(p) * (0, 0, -1) = (0, sin p, -cos p).
inline Vec3 BladeLocal(float pitchDeg)
{
    const float p = pitchDeg * 0.017453292519943f;
    return { 0.0f, std::sin(p), -std::cos(p) };
}

// m is row-major [3*r + c] with the grip axes as COLUMNS - boneblend.cpp MeleeGripSnapshot::cWorld's layout.
inline Vec3 MulBasis(const float* m, const Vec3& v)
{
    return { m[0] * v.x + m[1] * v.y + m[2] * v.z,
             m[3] * v.x + m[4] * v.y + m[5] * v.z,
             m[6] * v.x + m[7] * v.y + m[8] * v.z };
}

inline Vec3 BladeDirWorld(const float* cWorld, float pitchDeg) { return Normalised(MulBasis(cWorld, BladeLocal(pitchDeg))); }

// v rotated by the unit quaternion (qx, qy, qz, qw): t = 2 (q x v); v' = v + w t + q x t.
inline Vec3 Rotate(float qx, float qy, float qz, float qw, const Vec3& v)
{
    const Vec3 t{ 2.0f * (qy * v.z - qz * v.y), 2.0f * (qz * v.x - qx * v.z), 2.0f * (qx * v.y - qy * v.x) };
    return { v.x + qw * t.x + (qy * t.z - qz * t.y),
             v.y + qw * t.y + (qz * t.x - qx * t.z),
             v.z + qw * t.z + (qx * t.y - qy * t.x) };
}

// A direction expressed in the head-YAW frame (+X right, +Y up, -Z ahead). Same yaw extraction as
// holster::LocalOffset / ovr2xr.cpp's vr_decouple, so "ahead" means the same thing in both features.
inline Vec3 InHeadYawFrame(float hx, float hy, float hz, float hw, const Vec3& d)
{
    const float yaw = std::atan2(2.0f * (hw * hy + hx * hz), 1.0f - 2.0f * (hy * hy + hx * hx));
    const float c = std::cos(yaw);
    const float s = std::sin(yaw);
    return { d.x * c - d.z * s, d.y, d.x * s + d.z * c };
}

// The head-yaw frame's +Z ("behind" - toward the player's body for a hand held in front) expressed in APP space: the
// inverse of InHeadYawFrame applied to (0,0,1). Same yaw extraction, so InHeadYawFrame(q, BackInApp(q)) == (0,0,1).
inline Vec3 BackInApp(float hx, float hy, float hz, float hw)
{
    const float yaw = std::atan2(2.0f * (hw * hy + hx * hz), 1.0f - 2.0f * (hy * hy + hx * hx));
    return { std::sin(yaw), 0.0f, std::cos(yaw) };
}

struct Params {
    float armSpeed;    // melee_swing_arm_speed, m/s - Idle -> Armed below this
    float fireSpeed;   // melee_swing_fire_speed, m/s - Armed -> Candidate at or above this
    float endSpeed;    // melee_swing_end_speed, m/s - Active -> END below this; Candidate -> dropped below this
    float windowMs;    // melee_swing_window_ms - the velocity window's MINIMUM span; no verdict until it spans this
    float maxMs;       // melee_swing_max_ms - hard END, measured from FIRE
    float minTravelM;  // melee_swing_min_travel_m - a CANDIDATE fires only once its path length reaches this (m)
    // Return-stroke rule (Stage 1 redo 4): a candidate that reaches minTravelM moving TOWARD the body, OPPOSITE the last
    // fired swing, within returnMs of that swing's END, is DROPPED (Drop::Return) instead of FIRED.
    float returnMs = 0.0f;    // melee_swing_return_ms - 0 = rule off
    float returnCos = 2.0f;   // cos(melee_swing_return_deg) - the cone around "toward the body"; > 1 = never
    // Option A (Jay 2026-09-30): melee_swing_refuse_toward_body. true = ANY candidate inside the cone is DROPPED
    // (Drop::TowardBody) - no window, no previous swing needed; returnMs is then unused. false = the window rule above.
    bool  refuseToward = false;
    // The return speed gate (Jay 2026-10-26, "Do the speed-gated return, 4.5 m/s"): inside the window rule, a candidate whose
    // peak so far is at least this fast is a BACKSWING and fires; slower is the arm's recoil and stays Drop::Return. Only the
    // window rule - refuseToward is not opened by it. 0 = no gate (melee_swing_return_fast_mps).
    float returnFastMps = 0.0f;
    // The reversal END (Jay 2026-10-26, "Fix the short-travel misses too"): cos(melee_swing_reversal_deg) - an Active swing
    // ENDs Reversal when its velocity has turned more than that angle from the velocity ONE SPEED WINDOW (windowMs) earlier.
    // 0.0 = 90 deg. A curved arc turns a little per window and stays one swing; a backswing flips within one.
    float reversalCos = 0.0f;
    // The reversal look-back (Jay 2026-10-26, "Yes, build it"): the reference velocity is the one at least this many ms old
    // (melee_swing_reversal_ms); 0 = windowMs. 100 = a forehand -> backhand U-turn is a new swing, a curved arc is not.
    float reversalMs = 0.0f;
};

struct Sample {
    double tMs;        // monotonic time of this sample, ms
    Vec3   p;          // right grip position, APP space (stick locomotion and turning do not move it)
    bool   poseOk;     // head AND right grip both valid
    bool   inHolster;  // the hand is inside the right-shoulder holster zone
    Vec3   back{ 0.0f, 0.0f, 0.0f };   // unit "toward the body" in APP space (BackInApp); zero = unknown - never refuses
    // Fix 5 (HANDOFF-MELEE-FIXES-2026-11-19, Jay "Go ahead"): the right grip, for the shoulder-zone exit rule. Default
    // false = unknown, which keeps the old rule (Idle on exit).
    bool   gripKnown = false;   // GetControllerSqueeze(right) returned a finite value this sample
    bool   gripDown = false;    // gripKnown only: squeeze >= holster::kGripThreshold (the holster's own press threshold)
};

// Fix 5: what the first pose-valid sample OUTSIDE the shoulder zone did after a zone visit (Step::zoneExit).
// Armed = no grip press inside the zone - a stroke out of it can fire; IdlePressed = a grip press inside (a draw / stow) -
// the old rule, must slow below armSpeed first; IdleUnknown = the grip state was unknown on an in-zone sample - the old rule.
enum class ZoneExit { None, Armed, IdlePressed, IdleUnknown };

inline const char* ZoneExitName(ZoneExit z)
{
    switch (z) {
    case ZoneExit::Armed:       return "ARMED (no grip press inside the zone - a stroke out of it can fire)";
    case ZoneExit::IdlePressed: return "IDLE (grip pressed inside the zone - must slow below arm speed first)";
    case ZoneExit::IdleUnknown: return "IDLE (grip state unknown inside the zone - must slow below arm speed first)";
    default:                    return "none";
    }
}

// Candidate (B1 rebound suppression, Stage 1 run 1: 11 FIRE for 5 slashes): Armed + speed >= fireSpeed no longer
// FIREs. It starts a candidate whose PATH LENGTH is accumulated; it FIREs (-> Active) once the path reaches minTravelM,
// and is DROPPED - no FIRE, no END - if the hand slows below endSpeed first (the arm's recoil after a slash).
enum class State { Idle, Armed, Candidate, Active };
enum class End { None, Slow, Reversal, MaxDuration, PoseLost, Blocked, Holster };

inline const char* EndName(End e)
{
    switch (e) {
    case End::Slow:        return "slow";
    case End::Reversal:    return "reversal";
    case End::MaxDuration: return "max-duration";
    case End::PoseLost:    return "pose-lost";
    case End::Blocked:     return "blocked";
    case End::Holster:     return "holster";
    default:               return "none";
    }
}

// Why a CANDIDATE was dropped (fix round 1, Minor 8 - the [SWING] candidate dropped line). PoseLost covers a
// non-finite sample too (the same refusal as the Active case). Relatch comes only from Abandon() (the driver).
enum class Drop { None, Slow, PoseLost, Holster, Blocked, Stall, Relatch, Return, TowardBody };

inline const char* DropName(Drop d)
{
    switch (d) {
    case Drop::Slow:     return "slow";
    case Drop::PoseLost: return "pose-lost";
    case Drop::Holster:  return "holster";
    case Drop::Blocked:  return "blocked";
    case Drop::Stall:    return "stall";
    case Drop::Relatch:  return "relatch";
    case Drop::Return:   return "return";
    case Drop::TowardBody: return "toward-body";
    default:             return "none";
    }
}

struct Step {
    bool  fired = false;
    bool  ended = false;
    End   reason = End::None;
    State state = State::Idle;
    bool  speedValid = false;
    float speed = 0.0f;
    Vec3  vel{ 0.0f, 0.0f, 0.0f };
    float spanMs = 0.0f;   // speedValid only: the velocity window's ACTUAL span, ms (>= Params::windowMs)
    int   spanSamples = 0; // speedValid only: samples that span covers, both ends included
    float peak = 0.0f;    // of the active swing, or of the one that just ended, or of the candidate just dropped
    float durMs = 0.0f;    // of the active swing / the one that just ended (from FIRE); of a DROPPED candidate: from
                           // its path start (candStartMs) to the drop, -1 when the drop sample's time is not finite
    bool  repeat = false;          // bit-identical to the previous sample - dropped
    bool  nonFinite = false;       // refused as a pose loss
    bool  holsterRefused = false;  // refused: hand in the holster zone
    ZoneExit zoneExit = ZoneExit::None;   // fix 5: this sample ended a shoulder-zone visit - which state it left in
    bool  candStarted = false;     // this sample started a CANDIDATE (Armed + speed >= fireSpeed)
    bool  dropped = false;         // a CANDIDATE was dropped this sample - no FIRE, no END
    Drop  dropWhy = Drop::None;    // dropped only: why
    float travelM = 0.0f;          // path length from the candidate start: at FIRE, at END, or so far
    double candStartMs = 0.0;      // the candidate's path start time (see Update) - valid in Candidate/Active/END
    float maxTurnDeg = -1.0f;      // at END: the largest angle between the velocity and its reversal look-back reference over
                                   // the swing, deg, in the detector's own (app) space; -1 = never measured
};

// HOLD LATCH (Jay 2026-10-27: a thrown body dismembered on release - "gate the grab too"). A swing - candidate or active - that
// was under way at any moment while `holding` (a body grab on the right hand, or a zombie holding the player) keeps its hit off
// to its END: the hold gate used to be per frame, so a throw's swing that ran past the release called the hit (27 of 27 such
// releases in the archived logs, 2-40 calls each). Cleared at the END, or when the swing is gone without one (a dropped
// candidate). Returns true = the hit may be called this frame.
struct HoldLatch {
    bool on = false;
};

// THE LIGHT STAGGER (INV-639; Jay 2026-10-27: "It should count" -> "Just light"). The game's damage reaction switches the player
// to controller group 30, which stops the melee controller (id 14 is not a member) - its slot 16 (our swing) stops and only slot
// 17, the inactive update, runs until the reaction hands back. Only that group feeds the swing from slot 17.
constexpr int kLightStaggerGroup = 30;
inline bool FeedFromInactive(int group) { return group == kLightStaggerGroup; }

inline bool HoldLatchStep(HoldLatch& l, bool holding, const Step& st)
{
    const bool inSwing = st.fired || st.state == State::Candidate || st.state == State::Active;
    if (holding && inSwing) { l.on = true; }
    if (!inSwing && !st.ended) { l.on = false; }
    const bool allow = !holding && !l.on;
    if (st.ended) { l.on = false; }
    return allow;
}

class Detector {
public:
    static constexpr int kRing = 64;

    Step Update(const Sample& s, const Params& p)
    {
        Step st{};
        const bool finite = Finite(s.p) && std::isfinite(s.tMs);
        if (!finite) { st.nonFinite = true; }

        // Fix 5: a grip PRESS = down now while the previous sample's grip was not known-down (a held grip entering the zone is
        // not a press - the holster treats it the same way). Bookkept on every sample, pose or not.
        const bool gripPress = s.gripKnown && s.gripDown && !(gripPrevKnown_ && gripPrevDown_);
        gripPrevKnown_ = s.gripKnown;
        gripPrevDown_ = s.gripKnown && s.gripDown;

        // A missing or garbage pose ends a swing and wipes the history: a velocity across a gap is not a velocity.
        // A pending candidate is DROPPED (no FIRE, no END - nothing fired), exactly like the Idle/Armed case.
        if (!s.poseOk || !finite) {
            if (state_ == State::Active) { st = EndSwing(st, End::PoseLost, s.tMs, State::Idle); }
            else if (state_ == State::Candidate) { st = DropCandidate(st, State::Idle, Drop::PoseLost, s.tMs); }
            else { state_ = State::Idle; }
            Clear();
            zoneVisit_ = false;   // fix 5: tracking resumed after a gap is Idle (spec §2.1), wherever the hand was
            st.state = state_;
            return st;
        }
        // The holster reach is never a swing (spec §2.1, REF §3.4c).
        if (s.inHolster) {
            // Fix 5: remember whether this visit saw a grip press or an unknown grip - either keeps the old Idle on exit.
            if (!zoneVisit_) {
                zoneVisit_ = true;
                zoneUsed_ = false;
                zoneUnknown_ = false;
            }
            if (!s.gripKnown) { zoneUnknown_ = true; }
            if (gripPress) { zoneUsed_ = true; }
            st.holsterRefused = true;
            if (state_ == State::Active) { st = EndSwing(st, End::Holster, s.tMs, State::Idle); }
            else if (state_ == State::Candidate) { st = DropCandidate(st, State::Idle, Drop::Holster, s.tMs); }
            else { state_ = State::Idle; }
            Clear();
            st.state = state_;
            return st;
        }
        // Fix 5 (HANDOFF-MELEE-FIXES-2026-11-19 #5, Jay "Go ahead"): the first pose-valid sample out of the zone. A visit with no
        // grip press and a known grip throughout leaves ARMED, so a stroke that leaves the zone already faster than armSpeed (a
        // wind-up over the shoulder into a chop) can fire; before, it stayed Idle until the hand slowed below armSpeed. The ring
        // is empty (Clear() above), so this sample still has no speed verdict - Armed -> Candidate waits for a full window.
        if (zoneVisit_) {
            zoneVisit_ = false;
            if (state_ == State::Idle) {
                if (zoneUsed_) { st.zoneExit = ZoneExit::IdlePressed; }
                else if (zoneUnknown_) { st.zoneExit = ZoneExit::IdleUnknown; }
                else { st.zoneExit = ZoneExit::Armed; state_ = State::Armed; }
            }
        }
        // A bit-identical pose is a repeat, not a sample (REF §3.3: per-frame speed alternates spike/zero).
        if (havePrev_ && s.p.x == prevP_.x && s.p.y == prevP_.y && s.p.z == prevP_.z) {
            st.repeat = true;
            // Final-review I2: same order as the non-repeat path - a Block() ends the swing on the very next Update,
            // repeat or not, so the driver never calls the hit again after a wall strike.
            if (state_ == State::Active) {
                if (blocked_) {
                    st = EndSwing(st, End::Blocked, s.tMs, State::Idle);
                } else if (s.tMs - tFire_ >= p.maxMs) {
                    st = EndSwing(st, End::MaxDuration, s.tMs, State::Idle);
                }
            }
            // A blocked candidate is dropped on the very next Update, repeat or not - same order as the Active case.
            else if (state_ == State::Candidate && blocked_) {
                st = DropCandidate(st, State::Idle, Drop::Blocked, s.tMs);
                st.state = state_;
                return st;
            }
            // A repeat adds no path. A pending candidate simply waits for the next real sample.
            if (state_ == State::Candidate || state_ == State::Active) { FillTravel(st); }
            st.state = state_;
            return st;
        }
        // Path length: the segment from the previous NON-REPEAT sample to this one. The candidate's path starts at that
        // previous sample (prevT), so the segment that carried the hand across fireSpeed counts, and the driver's sweep
        // backfill (which starts at the frame at/just before candStartMs) covers exactly the path that was measured.
        const bool hadPrev = havePrev_;
        const float seg = hadPrev ? Len(Sub(s.p, prevP_)) : 0.0f;
        const double prevT = hadPrev ? prevT_ : s.tMs;
        havePrev_ = true;
        prevP_ = s.p;
        prevT_ = s.tMs;
        if (state_ == State::Candidate || state_ == State::Active) { path_ += seg; }

        Vec3 v{ 0.0f, 0.0f, 0.0f };
        double spanMs = 0.0;
        int spanPts = 0;
        st.speedValid = Velocity(s.tMs, s.p, p.windowMs, v, spanMs, spanPts);
        Push(s.tMs, s.p);
        if (st.speedValid) {
            st.vel = v;
            st.speed = Len(v);
            st.spanMs = static_cast<float>(spanMs);
            st.spanSamples = spanPts;
        }

        if (state_ == State::Active) {
            if (st.speedValid && st.speed > peak_) { peak_ = st.speed; }
            st.durMs = static_cast<float>(s.tMs - tFire_);
            st.peak = peak_;
            FillTravel(st);
            if (blocked_) { return EndSwing(st, End::Blocked, s.tMs, State::Idle); }
            if (s.tMs - tFire_ >= p.maxMs) { return EndSwing(st, End::MaxDuration, s.tMs, State::Idle); }
            if (st.speedValid) {
                if (st.speed < p.endSpeed) {
                    return EndSwing(st, End::Slow, s.tMs, st.speed < p.armSpeed ? State::Armed : State::Idle);
                }
                // Reversal = turned more than reversalCos's angle from the velocity one speed window ago (not from the FIRE
                // velocity: a curved arc builds that turn slowly and is one swing - Jay 2026-10-26).
                const Vec3 ref = ReversalRef(s.tMs, p.reversalMs > 0.0f ? p.reversalMs : p.windowMs);
                const float lv = Len(v), lr = Len(ref);
                if (lv > 1e-6f && lr > 1e-6f) {
                    const float c = Dot(v, ref) / (lv * lr);
                    if (c < minTurnCos_) { minTurnCos_ = c; }   // the swing's largest turn so far (Step::maxTurnDeg at END)
                    if (c < p.reversalCos) { return EndSwing(st, End::Reversal, s.tMs, State::Armed); }
                }
                PushVel(s.tMs, v);
            }
            st.state = state_;
            return st;
        }
        if (state_ == State::Candidate) {
            if (st.speedValid && st.speed > peak_) { peak_ = st.speed; }
            st.peak = peak_;
            FillTravel(st);
            // Blocked before FIRE: dropped, next state Idle - the same as a Blocked END (brief: blocked -> drop).
            if (blocked_) { return DropCandidate(st, State::Idle, Drop::Blocked, s.tMs); }
            // No verdict across a stall is not "still moving": a candidate is never carried over one (fail closed).
            if (!st.speedValid) { return DropCandidate(st, State::Idle, Drop::Stall, s.tMs); }
            // Slowed before reaching minTravelM - the arm's recoil, or a flick. Same next state as a Slow END.
            if (st.speed < p.endSpeed) {
                return DropCandidate(st, st.speed < p.armSpeed ? State::Armed : State::Idle, Drop::Slow, s.tMs);
            }
            if (path_ >= p.minTravelM) {
                // Next state Idle: the rest of the stroke must slow below armSpeed before anything can fire again.
                const Drop why = TowardVerdict(v, s, p);
                if (why != Drop::None) { return DropCandidate(st, State::Idle, why, s.tMs); }
                Fire(st, s.tMs, v);
            }
            st.state = state_;
            return st;
        }
        if (!st.speedValid) {
            st.state = state_;
            return st;
        }
        if (state_ == State::Idle) {
            if (st.speed < p.armSpeed) { state_ = State::Armed; }
        } else if (state_ == State::Armed && st.speed >= p.fireSpeed) {
            state_ = State::Candidate;
            st.candStarted = true;
            candStart_ = prevT;
            path_ = seg;
            peak_ = st.speed;   // the candidate is the same stroke - its peak counts toward the swing's
            blocked_ = false;
            st.peak = peak_;
            FillTravel(st);
            if (path_ >= p.minTravelM) {   // one segment already long enough
                const Drop why = TowardVerdict(v, s, p);
                if (why != Drop::None) { return DropCandidate(st, State::Idle, why, s.tMs); }
                Fire(st, s.tMs, v);
            }
        }
        st.state = state_;
        return st;
    }

    // The driver reports the game's "swing blocked by a surface" latch (this+772, spec F6). Ends the ACTIVE swing on
    // the next Update, or DROPS a pending Candidate on the next Update (no FIRE, no END). Ignored in Idle/Armed. The
    // driver today only calls the hit - and so only reads this+772 - while Active; the Candidate case is fail-closed.
    void Block()
    {
        if (state_ == State::Active || state_ == State::Candidate) { blocked_ = true; }
    }

    // Fix round 1 (Minor 8): the driver is about to throw this detector away (a primary relatch). A pending Candidate is
    // DROPPED (reason Relatch) and its travel / peak / duration returned, so the drop can be logged like any other.
    // Anything else: a Step with dropped == false, state unchanged (an Active swing is the driver's relatch END).
    Step Abandon(double tNow)
    {
        Step st{};
        if (state_ != State::Candidate) {
            st.state = state_;
            return st;
        }
        st = DropCandidate(st, State::Idle, Drop::Relatch, tNow);
        Clear();
        return st;
    }

    State state() const { return state_; }

private:
    struct Pt { double t; Vec3 p; };

    // The reference sample is the NEWEST one at least windowMs old. None yet -> no verdict. Older than 3x the window
    // -> a stall sits inside the window, and a velocity across a stall is not one -> no verdict.
    // spanMs / spanPts (valid only on true): the actual span used and the samples it covers - the reference, the
    // current sample and every one between (k + 2 for the ring's k-th newest).
    bool Velocity(double tNow, const Vec3& pNow, float windowMs, Vec3& v, double& spanMs, int& spanPts) const
    {
        for (int k = 0; k < n_; ++k) {
            const Pt& r = ring_[(head_ - k + kRing) % kRing];
            const double age = tNow - r.t;
            if (age >= windowMs) {
                if (age > 3.0 * windowMs || age <= 0.0) { return false; }
                const float inv = static_cast<float>(1000.0 / age);
                const Vec3 d = Sub(pNow, r.p);
                v = { d.x * inv, d.y * inv, d.z * inv };
                spanMs = age;
                spanPts = k + 2;
                return true;
            }
        }
        return false;
    }

    void Push(double t, const Vec3& p)
    {
        head_ = (head_ + 1) % kRing;
        ring_[head_] = { t, p };
        if (n_ < kRing) { ++n_; }
    }

    void Clear()
    {
        n_ = 0;
        havePrev_ = false;
    }

    Step EndSwing(Step st, End why, double tNow, State next)
    {
        st.ended = true;
        st.reason = why;
        st.durMs = static_cast<float>(tNow - tFire_);
        if (minTurnCos_ <= 1.0f) {
            const float c = minTurnCos_ < -1.0f ? -1.0f : minTurnCos_;
            st.maxTurnDeg = std::acos(c) * 57.2957795f;
        } else {
            st.maxTurnDeg = -1.0f;
        }
        st.peak = peak_;
        FillTravel(st);   // the swing's TOTAL path, from the candidate start
        state_ = next;
        blocked_ = false;
        st.state = state_;
        // This swing becomes "the last swing" the return-stroke rule compares against. A DROPPED candidate never does.
        haveLast_ = true;
        tLastEnd_ = tNow;
        vLast_ = vFire_;
        return st;
    }

    // Judged at the FIRE point (path >= minTravelM) with the velocity that would FIRE. Only a stroke inside the returnCos
    // cone around "toward the body" can be refused; a zero / unknown back vector never is (fail OPEN).
    //   refuseToward (Option A): inside the cone -> TowardBody, always.
    //   otherwise, the window rule: inside the cone AND within returnMs of the last swing's END AND opposite that swing's
    //   FIRE velocity -> Return. returnMs 0 or no last swing -> None.
    Drop TowardVerdict(const Vec3& v, const Sample& s, const Params& p) const
    {
        const float lb = Len(s.back);
        const float lv = Len(v);
        if (!(lb > 0.5f) || !(lv > 1e-6f)) { return Drop::None; }
        if (!(Dot(v, s.back) / (lv * lb) >= p.returnCos)) { return Drop::None; }
        if (p.refuseToward) { return Drop::TowardBody; }
        if (!(p.returnMs > 0.0f) || !haveLast_) { return Drop::None; }
        if (!(s.tMs - tLastEnd_ <= static_cast<double>(p.returnMs))) { return Drop::None; }
        if (p.returnFastMps > 0.0f && peak_ >= p.returnFastMps) { return Drop::None; }   // a fast backswing, not recoil
        return Dot(v, vLast_) < 0.0f ? Drop::Return : Drop::None;
    }

    // Candidate -> Active. vFire is the velocity AT the FIRE sample; maxMs is measured from here.
    void Fire(Step& st, double tNow, const Vec3& v)
    {
        state_ = State::Active;
        st.fired = true;
        tFire_ = tNow;
        vFire_ = v;
        vHead_ = 0;            // the reversal reference history starts at the FIRE velocity
        vN_ = 1;
        minTurnCos_ = 2.0f;    // no turn measured yet this swing
        vHist_[0] = { tNow, v };
        blocked_ = false;
        st.peak = peak_;
        st.durMs = 0.0f;
        FillTravel(st);
    }

    // No FIRE happened, so no END either - the driver counts it (Step::dropped) and logs why / how far / how long.
    Step DropCandidate(Step st, State next, Drop why, double tNow)
    {
        st.dropped = true;
        st.dropWhy = why;
        st.peak = peak_;
        const double dur = tNow - candStart_;
        st.durMs = std::isfinite(dur) ? static_cast<float>(dur) : -1.0f;
        FillTravel(st);
        state_ = next;
        blocked_ = false;
        st.state = state_;
        return st;
    }

    void FillTravel(Step& st) const
    {
        st.travelM = path_;
        st.candStartMs = candStart_;
    }

    Pt    ring_[kRing] = {};
    int   head_ = 0;
    int   n_ = 0;
    bool  havePrev_ = false;
    Vec3  prevP_{ 0.0f, 0.0f, 0.0f };
    double prevT_ = 0.0;         // time of prevP_ (the previous NON-REPEAT sample)
    double candStart_ = 0.0;     // the candidate's path start = the sample before the one that crossed fireSpeed
    float path_ = 0.0f;          // path length since candStart_, non-repeat samples only (Candidate + Active)
    State state_ = State::Idle;
    bool  blocked_ = false;
    // Fix 5: the shoulder-zone visit and the grip's previous state (Update only).
    bool  zoneVisit_ = false;      // inside a zone visit (since its first refused sample)
    bool  zoneUsed_ = false;       // that visit saw a grip press
    bool  zoneUnknown_ = false;    // that visit saw an in-zone sample with the grip state unknown
    bool  gripPrevKnown_ = false;  // the previous sample's grip was known
    bool  gripPrevDown_ = false;   // ... and down
    double tFire_ = 0.0;
    Vec3  vFire_{ 0.0f, 0.0f, 0.0f };
    float peak_ = 0.0f;
    bool   haveLast_ = false;          // a swing has ENDED since this detector was made (return-stroke rule)
    double tLastEnd_ = 0.0;            // its END time
    Vec3   vLast_{ 0.0f, 0.0f, 0.0f }; // its FIRE velocity
    // The reversal reference (Active only): the swing's recent velocities, newest at vHead_. The reference is the newest one
    // at least windowMs old; before one is that old, the oldest held (the FIRE velocity).
    struct Vt { double t; Vec3 v; };
    static constexpr int kVHist = 32;
    Vt  vHist_[kVHist] = {};
    int vHead_ = 0;
    int vN_ = 0;
    float minTurnCos_ = 2.0f;   // the swing's smallest cos(velocity, reference) - its largest turn; > 1 = none measured

    Vec3 ReversalRef(double tNow, float windowMs) const
    {
        for (int k = 0; k < vN_; ++k) {
            const Vt& e = vHist_[(vHead_ - k + kVHist) % kVHist];
            if (tNow - e.t >= windowMs) { return e.v; }
        }
        return vHist_[(vHead_ - (vN_ - 1) + kVHist) % kVHist].v;
    }

    void PushVel(double t, const Vec3& v)
    {
        vHead_ = (vHead_ + 1) % kVHist;
        vHist_[vHead_] = { t, v };
        if (vN_ < kVHist) { ++vN_; }
    }
};

// The swing driver's gate (Stage 1 run 1 `threads 7`, 2026-09-30). Slot 16 rotates over the game's 7 worker threads
// and INV-282: thread identity cannot decide concurrency - so the driver's plain globals are guarded here. NON-BLOCKING
// on purpose: a second entry while held is REFUSED and COUNTED, never waited for - spinning would stall a game worker,
// and the gated code calls the game's hit (which may itself wait on jobs). TryEnter's acquire and Leave's release also
// order frame N's writes (thread A) before frame N+1's reads (thread B), whatever the job system guarantees.
class Gate {
public:
    bool TryEnter()
    {
        int expected = 0;
        if (held_.compare_exchange_strong(expected, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
            return true;
        }
        overlaps_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    void Leave() { held_.store(0, std::memory_order_release); }
    long Overlaps() const { return overlaps_.load(std::memory_order_relaxed); }   // session total, monotonic

private:
    std::atomic<int>  held_{ 0 };
    std::atomic<long> overlaps_{ 0 };
};

}  // namespace swing
}  // namespace dlvr
