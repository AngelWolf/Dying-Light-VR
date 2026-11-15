// coopnet_logic.h - the co-op arm-pose channel's PURE logic: the packet layout and the jitter buffer.
// Spec docs/superpowers/specs/2026-11-13-coop-arm-pose-channel-design.md; plan docs/superpowers/plans/2026-11-13-coop-arm-pose-channel.md
// Task 2; trace docs/traces/20261113_1840_coopnet_logic.md. No Windows, no game memory, no globals, no threads - unit-tested by
// src/src/tests/coopnet_logic_test.cpp (cl, not part of the DLL).
//
// Jay's rules this encodes: "Smooth with a small buffer is better than stutter" (every frame sent, played back a little behind real
// time through an adaptive delay); data stops -> after the timeout the partner's body eases back to the game's animation, and eases
// back on when data returns ("Yes, that matches"); a partner on another mod build = no data.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "fingertrack_logic.h"   // ft::Weights - the per-hand finger state the local finger pass already eases

namespace dlvr {
namespace coop {

constexpr uint8_t kCoopProto = 4;     // first byte of every packet; another value = another protocol = no data
                                      // 2 (2026-11-13, after RUN CO1): + rootPos (the position key, INV-734)
                                      // 3 (2026-11-13, INV-736; trace 20261113_2150): + rootYaw; hand[0..2] = the WRIST target,
                                      //   hand[3..6] = the hand BONE's rotation (unless handState bit 0x40)
                                      // 4 (2026-11-15, trace 20261115_1735_coop_arms_world): + elbow[2][3], the sender's elbow bend
                                      //   direction per arm (unless handState bit 0x20); rootYaw now PLACES the arms (receiver)
constexpr size_t  kMaxPacket = 1170;  // the EOS P2P maximum packet size

// ---- The body frame (plan Task 4; trace docs/traces/20261113_1850_coop_sender.md) ----
// Ry(yaw) is the solve's own yaw matrix [c 0 s; 0 1 0; -s 0 c] (boneblend.cpp SolveArmIk, the `ry` beside cWorld), so x' = c x + s z,
// z' = -s x + c z; its quaternion is (0, sin(yaw/2), 0, cos(yaw/2)), xyzw.
inline void RotYVec(float yaw, const float in[3], float out[3])
{
    const float c = std::cos(yaw), s = std::sin(yaw);
    const float x = in[0], y = in[1], z = in[2];
    out[0] = c * x + s * z;
    out[1] = y;
    out[2] = -s * x + c * z;
}
// out = qY(yaw) x q (rotate q by yaw about +Y, applied after q). xyzw.
inline void RotYQuat(float yaw, const float q[4], float out[4])
{
    const float hy = std::sin(0.5f * yaw), hw = std::cos(0.5f * yaw);
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    out[0] = hw * x + hy * z;
    out[1] = hw * y + hy * w;
    out[2] = hw * z - hy * x;
    out[3] = hw * w - hy * y;
}
// A pose (xyz + quat xyzw) from the world / tracking frame into a frame turned by `yaw` about +Y, and back.
inline void ToBodyFrame(float yaw, const float pose[7], float out[7])
{
    RotYVec(-yaw, pose, out);
    RotYQuat(-yaw, pose + 3, out + 3);
}
inline void FromBodyFrame(float yaw, const float pose[7], float out[7])
{
    RotYVec(yaw, pose, out);
    RotYQuat(yaw, pose + 3, out + 3);
}

// One frame of a player's solve INPUTS. Poses = position xyz + quaternion xyzw, relative to the sender's body root (position + yaw).
struct PoseSample {
    uint32_t   seq;           // per sender, increasing
    double     sendMs;        // the SENDER's clock - never compared directly with the receiver's
    float      head[7];
    float      hand[2][7];    // [0] = left, [1] = right. Protocol 3: xyz = the WRIST target (the grip point minus the rig's INV-161
                              // step-back), quat = the hand BONE's world rotation (handState bit 0x40: the controller's, no bone yet)
    ft::Weights finger[2];    // eased index / thumb / lower weights per hand
    uint8_t    handState[2];  // the sender's own grip / reload / two-hand state per hand (its owners' enums)
    uint64_t   netId;         // IGSObject::GetNetID of the sender's PlayerDI - a PER-MACHINE id (INV-734), diagnostic only
    uint32_t   build;         // the sender's mod build id
    float      rootPos[3];    // the sender's body root, WORLD metres - the receiver's key to the right partner body (INV-734)
    float      rootYaw;       // the sender's body root WORLD yaw, rad (atan2 of element 0's column 0, x / z). Protocol 4: the yaw the
                              // receiver rebuilds the hands with (the frame the sample was built in), interpolated shortest-arc
    float      elbow[2][3];   // [0] left, [1] right: unit vector from the sender's shoulder->wrist line to its elbow, body frame
                              // (Ry(-rootYaw) x world). handState bit 0x20 (kHandNoElbowBit): none - straight arm / not published
};

namespace detail {
template <typename T> inline void Put(uint8_t*& p, const T& v) { std::memcpy(p, &v, sizeof(T)); p += sizeof(T); }
template <typename T> inline void Get(const uint8_t*& p, T& v) { std::memcpy(&v, p, sizeof(T)); p += sizeof(T); }
constexpr size_t kPayload = sizeof(uint32_t) + sizeof(double) + 7 * sizeof(float) + 14 * sizeof(float) + 6 * sizeof(float) +
                            2 * sizeof(uint8_t) + sizeof(uint64_t) + sizeof(uint32_t) + 3 * sizeof(float) + sizeof(float) +
                            6 * sizeof(float);
constexpr size_t kPacketSize = 1 + kPayload;
static_assert(kPacketSize <= kMaxPacket, "a pose packet must fit one EOS P2P packet");
}  // namespace detail

// Little-endian (x64 native), version byte first. Returns the bytes written, 0 when `cap` is too small.
inline size_t Pack(const PoseSample& s, uint8_t* out, size_t cap)
{
    if (!out || cap < detail::kPacketSize) { return 0; }
    uint8_t* p = out;
    detail::Put(p, kCoopProto);
    detail::Put(p, s.seq);
    detail::Put(p, s.sendMs);
    for (float v : s.head) { detail::Put(p, v); }
    for (int h = 0; h < 2; ++h) { for (float v : s.hand[h]) { detail::Put(p, v); } }
    for (int h = 0; h < 2; ++h) { detail::Put(p, s.finger[h].index); detail::Put(p, s.finger[h].thumb); detail::Put(p, s.finger[h].lower); }
    detail::Put(p, s.handState[0]);
    detail::Put(p, s.handState[1]);
    detail::Put(p, s.netId);
    detail::Put(p, s.build);
    for (float v : s.rootPos) { detail::Put(p, v); }
    detail::Put(p, s.rootYaw);
    for (int h = 0; h < 2; ++h) { for (float v : s.elbow[h]) { detail::Put(p, v); } }
    return static_cast<size_t>(p - out);
}

// handState bit: the sender had no solve for this hand (= coopik::kNoData; static_assert in coopik_logic.h). Such a hand carries the
// sender's zero pose (cooppose.cpp), so only finiteness is required of it.
constexpr uint8_t kHandNoDataBit = 0x80;
// handState bit (protocol 4): elbow[h] carries no direction (= coopik::kNoElbow; static_assert in coopik_logic.h). Only finiteness is
// required of it then.
constexpr uint8_t kHandNoElbowBit = 0x20;
constexpr float   kMaxPoseM = 1.0e4f;   // B5 (audit S17 17-2): no position the game can hold is this far from the origin

namespace detail {
inline bool Finite(const float* v, int n)
{
    for (int i = 0; i < n; ++i) { if (!std::isfinite(v[i])) { return false; } }
    return true;
}
inline bool PosInRange(const float* p) { return std::fabs(p[0]) <= kMaxPoseM && std::fabs(p[1]) <= kMaxPoseM && std::fabs(p[2]) <= kMaxPoseM; }
// A pose's quaternion (xyzw at q[0..3]) must be near unit length; it is then normalised in place.
inline bool UnitQuat(float* q)
{
    const float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(len >= 0.9f && len <= 1.1f)) { return false; }
    for (int i = 0; i < 4; ++i) { q[i] /= len; }
    return true;
}
inline float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
// A direction (3 floats) must be near unit length; it is then normalised in place.
inline bool UnitVec(float* v)
{
    const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(len >= 0.9f && len <= 1.1f)) { return false; }
    for (int i = 0; i < 3; ++i) { v[i] /= len; }
    return true;
}
}  // namespace detail

// B5 (audit S17 17-2, Jay 2026-11-15 "Do both together"; trace 20261115_1400_b5_coop_unpack_and_tick_hook): a peer's numbers reach
// another player's arm bones on this machine, so a sample is checked before anything uses it. False (and `out` untouched) for: any
// non-finite float or send time; a head / root / hand-with-data position beyond kMaxPoseM; a head / hand-with-data quaternion whose
// length is outside 0.9..1.1 (accepted ones are normalised). Finger weights are clamped to 0..1. `why` (optional) names the reason.
inline bool ValidateSample(PoseSample& s, const char** why)
{
    const char* bad = nullptr;
    if (!std::isfinite(s.sendMs) || !detail::Finite(s.head, 7) || !detail::Finite(s.hand[0], 7) || !detail::Finite(s.hand[1], 7) ||
        !detail::Finite(s.rootPos, 3) || !std::isfinite(s.rootYaw) || !detail::Finite(s.elbow[0], 3) || !detail::Finite(s.elbow[1], 3)) {
        bad = "a non-finite value (NaN / Inf)";
    } else {
        for (int h = 0; h < 2 && !bad; ++h) {
            const float f[3] = { s.finger[h].index, s.finger[h].thumb, s.finger[h].lower };
            if (!detail::Finite(f, 3)) { bad = "a non-finite finger weight"; }
        }
    }
    if (!bad && (!detail::PosInRange(s.head) || !detail::PosInRange(s.rootPos))) { bad = "a head / root position out of range"; }
    if (!bad && !detail::UnitQuat(s.head + 3)) { bad = "a head rotation that is not a unit quaternion"; }
    for (int h = 0; h < 2 && !bad; ++h) {
        if (s.handState[h] & kHandNoDataBit) { continue; }
        if (!detail::PosInRange(s.hand[h])) { bad = "a hand position out of range"; }
        else if (!detail::UnitQuat(s.hand[h] + 3)) { bad = "a hand rotation that is not a unit quaternion"; }
    }
    // protocol 4: an elbow direction the sender says it has must be a unit vector (whatever the hand's own data bit says)
    for (int h = 0; h < 2 && !bad; ++h) {
        if (s.handState[h] & kHandNoElbowBit) { continue; }
        if (!detail::UnitVec(s.elbow[h])) { bad = "an elbow direction that is not a unit vector"; }
    }
    if (bad) {
        if (why) { *why = bad; }
        return false;
    }
    for (int h = 0; h < 2; ++h) {
        s.finger[h].index = detail::Clamp01(s.finger[h].index);
        s.finger[h].thumb = detail::Clamp01(s.finger[h].thumb);
        s.finger[h].lower = detail::Clamp01(s.finger[h].lower);
    }
    return true;
}

// False for a short buffer, another protocol version, or a sample ValidateSample refuses; `out` untouched then.
inline bool Unpack(const uint8_t* in, size_t n, PoseSample* out, const char** why = nullptr)
{
    if (why) { *why = nullptr; }
    if (!in || !out || n < detail::kPacketSize || in[0] != kCoopProto) {
        if (why) { *why = "a short packet or another protocol"; }
        return false;
    }
    const uint8_t* p = in + 1;
    PoseSample s{};
    detail::Get(p, s.seq);
    detail::Get(p, s.sendMs);
    for (float& v : s.head) { detail::Get(p, v); }
    for (int h = 0; h < 2; ++h) { for (float& v : s.hand[h]) { detail::Get(p, v); } }
    for (int h = 0; h < 2; ++h) { detail::Get(p, s.finger[h].index); detail::Get(p, s.finger[h].thumb); detail::Get(p, s.finger[h].lower); }
    detail::Get(p, s.handState[0]);
    detail::Get(p, s.handState[1]);
    detail::Get(p, s.netId);
    detail::Get(p, s.build);
    for (float& v : s.rootPos) { detail::Get(p, v); }
    detail::Get(p, s.rootYaw);
    for (int h = 0; h < 2; ++h) { for (float& v : s.elbow[h]) { detail::Get(p, v); } }
    if (!ValidateSample(s, why)) { return false; }
    *out = s;
    return true;
}

// The position key (after RUN CO1, INV-734): the index of the peer whose sent root is nearest `body` (world), among those with a
// sample (`have`) that no other body holds (`taken`), within `maxM` metres; -1 when none qualifies.
inline int NearestPeer(const float body[3], const float pos[][3], const bool* have, const bool* taken, int n, float maxM)
{
    int best = -1;
    float bestD2 = maxM * maxM;
    for (int i = 0; i < n; ++i) {
        if (!have[i] || taken[i]) { continue; }
        const float dx = pos[i][0] - body[0], dy = pos[i][1] - body[1], dz = pos[i][2] - body[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 <= bestD2) { bestD2 = d2; best = i; }
    }
    return best;
}

// Item 2 of the 2026-11-14 proposal (trace 20261115_1735_coop_arms_world): whether a body keeps the peer it is bound to. NO distance
// cut-off (the old 4.0 m keep dropped the 3rd player 13 / 11 times in CO3). Released only when: the peer has no live samples
// (`live` = a sample AND buffer weight > 0 - the existing stale test), another body holds it (`taken`), or another UNBOUND body is a
// clearly nearer match - within `bindM` of the peer's sent root AND at least `marginM` nearer than this body (`dOther` < 0 = none).
// `*why` (optional) names the release: 0 kept, 1 no live samples, 2 held by another body, 3 another body nearer.
inline bool KeepBoundPeer(bool live, bool taken, float dThis, float dOther, float bindM, float marginM, int* why = nullptr)
{
    int r = 0;
    if (!live) { r = 1; }
    else if (taken) { r = 2; }
    else if (dOther >= 0.0f && dOther <= bindM && dOther + marginM < dThis) { r = 3; }
    if (why) { *why = r; }
    return r == 0;
}

// Shortest-arc interpolation of an angle in radians (a +179 / -179 deg pair passes through 180, not 0); result wrapped to (-pi, pi].
inline float LerpAngle(float a, float b, float t)
{
    constexpr float kPi = 3.14159265358979f, k2Pi = 6.28318530717959f;
    float d = std::fmod(b - a, k2Pi);
    if (d > kPi) { d -= k2Pi; }
    if (d <= -kPi) { d += k2Pi; }
    float r = a + d * t;
    r = std::fmod(r, k2Pi);
    if (r > kPi) { r -= k2Pi; }
    if (r <= -kPi) { r += k2Pi; }
    return r;
}

// Quaternion slerp (xyzw), shortest path, normalised.
inline void Slerp(const float* a, const float* b, float t, float* out)
{
    float bb[4] = { b[0], b[1], b[2], b[3] };
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0.0f) { d = -d; for (float& v : bb) { v = -v; } }
    float wa = 1.0f - t, wb = t;
    if (d < 0.9995f) {
        const float th = std::acos(std::min(d, 1.0f));
        const float s = std::sin(th);
        wa = std::sin((1.0f - t) * th) / s;
        wb = std::sin(t * th) / s;
    }
    float len = 0.0f;
    for (int i = 0; i < 4; ++i) { out[i] = wa * a[i] + wb * bb[i]; len += out[i] * out[i]; }
    len = std::sqrt(len);
    if (len > 1e-8f) { for (int i = 0; i < 4; ++i) { out[i] /= len; } }
}

inline void LerpPose(const float* a, const float* b, float t, float* out)
{
    for (int i = 0; i < 3; ++i) { out[i] = a[i] + (b[i] - a[i]) * t; }
    Slerp(a + 3, b + 3, t, out + 3);
}

// One peer's buffer. Push from the network, Sample once per rig tick.
class JitterBuffer {
public:
    static constexpr int   kCap = 64;
    static constexpr float kMaxDelayMs = 100.0f;

    // Same protocol AND the same mod build, else the peer is "no data".
    bool Accept(uint8_t proto, uint32_t build, uint32_t ourBuild) const { return proto == kCoopProto && build == ourBuild; }

    void SetParams(float timeoutMs, float easeMs, float delayMinMs)
    {
        m_timeoutMs = timeoutMs;
        m_easeMs = easeMs;
        m_delayMinMs = delayMinMs;
    }

    // A sample with a seq not newer than the newest kept is dropped (late / duplicate).
    void Push(const PoseSample& s, double arriveMs)
    {
        if (m_n > 0 && s.seq <= m_s[(m_head + m_n - 1) % kCap].seq) { return; }
        if (m_n == kCap) { m_head = (m_head + 1) % kCap; --m_n; }
        m_s[(m_head + m_n) % kCap] = s;
        ++m_n;
        m_off[m_offN % kCap] = arriveMs - s.sendMs;
        ++m_offN;
        m_lastArriveMs = arriveMs;
        m_have = true;
    }

    int Count() const { return m_n; }

    // clamp(2 x the 90th-percentile deviation of (arrive - send) above its minimum, over the last 64 arrivals, delayMin, 100 ms).
    float PlaybackDelayMs() const
    {
        const int n = static_cast<int>(std::min<size_t>(m_offN, kCap));
        if (n == 0) { return m_delayMinMs; }
        double dev[kCap];
        const double mn = MinOffset();
        for (int i = 0; i < n; ++i) { dev[i] = m_off[i] - mn; }
        std::sort(dev, dev + n);
        const double p90 = dev[static_cast<int>(0.9 * (n - 1))];
        return static_cast<float>(std::clamp(2.0 * p90, static_cast<double>(m_delayMinMs),
                                             static_cast<double>(std::max(kMaxDelayMs, m_delayMinMs))));
    }

    // The pose at sender time t: interpolated between the two samples around it; before the oldest / after the newest = held.
    bool SampleAtSenderTime(double t, PoseSample* out) const
    {
        if (m_n == 0 || !out) { return false; }
        const PoseSample& first = At(0);
        const PoseSample& last = At(m_n - 1);
        if (t <= first.sendMs) { *out = first; return true; }
        if (t >= last.sendMs) { *out = last; return true; }
        for (int i = 1; i < m_n; ++i) {
            const PoseSample& b = At(i);
            if (b.sendMs < t) { continue; }
            const PoseSample& a = At(i - 1);
            const double span = b.sendMs - a.sendMs;
            const float k = span > 0.0 ? static_cast<float>((t - a.sendMs) / span) : 1.0f;
            PoseSample r = k < 0.5f ? a : b;   // ids / states from the nearer sample
            r.sendMs = t;
            LerpPose(a.head, b.head, k, r.head);
            for (int c = 0; c < 3; ++c) { r.rootPos[c] = a.rootPos[c] + (b.rootPos[c] - a.rootPos[c]) * k; }
            r.rootYaw = LerpAngle(a.rootYaw, b.rootYaw, k);   // protocol 4: the yaw places the hands - no step between samples
            for (int h = 0; h < 2; ++h) {
                // the elbow: nlerp when both samples carry one; else the nearer sample's (r already holds it and its handState)
                if (((a.handState[h] | b.handState[h]) & kHandNoElbowBit) == 0) {
                    float e[3], l2 = 0.0f;
                    for (int c = 0; c < 3; ++c) { e[c] = a.elbow[h][c] + (b.elbow[h][c] - a.elbow[h][c]) * k; l2 += e[c] * e[c]; }
                    const float l = std::sqrt(l2);
                    if (l > 1e-4f) { for (int c = 0; c < 3; ++c) { r.elbow[h][c] = e[c] / l; } }
                    // opposite directions (l ~ 0): the nearer sample's, already in r
                }
                LerpPose(a.hand[h], b.hand[h], k, r.hand[h]);
                r.finger[h].index = a.finger[h].index + (b.finger[h].index - a.finger[h].index) * k;
                r.finger[h].thumb = a.finger[h].thumb + (b.finger[h].thumb - a.finger[h].thumb) * k;
                r.finger[h].lower = a.finger[h].lower + (b.finger[h].lower - a.finger[h].lower) * k;
            }
            *out = r;
            return true;
        }
        *out = last;
        return true;
    }

    // Once per rig tick on the receiver's clock. `weight` 0..1 = how much the body follows the controllers (vs the game's animation):
    // eases toward 1 while data is fresh, toward 0 once nothing arrived for timeoutMs, over easeMs. Playback never steps backwards.
    bool Sample(double nowMs, PoseSample* out, float* weight)
    {
        const double dt = m_haveNow ? std::max(0.0, nowMs - m_lastNowMs) : 0.0;
        m_lastNowMs = nowMs;
        m_haveNow = true;
        const float target = (m_have && nowMs - m_lastArriveMs <= m_timeoutMs) ? 1.0f : 0.0f;
        if (m_easeMs <= 0.0f) {
            m_weight = target;
        } else {
            const float step = static_cast<float>(dt / m_easeMs);
            m_weight = target > m_weight ? std::min(target, m_weight + step) : std::max(target, m_weight - step);
        }
        if (weight) { *weight = m_weight; }
        if (m_n == 0) { return false; }
        double play = nowMs - MinOffset() - PlaybackDelayMs();
        if (m_havePlay && play < m_lastPlay) { play = m_lastPlay; }
        m_lastPlay = play;
        m_havePlay = true;
        return SampleAtSenderTime(play, out);
    }

private:
    const PoseSample& At(int i) const { return m_s[(m_head + i) % kCap]; }
    double MinOffset() const
    {
        const int n = static_cast<int>(std::min<size_t>(m_offN, kCap));
        double mn = m_off[0];
        for (int i = 1; i < n; ++i) { mn = std::min(mn, m_off[i]); }
        return mn;
    }

    PoseSample m_s[kCap] = {};
    int        m_head = 0, m_n = 0;
    double     m_off[kCap] = {};
    size_t     m_offN = 0;
    double     m_lastArriveMs = 0.0, m_lastNowMs = 0.0, m_lastPlay = 0.0;
    bool       m_have = false, m_haveNow = false, m_havePlay = false;
    float      m_weight = 0.0f;
    float      m_timeoutMs = 500.0f, m_easeMs = 150.0f, m_delayMinMs = 0.0f;
};

}  // namespace coop
}  // namespace dlvr
