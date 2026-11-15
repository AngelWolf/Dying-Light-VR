// Every upscaler preset the card supports, for the launcher's Config tab (Jay 2026-11-15: "Make sure that users are able to pick
// ANY of the upscaling presets that their card supports"). Matches the in-game Anti-Aliasing tab (overlay.cpp DrawDlssTab):
// DLSS 0..4 (DLAA, Quality, Balanced, Performance, Ultra Performance) on NVIDIA RTX; FSR 3.1 0..3 (Native AA .. Performance) on any
// GPU; FSR 4 0..3 on the cards setup_gpu.h's Recommend gives FSR 4 (RX 9070 / 9060). No Ultra Performance for FSR (PAKGEN refuses
// it). The setup's shortlist (setup_gpu.h OptionsFor) is left as it is. Pure and header-only - tested by
// src_beta/src/tests/launcher_gpu_test.cpp.
#pragma once

#include <string>
#include <vector>

#include "setup_gpu.h"

namespace dlvr::launcher {

// The order shown: the card's own upscaler first (DLSS, or FSR 4), then FSR 3.1. *dflt = the recommendation's row.
inline std::vector<setup::AaOption> AllUpscalerOptions(const setup::Recommendation& r, const std::string& adapterName, unsigned vendorId,
                                                       int* dflt)
{
    using setup::AaWord;
    std::vector<setup::AaOption> o;
    const bool nvidiaRtx = (vendorId == 0x10DE) && setup::detail::AnyWord(setup::detail::Words(adapterName), "rtx");
    if (nvidiaRtx || r.method == AaWord::Dlss) {
        for (int q = 0; q <= 4; ++q) { o.push_back({AaWord::Dlss, q}); }
    }
    if (r.method == AaWord::Fsr4) {
        for (int q = 0; q <= 3; ++q) { o.push_back({AaWord::Fsr4, q}); }
    }
    for (int q = 0; q <= 3; ++q) { o.push_back({AaWord::Fsr3, q}); }
    *dflt = 0;
    for (size_t i = 0; i < o.size(); ++i) {
        if (o[i].method == r.method && o[i].quality == r.quality) { *dflt = static_cast<int>(i); }
    }
    return o;
}

namespace detail {

inline std::string FirstToken(std::string t)
{
    if (t.compare(0, 3, "\xEF\xBB\xBF") == 0) { t.erase(0, 3); }
    std::string w;
    for (char c : t) {
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            if (!w.empty()) { break; }
            continue;
        }
        w += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return w;
}

}  // namespace detail

// dlvr\aa_method.txt + dlvr\dlss_quality.txt (first lines) -> the upscaler choice on disk. dlss 0..4; fsr3 / fsr4 0..3, and a 4
// there reads as 3 (PAKGEN runs Performance for it). Anything else (none / taa / a bad number / absent) = false.
inline bool ReadAaOnDisk(const std::string& aaMethodText, const std::string& qualityText, setup::AaOption* out)
{
    using setup::AaWord;
    const std::string m = detail::FirstToken(aaMethodText);
    const std::string q = detail::FirstToken(qualityText);
    if (q.size() != 1 || q[0] < '0' || q[0] > '4') { return false; }
    int n = q[0] - '0';
    if (m == "dlss") { *out = {AaWord::Dlss, n}; return true; }
    if (m == "fsr3" || m == "fsr4") {
        if (n == 4) { n = 3; }
        *out = {m == "fsr4" ? AaWord::Fsr4 : AaWord::Fsr3, n};
        return true;
    }
    return false;
}

}  // namespace dlvr::launcher
