// Tests for rootyaw_logic.h (trace docs/traces/20261117_1730_root_yaw_hold.md). Build: cl /nologo /EHsc /W4 /WX /std:c++17
// /I src/src/ovr2xr src/src/tests/rootyaw_logic_test.cpp
#include "rootyaw_logic.h"

#include <cmath>
#include <cstdio>

using namespace dlvr::rootyaw;

static int g_fail = 0;
static void Check(bool ok, const char* what)
{
    if (!ok) { ++g_fail; std::printf("FAIL: %s\n", what); }
}
static bool Near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

// Builds a rotation (rows) = Ry(yaw) * Rx(pitch) * Rz(roll), translation t.
static void Make(float yaw, float pitch, float roll, const float t[3], float* m)
{
    const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch), cr = std::cos(roll),
                sr = std::sin(roll);
    const float Ry[9] = { cy, 0, sy, 0, 1, 0, -sy, 0, cy };
    const float Rx[9] = { 1, 0, 0, 0, cp, -sp, 0, sp, cp };
    const float Rz[9] = { cr, -sr, 0, sr, cr, 0, 0, 0, 1 };
    float A[9], B[9];
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) { A[3*r+c] = Rx[3*r]*Rz[c] + Rx[3*r+1]*Rz[3+c] + Rx[3*r+2]*Rz[6+c]; }
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) { B[3*r+c] = Ry[3*r]*A[c] + Ry[3*r+1]*A[3+c] + Ry[3*r+2]*A[6+c]; }
    for (int r = 0; r < 3; ++r) { m[4*r] = B[3*r]; m[4*r+1] = B[3*r+1]; m[4*r+2] = B[3*r+2]; m[4*r+3] = t[r]; }
}
static void Mul34(const float* a, const float* b, float* o)
{
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) {
        o[4*r+c] = a[4*r]*b[c] + a[4*r+1]*b[4+c] + a[4*r+2]*b[8+c] + (c == 3 ? a[4*r+3] : 0.0f);
    }
}

static void pre_rot_y_moves_heading_only()
{
    const float t[3] = { 0.1f, 0.9f, -0.2f };
    float m[12];
    Make(0.3f, 0.12f, -0.08f, t, m);
    const int col = MostHorizontalCol(m);
    Check(col >= 0, "a column is horizontal enough");
    float h0 = 0, h1 = 0;
    Check(HeadingOfCol(m, col, &h0), "heading before");
    float y0[3] = { m[4], m[5], m[6] };
    PreRotY(0.25f, m);
    Check(HeadingOfCol(m, col, &h1), "heading after");
    Check(Near(WrapPi(h1 - h0), 0.25f, 1e-5f), "PreRotY adds exactly a to the heading");
    Check(Near(m[4], y0[0], 1e-6f) && Near(m[5], y0[1], 1e-6f) && Near(m[6], y0[2], 1e-6f), "the up row (pitch / roll) is unchanged");
    Check(m[3] == t[0] && m[7] == t[1] && m[11] == t[2], "translation untouched");
    // replace: drive the heading back to a latched value
    float hNow = 0;
    HeadingOfCol(m, col, &hNow);
    PreRotY(WrapPi(h0 - hNow), m);
    float hBack = 0;
    HeadingOfCol(m, col, &hBack);
    Check(Near(WrapPi(hBack - h0), 0.0f, 1e-5f), "held back to the latched heading");
}

static void local_from_world_recomposes()
{
    const float tp[3] = { 0.0f, 1.2f, 0.05f }, tc[3] = { 0.13f, 0.0f, -0.05f };
    float P[12], L[12], W[12];
    Make(-0.7f, 0.05f, 0.02f, tp, P);
    P[0] *= 1.0f; P[5] *= 1.0f;
    Make(0.15f, -0.1f, 0.3f, tc, L);
    Mul34(P, L, W);
    PreRotY(0.2f, W);                       // the world the hold wants
    float Lnew[12];
    for (int i = 0; i < 12; ++i) { Lnew[i] = L[i]; }
    Check(LocalRotFromWorld(P, W, Lnew), "parent invertible");
    float back[12];
    Mul34(P, Lnew, back);
    float maxErr = 0;
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) { maxErr = std::fmax(maxErr, std::fabs(back[4*r+c] - W[4*r+c])); }
    Check(maxErr < 1e-5f, "parent x new local == the wanted world rotation");
    Check(Lnew[3] == L[3] && Lnew[7] == L[7] && Lnew[11] == L[11], "local translation kept");
}

// RUN ROOT1 fix: the spine top's local from the OLD (pre-root-write) worlds lands on the target once the root's Ry(dRoot) reaches
// the subtree - and the old "read after, stale" formula lands dRoot off (the bug, reproduced as a deliberate miss).
static void spine_top_from_old_worlds_lands_on_target()
{
    const float tp[3] = { 0.02f, 1.30f, 0.01f }, th[3] = { 0.12f, 0.0f, 0.0f };
    float S3[12], L[12], H[12];
    Make(0.40f, 0.03f, -0.02f, tp, S3);         // spine3 world (old)
    Make(-0.20f, 0.10f, 0.05f, th, L);
    Mul34(S3, L, H);                            // hspine1 world (old)
    const int col = MostHorizontalCol(H);
    float hOld = 0;
    HeadingOfCol(H, col, &hOld);
    const float dRoot = 0.30f, target = hOld + 0.05f;
    // fixed formula
    const float err = WrapPi(target - WrapPi(hOld + dRoot));
    float want[12], nl[12];
    for (int i = 0; i < 12; ++i) { want[i] = H[i]; nl[i] = L[i]; }
    PreRotY(err, want);
    Check(LocalRotFromWorld(S3, want, nl), "spine3 invertible");
    float S3new[12];
    for (int i = 0; i < 12; ++i) { S3new[i] = S3[i]; }
    PreRotY(dRoot, S3new);                      // the root's turn reaches spine3
    float Hfinal[12];
    Mul34(S3new, nl, Hfinal);
    float hF = 0;
    HeadingOfCol(Hfinal, col, &hF);
    Check(Near(WrapPi(hF - target), 0.0f, 1e-4f), "the spine top lands on the target after the root's turn");
    // the ROOT1 formula (stale reads taken as fresh): error = target - hOld -> lands dRoot past the target
    const float errOld = WrapPi(target - hOld);
    float want2[12], nl2[12];
    for (int i = 0; i < 12; ++i) { want2[i] = H[i]; nl2[i] = L[i]; }
    PreRotY(errOld, want2);
    LocalRotFromWorld(S3, want2, nl2);
    float H2[12];
    Mul34(S3new, nl2, H2);
    float h2 = 0;
    HeadingOfCol(H2, col, &h2);
    Check(!Near(WrapPi(h2 - target), 0.0f, 1e-3f) && Near(WrapPi(h2 - target), dRoot, 1e-4f),
          "MISS: the ROOT1 formula overshoots by exactly dRoot");
}

// The chest hold: locals only. O2 = root x a x b (spine2 in object space), O3 = O2 x c. new c = inverse(O2) x Ry(err) x O3 puts
// spine3's object-space heading on the target, and the root's later Ry(dRoot) moves both by the same dRoot.
static void chest_from_locals_lands_on_target()
{
    const float t0[3] = { 0, 0.93f, 0 }, t1[3] = { 0.1f, 0, 0 };
    float R[12], A[12], Bm[12], C[12], O2a[12], O2[12], O3[12];
    Make(-0.85f, 0.05f, 0.02f, t0, R);
    Make(0.10f, 0.20f, -0.10f, t1, A);
    Make(-0.05f, 0.10f, 0.15f, t1, Bm);
    Make(0.20f, -0.05f, 0.05f, t1, C);
    ComposeRT(R, A, O2a);
    ComposeRT(O2a, Bm, O2);
    ComposeRT(O2, C, O3);
    const int col = MostHorizontalCol(O3);
    float h = 0;
    HeadingOfCol(O3, col, &h);
    const float dRoot = 0.25f, target = h - 0.15f + dRoot - 0.1f;
    const float err = WrapPi(target - WrapPi(h + dRoot));
    float want[12], nc[12];
    for (int i = 0; i < 12; ++i) { want[i] = O3[i]; nc[i] = C[i]; }
    PreRotY(err, want);
    Check(LocalRotFromWorld(O2, want, nc), "spine2 object-space invertible");
    float Rh[12];
    for (int i = 0; i < 12; ++i) { Rh[i] = R[i]; }
    PreRotY(dRoot, Rh);                         // the held root
    float X1[12], X2[12], X3[12];
    ComposeRT(Rh, A, X1);
    ComposeRT(X1, Bm, X2);
    ComposeRT(X2, nc, X3);
    float hF = 0;
    HeadingOfCol(X3, col, &hF);
    Check(Near(WrapPi(hF - target), 0.0f, 1e-4f), "the chest lands on the target with the root held");
    Check(nc[3] == C[3] && nc[7] == C[7] && nc[11] == C[11], "chest local translation kept");
}

static void deliberate_misses()
{
    float z[12] = {};
    float inv[9];
    Check(!Inverse3(z, inv), "MISS: a singular parent is refused");
    float h = 0;
    Check(!HeadingOfCol(z, 0, &h), "MISS: a degenerate column has no heading");
    Check(!HeadingOfCol(z, 3, &h), "MISS: column 3 is not a rotation column");
    Check(MostHorizontalCol(z) == -1, "MISS: no column when all are zero");
    // a vertical-only column is never chosen over a horizontal one
    const float t[3] = { 0, 0, 0 };
    float m[12];
    Make(0.0f, 0.0f, 0.0f, t, m);           // identity: col 1 is straight up
    Check(MostHorizontalCol(m) != 1, "MISS: the up column is not picked");
}

int main()
{
    pre_rot_y_moves_heading_only();
    local_from_world_recomposes();
    spine_top_from_old_worlds_lands_on_target();
    chest_from_locals_lands_on_target();
    deliberate_misses();
    std::printf(g_fail ? "FAILED: %d failure(s)\n" : "PASSED: %d failure(s)\n", g_fail);
    return g_fail ? 1 : 0;
}
