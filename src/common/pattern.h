// Resolving code sites by signature instead of by fixed offset.
//
// Every address we patch was originally written down as an RVA taken from one
// binary - the Epic build of engine_x64_rwdi.dll. That works exactly as long as
// everyone runs that binary. The Steam build is a different compile of the same
// version: rd3d11 is byte-identical and patched fine, while both engine gates
// found unrelated bytes and refused, so the engine never ran its Oculus init and
// VR never started.
//
// A byte pattern around the instruction survives a recompile in a way an offset
// cannot. The RVA is kept as a hint and checked first, so the build we developed
// against costs nothing and cannot regress; the scan is the fallback.
//
// A pattern that matches more than once is treated as no match at all. Patching
// the wrong one of several candidates is far worse than not patching: it writes
// a jump into the middle of unrelated code and the failure surfaces later, as a
// crash, somewhere else entirely.
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace dlvr {

// One signature. `delta` is added to the match address to reach the instruction
// we actually want, which is usually a few bytes into the pattern - patterns
// need leading context to be unique.
struct SitePattern {
    const char* bytes;   // "48 8B D8 48 83 F8 FF 74 ?? 48 8D 4C 24 ?? FF 15"
    int delta;
};

namespace pattern_detail {

// "4A" -> 0x4A, "??" -> wildcard. Returns the token count, 0 on a malformed
// pattern.
inline int Parse(const char* pattern, uint8_t* bytes, bool* wild, int max)
{
    int n = 0;
    const char* p = pattern;
    while (*p && n < max) {
        while (*p == ' ') {
            ++p;
        }
        if (!*p) {
            break;
        }
        if (p[0] == '?') {
            wild[n] = true;
            bytes[n] = 0;
            ++n;
            p += (p[1] == '?') ? 2 : 1;
            continue;
        }
        int hi = -1, lo = -1;
        for (int k = 0; k < 2; ++k) {
            const char c = p[k];
            const int v = (c >= '0' && c <= '9')   ? c - '0'
                          : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                          : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                                                   : -1;
            if (v < 0) {
                return 0;
            }
            (k == 0 ? hi : lo) = v;
        }
        wild[n] = false;
        bytes[n] = static_cast<uint8_t>((hi << 4) | lo);
        ++n;
        p += 2;
    }
    return n;
}

}  // namespace pattern_detail

// Scans the executable sections of `mod` for `pattern`. Returns the RVA of the
// single match, or 0 when there is none or more than one. `count` receives how
// many were found, counted up to 2 - past that the answer is already "ambiguous"
// and the exact number tells us nothing.
inline uintptr_t FindPatternRva(HMODULE mod, const char* pattern, int* count = nullptr)
{
    if (count) {
        *count = 0;
    }
    if (!mod || !pattern) {
        return 0;
    }

    constexpr int kMaxTokens = 128;
    uint8_t pat[kMaxTokens];
    bool wild[kMaxTokens];
    const int len = pattern_detail::Parse(pattern, pat, wild, kMaxTokens);
    if (len <= 0) {
        return 0;
    }

    const auto base = reinterpret_cast<uintptr_t>(mod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }
    const auto* sec = IMAGE_FIRST_SECTION(nt);

    uintptr_t found = 0;
    int hits = 0;
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections && hits < 2; ++s) {
        if (!(sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) {
            continue;
        }
        DWORD size = sec[s].SizeOfRawData;
        if (sec[s].Misc.VirtualSize && sec[s].Misc.VirtualSize < size) {
            size = sec[s].Misc.VirtualSize;
        }
        if (size < static_cast<DWORD>(len)) {
            continue;
        }
        const auto* p = reinterpret_cast<const uint8_t*>(base + sec[s].VirtualAddress);
        const size_t last = size - len;
        for (size_t i = 0; i <= last; ++i) {
            int k = 0;
            for (; k < len; ++k) {
                if (!wild[k] && p[i + k] != pat[k]) {
                    break;
                }
            }
            if (k == len) {
                if (hits == 0) {
                    found = sec[s].VirtualAddress + i;
                }
                if (++hits >= 2) {
                    break;
                }
            }
        }
    }

    if (count) {
        *count = hits;
    }
    return hits == 1 ? found : 0;
}

// Resolves one patch site. `expect` is a pattern - not plain bytes - that must
// be present at the answer, the same check the caller would do anyway, applied
// to the hint and to every candidate. A pattern that matches somewhere plausible
// but wrong still cannot produce an address we would write to.
//
// It has to allow wildcards because what we verify is usually a jump, and a jump
// carries a displacement that changes with any code motion. Requiring the exact
// `74 1E` we saw once would reject the correct instruction in a build where the
// branch target moved by a byte, which is the failure this whole file exists to
// avoid.
//
// `how` receives a short human-readable account of which route succeeded, for
// the log. It is written on failure too, and is the whole diagnostic when a
// future build breaks this again.
inline uintptr_t ResolveSite(HMODULE mod, uintptr_t hintRva,
                             const char* expect,
                             const SitePattern* pats, int npats,
                             char* how, size_t howLen)
{
    if (how && howLen) {
        how[0] = '\0';
    }
    if (!mod) {
        if (how) {
            strncpy_s(how, howLen, "module not loaded", _TRUNCATE);
        }
        return 0;
    }
    const auto base = reinterpret_cast<uintptr_t>(mod);

    constexpr int kMaxExpect = 32;
    uint8_t ebytes[kMaxExpect];
    bool ewild[kMaxExpect];
    const int elen = pattern_detail::Parse(expect, ebytes, ewild, kMaxExpect);
    if (elen <= 0) {
        if (how) {
            strncpy_s(how, howLen, "malformed expect pattern", _TRUNCATE);
        }
        return 0;
    }

    auto matches = [&](uintptr_t rva) {
        auto* at = reinterpret_cast<const uint8_t*>(base + rva);
        if (IsBadReadPtr(at, elen)) {
            return false;
        }
        for (int k = 0; k < elen; ++k) {
            if (!ewild[k] && at[k] != ebytes[k]) {
                return false;
            }
        }
        return true;
    };

    if (hintRva && matches(hintRva)) {
        if (how) {
            _snprintf_s(how, howLen, _TRUNCATE, "known offset +%#zx", hintRva);
        }
        return hintRva;
    }

    for (int i = 0; i < npats; ++i) {
        int n = 0;
        const uintptr_t hit = FindPatternRva(mod, pats[i].bytes, &n);
        if (!hit) {
            if (how && n > 1) {
                _snprintf_s(how, howLen, _TRUNCATE,
                            "pattern %d matched in more than one place", i + 1);
            }
            continue;
        }
        const uintptr_t rva = hit + pats[i].delta;
        if (!matches(rva)) {
            if (how) {
                _snprintf_s(how, howLen, _TRUNCATE,
                            "pattern %d hit +%#zx but the bytes there are wrong",
                            i + 1, rva);
            }
            continue;
        }
        if (how) {
            _snprintf_s(how, howLen, _TRUNCATE,
                        "pattern %d -> +%#zx (this build is not the one we "
                        "developed against)", i + 1, rva);
        }
        return rva;
    }

    if (how && how[0] == '\0') {
        strncpy_s(how, howLen, "no pattern matched", _TRUNCATE);
    }
    return 0;
}

}  // namespace dlvr
