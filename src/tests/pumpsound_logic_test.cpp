// Standalone unit test for src/src/ovr2xr/pumpsound_logic.h - NOT part of the DLL build.
// docs/superpowers/plans/2026-11-04-manual-reload-pump-shotgun.md Task 3. Exit code = number of failures.
// Compile: cl /nologo /EHsc /W4 /WX /std:c++17 /I src\src\ovr2xr src\src\tests\pumpsound_logic_test.cpp
#include "pumpsound_logic.h"

#include <cstdio>

using namespace dlvr::pumpsound;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)
#define RUN(fn)                                                                  \
    do {                                                                         \
        const int before_ = g_fail;                                              \
        fn();                                                                    \
        std::printf("%s %s\n", g_fail == before_ ? "PASS" : "FAIL", #fn);        \
    } while (0)

namespace {

void SkipsOnlyTheRackSound()
{
    CHECK(PumpSoundSkip("ShotgunB_reload_XX.wav", true));
    CHECK(PumpSoundSkip("shotgunb_reload_00", true));
    CHECK(PumpSoundSkip("SHOTGUNB_RELOAD_00.WAV", true));      // case-insensitive
    CHECK(!PumpSoundSkip("ShotgunB_bullet_load_XX.wav", true));
    CHECK(!PumpSoundSkip("ShotgunB_shot_XX.wav", true));
    CHECK(!PumpSoundSkip("beretta_reload_part_00.wav", true));
}

void SilencedOffPlaysEverything()
{
    CHECK(!PumpSoundSkip("ShotgunB_reload_XX.wav", false));
    CHECK(!PumpSoundSkip("shotgunb_reload_00", false));
    CHECK(!PumpSoundSkip("ShotgunB_shot_XX.wav", false));
}

void NullAndShortNamesPlay()
{
    CHECK(!PumpSoundSkip(nullptr, true));
    CHECK(!PumpSoundSkip("", true));
    CHECK(!PumpSoundSkip("shotgunb_reloa", true));             // one char short of the prefix
    CHECK(!PumpSoundSkip("xshotgunb_reload_00", true));        // a prefix, not a substring
}

}  // namespace

int main()
{
    RUN(SkipsOnlyTheRackSound);
    RUN(SilencedOffPlaysEverything);
    RUN(NullAndShortNamesPlay);
    std::printf("pumpsound_logic_test: %d failure(s)\n", g_fail);
    return g_fail;
}
