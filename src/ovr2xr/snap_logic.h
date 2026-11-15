// Pure logic for snap turning - docs/SPEC-VR-COMFORT-2026-09-30.md §5 (C0).
// No OpenXR, no Windows, no logging, so src/src/tests/snap_logic_test.cpp compiles it on its own.
// ovr2xr.cpp (the right-stick edge, dlvr_FilterScePad) and boneblend.cpp (camyaw::Hook, which applies the
// snap to the look/facing yaws) own everything with side effects.
#pragma once

namespace dlvr {
namespace snapturn {

// A flick fires at |x| >= kFire and re-arms only once the stick is back under kRearm, so a thumb resting
// near the fire line cannot chatter out a string of snaps.
constexpr float kFire  = 0.7f;
constexpr float kRearm = 0.3f;
// A snap queued by the pad thread and not consumed by the player update within this long is DROPPED, not
// applied late: the game was paused, in a menu, loading, or not on the normal look controller.
constexpr unsigned long long kMaxQueueAgeMs = 300;

// comfort_snap_angle's items. Jay 2026-09-30: 30 degrees is the standard; 15/45 etc. selectable.
constexpr int kAngleCount = 5;
constexpr int kDefaultAngleIndex = 1;
inline float AngleDeg(int idx)
{
    static const float kDeg[kAngleCount] = { 15.0f, 30.0f, 45.0f, 60.0f, 90.0f };
    if (idx < 0) { idx = 0; }
    if (idx >= kAngleCount) { idx = kAngleCount - 1; }
    return kDeg[idx];
}

inline bool QueueStale(unsigned long long nowMs, unsigned long long queuedMs)
{
    return nowMs > queuedMs && nowMs - queuedMs > kMaxQueueAgeMs;
}

// The right stick's X axis in, one snap out: +1 right, -1 left, 0 nothing.
class Edge {
public:
    int Update(float x, unsigned long long nowMs, unsigned long long repeatMs)
    {
        const float ax = x < 0.0f ? -x : x;
        const int side = x > 0.0f ? 1 : -1;
        if (!primed_) {
            // First poll (or first after Reset): a stick already off centre is a flick we never saw start.
            primed_ = true;
            held_ = ax >= kRearm ? side : 0;
            return 0;
        }
        if (ax < kRearm) {
            held_ = 0;
            return 0;
        }
        if (ax < kFire) {
            return 0;   // between the lines: hysteresis, nothing changes
        }
        if (held_ != side) {
            // From centre, or straight across from the other side (a slow poll can miss the centre).
            held_ = side;
            last_ = nowMs;
            return side;
        }
        if (repeatMs > 0 && nowMs - last_ >= repeatMs) {
            last_ = nowMs;
            return side;
        }
        return 0;
    }

    void Reset() { primed_ = false; held_ = 0; }

private:
    bool primed_ = false;
    int  held_   = 0;
    unsigned long long last_ = 0;
};

}  // namespace snapturn
}  // namespace dlvr
