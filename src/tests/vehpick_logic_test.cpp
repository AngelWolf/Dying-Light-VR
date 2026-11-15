// Standalone unit test for src/src/ovr2xr/vehpick_logic.h - NOT part of the DLL build. Exit code = number of failures.
// With a path: vehpick_logic_test <dlvr_vehrec.bin> runs the rule over a real [VEHREC] recording and prints one entry per frame.
#include "vehpick_logic.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace dlvr::vehpick;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

// One simulated frame: the head history, the camera carrying entry `carried`, a car term common to every entry.
struct Sim {
    float hist[kRing] = {};
    void Push(float h) { for (int j = kRing - 1; j > 0; --j) { hist[j] = hist[j - 1]; } hist[0] = h; }
    int Step(State& s, int carried, float term)
    {
        float off[kRing];
        for (int j = 0; j < kRing; ++j) { off[j] = hist[carried] + term - hist[j]; }
        return dlvr::vehpick::Step(s, off, kRing, hist[0], hist[3]);
    }
};
static float Head(int i) { return 60.0f * std::sin(2.0f * 3.14159265f * 0.35f * static_cast<float>(i) / 128.0f); }
// Deterministic tracking noise, about +-0.03 deg.
static float Noise(int i) { const unsigned u = static_cast<unsigned>(i) * 2654435761u; return (static_cast<float>(u >> 8) / 16777216.0f - 0.5f) * 0.06f; }
// A real head's turns: rest, a quarter-second turn to the side, rest, back (the flick the recording is full of).
static float Flick(int i)
{
    const int p = i % 384;
    const float a = p < 256 ? 0.0f : 70.0f * (p < 288 ? static_cast<float>(p - 256) / 32.0f : 1.0f);
    return ((i / 384) % 2) ? a : -a;
}

static void StartsOnK1()
{
    State s;
    CHECK(s.k == 1);
}

static void HeadStillNeverMoves()   // the entries are the same pose: no evidence, however the term moves
{
    State s;
    s.k = 3;
    Sim m;
    for (int i = 0; i < 2000; ++i) {
        m.Push(Noise(i));
        CHECK(m.Step(s, 1, 4.0f * std::sin(static_cast<float>(i) * 0.05f)) == 3);
    }
}

static void NoiseOnASlowTurnDoesNotFlip()   // the margin: near-equal scores are not evidence
{
    State s;
    Sim m;
    int changes = 0, prev = 1;
    for (int i = 0; i < 5120; ++i) {
        m.Push(20.0f * std::sin(2.0f * 3.14159265f * 0.1f * static_cast<float>(i) / 128.0f) + Noise(i));
        const int u = m.Step(s, 1, 0.0f);
        if (u != prev) { ++changes; prev = u; }
    }
    CHECK(changes == 0);
}

static void OneFrameOnAnotherEntryDoesNotMoveIt()   // the recording's 4 out-and-back hops; the clamp keeps one frame from owning a score
{
    State s;
    Sim m;
    for (int i = 0; i < 700; ++i) { m.Push(Flick(i)); m.Step(s, 1, 0.0f); }
    for (int hop = 0; hop < 3; ++hop) {
        for (int i = 0; i < 40; ++i) { m.Push(Flick(256 + i)); m.Step(s, 1, 0.0f); }
        m.Push(Flick(296));
        m.Step(s, 6, 0.0f);
        CHECK(s.k == 1);
    }
}

static void FindsTheCarriedEntry()  // every carried entry from every start, by the head turning
{
    for (int carried = 0; carried < 4; ++carried) {
        for (int start = 0; start < 4; ++start) {
            State s;
            s.k = start;
            Sim m;
            int last = -1;
            for (int i = 0; i < 1280; ++i) { m.Push(Head(i)); last = m.Step(s, carried, 0.0f); }
            CHECK(last == carried);
        }
    }
}

static void CarTermDoesNotMoveIt()  // a real term is common to every entry
{
    State s;
    Sim m;
    int changes = 0, prev = 1;
    for (int i = 0; i < 2560; ++i) {
        m.Push(Head(i));
        const float t = static_cast<float>(i);
        const int u = m.Step(s, 1, 1.2f * std::sin(t * 0.02f) + 0.3f * std::sin(t * 0.15f));
        if (u != prev) { ++changes; prev = u; }
    }
    CHECK(prev == 1);
    CHECK(changes == 0);
}

static void OneOddFrameDoesNotMoveIt()
{
    State s;
    Sim m;
    for (int i = 0; i < 640; ++i) { m.Push(Head(i)); m.Step(s, 1, 0.0f); }
    CHECK(s.k == 1);
    m.Push(Head(640));
    CHECK(m.Step(s, 1, 6.9f) == 1);   // the 58.89 s shape: one 6.9 deg frame
    m.Push(Head(641));
    CHECK(m.Step(s, 1, 0.0f) == 1);
}

static void FollowsACarriedEntryThatChanges()
{
    State s;
    Sim m;
    int i = 0;
    for (; i < 768; ++i) { m.Push(Flick(i) + Noise(i)); m.Step(s, 1, 0.0f); }
    int last = -1;
    for (; i < 1920; ++i) { m.Push(Flick(i) + Noise(i)); last = m.Step(s, 0, 0.0f); }
    CHECK(last == 0);
}

static void ShortRingStaysInside()
{
    State s;
    s.k = 5;
    float off[2] = { 0.0f, 0.0f };
    CHECK(Step(s, off, 2, 0.0f, 0.0f) == 1);
    CHECK(s.k == 1);
    CHECK(Step(s, off, 0, 0.0f, 0.0f) == 0);   // nothing to declare from: entry 0, as RdVehPick's fallback
}

static void ResetKeepsTheHeldEntry()
{
    State s;
    s.k = 2;
    Sim m;
    m.Push(1.0f);
    m.Step(s, 2, 0.0f);
    Reset(s);
    CHECK(s.k == 2);
    for (int j = 0; j < kRing; ++j) { CHECK(!s.haveLo[j]); }
}

static int Replay(const char* path)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) { std::printf("cannot open %s\n", path); return 1; }
    char head[20];
    if (fread(head, 1, 20, f) != 20 || std::memcmp(head, "DLVRVEH1", 8) != 0) { std::printf("not a recording\n"); return 1; }
    unsigned char rec[176];
    State s;
    while (fread(rec, 1, sizeof(rec), f) == sizeof(rec)) {
        float cy, after[8], h[8];
        int n;
        std::memcpy(&cy, rec + 20, 4);
        std::memcpy(&n, rec + 64, 4);
        std::memcpy(after, rec + 80, 32);
        std::memcpy(h, rec + 112, 32);
        float off[kRing];
        for (int j = 0; j < n && j < kRing; ++j) {
            off[j] = Wrap180((cy + 1.5707963f) * 57.2957795f + after[j]);
        }
        std::printf("%d\n", Step(s, off, n, h[0], h[n > 3 ? 3 : (n > 0 ? n - 1 : 0)]));
    }
    fclose(f);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc > 1) { return Replay(argv[1]); }
    StartsOnK1();
    HeadStillNeverMoves();
    NoiseOnASlowTurnDoesNotFlip();
    OneFrameOnAnotherEntryDoesNotMoveIt();
    FindsTheCarriedEntry();
    CarTermDoesNotMoveIt();
    OneOddFrameDoesNotMoveIt();
    FollowsACarriedEntryThatChanges();
    ShortRingStaysInside();
    ResetKeepsTheHeldEntry();
    std::printf("vehpick_logic_test: %d failure(s)\n", g_fail);
    return g_fail;
}
