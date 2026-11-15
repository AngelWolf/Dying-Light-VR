// bodytrack_logic.h - THE BODY-TRACKED ELBOW (rig_elbow_body_tracking), pure maths: no globals, no LOG, no allocation.
// 2026-11-08, Jay: "I'd like to add it as an additional option for headsets that DO support it. Mainly just for the elbows, that way
// we can have the arms as natural as possible in terms of position" -> design agreed in chat ("Yes"): the runtime's body-tracked
// elbow (XR_FB_body_tracking, VDXR + Virtual Desktop's "Forward tracking data") sets WHICH WAY the game elbow points; the hands stay
// pinned to the controllers and the arm lengths stay the game's. The face limit stays, the swing limit is off while tracked, and a
// glitch guard drops a sample whose elbow moved further than a real arm can. Unit test: src/src/tests/bodytrack_logic_test.cpp.
//
// Conventions as boneblend.cpp / elbowreal_logic.h: vectors float[3]; the app -> world map is Ry(yaw) row-major
// { c, 0, s,  0, 1, 0,  -s, 0, c } (boneblend.cpp's `ry` built from cby / sby); a swivel of +a turns t -> t cos a + (dir x t) sin a
// (elbowreal::SolveSwivel's convention).
//
// [UNKNOWN] which point of the arm each XR_FB_body_tracking joint's POSITION is (the spec's names are ARM_UPPER / ARM_LOWER /
// HAND_WRIST; read here as shoulder / elbow / wrist). Not established - the [BODYTRACK] lines print both segment lengths so the first
// headset run decides it (a real upper arm and forearm are each roughly 0.2 - 0.35 m).
#pragma once

#include <cmath>

namespace dlvr {
namespace bodytrack {

// The glitch guard: an elbow step larger than this speed times the time since the elbow last moved, plus the slack, is not a real
// arm. 8 m/s is above a fast melee swing's elbow; the slack absorbs tracking noise on a short dt.
constexpr float kMaxElbowSpeedMps = 8.0f;
constexpr float kGlitchSlackM = 0.03f;
// A nearly straight real arm has no usable elbow direction: the tracked elbow's offset from the shoulder - wrist line fades the
// tracked pole in from kPerpLoM to kPerpHiM.
constexpr float kPerpLoM = 0.02f;
constexpr float kPerpHiM = 0.05f;
// The tracked direction, carried into the game arm, must keep at least this much of itself off the game arm's own line.
constexpr float kMinOffArm = 0.15f;
// The fade between the tracked elbow and the normal one (seconds, each way) - elbowreal::FadeStep's.
constexpr float kFadeS = 0.3f;
// The face limit's search: 5-deg steps either side of the tracked swivel.
constexpr int kFaceSteps = 36;

inline float Dot3(const float a[3], const float b[3]) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
inline float Len3(const float a[3]) { return std::sqrt(Dot3(a, a)); }
inline bool Finite3(const float a[3]) { return std::isfinite(a[0]) && std::isfinite(a[1]) && std::isfinite(a[2]); }

// The tracked elbow's offset from the shoulder -> wrist line (app space). outUnit = its direction (unit), outPerpM = its length,
// outUpperM / outLowerM = |E - S| / |W - E|. False (outputs untouched except the lengths) when an input is not finite, the
// shoulder - wrist line is under 5 cm, or the offset is under 1 mm.
inline bool ElbowOffset(const float S[3], const float E[3], const float W[3], float outUnit[3], float* outPerpM, float* outUpperM,
                        float* outLowerM)
{
    if (!Finite3(S) || !Finite3(E) || !Finite3(W)) { return false; }
    const float se[3] = { E[0] - S[0], E[1] - S[1], E[2] - S[2] };
    const float ew[3] = { W[0] - E[0], W[1] - E[1], W[2] - E[2] };
    float sw[3] = { W[0] - S[0], W[1] - S[1], W[2] - S[2] };
    if (outUpperM) { *outUpperM = Len3(se); }
    if (outLowerM) { *outLowerM = Len3(ew); }
    const float swl = Len3(sw);
    if (!(swl > 0.05f)) { return false; }
    sw[0] /= swl; sw[1] /= swl; sw[2] /= swl;
    const float k = Dot3(se, sw);
    const float p[3] = { se[0] - sw[0]*k, se[1] - sw[1]*k, se[2] - sw[2]*k };
    const float pl = Len3(p);
    if (!(pl > 1e-3f)) { return false; }
    outUnit[0] = p[0]/pl; outUnit[1] = p[1]/pl; outUnit[2] = p[2]/pl;
    if (outPerpM) { *outPerpM = pl; }
    return true;
}

// True = the elbow moved further than a real arm can since the elbow's last distinct position (dtS seconds ago). No reference
// (dtS not in (0, 0.25]) = never a glitch.
inline bool IsGlitch(const float prevE[3], const float E[3], float dtS)
{
    if (!(dtS > 0.0f) || dtS > 0.25f) { return false; }
    const float d[3] = { E[0] - prevE[0], E[1] - prevE[1], E[2] - prevE[2] };
    const float step = Len3(d);
    if (!std::isfinite(step)) { return true; }
    return step > kMaxElbowSpeedMps * dtS + kGlitchSlackM;
}

// 0 at kPerpLoM, 1 at kPerpHiM, cubic between.
inline float PerpWeight(float perpM)
{
    if (!std::isfinite(perpM)) { return 0.0f; }
    float u = (perpM - kPerpLoM) / (kPerpHiM - kPerpLoM);
    if (u < 0.0f) { u = 0.0f; }
    if (u > 1.0f) { u = 1.0f; }
    return u * u * (3.0f - 2.0f * u);
}

// App space -> world: Ry(yaw), c = cos(yaw), s = sin(yaw) - the same map boneblend.cpp applies to the controller (cWorld = ry . cq).
inline void AppToWorld(const float a[3], float c, float s, float out[3])
{
    const float x = c*a[0] + s*a[2], y = a[1], z = -s*a[0] + c*a[2];
    out[0] = x; out[1] = y; out[2] = z;
}

// The game arm's pole from the tracked direction (world): its part perpendicular to the game's shoulder -> wrist direction `dir`
// (unit), normalised. False when less than kMinOffArm of it is off the game arm's line (the two arms disagree too far to say).
inline bool PoleFromTracked(const float worldDir[3], const float dir[3], float outPole[3])
{
    if (!Finite3(worldDir) || !Finite3(dir)) { return false; }
    const float k = Dot3(worldDir, dir);
    const float p[3] = { worldDir[0] - dir[0]*k, worldDir[1] - dir[1]*k, worldDir[2] - dir[2]*k };
    const float pl = Len3(p);
    if (!(pl >= kMinOffArm)) { return false; }
    outPole[0] = p[0]/pl; outPole[1] = p[1]/pl; outPole[2] = p[2]/pl;
    return true;
}

// The game elbow for a pole: S + (dir cos A + pole sin A) L1 - elbowreal::ForearmBasisCS's E (pole perpendicular to dir, unit).
inline void ElbowAt(const float S[3], const float dir[3], const float pole[3], float cA, float sA, float L1, float outE[3])
{
    for (int k = 0; k < 3; ++k) { outE[k] = S[k] + (dir[k]*cA + pole[k]*sA) * L1; }
}

struct FaceResult {
    float pole[3];
    float offsetDeg;   // the swivel added to the tracked pole (0 = unchanged)
    float distM;       // the chosen elbow's distance from the camera (-1 = the limit was off)
    bool  moved;       // the tracked elbow was nearer the camera than faceMinM, so the limit turned it
    bool  fallback;    // no swivel reached faceMinM: the farthest one was taken
};

// THE FACE LIMIT on the tracked pole: unchanged when the game elbow it gives is at least faceMinM from the camera (or the limit is
// off: faceMinM <= 0 / no camera). Otherwise the smallest swivel turn (5-deg steps, +a before -a) that reaches faceMinM; none = the
// farthest. pole perpendicular to dir, both unit; A = the shoulder angle, L1 = the upper arm.
inline FaceResult FaceLimit(const float S[3], const float dir[3], const float pole[3], float A, float L1, const float cam[3],
                            bool haveCam, float faceMinM)
{
    FaceResult r{};
    r.pole[0] = pole[0]; r.pole[1] = pole[1]; r.pole[2] = pole[2];
    r.offsetDeg = 0.0f; r.distM = -1.0f; r.moved = false; r.fallback = false;
    if (!haveCam || !(faceMinM > 0.0f) || !Finite3(cam) || !std::isfinite(A) || !std::isfinite(L1)) { return r; }
    const float cA = std::cos(A), sA = std::sin(A);
    auto distFor = [&](const float p[3]) {
        float E[3];
        ElbowAt(S, dir, p, cA, sA, L1, E);
        const float d[3] = { E[0] - cam[0], E[1] - cam[1], E[2] - cam[2] };
        return Len3(d);
    };
    r.distM = distFor(pole);
    if (!(r.distM < faceMinM)) { return r; }
    r.moved = true;
    const float q[3] = { dir[1]*pole[2] - dir[2]*pole[1], dir[2]*pole[0] - dir[0]*pole[2], dir[0]*pole[1] - dir[1]*pole[0] };
    float bestD = r.distM, bestA = 0.0f;
    for (int i = 1; i <= kFaceSteps; ++i) {
        for (int sgn = 1; sgn >= -1; sgn -= 2) {
            if (i == kFaceSteps && sgn < 0) { continue; }   // +180 and -180 are the same swivel
            const float a = static_cast<float>(sgn * i) * 5.0f;
            const float ar = a * 0.01745329252f, c = std::cos(ar), s = std::sin(ar);
            const float p[3] = { pole[0]*c + q[0]*s, pole[1]*c + q[1]*s, pole[2]*c + q[2]*s };
            const float d = distFor(p);
            if (d >= faceMinM) {
                r.pole[0] = p[0]; r.pole[1] = p[1]; r.pole[2] = p[2];
                r.offsetDeg = a; r.distM = d;
                return r;
            }
            if (d > bestD) { bestD = d; bestA = a; }
        }
    }
    r.fallback = true;
    const float ar = bestA * 0.01745329252f, c = std::cos(ar), s = std::sin(ar);
    r.pole[0] = pole[0]*c + q[0]*s; r.pole[1] = pole[1]*c + q[1]*s; r.pole[2] = pole[2]*c + q[2]*s;
    r.offsetDeg = bestA; r.distM = bestD;
    return r;
}

// Angle between two unit vectors, degrees (clamped dot).
inline float AngleDeg(const float a[3], const float b[3])
{
    float d = Dot3(a, b);
    if (d > 1.0f) { d = 1.0f; }
    if (d < -1.0f) { d = -1.0f; }
    return std::acos(d) * 57.29578f;
}

}  // namespace bodytrack
}  // namespace dlvr
