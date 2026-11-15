// bodytrack.h - THE BODY-TRACKED ELBOW's OpenXR side (rig_elbow_body_tracking, 2026-11-08). XR_FB_body_tracking, located once per
// predicted display time in the same app space as the controllers; the rig (boneblend.cpp SolveArmIk) reads the published arms.
// The maths is bodytrack_logic.h.
#pragma once

#include <openxr/openxr.h>

#include <cstdint>

namespace dlvr {

extern bool RigElbowBodyTracking;   // rig_elbow_body_tracking - the elbows follow the runtime's body-tracked elbows (live)

// BEFORE xrCreateInstance: appends XR_FB_body_tracking to exts[*count] when the runtime lists it (and there is room). Says which, by
// name, once. Returns true when it was appended.
bool BodyTrackAddExtension(const char** exts, uint32_t* count, uint32_t cap);

// After the session exists (beside ControllersInit): reads the system's body-tracking support and resolves the entry points. The
// tracker itself is created on the first locate with the setting on. Logs the outcome, NAMED, unconditionally.
void BodyTrackInit(XrInstance instance, XrSystemId systemId, XrSession session);

// Beside ControllersLocate (same time, same space). Locates the body once per predicted display time; any thread.
// COST: setting off - one bool test; on - one xrLocateBodyJointsFB per new display time (70 joints) + ~60 flops, under a try-lock.
void BodyTrackLocate(XrTime predictedDisplayTime, XrSpace appSpace);

// One arm's latest tracked sample, app space. arm: 0 = RIGHT, 1 = LEFT (boneblend's aidx).
struct BodyArm {
    bool     usable;      // active, confident, the three joints located, not a glitch, offset computed
    float    dirApp[3];   // the tracked elbow's direction off the shoulder -> wrist line (unit, app space)
    float    perpM;       // its distance off that line (m)
    uint64_t seq;         // the locate this came from (rises once per new display time)
};
// False = no sample yet (the setting is off, unsupported, or nothing located). `out` is filled either way when it returns true.
bool BodyTrackGetArm(int arm, BodyArm& out);

void BodyTrackShutdown();

}  // namespace dlvr
