// WRIST HUD STAGE 1 - the HUD's drawable leaves, handed from the layer (LibOVRRT64_1, which reads the engine's UI tree inside
// the game's own UI calls - wristprobe.cpp) to the bootstrap (xinput1_3, which sees each HUD draw - dlssflat.cpp wristroute::).
// Spec docs/superpowers/specs/2026-11-12-wrist-hud-design.md §4 / §10 stage 1; the matching is Mamfro's (dying-light-games-vr,
// dl1/src/hud/ui.rs + panels.rs - used with the author's permission). Plain data, no engine types: both DLLs include it.
//
// Coordinates: the engine's UI-screen pixels (GetGlobalTransform translation / GetSize - INV-725: the 1280 x 720 layout x 4 +
// (128, 0) inside a 5376 x 2880 UI screen), y DOWN.
#pragma once

namespace dlvr {
namespace wristleaf {

constexpr int kPieces    = 5;      // 0 HudRadar, 1 health_wrap, 2 StackObjectives, 3 HudPrimaryWeaponIndicator, 4 HudSecondaryWeaponIndicator
constexpr int kMaxLeaves = 2048;
constexpr int kMinimap   = 0;

struct Leaf {
    float at[2];          // the element's global translation = an image quad's corner (local 0, 0)
    float rect[4];        // left, top, right, bottom
    signed char piece;    // 0..4, -1 = not one of the five
    unsigned char text;   // 1 = a text element (RTTI class name holds "Text")
    unsigned char last;   // 1 = the last leaf of its widget in tree order (the next widget's draws start after it)
    unsigned char pad;
};

struct Header {
    float root[4];                        // HUD_DI's own box
    float pieceBox[kPieces][4];           // each piece's box (the union of its leaves; the minimap = its largest leaf)
    unsigned long long takenMs;           // GetTickCount64 of the snapshot (0 = never)
    long gen;                             // +1 per publish
    int count;                            // leaves
    int shownMask;                        // bit p = piece p had a visible leaf
    int standDown;                        // 1 = an in-game menu or a selection wheel is open (set by dlvr_WristLeaves; 0 in every publish)
};

// Exported by LibOVRRT64_1 as dlvr_WristLeaves: copies the latest snapshot. Returns the leaf count (<= max), or -1 when the
// layer has none (stage 1 off, or no HUD_DI snapshot yet).
using LeavesFn = int (*)(Header* hdr, Leaf* out, int max);

}  // namespace wristleaf
}  // namespace dlvr
