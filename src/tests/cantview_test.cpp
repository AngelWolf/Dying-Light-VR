// cantview_logic.h unit test (trace docs/traces/20261118_1150_canted_views.md). No CMake target - compile with cl:
//   cl /nologo /EHsc /W4 /std:c++17 /I "<tree>\src\ovr2xr" "<tree>\src\tests\cantview_test.cpp" /Fe:cantview_test.exe
// Prints "cantview_test: N failure(s)" and exits N.
#include "cantview_logic.h"

#include <cmath>
#include <cstdio>

using namespace dlvr::cantview;

static int g_fail = 0;
static int g_checks = 0;

static void Check(bool ok, const char* what, double a = 0.0, double b = 0.0)
{
    ++g_checks;
    if (!ok) {
        ++g_fail;
        std::printf("FAIL: %s (%.9g vs %.9g)\n", what, a, b);
    }
}

// The angle between two orientations in DOUBLE on the relative rotation (memory test-angles-in-double).
static double AngleD(const Quat& a, const Quat& b)
{
    const double ax = -a.x, ay = -a.y, az = -a.z, aw = a.w;
    const double rx = aw * b.x + ax * b.w + ay * b.z - az * b.y;
    const double ry = aw * b.y - ax * b.z + ay * b.w + az * b.x;
    const double rz = aw * b.z + ax * b.y - ay * b.x + az * b.w;
    const double rw = aw * b.w - ax * b.x - ay * b.y - az * b.z;
    return 2.0 * std::atan2(std::sqrt(rx * rx + ry * ry + rz * rz), std::fabs(rw)) * 57.29577951308232;
}

static Quat AxisAngle(double ax, double ay, double az, double deg)
{
    const double n = std::sqrt(ax * ax + ay * ay + az * az);
    const double h = deg * 0.017453292519943295 * 0.5;
    const double s = std::sin(h) / n;
    return { static_cast<float>(ax * s), static_cast<float>(ay * s), static_cast<float>(az * s),
             static_cast<float>(std::cos(h)) };
}

static Fov FovDeg(double l, double r, double u, double d)   // outer/inner as signed OpenXR angles in degrees
{
    const double k = 0.017453292519943295;
    return { static_cast<float>(l * k), static_cast<float>(r * k), static_cast<float>(u * k), static_cast<float>(d * k) };
}

// Every point of frustum `src` (frame E), carried into frame H by rel = conj(qH) qE, lies inside `dst` (frame H).
// Grid of 21 x 21 over the image plane, edges included. Returns the worst overshoot in tangent units (<= 0 = inside).
static double WorstOutside(const Fov& src, const Quat& rel, const Fov& dst)
{
    const double l = std::tan(src.left), r = std::tan(src.right), d = std::tan(src.down), u = std::tan(src.up);
    const double L = std::tan(dst.left), R = std::tan(dst.right), D = std::tan(dst.down), U = std::tan(dst.up);
    double worst = -1e30;
    for (int i = 0; i <= 20; ++i) {
        for (int j = 0; j <= 20; ++j) {
            const float v[3] = { static_cast<float>(l + (r - l) * i / 20.0), static_cast<float>(d + (u - d) * j / 20.0), -1.0f };
            float o[3];
            Rotate(rel, v, o);
            const double px = o[0] / -static_cast<double>(o[2]);
            const double py = o[1] / -static_cast<double>(o[2]);
            const double out[4] = { L - px, px - R, D - py, py - U };
            for (double x : out) { if (x > worst) { worst = x; } }
        }
    }
    return worst;
}

int main()
{
    const Fov kCrystalish = FovDeg(-60.0, 45.0, 50.0, -55.0);   // left eye: outer 60, inner 45 (a wide-FOV shape, not a measured lens)
    const Fov kCrystalishR = FovDeg(-45.0, 60.0, 50.0, -55.0);  // right eye: the mirror
    const Fov kQuest3 = FovDeg(-54.0, 40.0, 44.0, -55.0);       // ovr2xr.cpp:7561 numbers

    // 1. Parallel views: untouched, bit for bit.
    {
        Quat q[2] = { AxisAngle(0.2, 1.0, 0.1, 37.0), AxisAngle(0.2, 1.0, 0.1, 37.0) };
        Fov f[2] = { kQuest3, kQuest3 };
        const Quat q0 = q[0];
        const Fov f0 = f[0];
        const Result r = ToParallel(q, f);
        Check(!r.converted, "parallel: not converted");
        Check(q[0].x == q0.x && q[0].y == q0.y && q[0].z == q0.z && q[0].w == q0.w, "parallel: q bit-identical");
        Check(f[0].left == f0.left && f[0].right == f0.right && f[0].up == f0.up && f[0].down == f0.down,
              "parallel: fov bit-identical");
    }
    // 2. 0.5 deg apart: below kMinCantDeg, untouched.
    {
        const Quat h = AxisAngle(0.0, 1.0, 0.0, 10.0);
        Quat q[2] = { Mul(h, AxisAngle(0, 1, 0, 0.25)), Mul(h, AxisAngle(0, 1, 0, -0.25)) };
        Fov f[2] = { kQuest3, kQuest3 };
        const Result r = ToParallel(q, f);
        Check(!r.converted, "0.5 deg: not converted", r.angleDeg, kMinCantDeg);
        Check(std::fabs(r.angleDeg - 0.5) < 0.01, "0.5 deg: angle measured", r.angleDeg, 0.5);
    }
    // 3. Symmetric outward cant c under a turned, pitched, rolled head: converted, both eyes = head, FOV covers the eye.
    const double cants[] = { 1.5, 5.0, 10.0, 15.0, 19.0, 25.0 };   // not 20: 60 + 20 sits ON the 80 deg clamp (float decides)
    const Quat heads[] = { Quat{ 0, 0, 0, 1 }, AxisAngle(0.0, 1.0, 0.0, 135.0), AxisAngle(1.0, 0.3, 0.0, -25.0),
                           AxisAngle(0.4, 1.0, 0.6, 71.0) };
    for (double c : cants) {
        for (const Quat& h : heads) {
            Quat q[2] = { Mul(h, AxisAngle(0, 1, 0, +c)), Mul(h, AxisAngle(0, 1, 0, -c)) };
            Fov f[2] = { kCrystalish, kCrystalishR };
            const Fov raw[2] = { f[0], f[1] };
            const Quat rawQ[2] = { q[0], q[1] };
            // what the PRE-FIX submit declared for eye 1 (rendered = head given = view 0): rendered * inv(view0) * view1
            const Quat oldDecl1 = Mul(rawQ[0], Mul(Conj(rawQ[0]), rawQ[1]));
            const double oldErr = AngleD(rawQ[0], oldDecl1);   // vs what was rendered (view 0's orientation)
            Check(std::fabs(oldErr - 2.0 * c) < 0.01, "pre-fix eye 1 declared 2c off what was rendered", oldErr, 2.0 * c);

            const Result r = ToParallel(q, f);
            Check(r.converted, "cant: converted", c);
            Check(std::fabs(r.angleDeg - 2.0 * c) < 0.01, "cant: angle = 2c", r.angleDeg, 2.0 * c);
            Check(AngleD(q[0], h) < 1e-3, "cant: eye 0 = head", AngleD(q[0], h), 0.0);
            Check(AngleD(q[1], h) < 1e-3, "cant: eye 1 = head", AngleD(q[1], h), 0.0);
            Check(AngleD(q[0], q[1]) < 1e-4, "cant: declared eyes equal (the double image gone)", AngleD(q[0], q[1]), 0.0);
            // outer 60 + c reaches the tan(80) clamp at c >= 20: expect it reported there, and coverage only below it
            const bool expectClamp = 60.0 + c >= 79.99;
            Check(r.clamped == expectClamp, "cant: clamped exactly when 60 + c reaches 80 deg", c, expectClamp ? 1.0 : 0.0);
            if (expectClamp) { continue; }
            for (int e = 0; e < 2; ++e) {
                const Quat rel = Mul(Conj(q[e]), rawQ[e]);   // eye frame -> converted (head) frame
                const double w = WorstOutside(raw[e], rel, f[e]);
                Check(w <= 1e-4, "cant: converted FOV contains the eye's display", w, 0.0);
                // tight: the outer side grows by about c, the inner shrinks by about c (pure yaw)
                const double outerRaw = (e == 0 ? -raw[e].left : raw[e].right) * 57.29577951308232;
                const double outerNew = (e == 0 ? -f[e].left : f[e].right) * 57.29577951308232;
                Check(std::fabs(outerNew - (outerRaw + c)) < 0.05, "cant: outer edge moved out by c", outerNew, outerRaw + c);
                const double innerRaw = (e == 0 ? raw[e].right : -raw[e].left) * 57.29577951308232;
                const double innerNew = (e == 0 ? f[e].right : -f[e].left) * 57.29577951308232;
                Check(std::fabs(innerNew - (innerRaw - c)) < 0.05, "cant: inner edge moved in by c", innerNew, innerRaw - c);
                Check(f[e].up >= raw[e].up - 1e-6f && f[e].down <= raw[e].down + 1e-6f, "cant: vertical never shrinks");
            }
        }
    }
    // 4. Fake cant then conversion = the original orientation, a FOV that contains the original.
    const double fakes[] = { 5.0, 10.0, 25.0 };
    for (double c : fakes) {
        const Quat h = AxisAngle(0.1, 1.0, -0.2, -48.0);
        Quat q[2] = { h, h };
        Fov f[2] = { kQuest3, FovDeg(-40.0, 54.0, 44.0, -55.0) };
        const Fov raw[2] = { f[0], f[1] };
        bool cl = false;
        FakeCant(0, static_cast<float>(c), q[0], f[0], &cl);
        FakeCant(1, static_cast<float>(c), q[1], f[1], &cl);
        Check(!cl, "fake: not clamped", c);
        Check(std::fabs(AngleD(q[0], q[1]) - 2.0 * c) < 0.01, "fake: views 2c apart", AngleD(q[0], q[1]), 2.0 * c);
        // left eye turned OUTWARD (its forward goes to -X in the head frame)
        {
            const float fwd[3] = { 0.0f, 0.0f, -1.0f };
            float o[3];
            Rotate(Mul(Conj(h), q[0]), fwd, o);
            Check(o[0] < 0.0f, "fake: view 0 turned left (outward)", o[0], 0.0);
        }
        const Result r = ToParallel(q, f);
        Check(r.converted, "fake: converted", c);
        Check(AngleD(q[0], h) < 1e-3 && AngleD(q[1], h) < 1e-3, "fake: back to the head", AngleD(q[0], h), 0.0);
        for (int e = 0; e < 2; ++e) {
            Check(WorstOutside(raw[e], Quat{ 0, 0, 0, 1 }, f[e]) <= 1e-4, "fake round trip contains the original FOV",
                  WorstOutside(raw[e], Quat{ 0, 0, 0, 1 }, f[e]), 0.0);
        }
    }
    // 5. Clamp: an 85 deg outer FOV turned another 10 deg out cannot be parallel - clamped and reported.
    {
        Quat q[2] = { AxisAngle(0, 1, 0, 10.0), AxisAngle(0, 1, 0, -10.0) };
        Fov f[2] = { FovDeg(-85.0, 45.0, 50.0, -50.0), FovDeg(-45.0, 85.0, 50.0, -50.0) };
        const Result r = ToParallel(q, f);
        Check(r.converted && r.clamped, "clamp: reported");
        Check(std::fabs(std::tan(f[0].left) + kMaxTan) < 1e-3, "clamp: outer = -kMaxTan", std::tan(f[0].left), -kMaxTan);
    }
    // 6. q and -q are the same rotation: a sign-flipped view must give the same head.
    {
        const Quat h = AxisAngle(0.0, 1.0, 0.0, 20.0);
        Quat a = Mul(h, AxisAngle(0, 1, 0, 10.0));
        Quat b = Mul(h, AxisAngle(0, 1, 0, -10.0));
        b = { -b.x, -b.y, -b.z, -b.w };
        Quat q[2] = { a, b };
        Fov f[2] = { kCrystalish, kCrystalishR };
        const Result r = ToParallel(q, f);
        Check(r.converted && AngleD(q[0], h) < 1e-3, "sign flip: head found", AngleD(q[0], h), 0.0);
    }
    // 7. AngleDeg resolves small angles (not acos-blind).
    Check(std::fabs(AngleDeg(Quat{ 0, 0, 0, 1 }, AxisAngle(0, 1, 0, 0.02)) - 0.02) < 0.002, "AngleDeg resolves 0.02 deg");

    std::printf("cantview_test: %d checks, %d failure(s)\n", g_checks, g_fail);
    return g_fail;
}
