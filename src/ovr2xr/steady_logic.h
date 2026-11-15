// Pure logic for hand steadiness - docs/SPEC-VR-COMFORT-2026-09-30.md §6 (C1).
// A 1-euro filter (Casiez, Roussel, Vogel 2012) on a controller pose: an exponential low-pass whose cutoff rises with
// the hand's speed - heavy smoothing when the hand is still (tremor, tracking noise), almost none on a deliberate
// move (no lag). No OpenXR, no Windows, no logging, so src/src/tests/steady_logic_test.cpp compiles it on its own.
// controllers.cpp owns the per-frame call and the published copies.
#pragma once

#include <cmath>

namespace dlvr {
namespace steady {

struct V3 { float x, y, z; };
struct Q  { float x, y, z, w; };

struct Params {
    float minCutoffHz;   // the cutoff when the hand is still - lower = steadier (and laggier when still)
    float betaPos;       // Hz of extra cutoff per m/s of hand speed
    float betaRot;       // Hz of extra cutoff per rad/s of hand rotation speed
    float dCutoffHz;     // the speed estimate's own low-pass
};

// A frame gap longer than this (tracking lost, a stall, a pause) restarts the filter from the raw pose rather than
// blending from where the hand was.
constexpr float kMaxDt = 0.25f;

// The two VR Comfort sliders, 0-100 each. STRENGTH: minCutoff from 5 Hz (0, light) down to 0.3 Hz (100, heavy) on a log
// scale, 50 = 1.22 Hz. RESPONSE: scales both betas from 0.1x (0) through 1x (50) to 10x (100). The 1x betas are
// starting values from the 1-euro tuning method (beta raised until a fast move stops lagging), NOT a measurement:
// 20 Hz per m/s puts a 0.5 m/s reach at ~11 Hz; 3 Hz per rad/s puts a 180 deg/s re-aim at ~11 Hz.
inline Params FromSliders(int strengthPct, int responsePct)
{
    const float s = static_cast<float>(strengthPct < 0 ? 0 : (strengthPct > 100 ? 100 : strengthPct)) / 100.0f;
    const float r = static_cast<float>(responsePct < 0 ? 0 : (responsePct > 100 ? 100 : responsePct)) / 100.0f;
    const float scale = std::pow(10.0f, 2.0f * r - 1.0f);
    return { 5.0f * std::pow(0.3f / 5.0f, s), 20.0f * scale, 3.0f * scale, 1.0f };
}

// MELEE (Jay 2026-09-30: "a very SLIGHT filtering on for the melee weapons ... to stop the most MINUTE of hand
// movements"). The same filter on the maximum-response curve (betas x10: 200 Hz per m/s, 30 Hz per rad/s), so at swing
// speed the cutoff is hundreds of Hz and the output is the raw pose to within millimetres - the swing's hit sweep reads
// the rig's solve (swing.cpp SwingGripRead). Only the one slider; 0 means OFF, which the CALLER handles (raw pose).
constexpr int kMeleeDefaultStrength = 15;   // still cutoff ~3.3 Hz, vs the gun's 1.22 Hz at strength 50
inline Params MeleeFromSlider(int strengthPct) { return FromSliders(strengthPct, 100); }

inline float Alpha(float cutoffHz, float dt)
{
    const float tau = 1.0f / (6.28318531f * cutoffHz);
    return 1.0f / (1.0f + tau / dt);
}

inline float Dot(const Q& a, const Q& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

inline Q Normalize(const Q& q)
{
    const float n = std::sqrt(Dot(q, q));
    if (!(n > 1e-8f)) { return { 0.0f, 0.0f, 0.0f, 1.0f }; }
    return { q.x / n, q.y / n, q.z / n, q.w / n };
}

// From a toward b by t, shortest way round (b is flipped into a's hemisphere first).
inline Q Slerp(const Q& a, Q b, float t)
{
    float d = Dot(a, b);
    if (d < 0.0f) { b = { -b.x, -b.y, -b.z, -b.w }; d = -d; }
    if (d > 0.9995f) {
        return Normalize({ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t });
    }
    const float th = std::acos(d);
    const float s = std::sin(th);
    const float wa = std::sin((1.0f - t) * th) / s;
    const float wb = std::sin(t * th) / s;
    return { a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb };
}

class PoseFilter {
public:
    void Reset() { primed_ = false; }

    // dt in seconds since the previous call. outP/outQ always receive a pose (never left unset).
    void Update(const V3& p, const Q& qIn, float dt, const Params& k, V3& outP, Q& outQ)
    {
        const Q q = Normalize(qIn);
        const bool finite = std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) && std::isfinite(q.w);
        if (!primed_ || dt > kMaxDt || !finite) {
            primed_ = finite;
            hatP_ = prevP_ = p;
            hatQ_ = prevQ_ = q;
            dP_ = { 0.0f, 0.0f, 0.0f };
            dW_ = { 0.0f, 0.0f, 0.0f };
            outP = p;
            outQ = q;
            return;
        }
        if (!(dt > 0.0f)) {   // same display time located twice: nothing new to filter
            outP = hatP_;
            outQ = hatQ_;
            return;
        }
        const float aD = Alpha(k.dCutoffHz, dt);

        // Position: velocity VECTOR low-passed, so noise that alternates direction averages out instead of reading
        // as speed.
        const V3 v{ (p.x - prevP_.x) / dt, (p.y - prevP_.y) / dt, (p.z - prevP_.z) / dt };
        dP_ = { dP_.x + aD * (v.x - dP_.x), dP_.y + aD * (v.y - dP_.y), dP_.z + aD * (v.z - dP_.z) };
        const float speed = std::sqrt(dP_.x * dP_.x + dP_.y * dP_.y + dP_.z * dP_.z);
        const float aP = Alpha(k.minCutoffHz + k.betaPos * speed, dt);
        hatP_ = { hatP_.x + aP * (p.x - hatP_.x), hatP_.y + aP * (p.y - hatP_.y), hatP_.z + aP * (p.z - hatP_.z) };

        // Rotation: angular velocity VECTOR from r = q * prev^-1 (small-angle: 2 * r.xyz / dt), same reasoning.
        Q qc = q;
        if (Dot(qc, prevQ_) < 0.0f) { qc = { -qc.x, -qc.y, -qc.z, -qc.w }; }
        const Q pi{ -prevQ_.x, -prevQ_.y, -prevQ_.z, prevQ_.w };
        const Q r{ qc.w * pi.x + qc.x * pi.w + qc.y * pi.z - qc.z * pi.y,
                   qc.w * pi.y - qc.x * pi.z + qc.y * pi.w + qc.z * pi.x,
                   qc.w * pi.z + qc.x * pi.y - qc.y * pi.x + qc.z * pi.w,
                   qc.w * pi.w - qc.x * pi.x - qc.y * pi.y - qc.z * pi.z };
        const float sgn = r.w < 0.0f ? -1.0f : 1.0f;
        const V3 w{ 2.0f * sgn * r.x / dt, 2.0f * sgn * r.y / dt, 2.0f * sgn * r.z / dt };
        dW_ = { dW_.x + aD * (w.x - dW_.x), dW_.y + aD * (w.y - dW_.y), dW_.z + aD * (w.z - dW_.z) };
        const float angSpeed = std::sqrt(dW_.x * dW_.x + dW_.y * dW_.y + dW_.z * dW_.z);
        const float aR = Alpha(k.minCutoffHz + k.betaRot * angSpeed, dt);
        hatQ_ = Normalize(Slerp(hatQ_, qc, aR));

        prevP_ = p;
        prevQ_ = qc;
        speed_ = speed;
        angSpeed_ = angSpeed;
        outP = hatP_;
        outQ = hatQ_;
    }

    float Speed() const { return speed_; }         // filtered m/s, for the probe
    float AngSpeed() const { return angSpeed_; }   // filtered rad/s, for the probe

private:
    bool primed_ = false;
    V3 hatP_{}, prevP_{}, dP_{};
    Q  hatQ_{ 0.0f, 0.0f, 0.0f, 1.0f }, prevQ_{ 0.0f, 0.0f, 0.0f, 1.0f };
    V3 dW_{};
    float speed_ = 0.0f, angSpeed_ = 0.0f;
};

}  // namespace steady
}  // namespace dlvr
