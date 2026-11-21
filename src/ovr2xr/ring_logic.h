// Pure logic: the visible holster rings (option B) - spec docs/superpowers/specs/2026-11-20-holster-pivot-posture-rings-design.md §5.3 /
// §6, plan docs/superpowers/plans/2026-11-20-holster-pivot-posture-rings.md Task 5. No OpenXR, no Windows, no logging, so
// src/src/tests/ring_logic_test.cpp compiles it on its own. holster.cpp measures the distances; overlay.cpp draws.
#pragma once

namespace dlvr {
namespace holster {
namespace ring {

constexpr float kFadeM     = 0.30f;   // a ring fades in as its hand comes within this of the zone's edge
constexpr float kEdgeAlpha = 0.75f;   // ... reaching this at the edge; 1.0 once the hand is inside ("brightens slightly")
constexpr int   kMaxRings  = 3;
constexpr float kFill      = 0.14f;   // option B (Jay 2026-11-20): the disc's opacity at alpha 1
constexpr float kOutline   = 0.60f;   // ... and the outline's
constexpr float kLineFrac  = 0.045f;  // the outline's width as a share of the radius (backups/20261120_holster_ring_renders)

// distToEdgeM = |grip - zone centre| - radius; inside = the hand is in the zone (the caller's own test, exit radius included).
inline float Alpha(float distToEdgeM, bool inside)
{
    if (inside) { return 1.0f; }
    const float d = distToEdgeM < 0.0f ? 0.0f : distToEdgeM;
    if (d >= kFadeM) { return 0.0f; }
    return kEdgeAlpha * (1.0f - d / kFadeM);
}

// The rings to draw: enabled zones with a visible alpha (not inside = by distance), nearest first, at most maxOut. Returns the count.
inline int Pick(const float* distToEdge, const bool* enabled, int n, int* outIdx, int maxOut)
{
    int c = 0;
    for (int i = 0; i < n; ++i) {
        if (!enabled[i] || Alpha(distToEdge[i], false) <= 0.0f) { continue; }
        int at = c < maxOut ? c : maxOut;   // insertion sort into the first maxOut slots
        while (at > 0 && distToEdge[outIdx[at - 1]] > distToEdge[i]) {
            if (at < maxOut) { outIdx[at] = outIdx[at - 1]; }
            --at;
        }
        if (at < maxOut) {
            outIdx[at] = i;
            if (c < maxOut) { ++c; }
        }
    }
    return c;
}

}  // namespace ring
}  // namespace holster
}  // namespace dlvr
