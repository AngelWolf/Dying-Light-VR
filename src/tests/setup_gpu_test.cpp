// Standalone unit test for src/src/setup/setup_gpu.h - NOT part of any DLL build.
// docs/superpowers/specs/2026-10-17-installer-design.md 5.3 (the card table). Exit code = number of failures.
#include "setup_gpu.h"

#include <cstdio>
#include <cstring>

using namespace dlvr::setup;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static const unsigned kNv = 0x10DE;
static const unsigned kAmd = 0x1002;
static const unsigned kIntel = 0x8086;

static bool Is(const Recommendation& r, AaWord m, int q, const char* vd, bool askDlaa, bool recognised)
{
    return r.method == m && r.quality == q && std::strcmp(r.vdAdvice, vd) == 0 && r.askDlaa == askDlaa &&
           r.recognised == recognised;
}

static void spec_rows()
{
    CHECK(Is(Recommend("NVIDIA GeForce RTX 3060 Ti", kNv), AaWord::Dlss, 1, "Medium / High", false, true));
    CHECK(Is(Recommend("NVIDIA GeForce RTX 3080", kNv), AaWord::Dlss, 1, "High", false, true));
    CHECK(Is(Recommend("NVIDIA GeForce RTX 4070", kNv), AaWord::Dlss, 1, "Ultra", false, true));
    CHECK(Is(Recommend("NVIDIA GeForce RTX 4080", kNv), AaWord::Dlss, 1, "Ultra", false, true));
    CHECK(Is(Recommend("NVIDIA GeForce RTX 4090", kNv), AaWord::Dlss, 1, "Godlike", true, true));
    CHECK(Is(Recommend("NVIDIA GeForce RTX 5090", kNv), AaWord::Dlss, 1, "Godlike", true, true));
    CHECK(Is(Recommend("AMD Radeon RX 9070 XT", kAmd), AaWord::Fsr4, 1, "Ultra", false, true));
    CHECK(Is(Recommend("AMD Radeon RX 9060 XT", kAmd), AaWord::Fsr4, 1, "Ultra", false, true));
    CHECK(Is(Recommend("AMD Radeon RX 7900 XTX", kAmd), AaWord::Fsr3, 1, "High", false, true));
    CHECK(Is(Recommend("AMD Radeon RX 6700 XT", kAmd), AaWord::Fsr3, 2, "High", false, true));
    CHECK(Is(Recommend("NVIDIA GeForce GTX 1080 Ti", kNv), AaWord::Fsr3, 2, "High", false, true));
    CHECK(Is(Recommend("NVIDIA GeForce RTX 2080 Ti", kNv), AaWord::Dlss, 1, "Medium / High", false, true));
}
static void nearest_rows()
{
    CHECK(Is(Recommend("NVIDIA GeForce RTX 2060", kNv), AaWord::Dlss, 1, "Medium / High", false, true));
    CHECK(Is(Recommend("NVIDIA GeForce RTX 4060 Ti", kNv), AaWord::Dlss, 1, "Medium / High", false, true));
    CHECK(Is(Recommend("NVIDIA GeForce RTX 3090", kNv), AaWord::Dlss, 1, "High", false, true));
    CHECK(Is(Recommend("NVIDIA GeForce RTX 4070 SUPER", kNv), AaWord::Dlss, 1, "Ultra", false, true));
    CHECK(Is(Recommend("NVIDIA GeForce RTX 5080", kNv), AaWord::Dlss, 1, "Ultra", false, true));
    CHECK(Is(Recommend("AMD Radeon RX 9070", kAmd), AaWord::Fsr4, 1, "Ultra", false, true));
    CHECK(Is(Recommend("AMD Radeon RX 6800 XT", kAmd), AaWord::Fsr3, 1, "High", false, true));
    CHECK(Is(Recommend("AMD Radeon RX 7900 GRE", kAmd), AaWord::Fsr3, 1, "High", false, true));
    CHECK(Is(Recommend("AMD Radeon RX 7600", kAmd), AaWord::Fsr3, 2, "High", false, true));
    CHECK(Is(Recommend("AMD Radeon RX 5700 XT", kAmd), AaWord::Fsr3, 2, "High", false, true));
}
static void gpu_laptop_maps_to_desktop()
{
    CHECK(Is(Recommend("NVIDIA GeForce RTX 4070 Laptop GPU", kNv), AaWord::Dlss, 1, "Ultra", false, true));
    CHECK(Is(Recommend("NVIDIA GeForce RTX 3080 Ti Laptop GPU", kNv), AaWord::Dlss, 1, "High", false, true));
}
static void gpu_ti_not_base()
{
    CHECK(Is(Recommend("NVIDIA GeForce RTX 3070 Ti", kNv), AaWord::Dlss, 1, "High", false, true));
    CHECK(Is(Recommend("NVIDIA GeForce RTX 3070", kNv), AaWord::Dlss, 1, "Medium / High", false, true));
    CHECK(Is(Recommend("nvidia geforce rtx 3070 ti", kNv), AaWord::Dlss, 1, "High", false, true));
}
static void gpu_unknown_falls_back_flagged()
{
    CHECK(Is(Recommend("Intel(R) Arc(TM) A770 Graphics", kIntel), AaWord::Fsr3, 2, "High", false, true));
    CHECK(Is(Recommend("NVIDIA GeForce RTX 6080", kNv), AaWord::Fsr3, 2, "High", false, false));
    CHECK(Is(Recommend("Some Unknown Adapter", 0x1234), AaWord::Fsr3, 2, "High", false, false));
    CHECK(Is(Recommend("", kAmd), AaWord::Fsr3, 2, "High", false, false));
}
// Spec 5.3 "The user can change the recommendation": any NVIDIA RTX - in the table or not - is offered DLSS (review
// finding 2). The recommended entry is the default.
static bool HasOpt(const std::vector<AaOption>& o, AaWord m, int q)
{
    for (const auto& x : o) {
        if (x.method == m && x.quality == q) { return true; }
    }
    return false;
}
static void options_unknown_rtx_offers_dlss()
{
    const auto r = Recommend("NVIDIA GeForce RTX 4050 Laptop GPU", kNv);
    CHECK(!r.recognised && r.method == AaWord::Fsr3);
    int dflt = -1;
    const auto o = OptionsFor(r, "NVIDIA GeForce RTX 4050 Laptop GPU", kNv, &dflt);
    CHECK(HasOpt(o, AaWord::Dlss, 1) && HasOpt(o, AaWord::Dlss, 0));
    CHECK(dflt >= 0 && dflt < static_cast<int>(o.size()) && o[dflt].method == AaWord::Fsr3 && o[dflt].quality == 2);
}
static void options_known_cards()
{
    int dflt = -1;
    auto o = OptionsFor(Recommend("NVIDIA GeForce RTX 3080", kNv), "NVIDIA GeForce RTX 3080", kNv, &dflt);
    CHECK(o[dflt].method == AaWord::Dlss && o[dflt].quality == 1 && HasOpt(o, AaWord::Fsr3, 1));
    o = OptionsFor(Recommend("AMD Radeon RX 9070 XT", kAmd), "AMD Radeon RX 9070 XT", kAmd, &dflt);
    CHECK(o[dflt].method == AaWord::Fsr4 && !HasOpt(o, AaWord::Dlss, 1) && HasOpt(o, AaWord::Fsr3, 1));
    o = OptionsFor(Recommend("NVIDIA GeForce GTX 1080 Ti", kNv), "NVIDIA GeForce GTX 1080 Ti", kNv, &dflt);
    CHECK(o[dflt].method == AaWord::Fsr3 && o[dflt].quality == 2 && !HasOpt(o, AaWord::Dlss, 1));
}
// Review finding 4: an on-disk choice the list does not offer (FSR 3.1 Performance on an RTX card) is ADDED and selected,
// so an untouched dropdown keeps it (spec 8.5) instead of falling back to the recommendation and overwriting it.
static void current_value_added_when_missing()
{
    int dflt = -1;
    auto o = OptionsFor(Recommend("NVIDIA GeForce RTX 3080", kNv), "NVIDIA GeForce RTX 3080", kNv, &dflt);
    const size_t n = o.size();
    int idx = SelectCurrent(&o, true, {AaWord::Fsr3, 3}, dflt);
    CHECK(o.size() == n + 1 && idx == static_cast<int>(n) && o[static_cast<size_t>(idx)].method == AaWord::Fsr3 &&
          o[static_cast<size_t>(idx)].quality == 3);
    // already offered: selected, nothing added
    auto p = OptionsFor(Recommend("NVIDIA GeForce RTX 3080", kNv), "NVIDIA GeForce RTX 3080", kNv, &dflt);
    idx = SelectCurrent(&p, true, {AaWord::Dlss, 0}, dflt);
    CHECK(p.size() == n && p[static_cast<size_t>(idx)].method == AaWord::Dlss && p[static_cast<size_t>(idx)].quality == 0);
    // nothing on disk: the recommendation
    idx = SelectCurrent(&p, false, {AaWord::Fsr3, 3}, dflt);
    CHECK(p.size() == n && idx == dflt);
}
static void aa_word_text()
{
    CHECK(std::strcmp(AaWordText(AaWord::Dlss), "dlss") == 0);
    CHECK(std::strcmp(AaWordText(AaWord::Fsr3), "fsr3") == 0);
    CHECK(std::strcmp(AaWordText(AaWord::Fsr4), "fsr4") == 0);
}

int main()
{
    spec_rows();
    nearest_rows();
    gpu_laptop_maps_to_desktop();
    gpu_ti_not_base();
    gpu_unknown_falls_back_flagged();
    aa_word_text();
    options_unknown_rtx_offers_dlss();
    options_known_cards();
    current_value_added_when_missing();
    std::printf("setup_gpu_test: %d failure(s)\n", g_fail);
    return g_fail;
}
