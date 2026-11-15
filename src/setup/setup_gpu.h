// The setup program's GPU recommendation (docs/superpowers/specs/2026-10-17-installer-design.md 5.3).
// Pure and header-only - tested by src/src/tests/setup_gpu_test.cpp.
//
// Jay 2026-10-17: 3060 Ti "VD Medium/High & DLSS Quality", 3080 "High & DLSS Quality", 4080 "Ultra & DLSS Quality",
// 4090/5090 "Godlike & DLSS Quality / DLAA" (-> "Leave it to user preference": askDlaa), "4070 Should handle Ultra",
// 9070 XT "equal to 4080 & its FSR counterpart", "FSR 4 quality for 9070XT/9060XT", "Anything that doesn't support
// FSR4 or DLSS can use FSR3.1", Quality / Balanced split at 2080 Ti level ("Your guess sounds right"). The VD preset is
// ADVICE ONLY. Cards between his rows go to the nearest row (the spec's builder list). No card here was measured.
#pragma once

#include <cctype>
#include <string>
#include <vector>

namespace dlvr::setup {

enum class AaWord { Dlss, Fsr3, Fsr4 };

inline const char* AaWordText(AaWord w)
{
    switch (w) {
    case AaWord::Dlss: return "dlss";
    case AaWord::Fsr4: return "fsr4";
    default: return "fsr3";
    }
}

struct Recommendation {
    AaWord method;
    int quality;            // dlss_quality.txt: 0 DLAA / native, 1 Quality, 2 Balanced, 3 Performance
    const char* vdAdvice;   // Virtual Desktop graphics quality - shown, never set
    bool askDlaa;           // 4090 / 5090: the user picks DLSS Quality or DLAA
    bool recognised;        // false = not in the table; the window says so
};

namespace detail {

// Lower-case alphanumeric words of the adapter name ("RTX 4070 Ti Laptop GPU" -> rtx 4070 ti laptop gpu).
inline std::vector<std::string> Words(const std::string& name)
{
    std::vector<std::string> out;
    std::string cur;
    for (char ch : name) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c)) {
            cur += static_cast<char>(std::tolower(c));
        } else if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) {
        out.push_back(cur);
    }
    return out;
}

inline int Number(const std::string& w)
{
    if (w.empty() || w.size() > 6) {
        return -1;
    }
    for (char c : w) {
        if (c < '0' || c > '9') {
            return -1;
        }
    }
    return std::stoi(w);
}

// The model number after `family` ("rtx", "gtx", "rx") and whether the next word is `ti`; -1 if absent.
inline int ModelAfter(const std::vector<std::string>& w, const char* family, bool* ti)
{
    for (size_t i = 0; i + 1 < w.size(); ++i) {
        if (w[i] == family) {
            const int n = Number(w[i + 1]);
            if (n >= 0) {
                *ti = (i + 2 < w.size() && w[i + 2] == "ti");
                return n;
            }
        }
    }
    return -1;
}

inline bool AnyWord(const std::vector<std::string>& w, const char* a)
{
    for (const auto& s : w) {
        if (s == a) {
            return true;
        }
    }
    return false;
}

inline bool OneOf(int n, std::initializer_list<int> list)
{
    for (int x : list) {
        if (x == n) {
            return true;
        }
    }
    return false;
}

}  // namespace detail

inline Recommendation Recommend(const std::string& adapterName, unsigned vendorId)
{
    const Recommendation fallback{AaWord::Fsr3, 2, "High", false, false};
    const std::vector<std::string> w = detail::Words(adapterName);
    bool ti = false;

    const int rtx = detail::ModelAfter(w, "rtx", &ti);
    if (rtx >= 0) {
        if (rtx == 4090 || rtx == 5090) {
            return {AaWord::Dlss, 1, "Godlike", true, true};
        }
        if (detail::OneOf(rtx, {4070, 4080, 5070, 5080})) {
            return {AaWord::Dlss, 1, "Ultra", false, true};
        }
        if (detail::OneOf(rtx, {3080, 3090}) || (rtx == 3070 && ti)) {
            return {AaWord::Dlss, 1, "High", false, true};
        }
        if (detail::OneOf(rtx, {2060, 2070, 2080, 3050, 3060, 3070, 4060, 5060})) {
            return {AaWord::Dlss, 1, "Medium / High", false, true};
        }
        return fallback;   // an RTX the table does not know (a future card)
    }
    if (detail::ModelAfter(w, "gtx", &ti) >= 0) {
        return {AaWord::Fsr3, 2, "High", false, true};   // no DLSS, below 2080 Ti level
    }
    const int rx = detail::ModelAfter(w, "rx", &ti);
    if (rx >= 0) {
        if (rx == 9070 || rx == 9060) {
            return {AaWord::Fsr4, 1, "Ultra", false, true};
        }
        if (detail::OneOf(rx, {6800, 6900, 6950, 7800, 7900})) {
            return {AaWord::Fsr3, 1, "High", false, true};
        }
        if ((rx >= 400 && rx <= 590) || (rx >= 5000 && rx <= 7999)) {
            return {AaWord::Fsr3, 2, "High", false, true};
        }
        return fallback;
    }
    if (vendorId == 0x8086 || detail::AnyWord(w, "arc")) {
        return {AaWord::Fsr3, 2, "High", false, true};   // Intel: no DLSS / FSR 4
    }
    return fallback;
}

struct AaOption {
    AaWord method;
    int quality;
};

// The upscaler choices offered for this card, the recommendation's entry as *dflt. Spec 5.3: "The user can change the
// recommendation" - so ANY NVIDIA RTX is offered DLSS, including one the table does not know (its default stays the
// flagged FSR 3.1 Balanced); an FSR 4 card is offered FSR 4 and FSR 3.1; everything else FSR 3.1.
inline std::vector<AaOption> OptionsFor(const Recommendation& r, const std::string& adapterName, unsigned vendorId,
                                        int* dflt)
{
    std::vector<AaOption> o;
    const bool nvidiaRtx = (vendorId == 0x10DE) && detail::AnyWord(detail::Words(adapterName), "rtx");
    if (nvidiaRtx || r.method == AaWord::Dlss) {
        o = {{AaWord::Dlss, 1}, {AaWord::Dlss, 0}, {AaWord::Dlss, 2}, {AaWord::Dlss, 3}, {AaWord::Fsr3, 1}, {AaWord::Fsr3, 2}};
    } else if (r.method == AaWord::Fsr4) {
        o = {{AaWord::Fsr4, 1}, {AaWord::Fsr4, 2}, {AaWord::Fsr3, 1}, {AaWord::Fsr3, 2}};
    } else {
        o = {{AaWord::Fsr3, 1}, {AaWord::Fsr3, 2}, {AaWord::Fsr3, 3}, {AaWord::Fsr3, 0}};
    }
    *dflt = 0;
    for (size_t i = 0; i < o.size(); ++i) {
        if (o[i].method == r.method && o[i].quality == r.quality) {
            *dflt = static_cast<int>(i);
        }
    }
    return o;
}

// The dropdown's selection for an existing install (spec 8.5: an untouched dropdown keeps what is on disk): the on-disk
// choice if the list offers it; if the list does not (FSR 3.1 Performance on an RTX card), it is APPENDED and selected,
// so leaving it alone never overwrites it with the recommendation. Nothing on disk = the recommendation (dflt).
inline int SelectCurrent(std::vector<AaOption>* options, bool haveOnDisk, AaOption onDisk, int dflt)
{
    if (!haveOnDisk) { return dflt; }
    for (size_t i = 0; i < options->size(); ++i) {
        if ((*options)[i].method == onDisk.method && (*options)[i].quality == onDisk.quality) { return static_cast<int>(i); }
    }
    options->push_back(onDisk);
    return static_cast<int>(options->size() - 1);
}

}  // namespace dlvr::setup
