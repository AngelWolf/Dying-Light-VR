// Physical breakout - the pure part (spec docs/superpowers/specs/2026-10-24-physical-breakout-design.md, plan Task 2).
// ShoveDetector: two grip positions + the head (OpenXR tracking space, y up) -> one event per two-handed shove.
// Decide: the grab's state (INV-618: type, phase, elapsed, the late-begin threshold, the win-blocking byte) -> what to call.
// No game reads here; breakout.cpp feeds both. Tests: src/tests/breakout_logic_test.cpp.
#pragma once

#include <cmath>

namespace dlvr {
namespace breakout {

struct ShoveParams {
    float speedMps = 1.6f;       // breakout_shove_speed_mps: each hand must reach this
    float pairS = 0.150f;        // both hands reach it within this of each other
    float maxAngleDeg = 60.0f;   // the two hands move the same way within this
    float maxTowardHead = 0.30f; // the mean motion's horizontal part toward the head, as a share of its speed (a pull is not a shove)
    float rearm = 0.5f;          // after a shove, both hands below rearm x speedMps before the next counts
};

struct ShoveInfo {
    float speedL, speedR, angleDeg, towardHead;
};

// Velocity over a ~100 ms window of samples - the rule of looting::SpeedWindow (D-269: never a per-poll delta), keeping the vector.
class VelWindow {
public:
    static constexpr int kCap = 128;   // ~224 ms at 570 polls/s
    void Reset() { n_ = 0; }
    void Push(double t, const float p[3])
    {
        head_ = (head_ + 1) % kCap;
        t_[head_] = t;
        for (int k = 0; k < 3; ++k) { p_[head_][k] = p[k]; }
        if (n_ < kCap) { ++n_; }
    }
    // false until the window holds >= minS of samples
    bool Vel(float out[3], double windowS = 0.10, double minS = 0.05) const
    {
        if (n_ < 2) { return false; }
        const double tn = t_[head_];
        int ref = -1;
        for (int i = 1; i < n_; ++i) {
            const int j = (head_ - i + kCap) % kCap;
            if (tn - t_[j] >= windowS) { ref = j; break; }
        }
        if (ref < 0) {
            const int oldest = (head_ - (n_ - 1) + kCap) % kCap;
            if (tn - t_[oldest] < minS) { return false; }
            ref = oldest;
        }
        const float dt = static_cast<float>(tn - t_[ref]);
        for (int k = 0; k < 3; ++k) { out[k] = (p_[head_][k] - p_[ref][k]) / dt; }
        return true;
    }

private:
    double t_[kCap] = {};
    float p_[kCap][3] = {};
    int head_ = 0;
    int n_ = 0;
};

class ShoveDetector {
public:
    void Reset()
    {
        for (int h = 0; h < 2; ++h) { w_[h].Reset(); fastT_[h] = -1.0; }
        armed_ = true;
    }

    // One poll. true exactly once per accepted shove; `out` gets its numbers.
    bool Push(double t, const float left[3], bool haveL, const float right[3], bool haveR, const float head[3],
              const ShoveParams& p, ShoveInfo* out)
    {
        const float* pos[2] = { left, right };
        const bool have[2] = { haveL, haveR };
        float v[2][3] = {};
        float s[2] = { 0.0f, 0.0f };
        for (int h = 0; h < 2; ++h) {
            if (!have[h]) {
                w_[h].Reset();
                fastT_[h] = -1.0;
                continue;
            }
            w_[h].Push(t, pos[h]);
            if (w_[h].Vel(v[h])) { s[h] = Len(v[h]); }
            if (s[h] >= p.speedMps) {
                if (fastT_[h] < 0.0) { fastT_[h] = t; }
            } else {
                fastT_[h] = -1.0;
            }
        }
        if (!armed_) {
            if (s[0] < p.rearm * p.speedMps && s[1] < p.rearm * p.speedMps) { armed_ = true; }
            return false;
        }
        if (fastT_[0] < 0.0 || fastT_[1] < 0.0 || std::fabs(fastT_[0] - fastT_[1]) > p.pairS) { return false; }
        const float cosA = Dot(v[0], v[1]) / (s[0] * s[1]);
        const float angle = std::acos(cosA > 1.0f ? 1.0f : (cosA < -1.0f ? -1.0f : cosA)) * 57.29578f;
        float mean[3], toHead[3];
        for (int k = 0; k < 3; ++k) {
            mean[k] = 0.5f * (v[0][k] + v[1][k]);
            toHead[k] = head[k] - 0.5f * (left[k] + right[k]);
        }
        toHead[1] = 0.0f;
        const float meanLen = Len(mean), headLen = Len(toHead);
        const float toward = (meanLen > 0.0f && headLen > 0.0f) ? Dot(mean, toHead) / (meanLen * headLen) : 0.0f;
        if (angle > p.maxAngleDeg || toward > p.maxTowardHead) { return false; }
        armed_ = false;
        if (out) { *out = ShoveInfo{ s[0], s[1], angle, toward }; }
        return true;
    }

private:
    static float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    static float Len(const float a[3]) { return std::sqrt(Dot(a, a)); }

    VelWindow w_[2];
    double fastT_[2] = { -1.0, -1.0 };   // when each hand last rose above speedMps (-1 = not fast)
    bool armed_ = true;
};

enum class Kind { Ignore, Phase8, Tap, Hold };

struct GrabState {
    bool grabbed;       // GrabController +16 set and its enemy pair (+144 / +152) non-null
    int type;           // player vfunc +872: 0-3 zombie grabs, 4 leg grab, 5 Volatile (by name)
    int phase;          // GrabController +204
    float elapsedS;     // now - phase start (+208)
    float lateBeginS;   // the KICK path's late-begin threshold for this type / anim set
    bool winBlocked;    // GrabController +177 (anim event 541 .. 540) - logged only; Decide does not read it (breakout_run1)
};

// Zombie: phase 8 exactly when the game's own KICK would set it; a shove that is early waits (Hold). +177 is NOT tested: it stays
// set through the whole fight phase and gates only the tap bar - the KICK path never reads it (breakout_run1: 17 shoves held by it).
// Volatile: every shove is one tap whatever the phase (the game's OnInput taps in any phase). Every other type: the game's own.
inline Kind Decide(const GrabState& g)
{
    if (!g.grabbed) { return Kind::Ignore; }
    if (g.type == 5) { return Kind::Tap; }
    if (g.type < 0 || g.type > 3) { return Kind::Ignore; }
    if (g.phase == 5 || g.phase == 10) { return Kind::Phase8; }
    if (g.phase >= 0 && g.phase <= 4) { return g.elapsedS >= g.lateBeginS ? Kind::Phase8 : Kind::Hold; }
    return Kind::Ignore;
}

// One grab = one enemy. A grab that ends and the next that starts with no free frame between them is two grabs (final review,
// Important 1): the tick re-reads the type on `started` and drops a held shove on `ended`.
struct GrabEdge {
    bool started, ended;
};
inline GrabEdge Edges(bool wasGrabbed, unsigned long long lastEnemy, bool grabbed, unsigned long long enemy)
{
    const bool same = wasGrabbed && grabbed && enemy == lastEnemy;
    return GrabEdge{ grabbed && !same, wasGrabbed && !same };
}

// A held shove older than this is dropped (spec §6b.2).
inline bool Expired(double shoveT, double nowT) { return nowT - shoveT > 1.0; }

// The melee gate (swing.cpp; Jay 2026-10-27: an attack as a zombie grabs should count - "only become an issue since we added the
// manual shove" -> "change the timing on the gate"). The hit is blocked during a grab only from kMeleeOpenMs after GRAB START:
// in the archived logs the first detected shove came a median 759 ms in (5 of 200 before 400 ms), the 46 attacks in flight at a
// grab start ended 2-345 ms after it, and the game's own kick lock is 0.37 s. startMs 0 = no stamp -> open.
constexpr unsigned long long kMeleeOpenMs = 400;
inline bool MeleeGateClosed(bool grabbed, unsigned long long startMs, unsigned long long nowMs)
{
    return grabbed && startMs != 0 && nowMs >= startMs && nowMs - startMs >= kMeleeOpenMs;
}

}  // namespace breakout
}  // namespace dlvr
