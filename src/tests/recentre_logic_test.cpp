// Standalone unit test for src/src/ovr2xr/recentre_logic.h - NOT part of the DLL build.
// docs/superpowers/specs/2026-10-20-recentre-absorb-yaw-design.md §6. Exit code = number of failures.
#include "recentre_logic.h"

#include <cstdio>

using namespace dlvr::recentre;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static bool Near(float a, float b, float tol) { return std::fabs(a - b) < tol; }

// A head (or space) turned `yawDeg` about +Y (left positive), then pitched `pitchDeg` about its own X (up positive).
static void HeadQuat(float yawDeg, float pitchDeg, float q[4])
{
    const float y = yawDeg * 0.017453292f, p = pitchDeg * 0.017453292f;
    const float cy = std::cos(y * 0.5f), sy = std::sin(y * 0.5f), cp = std::cos(p * 0.5f), sp = std::sin(p * 0.5f);
    q[0] = cy * sp; q[1] = sy * cp; q[2] = -sy * sp; q[3] = cy * cp;   // qYaw * qPitch
}

// The convention the header documents: a pose located in appSpace has yaw = yaw_in_reference - base.
static float HeadInApp(float headInRefDeg, float baseDeg) { return Wrap180(headInRefDeg - baseDeg); }

static void exact_path_keeps_head_yaw()
{
    // Physical head yaw 30 in the old LOCAL (base 0 -> 30 in appSpace). The runtime turns LOCAL by +90: the new LOCAL's pose in
    // the old one has yaw +90, so the head in the NEW LOCAL reads 30 - 90 = -60.
    const float base = NewBaseYaw(0.0f, true, 90.0f, 0.0f, 0.0f);
    CHECK(Near(base, -90.0f, 1e-4f));
    CHECK(Near(HeadInApp(30.0f - 90.0f, base), 30.0f, 1e-4f));
}

static void fallback_path_same()
{
    // No exact pose (VDXR, INV-300): the head read 30 in appSpace last frame and -60 now (the jump).
    CHECK(Near(NewBaseYaw(0.0f, false, 0.0f, -60.0f, 30.0f), -90.0f, 1e-4f));
    // 0.8 deg of real head turn inside that frame stays inside the one-frame bound.
    CHECK(Near(NewBaseYaw(0.0f, false, 0.0f, -59.2f, 30.0f), -90.0f, 0.8001f));
}

static void two_events_one_absorb()
{
    Pending p;
    Queue(p, true, 100, true, 40.0f);
    Queue(p, true, 117, true, 50.0f);
    CHECK(p.has);
    CHECK(p.changeTime == 117);
    CHECK(Near(p.poseYawDeg, 90.0f, 1e-4f));
    CHECK(p.poseValid);
    CHECK(!Due(p, 116));
    CHECK(Due(p, 117));
    // One invalid pose in the pair makes the whole absorb take the fallback.
    Pending q;
    Queue(q, true, 100, true, 40.0f);
    Queue(q, true, 117, false, 0.0f);
    CHECK(!q.poseValid);
}

static void queue_ignores_other_type()
{
    Pending p;
    Queue(p, false, 100, true, 40.0f);
    CHECK(!p.has);
    CHECK(!Due(p, 1000));
}

static void wraps_across_180()
{
    CHECK(Near(NewBaseYaw(170.0f, true, -30.0f, 0.0f, 0.0f), -160.0f, 1e-4f));
    CHECK(Near(NewBaseYaw(-170.0f, false, 0.0f, 175.0f, -175.0f), 180.0f, 1e-3f) ||
          Near(NewBaseYaw(-170.0f, false, 0.0f, 175.0f, -175.0f), -180.0f, 1e-3f));   // jump -10 across the seam
}

static void sign_control()
{
    // The WRONG sign (base + poseYaw) would put the head at 30 - 180 = -150: the jump doubled. The real formula must not.
    const float wrong = Wrap180(0.0f + 90.0f);
    CHECK(Near(HeadInApp(30.0f - 90.0f, wrong), -150.0f, 1e-4f));            // the control can fail ...
    CHECK(!Near(HeadInApp(30.0f - 90.0f, NewBaseYaw(0.0f, true, 90.0f, 0.0f, 0.0f)), -150.0f, 1.0f));   // ... ours does not
}

static void base_compose_after_absorb()
{
    // The game's own recentre (ovr_RecenterTrackingOrigin) with the head at yaw 25 IN appSpace, base -90: the new base must face
    // that head, i.e. base + 25 in the reference space.
    CHECK(Near(ComposeBaseYaw(-90.0f, 25.0f), -65.0f, 1e-4f));
    CHECK(Near(ComposeBaseYaw(170.0f, 20.0f), -170.0f, 1e-4f));
}

static void yaw_of_pitched_head()
{
    float q[4];
    HeadQuat(30.0f, -40.0f, q);
    CHECK(Near(YawDegAboutY(q), 30.0f, 1e-3f));
    HeadQuat(-120.0f, 25.0f, q);
    CHECK(Near(YawDegAboutY(q), -120.0f, 1e-3f));
    float r[4];
    QuatFromYawDeg(30.0f, r);
    CHECK(Near(YawDegAboutY(r), 30.0f, 1e-4f));
    CHECK(Near(r[0], 0.0f, 1e-7f) && Near(r[2], 0.0f, 1e-7f));
    CHECK(Near(r[1] * r[1] + r[3] * r[3], 1.0f, 1e-6f));
}

// v2 (run 1, 2026-10-20): VDXR moves the space a few frames AFTER the event - the absorb watches for the jump.
static void jump_threshold()
{
    CHECK(!IsRecentreJump(6.0f, 0.02f));     // a fast real head turn: ~500 deg/s at 90 Hz = 5.6 deg/frame
    CHECK(!IsRecentreJump(-9.9f, 0.099f));
    CHECK(IsRecentreJump(10.5f, 0.0f));
    CHECK(IsRecentreJump(-93.5f, 0.0f));     // run 1's measured step
    CHECK(IsRecentreJump(0.0f, 0.11f));      // a position-only recentre (already facing forward)
}

static void watch_window()
{
    Pending p;
    Queue(p, true, 1000, false, 0.0f);
    CHECK(!WatchExpired(p, 1000 + kWatchNs));
    CHECK(WatchExpired(p, 1000 + kWatchNs + 1));
    CHECK(!WatchExpired(Pending{}, 1000000000000LL));   // nothing queued = nothing to expire
    // A second hold inside the window restarts it from the later changeTime.
    Queue(p, true, 1000 + kWatchNs / 2, false, 0.0f);
    CHECK(!WatchExpired(p, 1000 + kWatchNs + 1));
}

// v3 (run 2, 2026-10-20): the move can land BEFORE the event (VDXR reads the flag only in xrWaitFrame) - look back.
static const long long kFrame = 11111111;   // 90 Hz in ns

static void Fill(HeadRing& r, int frames, long long t0, float yaw0, float yawPerFrame)
{
    for (int i = 0; i < frames; ++i) {
        HeadPush(r, t0 + i * kFrame, Wrap180(yaw0 + i * yawPerFrame), 0.0f, 1.6f, 0.0f);   // as YawDegAboutY reports it
    }
}

static void lookback_finds_step_before_event()
{
    HeadRing r;
    Fill(r, 10, 0, 90.0f, 0.5f);                     // turned 90 left, drifting 0.5 deg/frame
    HeadPush(r, 10 * kFrame, 0.1f, 0.0f, 1.6f, 0.0f);   // the runtime re-origins: yaw 94.5 -> 0.1 in one frame
    HeadPush(r, 11 * kFrame, 0.6f, 0.0f, 1.6f, 0.0f);
    const long long changeTime = 12 * kFrame;        // the event arrives a frame later
    Step s{};
    CHECK(FindRecentreStep(r, changeTime - kLookbackNs, s));
    CHECK(Near(s.yawStepDeg, 0.1f - 94.5f, 1e-3f));
    CHECK(s.t == 10 * kFrame);                        // the AFTER sample of the step
    CHECK(s.t - changeTime == -2 * kFrame);
}

static void lookback_ignores_steps_outside_window()
{
    HeadRing r;
    HeadPush(r, 0, 90.0f, 0.0f, 1.6f, 0.0f);
    HeadPush(r, kFrame, 0.0f, 0.0f, 1.6f, 0.0f);      // an old jump
    Fill(r, 40, 2 * kFrame, 0.0f, 0.2f);
    Step s{};
    CHECK(!FindRecentreStep(r, 42 * kFrame - kLookbackNs, s));   // 40 frames later: outside 250 ms
}

static void lookback_position_only_and_largest()
{
    HeadRing r;
    HeadPush(r, 0, 5.0f, 0.30f, 1.6f, -0.20f);
    HeadPush(r, kFrame, 5.5f, 0.0f, 1.6f, 0.0f);      // position-only recentre: 0.36 m step
    Step s{};
    CHECK(FindRecentreStep(r, 0, s));
    CHECK(Near(s.yawStepDeg, 0.5f, 1e-4f));
    CHECK(s.posStepM > 0.35f);
    HeadPush(r, 2 * kFrame, -60.0f, 0.0f, 1.6f, 0.0f); // a bigger yaw step later wins
    CHECK(FindRecentreStep(r, 0, s));
    CHECK(Near(s.yawStepDeg, -65.5f, 1e-3f));
}

static void ring_same_time_overwrites_and_wraps()
{
    HeadRing r;
    HeadPush(r, 0, 10.0f, 0.0f, 1.6f, 0.0f);
    HeadPush(r, 0, 11.0f, 0.0f, 1.6f, 0.0f);          // a second locate of the same frame replaces the first
    CHECK(r.n == 1);
    Fill(r, kHeadRingSize + 5, kFrame, 11.0f, 0.1f);  // more than the ring holds: oldest dropped, no step seen
    CHECK(r.n == kHeadRingSize);
    Step s{};
    CHECK(!FindRecentreStep(r, 0, s));
    // After the wrap the order must still be chronological: a step pushed last is found, with its own time.
    const long long tEnd = (kHeadRingSize + 6) * kFrame;
    HeadPush(r, tEnd, 120.0f, 0.0f, 1.6f, 0.0f);
    CHECK(FindRecentreStep(r, 0, s));
    CHECK(s.t == tEnd);
    CHECK(HeadAt(r, 0).t < HeadAt(r, r.n - 1).t);
    HeadClear(r);
    CHECK(r.n == 0);
    CHECK(!FindRecentreStep(r, 0, s));
}

static void no_step_in_steady_turn()
{
    HeadRing r;
    Fill(r, 25, 0, 150.0f, 6.0f);                     // a fast real turn across the +-180 seam (180 -> -174 is a 6 deg step)
    Step s{};
    CHECK(!FindRecentreStep(r, 0, s));
}

int main()
{
    lookback_finds_step_before_event();
    lookback_ignores_steps_outside_window();
    lookback_position_only_and_largest();
    ring_same_time_overwrites_and_wraps();
    no_step_in_steady_turn();
    jump_threshold();
    watch_window();
    exact_path_keeps_head_yaw();
    fallback_path_same();
    two_events_one_absorb();
    queue_ignores_other_type();
    wraps_across_180();
    sign_control();
    base_compose_after_absorb();
    yaw_of_pitched_head();
    std::printf("%s (%d failure(s))\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail;
}
