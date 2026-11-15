// Dumps a run of submitted frames into one image, so the glitch can be looked
// at instead of inferred.
//
// Every number we have says the pipeline is consistent: the pose the game draws
// with is the pose we declare, one frame old and reported as such. Yet the
// picture jumps when frames are missed. At that point another counter is worth
// less than one look at the actual pixels, side by side, in order.
//
// What lands on disk is a grid of the last N submitted eye textures - both eyes
// per cell, because they come from one double-wide swapchain and the whole point
// is to see whether they agree - plus a text file giving each cell's frame
// number, timings and the two poses. Read them together: the grid says what
// happened, the text says when and from where.
#pragma once

#include <d3d11.h>

#include <cstdint>

namespace dlvr {

// What we knew about a frame at the moment it was submitted.
struct CaptureMeta {
    unsigned frame;
    double   gameMs;        // engine work for this frame
    double   endMs;         // previous xrEndFrame
    double   waitMs;        // previous xrWaitFrame
    float    declared[4];   // the orientation we handed the compositor
    float    ours[4];       // the orientation we last located
    float    stepDeg;       // how far the declared pose moved since the last one
};

// Ask for the next `frames` submissions to be captured. Safe to call while a
// capture is already running - it is ignored rather than restarting, so holding
// the key does not produce a file per frame.
void CaptureArm(int frames);
bool CaptureActive();

// Call once per submit, with the texture that was actually handed to the
// compositor. Does nothing unless armed.
void CaptureFrame(ID3D11Device* device, ID3D11Texture2D* eyes, const CaptureMeta& meta);

// Polls the capture hotkey. Cheap; call once per frame.
void CaptureTick();

// One line for the overlay: idle, capturing, or where the last file went.
const char* CaptureStatus();

// Virtual-key code that arms a capture. Default F10.
extern int CaptureKey;
// How many frames one capture collects.
extern int CaptureFrames;
// Which mip of the eye texture is stored. 0 is full size and enormous; 3 turns
// a 6144-wide pair into 768, which is enough to see a jump and small enough to
// write twenty of.
extern int CaptureMip;

}  // namespace dlvr
