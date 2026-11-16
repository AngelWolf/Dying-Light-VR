// Standalone unit test for src/ovr2xr/eyefov_logic.h - NOT part of the DLL build.
// INV-597 / docs/BETA-FINDINGS-2026-10-19.md 7f (Jay: "Build it."). Exit code = number of failures.
#include "eyefov_logic.h"

#include <cstdio>

using namespace dlvr::eyefov;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static bool Same(const Tans& a, const Tans& b) { return a.l == b.l && a.r == b.r && a.u == b.u && a.d == b.d; }
static bool Covers(const Tans& big, const Tans& t) { return big.l >= t.l && big.r >= t.r && big.u >= t.u && big.d >= t.d; }

// A tester's Steam Frame, as logged (HmdDesc ... given, symmetric_fov=2): the two eyes were handed different frustums and the
// AFW rebuild's held->target read 0.98135 = their ratio. Shared, both eyes get the wider one.
static void steam_frame_eyes_get_one_frustum()
{
    const Tans e0{ 1.7260f, 1.7260f, 1.7260f, 1.7260f };
    const Tans e1{ 1.6938f, 1.6938f, 1.6938f, 1.6938f };
    CHECK(Differ(e0, e1));
    CHECK(Same(Shared(e0, e1), e0));
    CHECK(Same(Shared(e1, e0), e0));
}

// Quest 3 (mirror-image eyes): nothing changes.
static void mirror_eyes_unchanged()
{
    const Tans q{ 1.3764f, 1.3764f, 1.4624f, 1.4624f };
    CHECK(!Differ(q, q));
    CHECK(Same(Shared(q, q), q));
}

// Per component: a wider-horizontal eye and a taller eye -> the frustum that contains both.
static void shared_contains_both_eyes()
{
    const Tans a{ 1.50f, 1.50f, 1.70f, 1.70f };
    const Tans b{ 1.60f, 1.60f, 1.60f, 1.60f };
    const Tans s = Shared(a, b);
    CHECK(Covers(s, a));
    CHECK(Covers(s, b));
    CHECK(s.l == 1.60f && s.u == 1.70f);
}

// Each eye computes Shared(own, other): both must land on the identical frustum - the AFW rebuild assumes one projection.
static void both_eyes_identical()
{
    const Tans e0{ 1.5396f, 1.5396f, 1.7092f, 1.7092f };   // a tester's Index, as logged
    const Tans e1{ 1.5416f, 1.5416f, 1.7116f, 1.7116f };
    CHECK(Same(Shared(e0, e1), Shared(e1, e0)));
    CHECK(Differ(e0, e1));
}

// Meta's PC runtime (Quest Link): any symmetrising acts as 0; every other runtime keeps the setting (Jay 2026-11-16: "A").
// Names as logged: "Oculus 1.208.0" (the runtimeName field is "Oculus"), "VirtualDesktopXR", "SteamVR/OpenXR".
static void meta_runtime_uses_its_own_fov()
{
    CHECK(RuntimeStretchesToOwnFov("Oculus"));
    CHECK(!RuntimeStretchesToOwnFov("VirtualDesktopXR"));
    CHECK(!RuntimeStretchesToOwnFov("SteamVR/OpenXR"));
    CHECK(!RuntimeStretchesToOwnFov("Ocul"));
    CHECK(!RuntimeStretchesToOwnFov(""));
    CHECK(!RuntimeStretchesToOwnFov(nullptr));
    CHECK(EffectiveSymmetric(2, true) == 0);
    CHECK(EffectiveSymmetric(1, true) == 0);
    CHECK(EffectiveSymmetric(0, true) == 0);
    CHECK(EffectiveSymmetric(2, false) == 2);
    CHECK(EffectiveSymmetric(1, false) == 1);
    CHECK(EffectiveSymmetric(0, false) == 0);
}

int main()
{
    steam_frame_eyes_get_one_frustum();
    mirror_eyes_unchanged();
    shared_contains_both_eyes();
    both_eyes_identical();
    meta_runtime_uses_its_own_fov();
    std::printf("eyefov_logic_test: %d failure(s)\n", g_fail);
    return g_fail;
}
