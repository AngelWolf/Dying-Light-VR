// pushgrab_logic.h - the open-hand push and grab's decisions (spec docs/superpowers/specs/2026-10-31-open-hand-push-and-grab-design.md,
// plan docs/superpowers/plans/2026-10-31-open-hand-push-and-grab.md Task 2). Pure: no game, no engine, no clock of its own - the
// caller passes time in seconds. Unit-tested by src/tests/pushgrab_logic_test.cpp. World = the game world, Y up.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace dlvr {
namespace pushgrab {

enum class PushTier { None = 0, Shove = 1, KnockDown = 2 };

struct PushParams {
    float minMps;        // push_min_mps - below this horizontal hand speed nothing happens
    float knockdownMps;  // push_knockdown_mps - at or above it the shove knocks down
    float rearmS;        // push_rearm_s - a second push in the same contact after this long
};

struct PushResult {
    PushTier tier;
    float speed;   // horizontal hand speed, m/s
    float dir[3];  // the hand's horizontal velocity, unit, y = 0 (zero when the hand is still)
};

// The hand moving AWAY from the body (direction against the inward normal by more than this cosine) never pushes; a sideways sweep
// (cosine ~0) still does. [Claude's call, plan Task 2]
constexpr float kAwayCos = -0.2f;
// Out of contact this long counts as leaving the body (re-arms the push) - the same 0.15 s as handcoll's ContactEdge.
constexpr double kExitS = 0.15;

// handVel = the real hand's velocity; outwardNormal = the touched surface's outward normal (from the body toward the hand).
inline PushResult ClassifyPush(const float handVel[3], const float outwardNormal[3], const PushParams& p)
{
    PushResult r{ PushTier::None, 0.0f, { 0.0f, 0.0f, 0.0f } };
    const float vx = handVel[0], vz = handVel[2];
    r.speed = std::sqrt(vx * vx + vz * vz);
    if (!(r.speed > 1e-6f)) { r.speed = 0.0f; return r; }
    r.dir[0] = vx / r.speed;
    r.dir[2] = vz / r.speed;
    const float ix = -outwardNormal[0], iz = -outwardNormal[2];
    const float il = std::sqrt(ix * ix + iz * iz);
    if (il > 1e-6f && (r.dir[0] * ix + r.dir[2] * iz) / il < kAwayCos) { return r; }
    if (r.speed < p.minMps) { return r; }
    r.tier = r.speed >= p.knockdownMps ? PushTier::KnockDown : PushTier::Shove;
    return r;
}

// Final review I2 (spec §4: "from the controller velocity"): the TIER comes from the controller's own horizontal speed in its
// tracking space (`trackedSpeed`, m/s) - a snap / smooth turn or walking moves the hand in the game world without moving the
// controller, and must not push; the DIRECTION (and the moving-away test) still come from the hand's game-world velocity.
inline PushResult ClassifyPushTracked(const float worldVel[3], float trackedSpeed, const float outwardNormal[3], const PushParams& p)
{
    PushResult r = ClassifyPush(worldVel, outwardNormal, PushParams{ 0.0f, 3.4e38f, p.rearmS });
    const bool direction = r.tier != PushTier::None;   // a direction exists and is not moving away
    r.speed = trackedSpeed;
    r.tier = PushTier::None;
    if (!direction || !(trackedSpeed >= p.minMps)) { return r; }
    r.tier = trackedSpeed >= p.knockdownMps ? PushTier::KnockDown : PushTier::Shove;
    return r;
}

// Final review I1: the grip is withheld from the game from a grab's START until the grip OPENS - through the break-free timer and
// after a game-side refusal - not only while the grab machine holds. Returns "withhold now".
class GripLatch {
public:
    bool Update(bool startEvent, bool gripDown)
    {
        if (!gripDown) {
            on_ = false;
            return false;
        }
        if (startEvent) { on_ = true; }
        return on_;
    }

private:
    bool on_ = false;
};

// The origin the game's hit (+DC7190: dir = normalise(target - origin)) needs so that its direction is `dir`.
inline void OriginForDirection(const float target[3], const float dir[3], float back, float outOrigin[3])
{
    for (int i = 0; i < 3; ++i) { outOrigin[i] = target[i] - dir[i] * back; }
}

// One push per contact: armed on entering contact; firing disarms; re-armed by leaving the body (>= kExitS out) or by rearmS since
// the last push while still in contact. A slow touch does not spend the arm.
class PushArm {
public:
    bool Fire(bool inContact, bool overThreshold, double nowS, float rearmS)
    {
        if (!inContact) {
            if (outSince_ < 0.0) { outSince_ = nowS; }
            if (nowS - outSince_ >= kExitS) { armed_ = true; }
            return false;
        }
        outSince_ = -1.0;
        if (!armed_ && nowS - lastFire_ >= static_cast<double>(rearmS) - 1e-9) { armed_ = true; }
        if (armed_ && overThreshold) {
            armed_ = false;
            lastFire_ = nowS;
            return true;
        }
        return false;
    }

private:
    bool armed_ = true;
    double lastFire_ = -1e9;
    double outSince_ = -1.0;
};

enum class GrabEvent { None, Start, Release, BreakFree };

// Start only on a grip PRESS edge while the hand is in contact; Release when the grip opens while holding; BreakFree once when held
// >= breakFreeS (then not holding - a new grab needs a new press).
class GrabMachine {
public:
    GrabEvent Update(bool gripDown, bool inContact, double nowS, float breakFreeS)
    {
        const bool press = gripDown && !prevGrip_;
        prevGrip_ = gripDown;
        if (holding_) {
            if (!gripDown) {
                holding_ = false;
                return GrabEvent::Release;
            }
            if (nowS - startS_ >= static_cast<double>(breakFreeS)) {
                holding_ = false;
                return GrabEvent::BreakFree;
            }
            return GrabEvent::None;
        }
        if (press && inContact) {
            holding_ = true;
            startS_ = nowS;
            return GrabEvent::Start;
        }
        return GrabEvent::None;
    }
    bool Holding() const { return holding_; }

private:
    bool prevGrip_ = false;
    bool holding_ = false;
    double startS_ = 0.0;
};

// Let go at or above dropMps = a throw; below = just dropped.
inline bool IsThrow(const float handVel[3], float dropMps)
{
    const float s = std::sqrt(handVel[0] * handVel[0] + handVel[1] * handVel[1] + handVel[2] * handVel[2]);
    return s >= dropMps;
}

// The zombie close approach (spec 2026-10-31-zombie-close-approach-and-grab-intent-design.md §2): the game's melee range for a target,
// capped only for an ordinary zombie (ZombieLogicModule) targeting the player. Never raises a range; a non-finite cap changes nothing.
inline float CapZombieMeleeRange(float original, bool isZombieModule, bool targetIsPlayer, float cap)
{
    return isZombieModule && targetIsPlayer && std::isfinite(cap) && original > cap ? cap : original;
}

// The arm-only shove speed (2026-10-31 (6), Jay: "it was also factoring in the player running towards a zombie as arm acceleration";
// PG3-PG5: 13 of 18 shoves fired with the hand-minus-head speed under 1.2 m/s - the old per-update difference of the controller alone
// counted body moves and repeated poses). Feed it the hand MINUS the head (both in the same tracking space) only when a NEW pose
// arrives, with the time it was first seen; Speed = the 3D distance from the oldest sample in the window to the newest over their
// time span - walking, turning, roomscale and recentres move hand and head together and cancel; vertical counts.
class ArmSpeedMeter {
public:
    static constexpr double kMinSpanS = 0.05;   // shorter history = no reading yet
    static constexpr double kMaxSpanS = 0.15;   // older samples are not used
    static constexpr double kStaleS = 0.20;     // no new pose for this long = not moving as far as we know
    void Add(double t, const float rel[3])
    {
        if (n_ > 0) {
            const Sample& last = s_[(head_ + n_ - 1) % kCap];
            if (t <= last.t || (rel[0] == last.p[0] && rel[1] == last.p[1] && rel[2] == last.p[2])) { return; }   // not a new pose
        }
        if (n_ == kCap) { head_ = (head_ + 1) % kCap; --n_; }
        Sample& s = s_[(head_ + n_) % kCap];
        s.t = t;
        for (int i = 0; i < 3; ++i) { s.p[i] = rel[i]; }
        ++n_;
    }
    void Reset() { n_ = 0; head_ = 0; }
    float Speed(double now) const
    {
        if (n_ < 2) { return 0.0f; }
        const Sample& b = s_[(head_ + n_ - 1) % kCap];
        if (now - b.t > kStaleS) { return 0.0f; }
        for (int k = 0; k < n_ - 1; ++k) {   // oldest first: the first sample inside the window
            const Sample& a = s_[(head_ + k) % kCap];
            const double span = b.t - a.t;
            if (span > kMaxSpanS) { continue; }
            if (span < kMinSpanS) { return 0.0f; }
            const float dx = b.p[0] - a.p[0], dy = b.p[1] - a.p[1], dz = b.p[2] - a.p[2];
            return static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz) / span);
        }
        return 0.0f;
    }

private:
    struct Sample { double t; float p[3]; };
    static constexpr int kCap = 32;
    Sample s_[kCap] = {};
    int head_ = 0, n_ = 0;
};

// The player's AwayFromAIHelper size (INV-669): a Y-ellipsoid (XZ, Y, XZ) made from one shared gamedll value. Returns 1 = write `out`,
// 0 = already right, -1 = the current value is neither the game's `orig` nor `ours` (the last value we wrote; null = none) - never
// written. On: XZ = `want` clamped to 0.1..orig, Y kept; off (or a bad want): the game's own value.
// The game's own helper size, EXACTLY as the gamedll stores it (file bytes 99 99 19 3F / CC CC CC 3E = 0.59999996 / 0.39999998, read
// 10-31 (6) after RUN PG7 refused it - 0.6f / 0.4f are one bit away and never compare equal).
inline void HelperGameSize(float out[3])
{
    const uint32_t xz = 0x3F199999u, y = 0x3ECCCCCCu;
    std::memcpy(&out[0], &xz, 4);
    std::memcpy(&out[1], &y, 4);
    std::memcpy(&out[2], &xz, 4);
}

inline int HelperPlan(bool on, float want, const float cur[3], const float orig[3], const float ours[3], float out[3])
{
    const float xz = !std::isfinite(want) ? orig[0] : want < 0.1f ? 0.1f : want > orig[0] ? orig[0] : want;
    out[0] = on ? xz : orig[0];
    out[1] = orig[1];
    out[2] = on ? xz : orig[2];
    const auto same = [](const float a[3], const float b[3]) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2]; };
    if (same(cur, out)) { return 0; }
    if (same(cur, orig) || (ours && same(cur, ours))) { return 1; }
    return -1;
}

// Grab intent (spec §3): a hand squeezed at least `threshold` at the moment it would shove is reaching to grab. A failed read = open.
// canGrab = this hand could start a grab now (vr_grip on, the grip not owned by another feature - final review I3); false = open.
inline bool GrabIntent(bool canGrab, bool haveSqueeze, float squeeze, float threshold)
{
    return canGrab && haveSqueeze && squeeze >= threshold;
}

}  // namespace pushgrab
}  // namespace dlvr
