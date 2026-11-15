// DLSS FLAT PROTOTYPE - MILESTONE B: ACQUIRE THE GAME'S D3D11 DEVICE, IN
// FLATSCREEN, AND LATCH THE THREE DLSS INPUTS. READ-ONLY.
//
// WHY THIS LIVES IN THE BOOTSTRAP AND NOT IN THE LAYER
//
// `LibOVRRT64_1.dll` is NOT LOADED IN FLATSCREEN. The whole ovr2xr layer only
// exists on the Oculus path, so `dlssprobe` - which found the GBuffer in VR -
// cannot run in flat at all. `xinput1_3.dll` is the one binary that is always
// mapped (engine_x64_rwdi.dll statically imports it by ordinal), so a flat
// prototype has to live here.
//
// WHAT IS ALREADY ESTABLISHED, so this does not re-derive it
//
//   Step 0 (RenderDoc, flat)  MV space is UV [0,1]; direction is
//                             `previous = current - MV`; camera AND object
//                             motion both present.
//   Step 1 (dlssprobe, VR)    per-eye render 2688x2880; MV is GBuffer RT3,
//                             R16G16_FLOAT; depth R32G8X24_TYPELESS; the eye
//                             swapchain is 5376x2880 DOUBLE-WIDE.
//   Milestone A (ngx_smoke)   NGX initialises and evaluates on this machine for
//                             D3D11, at 1920x1080 and at 2688x2880, including
//                             the subrect path. No NVIDIA app id needed; we must
//                             ship nvngx_dlss.dll and pass a path list.
//
// So the only open question this milestone answers is: CAN WE GET THE GAME'S
// OWN DEVICE, AND SEE ITS GBUFFER, FROM THE BOOTSTRAP, IN FLATSCREEN?
//
// HOW
//
// `rd3d11_x64_rwdi.dll` imports `D3D11CreateDevice` from `d3d11.dll` by name
// (verified in its import table), so the existing `PatchIat` reaches it. The
// existing rd3d11 watchdog already polls at 1 ms and catches the module the
// moment it is mapped - well before the renderer creates anything - and that is
// where this installs from.
//
// The IAT hook calls the real function FIRST, then keeps the returned device.
// It changes no creation flag and no feature level: a device created
// differently is a different device, and this must observe the real one.
//
// It then vtable-hooks OMSetRenderTargets on the immediate context - the same
// technique, slot and sanity check `dlssprobe` uses in VR - and latches the
// first bind that looks like the GBuffer.
//
// PASSING VALUES, STATED IN ADVANCE:
//
//   [DLSSFLAT] IAT hook installed ...        the import was found and patched
//   [DLSSFLAT] DEVICE ACQUIRED ... NVIDIA    the real device, on the real adapter
//   [DLSSFLAT] GBUFFER BIND SEEN             with an R16G16_FLOAT target, a DSV,
//                                            and both at the flat render size
//
// All three -> milestone B is done and milestone C (a real evaluation in flat)
// has everything it needs. Any one missing is a specific, named blocker.
//
// SILENCE IS AMBIGUOUS, so every stage logs unconditionally, including its
// refusals: "import not found", "slot does not point at executable code", and
// "installed, waiting" are three different findings and the log separates them.
//
// ENABLED BY A MARKER FILE, `dlvr\dlss_flat.txt`, matching this binary's
// existing convention - the bootstrap runs before the engine's entry point and
// has no access to ovr2xr.txt.
#pragma once

#include <windows.h>

namespace dlvr {
namespace dlssflat {

// True when dlvr\dlss_flat.txt exists. Read once, at DllMain.
extern bool Enabled;

// Checks for the marker file and logs the decision. Call from DllMain.
void ReadMarker();

// Patches rd3d11's D3D11CreateDevice import. Call from the rd3d11 watchdog the
// moment the module appears, BEFORE the signature gate patch is logged, so the
// ordering is visible in the log. No-ops entirely while Enabled is false.
void InstallFromWatchdog(HMODULE rd3d11);

}  // namespace dlssflat
}  // namespace dlvr
