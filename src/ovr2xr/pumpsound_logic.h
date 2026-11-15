// pumpsound_logic.h - the pure skip rule of the pump shotgun's rack-sound detour (pumpsound.cpp). Header-only, no Windows,
// unit-tested by src/src/tests/pumpsound_logic_test.cpp.
//
// While a manual pump is the held item the layer plays the rack itself (spec 2026-11-04-manual-reload-pump-shotgun-design), so the
// game's own per-shot rack sound ("ShotgunB_reload", played by the animscript on every shot) is dropped. Only that one family is
// dropped: the load, shot and every other gun's sounds are untouched. The raw text of m_SoundName (case, ".wav", "_XX") is
// [UNKNOWN] (ledger ruling R5), so the compare is case-insensitive on the PREFIX alone.
#pragma once

namespace dlvr {
namespace pumpsound {

constexpr const char kSkipPrefix[] = "shotgunb_reload";   // lower case; compared case-insensitively

inline char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; }

// True = do not play this sound. COST: one bool test when not silenced; else at most 15 char compares.
inline bool PumpSoundSkip(const char* name, bool silenced)
{
    if (!silenced || !name) { return false; }
    for (int i = 0; kSkipPrefix[i]; ++i) {
        if (LowerAscii(name[i]) != kSkipPrefix[i]) { return false; }   // a NUL in name stops here (it never equals a letter)
    }
    return true;
}

}  // namespace pumpsound
}  // namespace dlvr
