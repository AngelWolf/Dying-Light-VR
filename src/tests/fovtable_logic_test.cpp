// Standalone unit test for src/ovr2xr/fovtable_logic.h - NOT part of the DLL build.
// docs/BETA-FINDINGS-2026-10-19.md F1 (Jay: "Table + wide fallback"). Exit code = number of failures.
#include "fovtable_logic.h"

#include <cstdio>

using namespace dlvr::fovtable;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static bool Covers(const Tans& row, const Tans& lens)
{
    return row.l >= lens.l && row.r >= lens.r && row.u >= lens.u && row.d >= lens.d;
}

// Every FOV measured from a tester or Jay, eye 0 AND eye 1, as the layer logged it (HmdDesc "runtime L= R= U= D=",
// 4 dp; PSVR2 from "runtime Fov[]", 3 dp). A row must CONTAIN each of these - a row smaller than the lens is the
// black-bar bug this table exists to remove.
struct Measured { const char* name; Tans e0, e1; };
static const Measured kMeasured[] = {
    { "Meta Quest 3",                     { 1.3764f, 0.8391f, 0.9657f, 1.4281f }, { 0.8391f, 1.3764f, 0.9657f, 1.4281f } },
    { "Meta Quest 3",                     { 1.2799f, 0.9004f, 1.1106f, 1.1918f }, { 0.9004f, 1.2799f, 1.1106f, 1.1918f } },
    { "Meta Quest 3",                     { 1.1504f, 1.0000f, 1.1106f, 1.1918f }, { 1.0000f, 1.1504f, 1.1106f, 1.1918f } },
    { "SteamVR/OpenXR : oculus",          { 1.3764f, 0.8391f, 0.9657f, 1.4281f }, { 0.8391f, 1.3764f, 0.9657f, 1.4281f } },
    { "SteamVR/OpenXR : oculus",          { 1.2799f, 1.0000f, 1.1106f, 1.1918f }, { 1.0000f, 1.2799f, 1.1106f, 1.1918f } },
    { "SteamVR/OpenXR : playstation_vr2", { 1.842f, 0.947f, 1.329f, 1.329f },     { 0.947f, 1.842f, 1.329f, 1.329f } },
    { "SteamVR/OpenXR : lighthouse",      { 1.5396f, 1.0514f, 1.4060f, 1.4129f }, { 1.0515f, 1.5416f, 1.4080f, 1.4127f } },
    { "SteamVR/OpenXR : cv",              { 1.6713f, 1.2053f, 1.1722f, 1.7260f }, { 1.2038f, 1.6827f, 1.1974f, 1.6938f } },
};

static void every_measured_lens_fits_its_row()
{
    for (const Measured& m : kMeasured) {
        Tans t0{}, t1{};
        CHECK(Lookup(m.name, 0, t0) != nullptr);
        CHECK(Lookup(m.name, 1, t1) != nullptr);
        if (!Covers(t0, m.e0)) { std::printf("  eye0 too small for %s\n", m.name); ++g_fail; }
        if (!Covers(t1, m.e1)) { std::printf("  eye1 too small for %s\n", m.name); ++g_fail; }
    }
}

static void every_measured_lens_fits_the_wide_fallback()
{
    for (const Measured& m : kMeasured) {
        Tans t0{}, t1{};
        CHECK(Lookup("Some Headset Nobody Has Measured", 0, t0) == nullptr);
        CHECK(Lookup("Some Headset Nobody Has Measured", 1, t1) == nullptr);
        CHECK(Covers(t0, m.e0));
        CHECK(Covers(t1, m.e1));
    }
}

static void eye1_is_eye0_mirrored()
{
    Tans a{}, b{};
    Lookup("SteamVR/OpenXR : playstation_vr2", 0, a);
    Lookup("SteamVR/OpenXR : playstation_vr2", 1, b);
    CHECK(a.l == b.r && a.r == b.l && a.u == b.u && a.d == b.d);
    CHECK(a.l > a.r);   // eye 0 = the LEFT eye: its outer (left) edge is the wide one
}

static void quest3_row_is_not_the_wide_fallback()
{
    // The point of a row: a known headset gets its own (smaller) FOV, not the all-headsets superset, so its first
    // launch is not softer than it needs to be.
    Tans q{}, w{};
    Lookup("Meta Quest 3", 0, q);
    Lookup("", 0, w);
    CHECK(q.l < w.l);
    CHECK(q.d < w.d);
}

static void name_must_match_exactly()
{
    Tans t{};
    CHECK(Lookup("Meta Quest 3S", 0, t) == nullptr);   // a different headset, not a prefix match
    CHECK(Lookup("meta quest 3", 0, t) == nullptr);    // the runtime's own spelling only
    CHECK(Lookup(nullptr, 0, t) == nullptr);           // no name (xrGetSystemProperties failed) -> wide fallback
    CHECK(t.l > 1.0f && t.d > 1.0f);                   // ... and the fallback is filled, never zeros
}

static void returned_label_is_the_name()
{
    Tans t{};
    const char* lbl = Lookup("SteamVR/OpenXR : cv", 0, t);
    CHECK(lbl != nullptr && lbl[0] == 'S');
}

int main()
{
    every_measured_lens_fits_its_row();
    every_measured_lens_fits_the_wide_fallback();
    eye1_is_eye0_mirrored();
    quest3_row_is_not_the_wide_fallback();
    name_must_match_exactly();
    returned_label_is_the_name();
    std::printf("fovtable_logic_test: %d failure(s)\n", g_fail);
    return g_fail;
}
