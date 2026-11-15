// cinyaw_logic - the pure decisions of the cutscene camera fix (2026-10-25, docs/superpowers/specs/2026-10-25-cutscene-camera-design.md;
// INV-620). Header-only, no Windows includes, so tests/cinyaw_logic_test.cpp compiles it standalone. Degrees, in +4460 units (D2's h
// after the flip, boneblend.cpp camyaw::Hook).
#pragma once

#include <cmath>
#include <cstring>

namespace dlvr {
namespace cinyaw {

inline float Wrap180(float d)
{
    while (d > 180.0f) { d -= 360.0f; }
    while (d < -180.0f) { d += 360.0f; }
    return d;
}

// (a) CinematicController's look slot 0 returned `returnedYaw`. It is the head-inclusive +4460 (the runaway's input) only when it equals
// `cam4460` bit for bit; the movie's own look deviation is left alone. Inside D2's own chase (`inChase`) the addend is already out of +4460.
inline float RemoveAddend(float returnedYaw, float cam4460, float added, bool addedIsThisObj, bool inChase)
{
    if (!addedIsThisObj || inChase || std::memcmp(&returnedYaw, &cam4460, sizeof(float)) != 0) {
        return returnedYaw;
    }
    return returnedYaw - added;
}

// ADDENDUM 2: split q = A(yaw) B(pitch) C(roll) (Hamilton, intrinsic a then b then c; components x, y, z, w as camhook; axes 0 x 1 y 2 z,
// distinct) into its angles (radians). Through the rotation matrix: e = +1 for a cyclic (a, b, c), -1 otherwise; M[a][c] = e sin p. At the
// gimbal (|sin p| ~ 1) roll is folded into yaw (callers drop roll). Exact near +-90 deg pitch where the Euler-yaw formula alone is not.
// False (nothing written) when the axes are not distinct.
inline bool SplitYawPitchRoll(const float q[4], int a, int b, int c, float* yaw, float* pitch, float* roll)
{
    if (a == b || b == c || a == c || a < 0 || b < 0 || c < 0 || a > 2 || b > 2 || c > 2) { return false; }
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float M[3][3] = { { 1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y - w * z), 2.0f * (x * z + w * y) },
                            { 2.0f * (x * y + w * z), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z - w * x) },
                            { 2.0f * (x * z - w * y), 2.0f * (y * z + w * x), 1.0f - 2.0f * (x * x + y * y) } };
    const float e = ((b - a + 3) % 3 == 1) ? 1.0f : -1.0f;
    float sp = e * M[a][c];
    sp = sp > 1.0f ? 1.0f : (sp < -1.0f ? -1.0f : sp);
    *pitch = std::asin(sp);
    if (std::fabs(sp) < 0.999999f) {
        *yaw = std::atan2(-e * M[b][c], M[c][c]);
        *roll = std::atan2(-e * M[a][b], M[a][a]);
    } else {
        *roll = 0.0f;
        *yaw = std::atan2(e * M[c][b], M[b][b]);
    }
    return true;
}

// (c) per local-player update: the head turn since the cutscene started goes on the render root; the hand-back turns the body by the snap.
struct State {
    bool               active = false;
    float              y0 = 0.0f;
    unsigned long long obj = 0;
};

struct Step {
    float rootAddDeg = 0.0f;   // yaw for the cached render camera this update (0 when not in a cutscene)
    bool  handback = false;    // this update ends a cutscene: turn the body once by handbackDeg
    float handbackDeg = 0.0f;
    bool  started = false;     // this update latched y0
};

// `folded` = body-follows-head's folded yaw for this player at this update (0 when the fold never ran). After the game's Stop writes the
// look, the camera is fields + (h - folded) whether the fold resumes or is switched off (headfollow_logic.h), so the hand-back that keeps
// the view where it was during the cutscene (anim + h - y0) is folded - y0.
inline Step Update(State& s, unsigned long long obj, bool ownerIsCinematic, float h, float folded = 0.0f)
{
    Step st;
    if (s.active && obj != s.obj) {   // a new player object: drop the old cutscene, never hand it back to someone else
        s = State();
    }
    if (ownerIsCinematic) {
        if (!s.active) {
            s.active = true;
            s.y0 = h;
            s.obj = obj;
            st.started = true;
        }
        st.rootAddDeg = Wrap180(h - s.y0);
        return st;
    }
    if (s.active) {
        st.handback = true;
        st.handbackDeg = Wrap180(folded - s.y0);
        s = State();
    }
    return st;
}

}  // namespace cinyaw
}  // namespace dlvr
