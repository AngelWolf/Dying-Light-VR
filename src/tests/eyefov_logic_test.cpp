// Standalone unit test for src/ovr2xr/eyefov_logic.h - NOT part of the DLL build.
// INV-597 / docs/BETA-FINDINGS-2026-10-19.md 7f (Jay: "Build it."). Exit code = number of failures.
#include "eyefov_logic.h"
#include "fovcrop_logic.h"

#include <cmath>
#include <cstdio>
#include <limits>

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

// Meta's PC runtime (Quest Link) is named: its views are submitted as its own FOV's window (fovcrop_logic.h, 2026-11-24; until then
// symmetric_fov was forced to 0 there). Names as logged: "Oculus 1.208.0" (runtimeName "Oculus"), "VirtualDesktopXR", "SteamVR/OpenXR".
static void meta_runtime_uses_its_own_fov()
{
    CHECK(RuntimeStretchesToOwnFov("Oculus"));
    CHECK(!RuntimeStretchesToOwnFov("VirtualDesktopXR"));
    CHECK(!RuntimeStretchesToOwnFov("SteamVR/OpenXR"));
    CHECK(!RuntimeStretchesToOwnFov("Ocul"));
    CHECK(!RuntimeStretchesToOwnFov(""));
    CHECK(!RuntimeStretchesToOwnFov(nullptr));
}

// ---- the LIVE eye FOV (trace docs/traces/20261125_1040_live_eye_fov.md) ----
static Tans Mirror(const Tans& t) { return Tans{ t.r, t.l, t.u, t.d }; }
static dlvr::fovcrop::Fov Ang(const Tans& t)
{
    return dlvr::fovcrop::Fov{ -std::atan(t.l), std::atan(t.r), std::atan(t.u), -std::atan(t.d) };
}

// What the layer hands the engine: symmetrise by mode, then grow to the eye texture's shape.
static Tans Given(const Tans& real, int mode, double aspect)
{
    return GrowToAspect(Symmetrise(real, mode), static_cast<float>(aspect));
}

struct Hmd { const char* name; Tans eye0; int w, h; };
static const Hmd kHmds[] = {
    { "Pimax Dream Air, 2026-11-24 live", { 1.3890f, 1.2030f, 1.0450f, 1.0450f }, 4104, 3320 },
    { "Quest 3, VDXR (Jay)", { 1.376382f, 0.839100f, 0.965689f, 1.428148f }, 3072, 3264 },
    { "PSVR2 (logged at 0)", { 1.2640f, 0.8900f, 1.2740f, 1.2740f }, 3400, 4024 },
    { "Steam Frame eye 0 (square)", { 1.7260f, 1.1000f, 1.4000f, 1.7260f }, 2160, 2160 },
};

// Mode 0: the drawn frustum contains the panel (so the crop always finds it) and its two shapes agree.
static void mode0_contains_the_panel()
{
    for (const Hmd& h : kHmds) {
        const double aspect = static_cast<double>(h.w) / h.h;
        for (int e = 0; e < 2; ++e) {
            const Tans real = e ? Mirror(h.eye0) : h.eye0;
            const Tans g = Given(real, 0, aspect);
            CHECK(g.l >= real.l - 1e-6f && g.r >= real.r - 1e-6f && g.u >= real.u - 1e-6f && g.d >= real.d - 1e-6f);
            CHECK(std::fabs((g.l + g.r) / (g.u + g.d) - aspect) < 1e-5);
            const auto c = dlvr::fovcrop::Crop(Ang(g), Ang(real), dlvr::fovcrop::Rect{ 0, 0, h.w, h.h });
            CHECK(c.why == dlvr::fovcrop::kCropped || c.why == dlvr::fovcrop::kWhole);
        }
    }
}

// THE CONTROL: the tester's 2026-11-24 launch - drawn from the FOV saved one launch earlier - must be refused by the same crop.
static void stale_saved_fov_is_refused()
{
    const Tans saved{ 1.3633f, 1.1808f, 1.0262f, 1.0262f };
    const Tans live{ 1.3890f, 1.2030f, 1.0450f, 1.0450f };
    const double aspect = 4104.0 / 3320.0;
    const auto c = dlvr::fovcrop::Crop(Ang(Given(saved, 0, aspect)), Ang(live), dlvr::fovcrop::Rect{ 0, 0, 4104, 3320 });
    CHECK(c.why == dlvr::fovcrop::kOutside);
    CHECK(std::fabs(c.marginPx[0] - (-41.9)) < 1.0);   // his log: margin px L-41.9
    const auto ok = dlvr::fovcrop::Crop(Ang(Given(live, 0, aspect)), Ang(live), dlvr::fovcrop::Rect{ 0, 0, 4104, 3320 });
    CHECK(ok.why == dlvr::fovcrop::kCropped || ok.why == dlvr::fovcrop::kWhole);
}

// The startup answer (mode 2 whatever symmetric_fov is) sets the engine's one centred game camera: vertical = max(up, down),
// horizontal = vertical x the texture's shape. It must reach each eye's outer edge; the mode-0 answer does not (the 1.07 gap).
static void superset_covers_the_outer_edge()
{
    for (const Hmd& h : kHmds) {
        const double aspect = static_cast<double>(h.w) / h.h;
        const Tans s = Given(Symmetrise(h.eye0, 0), 2, aspect);
        CHECK(Covers(s, h.eye0));
        CHECK(Covers(s, Mirror(h.eye0)));
        CHECK(Max(s.u, s.d) * aspect >= h.eye0.l - 1e-5);
    }
    const Tans j0 = Given(kHmds[1].eye0, 0, 3072.0 / 3264.0);
    CHECK(Max(j0.u, j0.d) * (3072.0 / 3264.0) < kHmds[1].eye0.l);   // control: Jay at 0 falls short
}

static void mode2_is_unchanged()
{
    // Quest 3 as logged at symmetric_fov 2 (3072 x 3264): L R 1.3764, U D 1.4624.
    const Tans s = Given(kHmds[1].eye0, 2, 3072.0 / 3264.0);
    CHECK(std::fabs(s.l - 1.3764f) < 1e-4f && s.l == s.r);
    CHECK(std::fabs(s.u - 1.4624f) < 1e-4f && s.u == s.d);
    const Tans m1 = Symmetrise(kHmds[1].eye0, 1);
    CHECK(m1.l == kHmds[1].eye0.l && m1.r == kHmds[1].eye0.r && m1.u == m1.d && m1.u == 1.428148f);
    CHECK(Same(Symmetrise(kHmds[1].eye0, 0), kHmds[1].eye0));
}

static void plausibility()
{
    const Tans ok{ 1.3f, 1.1f, 1.0f, 1.0f };
    CHECK(Plausible(ok));
    CHECK(!Plausible(Tans{ 0.0f, 0.0f, 0.0f, 0.0f }));
    CHECK(!Plausible(Tans{ -1.3f, 1.1f, 1.0f, 1.0f }));
    CHECK(!Plausible(Tans{ 1.3f, 1.1f, 1.0f, 50.0f }));
    CHECK(!Plausible(Tans{ std::numeric_limits<float>::quiet_NaN(), 1.1f, 1.0f, 1.0f }));
    CHECK(!Plausible(Tans{ std::numeric_limits<float>::infinity(), 1.1f, 1.0f, 1.0f }));
}

static const Tans kStart[2] = { { 1.3633f, 1.1808f, 1.0291f, 1.0291f }, { 1.1808f, 1.3633f, 1.0291f, 1.0291f } };
static const Tans kLive[2] = { { 1.3890f, 1.2030f, 1.0484f, 1.0484f }, { 1.2030f, 1.3890f, 1.0484f, 1.0484f } };

static void latch_before_anything_is_shown()
{
    LiveFov f;
    SetStart(f, kStart);
    CHECK(TryLatch(f, false, kLive) == LatchResult::kNone);          // views not located yet
    CHECK(Same(Used(f, 0), kStart[0]) && Same(Used(f, 1), kStart[1]));
    CHECK(TryLatch(f, true, kLive) == LatchResult::kSwitched);       // nothing shown since launch
    CHECK(Same(Used(f, 0), kLive[0]) && Same(Used(f, 1), kLive[1]));
    for (int i = 0; i < kHoldSubmits; ++i) { CHECK(!NoteSubmit(f, true)); }   // kept off the display
    CHECK(NoteSubmit(f, true));                                      // then shown
    CHECK(TryLatch(f, true, kStart) == LatchResult::kNone);          // once per launch
    CHECK(TryLatch(f, false, kStart) == LatchResult::kNone);
    CHECK(Same(Used(f, 0), kLive[0]));                               // a tracking loss does not bring the saved FOV back
}

static void waits_while_the_game_is_shown()
{
    LiveFov f;
    SetStart(f, kStart);
    CHECK(NoteSubmit(f, true));                                      // a game image is on the display
    CHECK(TryLatch(f, true, kLive) == LatchResult::kWaiting);
    CHECK(TryLatch(f, true, kLive) == LatchResult::kStillWaiting);   // logged once only
    CHECK(Same(Used(f, 0), kStart[0]));                              // FROZEN, not live
    CHECK(NoteSubmit(f, true));
    CHECK(!NoteSubmit(f, false));                                    // a loading screen
    CHECK(TryLatch(f, true, kLive) == LatchResult::kSwitched);
    int hidden = 0;
    while (!NoteSubmit(f, true)) { ++hidden; }
    CHECK(hidden == kHoldSubmits);
}

static void equal_fov_switches_at_once_with_no_hold()
{
    LiveFov f;
    SetStart(f, kStart);
    CHECK(NoteSubmit(f, true));
    const Tans same[2] = { { 1.3633004f, 1.1808f, 1.0291f, 1.0291f }, { 1.1808f, 1.3633f, 1.0291f, 1.0290996f } };
    CHECK(TryLatch(f, true, same) == LatchResult::kSame);
    CHECK(NoteSubmit(f, true));                                      // never hidden
    CHECK(Same(Used(f, 0), kStart[0]));                              // keeps the exact startup numbers
}

static void implausible_is_refused_for_both_eyes_and_retried()
{
    LiveFov f;
    SetStart(f, kStart);
    const Tans bad[2] = { kLive[0], { 0.0f, 0.0f, 0.0f, 0.0f } };
    CHECK(TryLatch(f, true, bad) == LatchResult::kRefused);
    CHECK(Same(Used(f, 0), kStart[0]) && Same(Used(f, 1), kStart[1]));
    CHECK(TryLatch(f, true, bad) == LatchResult::kNone);             // said once
    CHECK(TryLatch(f, true, kLive) == LatchResult::kSwitched);       // a good one later still counts
}

static void start_is_set_once_and_needed()
{
    LiveFov f;
    CHECK(!Ready(f));
    CHECK(TryLatch(f, true, kLive) == LatchResult::kNone);           // nothing to switch from yet
    SetStart(f, kStart);
    CHECK(Ready(f));
    SetStart(f, kLive);                                              // ignored
    CHECK(Same(Used(f, 0), kStart[0]));
    CHECK(Same(Used(f, 7), kStart[1]));                              // index is masked, never out of range
}

int main()
{
    mode0_contains_the_panel();
    stale_saved_fov_is_refused();
    superset_covers_the_outer_edge();
    mode2_is_unchanged();
    plausibility();
    latch_before_anything_is_shown();
    waits_while_the_game_is_shown();
    equal_fov_switches_at_once_with_no_hold();
    implausible_is_refused_for_both_eyes_and_retried();
    start_is_set_once_and_needed();
    steam_frame_eyes_get_one_frustum();
    mirror_eyes_unchanged();
    shared_contains_both_eyes();
    both_eyes_identical();
    meta_runtime_uses_its_own_fov();
    std::printf("eyefov_logic_test: %d failure(s)\n", g_fail);
    return g_fail;
}
