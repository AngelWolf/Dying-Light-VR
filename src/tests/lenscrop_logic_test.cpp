// Standalone unit test for src/src/common/lenscrop_logic.h - NOT part of the DLL build.
// The DLSS / FSR lens crop's row band (docs/superpowers/specs/2026-11-11-dlss-lens-crop-design.md §4.1).
// Exit code = number of failures.
#include "lenscrop_logic.h"

#include <cmath>
#include <cstdio>
#include <limits>

using namespace dlvr::lenscrop;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static const double kDeg = 3.14159265358979323846 / 180.0;

// The lens mask's own Visible() (bootstrap/dlssflat.cpp, namespace lensmask) - the predicate the crop must contain - applied to
// one row's centre over 257 columns. true = some texel of this row can be displayed.
static bool RefVisibleRow(const float g[12], float v)
{
    const float c = g[8], s = g[9];
    const float ty = g[2] + (-g[3] - g[2]) * v;   // lerp(img.z, -img.w, uv.y)
    for (int i = 0; i <= 256; ++i) {
        const float u = static_cast<float>(i) / 256.0f;
        const float tx = -g[0] + (g[1] + g[0]) * u;   // lerp(-img.x, img.y, uv.x)
        const float y2 = ty * c + s;
        const float z2 = ty * s - c;
        if (z2 >= 0.0f) { continue; }
        const float hx = tx / -z2, vy = y2 / -z2;
        if (hx >= -g[4] && hx <= g[5] && vy >= -g[7] && vy <= g[6]) { return true; }
    }
    return false;
}

// BK12 (2026-11-11, layer log): eye0 runtime L 1.3764 R 0.8391 U 0.9657 D 1.4281, rendered L = R = 1.3764, U = D = 1.4747;
// declared pitch = the runtime's vertical bisector (ovr2xr.cpp: (up + down) / 2 = (44.0 - 55.0) / 2 = -5.5 deg).
static void Bk12(float g[12], double marginDeg)
{
    const double m = marginDeg * kDeg;
    g[0] = 1.3764f; g[1] = 1.3764f; g[2] = 1.4747f; g[3] = 1.4747f;
    g[4] = static_cast<float>(std::tan(std::atan(1.3764) + m));
    g[5] = static_cast<float>(std::tan(std::atan(0.8391) + m));
    g[6] = static_cast<float>(std::tan(std::atan(0.9657) + m));
    g[7] = static_cast<float>(std::tan(std::atan(1.4281) + m));
    const double p = -5.5 * kDeg;
    g[8] = static_cast<float>(std::cos(p)); g[9] = static_cast<float>(std::sin(p));
    g[10] = static_cast<float>(marginDeg); g[11] = -5.5f;
}

static void bk12_quest3_rows_match_reference()
{
    float g[12];
    Bk12(g, 3.0);
    Band b{};
    CHECK(VisibleRows(g, &b));
    const int n = 4096;
    int outside = 0;
    for (int r = 0; r < n; ++r) {
        const float v = (static_cast<float>(r) + 0.5f) / static_cast<float>(n);
        if (RefVisibleRow(g, v) && (v < b.v0 - 1.0f / n || v > b.v1 + 1.0f / n)) { ++outside; }
    }
    CHECK(outside == 0);
    CHECK(!RefVisibleRow(g, b.v0 - 2.0f / n));   // the band is tight: just beyond each edge is not displayable
    CHECK(!RefVisibleRow(g, b.v1 + 2.0f / n));
    const float cropped = 1.0f - (b.v1 - b.v0);
    CHECK(cropped > 0.09f && cropped < 0.14f);   // design §6: ~11-12 %
    std::printf("bk12: v0 %.5f v1 %.5f cropped %.4f\n", b.v0, b.v1, cropped);
}

// Negative control: the containment check above must be able to FAIL - a band shrunk by 0.01 at each end leaves rows out.
static void shrunk_band_fails_containment()
{
    float g[12];
    Bk12(g, 3.0);
    Band b{};
    CHECK(VisibleRows(g, &b));
    const Band s{ b.v0 + 0.01f, b.v1 - 0.01f };
    int outside = 0;
    for (int r = 0; r < 4096; ++r) {
        const float v = (static_cast<float>(r) + 0.5f) / 4096.0f;
        if (RefVisibleRow(g, v) && (v < s.v0 || v > s.v1)) { ++outside; }
    }
    CHECK(outside > 0);
}

static void refuses_without_geometry()
{
    float g[12] = {};
    Band b{};
    CHECK(!VisibleRows(g, &b));
    Bk12(g, 3.0);
    g[6] = std::numeric_limits<float>::quiet_NaN();
    CHECK(!VisibleRows(g, &b));
    Bk12(g, 3.0);
    g[2] = -1.0f;
    CHECK(!VisibleRows(g, &b));
    Bk12(g, 3.0);
    g[8] = 0.0f; g[9] = 0.0f;   // not a rotation
    CHECK(!VisibleRows(g, &b));
    Bk12(g, 3.0);
    CHECK(!VisibleRows(g, nullptr));
}

static void wide_margin_crops_nothing()
{
    float g[12];
    Bk12(g, 15.0);   // the S_FLOAT maximum
    Band b{};
    const bool ok = VisibleRows(g, &b);
    CHECK(!ok || (b.v0 == 0.0f && b.v1 == 1.0f));
    if (ok) {
        Rect in{}, out{};
        CHECK(!SnapRows(b, 2688, 2880, 2688, 2880, &in, &out));
    }
}

static void union_keeps_both()
{
    const Band u = Union(Band{ 0.06f, 0.94f }, Band{ 0.04f, 0.95f });
    CHECK(u.v0 == 0.04f && u.v1 == 0.95f);
}

// The SnapRows contract (plan Task 1): in within render, out within output, out contains the band, rows consistent with the ratio.
static bool Contract(const Band& b, uint32_t rW, uint32_t rH, uint32_t oW, uint32_t oH, const Rect& in, const Rect& out)
{
    const double s = static_cast<double>(oH) / static_cast<double>(rH);
    bool ok = true;
    ok = ok && in.x == 0 && in.w == rW && out.x == 0 && out.w == oW;
    ok = ok && in.y + in.h <= rH && out.y + out.h <= oH && in.h > 0 && out.h > 0;
    ok = ok && static_cast<double>(out.y) <= b.v0 * oH && static_cast<double>(out.y + out.h) >= b.v1 * oH;
    ok = ok && std::fabs(static_cast<double>(out.y) - in.y * s) <= 1.0;
    ok = ok && std::fabs(static_cast<double>(out.h) - in.h * s) <= 2.0;
    return ok;
}

static void snap_dlaa_exact()
{
    float g[12];
    Bk12(g, 3.0);
    Band b{};
    CHECK(VisibleRows(g, &b));
    Rect in{}, out{};
    CHECK(SnapRows(b, 2688, 2880, 2688, 2880, &in, &out));
    CHECK(SameRect(in, out));
    CHECK(Contract(b, 2688, 2880, 2688, 2880, in, out));
    std::printf("dlaa: in y %u h %u | out y %u h %u\n", in.y, in.h, out.y, out.h);
}

static void snap_quality_ratio()
{
    float g[12];
    Bk12(g, 3.0);
    Band b{};
    CHECK(VisibleRows(g, &b));
    Rect in{}, out{};
    CHECK(SnapRows(b, 1792, 1920, 2688, 2880, &in, &out));
    CHECK(Contract(b, 1792, 1920, 2688, 2880, in, out));
    std::printf("quality: in y %u h %u | out y %u h %u\n", in.y, in.h, out.y, out.h);
}

static void snap_balanced_ratio()
{
    float g[12];
    Bk12(g, 3.0);
    Band b{};
    CHECK(VisibleRows(g, &b));
    Rect in{}, out{};
    CHECK(SnapRows(b, 1563, 1674, 2688, 2880, &in, &out));
    CHECK(Contract(b, 1563, 1674, 2688, 2880, in, out));
    std::printf("balanced: in y %u h %u | out y %u h %u\n", in.y, in.h, out.y, out.h);
}

static void snap_refuses_zero_size()
{
    Rect in{}, out{};
    CHECK(!SnapRows(Band{ 0.06f, 0.94f }, 0, 2880, 2688, 2880, &in, &out));
    CHECK(!SnapRows(Band{ 0.06f, 0.94f }, 2688, 2880, 2688, 0, &in, &out));
}

static void same_rect()
{
    CHECK(SameRect(Rect{ 0, 160, 2688, 2560 }, Rect{ 0, 160, 2688, 2560 }));
    CHECK(!SameRect(Rect{ 0, 160, 2688, 2560 }, Rect{ 0, 161, 2688, 2560 }));
    CHECK(!SameRect(Rect{ 0, 160, 2688, 2560 }, Rect{ 0, 160, 2688, 2559 }));
}

static void share()
{
    CHECK(std::fabs(CroppedShare(Rect{ 0, 160, 2688, 2560 }, 2688, 2880) - (1.0 - 2560.0 / 2880.0)) < 1e-9);
}

int main()
{
    bk12_quest3_rows_match_reference();
    shrunk_band_fails_containment();
    refuses_without_geometry();
    wide_margin_crops_nothing();
    union_keeps_both();
    snap_dlaa_exact();
    snap_quality_ratio();
    snap_balanced_ratio();
    snap_refuses_zero_size();
    same_rect();
    share();
    std::printf("lenscrop_logic: %d failure(s)\n", g_fail);
    return g_fail;
}
