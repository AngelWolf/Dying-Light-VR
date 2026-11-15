// Standalone unit test for src/src/common/foveation_logic.h - NOT part of the DLL build.
// Fixed foveated DLSS / FSR geometry (docs/superpowers/specs/2026-11-11-foveated-dlss-design.md §4.2 / §4.5,
// plan docs/superpowers/plans/2026-11-11-foveated-dlss.md Task 1). Exit code = number of failures.
#include "foveation_logic.h"

#include <cmath>
#include <cstdio>
#include <limits>

using namespace dlvr;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static const double kDeg = 3.14159265358979323846 / 180.0;

// BK12 (2026-11-11, layer log), as lenscrop_logic_test.cpp's Bk12: eye0 runtime L 1.3764 R 0.8391 U 0.9657 D 1.4281, rendered
// L = R = 1.3764, U = D = 1.4747, declared pitch -5.5 deg. eye 1 = the runtime's L / R swapped (the mirror image).
static void Bk12(float g[12], double marginDeg, int eye)
{
    const double m = marginDeg * kDeg;
    const double L = eye == 0 ? 1.3764 : 0.8391, R = eye == 0 ? 0.8391 : 1.3764;
    g[0] = 1.3764f; g[1] = 1.3764f; g[2] = 1.4747f; g[3] = 1.4747f;
    g[4] = static_cast<float>(std::tan(std::atan(L) + m));
    g[5] = static_cast<float>(std::tan(std::atan(R) + m));
    g[6] = static_cast<float>(std::tan(std::atan(0.9657) + m));
    g[7] = static_cast<float>(std::tan(std::atan(1.4281) + m));
    const double p = -5.5 * kDeg;
    g[8] = static_cast<float>(std::cos(p)); g[9] = static_cast<float>(std::sin(p));
    g[10] = static_cast<float>(marginDeg); g[11] = -5.5f;
}

static fov::Params Defaults()
{
    fov::Params p{};
    p.width = 0.55f; p.height = 0.45f; p.heightOffset = 0.0f; p.roundness = 0.0f; p.transition = 0.040f;
    p.supersample = 1.0f; p.peripheryScale = 0.75f; p.peripheryPreset = 5; p.periphery = true; p.border = false;
    return p;
}

// The lens mask's own Visible() (bootstrap/dlssflat.cpp, namespace lensmask) mapping of uv -> the eye's frame, (hx, vy). Straight
// ahead is hx = vy = 0, so the reference for LensCentre is: Visible()'s mapping of (u, v) lands on (0, 0).
static void RefEyeTangent(const float g[12], float u, float v, float* hx, float* vy)
{
    const float c = g[8], s = g[9];
    const float tx = -g[0] + (g[1] + g[0]) * u;
    const float ty = g[2] + (-g[3] - g[2]) * v;
    const float y2 = ty * c + s, z2 = ty * s - c;
    *hx = tx / -z2;
    *vy = y2 / -z2;
}

// Straight ahead (Jay, 2026-11-11). BK12's image is symmetric left / right (L = R = 1.3764), so u = 0.5 in both eyes; the declared
// pitch is -5.5 deg (the view is pitched down), so the eye's forward sits ABOVE the image's middle: v = (1.4747 - tan 5.5) / 2.9494
// = 0.4673.
static void bk12_straight_ahead()
{
    float g0[12], g1[12];
    Bk12(g0, 0.0, 0);
    Bk12(g1, 0.0, 1);
    float u0 = -1, v0 = -1, u1 = -1, v1 = -1;
    CHECK(fov::LensCentre(g0, &u0, &v0));
    CHECK(fov::LensCentre(g1, &u1, &v1));
    CHECK(std::fabs(u0 - 0.5f) < 1e-5f && std::fabs(u1 - 0.5f) < 1e-5f);
    const double vRef = (1.4747 - std::tan(5.5 * kDeg)) / (2.0 * 1.4747);
    CHECK(std::fabs(v0 - vRef) < 1e-4 && std::fabs(v1 - vRef) < 1e-4);
    CHECK(v0 < 0.48f);
    float hx = 1, vy = 1;
    RefEyeTangent(g0, u0, v0, &hx, &vy);
    CHECK(std::fabs(hx) < 1e-4f && std::fabs(vy) < 1e-4f);
    // an off-centre image (L 1.2, R 1.5): forward is still hx = 0 in the mask's own mapping
    g0[0] = 1.2f; g0[1] = 1.5f;
    CHECK(fov::LensCentre(g0, &u0, &v0));
    RefEyeTangent(g0, u0, v0, &hx, &vy);
    CHECK(std::fabs(hx) < 1e-4f && std::fabs(vy) < 1e-4f);
    CHECK(std::fabs(u0 - 1.2f / 2.7f) < 1e-5f);
    std::printf("bk12 straight ahead: eye0 u %.5f v %.5f | eye1 u %.5f v %.5f\n", 0.5, vRef, u1, v1);
}

// The horizontal offset: + = toward the temple, mirrored per eye (0 = LEFT -> image left); 0 = no shift; the height offset alike in
// both eyes, + = down. Shift = offset * (1 - size) / 2 (Cheeky's unit).
static void fovea_centre_offsets()
{
    fov::Params p = Defaults();
    float cu = 0, cv = 0;
    fov::FoveaCentre(0.5f, 0.467f, 0, p, &cu, &cv);
    CHECK(cu == 0.5f && cv == 0.467f);
    p.xOffset = 0.4f;
    float cu0 = 0, cv0 = 0, cu1 = 0, cv1 = 0;
    fov::FoveaCentre(0.5f, 0.467f, 0, p, &cu0, &cv0);
    fov::FoveaCentre(0.5f, 0.467f, 1, p, &cu1, &cv1);
    const float shift = 0.4f * (1.0f - 0.55f) / 2.0f;
    CHECK(std::fabs(cu0 - (0.5f - shift)) < 1e-6f);   // left eye: toward the image's left
    CHECK(std::fabs(cu1 - (0.5f + shift)) < 1e-6f);   // right eye: toward the image's right
    CHECK(cv0 == 0.467f && cv1 == 0.467f);
    p.xOffset = 0.0f;
    p.heightOffset = 0.5f;
    fov::FoveaCentre(0.5f, 0.467f, 0, p, &cu0, &cv0);
    fov::FoveaCentre(0.5f, 0.467f, 1, p, &cu1, &cv1);
    CHECK(std::fabs(cv0 - (0.467f + 0.5f * (1.0f - 0.45f) / 2.0f)) < 1e-6f && cv0 == cv1);
    CHECK(cu0 == 0.5f && cu1 == 0.5f);
}

static void lens_centre_refuses_bad_geometry()
{
    float g[12] = {};
    float u = 0, v = 0;
    CHECK(!fov::LensCentre(g, &u, &v));
    Bk12(g, 0.0, 0);
    g[4] = std::numeric_limits<float>::quiet_NaN();
    CHECK(!fov::LensCentre(g, &u, &v));
    Bk12(g, 0.0, 0);
    CHECK(!fov::LensCentre(g, nullptr, &v));
    CHECK(!fov::LensCentre(nullptr, &u, &v));
}

static bool Within(const lenscrop::Rect& r, uint32_t W, uint32_t H)
{
    return r.w > 0 && r.h > 0 && r.x + r.w <= W && r.y + r.h <= H;
}

static void fovea_rects_contract()
{
    float g[12];
    Bk12(g, 0.0, 0);
    float u = 0, v = 0;
    CHECK(fov::LensCentre(g, &u, &v));
    const fov::Params p = Defaults();
    lenscrop::Rect in{}, out{};
    uint32_t pw = 0, ph = 0;
    CHECK(fov::FoveaRects(u, v, p, 1792, 1920, 2688, 2880, &in, &out, &pw, &ph));
    CHECK(Within(in, 1792, 1920));
    CHECK(Within(out, 2688, 2880));
    // the fovea's uv box, clipped to the image, lies inside out
    const double bx0 = std::fmax(0.0, u - p.width / 2.0), bx1 = std::fmin(1.0, u + p.width / 2.0);
    const double by0 = std::fmax(0.0, v - p.height / 2.0), by1 = std::fmin(1.0, v + p.height / 2.0);
    CHECK(out.x <= bx0 * 2688 && out.x + out.w >= bx1 * 2688);
    CHECK(out.y <= by0 * 2880 && out.y + out.h >= by1 * 2880);
    CHECK(std::fabs(static_cast<double>(out.x) - in.x * 1.5) <= 1.0);
    CHECK(std::fabs(static_cast<double>(out.y) - in.y * 1.5) <= 1.0);
    CHECK(std::fabs(static_cast<double>(out.w) - in.w * 1.5) <= 2.0);
    CHECK(std::fabs(static_cast<double>(out.h) - in.h * 1.5) <= 2.0);
    CHECK(pw == out.w && ph == out.h);
    std::printf("quality: in %u,%u %ux%u | out %u,%u %ux%u | priv %ux%u\n", in.x, in.y, in.w, in.h, out.x, out.y, out.w, out.h, pw, ph);
}

static void offsets_move_rects()
{
    fov::Params p = Defaults();
    lenscrop::Rect in0{}, out0{}, in1{}, out1{};
    uint32_t pw = 0, ph = 0;
    float cu = 0, cv = 0;
    fov::FoveaCentre(0.5f, 0.5f, 0, p, &cu, &cv);
    CHECK(fov::FoveaRects(cu, cv, p, 1792, 1920, 2688, 2880, &in0, &out0, &pw, &ph));
    p.heightOffset = 0.5f;   // Cheeky: centered.y -= offset * (1 - height) -> the shape moves DOWN by offset * (1 - height) / 2 in uv
    fov::FoveaCentre(0.5f, 0.5f, 0, p, &cu, &cv);
    CHECK(fov::FoveaRects(cu, cv, p, 1792, 1920, 2688, 2880, &in1, &out1, &pw, &ph));
    const double expectY = 0.5 * (1.0 - 0.45) / 2.0 * 1920.0;
    CHECK(std::fabs((static_cast<double>(in1.y) - in0.y) - expectY) <= 1.0);
    CHECK(in1.x == in0.x && in1.w == in0.w);
    p.heightOffset = 0.0f;
    p.xOffset = 0.5f;        // left eye: toward the temple = the image's left
    fov::FoveaCentre(0.5f, 0.5f, 0, p, &cu, &cv);
    CHECK(fov::FoveaRects(cu, cv, p, 1792, 1920, 2688, 2880, &in1, &out1, &pw, &ph));
    const double expectX = 0.5 * (1.0 - 0.55) / 2.0 * 1792.0;
    CHECK(std::fabs((static_cast<double>(in0.x) - in1.x) - expectX) <= 1.0);
    CHECK(in1.y == in0.y && in1.h == in0.h);
}

static void supersample_scales_private_size()
{
    fov::Params p = Defaults();
    p.supersample = 2.0f;
    lenscrop::Rect in{}, out{};
    uint32_t pw = 0, ph = 0;
    CHECK(fov::FoveaRects(0.4f, 0.5f, p, 1792, 1920, 2688, 2880, &in, &out, &pw, &ph));
    CHECK(pw == 2 * out.w && ph == 2 * out.h);
}

static void fovea_clamped_to_image()
{
    fov::Params p = Defaults();
    p.width = 1.0f; p.height = 1.0f;
    lenscrop::Rect in{}, out{};
    uint32_t pw = 0, ph = 0;
    CHECK(fov::FoveaRects(0.6f, 0.5f, p, 1792, 1920, 2688, 2880, &in, &out, &pw, &ph));
    CHECK(in.x == 0 && in.y == 0 && in.w == 1792 && in.h == 1920);
    CHECK(out.x == 0 && out.y == 0 && out.w == 2688 && out.h == 2880);
    // a smaller fovea near the edge: shifted inside, never past it, size kept
    p.width = 0.55f; p.height = 0.45f;
    CHECK(fov::FoveaRects(0.98f, 0.02f, p, 1792, 1920, 2688, 2880, &in, &out, &pw, &ph));
    CHECK(Within(in, 1792, 1920) && Within(out, 2688, 2880));
    CHECK(in.x + in.w == 1792 && in.y == 0);
    CHECK(std::fabs(static_cast<double>(in.w) - 0.55 * 1792) <= 2.0);
    // bad input refuses
    CHECK(!fov::FoveaRects(0.5f, 0.5f, p, 0, 1920, 2688, 2880, &in, &out, &pw, &ph));
    CHECK(!fov::FoveaRects(std::numeric_limits<float>::quiet_NaN(), 0.5f, p, 1792, 1920, 2688, 2880, &in, &out, &pw, &ph));
    CHECK(!fov::FoveaRects(0.5f, 0.5f, p, 1792, 1920, 2688, 2880, nullptr, &out, &pw, &ph));
}

static void private_size_capped()
{
    fov::Params p = Defaults();
    p.width = 1.0f; p.height = 1.0f; p.supersample = 2.0f;
    lenscrop::Rect in{}, out{};
    uint32_t pw = 0, ph = 0;
    CHECK(fov::FoveaRects(0.5f, 0.5f, p, 6000, 6000, 10000, 10000, &in, &out, &pw, &ph));
    CHECK(pw == 16384 && ph == 16384);
}

static void working_size()
{
    uint32_t w = 0, h = 0;
    fov::WorkingSize(1792, 1920, 0.75f, &w, &h);
    CHECK(w == 1344 && h == 1440);
    fov::WorkingSize(1792, 1920, 0.20f, &w, &h);
    CHECK(w == 358 && h == 384);
    fov::WorkingSize(1792, 1920, 0.001f, &w, &h);
    CHECK(w == 32 && h == 32);
}

static void composite_weight()
{
    const fov::Params p = Defaults();
    const uint32_t W = 2688, H = 2880;
    const float u = 0.4f, v = 0.5f;
    const float cx = u * W - 0.5f, cy = v * H - 0.5f;   // pixel whose centre is the shape's centre
    CHECK(std::fabs(fov::CompositeWeight(cx, cy, W, H, u, v, p) - 1.0f) < 1e-6f);
    CHECK(fov::CompositeWeight(5.0f, 5.0f, W, H, u, v, p) == 0.0f);                          // a corner, far outside
    CHECK(fov::CompositeWeight(cx + 0.30f * W, cy, W, H, u, v, p) == 0.0f);                 // past the half width (0.275)
    // along a ray outward the weight never increases, and it crosses from 1 to 0 within the feather
    float prev = 2.0f;
    bool sawPartial = false;
    for (int i = 0; i <= 2000; ++i) {
        const float x = cx + 0.30f * W * static_cast<float>(i) / 2000.0f;
        const float wgt = fov::CompositeWeight(x, cy, W, H, u, v, p);
        CHECK(wgt <= prev + 1e-7f);
        CHECK(wgt >= 0.0f && wgt <= 1.0f);
        if (wgt > 0.0f && wgt < 1.0f) { sawPartial = true; }
        prev = wgt;
    }
    CHECK(sawPartial);
    // transition 0 = a hard step: no partial weight anywhere on the ray
    fov::Params hard = p;
    hard.transition = 0.0f;
    bool partial = false;
    for (int i = 0; i <= 2000; ++i) {
        const float x = cx + 0.30f * W * static_cast<float>(i) / 2000.0f;
        const float wgt = fov::CompositeWeight(x, cy, W, H, u, v, hard);
        if (wgt != 0.0f && wgt != 1.0f) { partial = true; }
    }
    CHECK(!partial);
    // Cheeky's exact numbers at one point: d = |centered.x| / width with centered.x = (px + 0.5) / (W / 2) - 1 - (2u - 1);
    // weight = 1 - smoothstep(1 - transition / min(w, h), 1, d)
    const float px = cx + 0.27f * W;
    const double d = std::fabs((px + 0.5) / (0.5 * W) - 1.0 - (2.0 * u - 1.0)) / 0.55;
    const double a = 1.0 - 0.040 / 0.45;
    double t = (d - a) / (1.0 - a);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    const double ref = 1.0 - t * t * (3.0 - 2.0 * t);
    CHECK(std::fabs(fov::CompositeWeight(px, cy, W, H, u, v, p) - ref) < 1e-5);
}

// The composite's shape and FoveaRects' box must share one centre: with both offsets (right eye), the weight is 1 at the box's
// centre and 0 just inside the UNshifted box's top edge and its temple-opposite (left) edge - each shift (0.1375 of the height,
// 0.1125 of the width) exceeds the feather.
static void composite_follows_rect_offset()
{
    fov::Params p = Defaults();
    p.heightOffset = 0.5f;
    p.xOffset = 0.5f;
    const uint32_t W = 2688, H = 2880;
    const float u = 0.5f, v = 0.467f;
    float cu = 0, cv = 0;
    fov::FoveaCentre(u, v, 1, p, &cu, &cv);
    lenscrop::Rect in{}, out{};
    uint32_t pw = 0, ph = 0;
    CHECK(fov::FoveaRects(cu, cv, p, 1792, 1920, W, H, &in, &out, &pw, &ph));
    const float colMid = static_cast<float>(out.x) + 0.5f * static_cast<float>(out.w) - 0.5f;
    const float rowMid = static_cast<float>(out.y) + 0.5f * static_cast<float>(out.h) - 0.5f;
    CHECK(fov::CompositeWeight(colMid, rowMid, W, H, cu, cv, p) > 0.999f);
    const float unshiftedTop = (v - p.height / 2.0f) * H + 2.0f;
    CHECK(fov::CompositeWeight(colMid, unshiftedTop, W, H, cu, cv, p) == 0.0f);
    const float unshiftedLeft = (u - p.width / 2.0f) * W + 2.0f;
    CHECK(fov::CompositeWeight(unshiftedLeft, rowMid, W, H, cu, cv, p) == 0.0f);
    // every pixel with weight > 0 lies inside out (the composite reads the centre only there)
    int outside = 0;
    for (uint32_t y = 0; y < H; y += 7) {
        for (uint32_t x = 0; x < W; x += 7) {
            if (fov::CompositeWeight(static_cast<float>(x), static_cast<float>(y), W, H, cu, cv, p) > 0.0f &&
                (x < out.x || x >= out.x + out.w || y < out.y || y >= out.y + out.h)) { ++outside; }
        }
    }
    CHECK(outside == 0);
}

// ---- Task 3: the adoption rules (fovdlss::Get calls MakePlan + AdoptStep; tested here on the shipped code, not a copy).
static fov::Plan Bk12Plan(int eye, const fov::Params& p, uint32_t rW = 1792, uint32_t rH = 1920, uint32_t oW = 2688, uint32_t oH = 2880)
{
    float g[12];
    Bk12(g, 0.0, eye);
    fov::Plan pl{};
    CHECK(fov::MakePlan(g, eye, p, rW, rH, oW, oH, &pl));
    return pl;
}

// A 2 s slider drag (width changes every 16 ms), then the value holds: ONE adoption, 300 ms after the drag stops - never during it.
static void debounce_settles_once()
{
    fov::Params p = Defaults();
    fov::Adoption s{};
    uint64_t t = 1000;
    CHECK(fov::AdoptStep(s, Bk12Plan(0, p), t, 1792, 1920, 2688, 2880));   // the first answer is adopted at once
    int adoptions = 0;
    for (int i = 0; i < 125; ++i) {                                         // 125 x 16 ms = 2 s
        t += 16;
        p.width = 0.55f + 0.002f * static_cast<float>(i + 1);
        if (fov::AdoptStep(s, Bk12Plan(0, p), t, 1792, 1920, 2688, 2880)) { ++adoptions; }
    }
    CHECK(adoptions == 0);
    uint64_t adoptedAt = 0;
    for (int i = 0; i < 40; ++i) {                                          // held for 640 ms
        t += 16;
        if (fov::AdoptStep(s, Bk12Plan(0, p), t, 1792, 1920, 2688, 2880)) { ++adoptions; adoptedAt = t; }
    }
    CHECK(adoptions == 1);
    CHECK(adoptedAt >= 2000 + 1000 + 300 && adoptedAt <= 2000 + 1000 + 300 + 32);
    CHECK(fov::SamePlan(s.last, Bk12Plan(0, p)));
}

// Roundness, transition and the border are composite-only: they never change the plan, so they never recreate.
static void composite_only_never_recreates()
{
    fov::Params p = Defaults();
    fov::Adoption s{};
    uint64_t t = 1000;
    CHECK(fov::AdoptStep(s, Bk12Plan(1, p), t, 1792, 1920, 2688, 2880));
    int adoptions = 0;
    for (int i = 0; i < 200; ++i) {
        t += 16;
        p.roundness = static_cast<float>(i % 100) / 100.0f;
        p.transition = 0.5f * static_cast<float>(i % 50) / 50.0f;
        p.border = (i & 1) != 0;
        if (fov::AdoptStep(s, Bk12Plan(1, p), t, 1792, 1920, 2688, 2880)) { ++adoptions; }
    }
    CHECK(adoptions == 0);
    // control: a recreate-class change (the periphery scale) IS adopted after the settle
    p.peripheryScale = 0.5f;
    for (int i = 0; i < 30; ++i) {
        t += 16;
        if (fov::AdoptStep(s, Bk12Plan(1, p), t, 1792, 1920, 2688, 2880)) { ++adoptions; }
    }
    CHECK(adoptions == 1);
}

// A resolution change: the adopted rectangle no longer fits -> the new plan is adopted AT ONCE (no 300 ms with a stale rectangle).
static void rect_rebuilt_for_new_size()
{
    const fov::Params p = Defaults();
    fov::Adoption s{};
    uint64_t t = 1000;
    CHECK(fov::AdoptStep(s, Bk12Plan(0, p), t, 1792, 1920, 2688, 2880));
    t += 16;
    const fov::Plan small = Bk12Plan(0, p, 1344, 1440, 2016, 2160);
    CHECK(fov::AdoptStep(s, small, t, 1344, 1440, 2016, 2160));
    CHECK(fov::SamePlan(s.last, small));
    CHECK(s.last.in.x + s.last.in.w <= 1344 && s.last.in.y + s.last.in.h <= 1440);
    CHECK(s.last.out.x + s.last.out.w <= 2016 && s.last.out.y + s.last.out.h <= 2160);
    CHECK(s.last.workW == 1008 && s.last.workH == 1080);
    // a growth that still fits the old rectangle waits for the settle like any other change
    t += 16;
    CHECK(!fov::AdoptStep(s, Bk12Plan(0, p), t, 1792, 1920, 2688, 2880));
}

// The label is the eye: eye 0 and eye 1 take their own geometry; with a horizontal offset their centres mirror about the middle.
static void label_selects_eye_geometry()
{
    fov::Params p = Defaults();
    p.xOffset = 0.6f;
    const fov::Plan a = Bk12Plan(0, p), b = Bk12Plan(1, p);
    CHECK(a.cu < 0.5f && b.cu > 0.5f);
    CHECK(std::fabs((a.cu - 0.5f) + (b.cu - 0.5f)) < 1e-5f);
    CHECK(a.cv == b.cv);
    CHECK(a.in.y == b.in.y && a.in.h == b.in.h && a.in.w == b.in.w);
    CHECK(a.in.x + a.in.w / 2 < 896 && b.in.x + b.in.w / 2 > 896);
    // MakePlan refuses bad geometry
    float g[12] = {};
    fov::Plan bad{};
    CHECK(!fov::MakePlan(g, 0, p, 1792, 1920, 2688, 2880, &bad));
}

// Review F2: the centre feature's DLSS mode follows ITS ratio (private output / input) by nearest match, EnsureUpscale's table
// (DLAA 1.00, Quality 1.50, Balanced 1.72, Performance 2.00, Ultra Performance 3.00) and tie rule (the first minimum wins).
static void nearest_mode_index()
{
    CHECK(fov::NearestModeIndex(1.0) == 0);
    CHECK(fov::NearestModeIndex(1500.0 / 1000.0) == 1);
    CHECK(fov::NearestModeIndex(1.72) == 2);
    CHECK(fov::NearestModeIndex(2.0) == 3);
    CHECK(fov::NearestModeIndex(3.0) == 4);
    CHECK(fov::NearestModeIndex(1482.0 / 988.0) == 1);        // FV1's centre at supersample 1.00
    CHECK(fov::NearestModeIndex(2.0 * 1482.0 / 988.0) == 4);  // the same at supersample 2.00 (3.0x)
    CHECK(fov::NearestModeIndex(1.6) == 1);                   // nearer 1.50 than 1.72
    CHECK(fov::NearestModeIndex(2.5) == 3);                   // a tie keeps the first (Performance)
    CHECK(fov::NearestModeIndex(9.0) == 4);
    CHECK(fov::NearestModeIndex(0.5) == 0);
}

int main()
{
    bk12_straight_ahead();
    fovea_centre_offsets();
    lens_centre_refuses_bad_geometry();
    fovea_rects_contract();
    offsets_move_rects();
    supersample_scales_private_size();
    fovea_clamped_to_image();
    private_size_capped();
    working_size();
    composite_weight();
    composite_follows_rect_offset();
    debounce_settles_once();
    composite_only_never_recreates();
    rect_rebuilt_for_new_size();
    label_selects_eye_geometry();
    nearest_mode_index();
    std::printf("foveation_logic: %d failure(s)\n", g_fail);
    return g_fail;
}
