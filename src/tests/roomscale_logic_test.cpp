// Standalone unit test for src/src/ovr2xr/roomscale_logic.h - NOT part of the DLL build.
// docs/superpowers/plans/2026-10-27-roomscale-physical-crouch.md Task 5. Exit code = number of failures.
#include "roomscale_logic.h"

#include <cmath>
#include <cstdio>

using namespace dlvr::roomscale;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static bool Near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

static void bleed_moves_toward_origin()
{
    float sx = 9.0f, sz = 9.0f;
    BleedStep(0.3f, 0.0f, 0.3f, 0.5f, sx, sz);
    CHECK(Near(sx, 0.15f) && Near(sz, 0.0f));
    BleedStep(0.0f, -0.4f, 0.2f, 1.0f, sx, sz);            // along o, whatever its direction
    CHECK(Near(sx, 0.0f) && Near(sz, -0.2f));
}
static void bleed_never_overshoots()
{
    float sx = 0.0f, sz = 0.0f;
    BleedStep(0.03f, 0.04f, 0.3f, 0.5f, sx, sz);           // |o| 0.05, rate * dt 0.15
    CHECK(Near(sx, 0.03f) && Near(sz, 0.04f));
}
static void bleed_off_at_zero_rate()
{
    float sx = 1.0f, sz = 1.0f;
    BleedStep(0.3f, 0.2f, 0.0f, 0.5f, sx, sz);
    CHECK(sx == 0.0f && sz == 0.0f);
    sx = sz = 1.0f;
    BleedStep(0.3f, 0.2f, 0.3f, 0.0f, sx, sz);             // dt 0 = nothing either
    CHECK(sx == 0.0f && sz == 0.0f);
    sx = sz = 1.0f;
    BleedStep(0.0f, 0.0f, 0.3f, 0.5f, sx, sz);             // at the origin already
    CHECK(sx == 0.0f && sz == 0.0f);
}
static void stick_deadzone()
{
    CHECK(!StickMoving(0.19f, 0.0f));
    CHECK(StickMoving(0.21f, 0.0f));
    CHECK(!StickMoving(0.13f, 0.13f));                     // |(0.13, 0.13)| = 0.184
    CHECK(StickMoving(0.0f, -0.5f));
}
static void free_walk_tracks_body()
{
    const Out o = Update(In{ 0.10f, 0.0f, 0.10f, 0.0f, false, 0.25f });
    CHECK(Near(o.shiftX, 0.10f) && Near(o.shiftZ, 0.0f));
    CHECK(o.cmdX == 0.0f && o.cmdZ == 0.0f);
    CHECK(Near(o.leanM, 0.0f) && Near(o.absorbedM, 0.0f));
}
static void step_commands_the_body()
{
    const Out o = Update(In{ 0.0f, 0.12f, 0.0f, 0.0f, false, 0.25f });   // a real step, body not moved yet
    CHECK(o.shiftX == 0.0f && o.shiftZ == 0.0f);
    CHECK(Near(o.cmdX, 0.0f) && Near(o.cmdZ, 0.12f));
    CHECK(Near(o.leanM, 0.12f) && Near(o.absorbedM, 0.0f));
}
static void wall_lean_clamps()
{
    for (int i = 1; i <= 6; ++i) {                         // the head walks on into a wall the body cannot pass
        const float off = 0.1f * static_cast<float>(i);
        const Out o = Update(In{ off, 0.0f, 0.0f, 0.0f, false, 0.25f });
        CHECK(o.leanM <= 0.25f + 1e-5f);
        const float want = off > 0.25f ? off - 0.25f : 0.0f;
        CHECK(Near(o.absorbedM, want));
        CHECK(Near(o.shiftX, want) && o.shiftZ == 0.0f);   // the absorbed part moves the origin, along the offset
        CHECK(Near(o.cmdX, off));                          // the body is still asked to go there (collision stops it)
    }
}
static void stand_down_no_command()
{
    const Out o = Update(In{ 0.0f, -0.4f, 0.0f, 0.0f, true, 0.25f });
    CHECK(o.cmdX == 0.0f && o.cmdZ == 0.0f);
    CHECK(Near(o.leanM, 0.25f) && Near(o.absorbedM, 0.15f));
    CHECK(Near(o.shiftX, 0.0f) && Near(o.shiftZ, -0.15f));
}
static void tiny_offset_no_command()
{
    const Out o = Update(In{ 0.004f, 0.0f, 0.0f, 0.0f, false, 0.25f });
    CHECK(o.cmdX == 0.0f && o.cmdZ == 0.0f);
    CHECK(Near(o.leanM, 0.004f) && o.absorbedM == 0.0f);
}
static void moved_beyond_offset_is_shifted_out()
{
    // the body overshot (stick + steps): what it moved is always shifted out; the remainder is the other way
    const Out o = Update(In{ 0.10f, 0.0f, 0.15f, 0.0f, false, 0.25f });
    CHECK(Near(o.shiftX, 0.15f));
    CHECK(Near(o.cmdX, -0.05f) && Near(o.leanM, 0.05f));
}

// INV-740: the command dead zone.
static void dead_zone_drops_small_commands()
{
    float x = 0.027f, z = -0.036f;                         // |cmd| 0.045, inside the zone
    DeadZoneCommand(x, z, kCmdDeadZoneM);
    CHECK(x == 0.0f && z == 0.0f);
    x = 0.0f; z = 0.049f;
    DeadZoneCommand(x, z, kCmdDeadZoneM);
    CHECK(x == 0.0f && z == 0.0f);
    x = 0.0f; z = 0.0f;
    DeadZoneCommand(x, z, kCmdDeadZoneM);                  // nothing in, nothing out (no NaN)
    CHECK(x == 0.0f && z == 0.0f);
}
static void dead_zone_shortens_along_itself()
{
    float x = 0.18f, z = 0.24f;                            // |cmd| 0.30 -> 0.25, same direction
    DeadZoneCommand(x, z, kCmdDeadZoneM);
    CHECK(Near(x, 0.15f) && Near(z, 0.20f));
    x = -0.30f; z = 0.0f;
    DeadZoneCommand(x, z, 0.0f);                           // zone 0 = unchanged (the pre-INV-740 command)
    CHECK(Near(x, -0.30f) && z == 0.0f);
    x = -0.30f; z = 0.0f;
    DeadZoneCommand(x, z, -1.0f);                          // a negative zone is taken as 0
    CHECK(Near(x, -0.30f) && z == 0.0f);
}

// FIX 1: ShareAlong must equal PreStepDetour's own arithmetic (roomscale.cpp, copied here verbatim as the reference).
static void ref_prestep_share(float dx, float dz, float gx, float gz, float dt, float ex, float ez, float& sx, float& sz)
{
    sx = 0.0f; sz = 0.0f;
    const float elen = std::sqrt(ex * ex + ez * ez);
    if (elen > 1e-4f && dt > 0.0f) {
        const float rx = dx - gx * dt;
        const float rz = dz - gz * dt;
        float along = (rx * ex + rz * ez) / elen;
        const float maxAlong = elen * dt;
        along = along < 0.0f ? 0.0f : (along > maxAlong ? maxAlong : along);
        sx = ex / elen * along;
        sz = ez / elen * along;
    }
}
static void share_along_equals_prestep()
{
    const float cases[][7] = {
        { 0.006f, 0.0f, 0.0f, 0.0f, 0.02f, 0.3f, 0.0f },     // roomscale only, the full step
        { 0.004f, 0.0f, 0.0f, 0.0f, 0.02f, 0.3f, 0.0f },     // a wall cut the step short
        { 0.020f, 0.010f, 0.5f, 0.5f, 0.02f, 0.2f, -0.1f },  // stick + roomscale
        { -0.010f, 0.0f, 0.0f, 0.0f, 0.02f, 0.3f, 0.0f },    // moved against the extra: clamped to 0
        { 0.050f, 0.0f, 0.0f, 0.0f, 0.02f, 0.3f, 0.0f },     // more than |extra| x dt: clamped to it
        { 0.006f, 0.0f, 0.0f, 0.0f, 0.02f, 0.0f, 0.0f },     // no extra: 0
        { 0.006f, 0.0f, 0.0f, 0.0f, 0.0f, 0.3f, 0.0f },      // dt 0: 0
    };
    for (const auto& c : cases) {
        float ax, az, bx, bz;
        ShareAlong(c[0], c[1], c[2], c[3], c[4], c[5], c[6], ax, az);
        ref_prestep_share(c[0], c[1], c[2], c[3], c[4], c[5], c[6], bx, bz);
        CHECK(ax == bx && az == bz);   // bit-identical to the pre-step
    }
    float sx, sz;
    ShareAlong(0.05f, 0.0f, 0.0f, 0.0f, 0.02f, 0.3f, 0.0f, sx, sz);
    CHECK(Near(sx, 0.006f) && sz == 0.0f);                 // |extra| x dt = 0.3 x 0.02
}
static void intraframe_time_recovers_t()
{
    CHECK(Near(IntraframeTime(0.003f, 0.004f, 0.3f, 0.4f), 0.01f));   // vt = v x 10 ms
    CHECK(IntraframeTime(0.003f, 0.0f, 0.0f, 0.0f) == 0.0f);           // a still body: 0
    CHECK(IntraframeTime(-0.003f, 0.0f, 0.3f, 0.0f) == 0.0f);          // backwards: not believed -> 0
    CHECK(Near(IntraframeTime(0.3f, 0.0f, 0.3f, 0.0f), kMaxIntraS));   // 1 s: clamped
}
static void reported_match_is_exact()
{
    const float ring[3][3] = { { 0.1f, 1.6f, -0.2f }, { 0.11f, 1.6f, -0.21f }, { -0.0f, 1.5f, 0.0f } };
    CHECK(MatchReported(ring, 3, -0.11f, 1.6f, 0.21f) == 1);           // the store is (-x, y, -z)
    CHECK(MatchReported(ring, 3, -0.1f, 1.6f, 0.2f) == 0);
    CHECK(MatchReported(ring, 3, 0.11f, 1.6f, -0.21f) == -1);          // NOT negated: must miss
    CHECK(MatchReported(ring, 3, -0.110001f, 1.6f, 0.21f) == -1);      // one ulp-ish off: must miss
    CHECK(MatchReported(ring, 0, -0.1f, 1.6f, 0.2f) == -1);            // empty ring
}

int main()
{
    share_along_equals_prestep();
    intraframe_time_recovers_t();
    reported_match_is_exact();
    dead_zone_drops_small_commands();
    dead_zone_shortens_along_itself();
    bleed_moves_toward_origin();
    bleed_never_overshoots();
    bleed_off_at_zero_rate();
    stick_deadzone();
    free_walk_tracks_body();
    step_commands_the_body();
    wall_lean_clamps();
    stand_down_no_command();
    tiny_offset_no_command();
    moved_beyond_offset_is_shifted_out();
    std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail;
}
