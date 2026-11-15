// reload_hands.h - the manual reload's LEFT hand rules (pure, unit-tested: src/src/tests/reload_hands_test.cpp).
// Plan docs/superpowers/plans/2026-11-01-reload-hand-poses.md Task 2; spec docs/superpowers/specs/2026-11-01-reload-hand-poses-design.md.
// Which pose is active for a reload step, which generated row (reload_hand_poses.inc) serves a gun, the hand LOCKED to the part
// (rack: bone_chamber, mag hold: bone_magazine), and the one 100 ms blend clock. No game access, no logging, no allocation.
// Mag rows are keyed BY MESH (plan amendment 2026-11-01 (5): Jay's approved holds are per mesh); pistol rack rows by mesh, SMG rack
// rows by anim prefix (from the game's clips); rifle rows (plan 2026-11-03-manual-reload-rifle Task 3) by mesh for both - the mag in
// bone_clip's rest frame, the handle in bone_chamber's, the Commando's right handle keyed "wn_rifle_comando+x".
// Matrices: row-major 3x4, basis = columns, t = [3] [7] [11] (reload.cpp's convention); 3x3 the same without t.
#pragma once

#include <cmath>
#include <cstdio>
#include <cstring>

#include "reload_guns.h"
#include "reload_hand_poses.inc"
#include "reload_logic.h"

namespace dlvr {
namespace reload {

enum class HandPose { None, MagHold, Rack };

constexpr long long kHandBlendMs = 100;   // on and off - fingers, rotation and position share it (the bow's BowDrawFingerT)

// Pulling / MagInHand / FreshInHand hold a mag; Racking holds the slide / bolt; every other step leaves the hand free.
inline HandPose ActiveHandPose(Step s)
{
    switch (s) {
        case Step::Pulling:
        case Step::MagInHand:
        case Step::FreshInHand: return HandPose::MagHold;
        case Step::Racking: return HandPose::Rack;
        default: return HandPose::None;
    }
}

namespace hands_detail {
inline bool StartsNoCase(const char* s, const char* pre)
{
    for (; *pre; ++s, ++pre) {
        char a = *s, b = *pre;
        if (a >= 'A' && a <= 'Z') { a = static_cast<char>(a - 'A' + 'a'); }
        if (b >= 'A' && b <= 'Z') { b = static_cast<char>(b - 'A' + 'a'); }
        if (a != b) { return false; }
    }
    return true;
}
inline const HandRow* Find(const HandRow* rows, int n, const char* key)
{
    if (!rows || !key) { return nullptr; }
    for (int i = 0; i < n; ++i) {
        if (guns_detail::EqNoCase(rows[i].key, key)) { return &rows[i]; }
    }
    return nullptr;
}
}  // namespace hands_detail

// Pistol: by normalised mesh (wn_pistol_a / b / d / scifi); SMG: by anim prefix (submachinegun / submachinegun_alt); Rifle: by
// normalised mesh (wn_rifle_*; rifle plan 2026-11-03 Task 3), and `second` = the Commando's RIGHT handle, the row keyed
// "<mesh>+x" (only wn_rifle_comando has one). `second` is ignored off the rifle; its default keeps every existing caller unchanged.
// A mesh key must belong to the family (wn_pistol_* / wn_rifle_*). Null = no row (today's open hand, no lock) - a mesh nobody
// fitted, the Railgun (no handle), an unknown prefix, Family::None.
// Crossbow: by normalised mesh (wn_crossbow_* / dlc_wn_crossbow_*; crossbow plan Task 2), the hand in bone_stringreload's rest frame.
// Cost: a linear scan of kRackRowsCount (17) rows with a short case-blind compare - called once per gun change, not per frame.
inline const HandRow* RackRowFor(Family f, const char* meshNorm, const char* prefix, bool second = false)
{
    if (f == Family::Pistol) {
        if (!meshNorm || !hands_detail::StartsNoCase(meshNorm, "wn_pistol_")) { return nullptr; }
        return hands_detail::Find(kRackRows, kRackRowsCount, meshNorm);
    }
    if (f == Family::Rifle) {
        if (!meshNorm || !hands_detail::StartsNoCase(meshNorm, "wn_rifle_")) { return nullptr; }
        if (std::strchr(meshNorm, '+')) { return nullptr; }   // the "+x" row is reached only through `second`, never by name
        if (!second) { return hands_detail::Find(kRackRows, kRackRowsCount, meshNorm); }
        char key[96];
        const int n = std::snprintf(key, sizeof(key), "%s+x", meshNorm);
        if (n <= 0 || n >= static_cast<int>(sizeof(key))) { return nullptr; }
        return hands_detail::Find(kRackRows, kRackRowsCount, key);
    }
    if (f == Family::Smg) {
        if (!prefix || !(guns_detail::EqNoCase(prefix, "submachinegun") || guns_detail::EqNoCase(prefix, "submachinegun_alt"))) {
            return nullptr;
        }
        return hands_detail::Find(kRackRows, kRackRowsCount, prefix);
    }
    if (f == Family::Crossbow) {
        // Crossbow plan Task 2: Jay's rack grab, by normalised crossbow mesh (wn_crossbow_* / dlc_wn_crossbow_*), the hand in
        // bone_stringreload's REST LOCAL frame. One handle: `second` is ignored.
        if (!meshNorm || !(hands_detail::StartsNoCase(meshNorm, "wn_crossbow_") || hands_detail::StartsNoCase(meshNorm, "dlc_wn_crossbow_"))) {
            return nullptr;
        }
        return hands_detail::Find(kRackRows, kRackRowsCount, meshNorm);
    }
    return nullptr;
}

// The crossbow's bolt CARRY fist (kCarryRows, key "crossbow_bolt"; Jay's blend/pose_A2_jay.json): FINGERS ONLY - its handR / handT are
// identity / zero and lock nothing (the hand stays the controller's; the bolt rides it, XbowRow::boltCarry). Crossbow only, else null.
// Cost: a scan of kCarryRowsCount (1) row - once per gun change.
inline const HandRow* CarryRowFor(Family f)
{
    if (f != Family::Crossbow) { return nullptr; }
    return hands_detail::Find(kCarryRows, kCarryRowsCount, "crossbow_bolt");
}

// By normalised mesh for every family; the row must belong to the family (a pistol key is wn_pistol_*, a rifle key wn_rifle_*, an SMG
// key neither), else null.
// 2026-11-09 (plan 2026-11-09-manual-reload-remaining-guns Task 1): two keys belong to a family their name does not say - the toy gun
// (wn_shisha_gun, a Beretta-prefix PISTOL) and the grenade launcher (wn_grenadelauncher_a, the RIFLE mag flow). Exact, case-blind.
inline bool KeyIsPistol(const char* k)
{
    return hands_detail::StartsNoCase(k, "wn_pistol_") ||
           (hands_detail::StartsNoCase(k, "wn_shisha_gun") && k[sizeof("wn_shisha_gun") - 1] == 0);
}
inline bool KeyIsRifle(const char* k)
{
    return hands_detail::StartsNoCase(k, "wn_rifle_") ||
           (hands_detail::StartsNoCase(k, "wn_grenadelauncher_a") && k[sizeof("wn_grenadelauncher_a") - 1] == 0);
}

inline const HandRow* MagRowFor(Family f, const char* meshNorm)
{
    if (f == Family::None || !meshNorm) { return nullptr; }
    const bool pistolKey = KeyIsPistol(meshNorm);
    const bool rifleKey = KeyIsRifle(meshNorm);
    if ((f == Family::Pistol) != pistolKey) { return nullptr; }
    if ((f == Family::Rifle) != rifleKey) { return nullptr; }
    return hands_detail::Find(kMagRows, kMagRowsCount, meshNorm);
}

// The PISTOL's pull-out pose (kPullRows, by normalised mesh; Jay 2026-11-01: "the pistol needs a different pose for pulling the mag
// out, putting it in is fine though"). Pistols only - an SMG's mag hold serves its pull too. Null = no row.
inline const HandRow* PullRowFor(Family f, const char* meshNorm)
{
    if (f != Family::Pistol || !meshNorm || !KeyIsPistol(meshNorm)) { return nullptr; }
    return hands_detail::Find(kPullRows, kPullRowsCount, meshNorm);
}

// The row holding the mag on a step: the pulled mag (Pulling / MagInHand) = the pull row when the gun has one, else the hold row;
// the fresh mag (FreshInHand) = the hold row; any other step = none.
inline const HandRow* MagHoldRow(Step s, const HandRow* pull, const HandRow* hold)
{
    if (s == Step::Pulling || s == Step::MagInHand) { return pull ? pull : hold; }
    if (s == Step::FreshInHand) { return hold; }
    return nullptr;
}

// Does the pose blend on / off over kHandBlendMs? The rack does; the mag hold switches at once (Jay after HP2: the mag is in the hand
// from the grab, "no 100 ms ease").
inline bool PoseEases(HandPose p) { return p == HandPose::Rack; }

// The hand locked to the part: pos = part x handT, r9 = Ortho(part) x handR. Ortho = Gram-Schmidt on the columns (x, then y, z = x
// cross y) - the generator built the rows in the SAME orthonormalised frame, and a game world may carry scale. False (outputs
// untouched) on a non-finite input or a degenerate part basis.
inline bool LockedHand(const float partWorld12[12], const HandRow& r, float pos[3], float r9[9])
{
    for (int i = 0; i < 12; ++i) { if (!std::isfinite(partWorld12[i])) { return false; } }
    for (int i = 0; i < 9; ++i) { if (!std::isfinite(r.handR[i])) { return false; } }
    for (int i = 0; i < 3; ++i) { if (!std::isfinite(r.handT[i])) { return false; } }
    float x[3] = { partWorld12[0], partWorld12[4], partWorld12[8] };
    float y[3] = { partWorld12[1], partWorld12[5], partWorld12[9] };
    const float lx = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    if (!(lx > 1e-6f)) { return false; }
    for (float& v : x) { v /= lx; }
    const float d = x[0] * y[0] + x[1] * y[1] + x[2] * y[2];
    for (int i = 0; i < 3; ++i) { y[i] -= d * x[i]; }
    const float ly = std::sqrt(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
    if (!(ly > 1e-6f)) { return false; }
    for (float& v : y) { v /= ly; }
    const float z[3] = { x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0] };
    const float R[9] = { x[0], y[0], z[0], x[1], y[1], z[1], x[2], y[2], z[2] };
    float p[3], o[9];
    for (int rr = 0; rr < 3; ++rr) {
        p[rr] = partWorld12[4 * rr + 3] + R[3 * rr] * r.handT[0] + R[3 * rr + 1] * r.handT[1] + R[3 * rr + 2] * r.handT[2];
        for (int c = 0; c < 3; ++c) {
            o[3 * rr + c] = R[3 * rr] * r.handR[c] + R[3 * rr + 1] * r.handR[3 + c] + R[3 * rr + 2] * r.handR[6 + c];
        }
    }
    for (int i = 0; i < 3; ++i) { pos[i] = p[i]; }
    for (int i = 0; i < 9; ++i) { r9[i] = o[i]; }
    return true;
}

// 0..1 over kHandBlendMs since the pose last switched on (on) or off (!on). A negative age (clock went backwards) clamps.
inline float HandBlendT(bool on, long long msSinceChange)
{
    long long ms = msSinceChange < 0 ? 0 : msSinceChange;
    if (ms > kHandBlendMs) { ms = kHandBlendMs; }
    const float t = static_cast<float>(ms) / static_cast<float>(kHandBlendMs);
    return on ? t : 1.0f - t;
}

// While the rack lock holds the drawn hand on the slide / bolt, the rack pull is read from the COMMANDED (controller) hand - the
// drawn hand would follow the part and measure itself (spec section 3 "Read while planning" (b)).
inline bool UseCommandedHand(HandPose p, float lockT) { return p == HandPose::Rack && lockT > 0.0f; }

// The reload lock and the two-hand pin never both move the left hand: while the reload lock is on at all, the pin is skipped.
inline bool TwoHandPinAllowed(float reloadLockT) { return !(reloadLockT > 0.0f); }

// THE PUMP SHOTGUN's LOADING ANIMATION (plan docs/superpowers/plans/2026-11-04-manual-reload-pump-shotgun.md Task 2; spec §6d): Jay's
// approved C14 (2026-11-08, backups/20261108_shell_gap_close/blend/shell_hold_anim_C14.blend = C13 on his ONE-SHELL hold pose B: the
// game draws one shell, so the middle / ring / little fingers stay closed and the next shell appears under the thumb at cycle frame 29;
// no shell in the hand at cycle frames 24-28; 30 fps, 4 cycles of 36 frames), baked per frame
// by tools/pump_anim_export.py (Blender) into reload_pump_anim.inc - never edit it by hand. Per frame: the 16 left finger pins' LOCAL
// rotations (FingerRow: rest = the player_6_fpp mesh rest, pose = the frame's local; reload_hand_poses.inc's pin order) and each of
// the four shells' 3x4 in l_hand's frame (row-major, basis in the COLUMNS, t at [3] [7] [11], metres; the shell object's own frame,
// unit scale) + whether it is in the hand (shellOn). Cycle k (1-4) = frames 36(k-1)+1 .. 36k; shell k is under the thumb at the
// cycle's frame 1, pushed in at cycle frame 23 (the ADD-BULLET) and hidden from frame 24.
// palm1 (fix round 1, ledger ruling R9) = l_hand1's LOCAL rotation (the palm bone, parent of fingers 3 / 4 - not one of the 16 pins;
// Jay's shell hold moves it), same convention; the runtime writes it where it writes the fingers.
struct AnimFrame {
    FingerRow fingers[16];
    float     shell[4][12];
    bool      shellOn[4];
    float     palm1[9];
};

#include "reload_pump_anim.inc"   // kPumpAnim[] (144 frames) + kPumpAnimFrameOneCheck[16][9] (pose_shellhold_B.json's locals)

constexpr int kPumpAnimFrames = static_cast<int>(sizeof(kPumpAnim) / sizeof(kPumpAnim[0]));
static_assert(kPumpAnimFrames == 144, "reload_pump_anim.inc: 144 frames (C13: 4 cycles x 36 at 30 fps)");
constexpr int kPumpCycleFrames = 36;

// Cycle 1-4, frame 1-36 -> kPumpAnim[(cycle - 1) * 36 + frame - 1]; out of range clamps into the table. Pure: two clamps, no scan.
inline const AnimFrame& PumpAnimFrame(int cycle, int frame)
{
    if (cycle < 1) { cycle = 1; }
    if (cycle > kPumpAnimFrames / kPumpCycleFrames) { cycle = kPumpAnimFrames / kPumpCycleFrames; }
    if (frame < 1) { frame = 1; }
    if (frame > kPumpCycleFrames) { frame = kPumpCycleFrames; }
    return kPumpAnim[(cycle - 1) * kPumpCycleFrames + frame - 1];
}

// ---- PS2 smoothing (Jay: the loading animation "seems low framerate basically, the animation itself is 'framey'") ----
// The 36 C13 frames were shown as whole frames, stepped by GetTickCount64 (~15.6 ms) - about 19 distinct poses per 0.3 s cycle. Now
// the frame is a FLOAT and every reader blends the two neighbouring frames. Pure, unit-tested (reload_hands_test.cpp).

// A float frame (1 .. 36) -> the lower frame k (1 .. 36), the upper k2 = min(k + 1, 36) and the blend t (0 .. 1). Out of range clamps.
inline void PumpFrameSplit(float f, int* k, int* k2, float* t)
{
    if (!(f >= 1.0f)) { f = 1.0f; }   // also NaN
    if (f > static_cast<float>(kPumpCycleFrames)) { f = static_cast<float>(kPumpCycleFrames); }
    int lo = static_cast<int>(f);
    if (lo >= kPumpCycleFrames) { lo = kPumpCycleFrames; }
    *k = lo;
    *k2 = lo < kPumpCycleFrames ? lo + 1 : lo;
    *t = lo < kPumpCycleFrames ? f - static_cast<float>(lo) : 0.0f;
}

// Two neighbouring frames' 3x3 rotations (row-major, basis in the COLUMNS), blended: the 9 floats lerped, then Gram-Schmidt on the
// columns (x, then y, z = x cross y) - neighbouring C13 frames are a few degrees apart, so this matches a slerp to well under 0.1 deg.
// COST: 9 mul-adds + 2 sqrt + a cross. Degenerate (never for real frames) -> a's rotation.
inline void LerpRot9(const float a[9], const float b[9], float t, float out[9])
{
    float m[9];
    for (int i = 0; i < 9; ++i) { m[i] = a[i] + t * (b[i] - a[i]); }
    float x[3] = { m[0], m[3], m[6] }, y[3] = { m[1], m[4], m[7] };
    const float lx = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    if (!(lx > 1e-6f)) { for (int i = 0; i < 9; ++i) { out[i] = a[i]; } return; }
    for (float& v : x) { v /= lx; }
    const float d = x[0] * y[0] + x[1] * y[1] + x[2] * y[2];
    for (int i = 0; i < 3; ++i) { y[i] -= d * x[i]; }
    const float ly = std::sqrt(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
    if (!(ly > 1e-6f)) { for (int i = 0; i < 9; ++i) { out[i] = a[i]; } return; }
    for (float& v : y) { v /= ly; }
    const float z[3] = { x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0] };
    out[0] = x[0]; out[1] = y[0]; out[2] = z[0];
    out[3] = x[1]; out[4] = y[1]; out[5] = z[1];
    out[6] = x[2]; out[7] = y[2]; out[8] = z[2];
}

// The same for a 3x4 (t at [3] [7] [11] lerped).
inline void Lerp34(const float a[12], const float b[12], float t, float out[12])
{
    float ra[9], rb[9], r[9];
    for (int rr = 0; rr < 3; ++rr) { for (int c = 0; c < 3; ++c) { ra[3 * rr + c] = a[4 * rr + c]; rb[3 * rr + c] = b[4 * rr + c]; } }
    LerpRot9(ra, rb, t, r);
    for (int rr = 0; rr < 3; ++rr) {
        for (int c = 0; c < 3; ++c) { out[4 * rr + c] = r[3 * rr + c]; }
        out[4 * rr + 3] = a[4 * rr + 3] + t * (b[4 * rr + 3] - a[4 * rr + 3]);
    }
}

// The finger pass's published word (one 64-bit interlocked store, any thread reads it): bits 63..24 the publish time in quarter-ms
// (a high-resolution clock), 23..16 the cycle (1 .. 4; 0 = the HOLD - cycle 1 frame 1, never advanced), 15..0 the frame x 256.
constexpr long long kPumpAnimFreshQ = 150 * 4;   // stale after 150 ms (kPumpPublishFreshMs), in quarter-ms
inline long long PumpAnimPack(long long nowQ, int cycle, float frame)
{
    if (cycle < 0 || cycle > 4) { cycle = 0; }
    if (!(frame >= 1.0f)) { frame = 1.0f; }
    if (frame > static_cast<float>(kPumpCycleFrames)) { frame = static_cast<float>(kPumpCycleFrames); }
    const long long fq = static_cast<long long>(frame * 256.0f + 0.5f);
    return ((nowQ & 0xFFFFFFFFFFLL) << 24) | (static_cast<long long>(cycle) << 16) | (fq & 0xFFFF);
}
// Unpack at the reader's time: the cycle (1 .. 4) and the frame ADVANCED by the time since the publish (cycleFrames per cycleS), held
// at the cycle's last frame; the HOLD is cycle 1 frame 1, not advanced. False = no word / stale / malformed.
inline bool PumpAnimUnpack(long long w, long long nowQ, float cycleS, int* cycle, float* frame)
{
    if (!w || !(cycleS > 0.0f)) { return false; }
    const long long stampQ = (w >> 24) & 0xFFFFFFFFFFLL;
    const long long ageQ = (nowQ & 0xFFFFFFFFFFLL) - stampQ;
    if (ageQ < -4 || ageQ > kPumpAnimFreshQ) { return false; }
    const int c = static_cast<int>((w >> 16) & 0xFF);
    const float f = static_cast<float>(w & 0xFFFF) / 256.0f;
    if (c > 4 || !(f >= 1.0f) || f > static_cast<float>(kPumpCycleFrames)) { return false; }
    if (c == 0) { *cycle = 1; *frame = 1.0f; return true; }
    float g = f + (ageQ > 0 ? static_cast<float>(ageQ) : 0.0f) * 0.25f * static_cast<float>(kPumpCycleFrames) / (cycleS * 1000.0f);
    if (g > static_cast<float>(kPumpCycleFrames)) { g = static_cast<float>(kPumpCycleFrames); }
    *cycle = c;
    *frame = g;
    return true;
}

}  // namespace reload
}  // namespace dlvr
