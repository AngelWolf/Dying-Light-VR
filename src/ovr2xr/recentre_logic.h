// Pure logic for absorbing a runtime recentre's yaw - docs/superpowers/specs/2026-10-20-recentre-absorb-yaw-design.md (D-253).
// No OpenXR, no Windows, no logging, so src/src/tests/recentre_logic_test.cpp compiles it on its own.
// ovr2xr.cpp owns everything with side effects: PollEvents queues, BeginFrameIfNeeded recreates appSpace.
//
// Convention: appSpace = the reference space (LOCAL / STAGE) turned by `base` degrees about +Y. A pose located in appSpace has
// yaw = (its yaw in the reference space) - base. A runtime recentre that turns the reference space by +r reports
// poseInPreviousSpace with yaw +r.
#pragma once

#include <cmath>

namespace dlvr {
namespace recentre {

// Into (-180, 180].
inline float Wrap180(float d)
{
    d = std::fmod(d, 360.0f);
    if (d > 180.0f)   { d -= 360.0f; }
    if (d <= -180.0f) { d += 360.0f; }
    return d;
}

// Yaw of the forward axis (-Z) projected on XZ, degrees, a turn about +Y (left) positive. Pitch and roll do not move it.
inline float YawDegAboutY(const float q[4])
{
    const float fx = -2.0f * (q[0] * q[2] + q[1] * q[3]);
    const float fz = -(1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]));
    return std::atan2(-fx, -fz) * 57.2957795f;
}

// (x, y, z, w) of a pure turn about +Y.
inline void QuatFromYawDeg(float deg, float q[4])
{
    const float h = deg * 0.5f * 0.017453292f;
    q[0] = 0.0f;
    q[1] = std::sin(h);
    q[2] = 0.0f;
    q[3] = std::cos(h);
}

// A queued absorb: the recentre events seen since the last frame began.
struct Pending {
    bool      has        = false;
    long long changeTime = 0;
    bool      poseValid  = true;
    float     poseYawDeg = 0.0f;
    int       framesWatched = 0;   // v2: frames the fallback has watched for the jump (logged)
};

// One REFERENCE_SPACE_CHANGE_PENDING. typeMatches = the event's space type is the one appSpace was built on; anything else
// is ignored. A second event before the absorb runs merges into the first: latest time, yaws summed, exact only if both were.
inline void Queue(Pending& p, bool typeMatches, long long changeTime, bool poseValid, float poseYawDeg)
{
    if (!typeMatches) { return; }
    if (!p.has) {
        p.has        = true;
        p.changeTime = changeTime;
        p.poseValid  = poseValid;
        p.poseYawDeg = poseValid ? Wrap180(poseYawDeg) : 0.0f;
        return;
    }
    if (changeTime > p.changeTime) { p.changeTime = changeTime; }
    p.poseValid  = p.poseValid && poseValid;
    p.poseYawDeg = p.poseValid ? Wrap180(p.poseYawDeg + poseYawDeg) : 0.0f;
}

inline bool Due(const Pending& p, long long displayTime) { return p.has && displayTime >= p.changeTime; }

// v2 (run 1, 2026-10-20): VDXR queues the event BEFORE Virtual Desktop's OVR side moves the space (VDXR frame.cpp:66-83 only
// forwards ShouldRecenter); the move lands in one later frame. So without an exact pose the absorb WATCHES: each frame of the
// window it compares the head now with the previous frame's, and absorbs the first step no head can make in one frame.
// A real head turn tops out near 500 deg/s = ~5.6 deg per frame at 90 Hz.
const float     kJumpYawDeg = 10.0f;
const float     kJumpPosM   = 0.1f;
const long long kWatchNs    = 250000000LL;    // v3: 250 ms past changeTime (XrTime is nanoseconds)
const long long kLookbackNs = 250000000LL;    // v3: 250 ms before changeTime

inline bool IsRecentreJump(float yawStepDeg, float posStepM)
{
    return std::fabs(yawStepDeg) > kJumpYawDeg || posStepM > kJumpPosM;
}

inline bool WatchExpired(const Pending& p, long long displayTime) { return p.has && displayTime > p.changeTime + kWatchNs; }

// v3 (run 2, 2026-10-20): the move can land a frame BEFORE the event - VDXR reads the OVR side's flag only inside xrWaitFrame
// (frame.cpp:66-83), so a re-origin between our wait and our locate is in that frame's head while the event waits a frame. Each
// frame's head (yaw + position in appSpace) goes in a small ring; at the event the absorb takes the impossible one-frame step from
// [changeTime - kLookbackNs, now], watching on up to kWatchNs if it has not landed yet.
const int kHeadRingSize = 64;   // > 250 ms at 144 Hz (36 frames)

struct HeadSample {
    long long t;
    float     yawDeg, x, y, z;
};

struct HeadRing {
    HeadSample s[kHeadRingSize];
    int        start = 0;
    int        n     = 0;
};

inline const HeadSample& HeadAt(const HeadRing& r, int i) { return r.s[(r.start + i) % kHeadRingSize]; }

// One sample per display time: a second locate of the same frame replaces the first.
inline void HeadPush(HeadRing& r, long long t, float yawDeg, float x, float y, float z)
{
    const HeadSample v{ t, yawDeg, x, y, z };
    if (r.n > 0 && r.s[(r.start + r.n - 1) % kHeadRingSize].t == t) {
        r.s[(r.start + r.n - 1) % kHeadRingSize] = v;
        return;
    }
    if (r.n < kHeadRingSize) {
        r.s[(r.start + r.n) % kHeadRingSize] = v;
        ++r.n;
        return;
    }
    r.s[r.start] = v;
    r.start = (r.start + 1) % kHeadRingSize;
}

inline void HeadClear(HeadRing& r) { r.start = 0; r.n = 0; }

struct Step {
    float     yawStepDeg;   // after - before, wrapped
    float     posStepM;
    long long t;            // display time of the sample AFTER the step
};

// The recentre step among consecutive samples whose later one is at or after fromT: the largest yaw step over kJumpYawDeg, else
// the largest position step over kJumpPosM. False = none.
inline bool FindRecentreStep(const HeadRing& r, long long fromT, Step& out)
{
    bool  found = false;
    float best  = 0.0f;
    for (int i = 1; i < r.n; ++i) {
        const HeadSample& a = HeadAt(r, i - 1);
        const HeadSample& b = HeadAt(r, i);
        if (b.t < fromT) { continue; }
        const float yaw = Wrap180(b.yawDeg - a.yawDeg);
        const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
        const float pos = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!IsRecentreJump(yaw, pos)) { continue; }
        const float score = std::fabs(yaw) > kJumpYawDeg ? 1000.0f + std::fabs(yaw) : pos;
        if (!found || score > best) {
            found = true;
            best  = score;
            out   = Step{ yaw, pos, b.t };
        }
    }
    return found;
}

// The base the recreated appSpace needs so the head keeps its appSpace yaw across the recentre.
// Exact: base - poseYaw. Fallback (no pose from the runtime): base + the head's jump in appSpace (now - previous frame).
inline float NewBaseYaw(float baseYawDeg, bool poseValid, float poseYawDeg, float headNowDeg, float headPrevDeg)
{
    if (poseValid) { return Wrap180(baseYawDeg - poseYawDeg); }
    return Wrap180(baseYawDeg + Wrap180(headNowDeg - headPrevDeg));
}

// The game's own recentre (ovr_RecenterTrackingOrigin): face the head, whose yaw was located IN appSpace, so in the reference
// space it is base + that yaw.
inline float ComposeBaseYaw(float baseYawDeg, float headInAppYawDeg) { return Wrap180(baseYawDeg + headInAppYawDeg); }

}  // namespace recentre
}  // namespace dlvr
