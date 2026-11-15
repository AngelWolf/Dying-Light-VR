// Pure logic for centring appSpace on the head - docs/superpowers/specs/2026-10-26-roomscale-physical-crouch-design.md §2.1 (D-284).
// No OpenXR, no Windows, no logging, so src/src/tests/centre_logic_test.cpp compiles it on its own.
// ovr2xr.cpp owns everything with side effects: PollEvents feeds OnState, BeginFrameIfNeeded locates the head and recreates appSpace.
//
// WHY (INV-635 + addendum): the view sits off the body by the head's distance from the runtime LOCAL origin, which VDXR never
// places at the head at session start and which a headset off / on moves (a tester: median 1.397 m). At the first FOCUSED of a session
// and at every FOCUSED after a drop out, appSpace is moved SIDEWAYS ONLY (x/z) so the head sits over its origin. Height (D-159) and
// yaw (D-253's absorb) are untouched.
#pragma once

#include "headfollow_logic.h"

namespace dlvr {
namespace centre {

enum class Reason { None, SessionStart, Refocus };

const int kFocused = 5;   // XR_SESSION_STATE_FOCUSED

struct Tracker {
    int       lastState   = 0;
    bool      everCentred = false;
    Reason    pending     = Reason::None;
    long long armedAt     = 0;   // XrTime (ns) of the FOCUSED that armed `pending`
};

// One XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED. FOCUSED arms a SessionStart until the first centre has been done, else a Refocus
// when the previous state was below FOCUSED (a drop out: 5 -> 4/3 -> 5). A re-arm restarts the settle wait. Other states leave
// `pending` as it is (a drop out before the centre landed keeps it; the next FOCUSED re-arms it).
inline void OnState(Tracker& t, int xrState, long long nowNs)
{
    if (xrState == kFocused) {
        if (!t.everCentred) {
            t.pending = Reason::SessionStart;
            t.armedAt = nowNs;
        } else if (t.lastState < kFocused) {
            t.pending = Reason::Refocus;
            t.armedAt = nowNs;
        }
    }
    t.lastState = xrState;
}

// The head is given time to settle after FOCUSED (VD reports focus while the headset is still being pulled on), and the centre
// gives up after kGiveUpNs without a tracked head.
const long long kSettleNs = 1000000000LL;
const long long kGiveUpNs = 5000000000LL;

// Due = a centre is armed, D-253's absorb is not waiting (never two appSpace recreates fighting in one frame), the head is tracked
// and the settle time has passed.
inline bool Due(const Tracker& t, long long nowNs, bool absorbWaiting, bool headTracked)
{
    return t.pending != Reason::None && !absorbWaiting && headTracked && nowNs >= t.armedAt + kSettleNs;
}

inline bool GiveUp(const Tracker& t, long long nowNs) { return t.pending != Reason::None && nowNs > t.armedAt + kGiveUpNs; }

inline void Done(Tracker& t)
{
    t.everCentred = true;
    t.pending     = Reason::None;
}

inline const char* ReasonName(Reason r)
{
    return r == Reason::SessionStart ? "session start" : r == Reason::Refocus ? "headset back on (FOCUSED after a drop out)" : "none";
}

// The new appSpace origin in the reference space: the old one plus the head's x/z (located IN appSpace) turned by the base yaw -
// the same composition as ovr_RecenterTrackingOrigin (ovr2xr.cpp:9022-9026). Height is kept: out[1] = basePos[1].
inline void ComposeBasePos(float baseYawDeg, const float basePos[3], float headX, float headZ, float out[3])
{
    float x = headX, z = headZ;
    ::dlvr::headfollow::ReportedHeadPosForFold(baseYawDeg, x, z);   // rotate about +Y by the base
    out[0] = basePos[0] + x;
    out[1] = basePos[1];
    out[2] = basePos[2] + z;
}

}  // namespace centre
}  // namespace dlvr
