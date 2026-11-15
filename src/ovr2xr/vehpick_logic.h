// The buggy's declaration: WHICH head-yaw ring entry the camera carried (2026-11-05, Jay: "I want you to be 100% certain for this fix" ->
// "Yes"; STATE 11-05 (2)). No game calls, no state outside State: tests/vehpick_logic_test.cpp, and tools/vehrec_replay.py replays the same
// rule on a [VEHREC] recording.
// WHY NOT A PER-FRAME MATCH: RdVehPick's v2 matched every entry against a tracked car term, ties to the newest. That is right only while the
// camera carries k = 0: on k = 1 the head's own step was taken for a car term and k = 0 kept matching (BG5, real recording: parked + head
// turning 41.6 % of frames off, driving 66.9 %; replay == declared on 20,780 of 20,780). The carried entry is STEADY (parked k1 on 4,822 of 4,863,
// term 0.000); what moves frame to frame is the car's term.
// THE RULE: a real term is common to every entry, so it cannot tell them apart; only the wrong entries' offsets also wobble with the head. A
// running score per entry of its offset's squared step; the HELD entry changes only when another's score is clearly lower; nothing is scored
// while the head is still (the entries are then the same pose - no evidence). No single frame ever declares anything but the held entry.
#pragma once

namespace dlvr {
namespace vehpick {

constexpr int   kRing      = 8;
constexpr float kRate      = 1.0f / 48.0f;   // the score's running rate: about a third of a second at 128 frames / s
constexpr float kMargin    = 0.6f;           // another entry must score below this share of the held one
constexpr float kFloor     = 0.005f;         // a held score this small has nothing to answer for (deg^2; every case passes 0.003 - 0.02)
constexpr float kStepClamp = 3.0f;           // one odd frame cannot own a score (deg)
constexpr float kStillDeg  = 0.25f;          // newest vs 3 updates back under this: the head is still (deg)

struct State {
    int   k = 1;                 // the held entry; the camera normally carries 1 (INV-323)
    float sc[kRing] = {};
    float lo[kRing] = {};        // each entry's offset last frame
    bool  haveLo[kRing] = {};
};

inline float Wrap180(float d)
{
    while (d > 180.0f) { d -= 360.0f; }
    while (d < -180.0f) { d += 360.0f; }
    return d;
}

// A new stretch: the ring may jump; the held entry and the scores stay.
inline void Reset(State& s)
{
    for (int j = 0; j < kRing; ++j) { s.haveLo[j] = false; }
}

// off[j] = the camera yaw less entry j (deg), hNew / hOld = the head yaw added at the newest entry and 3 back (or the oldest there is).
// Returns the entry to declare from. n <= 0: entry 0, nothing is held or scored.
inline int Step(State& s, const float* off, int n, float hNew, float hOld)
{
    if (n <= 0) { return 0; }
    if (n > kRing) { n = kRing; }
    if (s.k >= n) { s.k = n - 1; }
    const float still = Wrap180(hNew - hOld);
    if ((still < 0.0f ? -still : still) < kStillDeg) {
        for (int j = 0; j < n; ++j) { s.lo[j] = off[j]; s.haveLo[j] = true; }
        return s.k;
    }
    for (int j = 0; j < n; ++j) {
        if (s.haveLo[j]) {
            float d = Wrap180(off[j] - s.lo[j]);
            d = d > kStepClamp ? kStepClamp : (d < -kStepClamp ? -kStepClamp : d);
            s.sc[j] += kRate * (d * d - s.sc[j]);
        }
        s.lo[j] = off[j];
        s.haveLo[j] = true;
    }
    int b = 0;
    for (int j = 1; j < n; ++j) { if (s.sc[j] < s.sc[b]) { b = j; } }
    if (b != s.k && s.sc[s.k] > kFloor && s.sc[b] < kMargin * s.sc[s.k]) { s.k = b; }
    return s.k;
}

}  // namespace vehpick
}  // namespace dlvr
