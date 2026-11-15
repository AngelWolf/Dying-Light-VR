// handcarry_logic.h - the pure half of the hand-carried model (spec docs/superpowers/specs/2026-10-22-hand-carried-model-design.md,
// plan docs/superpowers/plans/2026-10-22-hand-carried-model.md Task 2). No engine calls: the offset maths and the per-update
// decision. handcarry.cpp performs what Decide() returns. Unit test: src/src/tests/handcarry_logic_test.cpp.
#pragma once

#include <cmath>

namespace dlvr {
namespace handcarry {

// Jay 2026-10-22 ("Tune in the headset"): the carried arrow's relation in r_hand, adjusted live - tilted about the hand's Y axis and
// turned about its Z axis, both about the NOCK (rel's translation stays put), then slid HORIZONTALLY: along Y x shaft, perpendicular
// to both the knuckle line (the hand's Y - index +0.029, middle +0.004, ring -0.020, pinky -0.040 at f132, INV-613) and the turned
// shaft. Run 2 (Jay: "That was vertical, not horizontal" -> "1"): a slide along Y reads VERTICAL in the hold. R' = Rz(turn).Ry(tilt).R.
inline void AdjustAboutNock(const float rel[12], float tiltDeg, float turnDeg, float sideM, float out[12])
{
    for (int k = 0; k < 12; ++k) { out[k] = rel[k]; }
    if (tiltDeg == 0.0f && turnDeg == 0.0f && sideM == 0.0f) { return; }
    const float kD = 0.01745329252f;
    const float ct = std::cos(tiltDeg * kD), st = std::sin(tiltDeg * kD);
    const float cz = std::cos(turnDeg * kD), sz = std::sin(turnDeg * kD);
    const float Ry[9] = { ct, 0, st, 0, 1, 0, -st, 0, ct };
    const float Rz[9] = { cz, -sz, 0, sz, cz, 0, 0, 0, 1 };
    float M[9];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) { M[r * 3 + c] = Rz[r * 3] * Ry[c] + Rz[r * 3 + 1] * Ry[3 + c] + Rz[r * 3 + 2] * Ry[6 + c]; }
    }
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out[r * 4 + c] = M[r * 3] * rel[c] + M[r * 3 + 1] * rel[4 + c] + M[r * 3 + 2] * rel[8 + c];
        }
    }
    // Y x shaft (shaft = out's column 2) = (fz, 0, -fx); a shaft along the knuckle line has no horizontal - no slide.
    const float hx = out[10], hz = -out[2];
    const float hl = std::sqrt(hx * hx + hz * hz);
    if (hl > 1.0e-6f) {
        out[3] += sideM * hx / hl;
        out[11] += sideM * hz / hl;
    }
}

// 3x4 row-major, basis in the COLUMNS (the project's convention). out = base with its translation moved by -d along base's
// column 2 (+Z) - for the bow arrow, +Z is the arrow's forward (INV-495), so a positive d slides it back toward the hand (INV-613 A).
inline void SlideBack(const float base[12], float d, float out[12])
{
    for (int k = 0; k < 12; ++k) { out[k] = base[k]; }
    out[3] = base[3] - d * base[2];
    out[7] = base[7] - d * base[6];
    out[11] = base[11] - d * base[10];
}

// out = a . b (3x4 compose, translation a.R . b.t + a.t). The carried object's local on the hand = the slid relation . the object's
// OWN local L as the game left it on arrow_pose (drawn on the bow at arrow_pose . L, INV-613 F6) - final review 2026-10-22 (4).
inline void ComposeLocal(const float a[12], const float b[12], float out[12])
{
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            float s = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c];
            if (c == 3) { s += a[r * 4 + 3]; }
            out[r * 4 + c] = s;
        }
    }
}

// out = m^-1 for a 3x4 affine (row-major, basis in the columns): a GENERAL 3x3 inverse (adjugate / determinant), never the rigid
// transpose - the crossbow's bolt_pose world carries a uniform scale of 3.22526 (INV-676), so an object's own local L on it is not
// guaranteed rigid. Translation: -R^-1 . t. False (out untouched) when |det| < 1e-12 (a collapsed / zero matrix). Crossbow plan Task 3.
inline bool InvertAffine(const float m[12], float out[12])
{
    const double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const double det = a * A + b * B + c * C;
    if (!(std::fabs(det) >= 1.0e-12)) { return false; }   // also refuses a NaN determinant
    const double k = 1.0 / det;
    double r[9];
    r[0] = A * k;              r[1] = -(b * i - c * h) * k;  r[2] = (b * f - c * e) * k;
    r[3] = B * k;              r[4] = (a * i - c * g) * k;   r[5] = -(a * f - c * d) * k;
    r[6] = C * k;              r[7] = -(a * h - b * g) * k;  r[8] = (a * e - b * d) * k;
    const double tx = m[3], ty = m[7], tz = m[11];
    for (int row = 0; row < 3; ++row) {
        out[row * 4] = static_cast<float>(r[row * 3]);
        out[row * 4 + 1] = static_cast<float>(r[row * 3 + 1]);
        out[row * 4 + 2] = static_cast<float>(r[row * 3 + 2]);
        out[row * 4 + 3] = static_cast<float>(-(r[row * 3] * tx + r[row * 3 + 1] * ty + r[row * 3 + 2] * tz));
    }
    return true;
}

// HandCarryTakeAs's relation: the object must be DRAWN at onElem x drawn, and the local set on the hand is rel x L (ComposeLocal),
// so rel = drawn x L^-1. False (rel untouched) when L is singular (InvertAffine refused).
inline bool RelForDrawn(const float drawn[12], const float L[12], float rel[12])
{
    float inv[12];
    if (!InvertAffine(L, inv)) { return false; }
    ComposeLocal(drawn, inv, rel);
    return true;
}

// The same carry: the same owner (bow item) AND the same object - or a ZERO object read with the same owner, which holds the carry
// rather than forgetting an object that is still on the hand (final review 2026-10-22 (5)).
inline bool SameObject(const void* item, const void* obj, const void* homeModel, const void* heldObj)
{
    return item == homeModel && (obj == heldObj || obj == nullptr);
}

// A carry is held but the owner's tick did not run since the last watch (the bow draw was switched off, the string probe on, the
// controller unreadable): the watch must give it back (final review 2026-10-22 (1)).
inline bool TickMissed(bool active, long tickSeq, long seenSeq) { return active && tickSeq == seenSeq; }

enum class Act { None, Take, Retake, SetLocal, GiveBack, Forget };

struct In {
    bool wantCarry;        // the owner wants the object in the hand this update (the nock machine is in Carry)
    bool active;           // a carry is held (HandCarry::active)
    bool sameObject;       // SameObject(...) this update
    bool stillOn;          // the carried object's parent element == the hand element (read back)
    bool bodyChanged;      // the hand's model (the player body) is not the one the carry is on (a respawn) - give back first
    bool relChanged;       // the relation now differs from the one the current local was built with (a slider moved)
};

// In this order: a changed object is never touched again (the bow destroys its arrow, INV-613 F7); a new body -> home first (never
// a call on the old body); not wanted -> home; wanted and not held -> take; held but moved off the hand by the game (an arrow-type
// change, INV-613 F5) -> take again; the slide-back changed -> re-set the local only.
inline Act Decide(const In& in)
{
    if (in.active && !in.sameObject) { return Act::Forget; }
    if (in.active && in.bodyChanged) { return Act::GiveBack; }
    if (!in.wantCarry) { return in.active ? Act::GiveBack : Act::None; }
    if (!in.active) { return Act::Take; }
    if (!in.stillOn) { return Act::Retake; }
    if (in.relChanged) { return Act::SetLocal; }
    return Act::None;
}

}  // namespace handcarry
}  // namespace dlvr
