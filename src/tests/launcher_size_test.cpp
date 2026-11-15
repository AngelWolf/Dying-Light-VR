// launcher_size.h - the window size rule (spec docs/superpowers/specs/2026-11-15-launcher-design.md section 9):
// 1280 x 720 layout units x the monitor's scaling, capped at 80% of the monitor's work area, 16:9.
#include <cstdio>

#include "launcher_size.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using dlvr::launcher::WinSize;
using dlvr::launcher::WindowSize;

static void NeverFills(const WinSize& s, int workW, int workH)
{
    CHECK(s.w <= static_cast<int>(0.8 * workW + 0.5));
    CHECK(s.h <= static_cast<int>(0.8 * workH + 0.5));
    CHECK(s.w > 0 && s.h > 0);
}

int main()
{
    {   // Jay's 1440p at 100%: exactly 1280 x 720
        const WinSize s = WindowSize(1.0f, 2560, 1400);
        CHECK(s.w == 1280); CHECK(s.h == 720); CHECK(s.scale == 1.0f);
        NeverFills(s, 2560, 1400);
    }
    {   // 1080p at 100%
        const WinSize s = WindowSize(1.0f, 1920, 1040);
        CHECK(s.w == 1280); CHECK(s.h == 720);
        NeverFills(s, 1920, 1040);
    }
    {   // 1080p at 150%: height-limited to 80% of 1040 = 832, width 16:9
        const WinSize s = WindowSize(1.5f, 1920, 1040);
        CHECK(s.h == 832); CHECK(s.w == 1479);
        NeverFills(s, 1920, 1040);
    }
    {   // 4K at 200%
        const WinSize s = WindowSize(2.0f, 3840, 2100);
        CHECK(s.w == 2560); CHECK(s.h == 1440); CHECK(s.scale == 2.0f);
        NeverFills(s, 3840, 2100);
    }
    {   // 1366 x 768 laptop at 100%, work area 1366 x 728
        const WinSize s = WindowSize(1.0f, 1366, 728);
        CHECK(s.h == 582); CHECK(s.w == 1035);
        NeverFills(s, 1366, 728);
    }
    {   // a nonsense scale or work area never yields a zero or negative window
        const WinSize s = WindowSize(0.0f, 0, 0);
        CHECK(s.w > 0 && s.h > 0);
    }
    std::printf("launcher_size_test: %d failure(s)\n", g_fail);
    return g_fail;
}
