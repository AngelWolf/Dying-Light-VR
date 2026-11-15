// THE ANTI-ALIASING METHOD - dlvr\aa_method.txt, line 1 = none | dlss | fsr3 | fsr4 | taa.
//
// Jay, 2026-09-26: the Anti-Aliasing tab's dropdown (NO AA / DLSS / FSR 3.1 /
// FSR 4) is the ONE switch; "NO AA WILL BE DEFAULT". So an absent file is NO AA,
// and every older marker (dlss_vr.txt, dlss_upscale.txt, jitter_halton.txt, ...)
// is a sub-setting that only means anything under the method that uses it.
// docs/PROPOSAL-FSR-INTEROP-2026-09-26.md §8.
//
// Shared by PAKGEN (which pak to write) and dlssflat (what runs), because the two
// read at DLL attach and must never disagree about which method this launch is.
// Header-only: both translation units parse the file the same way.
#pragma once

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace dlvr {

// kAaTaa (2026-11-12, Jay: "We need a 'stock standard' anti aliasing solution, no upscale ... for those who don't want to or
// can't use FSR/DLSS without the performance hit" -> "Yes, go with TAA"): our own TAA (dlssflat.cpp RunTaa, D-128's code) at
// native resolution, WITH the engine's jitter (pakgen opens the jitter gate for it as for DLSS / FSR). Word "taa". BETA only.
// The hand marker aa_taa.txt still selects the same TAA.
enum AaMethod { kAaNone = 0, kAaDlss = 1, kAaFsr3 = 2, kAaFsr4 = 3, kAaTaa = 4 };

inline const char* AaMethodName(int m)
{
    switch (m) {
    case kAaDlss: return "DLSS";
    case kAaFsr3: return "FSR 3.1";
    case kAaFsr4: return "FSR 4";
    case kAaTaa:  return "TAA";
    default:      return "NO AA";
    }
}

// Reads `path`. *present says whether the file exists; `raw` receives line 1
// (trimmed, at most rawCap-1 chars) so the caller can print exactly what it
// read. An absent file, an unreadable one and an unrecognised word ALL mean NO
// AA - the documented default - and the caller logs which of the three it was.
inline int ReadAaMethod(const char* path, bool* present, char* raw, size_t rawCap)
{
    *present = false;
    if (rawCap) { raw[0] = 0; }
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) { return kAaNone; }
    *present = true;
    char buf[64] = {};
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    size_t n = 0;
    while (buf[n] && buf[n] != '\r' && buf[n] != '\n' && buf[n] != ' ' && buf[n] != '\t') { ++n; }
    buf[n] = 0;
    if (rawCap) { strncpy_s(raw, rawCap, buf, _TRUNCATE); }
    if (_stricmp(buf, "dlss") == 0) { return kAaDlss; }
    if (_stricmp(buf, "fsr3") == 0) { return kAaFsr3; }
    if (_stricmp(buf, "fsr4") == 0) { return kAaFsr4; }
    if (_stricmp(buf, "taa") == 0)  { return kAaTaa; }
    return kAaNone;   // "none", empty, or a word we do not know
}

}  // namespace dlvr
