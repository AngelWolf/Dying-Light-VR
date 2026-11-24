// afwtarget_logic.h (trace docs/traces/20261124_1700_afw_offcentre_stage1.md): the AFW rebuild's held-eye target on an off-centre frustum.
// Synthetic eyes: a view-projection built from a camera (rotation + position) and a frustum's tangents; the expected target is the HELD
// eye's own frustum at the FRESH camera moved sep along view-right, built the same way.
#include "afwtarget_logic.h"

#include <cmath>
#include <cstdio>

using namespace dlvr::afwtarget;

static int g_fail = 0;
#define CHECK(x) do { if (!(x)) { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #x); } } while (0)

struct Cam { double R[3][3]; double C[3]; };   // R rows = view right, up, forward (world)
struct Tans { double l, r, u, d; };

static Cam MakeCam(double yaw, double pitch, double roll, double cx, double cy, double cz)
{
    const double cyw = std::cos(yaw), syw = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch), cr = std::cos(roll), sr = std::sin(roll);
    const double Ry[3][3] = { { cyw, 0, -syw }, { 0, 1, 0 }, { syw, 0, cyw } };
    const double Rx[3][3] = { { 1, 0, 0 }, { 0, cp, sp }, { 0, -sp, cp } };
    const double Rz[3][3] = { { cr, sr, 0 }, { -sr, cr, 0 }, { 0, 0, 1 } };
    double t[3][3], R[3][3];
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) { t[i][j] = 0; for (int k = 0; k < 3; ++k) t[i][j] += Rx[i][k] * Ry[k][j]; }
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) { R[i][j] = 0; for (int k = 0; k < 3; ++k) R[i][j] += Rz[i][k] * t[k][j]; }
    Cam c{};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) c.R[i][j] = R[i][j];
    c.C[0] = cx; c.C[1] = cy; c.C[2] = cz;
    return c;
}

// clip x = P00 x_v + Cx z, clip y = P11 y_v + Cy z, clip w = z (z = depth along forward); row 2 = (0, 0, 0, near)
static void MakeVp(const Cam& c, const Tans& t, float m[16])
{
    const double P00 = 2.0 / (t.l + t.r), Cx = (t.l - t.r) / (t.l + t.r), P11 = 2.0 / (t.u + t.d), Cy = (t.d - t.u) / (t.u + t.d);
    double r0[3], r1[3], r3[3];
    for (int i = 0; i < 3; ++i) {
        r0[i] = P00 * c.R[0][i] + Cx * c.R[2][i];
        r1[i] = P11 * c.R[1][i] + Cy * c.R[2][i];
        r3[i] = c.R[2][i];
    }
    auto w = [&](const double* r) { return -(r[0] * c.C[0] + r[1] * c.C[1] + r[2] * c.C[2]); };
    const double v[16] = { r0[0], r0[1], r0[2], w(r0), r1[0], r1[1], r1[2], w(r1), 0, 0, 0, 0.1, r3[0], r3[1], r3[2], w(r3) };
    for (int i = 0; i < 16; ++i) { m[i] = static_cast<float>(v[i]); }
}

static Cam Moved(const Cam& c, double s)   // the camera moved s along its own view-right
{
    Cam o = c;
    for (int i = 0; i < 3; ++i) { o.C[i] += s * c.R[0][i]; }
    return o;
}

// max |difference| between the target (RT, tT) and matrix m's rows 0, 1, 3
static double Diff(const double RT[9], const double tT[3], const float m[16])
{
    const int rows[3] = { 0, 4, 12 };
    double d = 0.0;
    for (int k = 0; k < 3; ++k) {
        for (int i = 0; i < 3; ++i) { d = std::fmax(d, std::fabs(RT[k * 3 + i] - m[rows[k] + i])); }
        d = std::fmax(d, std::fabs(tT[k] - m[rows[k] + 3]));
    }
    return d;
}

static const Tans kSy1Left = { 1.3997, 0.8533, 0.9657, 1.4281 };   // RUN SY1 eye 0 at symmetric_fov 0
static const Tans kSy1Right = { 0.8533, 1.3997, 0.9657, 1.4281 };   // eye 1 mirrored

// The fresh LEFT eye at time 2, the held RIGHT eye's own matrix from time 1 (another head pose): the target is the right eye's frustum at
// the fresh camera + sep along view-right.
static void mirrored_eyes_held_right()
{
    const double sep = 0.0636;
    const Cam fresh = MakeCam(0.7, -0.25, 0.08, 1.2, 1.7, -3.1), heldOld = MakeCam(0.66, -0.22, 0.05, 1.25, 1.69, -3.05);
    float mF[16], mH[16], want[16];
    MakeVp(fresh, kSy1Left, mF);
    MakeVp(heldOld, kSy1Right, mH);
    MakeVp(Moved(fresh, sep), kSy1Right, want);
    double RT[9], tT[3];
    bool general = false;
    CHECK(HeldTarget(mF, mH, 1, sep, RT, tT, &general));
    CHECK(general);
    const double d = Diff(RT, tT, want);
    std::printf("held right: max |target - expected| %.2e\n", d);
    CHECK(d < 2e-5);
    // the old target (the fresh eye's frustum) is far off: the control that the test can fail
    float oldTarget[16];
    MakeVp(Moved(fresh, sep), kSy1Left, oldTarget);
    CHECK(Diff(RT, tT, oldTarget) > 0.1);
}

static void mirrored_eyes_held_left()
{
    const double sep = 0.0636;
    const Cam fresh = MakeCam(-1.1, 0.3, -0.04, -2.0, 1.6, 0.4), heldOld = MakeCam(-1.05, 0.28, -0.02, -2.02, 1.61, 0.41);
    float mF[16], mH[16], want[16];
    MakeVp(fresh, kSy1Right, mF);
    MakeVp(heldOld, kSy1Left, mH);
    MakeVp(Moved(fresh, -sep), kSy1Left, want);
    double RT[9], tT[3];
    bool general = false;
    CHECK(HeldTarget(mF, mH, 0, sep, RT, tT, &general));
    CHECK(general);
    CHECK(Diff(RT, tT, want) < 2e-5);
}

// Eyes that are not mirror images (a Steam Frame's 1.7260 / 1.6938 kind): the held eye's own SCALE comes along too.
static void unequal_eyes_take_the_held_scale()
{
    const double sep = 0.065;
    const Tans e0 = { 1.7260, 1.20, 1.10, 1.50 }, e1 = { 1.15, 1.6938, 1.05, 1.52 };
    const Cam fresh = MakeCam(0.2, 0.1, 0.0, 0.0, 1.7, 0.0), heldOld = MakeCam(0.18, 0.1, 0.01, 0.01, 1.7, 0.0);
    float mF[16], mH[16], want[16];
    MakeVp(fresh, e0, mF);
    MakeVp(heldOld, e1, mH);
    MakeVp(Moved(fresh, sep), e1, want);
    double RT[9], tT[3];
    bool general = false;
    CHECK(HeldTarget(mF, mH, 1, sep, RT, tT, &general));
    CHECK(general && Diff(RT, tT, want) < 2e-5);
}

// symmetric_fov 2: one centred frustum for both eyes -> general = false and the OLD arithmetic, bit for bit.
static void centred_equal_eyes_are_the_old_target()
{
    const double sep = 0.0636;
    const Tans sym = { 1.3764, 1.3764, 1.4747, 1.4747 };
    const Cam fresh = MakeCam(0.4, -0.1, 0.02, 0.5, 1.7, -1.0), heldOld = MakeCam(0.38, -0.09, 0.02, 0.51, 1.7, -1.0);
    float mF[16], mH[16];
    MakeVp(fresh, sym, mF);
    MakeVp(heldOld, sym, mH);
    double RT[9], tT[3];
    bool general = true;
    CHECK(HeldTarget(mF, mH, 1, sep, RT, tT, &general));
    CHECK(!general);
    // the old code, verbatim
    double oRT[9] = { mF[0], mF[1], mF[2], mF[4], mF[5], mF[6], mF[12], mF[13], mF[14] };
    double otT[3] = { mF[3], mF[7], mF[15] };
    const double wl = std::sqrt(oRT[6] * oRT[6] + oRT[7] * oRT[7] + oRT[8] * oRT[8]);
    const double cx = (oRT[0] * oRT[6] + oRT[1] * oRT[7] + oRT[2] * oRT[8]) / wl;
    const double ax[3] = { oRT[0] - cx * oRT[6] / wl, oRT[1] - cx * oRT[7] / wl, oRT[2] - cx * oRT[8] / wl };
    const double a = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
    otT[0] -= 1.0 * static_cast<double>(static_cast<float>(sep)) * a;
    double RT2[9], tT2[3];
    bool g2 = true;
    CHECK(HeldTarget(mF, mH, 1, static_cast<double>(static_cast<float>(sep)), RT2, tT2, &g2));
    bool same = true;
    for (int i = 0; i < 9; ++i) { same = same && RT2[i] == oRT[i]; }
    for (int i = 0; i < 3; ++i) { same = same && tT2[i] == otT[i]; }
    CHECK(same);
    CHECK(HeldTarget(mF, nullptr, 1, sep, RT, tT, &general) && !general);   // no held matrix: the old target
}

// InvertCarry: the inverse composes to the identity; IdentityMinusOffset is the old stale carry.
static void inverse_carry_composes_to_identity()
{
    const float g[3][4] = { { 1.02f, 0.03f, -0.21f, 0.05f }, { -0.01f, 0.98f, 0.02f, -0.3f }, { 0.004f, -0.002f, 1.0f, 0.01f } };
    float gi[3][4];
    CHECK(InvertCarry(g, gi));
    const double p[3] = { 0.3, -0.7, 2.5 };
    double t[3], back[3];
    for (int r = 0; r < 3; ++r) { t[r] = g[r][0] * p[0] + g[r][1] * p[1] + g[r][2] * p[2] + g[r][3]; }
    for (int r = 0; r < 3; ++r) { back[r] = gi[r][0] * t[0] + gi[r][1] * t[1] + gi[r][2] * t[2] + gi[r][3]; }
    CHECK(std::fabs(back[0] - p[0]) < 1e-5 && std::fabs(back[1] - p[1]) < 1e-5 && std::fabs(back[2] - p[2]) < 1e-5);
    float id[3][4];
    IdentityMinusOffset(g, id);
    CHECK(id[0][0] == 1.0f && id[1][1] == 1.0f && id[2][2] == 1.0f && id[0][1] == 0.0f && id[0][3] == -0.05f && id[1][3] == 0.3f);
    const float sing[3][4] = { { 1, 2, 3, 0 }, { 2, 4, 6, 0 }, { 0, 0, 1, 0 } };
    CHECK(!InvertCarry(sing, gi));
}

int main()
{
    mirrored_eyes_held_right();
    mirrored_eyes_held_left();
    unequal_eyes_take_the_held_scale();
    centred_equal_eyes_are_the_old_target();
    inverse_carry_composes_to_identity();
    std::printf("afwtarget_logic_test: %d failure(s)\n", g_fail);
    return g_fail;
}
