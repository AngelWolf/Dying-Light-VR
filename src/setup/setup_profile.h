// The setup program's controller-layout check (docs/superpowers/specs/2026-10-17-installer-design.md 5.6).
// Pure and header-only (needs third_party/puff/puff.c linked) - tested by src/src/tests/setup_profile_test.cpp.
//
// Jay 2026-10-17: "Read & warn" - the profile is Steam-cloud-synced, so it is READ, never written. Layout read
// 2026-10-17 from Jay's Steam\userdata\<id>\239140\remote\out\profiles\default.pro: gzip -> a list of entries
// [len byte][name][NUL][int32 LE]. The pad layout keys are I_LayoutPreset<x>Ctr1 / Ctr2; Jay's are all 0 and he is on
// Default / Default (D-158). NOT established: that 0 = Default for every key, which Ctr is the pad. So the check fails
// toward saying so: anything it cannot decode, or a profile with no layout keys, is Unreadable - never a false all-clear.
#pragma once

#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "puff.h"
}

namespace dlvr::setup {

enum class LayoutVerdict { AllDefault, NonDefault, Unreadable };

struct LayoutResult {
    LayoutVerdict verdict;
    std::vector<std::string> nonDefaultKeys;
    int layoutKeysFound;
};

// gzip (RFC 1952) -> raw bytes. False on a bad header, a failed inflate or a size that disagrees with ISIZE.
inline bool GunzipBytes(const std::vector<unsigned char>& gz, std::vector<unsigned char>* out)
{
    out->clear();
    if (gz.size() < 18 || gz[0] != 0x1F || gz[1] != 0x8B || gz[2] != 8) {
        return false;
    }
    const unsigned char flg = gz[3];
    size_t p = 10;
    if (flg & 4) {   // FEXTRA
        if (p + 2 > gz.size()) { return false; }
        p += 2 + (gz[p] | (gz[p + 1] << 8));
    }
    if (flg & 8) {   // FNAME
        while (p < gz.size() && gz[p] != 0) { ++p; }
        ++p;
    }
    if (flg & 16) {   // FCOMMENT
        while (p < gz.size() && gz[p] != 0) { ++p; }
        ++p;
    }
    if (flg & 2) {   // FHCRC
        p += 2;
    }
    if (p + 8 > gz.size()) {
        return false;
    }
    const size_t tail = gz.size() - 4;
    const unsigned long isize = static_cast<unsigned long>(gz[tail]) | (static_cast<unsigned long>(gz[tail + 1]) << 8) |
                                (static_cast<unsigned long>(gz[tail + 2]) << 16) |
                                (static_cast<unsigned long>(gz[tail + 3]) << 24);
    if (isize > (64ul << 20)) {
        return false;
    }
    out->resize(isize == 0 ? 1 : isize);
    unsigned long destLen = isize;
    unsigned long srcLen = static_cast<unsigned long>(gz.size() - 8 - p);
    const int rc = puff(out->data(), &destLen, gz.data() + p, &srcLen);
    if (rc != 0 || destLen != isize) {
        out->clear();
        return false;
    }
    out->resize(isize);
    return true;
}

namespace detail {

// A pad-layout key: I_LayoutPreset<middle>Ctr1|Ctr2 whose middle is not a keyboard binding or an invert flag.
inline bool IsLayoutKey(const std::string& k)
{
    const std::string pre = "I_LayoutPreset";
    if (k.size() < pre.size() + 4 || k.compare(0, pre.size(), pre) != 0) {
        return false;
    }
    const std::string tail = k.substr(k.size() - 4);
    if (tail != "Ctr1" && tail != "Ctr2") {
        return false;
    }
    const std::string mid = k.substr(pre.size(), k.size() - pre.size() - 4);
    return !mid.empty() && mid.compare(0, 11, "Keybinding_") != 0 && mid != "invert_y" && mid != "invert_y_mouse";
}

}  // namespace detail

inline LayoutResult ReadLayout(const std::vector<unsigned char>& gzFile)
{
    LayoutResult r{LayoutVerdict::Unreadable, {}, 0};
    std::vector<unsigned char> raw;
    if (!GunzipBytes(gzFile, &raw)) {
        return r;
    }
    const char* pre = "I_LayoutPreset";
    const size_t preLen = std::strlen(pre);
    for (size_t i = 0; i + preLen < raw.size(); ++i) {
        if (std::memcmp(&raw[i], pre, preLen) != 0) {
            continue;
        }
        size_t e = i;
        while (e < raw.size() && raw[e] != 0) { ++e; }
        if (e + 4 >= raw.size()) {
            break;
        }
        const std::string key(reinterpret_cast<const char*>(&raw[i]), e - i);
        const int v = static_cast<int>(static_cast<unsigned>(raw[e + 1]) | (static_cast<unsigned>(raw[e + 2]) << 8) |
                                       (static_cast<unsigned>(raw[e + 3]) << 16) |
                                       (static_cast<unsigned>(raw[e + 4]) << 24));
        if (detail::IsLayoutKey(key)) {
            ++r.layoutKeysFound;
            if (v != 0) {
                r.nonDefaultKeys.push_back(key);
            }
        }
        i = e;
    }
    if (r.layoutKeysFound == 0) {
        r.verdict = LayoutVerdict::Unreadable;
    } else {
        r.verdict = r.nonDefaultKeys.empty() ? LayoutVerdict::AllDefault : LayoutVerdict::NonDefault;
    }
    return r;
}

}  // namespace dlvr::setup
