// Standalone unit test for src/src/ovr2xr/uipin_logic.h - NOT part of the DLL build.
// docs/SPEC-UI-MENU-PIN-2026-10-11.md §7.1, on the cameras measured in INV-529. Exit code = number of failures.
#include "uipin_logic.h"

#include <cstdio>

using namespace dlvr::uipin;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static bool Near(float a, float b, float tol) { return std::fabs(a - b) < tol; }
static const float kDeg = 57.29578f;

// The in-game UI camera as measured (INV-529): pixel units, mirrored.
static Mat34 PixelCamera()
{
    return Mat34{ { 1.0f, 0.0f, 0.0f, 2688.0f, 0.0f, -1.0f, 0.0f, 1440.0f, 0.0f, 0.0f, 1.0f, 3476.467f } };
}
// A right-handed scene camera somewhere in the world, turned 40 degrees about the world's up, 1.7 m high.
static Mat34 SceneCamera()
{
    const float c = std::cos(0.698f), s = std::sin(0.698f);
    return Mat34{ { c, 0.0f, s, 12.0f, 0.0f, 1.0f, 0.0f, 1.7f, -s, 0.0f, c, -30.0f } };
}
// A move in the camera's own axes: yaw (left positive) and pitch (up positive) in radians, then a step (right, up, back).
static Mat34 LocalMove(float yaw, float pitch, float x, float y, float z)
{
    const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    const Mat34 ry{ { cy, 0.0f, sy, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, -sy, 0.0f, cy, 0.0f } };
    const Mat34 rx{ { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, cp, -sp, 0.0f, 0.0f, sp, cp, 0.0f } };
    Mat34 r = Mul(ry, rx);
    r.m[3] = x; r.m[7] = y; r.m[11] = z;
    return r;
}
static const float kPlaneCentre[3] = { 2688.0f, 1440.0f, 0.0f };

static void helpers_are_sound()
{
    const Mat34 s = SceneCamera();
    CHECK(IsRigid(s, 1e-4f));
    CHECK(Near(Det(s), 1.0f, 1e-4f));
    CHECK(Near(Det(PixelCamera()), -1.0f, 1e-6f));
    CHECK(IsRigid(PixelCamera(), 1e-6f));
    const Mat34 id = Mul(Inverse(s), s);
    for (int i = 0; i < 12; ++i) { CHECK(Near(id.m[i], (i == 0 || i == 5 || i == 10) ? 1.0f : 0.0f, 1e-4f)); }
    Mat34 bad = s;
    bad.m[5] = NAN;
    CHECK(!IsRigid(bad, 1e-2f));
    bad = s;
    bad.m[0] *= 1.2f;
    CHECK(!IsRigid(bad, 1e-2f));
    CHECK(Near(PlaneDistance(PixelCamera()), 3476.467f, 1e-3f));
}

static void eye_offset_is_read_back_from_the_engines_copy()
{
    Mat34 left = PixelCamera(), right = PixelCamera();
    left.m[3] = 2687.968f;      // the copies the engine made, INV-529
    right.m[3] = 2688.032f;
    CHECK(Near(EyeOffsetOf(PixelCamera(), left), -0.032f, 1e-3f));
    CHECK(Near(EyeOffsetOf(PixelCamera(), right), 0.032f, 1e-3f));
}

// The instant the menu opens the scene camera has not moved: unscaled, the result IS the engine's own copy.
static void no_motion_reproduces_the_engine()
{
    const Mat34 src = PixelCamera(), s = SceneCamera();
    float k = 0.0f;
    const Mat34 o = Pinned(src, s, s, -0.0317f, 0.0f, &k);
    CHECK(Near(k, 1.0f, 1e-6f));
    for (int i = 0; i < 12; ++i) {
        const float want = (i == 3) ? 2688.0f - 0.0317f : src.m[i];
        CHECK(Near(o.m[i], want, 1e-3f));
    }
    CHECK(Near(Det(o), -1.0f, 1e-4f));
}

// With a distance, the eye offset is carried in pixels: 2 m away, 1 m is 1738.2 px and the left eye sits 55.1 px left.
static void distance_gives_the_eyes_a_real_baseline()
{
    const Mat34 src = PixelCamera(), s = SceneCamera();
    float k = 0.0f;
    const Mat34 l = Pinned(src, s, s, -0.0317f, 2.0f, &k);
    const Mat34 r = Pinned(src, s, s, 0.0317f, 2.0f, &k);
    CHECK(Near(k, 1738.2335f, 0.01f));
    CHECK(Near(l.m[3], 2688.0f - 55.102f, 0.01f));
    CHECK(Near(r.m[3] - l.m[3], 110.204f, 0.02f));
    CHECK(Near(l.m[7], 1440.0f, 1e-3f) && Near(l.m[11], 3476.467f, 1e-2f));
}

// Turn the head 30 degrees LEFT: the menu must now be 30 degrees to the RIGHT of straight ahead, same distance.
static void head_yaw_leaves_the_menu_behind()
{
    const Mat34 src = PixelCamera(), s0 = SceneCamera();
    const Mat34 s = Mul(s0, LocalMove(0.5235988f, 0.0f, 0.0f, 0.0f, 0.0f));
    const Mat34 o = Pinned(src, s0, s, 0.0f, 2.0f, nullptr);
    float p[3];
    ToCamera(o, kPlaneCentre, p);
    CHECK(Near(std::atan2(p[0], -p[2]) * kDeg, 30.0f, 0.01f));
    CHECK(Near(std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]), 3476.467f, 0.05f));
    CHECK(Near(p[1], 0.0f, 0.05f));
    CHECK(IsRigid(o, 1e-4f) && Near(Det(o), -1.0f, 1e-4f));
    // the control: the two scene cameras swapped must give the OPPOSITE side, or this test could not tell a sign error
    const Mat34 wrong = Pinned(src, s, s0, 0.0f, 2.0f, nullptr);
    ToCamera(wrong, kPlaneCentre, p);
    CHECK(Near(std::atan2(p[0], -p[2]) * kDeg, -30.0f, 0.01f));
}

// Look UP 20 degrees: the menu must be 20 degrees BELOW straight ahead.
static void head_pitch_leaves_the_menu_behind()
{
    const Mat34 src = PixelCamera(), s0 = SceneCamera();
    const Mat34 s = Mul(s0, LocalMove(0.0f, 0.3490659f, 0.0f, 0.0f, 0.0f));
    const Mat34 o = Pinned(src, s0, s, 0.0f, 2.0f, nullptr);
    float p[3];
    ToCamera(o, kPlaneCentre, p);
    CHECK(Near(std::atan2(p[1], -p[2]) * kDeg, -20.0f, 0.01f));
    CHECK(Near(p[0], 0.0f, 0.05f));
}

// Lean 0.20 m toward a menu 2 m away: it is now 1.8 m away - 10 percent nearer, so 1 / 0.9 times larger.
static void leaning_in_brings_the_menu_closer()
{
    const Mat34 src = PixelCamera(), s0 = SceneCamera();
    const Mat34 s = Mul(s0, LocalMove(0.0f, 0.0f, 0.0f, 0.0f, -0.20f));
    const Mat34 o = Pinned(src, s0, s, 0.0f, 2.0f, nullptr);
    float p[3];
    ToCamera(o, kPlaneCentre, p);
    CHECK(Near(-p[2], 3476.467f * 0.9f, 0.05f));
    CHECK(Near(p[0], 0.0f, 0.05f) && Near(p[1], 0.0f, 0.05f));
}

// Step 0.10 m to the right and 0.05 m up: the menu's centre is 0.10 m to the left and 0.05 m below, in the UI's units.
static void stepping_aside_shifts_the_menu_the_other_way()
{
    const Mat34 src = PixelCamera(), s0 = SceneCamera();
    const Mat34 s = Mul(s0, LocalMove(0.0f, 0.0f, 0.10f, 0.05f, 0.0f));
    const Mat34 o = Pinned(src, s0, s, 0.0f, 2.0f, nullptr);
    float p[3];
    ToCamera(o, kPlaneCentre, p);
    CHECK(Near(p[0], -173.823f, 0.05f));
    CHECK(Near(p[1], -86.912f, 0.05f));
}

// Where the scene camera was when the menu opened does not matter - only how it has moved since.
static void only_the_motion_since_opening_counts()
{
    const Mat34 src = PixelCamera();
    const Mat34 move = LocalMove(0.3f, -0.2f, 0.05f, -0.02f, 0.1f);
    const Mat34 a0 = SceneCamera();
    const Mat34 b0 = Mul(Mat34{ { 0.0f, 0.0f, 1.0f, -400.0f, 0.0f, 1.0f, 0.0f, 55.0f, -1.0f, 0.0f, 0.0f, 9.0f } }, a0);
    const Mat34 oa = Pinned(src, a0, Mul(a0, move), 0.0317f, 2.0f, nullptr);
    const Mat34 ob = Pinned(src, b0, Mul(b0, move), 0.0317f, 2.0f, nullptr);
    // The second opening pose is ~400 m out, where float positions are 0.03 mm apart = 0.05 px at this scale: the
    // positions agree to a fraction of a pixel, the axes to float precision.
    for (int i = 0; i < 12; ++i) { CHECK(Near(oa.m[i], ob.m[i], (i == 3 || i == 7 || i == 11) ? 0.2f : 1e-4f)); }
}

// Four kilometres from the world's origin a 1 cm step must still come out as 1 cm (17.4 px at 2 m), not as float noise.
static void far_from_the_origin_a_small_step_survives()
{
    const Mat34 src = PixelCamera();
    Mat34 s0 = SceneCamera();
    s0.m[3] = 4000.0f; s0.m[11] = -3500.0f;
    Mat34 s = s0;
    s.m[3] += 0.01f * s0.m[0]; s.m[7] += 0.01f * s0.m[4]; s.m[11] += 0.01f * s0.m[8];   // 1 cm along its own right axis
    const Mat34 o = Pinned(src, s0, s, 0.0f, 2.0f, nullptr);
    float p[3];
    ToCamera(o, kPlaneCentre, p);
    CHECK(Near(p[0], -17.38f, 1.0f));          // float spacing at 4 km is 0.24 mm = 0.42 px; a millimetre would be 1.7 px
    const Mat34 rel = Relative(s0, Mul(s0, LocalMove(0.3f, -0.2f, 0.05f, -0.02f, 0.1f)));
    const Mat34 want = LocalMove(0.3f, -0.2f, 0.05f, -0.02f, 0.1f);
    for (int i = 0; i < 12; ++i) { CHECK(Near(rel.m[i], want.m[i], 2e-3f)); }
}

// The main menu: the UI camera becomes the scene camera, each eye 0.0317 m along the scene camera's right axis.
static void main_menu_uses_the_scene_camera()
{
    const Mat34 s = SceneCamera();
    const Mat34 l = MainMenu(s, -0.0317f), r = MainMenu(s, 0.0317f);
    for (int i = 0; i < 12; ++i) {
        if (i != 3 && i != 7 && i != 11) { CHECK(Near(l.m[i], s.m[i], 1e-6f) && Near(r.m[i], s.m[i], 1e-6f)); }
    }
    const float dx = r.m[3] - l.m[3], dy = r.m[7] - l.m[7], dz = r.m[11] - l.m[11];
    CHECK(Near(std::sqrt(dx * dx + dy * dy + dz * dz), 0.0634f, 1e-5f));
    CHECK(Near(dx * s.m[0] + dy * s.m[4] + dz * s.m[8], 0.0634f, 1e-5f));   // along the right axis, not across it
    CHECK(Near(EyeOffsetOf(s, l), -0.0317f, 1e-5f));
}

int main()
{
    helpers_are_sound();
    eye_offset_is_read_back_from_the_engines_copy();
    no_motion_reproduces_the_engine();
    distance_gives_the_eyes_a_real_baseline();
    head_yaw_leaves_the_menu_behind();
    head_pitch_leaves_the_menu_behind();
    leaning_in_brings_the_menu_closer();
    stepping_aside_shifts_the_menu_the_other_way();
    only_the_motion_since_opening_counts();
    far_from_the_origin_a_small_step_survives();
    main_menu_uses_the_scene_camera();
    std::printf("%d failure(s)\n", g_fail);
    return g_fail;
}
