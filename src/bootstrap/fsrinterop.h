// FSR 3.1 / FSR 4 THROUGH A D3D11 <-> D3D12 INTEROP.
//
// The design and every [UNKNOWN] it carries: docs/PROPOSAL-FSR-INTEROP-2026-09-26.md
// (approved by Jay 2026-09-26). The SDK facts it rests on:
// docs/REF-FSR-SDK-2026-09-26.md. FSR ships DX12-only (no DX11 backend exists in
// SDK 2.3), so the engine's D3D11 frame is handed to a D3D12 device on the SAME
// adapter through shared textures and one shared fence.
//
// STEP 2a (measured 2026-09-26, INV-253/254): setup - device on the right
// adapter, fence round trip, shared textures (D3D11-created, opened in D3D12),
// FFX loader, version list, one context per eye.
// STEP 2b/2c (passed flat + VR, Jay): the per-frame NativeAA resolve at the
// tonemap draw - copies in, cross-API sync, the FFX dispatch, substitution.
// STEP 3 (this build, PROPOSAL §8): the QUALITY / BALANCED / PERFORMANCE modes,
// at the engine's upscale copy on post-tonemap colour, like DLSS upscaling.
//
// Nothing here is linked statically: d3d12.dll and the FFX loader are both
// reached through LoadLibrary + GetProcAddress, so the mod still loads on a
// machine that has neither.
#pragma once

#include <windows.h>

#include "../common/lenscrop_logic.h"   // the lens crop's Rect (Dispatch's inRc / outRc)

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;

namespace dlvr {
namespace fsr {

// Runs the size-independent setup once (device, fence, loader, version choice)
// and logs every step under [FSR], each refusal named. With `nativeContexts`
// it also builds the NATIVE (render size == output size) textures and contexts
// at w x h - the tonemap-point path. Without it (the upscale modes) nothing is
// sized until EnsureUpscale. Idempotent: a later NATIVE call at a different
// size refuses (FSR stops for the launch). Render thread only - it signals a
// fence on `ctx` and flushes it.
//
// `colour`/`mv`/`depth` are the engine's own inputs, read only for their descs.
// `eyes` is 1 in flat, 2 in VR. `wantMajor` is 3 or 4 (dlvr\aa_method.txt).
bool Setup(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* colour,
           ID3D11Texture2D* mv, ID3D11Texture2D* depth, UINT w, UINT h, int eyes,
           int wantMajor, bool nativeContexts);

// THE UPSCALE MODES: sizes the shared textures and contexts for a render
// (scene) size and an output size - lazily, from the first matched upscale
// draw - and re-sizes them if either changes. The input colour is the engine's
// post-tonemap 8-bit image, so the contexts are NON-LINEAR, not HDR. Setup must
// have run (core only). Render thread only.
bool EnsureUpscale(UINT inW, UINT inH, UINT outW, UINT outH, int eyes);

// The D3D11 side of eye `eye`'s shared output. NATIVE: what the tonemap draw
// samples. Upscale: what Dispatch copies into the caller's sRGB-viewable twin.
// Null until sized. Not AddRef'd.
ID3D11Texture2D*          OutputTex(int eye);
ID3D11ShaderResourceView* OutputSrv(int eye);

// One FSR resolve for one eye (PROPOSAL §3). Call inside dlssflat's StateBackup
// window on the immediate context. `colour`/`mv`/`depth` are this frame's
// engine inputs (mv = the camera-fixed buffer when there is one); `jitterX/Y`
// are in render PIXELS in FSR's convention (dlssflat converts); `viewProj` is
// the eye's row-major view-projection or null (near/FOV fall back, and say so).
// `copyOutTo`: null for NATIVE; for the upscale modes the caller's TYPELESS
// output texture, which receives the result by CopyResource after the sync.
// Returns false on any failure, counted and throttled-logged here.
// LENS CROP (2026-11-11, docs/superpowers/plans/2026-11-11-dlss-lens-crop.md Task 5): `inRc` / `outRc` non-null = reconstruct only
// those rows (render / output size, full width). FSR reads and writes from the origin, so the inputs are copied in from the crop's
// offset and the result is placed at outRc's offset: upscale by the copy-out, NATIVE through a D3D12 scratch texture. The rows
// outside outRc are NOT written here - the caller fills them. Both null = the whole image, exactly as before.
bool Dispatch(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* colour, ID3D11Texture2D* mv,
              ID3D11Texture2D* depth, float jitterX, float jitterY, const float* viewProj,
              bool reset, ID3D11Texture2D* copyOutTo, const ::dlvr::lenscrop::Rect* inRc,
              const ::dlvr::lenscrop::Rect* outRc);

// For the overlay's "running now" line: the provider FFX actually created
// ("3.1.5"), or "" when no context exists yet; *fsr4Fallback = 1 when FSR 4 was
// asked for and 3.x is running instead.
const char* ProviderName(int* fsr4Fallback);

// FIXED FOVEATED FSR (plan docs/superpowers/plans/2026-11-11-foveated-dlss.md Task 5). Both write a PRIVATE texture of ours at
// (0, 0) (the composite in dlssflat.cpp places and blends them); neither touches the stock path's state beyond the shared inputs.
// CENTRE: this eye's existing context, render sub-size inRc.w x inRc.h (colour / vectors / depth copied from inRc's corner to the
// shared inputs' origin), upscale sub-size outW x outH (<= the context's max), result copied out of the shared output into dst.
bool DispatchCentre(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* colour, ID3D11Texture2D* mv, ID3D11Texture2D* depth,
                    float jitterX, float jitterY, const float* viewProj, bool reset, const ::dlvr::lenscrop::Rect& inRc,
                    UINT outW, UINT outH, ID3D11Texture2D* dst);
// PERIPHERY: a second context per eye at w x h -> w x h (NativeAA, the mode's flags), its own shared set; inputs are the
// downsampled colour (8-bit or R16G16B16A16_FLOAT), depth (R32_FLOAT) and vectors (R16G16_FLOAT) at w x h; result copied into dst.
bool DispatchPeriphery(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* colour, ID3D11Texture2D* mv, ID3D11Texture2D* depth,
                       UINT w, UINT h, float jitterX, float jitterY, const float* viewProj, bool reset, ID3D11Texture2D* dst);

}  // namespace fsr
}  // namespace dlvr
