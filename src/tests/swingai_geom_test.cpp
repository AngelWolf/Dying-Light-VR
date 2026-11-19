// [SWINGAI] v2 geometry - swingshadow::TriAabb / TriAabbDist / Inverse34 / Xform34 (trace docs/traces/20261119_1730_swing_probe.md).
// Build: cl /nologo /EHsc /W4 /std:c++17 /I src_beta/src/ovr2xr src_beta/src/tests/swingai_geom_test.cpp
#include "swingshadow_logic.h"
#include <cmath>
#include <cstdio>

using namespace dlvr::swingshadow;
static int g_fail = 0, g_n = 0;
static void Check(bool ok, const char* what)
{
    ++g_n;
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) { ++g_fail; }
}

int main()
{
    const float c[3] = { 0, 0, 0 }, h[3] = { 0.1f, 0.2f, 0.3f };
    const float mn[3] = { -0.1f, -0.2f, -0.3f }, mx[3] = { 0.1f, 0.2f, 0.3f };
    {   // a triangle through the centre
        const float a[3] = { -1, 0, 0 }, b[3] = { 1, 0, 0 }, d[3] = { 0, 1, 0 };
        Check(TriAabb(c, h, a, b, d), "triangle through the box centre overlaps");
        Check(TriAabbDist(mn, mx, a, b, d) == 0.0f, "its distance is 0");
    }
    {   // fully apart along x
        const float a[3] = { 0.5f, 0, 0 }, b[3] = { 0.6f, 0, 0 }, d[3] = { 0.5f, 0.1f, 0 };
        Check(!TriAabb(c, h, a, b, d), "triangle 0.4 m off +x does not overlap");
        const float dist = TriAabbDist(mn, mx, a, b, d);
        Check(std::fabs(dist - 0.4f) < 1e-4f, "its distance is 0.4");
    }
    {   // the plane test: a big triangle in the plane z = 0.35, above the box (all face tests pass in x / y)
        const float a[3] = { -5, -5, 0.35f }, b[3] = { 5, -5, 0.35f }, d[3] = { 0, 5, 0.35f };
        Check(!TriAabb(c, h, a, b, d), "triangle in a plane 0.05 m above the box does not overlap");
        const float a2[3] = { -5, -5, 0.25f }, b2[3] = { 5, -5, 0.25f }, d2[3] = { 0, 5, 0.25f };
        Check(TriAabb(c, h, a2, b2, d2), "the same triangle 0.05 m inside the top overlaps");
    }
    {   // an edge-axis separation: a sliver past a box edge diagonally - face and plane tests pass, an edge cross axis separates
        const float a[3] = { 0.25f, 0.0f, -1.0f }, b[3] = { 0.0f, 0.35f, -1.0f }, d[3] = { 0.25f, 0.0f, 1.0f };
        Check(!TriAabb(c, h, a, b, d), "a sliver past the box's x/y corner (separated by the TRIANGLE NORMAL) does not overlap");
        const float a2[3] = { 0.05f, 0.0f, -1.0f }, b2[3] = { 0.0f, 0.15f, -1.0f }, d2[3] = { 0.05f, 0.0f, 1.0f };
        Check(TriAabb(c, h, a2, b2, d2), "the same sliver moved inside the corner overlaps");
    }
    {   // ONLY an edge-cross axis separates: a triangle in the plane z = 0 (through the box), x range -1..0.3 and y range -1..0.3
        // (both overlap the box), all of it on the side x + y <= -0.7 of its edge a->b; the box spans x + y in [-0.3, 0.3]
        const float a[3] = { 0.3f, -1.0f, 0.0f }, b[3] = { -1.0f, 0.3f, 0.0f }, d[3] = { -1.0f, -1.0f, 0.0f };
        Check(!TriAabb(c, h, a, b, d), "a triangle separated ONLY by an edge-cross axis does not overlap");
        const float a2[3] = { 0.3f, -0.3f, 0.0f }, b2[3] = { -0.3f, 0.3f, 0.0f }, d2[3] = { -1.0f, -1.0f, 0.0f };
        Check(TriAabb(c, h, a2, b2, d2), "moved so its edge crosses the box (x + y = 0), it overlaps");
        const float dist = TriAabbDist(mn, mx, a, b, d);
        Check(dist > 0.25f && dist < 0.40f, "its sampled distance is about the corner gap (true gap 0.283 m)");
    }
    {   // the inverse: a rotation of 30 deg about y + translation, round trip
        const float s = std::sin(0.5235988f), co = std::cos(0.5235988f);
        const float m[12] = { co, 0, s, 1.5f, 0, 1, 0, -2.0f, -s, 0, co, 0.25f };
        float inv[12];
        Check(Inverse34(m, inv), "rotation + translation inverts");
        const float p[3] = { 0.3f, -0.7f, 1.1f };
        float w[3], back[3];
        Xform34(m, p, w);
        Xform34(inv, w, back);
        Check(std::fabs(back[0] - p[0]) < 1e-5f && std::fabs(back[1] - p[1]) < 1e-5f && std::fabs(back[2] - p[2]) < 1e-5f,
              "local -> world -> local returns the point");
        const float sing[12] = { 1, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0 };
        Check(!Inverse34(sing, inv), "a singular matrix is refused");
    }
    {   // a scaled matrix (the element matrix may carry scale)
        const float m[12] = { 2, 0, 0, 0, 0, 0.5f, 0, 1, 0, 0, 3, -1 };
        float inv[12], w[3], back[3];
        const float p[3] = { 1, 2, 3 };
        Inverse34(m, inv);
        Xform34(m, p, w);
        Xform34(inv, w, back);
        Check(std::fabs(back[0] - 1) < 1e-5f && std::fabs(back[1] - 2) < 1e-5f && std::fabs(back[2] - 3) < 1e-5f,
              "a scaled matrix round-trips");
    }
    std::printf("%d of %d PASS\n", g_n - g_fail, g_n);
    return g_fail ? 1 : 0;
}
