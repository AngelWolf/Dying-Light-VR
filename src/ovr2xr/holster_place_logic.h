// Pure placement rules for "place a holster by hand" - docs/superpowers/plans/2026-10-30-holster-place-by-hand.md Task 2.
// No OpenXR, no Windows, no logging, so src/src/tests/holster_place_logic_test.cpp compiles it on its own.
// holster.cpp owns everything with side effects. Every function here is a handful of float ops; none is called per frame
// except by the placing page itself (idle cost elsewhere: zero - nothing here runs unless a caller calls it).
#pragma once

#include "holster_logic.h"

#include <cmath>

namespace dlvr {
namespace holster {
namespace place {

// 0-2 equal holster.h HolsterZone values. kLChest (heal, spec 2026-11-01-manual-reload-design goal 2) is 4 HERE and
// kHolsterLChest = 3 in holster.h (that enum has no head zone) - every crossing names which.
enum Zone { kShoulder = 0, kRHip = 1, kLHip = 2, kHead = 3, kLChest = 4, kZones = 5 };

struct Range { Vec3 lo, hi; float rMin, rMax; };

// The S_FLOAT limits of holster_*_center_* / holster_*_radius (ovr2xr.cpp rows ~1785-1906): shoulder & head centre +-0.5 m each
// axis; hips x -0.8..0.8, y -1.2..0.5, z -0.8..0.8; radius 0.03..0.5 (head 0.03..0.3).
inline Range RangeOf(int zone)
{
    if (zone == kRHip || zone == kLHip || zone == kLChest) { return { { -0.8f, -1.2f, -0.8f }, { 0.8f, 0.5f, 0.8f }, 0.03f, 0.5f }; }
    if (zone == kHead) { return { { -0.5f, -0.5f, -0.5f }, { 0.5f, 0.5f, 0.5f }, 0.03f, 0.3f }; }
    return { { -0.5f, -0.5f, -0.5f }, { 0.5f, 0.5f, 0.5f }, 0.03f, 0.5f };
}

// The compiled defaults (holster.cpp 43-45, 68-70, 1344-1346, 1361-1363).
inline Vec3 DefaultCentre(int zone)
{
    switch (zone) {
        case kRHip:  return { 0.20f, -0.60f, 0.00f };
        case kLHip:  return { -0.20f, -0.60f, 0.00f };
        case kHead:  return { -0.12f, 0.00f, 0.00f };
        case kLChest: return { -0.12f, -0.30f, -0.12f };   // a start value, placed by hand (Tier D); clear of the hip and temple
        default:     return { 0.15f, -0.15f, 0.10f };
    }
}

inline float DefaultRadius(int zone) { return zone == kHead ? 0.10f : (zone == kLChest ? 0.08f : 0.15f); }

// Same-hand partner (the pair that must not overlap): shoulder <-> right hip, left hip <-> head.
inline int PartnerOf(int zone)
{
    switch (zone) {
        case kShoulder: return kRHip;
        case kRHip:     return kShoulder;
        case kLHip:     return kHead;
        default:        return kLHip;
    }
}

inline int HandOf(int zone) { return (zone == kLHip || zone == kHead || zone == kLChest) ? 1 : 0; }

// EVERY same-hand zone that must not overlap this one (the left hand has three since 2026-11-01). out[] gets up to 2; returns n.
inline int PartnersOf(int zone, int out[2])
{
    switch (zone) {
        case kShoulder: out[0] = kRHip; return 1;
        case kRHip:     out[0] = kShoulder; return 1;
        case kLHip:     out[0] = kHead; out[1] = kLChest; return 2;
        case kHead:     out[0] = kLHip; out[1] = kLChest; return 2;
        default:        out[0] = kLHip; out[1] = kHead; return 2;   // kLChest
    }
}

// Hips only: "placed by hand" = the fixed centre differs from the compiled default by more than 0.5 mm on any axis (no new
// settings key). The shoulder and head have no calibration to protect, so they are never "placed".
inline bool PlacedByHand(int zone, const Vec3& fixedCentre)
{
    if (zone != kRHip && zone != kLHip) { return false; }
    const Vec3 d = DefaultCentre(zone);
    return std::fabs(fixedCentre.x - d.x) > 0.0005f || std::fabs(fixedCentre.y - d.y) > 0.0005f ||
           std::fabs(fixedCentre.z - d.z) > 0.0005f;
}

enum class Verdict { Ok, Untracked, OutOfRange, Overlaps };

// `local` = the hand in this zone's frame; `partner` = the partner zone's EFFECTIVE params now; `partnerOn` = its enable.
// Order: not tracked, then outside the slider range (or non-finite), then overlap with the same-hand partner.
inline Verdict Check(int zone, bool tracked, const Vec3& local, float radius, const Params& partner, bool partnerOn)
{
    if (!tracked) { return Verdict::Untracked; }
    const Range r = RangeOf(zone);
    if (!std::isfinite(local.x) || !std::isfinite(local.y) || !std::isfinite(local.z)) { return Verdict::OutOfRange; }
    if (local.x < r.lo.x || local.x > r.hi.x || local.y < r.lo.y || local.y > r.hi.y || local.z < r.lo.z || local.z > r.hi.z) {
        return Verdict::OutOfRange;
    }
    if (partnerOn && ZonesOverlap(Params{ local, radius }, partner)) { return Verdict::Overlaps; }
    return Verdict::Ok;
}

// Check against every same-hand partner (PartnersOf order): the first verdict that is not Ok, else Ok.
inline Verdict CheckAll(int zone, bool tracked, const Vec3& local, float radius, const Params* partners, const bool* on, int n)
{
    static const Params kNone{ { 0.0f, 0.0f, 0.0f }, 0.0f };
    const Verdict v0 = Check(zone, tracked, local, radius, kNone, false);
    if (v0 != Verdict::Ok) { return v0; }
    for (int i = 0; i < n; ++i) {
        if (on[i] && ZonesOverlap(Params{ local, radius }, partners[i])) { return Verdict::Overlaps; }
    }
    return Verdict::Ok;
}

// Largest radius <= RangeOf.rMax whose exit sphere does not touch the partner's: (r + pr) * kExitScale < |c - pc|, clamped to
// >= rMin (a partner already too close yields rMin, never below the slider floor). Partner off -> rMax.
inline float RadiusMax(int zone, const Vec3& centre, const Params& partner, bool partnerOn)
{
    const Range rg = RangeOf(zone);
    if (!partnerOn) { return rg.rMax; }
    const float dx = centre.x - partner.centre.x;
    const float dy = centre.y - partner.centre.y;
    const float dz = centre.z - partner.centre.z;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    float r = dist / kExitScale - partner.radius;
    if (r > rg.rMax) { r = rg.rMax; }
    if (r < rg.rMin) { r = rg.rMin; }
    return r;
}

// RadiusMax against every same-hand partner: the smallest of each enabled partner's limit (rMax when none is on).
inline float RadiusMaxAll(int zone, const Vec3& centre, const Params* partners, const bool* on, int n)
{
    float r = RangeOf(zone).rMax;
    for (int i = 0; i < n; ++i) {
        if (!on[i]) { continue; }
        const float ri = RadiusMax(zone, centre, partners[i], true);
        if (ri < r) { r = ri; }
    }
    return r;
}

}  // namespace place
}  // namespace holster
}  // namespace dlvr
