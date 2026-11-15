// Finds CCamera instances by their type, not by what their bytes look like.
//
// The Windows build ships no symbols but it does keep RTTI, and RTTI names
// classes outright. Walking it gives CCamera's vtable address, and then every
// object in the process whose first qword is that vtable IS a CCamera. Not
// "looks like a rotation", not "moved when we moved" - is one.
//
// That replaces the whole statistical search. The float scan had to guess at
// layout, rank thousands of candidates, and could still be looking at a bone
// matrix; this returns the objects themselves, with the layout already known
// from the macOS symbols:
//
//     +0x10  mtx34  view matrix        (world -> camera)
//     +0x40  mtx34  camera matrix      (camera -> world, position in column 3)
//     +0x70  mtx44  projection
//     +0xB0  mtx44  combined
//
// The vtable RVA is deliberately NOT hard-coded. It is walked at runtime from
// the type name, so a game patch that moves the vtable costs nothing.
#pragma once

#include "xrloader.h"

#include <cstdint>

namespace dlvr {

struct RttiCamera {
    uintptr_t object;      // the CCamera itself
    uintptr_t camMatrix;   // object + the detected matrix offset
    float px, py, pz;      // world position, live
    float yawDeg, pitchDeg, rollDeg;
    float moveDeg;         // degrees turned in the last second - the live one
    bool moving;
    const char* className;
};

// The matrix offset inside CCamera, measured rather than assumed.
//
// +0x40 came from the macOS build's IBaseCamera accessors, and the Windows
// build is a different compiler on a different ABI - same source, but nothing
// guarantees the same padding or member order. Reading a hard-coded offset that
// happens to be wrong gives exactly what was observed: rows of numbers that
// never change, because they are not the camera's matrix at all.
//
// So the offset is found from the objects themselves: a CCamera holds a
// rotation and its inverse a fixed distance apart, and that pattern is looked
// for inside the object rather than assumed to be where it was on the Mac.
int RttiMatrixOffset();
int RttiMatrixSpacing();

// Which class to look for.
//
// CRendererCamera is the one the picture comes from. CCamera::operator= - the
// function we hook - is called by CRendererCamera::CreateFrom(CBaseCamera*),
// so the renderer builds its own camera from the game's every frame. Editing
// the game's camera afterwards moves the frustum, which is why geometry got
// culled, and never reaches the image, which is why nothing turned.
//
// And because CRendererCamera derives from CCamera it carries its OWN vtable,
// so a scan for CCamera's never saw a single one of them. Same offsets though -
// the base subobject sits at zero.
// "CRendererCamera", "CCamera", "CBaseCamera", or "both" for the first two at
// once.
//
// Both is what the culling needs. The rendered view comes from the renderer
// camera and the cull frustum does not: levelling only the renderer camera
// leaves the frustum still carrying the game's pitch, so aiming up with the
// stick keeps a level picture while quietly clipping everything below. The two
// have to be levelled together or they disagree about where the camera points.
void RttiSetClass(const char* name);

// Walks RTTI in the engine module for the current class's vtable. Cached after the first
// call. Returns 0 if the class or its locator cannot be found.
uintptr_t RttiFindCCameraVtable();
uintptr_t RttiVtableOf(const char* cls);

// The reverse direction: given a live object address found some other way
// (a caught register value, a pointer read out of another object), reads its
// class name back out via RTTI. False if the object has no vtable pointer at
// all, or the vtable has no RTTI (not every class does).
bool RttiClassNameOf(uintptr_t objectAddr, char* out, size_t outLen);

// Same as RttiClassNameOf, but ALSO returns the Complete Object Locator's
// "offset" field - the distance from the pointer you hold to the start of the
// COMPLETE object. Needed whenever you want to read a field at an offset taken
// from a class's RTTI field registrar: those offsets are relative to the
// complete object, and a pointer to a base subobject is NOT that address.
//
// HumanAI has TEN vtables in our gamedll and NOT ONE has offset 0
// (0xf58, 0xf60, 0xf68, 0xf50, 0xf70, 0x8f0, 0x8f8, 0xf20, 0x340, 0x348 -
// re_analysis/humanai_vtable.txt), so for that class the adjustment is never
// optional. Reading m_HitCount at entity+0x680 without it produced 17
// confident false positives on 2026-09-15.
bool RttiClassNameAndOffsetOf(uintptr_t objectAddr, char* out, size_t outLen,
                              uint32_t& offsetInClass);

// Scans committed memory for objects with that vtable. Returns how many were
// found; call RttiGetCameras to read them.
int RttiScanCameras();

// Keep only objects that actually carry a rotation and its inverse at the
// measured offsets.
//
// The scan finds every object with CCamera's vtable, and the log says only 8 of
// 77 hold the matrix pair. The rest are constructed but never set up - which is
// why most rows jitter with meaningless numbers and only one is steady. Hiding
// them turns a list nobody can read into a handful worth trying.
extern bool RttiOnlyRealCameras;

// Re-sort the list by how much each camera is turning.
//
// Sorting by activity puts the live camera on top, which is how you find it -
// and then makes the rows swap places under the cursor while you are trying to
// click one, which is how you fail to lock it. Was off by default (DEFAULT ON since 2026-10-01): address order is
// arbitrary but it holds still.
extern bool RttiSortByActivity;

int RttiCameraCount();
int RttiGetCameras(RttiCamera* out, int max);

// Refreshes the live values and the moving flags. Call once a frame.
void RttiUpdateCameras();

}  // namespace dlvr
