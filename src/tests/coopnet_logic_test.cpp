// Standalone unit test for src/src/ovr2xr/coopnet_logic.h (co-op arm-pose channel: packet + jitter buffer) - NOT part of the DLL build.
// Plan docs/superpowers/plans/2026-11-13-coop-arm-pose-channel.md Task 2. Exit code = failures.
// Compile: cl /nologo /EHsc /W4 /WX /std:c++17 /I src\src\ovr2xr src\src\tests\coopnet_logic_test.cpp
#include "coopnet_logic.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace dlvr::coop;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)
#define NEAR(a, b, tol) CHECK(std::fabs((a) - (b)) <= (tol))
#define RUN(fn)                                                                  \
    do {                                                                         \
        const int before_ = g_fail;                                              \
        fn();                                                                    \
        std::printf("%s %s\n", g_fail == before_ ? "PASS" : "FAIL", #fn);        \
    } while (0)

namespace {

// A sample whose every field is distinct; the pose positions follow x = base + t so interpolation is checkable.
PoseSample MakeSample(uint32_t seq, double sendMs, float base)
{
    PoseSample s{};
    s.seq = seq;
    s.sendMs = sendMs;
    for (int i = 0; i < 3; ++i) { s.head[i] = base + static_cast<float>(i); }
    s.head[3] = 0.0f; s.head[4] = 0.0f; s.head[5] = 0.0f; s.head[6] = 1.0f;   // identity quaternion (x y z w)
    for (int h = 0; h < 2; ++h) {
        for (int i = 0; i < 3; ++i) { s.hand[h][i] = base + 10.0f * static_cast<float>(h + 1) + static_cast<float>(i); }
        s.hand[h][3] = 0.0f; s.hand[h][4] = 0.0f; s.hand[h][5] = 0.0f; s.hand[h][6] = 1.0f;
        s.finger[h].index = 0.1f * static_cast<float>(h + 1);
        s.finger[h].thumb = 0.2f * static_cast<float>(h + 1);
        s.finger[h].lower = 0.3f * static_cast<float>(h + 1);
        s.handState[h] = static_cast<uint8_t>(h + 3);
    }
    s.netId = 0x1122334455667788ull;
    s.build = 0xCAFEF00Du;
    s.rootPos[0] = 100.0f + base; s.rootPos[1] = 20.0f; s.rootPos[2] = -300.0f - base;
    s.rootYaw = -2.5f + 0.01f * base;
    // protocol 4: unit bend directions (handState 3 / 4 leave bit 0x20 clear)
    s.elbow[0][0] = 0.0f; s.elbow[0][1] = -1.0f; s.elbow[0][2] = 0.0f;
    s.elbow[1][0] = 0.6f; s.elbow[1][1] = -0.8f; s.elbow[1][2] = 0.0f;
    return s;
}

bool SameSample(const PoseSample& a, const PoseSample& b)
{
    if (a.seq != b.seq || a.sendMs != b.sendMs || a.netId != b.netId || a.build != b.build) { return false; }
    if (std::memcmp(a.head, b.head, sizeof(a.head)) != 0 || std::memcmp(a.hand, b.hand, sizeof(a.hand)) != 0) { return false; }
    if (std::memcmp(a.rootPos, b.rootPos, sizeof(a.rootPos)) != 0 || a.rootYaw != b.rootYaw) { return false; }
    if (std::memcmp(a.elbow, b.elbow, sizeof(a.elbow)) != 0) { return false; }
    for (int h = 0; h < 2; ++h) {
        if (a.finger[h].index != b.finger[h].index || a.finger[h].thumb != b.finger[h].thumb ||
            a.finger[h].lower != b.finger[h].lower || a.handState[h] != b.handState[h]) {
            return false;
        }
    }
    return true;
}

void PackUnpackRoundTrip()
{
    const PoseSample s = MakeSample(42, 1234.5, 7.0f);
    uint8_t buf[kMaxPacket] = {};
    const size_t n = Pack(s, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(n <= 1170);
    CHECK(buf[0] == kCoopProto);
    PoseSample r{};
    CHECK(Unpack(buf, n, &r));
    CHECK(SameSample(s, r));
    CHECK(Pack(s, buf, n - 1) == 0);   // too small a buffer: refused, nothing claimed written
}

void UnpackRejectsShortAndWrongVersion()
{
    const PoseSample s = MakeSample(1, 10.0, 1.0f);
    uint8_t buf[kMaxPacket] = {};
    const size_t n = Pack(s, buf, sizeof(buf));
    PoseSample r{};
    CHECK(!Unpack(buf, n - 1, &r));
    CHECK(!Unpack(buf, 0, &r));
    buf[0] = static_cast<uint8_t>(kCoopProto + 1);
    CHECK(!Unpack(buf, n, &r));
}

// B5 (audit S17 17-2): one corrupt field refuses the whole sample and names why; a no-data hand's zero pose is still accepted.
bool UnpackOf(const PoseSample& s, PoseSample* r, const char** why)
{
    uint8_t buf[kMaxPacket] = {};
    const size_t n = Pack(s, buf, sizeof(buf));
    return Unpack(buf, n, r, why);
}

void UnpackRejectsBadValues()
{
    const float nan = std::nanf("");
    const float inf = HUGE_VALF;
    PoseSample r{};
    const char* why = nullptr;

    PoseSample s = MakeSample(1, 10.0, 1.0f);
    CHECK(UnpackOf(s, &r, &why));
    CHECK(why == nullptr);

    s = MakeSample(1, 10.0, 1.0f); s.head[1] = nan;
    r = PoseSample{}; CHECK(!UnpackOf(s, &r, &why)); CHECK(why != nullptr); CHECK(r.seq == 0);   // out untouched
    s = MakeSample(1, 10.0, 1.0f); s.hand[1][0] = inf;
    CHECK(!UnpackOf(s, &r, &why));
    s = MakeSample(1, 10.0, 1.0f); s.hand[0][0] = -inf;
    CHECK(!UnpackOf(s, &r, &why));
    s = MakeSample(1, 10.0, 1.0f); s.sendMs = std::nan("");
    CHECK(!UnpackOf(s, &r, &why));
    s = MakeSample(1, 10.0, 1.0f); s.rootYaw = nan;
    CHECK(!UnpackOf(s, &r, &why));
    s = MakeSample(1, 10.0, 1.0f); s.finger[1].thumb = nan;
    CHECK(!UnpackOf(s, &r, &why));
    s = MakeSample(1, 10.0, 1.0f); s.rootPos[2] = 2.0e4f;
    CHECK(!UnpackOf(s, &r, &why));
    s = MakeSample(1, 10.0, 1.0f); s.hand[1][1] = -2.0e4f;
    CHECK(!UnpackOf(s, &r, &why));
    s = MakeSample(1, 10.0, 1.0f); s.head[6] = 0.5f;   // length 0.5
    CHECK(!UnpackOf(s, &r, &why));
    s = MakeSample(1, 10.0, 1.0f); s.hand[0][3] = 0.0f; s.hand[0][6] = 0.0f;   // zero quaternion on a hand WITH data
    CHECK(!UnpackOf(s, &r, &why));

    // a hand with NO data (bit 0x80) carries the sender's zero pose: accepted, other checks still apply to the rest
    s = MakeSample(1, 10.0, 1.0f);
    s.handState[0] = static_cast<uint8_t>(s.handState[0] | kHandNoDataBit);
    for (int i = 0; i < 7; ++i) { s.hand[0][i] = 0.0f; }
    CHECK(UnpackOf(s, &r, &why));
    s.hand[0][2] = nan;   // ... but still never a NaN
    CHECK(!UnpackOf(s, &r, &why));

    // near-unit quaternion accepted and normalised; finger weights clamped
    s = MakeSample(1, 10.0, 1.0f);
    s.hand[1][6] = 1.05f;
    s.finger[0].index = 1.7f; s.finger[1].lower = -0.4f;
    CHECK(UnpackOf(s, &r, &why));
    NEAR(r.hand[1][6], 1.0f, 1e-6f);
    NEAR(r.finger[0].index, 1.0f, 0.0f);
    NEAR(r.finger[1].lower, 0.0f, 0.0f);

    // protocol 4: an elbow direction the sender claims must be unit; a claimed-none one only finite
    s = MakeSample(1, 10.0, 1.0f); s.elbow[1][2] = nan;
    CHECK(!UnpackOf(s, &r, &why));
    s = MakeSample(1, 10.0, 1.0f); s.elbow[0][1] = -0.3f;   // length 0.3, bit 0x20 clear
    CHECK(!UnpackOf(s, &r, &why));
    s.handState[0] = static_cast<uint8_t>(s.handState[0] | kHandNoElbowBit);   // ... unless it says it has none
    CHECK(UnpackOf(s, &r, &why));
    s.elbow[0][0] = inf;                                     // ... but never Inf
    CHECK(!UnpackOf(s, &r, &why));
    s = MakeSample(1, 10.0, 1.0f); s.elbow[1][0] = 0.63f;    // near-unit: accepted, normalised
    CHECK(UnpackOf(s, &r, &why));
    NEAR(std::sqrt(r.elbow[1][0] * r.elbow[1][0] + r.elbow[1][1] * r.elbow[1][1] + r.elbow[1][2] * r.elbow[1][2]), 1.0f, 1e-5f);
}

void AcceptRejectsOtherBuild()
{
    JitterBuffer jb;
    CHECK(jb.Accept(kCoopProto, 77u, 77u));
    CHECK(!jb.Accept(kCoopProto, 77u, 78u));
    CHECK(!jb.Accept(static_cast<uint8_t>(kCoopProto + 1), 77u, 77u));
}

// Deterministic jitter in [-4, +4] ms.
double Jit(int i) { return static_cast<double>(((i * 7919) % 9) - 4); }

void PlaysBehindNewestByAdaptiveDelay()
{
    JitterBuffer jb;
    jb.SetParams(500.0f, 150.0f, 0.0f);
    const double transit = 30.0;
    for (int i = 0; i < 200; ++i) {
        const double send = 1000.0 + 11.1 * i;
        jb.Push(MakeSample(static_cast<uint32_t>(i), send, static_cast<float>(i)), send + transit + Jit(i));
    }
    const float d = jb.PlaybackDelayMs();
    CHECK(d >= 8.0f);
    CHECK(d <= 25.0f);
    jb.SetParams(500.0f, 150.0f, 40.0f);   // delayMinMs is a floor
    CHECK(jb.PlaybackDelayMs() >= 40.0f);
}

void InterpolatesBetweenBracketingSamples()
{
    JitterBuffer jb;
    jb.SetParams(500.0f, 150.0f, 0.0f);
    PoseSample a = MakeSample(1, 1000.0, 0.0f);
    PoseSample b = MakeSample(2, 1010.0, 10.0f);
    // b's head rotated 90 deg about +Y: (0, sin45, 0, cos45)
    b.head[4] = std::sqrt(0.5f); b.head[6] = std::sqrt(0.5f);
    jb.Push(a, 1000.0);
    jb.Push(b, 1010.0);
    PoseSample m{};
    float w = 0.0f;
    CHECK(jb.SampleAtSenderTime(1005.0, &m));
    NEAR(m.head[0], 5.0f, 1e-5f);                       // position lerp, midpoint
    NEAR(m.hand[1][2], 0.0f + 20.0f + 2.0f + 5.0f, 1e-5f);
    // slerp midpoint of identity and 90 deg about Y = 45 deg about Y: (0, sin22.5, 0, cos22.5)
    NEAR(m.head[4], std::sin(3.14159265f / 8.0f), 1e-5f);
    NEAR(m.head[6], std::cos(3.14159265f / 8.0f), 1e-5f);
    (void)w;
}

void OutOfOrderSampleDropped()
{
    JitterBuffer jb;
    jb.SetParams(500.0f, 150.0f, 0.0f);
    jb.Push(MakeSample(5, 1050.0, 50.0f), 1050.0);
    jb.Push(MakeSample(6, 1060.0, 60.0f), 1060.0);
    const int before = jb.Count();
    jb.Push(MakeSample(4, 1040.0, 40.0f), 1061.0);   // older seq arrives late
    CHECK(jb.Count() == before);
    PoseSample m{};
    CHECK(jb.SampleAtSenderTime(1040.0, &m));        // before the oldest kept: holds the oldest, never the dropped one
    NEAR(m.head[0], 50.0f, 1e-5f);
}

void BurstThenGapHoldsLastThenEases()
{
    JitterBuffer jb;
    jb.SetParams(500.0f, 150.0f, 0.0f);
    // steady stream to settle the weight at 1
    double now = 0.0;
    for (int i = 0; i < 100; ++i) {
        now = 11.0 * i;
        jb.Push(MakeSample(static_cast<uint32_t>(i), now, static_cast<float>(i)), now);
        PoseSample o{}; float w = 0.0f;
        jb.Sample(now, &o, &w);
    }
    const double last = now;
    // burst of 5 samples within 1 ms, then silence
    for (int k = 0; k < 5; ++k) {
        jb.Push(MakeSample(static_cast<uint32_t>(100 + k), last + 11.0 * (k + 1), static_cast<float>(100 + k)), last + 0.2 * k);
    }
    const double lastArrive = last + 0.2 * 4;        // the timeout and the ease count from the LAST packet's arrival
    float prevX = -1e9f, w = 1.0f;
    bool backwards = false;
    for (double t = last; t <= last + 700.0; t += 5.0) {
        PoseSample o{};
        jb.Sample(t, &o, &w);
        if (o.head[0] < prevX - 1e-4f) { backwards = true; }
        prevX = o.head[0];
        if (t <= lastArrive + 500.0) { NEAR(w, 1.0f, 1e-4f); }
        if (t >= lastArrive + 650.0) { NEAR(w, 0.0f, 1e-4f); }
    }
    CHECK(!backwards);
}

void DataReturnEasesBackIn()
{
    JitterBuffer jb;
    jb.SetParams(500.0f, 150.0f, 0.0f);
    PoseSample o{};
    float w = -1.0f;
    jb.Sample(0.0, &o, &w);
    NEAR(w, 0.0f, 1e-6f);                            // no data yet: the game's animation
    jb.Push(MakeSample(1, 0.0, 0.0f), 0.0);
    jb.Sample(0.0, &o, &w);
    double t = 0.0;
    for (; t <= 75.0; t += 5.0) {
        jb.Push(MakeSample(static_cast<uint32_t>(2 + static_cast<int>(t)), t, 0.0f), t);
        jb.Sample(t, &o, &w);
    }
    NEAR(w, 0.5f, 0.04f);                            // half way at 75 ms
    for (; t <= 160.0; t += 5.0) {
        jb.Push(MakeSample(static_cast<uint32_t>(2 + static_cast<int>(t)), t, 0.0f), t);
        jb.Sample(t, &o, &w);
    }
    NEAR(w, 1.0f, 1e-4f);                            // fully on after 150 ms
}

// After RUN CO1 (trace 20261113_2035): the sender's world root travels in the packet and is interpolated like the pose positions.
void RootPosTravels()
{
    static_assert(kCoopProto == 4, "protocol 4 carries rootPos + rootYaw + elbow (trace 20261115_1735)");
    PoseSample a = MakeSample(1, 1000.0, 0.0f), b = MakeSample(2, 1010.0, 10.0f);
    uint8_t buf[kMaxPacket] = {};
    PoseSample r{};
    CHECK(Unpack(buf, Pack(a, buf, sizeof(buf)), &r));
    NEAR(r.rootPos[0], 100.0f, 0.0f);
    NEAR(r.rootPos[2], -300.0f, 0.0f);
    NEAR(r.rootYaw, -2.5f, 0.0f);
    uint8_t old[kMaxPacket] = {};
    std::memcpy(old, buf, sizeof(old));
    old[0] = 2;                                      // a protocol-2 peer = no data
    CHECK(!Unpack(old, sizeof(old), &r));
    old[0] = 3;                                      // ... and a protocol-3 one (the CO3 package)
    CHECK(!Unpack(old, sizeof(old), &r));
    JitterBuffer jb;
    jb.SetParams(500.0f, 150.0f, 0.0f);
    jb.Push(a, 1000.0);
    jb.Push(b, 1010.0);
    PoseSample m{};
    CHECK(jb.SampleAtSenderTime(1005.0, &m));
    NEAR(m.rootPos[0], 105.0f, 1e-4f);   // midpoint of 100 and 110
    NEAR(m.rootPos[2], -305.0f, 1e-4f);
    NEAR(m.rootYaw, -2.45f, 1e-5f);      // protocol 4: the yaw is interpolated too (was the nearer sample's)
}

// Protocol 4: the yaw places the hands, so it is interpolated - the SHORT way round across +-180.
void YawLerpsShortestArc()
{
    const float d = 0.0174532925f;
    NEAR(LerpAngle(10.0f * d, 30.0f * d, 0.5f), 20.0f * d, 1e-5f);
    const float m = LerpAngle(179.0f * d, -179.0f * d, 0.5f);   // through 180, not 0
    NEAR(std::fabs(m), 180.0f * d, 1e-4f);
    NEAR(LerpAngle(-179.0f * d, 179.0f * d, 0.25f), -179.5f * d, 1e-4f);
    NEAR(LerpAngle(1.0f, 2.0f, 0.0f), 1.0f, 1e-6f);
    NEAR(LerpAngle(1.0f, 2.0f, 1.0f), 2.0f, 1e-6f);
    PoseSample a = MakeSample(1, 1000.0, 0.0f), b = MakeSample(2, 1010.0, 10.0f);
    a.rootYaw = 179.0f * d; b.rootYaw = -177.0f * d;
    JitterBuffer jb;
    jb.SetParams(500.0f, 150.0f, 0.0f);
    jb.Push(a, 1000.0);
    jb.Push(b, 1010.0);
    PoseSample o{};
    CHECK(jb.SampleAtSenderTime(1005.0, &o));
    NEAR(std::fabs(o.rootYaw), 179.0f * d, 1e-4f);   // the midpoint of 179 and 183 (= -177) is 181 = -179
}

// Protocol 4: the elbow travels, is nlerped when both samples carry one, and taken from the nearer sample when one does not.
void ElbowTravelsAndLerps()
{
    PoseSample a = MakeSample(1, 1000.0, 0.0f), b = MakeSample(2, 1010.0, 10.0f);
    a.elbow[0][0] = 1.0f; a.elbow[0][1] = 0.0f; a.elbow[0][2] = 0.0f;
    b.elbow[0][0] = 0.0f; b.elbow[0][1] = 0.0f; b.elbow[0][2] = 1.0f;
    uint8_t buf[kMaxPacket] = {};
    PoseSample r{};
    CHECK(Unpack(buf, Pack(a, buf, sizeof(buf)), &r));
    NEAR(r.elbow[0][0], 1.0f, 0.0f);
    NEAR(r.elbow[1][1], -0.8f, 0.0f);
    JitterBuffer jb;
    jb.SetParams(500.0f, 150.0f, 0.0f);
    jb.Push(a, 1000.0);
    jb.Push(b, 1010.0);
    PoseSample m{};
    CHECK(jb.SampleAtSenderTime(1005.0, &m));
    NEAR(m.elbow[0][0], std::sqrt(0.5f), 1e-5f);   // half way between +X and +Z, unit
    NEAR(m.elbow[0][2], std::sqrt(0.5f), 1e-5f);
    // b has none for the left arm: before the midpoint the nearer sample is a (its direction, its bit clear) ...
    b.handState[0] = static_cast<uint8_t>(b.handState[0] | kHandNoElbowBit);
    JitterBuffer jb2;
    jb2.SetParams(500.0f, 150.0f, 0.0f);
    jb2.Push(a, 1000.0);
    jb2.Push(b, 1010.0);
    CHECK(jb2.SampleAtSenderTime(1003.0, &m));
    NEAR(m.elbow[0][0], 1.0f, 0.0f);
    CHECK((m.handState[0] & kHandNoElbowBit) == 0);
    // ... after it, b (no direction, the bit set) - never a blend toward a direction the sender did not have
    CHECK(jb2.SampleAtSenderTime(1007.0, &m));
    CHECK((m.handState[0] & kHandNoElbowBit) != 0);
}

// Item 2: no keep distance; released only when stale, taken, or an unbound body is a clearly nearer match.
void KeepBoundPeerRules()
{
    int why = -1;
    CHECK(KeepBoundPeer(true, false, 4.06f, -1.0f, 2.0f, 1.0f, &why) && why == 0);    // CO3's 3rd player at 4.06 m: KEPT now
    CHECK(KeepBoundPeer(true, false, 25.0f, -1.0f, 2.0f, 1.0f, &why) && why == 0);    // any distance while it sends
    CHECK(!KeepBoundPeer(false, false, 0.5f, -1.0f, 2.0f, 1.0f, &why) && why == 1);   // stale
    CHECK(!KeepBoundPeer(true, true, 0.5f, -1.0f, 2.0f, 1.0f, &why) && why == 2);     // another body holds it
    CHECK(!KeepBoundPeer(true, false, 3.0f, 0.4f, 2.0f, 1.0f, &why) && why == 3);     // an unbound body 0.4 m from it, we are 3.0
    // a body PASSING NEAR another (4 players): the other is not clearly nearer -> kept
    CHECK(KeepBoundPeer(true, false, 1.2f, 0.6f, 2.0f, 1.0f, &why) && why == 0);      // 0.6 m nearer < the 1.0 m margin
    CHECK(KeepBoundPeer(true, false, 6.0f, 2.5f, 2.0f, 1.0f, &why) && why == 0);      // the other is outside the bind range itself
    // three partners: each body keeps its own peer although all three sent roots sit within 2 m of each other
    const float body[3][3] = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } };
    const float pos[3][3] = { { 0.1f, 0.0f, 0.0f }, { 1.1f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.1f } };
    bool have[3] = { true, true, true }, taken[3] = { false, false, false };
    int bound[3];
    for (int i = 0; i < 3; ++i) {
        bound[i] = NearestPeer(body[i], pos, have, taken, 3, 2.0f);
        CHECK(bound[i] == i);
        taken[bound[i]] = true;
    }
}

// The position key: the nearest peer within range that no other body holds; -1 when none qualifies.
void NearestPeerPicksClosestFree()
{
    const float body[3] = { 10.0f, 0.0f, 10.0f };
    const float pos[3][3] = { { 10.5f, 0.0f, 10.0f }, { 10.2f, 0.0f, 10.1f }, { 40.0f, 0.0f, 40.0f } };
    bool have[3] = { true, true, true };
    bool taken[3] = { false, false, false };
    CHECK(NearestPeer(body, pos, have, taken, 3, 2.0f) == 1);   // 0.22 m beats 0.5 m
    taken[1] = true;
    CHECK(NearestPeer(body, pos, have, taken, 3, 2.0f) == 0);   // the nearest is held by another body
    have[0] = false;
    CHECK(NearestPeer(body, pos, have, taken, 3, 2.0f) == -1);  // the only free one is 42 m away - out of range
    CHECK(NearestPeer(body, pos, have, taken, 3, 100.0f) == 2); // ... unless the range allows it
    CHECK(NearestPeer(body, pos, have, taken, 0, 2.0f) == -1);
}

// Plan Task 4 Step 3: world -> body frame -> world is exact, for yaw 0 / 90 / 179 deg.
void ToBodyFrameRoundTrip()
{
    const float yaws[3] = { 0.0f, 1.57079633f, 179.0f * 0.0174532925f };
    // a pose: position + a unit quaternion (30 deg about an off-axis direction)
    const float ax = 0.3f, ay = 0.8f, az = -0.52f;
    const float al = std::sqrt(ax * ax + ay * ay + az * az);
    const float s = std::sin(0.2617994f), c = std::cos(0.2617994f);
    const float pose[7] = { 0.31f, -0.12f, 0.47f, s * ax / al, s * ay / al, s * az / al, c };
    for (float y : yaws) {
        float b[7] = {}, w[7] = {};
        ToBodyFrame(y, pose, b);
        FromBodyFrame(y, b, w);
        for (int i = 0; i < 7; ++i) { NEAR(w[i], pose[i], 1e-5f); }
    }
}

// The sample's frame reproduces the solve: anchor + Ry(rootYaw) x [Ry(bodyYaw - rootYaw) x off] == anchor + Ry(bodyYaw) x off (DEV
// boneblend.cpp :11896), and qY(rootYaw) x [qY(bodyYaw - rootYaw) x q] has the matrix Ry(bodyYaw) x R(q) (:11551-11554).
void BodyFrameMatchesSolve()
{
    const float bodyYaw = 0.9f, rootYaw = -2.3f;
    const float s = std::sin(0.4f), c = std::cos(0.4f);
    const float q[4] = { s * 0.6f, 0.0f, s * 0.8f, c };
    const float pose[7] = { 0.21f, -0.35f, 0.44f, q[0], q[1], q[2], q[3] };
    float sent[7] = {}, rebuilt[7] = {};
    ToBodyFrame(rootYaw - bodyYaw, pose, sent);      // = rotate by (bodyYaw - rootYaw), what the sender puts on the wire
    FromBodyFrame(rootYaw, sent, rebuilt);           // the receiver's own root yaw
    const float cby = std::cos(bodyYaw), sby = std::sin(bodyYaw);
    // position: the solve's x' = c*x + s*z, z' = -s*x + c*z
    NEAR(rebuilt[0], cby * pose[0] + sby * pose[2], 1e-5f);
    NEAR(rebuilt[1], pose[1], 1e-5f);
    NEAR(rebuilt[2], -sby * pose[0] + cby * pose[2], 1e-5f);
    // rotation: matrix of the rebuilt quaternion vs Ry x R(q), built exactly as the solve builds them
    auto mat = [](const float* qq, float m[9]) {
        const float x = qq[0], y = qq[1], z = qq[2], w = qq[3];
        m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y - z * w);     m[2] = 2 * (x * z + y * w);
        m[3] = 2 * (x * y + z * w);     m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z - x * w);
        m[6] = 2 * (x * z - y * w);     m[7] = 2 * (y * z + x * w);     m[8] = 1 - 2 * (x * x + y * y);
    };
    float cq[9], got[9];
    mat(q, cq);
    mat(rebuilt + 3, got);
    const float ry[9] = { cby, 0, sby, 0, 1, 0, -sby, 0, cby };
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k) {
            const float want = ry[r * 3 + 0] * cq[0 * 3 + k] + ry[r * 3 + 1] * cq[1 * 3 + k] + ry[r * 3 + 2] * cq[2 * 3 + k];
            NEAR(got[r * 3 + k], want, 1e-5f);
        }
    }
}

}  // namespace

int main()
{
    RUN(PackUnpackRoundTrip);
    RUN(UnpackRejectsShortAndWrongVersion);
    RUN(UnpackRejectsBadValues);
    RUN(AcceptRejectsOtherBuild);
    RUN(PlaysBehindNewestByAdaptiveDelay);
    RUN(InterpolatesBetweenBracketingSamples);
    RUN(OutOfOrderSampleDropped);
    RUN(BurstThenGapHoldsLastThenEases);
    RUN(DataReturnEasesBackIn);
    RUN(RootPosTravels);
    RUN(YawLerpsShortestArc);
    RUN(ElbowTravelsAndLerps);
    RUN(KeepBoundPeerRules);
    RUN(NearestPeerPicksClosestFree);
    RUN(ToBodyFrameRoundTrip);
    RUN(BodyFrameMatchesSolve);
    std::printf("%d failure(s)\n", g_fail);
    return g_fail;
}
