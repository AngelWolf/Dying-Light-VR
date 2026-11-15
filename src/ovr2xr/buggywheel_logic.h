// buggywheel_logic.h - the buggy's hands: the key turn and the gripped wheel, as decisions only (unit-tested:
// src/src/tests/buggywheel_logic_test.cpp). Spec docs/superpowers/specs/2026-11-10-buggy-wheel-and-key-design.md, plan
// docs/superpowers/plans/2026-11-10-buggy-wheel-and-key.md Task 1. Game facts: INV-715, INV-716 (+ addenda). No game access, no
// logging, no allocation. buggywheel.cpp fills a Frame each CarBuggyVis update and carries Out to the game.
//
// THE RULES (each pinned by a test):
//  - not enabled, or not the local player's buggy: Reset(), nothing out (the stick and the game untouched).
//  - KEY NEEDED = the game's engine is off and its start timer idle (on byte 0, timer < 0, INV-716 add. 4) - read from the game,
//    no latch of ours. While needed (and a key point exists) the raw throttle / brake are held at 0, so the game's own start
//    (smoothed throttle / brake > 0.55, +10574A0) cannot fire.
//  - the RIGHT hand gripping within kGrabRadiusM of the key point while needed = the key; turning it clockwise (the grip's
//    twist about its own Z going NEGATIVE, twist_logic.h's convention) ticks every kTickDeg; at kQuarterDeg the game's start
//    timer is started (only if the game would allow a start: canStart) and the key grip ends. Let go early = nothing.
//  - a hand gripping within kGrabRadiusM of the rim = the wheel. The wheel starts at the game's own angle (no jump), and follows
//    the MEAN of the gripping hands' turn about the column since the last change of who grips (no jump when a hand joins or
//    leaves), clamped to the game's full lock at this speed. Hand wins over the stick (Jay): the raw turn is written while gripped.
//  - a grip already held when a hand first becomes eligible, or after tracking returns, is not a press (twohand_logic.h's rule).
#pragma once

#include <cmath>

namespace dlvr {
namespace buggywheel {

constexpr int   kLeft  = 0;
constexpr int   kRight = 1;
constexpr float kGrabRadiusM = 0.10f;     // RigTwoHandPinRadius's shipped default (twohand.cpp:51) - my call, spec §9
constexpr float kQuarterDeg  = 30.0f;     // Jay RUN BK3: "30 deg" (was 90, "a quarter turn clockwise"; his BK3 turns stopped at 35-47)
constexpr float kTickDeg     = 10.0f;     // my call: 3 ticks to the start (was 15 of 90)
// RUN BK3 (Jay 2026-11-10: "the key was really hard to grab & twist"; "the hands ... could float") -> Jay picked all fixes, "30 deg",
// "And also make the hand LATCH TO THE KEY ONCE GRIPPED":
constexpr float kKeyGrabRadiusM = 0.15f;  // my call: the key wins over the rim within this of the FINGERTIPS while the engine is off
                                          // (the key sits 0.161 m from the wheel centre, 7 cm behind the rim - inside the rim's band)
constexpr float kRimAxialM   = 0.030f;    // the rim's plane along the wheel axis from the wheel element (mesh group 54: 1,217 rim
                                          // vertices 0.012 - 0.048 m, mean 0.0298; the axis sign = element X, y +0.483 vs the mesh +0.472)
// A wheel grip's point in the wheel's body frame (columns axis, ref, axis x ref) -> ON the rim: the rim's plane at the rim radius, at
// the angle the hand grabbed (on the axis - no angle - the reference direction).
inline void RimGrabLocal(const float local[3], float rimR, float out[3])
{
    const float r = std::sqrt(local[1] * local[1] + local[2] * local[2]);
    out[0] = kRimAxialM;
    if (!(r > 1e-6f)) { out[1] = rimR; out[2] = 0.0f; return; }
    out[1] = local[1] * rimR / r;
    out[2] = local[2] * rimR / r;
}
constexpr float kGripOn      = 0.5f;     // twohand_logic.h:20 kGripThreshold (one threshold, no hysteresis - copied)
constexpr float kGripOff     = 0.5f;

// THE KEY (Jay 2026-11-10: "The marked spot is fine" - backups/20261110_buggy_hand_renders/renders/key_marked_*.png): the key is part of
// the buggy's own mesh, right of the gauges (buggy model space (-0.149, 0.901, 0.456), from the vertices within 4.5 cm, +/- 1-2 cm).
// Stored RELATIVE TO THE BODY element 24 (the root the steering wheel and 86 other nodes hang from; rest world = 90 deg about Y, no
// translation: rel = (m.z, m.y, -m.x)), so it rides the body. kWheelRel is the steering wheel node 109 the same way - the runtime's
// control (the predicted wheel must match the live steeringwheel element, or the key stays off).
constexpr int   kBodyElem = 24;
constexpr float kKeyRel[3]   = { 0.456f, 0.901f, 0.149f };
constexpr float kWheelRel[3] = { 0.3656f, 0.880f, 0.0f };
constexpr float kKeyFrameTolM = 0.03f;   // predicted vs live wheel: over this the key frame is refused

// p = B x rel, B a 3x4 row-major world (translation at [3] [7] [11]) - GetElementWorldMatrix's layout.
inline void BodyPoint(const float B[12], const float rel[3], float out[3])
{
    for (int r = 0; r < 3; ++r) {
        out[r] = B[4 * r] * rel[0] + B[4 * r + 1] * rel[1] + B[4 * r + 2] * rel[2] + B[4 * r + 3];
    }
}

inline float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline float Wrap180(float d)
{
    while (d > 180.0f) { d -= 360.0f; }
    while (d < -180.0f) { d += 360.0f; }
    return d;
}

// The raw turn input whose game curve (+1063500: sign x |x|^1.5) gives wheelDeg / maxDeg of full lock.
inline float RawSteerFromWheel(float wheelDeg, float maxDeg)
{
    if (!(maxDeg > 0.0f)) { return 0.0f; }
    const float r = Clamp(wheelDeg / maxDeg, -1.0f, 1.0f);
    const float m = std::pow(std::fabs(r), 2.0f / 3.0f);
    return r < 0.0f ? -m : m;
}

// Degrees from `from` to `to` about `axis` (right-handed), both projected onto the plane normal to axis. 0 when either
// projection is shorter than 1e-4.
inline float SignedAngleAbout(const float axis[3], const float from[3], const float to[3])
{
    const float al = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (!(al > 1e-6f)) { return 0.0f; }
    const float a[3] = { axis[0] / al, axis[1] / al, axis[2] / al };
    float p[3], q[3];
    const float df = from[0] * a[0] + from[1] * a[1] + from[2] * a[2];
    const float dt = to[0] * a[0] + to[1] * a[1] + to[2] * a[2];
    for (int i = 0; i < 3; ++i) { p[i] = from[i] - df * a[i]; q[i] = to[i] - dt * a[i]; }
    const float lp = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    const float lq = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
    if (lp < 1e-4f || lq < 1e-4f) { return 0.0f; }
    const float c[3] = { p[1] * q[2] - p[2] * q[1], p[2] * q[0] - p[0] * q[2], p[0] * q[1] - p[1] * q[0] };
    const float s = c[0] * a[0] + c[1] * a[1] + c[2] * a[2];
    const float d = p[0] * q[0] + p[1] * q[1] + p[2] * q[2];
    return std::atan2(s, d) * 57.29577951f;
}

// ---- THE WHEEL HOLD (RUN BK4, Jay: "Use the hold you had in the render earlier") ----
// hold = a kBuggyHold row (buggy_hold.inc, tools/buggy_hand_poses.py): [0..2] the clip's frame-5 grip point, [3..5] its hand bone
// origin, [6..14] its hand bone rotation - all relative to the body element 24 (B = its live 3x4 world).
// Right-handed rotation about `axis` (need not be unit) - Rodrigues; the same sense as SignedAngleAbout and buggywheel.cpp's Rx.
inline void RotAxis(const float axis[3], float deg, float R[9])
{
    const float l = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    const float x = l > 1e-9f ? axis[0] / l : 1.0f, y = l > 1e-9f ? axis[1] / l : 0.0f, z = l > 1e-9f ? axis[2] / l : 0.0f;
    const float a = deg * 0.0174532925f, c = std::cos(a), s = std::sin(a), t = 1.0f - c;
    R[0] = t * x * x + c;     R[1] = t * x * y - s * z; R[2] = t * x * z + s * y;
    R[3] = t * x * y + s * z; R[4] = t * y * y + c;     R[5] = t * y * z - s * x;
    R[6] = t * x * z - s * y; R[7] = t * y * z + s * x; R[8] = t * z * z + c;
}
// The turn (deg) about the wheel axis that brings the hold's grip point round to the angle the hand grabbed at.
inline float HoldDeltaDeg(const float B[12], const float hold[15], const float centre[3], const float axis[3], const float ref[3],
                          const float grip[3])
{
    float hh[3];
    for (int r = 0; r < 3; ++r) { hh[r] = B[4 * r] * hold[0] + B[4 * r + 1] * hold[1] + B[4 * r + 2] * hold[2] + B[4 * r + 3] - centre[r]; }
    const float g[3] = { grip[0] - centre[0], grip[1] - centre[1], grip[2] - centre[2] };
    return Wrap180(SignedAngleAbout(axis, ref, g) - SignedAngleAbout(axis, ref, hh));
}
// The hold in the world, turned `deg` about the line (centre, axis): the grip point, the hand bone origin and its rotation.
inline void HoldWorld(const float B[12], const float hold[15], const float centre[3], const float axis[3], float deg, float hh[3],
                      float hand[3], float rot[9])
{
    float R[9];
    RotAxis(axis, deg, R);
    for (int which = 0; which < 2; ++which) {
        const float* rel = hold + 3 * which;
        float w[3], d[3];
        for (int r = 0; r < 3; ++r) { w[r] = B[4 * r] * rel[0] + B[4 * r + 1] * rel[1] + B[4 * r + 2] * rel[2] + B[4 * r + 3]; }
        for (int r = 0; r < 3; ++r) { d[r] = w[r] - centre[r]; }
        float* out = which == 0 ? hh : hand;
        for (int r = 0; r < 3; ++r) { out[r] = centre[r] + R[3 * r] * d[0] + R[3 * r + 1] * d[1] + R[3 * r + 2] * d[2]; }
    }
    float bw[9];   // B's rotation x the hold's rotation, then R x that
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) { bw[3 * r + c] = B[4 * r] * hold[6 + c] + B[4 * r + 1] * hold[9 + c] + B[4 * r + 2] * hold[12 + c]; }
    }
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) { rot[3 * r + c] = R[3 * r] * bw[c] + R[3 * r + 1] * bw[3 + c] + R[3 * r + 2] * bw[6 + c]; }
    }
}

// RUN BK5 (Jay: "use that as the point of where the wheel is for alignment, not to actually force those specific spots"): the rim as
// the clip's grip point sees it - its depth along the axis (axial) and its radius, from the hold's grip point on the live body.
inline void HoldAlign(const float B[12], const float hold[15], const float centre[3], const float axis[3], float* axial, float* radial)
{
    const float l = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    float d[3];
    for (int r = 0; r < 3; ++r) { d[r] = B[4 * r] * hold[0] + B[4 * r + 1] * hold[1] + B[4 * r + 2] * hold[2] + B[4 * r + 3] - centre[r]; }
    const float a = l > 1e-9f ? (d[0] * axis[0] + d[1] * axis[1] + d[2] * axis[2]) / l : 0.0f;
    const float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - a * a;
    *axial = a;
    *radial = dd > 0.0f ? std::sqrt(dd) : 0.0f;
}
// A wheel grip's point in the wheel's body frame (columns axis, ref, axis x ref) -> ON that rim (depth axialM, radius rimR) at the angle
// the hand grabbed (on the axis - no angle - the reference direction).
inline void RimGrabLocalAt(const float local[3], float rimR, float axialM, float out[3])
{
    const float r = std::sqrt(local[1] * local[1] + local[2] * local[2]);
    out[0] = axialM;
    if (!(r > 1e-6f)) { out[1] = rimR; out[2] = 0.0f; return; }
    out[1] = local[1] * rimR / r;
    out[2] = local[2] * rimR / r;
}

struct HandIn {
    bool  tracked = false;
    float squeeze = 0.0f;    // 0..1
    float pos[3] = {};       // world (the grip point - the palm)
    float pinch[3] = {};     // world: the key-pinch point (thumb / index tips, kKeyPinchFromGrip ahead of the grip); the key's grab test
    float rollDeg = 0.0f;    // the grip's accumulated twist about its own Z (twist_logic.h convention: clockwise = negative)
};

struct Frame {
    bool  enabled = false;       // buggy_hand_steer
    bool  localDriver = false;   // this is the local player's buggy and the player drives it (buggywheel.cpp's gate)
    bool  engineOn = false;      // engine module +648
    bool  canStart = false;      // the game would allow a start (+105C730: a driver and fuel)
    float timer = -1.0f;         // engine module +652 (< 0 = idle)
    float wheelCentre[3] = {};   // the steeringwheel element's world origin
    float wheelAxis[3] = {};     // the column (the element's rotation axis, world)
    float wheelRef[3] = {};      // a fixed in-plane reference direction (world; angle 0)
    float rimRadiusM = 0.0f;
    float maxWheelDeg = 0.0f;    // the game's full lock at this speed (params +4464 x +10848D0, INV-716 add. 5)
    float gameWheelDeg = 0.0f;   // the angle the game shows now (what a first grab starts from)
    bool  haveKeyPoint = false;
    float keyPos[3] = {};        // world
    HandIn hand[2];              // kLeft, kRight
};

enum Event : unsigned { kNone = 0, kGrabWheel = 1, kGrabKey = 2, kTick = 4, kStart = 8 };

struct Out {
    bool     holdInputs = false;   // raw throttle / brake -> 0
    bool     writeTurn = false;
    float    rawTurn = 0.0f;
    bool     startTimer = false;   // start the game's engine start timer (once)
    bool     wheelGripped = false;
    float    wheelDeg = 0.0f;      // valid while wheelGripped
    bool     pinned[2] = {};       // the hand you see is locked (wheel or key)
    unsigned events[2] = {};       // Event bits, per hand
};

class State {
public:
    void Reset()
    {
        primed_ = false;
        for (int i = 0; i < 2; ++i) { down_[i] = false; mode_[i] = Mode::Free; acc_[i] = 0.0f; last_[i] = 0.0f; }
        base_ = 0.0f;
        wheel_ = 0.0f;
        keyStart_ = 0.0f;
        keyTicks_ = 0;
    }

    Out Step(const Frame& f)
    {
        Out o{};
        if (!f.enabled || !f.localDriver) {
            Reset();
            return o;
        }
        const bool keyNeeded = !f.engineOn && f.timer < 0.0f && f.haveKeyPoint;
        o.holdInputs = keyNeeded;

        bool down[2];
        for (int i = 0; i < 2; ++i) { down[i] = f.hand[i].tracked && f.hand[i].squeeze >= kGripOn; }

        // 1. Hands already holding: tracking loss / release / turn.
        bool wheelSetChanged = false;
        for (int i = 0; i < 2; ++i) {
            const HandIn& h = f.hand[i];
            if (mode_[i] == Mode::Free) { continue; }
            if (!h.tracked || h.squeeze < kGripOff || (mode_[i] == Mode::Key && !keyNeeded)) {
                if (mode_[i] == Mode::Wheel) { wheelSetChanged = true; }
                mode_[i] = Mode::Free;
                continue;
            }
            if (mode_[i] == Mode::Wheel) {
                const float a = HandAngle(f, h.pos);
                acc_[i] += Wrap180(a - last_[i]);
                last_[i] = a;
            } else {   // Key
                const float turned = keyStart_ - h.rollDeg;
                const int idx = turned > 0.0f ? static_cast<int>(std::floor(turned / kTickDeg)) : 0;
                if (idx > keyTicks_) { o.events[i] |= kTick; }
                keyTicks_ = idx;
                if (turned >= kQuarterDeg) {
                    if (f.canStart) {
                        o.startTimer = true;
                        o.events[i] |= kStart;
                    }
                    mode_[i] = Mode::Free;   // the grip ends; down_ stays set, so no re-grab without a new press
                }
            }
        }
        // the wheel angle with the hands that held through this step
        int n = 0;
        float sum = 0.0f;
        for (int i = 0; i < 2; ++i) { if (mode_[i] == Mode::Wheel) { sum += acc_[i]; ++n; } }
        if (n > 0) { wheel_ = Clamp(base_ + sum / static_cast<float>(n), -f.maxWheelDeg, f.maxWheelDeg); }

        // 2. New presses.
        const bool anyWheelBefore = n > 0;
        for (int i = 0; i < 2; ++i) {
            const bool edge = primed_ && down[i] && !down_[i];
            if (!edge || mode_[i] != Mode::Free) { continue; }
            const HandIn& h = f.hand[i];
            if (i == kRight && keyNeeded && Dist(h.pinch, f.keyPos) <= kKeyGrabRadiusM) {   // the fingertips; before the rim
                mode_[i] = Mode::Key;
                keyStart_ = h.rollDeg;
                keyTicks_ = 0;
                o.events[i] |= kGrabKey;
            } else if (OnRim(f, h.pos)) {
                if (!anyWheelBefore && !wheelSetChanged) {
                    wheel_ = Clamp(f.gameWheelDeg, -f.maxWheelDeg, f.maxWheelDeg);
                }
                mode_[i] = Mode::Wheel;
                last_[i] = HandAngle(f, h.pos);
                wheelSetChanged = true;
                o.events[i] |= kGrabWheel;
            }
        }
        // 3. Whoever grips now: re-base, so a change of hands never moves the wheel.
        if (wheelSetChanged) {
            base_ = wheel_;
            for (int i = 0; i < 2; ++i) { acc_[i] = 0.0f; }
        }
        for (int i = 0; i < 2; ++i) {
            down_[i] = down[i];
            o.pinned[i] = mode_[i] != Mode::Free;
            if (mode_[i] == Mode::Wheel) { o.wheelGripped = true; }
        }
        // tracking loss: a squeeze still held when tracking returns is not a press
        for (int i = 0; i < 2; ++i) { if (!f.hand[i].tracked) { down_[i] = true; } }
        primed_ = true;
        if (o.wheelGripped) {
            o.wheelDeg = wheel_;
            o.writeTurn = true;
            o.rawTurn = RawSteerFromWheel(wheel_, f.maxWheelDeg);
        }
        return o;
    }

private:
    enum class Mode { Free, Wheel, Key };

    static float Dist(const float a[3], const float b[3])
    {
        const float d[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
        return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    }
    static float HandAngle(const Frame& f, const float p[3])
    {
        const float v[3] = { p[0] - f.wheelCentre[0], p[1] - f.wheelCentre[1], p[2] - f.wheelCentre[2] };
        return SignedAngleAbout(f.wheelAxis, f.wheelRef, v);
    }
    static bool OnRim(const Frame& f, const float p[3])
    {
        const float al = std::sqrt(f.wheelAxis[0] * f.wheelAxis[0] + f.wheelAxis[1] * f.wheelAxis[1] + f.wheelAxis[2] * f.wheelAxis[2]);
        if (!(al > 1e-6f) || !(f.rimRadiusM > 0.0f)) { return false; }
        const float v[3] = { p[0] - f.wheelCentre[0], p[1] - f.wheelCentre[1], p[2] - f.wheelCentre[2] };
        const float ax = (v[0] * f.wheelAxis[0] + v[1] * f.wheelAxis[1] + v[2] * f.wheelAxis[2]) / al;
        float w[3];
        for (int i = 0; i < 3; ++i) { w[i] = v[i] - ax * f.wheelAxis[i] / al; }
        const float r = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        return std::fabs(ax) <= kGrabRadiusM && std::fabs(r - f.rimRadiusM) <= kGrabRadiusM;
    }

    bool  primed_ = false;
    bool  down_[2] = {};
    Mode  mode_[2] = { Mode::Free, Mode::Free };
    float acc_[2] = {};
    float last_[2] = {};
    float base_ = 0.0f;
    float wheel_ = 0.0f;
    float keyStart_ = 0.0f;
    int   keyTicks_ = 0;
};

}  // namespace buggywheel
}  // namespace dlvr
