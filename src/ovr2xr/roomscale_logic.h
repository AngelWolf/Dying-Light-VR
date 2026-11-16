// Pure logic for roomscale and the roomscale-off drift bleed - docs/superpowers/specs/2026-10-26-roomscale-physical-crouch-design.md
// §2.2 (plan docs/superpowers/plans/2026-10-27-roomscale-physical-crouch.md Task 5). No OpenXR, no Windows, no logging, so
// src/src/tests/roomscale_logic_test.cpp compiles it on its own. All lengths in metres, all vectors in the APP frame, x/z only.
// ovr2xr.cpp owns the shift (ShiftAppSpaceTo); roomscale.cpp (Task 7) owns the walk body.
//
// THE IDEA: `o` = the head's horizontal offset from the appSpace origin (what the engine adds to the body, INV-306). Roomscale asks
// the body to walk by that offset (cmd) and shifts appSpace by what the body ACTUALLY moved, so the view stays over the body. A body
// stopped by a wall leaves a remainder: the view may lean up to leanMaxM past the body, and the origin absorbs the rest, so the head
// never passes through geometry (Jay: "That's correct"). Roomscale OFF: BleedStep moves the offset back toward the body only while
// the stick moves the character (Jay: "That's correct").
#pragma once

#include <cmath>

namespace dlvr {
namespace roomscale {

const float kStickDead   = 0.2f;     // |left stick| above this = the character is moving with the stick
const float kMinCommandM = 0.005f;   // a remainder shorter than this asks the body for nothing

inline bool StickMoving(float sx, float sy) { return std::sqrt(sx * sx + sy * sy) > kStickDead; }

// Roomscale OFF: the shift that moves the origin toward the head by min(|o|, rate * dt) along o (never past it). 0 when rate <= 0,
// dt <= 0 or the head is at the origin.
inline void BleedStep(float ox, float oz, float rateMps, float dtS, float& shiftX, float& shiftZ)
{
    shiftX = 0.0f;
    shiftZ = 0.0f;
    const float len = std::sqrt(ox * ox + oz * oz);
    if (rateMps <= 0.0f || dtS <= 0.0f || len <= 0.0f) { return; }
    const float step = rateMps * dtS;
    const float k = step >= len ? 1.0f : step / len;
    shiftX = ox * k;
    shiftZ = oz * k;
}

struct In {
    float ox, oz;           // the head's offset from the appSpace origin
    float movedX, movedZ;   // what the body actually moved since the last command, already in the app frame
    bool  standDown;        // a state where the body must not be moved (S5): no command, the offset is a clamped lean
    float leanMaxM;         // roomscale_lean_max_m
};

struct Out {
    float shiftX, shiftZ;   // move the appSpace origin by this (the body's move + the absorbed part)
    float cmdX, cmdZ;       // ask the body to walk by this (0 = nothing)
    float leanM;            // how far the view leans past the body after this update (<= leanMaxM)
    float absorbedM;        // how much of the remainder the origin took this update
};

// shift = moved; r = o - moved; no command while standing down, else cmd = r when |r| >= kMinCommandM; the part of r past
// leanMaxM is added to the shift (absorbed) and leanM = what is left of |r|.
inline Out Update(const In& in)
{
    Out out{};
    out.shiftX = in.movedX;
    out.shiftZ = in.movedZ;
    const float rx = in.ox - in.movedX;
    const float rz = in.oz - in.movedZ;
    const float r = std::sqrt(rx * rx + rz * rz);
    if (!in.standDown && r >= kMinCommandM) {
        out.cmdX = rx;
        out.cmdZ = rz;
    }
    const float lean = in.leanMaxM > 0.0f ? in.leanMaxM : 0.0f;
    if (r > lean) {
        const float k = (r - lean) / r;
        out.shiftX += rx * k;
        out.shiftZ += rz * k;
        out.absorbedM = r - lean;
        out.leanM = lean;
    } else {
        out.leanM = r;
    }
    return out;
}

// INV-740: the walk body is asked to close only the part of the remainder past this. A head turn moves the full-arc neck point by the
// difference between the wearer's own neck -> eye offset and the shipped one (head_neck_pivot_*_mm, one user's head): 15-25 mm off it
// left 0.13-1.14 m of walking per 10 s of turns in the harness (scratchpad rsproof); 0.05 m left 0.000 m for every head tested while a
// real 0.30 m step still walked 0.25 m. Fixed, not a setting.
const float kCmdDeadZoneM = 0.05f;

// cmd shortened by deadM along itself; 0 when |cmd| <= deadM or deadM < 0 is passed as 0.
inline void DeadZoneCommand(float& cmdX, float& cmdZ, float deadM)
{
    const float dz = deadM > 0.0f ? deadM : 0.0f;
    const float len = std::sqrt(cmdX * cmdX + cmdZ * cmdZ);
    if (len <= dz || len <= 0.0f) {
        cmdX = 0.0f;
        cmdZ = 0.0f;
        return;
    }
    const float k = (len - dz) / len;
    cmdX *= k;
    cmdZ *= k;
}

}  // namespace roomscale
}  // namespace dlvr
