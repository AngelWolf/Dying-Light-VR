// Standalone unit test for src/ovr2xr/deskmirror_logic.h - NOT part of the DLL build.
// Desktop mirror, one eye (Jay 2026-11-14; trace docs/traces/20261114_1030_desktop_mirror_single_eye.md). Exit code = number of failures.
#include "deskmirror_logic.h"

#include <cmath>
#include <cstdio>

using namespace dlvr::deskmirror;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static bool Near(double a, double b) { return std::fabs(a - b) < 1e-6; }
static bool Inside(const RectF& c, const RectI& r)
{
    return c.x >= r.x + 0.5 - 1e-9 && c.y >= r.y + 0.5 - 1e-9 && c.x + c.w <= r.x + r.w - 0.5 + 1e-9 && c.y + c.h <= r.y + r.h - 0.5 + 1e-9;
}

// Jay's Quest 3 left eye (2688 x 2880 in a 5376 x 2880 texture) into a 16:9 window: full width, a 16:9 band, inside the left half.
static void quest3_left_eye_16x9()
{
    const RectI eye{ 0, 0, 2688, 2880 };
    const RectF c = CropToAspect(eye, 16.0 / 9.0, 0.5, 0.5);
    CHECK(Near(c.w, 2687.0));
    CHECK(Near(c.w / c.h, 16.0 / 9.0));
    CHECK(Inside(c, eye));
    CHECK(Near(c.y + c.h * 0.5, 1440.0));   // centred
}

// The right half (view 0 packed on the right, swap_eyes / auto assign): the crop never leaves x >= 2688.5.
static void right_half_stays_right()
{
    const RectI eye{ 2688, 0, 2688, 2880 };
    const RectF c = CropToAspect(eye, 16.0 / 9.0, 0.0, 0.5);
    CHECK(Inside(c, eye));
    CHECK(c.x >= 2688.5);
}

// A window narrower than the eye (portrait): full height, the width cut, centred on u.
static void narrow_window_cuts_width()
{
    const RectI eye{ 0, 0, 2688, 2880 };
    const RectF c = CropToAspect(eye, 0.5, 0.62, 0.5);
    CHECK(Near(c.h, 2879.0));
    CHECK(Near(c.w, 1439.5));
    CHECK(Near(c.x + c.w * 0.5, 0.5 + 0.62 * 2687.0));
    CHECK(Inside(c, eye));
}

// A centre too near an edge is moved back inside, never cropped past the rect.
static void centre_near_edge_clamped()
{
    const RectI eye{ 0, 0, 2688, 2880 };
    const RectF c = CropToAspect(eye, 0.5, 0.99, 0.01);
    CHECK(Inside(c, eye));
    CHECK(Near(c.x + c.w, 2687.5));
}

// No window shape known (aspect 0) = the rect's own shape; a NaN centre = the middle.
static void fallbacks()
{
    const RectI eye{ 0, 0, 2688, 2880 };
    const RectF c = CropToAspect(eye, 0.0, std::nan(""), 0.5);
    CHECK(Near(c.w, 2687.0) && Near(c.h, 2879.0));
    CHECK(Near(CentreFrac(0.0, 1.0), 0.5));
    CHECK(Near(CentreFrac(-1.0, 1.0), 0.5));
    CHECK(Near(CentreFrac(1.3764, 0.8391), 1.3764 / (1.3764 + 0.8391)));
    const RectI tiny{ 0, 0, 1, 1 };
    const RectF t = CropToAspect(tiny, 1.0, 0.5, 0.5);
    CHECK(Near(t.w, 1.0) && Near(t.h, 1.0));
}

// The control: a crop that SHOULD fail the inside test does (the predicate can miss).
static void inside_test_can_fail()
{
    const RectI eye{ 0, 0, 2688, 2880 };
    CHECK(!Inside(RectF{ 0.0, 0.0, 2688.0, 2880.0 }, eye));
}

// 60 per second at 72 / 90 / 120 Hz submits: over 10 s the count is 600 (+-1), never more than one per submit.
static int CountDue(double hz, double seconds)
{
    RateGate g;
    const int64_t freq = 10000000;   // QPC-like ticks
    const int64_t period = freq / 60;
    int n = 0;
    const int frames = static_cast<int>(hz * seconds);
    for (int i = 0; i < frames; ++i) {
        const int64_t now = static_cast<int64_t>(i * (static_cast<double>(freq) / hz));
        if (g.Due(now, period)) { ++n; }
    }
    return n;
}
static void rate_sixty()
{
    const int a = CountDue(72.0, 10.0), b = CountDue(90.0, 10.0), c = CountDue(120.0, 10.0);
    std::printf("  60/s gate over 10 s: 72 Hz %d, 90 Hz %d, 120 Hz %d (expect 600-601)\n", a, b, c);
    CHECK(a >= 599 && a <= 601);
    CHECK(b >= 599 && b <= 601);
    CHECK(c >= 599 && c <= 601);
    // At 50 Hz (below the cap) every submit is due.
    CHECK(CountDue(50.0, 10.0) == 500);
}

// A long gap does not burst: after a 2 s pause the next submit is due once, the one after is not.
static void no_burst_after_pause()
{
    RateGate g;
    const int64_t period = 1000;
    CHECK(g.Due(0, period));
    CHECK(!g.Due(500, period));
    CHECK(g.Due(1000, period));
    CHECK(g.Due(100000, period));    // resynced
    CHECK(!g.Due(100100, period));
    CHECK(g.Due(101000, period));
}

int main()
{
    quest3_left_eye_16x9();
    right_half_stays_right();
    narrow_window_cuts_width();
    centre_near_edge_clamped();
    fallbacks();
    inside_test_can_fail();
    rate_sixty();
    no_burst_after_pause();
    std::printf("deskmirror_logic_test: %d failure(s)\n", g_fail);
    return g_fail;
}
