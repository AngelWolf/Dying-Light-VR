// The whole-file md5 of the Data3.pak this build ships, and the verdict the bootstrap exports (DlvrData3State).
// Jay 2026-10-18: "Md5 + refuse AFW if wrong" - a stock Data3 (a tester's, docs/BETA-FINDINGS-2026-10-18.md section 2) has no keep-RIFT,
// so AFW's held eye is cleared to black (INV-375). pakgen's "FIXED v1" check covers only three entries; this covers the file.
// HAND-MAINTAINED: equal to tools/make_setup_pkg.py's GAME_FILES pin for DW\Data3.pak. make_setup_pkg.py REFUSES to package
// when this, its pin and the staged pak disagree, so a rebuilt Data3 cannot ship with a stale value here.
// No Windows, no logging, so src/src/tests/data3_md5_test.cpp compiles it on its own.
#pragma once

static const char* const kFixedData3Md5 = "935318518b7a3d0624ca4ffb84df18be";   // 2026-11-24 (208) + per-eye reflection history (render_loop.ppfx; trace 20261124_2050); was 60556476 = 2026-11-22 (74) + data/panel/boxed_light_15.fm (U+E075 = the grip glyph; trace 20261122_1740); the GG1 test remap b4c70b1b dropped; was 394248e7 (2026-10-31 (6) Melee_VRShove)

// DlvrData3State() values. 0 = not checked yet.
static const int kData3Ours       = 1;
static const int kData3NotOurs    = -1;
static const int kData3Unreadable = -2;

// got = the pak's md5 as hex (either case); readable = the file was read whole.
inline int Data3Verdict(bool readable, const char* got)
{
    if (!readable || !got) { return kData3Unreadable; }
    const char* want = kFixedData3Md5;
    for (; *want; ++want, ++got) {
        char c = *got;
        if (c >= 'A' && c <= 'F') { c = static_cast<char>(c - 'A' + 'a'); }
        if (c != *want) { return kData3NotOurs; }
    }
    return *got == '\0' ? kData3Ours : kData3NotOurs;
}
