// Standalone unit test for src/src/ovr2xr/swingshadow_logic.h - NOT part of the DLL build.
// docs/superpowers/specs/2026-10-26-swing-shadow-probe-design.md §4.2-4.3 (the shadow span, the classification).
// Exit code = number of failures.
#include "swingshadow_logic.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace dlvr::swingshadow;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static bool Near(float a, float b, float eps) { return std::fabs(a - b) < eps; }

// A 64-entry ring holding 10 frames at indices 0..9 (head 9), 10 ms apart from t = 0.
struct Ring {
    double t[64] = {};
    bool   ok[64] = {};
    Ring()
    {
        for (int i = 0; i < 10; ++i) { t[i] = 10.0 * i; ok[i] = true; }
    }
};

static void angle_basics()
{
    CHECK(Near(AngleDeg({ 1, 0, 0 }, { 0, 1, 0 }), 90.0f, 1e-3f));
    CHECK(AngleDeg({ 0, 0, 0 }, { 1, 0, 0 }) == -1.0f);
}

static void segseg_parallel_and_crossing()
{
    float s = -1.0f, t = -1.0f;
    CHECK(Near(SegSegDist({ 0, 0, 0 }, { 1, 0, 0 }, { 0, 0.3f, 0 }, { 1, 0.3f, 0 }, &s, &t), 0.3f, 1e-5f));
    CHECK(Near(SegSegDist({ -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 }, &s, &t), 0.0f, 1e-5f));
    CHECK(Near(s, 0.5f, 1e-4f) && Near(t, 0.5f, 1e-4f));
}

static void shadow_pairs_backfill()
{
    Ring r;
    int p[64][2];
    bool trunc = true;
    const int n = ShadowPairs(r.t, r.ok, 10, 9, 64, 35.0, true, p, 64, &trunc);
    CHECK(n == 6);
    CHECK(!trunc);
    CHECK(n >= 1 && p[0][0] == 3 && p[0][1] == 4);
    CHECK(n == 6 && p[5][0] == 8 && p[5][1] == 9);
}

static void shadow_pairs_skip_not_ok()
{
    Ring r;
    r.ok[6] = false;
    int p[64][2];
    bool trunc = true;
    const int n = ShadowPairs(r.t, r.ok, 10, 9, 64, 35.0, true, p, 64, &trunc);
    CHECK(n == 4);
    for (int i = 0; i < n; ++i) { CHECK(p[i][0] != 6 && p[i][1] != 6); }
}

static void shadow_pairs_truncate_like_b1()
{
    Ring r;
    int p[64][2];
    bool trunc = false;
    const int n = ShadowPairs(r.t, r.ok, 10, 9, 64, -100.0, true, p, 64, &trunc);
    CHECK(trunc);
    CHECK(n == 9);
    CHECK(n >= 1 && p[0][0] == 0 && p[0][1] == 1);
}

static void shadow_pairs_no_backfill()
{
    Ring r;
    int p[64][2];
    bool trunc = true;
    const int n = ShadowPairs(r.t, r.ok, 10, 9, 64, 35.0, false, p, 64, &trunc);
    CHECK(n == 1 && p[0][0] == 8 && p[0][1] == 9);
    CHECK(!trunc);
}

static void shadow_pairs_wraps_the_ring()
{
    // head at index 1, frames at 62, 63, 0, 1 (oldest first): the pair indices wrap modulo the ring.
    double t[64] = {};
    bool ok[64] = {};
    t[62] = 0; t[63] = 10; t[0] = 20; t[1] = 30;
    ok[62] = ok[63] = ok[0] = ok[1] = true;
    int p[64][2];
    bool trunc = true;
    const int n = ShadowPairs(t, ok, 4, 1, 64, 5.0, true, p, 64, &trunc);
    CHECK(n == 3);
    CHECK(n == 3 && p[0][0] == 62 && p[0][1] == 63 && p[2][0] == 0 && p[2][1] == 1);
}

static void classify_four_classes()
{
    const Key a{ 0xA, 1 }, b{ 0xB, 2 }, c{ 0xC, 3 };
    CHECK(Classify(&a, 1, &a, 1, true, true).cls == Cls::Both);
    CHECK(Classify(&a, 1, nullptr, 0, true, true).cls == Cls::GameOnly);
    CHECK(Classify(nullptr, 0, &b, 1, true, true).cls == Cls::BladeOnly);
    CHECK(Classify(nullptr, 0, nullptr, 0, true, true).cls == Cls::Neither);
    const Key g2[2] = { a, b }, s2[2] = { b, c };
    const Tally t = Classify(g2, 2, s2, 2, true, true);
    CHECK(t.cls == Cls::Both && t.both == 1 && t.gameOnly == 1 && t.bladeOnly == 1);
    const Tally sp = Classify(&a, 1, &c, 1, true, true);
    CHECK(sp.cls == Cls::Split && sp.gameOnly == 1 && sp.bladeOnly == 1 && sp.both == 0);
    // the element is reported, not compared: the same object on another bone is still the same target
    const Key a2{ 0xA, 9 };
    CHECK(Classify(&a, 1, &a2, 1, true, true).cls == Cls::Both);
}

static void classify_no_b1()
{
    const Key a{ 0xA, 1 };
    CHECK(Classify(&a, 1, &a, 1, false, true).cls == Cls::NoB1);
    CHECK(Classify(nullptr, 0, nullptr, 0, false, false).cls == Cls::NoB1);
}

static void classify_incomplete()
{
    const Key a{ 0xA, 1 };
    CHECK(Classify(&a, 1, nullptr, 0, true, false).cls == Cls::Incomplete);
}

// ===== [SWINGLOS] (spec 2026-10-26-melee-los-probe-design.md §5) =====

// 3 quads of 13 floats: starts at floats 0..2 (prev) and 6..8 (cur); the END points (3..5, 9..11) are huge so a centroid that
// used them would be far off. Hand-computed: starts x = 1,3 / 5,7 / 9,11 -> mean 6; y = 0 everywhere; z = 2 everywhere.
static void los_centroid_uses_starts_only()
{
    float q[39] = {};
    for (int i = 0; i < 3; ++i) {
        float* e = q + 13 * i;
        e[0] = 1.0f + 4.0f * i; e[1] = 0.0f; e[2] = 2.0f;        // prev start
        e[3] = 1000.0f; e[4] = 1000.0f; e[5] = 1000.0f;           // prev end
        e[6] = 3.0f + 4.0f * i; e[7] = 0.0f; e[8] = 2.0f;        // cur start
        e[9] = -1000.0f; e[10] = -1000.0f; e[11] = -1000.0f;      // cur end
        e[12] = 777.0f;                                           // the 13th float (+48 flag etc.)
    }
    float c[3] = { -1.0f, -1.0f, -1.0f };
    CHECK(Centroid(q, 3, c));
    CHECK(Near(c[0], 6.0f, 1e-5f));
    CHECK(Near(c[1], 0.0f, 1e-5f));
    CHECK(Near(c[2], 2.0f, 1e-5f));
    CHECK(!Centroid(q, 0, c));
    CHECK(!Centroid(nullptr, 3, c));
    q[0] = std::nanf("");
    CHECK(!Centroid(q, 3, c));
}

// The search's own rule (C48E30 :365): clear -> every candidate; hit -> only the hit object (coll+24 == cand+0x18) or surface 3.
static void los_verdict_classes()
{
    const unsigned long long t18[2] = { 0x1018, 0x2018 };
    CHECK(LosVerdict(false, 0x9999, 0, t18, 2) == Los::Clear);
    CHECK(LosVerdict(true, 0x2018, 0, t18, 2) == Los::Target);
    CHECK(LosVerdict(true, 0x9999, 3, t18, 2) == Los::Water);
    CHECK(LosVerdict(true, 0x9999, 17, t18, 2) == Los::Blocked);
    CHECK(LosVerdict(true, 0, 0, t18, 2) == Los::BlockedWorld);
    CHECK(LosVerdict(true, 0x1018, 3, nullptr, 0) == Los::Water);
    CHECK(std::string(LosName(Los::BlockedWorld)) == "blocked-world");
}

// A blocked ray 1 (not the target, not water) while the game still hit something = our replica is not the game's ray.
static void los_agree_flags_contradiction()
{
    CHECK(Agree(Los::Clear, 3));
    CHECK(Agree(Los::Target, 1));
    CHECK(Agree(Los::Water, 2));
    CHECK(Agree(Los::Blocked, 0));
    CHECK(Agree(Los::BlockedWorld, 0));
    CHECK(!Agree(Los::Blocked, 1));
    CHECK(!Agree(Los::BlockedWorld, 2));
}

int main()
{
    los_centroid_uses_starts_only();
    los_verdict_classes();
    los_agree_flags_contradiction();
    angle_basics();
    segseg_parallel_and_crossing();
    shadow_pairs_backfill();
    shadow_pairs_skip_not_ok();
    shadow_pairs_truncate_like_b1();
    shadow_pairs_no_backfill();
    shadow_pairs_wraps_the_ring();
    classify_four_classes();
    classify_no_b1();
    classify_incomplete();
    std::printf("swingshadow_logic_test: %d failure(s)\n", g_fail);
    return g_fail;
}
