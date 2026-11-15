// calibassets.h - the calibration screen's build D assets (docs/superpowers/plans/2026-10-14-calibration-screen-build-d.md
// task 4): Jay's 14 PNGs from <game>\dlvr\calib\ and the game's own Boxed Semibold 30 font, read from the player's own
// install (Data0.pak's .fm + menu_common_PC.rpack's BC3 atlas) - in memory only, nothing of the game's is written to disk.
//
// THREADING: CalibArtStart starts ONE loader thread, once per launch (any thread may call it; it is idempotent). The loader
// does every file read / decode / inflate and writes only its own buffers, then raises one interlocked ready flag.
// CalibArtPump and every accessor below are RENDER THREAD ONLY: Pump creates the textures once, after the flag.
// COST: while the screen is closed nothing here runs. Open: CalibArtPump = one bool test (+ one interlocked read until the
// textures exist, then never again); the accessors are array reads.
#pragma once

#include <d3d11.h>

#include "calibassets_logic.h"

namespace dlvr {

// The first screen open: starts the loader thread once (idempotent, any thread).
void CalibArtStart();
// Render thread, every calibration frame: when the loader is done, create the textures once.
void CalibArtPump();
// The image's SRV (as an ImTextureID) + its pixel size, or 0 = missing / refused / not ready -> the draw falls back.
// `w` / `h` may be null.
unsigned long long CalibArtTex(calibart::Img i, int* w, int* h);
// The floor's texture for CopyResource (build D task 5), or nullptr.
ID3D11Texture2D* CalibArtFloorTexture();
// The void's walls (false) or ceiling (true) video texture for CopyResource (Jay 2026-10-16), or nullptr = no frame yet /
// missing / refused -> nothing is drawn there. Calling it stamps "being drawn": the decoder runs only while the calls come.
ID3D11Texture2D* CalibArtVoidTexture(bool ceiling);

struct CalibFont {
    unsigned long long tex;          // SRV as an ImTextureID: R8G8B8A8_UNORM, RGB 255, A = the glyph coverage, 4 mips
    float fontHeight, texW, texH;    // the .fm's FontHeight (px); the packed atlas size (px)
    const calibart::FontMetrics* m;  // the parsed .fm
    const calibart::PackedGlyph* g;  // [128], indexed by character code; the INNER rect in normalized packed coordinates
    ID3D11SamplerState* sampler;     // MIN_MAG_MIP_LINEAR, CLAMP, MaxLOD 3
};
// nullptr = not loaded -> ImGui's default font (D2).
const CalibFont* CalibArtFont();

}  // namespace dlvr
