// Standalone unit test for src/src/ovr2xr/holster_place_logic.h - NOT part of the DLL build.
// docs/superpowers/plans/2026-10-30-holster-place-by-hand.md Task 2. Exit code = number of failures.
#include "holster_place_logic.h"

#include <cmath>
#include <cstdio>

using namespace dlvr::holster;
using namespace dlvr::holster::place;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
static bool NearV(const Vec3& a, const Vec3& b) { return Near(a.x, b.x) && Near(a.y, b.y) && Near(a.z, b.z); }

static const Params kFarPartner{ { 0.9f, 0.9f, 0.9f }, 0.10f };

static void RangesMatchTheSettingsTable()
{
    const Range s = RangeOf(kShoulder);
    CHECK(NearV(s.lo, { -0.5f, -0.5f, -0.5f }));
    CHECK(NearV(s.hi, { 0.5f, 0.5f, 0.5f }));
    CHECK(Near(s.rMax, 0.5f));
    const Range l = RangeOf(kLHip);
    CHECK(NearV(l.lo, { -0.8f, -1.2f, -0.8f }));
    CHECK(NearV(l.hi, { 0.8f, 0.5f, 0.8f }));
    const Range r = RangeOf(kRHip);
    CHECK(NearV(r.lo, { -0.8f, -1.2f, -0.8f }));
    CHECK(NearV(r.hi, { 0.8f, 0.5f, 0.8f }));
    CHECK(Near(RangeOf(kHead).rMax, 0.3f));
    CHECK(NearV(RangeOf(kHead).lo, { -0.5f, -0.5f, -0.5f }));
    CHECK(NearV(RangeOf(kHead).hi, { 0.5f, 0.5f, 0.5f }));
    for (int z = 0; z < kZones; ++z) { CHECK(Near(RangeOf(z).rMin, 0.03f)); }
}

static void DefaultsAndPairs()
{
    CHECK(NearV(DefaultCentre(kShoulder), { 0.15f, -0.15f, 0.10f }));
    CHECK(NearV(DefaultCentre(kRHip), { 0.20f, -0.60f, 0.00f }));
    CHECK(NearV(DefaultCentre(kLHip), { -0.20f, -0.60f, 0.00f }));
    CHECK(NearV(DefaultCentre(kHead), { -0.12f, 0.00f, 0.00f }));
    CHECK(Near(DefaultRadius(kShoulder), 0.15f));
    CHECK(Near(DefaultRadius(kRHip), 0.15f));
    CHECK(Near(DefaultRadius(kLHip), 0.15f));
    CHECK(Near(DefaultRadius(kHead), 0.10f));
    CHECK(PartnerOf(kShoulder) == kRHip && PartnerOf(kRHip) == kShoulder);
    CHECK(PartnerOf(kLHip) == kHead && PartnerOf(kHead) == kLHip);
    CHECK(HandOf(kShoulder) == 0 && HandOf(kRHip) == 0 && HandOf(kLHip) == 1 && HandOf(kHead) == 1);
}

static void PlacedByHandOnlyWhenMoved()
{
    CHECK(!PlacedByHand(kRHip, { 0.2f, -0.6f, 0.0f }));
    CHECK(!PlacedByHand(kRHip, { 0.2f, -0.6f, 0.0004f }));
    CHECK(PlacedByHand(kRHip, { 0.2f, -0.6f, 0.001f }));
    CHECK(!PlacedByHand(kLHip, { -0.2f, -0.6f, 0.0f }));
    CHECK(PlacedByHand(kLHip, { -0.2f, -0.6f, 0.001f }));
    CHECK(!PlacedByHand(kShoulder, { 0.3f, 0.0f, 0.0f }));
    CHECK(!PlacedByHand(kHead, { 0.3f, 0.0f, 0.0f }));
}

static void PlacedByHandSurvivesCalibration()
{
    CHECK(PlacedByHand(kRHip, { 0.25f, -0.55f, 0.02f }));
    CHECK(PlacedByHand(kLHip, { -0.25f, -0.55f, 0.02f }));
}

static void RefuseUntracked()
{
    CHECK(Check(kShoulder, false, { 0.15f, -0.15f, 0.1f }, 0.15f, kFarPartner, true) == Verdict::Untracked);
}

static void RefuseOutOfRange()
{
    CHECK(Check(kShoulder, true, { 0.6f, 0.0f, 0.0f }, 0.15f, kFarPartner, true) == Verdict::OutOfRange);
    CHECK(Check(kLHip, true, { -0.2f, -1.3f, 0.0f }, 0.15f, kFarPartner, true) == Verdict::OutOfRange);
    CHECK(Check(kLHip, true, { -0.2f, -1.2f, 0.0f }, 0.15f, kFarPartner, true) == Verdict::Ok);
}

static void RefuseSameHandOverlap()
{
    const Params rHip{ { 0.2f, -0.6f, 0.0f }, 0.15f };
    CHECK(Check(kShoulder, true, { 0.2f, -0.5f, 0.0f }, 0.15f, rHip, true) == Verdict::Overlaps);
    CHECK(Check(kShoulder, true, { 0.2f, -0.5f, 0.0f }, 0.15f, rHip, false) == Verdict::Ok);
}

static void PlaceStoresTheLocalItWasGiven()
{
    const Vec3 local{ 0.12f, -0.2f, 0.08f };
    CHECK(Check(kShoulder, true, local, 0.15f, kFarPartner, true) == Verdict::Ok);
    CHECK(local.x == 0.12f && local.y == -0.2f && local.z == 0.08f);
}

static void RadiusMaxStopsShortOfOverlap()
{
    const Params p{ { 0.0f, 0.0f, 0.0f }, 0.10f };
    const Vec3 c{ 0.40f, 0.0f, 0.0f };
    const float rm = RadiusMax(kShoulder, c, p, true);
    CHECK(Near(rm, 0.40f / 1.2f - 0.10f, 1e-3f));
    CHECK(rm <= RangeOf(kShoulder).rMax && rm >= RangeOf(kShoulder).rMin);
    // at the returned radius the pair must not overlap (strictly inside the exit spheres' gap)
    CHECK(!ZonesOverlap(Params{ c, rm - 0.001f }, p));
    // partner touching -> rMin
    CHECK(Near(RadiusMax(kShoulder, { 0.05f, 0.0f, 0.0f }, p, true), RangeOf(kShoulder).rMin));
    // partner off -> the slider maximum; head keeps its own
    CHECK(Near(RadiusMax(kShoulder, c, p, false), RangeOf(kShoulder).rMax));
    CHECK(Near(RadiusMax(kHead, { 0.9f, 0.0f, 0.0f }, p, true), RangeOf(kHead).rMax));
    CHECK(Near(RadiusMax(kHead, c, p, false), 0.3f));
}

// Spec 2026-11-01-manual-reload-design goal 2: heal moves to a LEFT-CHEST zone (zone 4 here), placed with the left hand.
static void LeftChestZoneDefinition()
{
    static_assert(kZones == 5, "the left chest is the fifth placeable zone");
    const Range c = RangeOf(kLChest);
    CHECK(NearV(c.lo, { -0.8f, -1.2f, -0.8f }));   // the hips' ranges (plan Task 3, my call)
    CHECK(NearV(c.hi, { 0.8f, 0.5f, 0.8f }));
    CHECK(Near(c.rMin, 0.03f) && Near(c.rMax, 0.5f));
    CHECK(NearV(DefaultCentre(kLChest), { -0.12f, -0.30f, -0.12f }));
    CHECK(Near(DefaultRadius(kLChest), 0.08f));
    CHECK(HandOf(kLChest) == 1);
    CHECK(!PlacedByHand(kLChest, { 0.3f, 0.0f, 0.0f }));   // no calibration to protect
}

static void LeftHandThreeZonesNeverOverlap()
{
    int out[2] = { -1, -1 };
    CHECK(PartnersOf(kLChest, out) == 2 && out[0] == kLHip && out[1] == kHead);
    CHECK(PartnersOf(kLHip, out) == 2 && out[0] == kHead && out[1] == kLChest);
    CHECK(PartnersOf(kHead, out) == 2 && out[0] == kLHip && out[1] == kLChest);
    CHECK(PartnersOf(kShoulder, out) == 1 && out[0] == kRHip);
    CHECK(PartnersOf(kRHip, out) == 1 && out[0] == kShoulder);
    const Params hip{ DefaultCentre(kLHip), DefaultRadius(kLHip) };
    const Params head{ DefaultCentre(kHead), DefaultRadius(kHead) };
    const Params both[2] = { hip, head };
    const bool on[2] = { true, true };
    CHECK(CheckAll(kLChest, true, { -0.2f, -0.55f, 0.0f }, 0.08f, both, on, 2) == Verdict::Overlaps);   // over the hip
    CHECK(CheckAll(kLChest, true, { -0.12f, -0.05f, 0.0f }, 0.08f, both, on, 2) == Verdict::Overlaps);  // over the temple
    const bool hipOff[2] = { false, true };
    CHECK(CheckAll(kLChest, true, { -0.2f, -0.55f, 0.0f }, 0.08f, both, hipOff, 2) == Verdict::Ok);     // hip disabled
    CHECK(CheckAll(kLChest, false, DefaultCentre(kLChest), 0.08f, both, on, 2) == Verdict::Untracked);
}

// The compiled defaults must not refuse each other at startup (HolsterLeftPoll refuses overlapping zones by name).
static void LeftDefaultsDoNotOverlap()
{
    const Params chest{ DefaultCentre(kLChest), DefaultRadius(kLChest) };
    const Params hip{ DefaultCentre(kLHip), DefaultRadius(kLHip) };
    const Params head{ DefaultCentre(kHead), DefaultRadius(kHead) };
    CHECK(!ZonesOverlap(chest, hip));
    CHECK(!ZonesOverlap(chest, head));
    CHECK(!ZonesOverlap(hip, head));
    const Params both[2] = { hip, head };
    const bool on[2] = { true, true };
    CHECK(CheckAll(kLChest, true, DefaultCentre(kLChest), DefaultRadius(kLChest), both, on, 2) == Verdict::Ok);
}

static void RadiusMaxAllStopsAtTheNearest()
{
    const Params nearP{ { 0.0f, 0.0f, 0.0f }, 0.10f };
    const Params farP{ { 0.0f, -0.9f, 0.0f }, 0.10f };
    const Params ps[2] = { farP, nearP };
    const bool on[2] = { true, true };
    const Vec3 c{ 0.40f, 0.0f, 0.0f };
    CHECK(Near(RadiusMaxAll(kLChest, c, ps, on, 2), 0.40f / 1.2f - 0.10f, 1e-3f));
    const bool nearOff[2] = { true, false };
    CHECK(RadiusMaxAll(kLChest, c, ps, nearOff, 2) > 0.40f / 1.2f - 0.10f + 0.01f);
    const bool none[2] = { false, false };
    CHECK(Near(RadiusMaxAll(kLChest, c, ps, none, 2), RangeOf(kLChest).rMax));
}

int main()
{
    LeftChestZoneDefinition();
    LeftHandThreeZonesNeverOverlap();
    LeftDefaultsDoNotOverlap();
    RadiusMaxAllStopsAtTheNearest();
    RangesMatchTheSettingsTable();
    DefaultsAndPairs();
    PlacedByHandOnlyWhenMoved();
    PlacedByHandSurvivesCalibration();
    RefuseUntracked();
    RefuseOutOfRange();
    RefuseSameHandOverlap();
    PlaceStoresTheLocalItWasGiven();
    RadiusMaxStopsShortOfOverlap();
    std::printf("holster_place_logic_test: %d failure(s)\n", g_fail);
    return g_fail;
}
