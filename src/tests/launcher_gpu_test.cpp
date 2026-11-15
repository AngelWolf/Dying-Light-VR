// launcher_gpu.h - every upscaler preset the card supports (Jay 2026-11-15: "Make sure that users are able to pick ANY of the
// upscaling presets that their card supports"), matching the in-game Anti-Aliasing tab (overlay.cpp DrawDlssTab).
#include <cstdio>
#include <string>
#include <vector>

#include "launcher_gpu.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace dlvr::launcher;
using dlvr::setup::AaOption;
using dlvr::setup::AaWord;

static int Count(const std::vector<AaOption>& o, AaWord m)
{
    int n = 0;
    for (const auto& x : o) { n += x.method == m ? 1 : 0; }
    return n;
}

static bool Has(const std::vector<AaOption>& o, AaWord m, int q)
{
    for (const auto& x : o) {
        if (x.method == m && x.quality == q) { return true; }
    }
    return false;
}

int main()
{
    {   // an RTX 4080: DLSS all five (DLAA .. Ultra Performance) + FSR 3.1 all four; no FSR 4; the default = DLSS Quality
        const auto r = dlvr::setup::Recommend("NVIDIA GeForce RTX 4080", 0x10DE);
        int d = -1;
        const auto o = AllUpscalerOptions(r, "NVIDIA GeForce RTX 4080", 0x10DE, &d);
        CHECK(Count(o, AaWord::Dlss) == 5 && Count(o, AaWord::Fsr3) == 4 && Count(o, AaWord::Fsr4) == 0);
        CHECK(Has(o, AaWord::Dlss, 4) && Has(o, AaWord::Fsr3, 0) && Has(o, AaWord::Fsr3, 3));
        CHECK(d >= 0 && o[static_cast<size_t>(d)].method == AaWord::Dlss && o[static_cast<size_t>(d)].quality == 1);
    }
    {   // an RX 9070 XT: FSR 4 all four + FSR 3.1 all four; no DLSS; default FSR 4 Quality
        const auto r = dlvr::setup::Recommend("AMD Radeon RX 9070 XT", 0x1002);
        int d = -1;
        const auto o = AllUpscalerOptions(r, "AMD Radeon RX 9070 XT", 0x1002, &d);
        CHECK(Count(o, AaWord::Fsr4) == 4 && Count(o, AaWord::Fsr3) == 4 && Count(o, AaWord::Dlss) == 0);
        CHECK(!Has(o, AaWord::Fsr4, 4));   // no Ultra Performance for FSR (in-game tab, PAKGEN refuses it)
        CHECK(d >= 0 && o[static_cast<size_t>(d)].method == AaWord::Fsr4 && o[static_cast<size_t>(d)].quality == 1);
    }
    {   // a GTX 1080 and an RX 6800: FSR 3.1 all four only
        for (const auto& c : {std::pair<const char*, unsigned>{"NVIDIA GeForce GTX 1080", 0x10DE}, {"AMD Radeon RX 6800", 0x1002}}) {
            const auto r = dlvr::setup::Recommend(c.first, c.second);
            int d = -1;
            const auto o = AllUpscalerOptions(r, c.first, c.second, &d);
            CHECK(o.size() == 4 && Count(o, AaWord::Fsr3) == 4);
            CHECK(d >= 0 && o[static_cast<size_t>(d)].method == r.method && o[static_cast<size_t>(d)].quality == r.quality);
        }
    }
    {   // an RTX the table does not know: still offered DLSS (as the setup did), default = the flagged FSR 3.1 Balanced
        const auto r = dlvr::setup::Recommend("NVIDIA GeForce RTX 6090", 0x10DE);
        int d = -1;
        const auto o = AllUpscalerOptions(r, "NVIDIA GeForce RTX 6090", 0x10DE, &d);
        CHECK(Count(o, AaWord::Dlss) == 5 && Count(o, AaWord::Fsr3) == 4);
        CHECK(o[static_cast<size_t>(d)].method == AaWord::Fsr3 && o[static_cast<size_t>(d)].quality == 2);
    }
    {   // reading the files back: DLSS Ultra Performance (4) is a real choice; an FSR 4 on disk is Performance (what PAKGEN runs)
        AaOption a{};
        CHECK(ReadAaOnDisk("dlss\r\n", "4\r\n", &a) && a.method == AaWord::Dlss && a.quality == 4);
        CHECK(ReadAaOnDisk("fsr3\r\n", "4\r\n", &a) && a.method == AaWord::Fsr3 && a.quality == 3);
        CHECK(ReadAaOnDisk("\xEF\xBB\xBF" "fsr4\n", "0\n1920 1080\n", &a) && a.method == AaWord::Fsr4 && a.quality == 0);
        CHECK(!ReadAaOnDisk("taa\r\n", "1\r\n", &a));   // not an upscaler choice here
        CHECK(!ReadAaOnDisk("dlss\r\n", "7\r\n", &a));
        CHECK(!ReadAaOnDisk("", "1", &a));
    }
    std::printf("launcher_gpu_test: %d failure(s)\n", g_fail);
    return g_fail;
}
