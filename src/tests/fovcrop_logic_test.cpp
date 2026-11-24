// Standalone unit test for src/ovr2xr/fovcrop_logic.h - NOT part of the DLL build.
// SteamVR panel crop (trace docs/traces/20261118_1345_steamvr_panel_crop.md). Exit code = number of failures.
#include "fovcrop_logic.h"

#include <cmath>
#include <cstdio>

using namespace dlvr::fovcrop;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

// A frustum from POSITIVE tangents (the log's "L R U D" form) in OpenXR signs.
static Fov FromTans(double l, double r, double u, double d)
{
    return Fov{ static_cast<float>(-std::atan(l)), static_cast<float>(std::atan(r)), static_cast<float>(std::atan(u)),
                static_cast<float>(-std::atan(d)) };
}

// Every cropped result: the rect lies in the viewport; the declared frustum is exactly the rect's pixel edges in the image's
// tangent mapping (same rays); it contains the panel; and it exceeds the panel by under one pixel on every side.
static void CheckCropInvariants(const Fov& image, const Fov& panel, const Rect& vp, const Result& r)
{
    CHECK(r.why == kCropped);
    CHECK(r.rect.x >= vp.x && r.rect.y >= vp.y && r.rect.w > 0 && r.rect.h > 0);
    CHECK(r.rect.x + r.rect.w <= vp.x + vp.w && r.rect.y + r.rect.h <= vp.y + vp.h);
    const double iL = std::tan(static_cast<double>(image.left)), iR = std::tan(static_cast<double>(image.right));
    const double iU = std::tan(static_cast<double>(image.up)), iD = std::tan(static_cast<double>(image.down));
    const double sx = iR - iL, sy = iU - iD;
    const double pxX = sx / vp.w, pxY = sy / vp.h;   // one pixel, in tangent units
    const int x0 = r.rect.x - vp.x, x1 = x0 + r.rect.w, y0 = r.rect.y - vp.y, y1 = y0 + r.rect.h;
    const double dL = std::tan(static_cast<double>(r.fov.left)), dR = std::tan(static_cast<double>(r.fov.right));
    const double dU = std::tan(static_cast<double>(r.fov.up)), dD = std::tan(static_cast<double>(r.fov.down));
    // same rays: the declared edge tangent IS the pixel edge's tangent (float storage: 1e-5 of a tangent << 0.01 px here)
    CHECK(std::fabs(dL - (iL + sx * x0 / vp.w)) < 1e-5);
    CHECK(std::fabs(dR - (iL + sx * x1 / vp.w)) < 1e-5);
    CHECK(std::fabs(dU - (iU - sy * y0 / vp.h)) < 1e-5);
    CHECK(std::fabs(dD - (iU - sy * y1 / vp.h)) < 1e-5);
    const double pL = std::tan(static_cast<double>(panel.left)), pR = std::tan(static_cast<double>(panel.right));
    const double pU = std::tan(static_cast<double>(panel.up)), pD = std::tan(static_cast<double>(panel.down));
    // contains the panel (to float precision), by under one pixel each side
    CHECK(dL <= pL + 1e-5 && pL - dL < pxX);
    CHECK(dR >= pR - 1e-5 && dR - pR < pxX);
    CHECK(dU >= pU - 1e-5 && dU - pU < pxY);
    CHECK(dD <= pD + 1e-5 && pD - dD < pxY);
}

// The Crystal Light player's own numbers (backups/20261118_pimax_canted_tester_logs: HmdDesc given L=R 1.2641 U=D 1.4957,
// runtime eye0 L1.2641 R0.8895 U/D 1.2743, viewports L=(0,0 3536x4184) R=(3536,0 3536x4184)).
static void crystal_light_both_eyes()
{
    const Fov image = FromTans(1.2641, 1.2641, 1.4957, 1.4957);
    const Fov p0 = FromTans(1.2641, 0.8895, 1.2743, 1.2743);
    const Fov p1 = FromTans(0.8895, 1.2641, 1.2743, 1.2743);
    const Rect v0{ 0, 0, 3536, 4184 }, v1{ 3536, 0, 3536, 4184 };
    const Result r0 = Crop(image, p0, v0);
    const Result r1 = Crop(image, p1, v1);
    CheckCropInvariants(image, p0, v0, r0);
    CheckCropInvariants(image, p1, v1, r1);
    // left eye keeps its outer (left) edge, loses ~524 px on the inner side; right eye mirrored
    CHECK(r0.rect.x == 0);
    CHECK(r0.rect.w >= 3010 && r0.rect.w <= 3014);
    CHECK(r1.rect.x + r1.rect.w == 3536 + 3536);
    CHECK(r1.rect.w == r0.rect.w);
    CHECK(r0.rect.y == r1.rect.y && r0.rect.h == r1.rect.h);
    CHECK(r0.rect.y >= 308 && r0.rect.y <= 311);
    CHECK(r0.rect.h >= 3563 && r0.rect.h <= 3568);
    std::printf("crystal light: eye0 rect (%d,%d %dx%d) eye1 rect (%d,%d %dx%d)\n", r0.rect.x, r0.rect.y, r0.rect.w, r0.rect.h,
                r1.rect.x, r1.rect.y, r1.rect.w, r1.rect.h);
}

// Jay's Quest 3 (INV-612 / CV1: runtime L1.3764 R0.8391 U0.9657 D1.4281, given L=R 1.3764 U=D 1.4747): off-centre on BOTH axes.
static void quest3_offcentre_both_axes()
{
    const Fov image = FromTans(1.3764, 1.3764, 1.4747, 1.4747);
    const Fov p0 = FromTans(1.3764, 0.8391, 0.9657, 1.4281);
    const Rect v0{ 0, 0, 2688, 2880 };
    const Result r0 = Crop(image, p0, v0);
    CheckCropInvariants(image, p0, v0, r0);
    CHECK(r0.rect.x == 0);
    CHECK(r0.rect.y > 0 && r0.rect.y + r0.rect.h <= 2880);
}

static void whole_window_is_not_cut()
{
    const Fov f = FromTans(1.2, 1.2, 1.3, 1.3);
    const Rect v{ 0, 0, 2000, 2200 };
    const Result r = Crop(f, f, v);
    CHECK(r.why == kWhole);
    CHECK(r.rect.x == 0 && r.rect.y == 0 && r.rect.w == 2000 && r.rect.h == 2200);
    CHECK(r.fov.left == f.left && r.fov.right == f.right && r.fov.up == f.up && r.fov.down == f.down);
}

static void panel_past_the_image_is_not_cut()
{
    const Fov image = FromTans(1.0, 1.0, 1.0, 1.0);
    const Rect v{ 0, 0, 1000, 1000 };
    CHECK(Crop(image, FromTans(1.2, 0.8, 0.9, 0.9), v).why == kOutside);   // left
    CHECK(Crop(image, FromTans(0.8, 1.2, 0.9, 0.9), v).why == kOutside);   // right
    CHECK(Crop(image, FromTans(0.8, 0.8, 1.2, 0.9), v).why == kOutside);   // up
    CHECK(Crop(image, FromTans(0.8, 0.8, 0.9, 1.2), v).why == kOutside);   // down
    const Result r = Crop(image, FromTans(1.2, 0.8, 0.9, 0.9), v);
    CHECK(r.rect.w == 1000 && r.fov.left == image.left);                  // untouched
}

// A panel edge past the image by under half a pixel (float noise) is still cut, clamped at the image edge.
static void subpixel_overhang_is_clamped()
{
    const Fov image = FromTans(1.0, 1.0, 1.0, 1.0);
    const Rect v{ 0, 0, 1000, 1000 };
    const double half = 0.3 * 2.0 / 1000.0;   // 0.3 px in tangent units
    const Result r = Crop(image, FromTans(1.0 + half, 0.5, 0.9, 0.9), v);
    CHECK(r.why == kCropped);
    CHECK(r.rect.x == 0);
    CHECK(r.fov.left == static_cast<float>(std::atan(-1.0)) || std::fabs(r.fov.left - static_cast<float>(std::atan(-1.0))) < 1e-6f);
}

static void bad_input_is_not_cut()
{
    const Fov image = FromTans(1.0, 1.0, 1.0, 1.0);
    CHECK(Crop(image, FromTans(0.8, 0.8, 0.8, 0.8), Rect{ 0, 0, 0, 100 }).why == kBadInput);
    CHECK(Crop(image, FromTans(0.8, 0.8, 0.8, 0.8), Rect{ 0, 0, 100, -1 }).why == kBadInput);
    Fov nanFov = image;
    nanFov.left = std::nanf("");
    CHECK(Crop(nanFov, FromTans(0.8, 0.8, 0.8, 0.8), Rect{ 0, 0, 100, 100 }).why == kBadInput);
    CHECK(Crop(image, nanFov, Rect{ 0, 0, 100, 100 }).why == kBadInput);
    const Fov inverted{ image.right, image.left, image.up, image.down };
    CHECK(Crop(inverted, FromTans(0.8, 0.8, 0.8, 0.8), Rect{ 0, 0, 100, 100 }).why == kBadInput);
}

// The viewport offset is carried: a right-eye half of a shared texture.
static void viewport_offset_is_carried()
{
    const Fov image = FromTans(1.0, 1.0, 1.0, 1.0);
    const Rect v{ 700, 50, 1000, 1000 };
    const Result r = Crop(image, FromTans(0.5, 0.5, 0.5, 0.5), v);
    CheckCropInvariants(image, FromTans(0.5, 0.5, 0.5, 0.5), v, r);
    CHECK(r.rect.x == 700 + 250 && r.rect.y == 50 + 250 && r.rect.w == 500 && r.rect.h == 500);
}

// META'S RUNTIME (trace docs/traces/20261124_1550_meta_panel_crop.md): it spreads the submitted rectangle over its own FOV. Where a
// direction lands = the panel tangent at that fraction of the rectangle. Straight ahead (tangent 0) must land on 0 in both eyes.
static double ShownForwardDeg(const Fov& declared, const Fov& panel)
{
    const double dL = std::tan(static_cast<double>(declared.left)), dR = std::tan(static_cast<double>(declared.right));
    const double pL = std::tan(static_cast<double>(panel.left)), pR = std::tan(static_cast<double>(panel.right));
    const double u = (0.0 - dL) / (dR - dL);
    return std::atan(pL + u * (pR - pL)) * 57.29577951308232;
}

// The Meta tester's own numbers (2026-11-23 20:34: runtime L1.3076 R0.7971 U0.9174 D1.3567, 1968x2160 per eye): symmetrised + grown to
// the texture's shape, then cut: the window is the panel to under a pixel and straight ahead stays straight ahead in both eyes.
static void meta_tester_both_eyes()
{
    double h = 1.3076, v = 1.3567;
    const double aspect = 1968.0 / 2160.0;
    if (h / v > aspect) { v = h / aspect; } else { h = v * aspect; }
    const Fov image = FromTans(h, h, v, v);
    const Fov p0 = FromTans(1.3076, 0.7971, 0.9174, 1.3567), p1 = FromTans(0.7971, 1.3076, 0.9174, 1.3567);
    const Rect v0{ 0, 0, 1968, 2160 }, v1{ 1968, 0, 1968, 2160 };
    const Result r0 = CropScaled(image, p0, v0), r1 = CropScaled(image, p1, v1);
    CheckCropInvariants(image, p0, v0, r0);
    CheckCropInvariants(image, p1, v1, r1);
    CHECK(r0.rect.w == r1.rect.w && r0.rect.h == r1.rect.h && r0.rect.y == r1.rect.y);
    CHECK(std::fabs(ShownForwardDeg(r0.fov, p0)) < 0.05);
    CHECK(std::fabs(ShownForwardDeg(r1.fov, p1)) < 0.05);
    std::printf("meta tester: eye0 rect (%d,%d %dx%d) eye1 rect (%d,%d %dx%d), forward %.4f / %.4f deg\n", r0.rect.x, r0.rect.y, r0.rect.w,
                r0.rect.h, r1.rect.x, r1.rect.y, r1.rect.w, r1.rect.h, ShownForwardDeg(r0.fov, p0), ShownForwardDeg(r1.fov, p1));
    // the control: the UNCUT symmetric image (the 2026-11-16 cross-eye) lands far off, opposite ways
    CHECK(ShownForwardDeg(image, p0) < -10.0 && ShownForwardDeg(image, p1) > 10.0);
}

// The first-launch table row (Quest 3: L1.38 R1.01 U1.12 D1.43), symmetrised: contains both FOVs Meta reported, so it is plainly cut.
static void meta_first_launch_row()
{
    const Fov image = FromTans(1.38, 1.38, 1.43, 1.43);
    const Rect v{ 0, 0, 2064, 2272 };
    const Fov big = FromTans(1.3764, 0.8391, 0.9657, 1.4281), small = FromTans(1.3076, 0.7971, 0.9174, 1.3567);
    const Result a = CropScaled(image, big, v), b = CropScaled(image, small, v);
    CheckCropInvariants(image, big, v, a);
    CheckCropInvariants(image, small, v, b);
    CHECK(std::fabs(ShownForwardDeg(a.fov, big)) < 0.05 && std::fabs(ShownForwardDeg(b.fov, small)) < 0.05);
}

// A cache smaller than the live FOV (1.3076 / 1.3567 cached, 1.3764 / 1.4281 live): the panel reaches past the image. Scaled: the window is
// the panel shrunk uniformly about straight ahead - inside the image, the panel's own proportions, forward still forward.
static void meta_cache_too_small_is_zoomed()
{
    const Fov image = FromTans(1.3076, 1.3076, 1.4352, 1.4352);
    const Rect v0{ 0, 0, 1968, 2160 }, v1{ 1968, 0, 1968, 2160 };
    const Fov p0 = FromTans(1.3764, 0.8391, 0.9657, 1.4281), p1 = FromTans(0.8391, 1.3764, 0.9657, 1.4281);
    CHECK(Crop(image, p0, v0).why == kOutside);   // SteamVR's path: still not cut
    const Result r0 = CropScaled(image, p0, v0), r1 = CropScaled(image, p1, v1);
    CHECK(r0.why == kScaled && r1.why == kScaled);
    CHECK(r0.rect.x == 0 && r1.rect.x + r1.rect.w == 1968 + 1968);       // the limiting (outer) edge is the image's own
    CHECK(r0.rect.w == r1.rect.w && r0.rect.w < 1968);
    CHECK(std::fabs(ShownForwardDeg(r0.fov, p0)) < 0.05 && std::fabs(ShownForwardDeg(r1.fov, p1)) < 0.05);
    const double k = 1.3076 / 1.3764;
    CHECK(std::fabs(std::tan(static_cast<double>(r0.fov.right)) - k * 0.8391) < 0.002);
    CHECK(std::fabs(std::tan(static_cast<double>(r0.fov.up)) - k * 0.9657) < 0.002);
    CHECK(std::fabs(-std::tan(static_cast<double>(r0.fov.down)) - k * 1.4281) < 0.002);
}

// A panel that does not contain straight ahead cannot be shrunk about it: refused, never a wrong window.
static void scaled_refuses_a_panel_without_forward()
{
    const Fov image = FromTans(1.0, 1.0, 1.0, 1.0);
    const Rect v{ 0, 0, 1000, 1000 };
    CHECK(CropScaled(image, FromTans(-0.2, 1.4, 0.9, 0.9), v).why == kOutside);
    CHECK(CropScaled(image, FromTans(0.8, 0.8, 0.8, 0.8), Rect{ 0, 0, 0, 100 }).why == kBadInput);
    const Result w = CropScaled(image, image, v);
    CHECK(w.why == kWhole);
    CHECK(CropScaled(image, FromTans(2.0, 2.0, 2.0, 2.0), v).why == kScaled);   // all four past: the whole image, reported as scaled
}

int main()
{
    meta_tester_both_eyes();
    meta_first_launch_row();
    meta_cache_too_small_is_zoomed();
    scaled_refuses_a_panel_without_forward();
    crystal_light_both_eyes();
    quest3_offcentre_both_axes();
    whole_window_is_not_cut();
    panel_past_the_image_is_not_cut();
    subpixel_overhang_is_clamped();
    bad_input_is_not_cut();
    viewport_offset_is_carried();
    std::printf("fovcrop_logic_test: %d failure(s)\n", g_fail);
    return g_fail;
}
