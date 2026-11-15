// twist_logic.h - THE GATE FLICK FROM THE RIGHT CONTROLLER'S TWIST (unit-tested: src/src/tests/twist_logic_test.cpp).
// RUN RG5 (Jay: "The gate keeps not working" / "The flick for the gate is a wrist rotation, very quick" -> "Controller twist"): three runs
// measured the GUN's roll from bone_root at the hook, whose world updates on every second call (INV-165 / INV-168) - a quick twist read as
// one huge spike between zeros (+10077 deg/s in RG4 / RG5) and missed the two-updates-in-a-row rule. The source is now the controller's
// own grip pose (controllers.h GetControllerGripPose, the RAW one - speed is never measured from the steady copies, controllers.h:80),
// located once a frame; a sample is kept only when the pose CHANGED (a repeat on the second hook call is not a new sample).
// TWIST = the turn about the grip's own Z (OpenXR grip: -Z runs forward through the closed fist - the forearm / wrist-rotation axis),
// split from any swing (swing-twist). RIGHT = the top of the hand rolls to the right seen from behind = a NEGATIVE turn about +Z.
// Peak(window): the largest twist right / left between ANY two samples in the window, earlier -> later (a twist out and back still
// counts). No game access, no logging, no allocation.
#pragma once

#include <cmath>

namespace dlvr {
namespace reload {

struct TwistPeak { float rightDeg = 0.0f, leftDeg = 0.0f; };

// RUN RG6: toggles read ~175 deg BOTH ways - when the handle axis itself swings far the twist about it is meaningless (swing-twist
// breaks down). A pair of samples whose grip Z axes differ by more than this is skipped (a wrist rotation barely moves that axis).
// Jay: "You can fix the gate now aswell" (proposed: drop big-swing readings + close at 45).
constexpr float kTwistMaxSwingDeg = 40.0f;

class TwistWindow {
public:
    static constexpr int    kCap = 64;
    static constexpr double kKeepS = 0.5;

    // q = (x y z w); t in seconds. A pose equal to the last one is not a new sample.
    void Push(const float q[4], double t)
    {
        if (n_ > 0) {
            const Sample& l = s_[(head_ + n_ - 1) % kCap];
            if (l.q[0] == q[0] && l.q[1] == q[1] && l.q[2] == q[2] && l.q[3] == q[3]) { return; }
        }
        while (n_ > 0 && t - s_[head_].t > kKeepS) { head_ = (head_ + 1) % kCap; --n_; }
        if (n_ == kCap) { head_ = (head_ + 1) % kCap; --n_; }
        Sample& d = s_[(head_ + n_) % kCap];
        for (int i = 0; i < 4; ++i) { d.q[i] = q[i]; }
        d.t = t;
        ++n_;
    }
    void Clear() { n_ = 0; head_ = 0; }
    int Count() const { return n_; }

    TwistPeak Peak(double windowS) const
    {
        TwistPeak p;
        if (n_ < 2) { return p; }
        const double tEnd = s_[(head_ + n_ - 1) % kCap].t;
        for (int a = 0; a < n_; ++a) {
            const Sample& A = s_[(head_ + a) % kCap];
            if (tEnd - A.t > windowS) { continue; }
            for (int b = a + 1; b < n_; ++b) {
                const float* qb = s_[(head_ + b) % kCap].q;
                if (SwingDeg(A.q, qb) > kTwistMaxSwingDeg) { continue; }   // RG6: a big swing is not a twist
                const float tw = TwistDeg(A.q, qb);   // + = about +Z = LEFT
                if (-tw > p.rightDeg) { p.rightDeg = -tw; }
                if (tw > p.leftDeg) { p.leftDeg = tw; }
            }
        }
        return p;
    }

    // the angle (deg) between the two orientations' grip Z axes - the swing
    static float SwingDeg(const float q0[4], const float q1[4])
    {
        auto zAxis = [](const float* q, float z[3]) {
            z[0] = 2.0f * (q[0] * q[2] + q[3] * q[1]);
            z[1] = 2.0f * (q[1] * q[2] - q[3] * q[0]);
            z[2] = 1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]);
        };
        float a[3], b[3];
        zAxis(q0, a);
        zAxis(q1, b);
        const float la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
        if (!(la > 1e-6f) || !(lb > 1e-6f)) { return 180.0f; }
        float c = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb);
        c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
        return std::acos(c) * 57.2957795f;
    }

    // the twist about the LOCAL z from q0 to q1, degrees in (-180, 180]: r = conj(q0) x q1, twist = 2 atan2(r.z, r.w)
    static float TwistDeg(const float q0[4], const float q1[4])
    {
        const float ax = -q0[0], ay = -q0[1], az = -q0[2], aw = q0[3];
        const float rz = aw * q1[2] + ax * q1[1] - ay * q1[0] + az * q1[3];
        const float rw = aw * q1[3] - ax * q1[0] - ay * q1[1] - az * q1[2];
        float deg = 2.0f * std::atan2(rz, rw) * 57.2957795f;
        if (deg > 180.0f) { deg -= 360.0f; }
        if (deg <= -180.0f) { deg += 360.0f; }
        return std::isfinite(deg) ? deg : 0.0f;
    }

private:
    struct Sample { float q[4]; double t; };
    Sample s_[kCap] = {};
    int    head_ = 0, n_ = 0;
};

}  // namespace reload
}  // namespace dlvr
