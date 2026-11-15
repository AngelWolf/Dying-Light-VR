// swingshadow_logic.h - the pure half of the swing shadow probe (diag_swing_shadow).
// docs/superpowers/specs/2026-10-26-swing-shadow-probe-design.md §4.2-4.3; test src/src/tests/swingshadow_logic_test.cpp.
// No Windows, no OpenXR, no logging, no globals, no allocation - swing.cpp's probe calls these and does all I/O itself.
#pragma once

#include <cmath>

namespace dlvr {
namespace swingshadow {

struct V3 { float x, y, z; };

inline V3    Sub(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline bool  Finite(V3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

// Degrees between two directions; -1 when either is shorter than 1e-6 or non-finite (swing.cpp AngleDeg's contract).
inline float AngleDeg(V3 a, V3 b)
{
    if (!Finite(a) || !Finite(b)) { return -1.0f; }
    const float la = std::sqrt(Dot(a, a));
    const float lb = std::sqrt(Dot(b, b));
    if (!(la > 1e-6f) || !(lb > 1e-6f)) { return -1.0f; }
    float d = Dot(a, b) / (la * lb);
    d = d > 1.0f ? 1.0f : (d < -1.0f ? -1.0f : d);
    return std::acos(d) * 57.29578f;
}

inline float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// Closest distance between segments p0-p1 and q0-q1; *sOut / *tOut = the parameters (0..1) of the closest points.
// The standard clamped closest-point solve (degenerate segments handled as points).
inline float SegSegDist(V3 p0, V3 p1, V3 q0, V3 q1, float* sOut, float* tOut)
{
    const V3 d1 = Sub(p1, p0), d2 = Sub(q1, q0), r = Sub(p0, q0);
    const float a = Dot(d1, d1), e = Dot(d2, d2), f = Dot(d2, r);
    float s = 0.0f, t = 0.0f;
    if (a <= 1e-12f && e <= 1e-12f) {
        s = t = 0.0f;
    } else if (a <= 1e-12f) {
        t = Clamp01(f / e);
    } else {
        const float c = Dot(d1, r);
        if (e <= 1e-12f) {
            s = Clamp01(-c / a);
        } else {
            const float b = Dot(d1, d2);
            const float den = a * e - b * b;
            s = den > 1e-12f ? Clamp01((b * f - c * e) / den) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) { t = 0.0f; s = Clamp01(-c / a); }
            else if (t > 1.0f) { t = 1.0f; s = Clamp01((b - c) / a); }
        }
    }
    if (sOut) { *sOut = s; }
    if (tOut) { *tOut = t; }
    const V3 cp = { p0.x + d1.x * s, p0.y + d1.y * s, p0.z + d1.z * s };
    const V3 cq = { q0.x + d2.x * t, q0.y + d2.y * t, q0.z + d2.z * t };
    const V3 dd = Sub(cp, cq);
    return std::sqrt(Dot(dd, dd));
}

// The ring-index pairs B1Build sub-steps, so the shadow uses the SAME span (swing.cpp B1Build, DEV :565-603):
// backfill -> the newest frame at or before candStartMs (newest-first search over the n valid entries), capped at the
// oldest valid entry with *truncated = true; then every ADJACENT pair from the oldest, both ok (never bridged).
// No backfill, or no ok pair in it -> the single pair (head-1, head) when both are ok (B1 then uses g_prev / g_cur).
// tMs / ok are indexed by RING index (size ringSize); head = the newest entry. Returns the pair count (<= maxPairs).
inline int ShadowPairs(const double* tMs, const bool* ok, int n, int head, int ringSize, double candStartMs, bool backfill,
                       int (*pairs)[2], int maxPairs, bool* truncated)
{
    if (truncated) { *truncated = false; }
    if (n <= 0 || ringSize <= 0 || head < 0) { return 0; }
    auto at = [&](int k) { return ((head - k) % ringSize + ringSize) % ringSize; };
    int span = 0;
    if (backfill) {
        int k = 0;
        for (; k < n; ++k) {
            if (tMs[at(k)] <= candStartMs) { break; }
        }
        if (k == n) {
            k = n - 1;
            if (truncated) { *truncated = true; }
        }
        span = k + 1;
    }
    int np = 0;
    for (int k = span - 1; k >= 1 && np < maxPairs; --k) {
        const int a = at(k), b = at(k - 1);
        if (!ok[a] || !ok[b]) { continue; }
        pairs[np][0] = a;
        pairs[np][1] = b;
        ++np;
    }
    if (np == 0 && n >= 2 && maxPairs > 0) {
        const int a = at(1), b = at(0);
        if (ok[a] && ok[b]) {
            pairs[0][0] = a;
            pairs[0][1] = b;
            np = 1;
        }
    }
    return np;
}

enum class Cls { Both, GameOnly, BladeOnly, Neither, Split, Incomplete, NoB1 };

inline const char* ClsName(Cls c)
{
    switch (c) {
    case Cls::Both:       return "BOTH";
    case Cls::GameOnly:   return "GAME ONLY";
    case Cls::BladeOnly:  return "BLADE ONLY";
    case Cls::Neither:    return "NEITHER";
    case Cls::Split:      return "SPLIT";
    case Cls::Incomplete: return "INCOMPLETE";
    case Cls::NoB1:       return "NO B1";
    }
    return "?";
}

struct Key { unsigned long long obj; int elem; };
struct Tally { int both, gameOnly, bladeOnly; Cls cls; };

// One swing's target sets (union over its frames), compared by OBJECT - the element is reported, not compared.
// NoB1 when the game never ran its hit this swing; Incomplete when the shadow could not run for all of it.
inline Tally Classify(const Key* game, int nGame, const Key* blade, int nBlade, bool b1Armed, bool shadowComplete)
{
    Tally t{ 0, 0, 0, Cls::Neither };
    auto in = [](const Key* set, int n, unsigned long long o) {
        for (int i = 0; i < n; ++i) { if (set[i].obj == o) { return true; } }
        return false;
    };
    for (int i = 0; i < nGame; ++i) {
        if (in(blade, nBlade, game[i].obj)) { ++t.both; } else { ++t.gameOnly; }
    }
    for (int i = 0; i < nBlade; ++i) {
        if (!in(game, nGame, blade[i].obj)) { ++t.bladeOnly; }
    }
    if (!b1Armed) { t.cls = Cls::NoB1; return t; }
    if (!shadowComplete) { t.cls = Cls::Incomplete; return t; }
    if (t.both > 0) { t.cls = Cls::Both; }
    else if (t.gameOnly > 0 && t.bladeOnly > 0) { t.cls = Cls::Split; }
    else if (t.gameOnly > 0) { t.cls = Cls::GameOnly; }
    else if (t.bladeOnly > 0) { t.cls = Cls::BladeOnly; }
    else { t.cls = Cls::Neither; }
    return t;
}

// ===== [SWINGLOS] - the melee line-of-sight replica (spec 2026-10-26-melee-los-probe-design.md, INV-632 / INV-625) =====

// Ray 1's end point exactly as gamedll sub_180C48E30 :272-325 computes it: the mean of every 13-float quad's two START points
// (floats 0..2 = prev start, 6..8 = cur start), divided by 2 x count - never the end points. False on no quads or a non-finite sum.
inline bool Centroid(const float* quads, unsigned count, float out[3])
{
    if (!quads || count == 0) { return false; }
    double s[3] = { 0.0, 0.0, 0.0 };
    for (unsigned i = 0; i < count; ++i) {
        const float* e = quads + 13 * static_cast<size_t>(i);
        for (int k = 0; k < 3; ++k) { s[k] += static_cast<double>(e[k]) + static_cast<double>(e[6 + k]); }
    }
    for (int k = 0; k < 3; ++k) {
        out[k] = static_cast<float>(s[k] / (2.0 * count));
        if (!std::isfinite(out[k])) { return false; }
    }
    return true;
}

// What one ray means to the search (C48E30 :365 / :536-547): clear passes every candidate; a hit passes only the object it hit
// (coll+24 == candidate+0x18 - `targets18` holds candidate+0x18 values) or surface 3 (SurfaceId_Water); anything else is a block,
// by a model (back-pointer set) or by world geometry / a hit with no model (null back-pointer).
enum class Los { Clear, Target, Water, Blocked, BlockedWorld };

inline const char* LosName(Los v)
{
    switch (v) {
    case Los::Clear:        return "clear";
    case Los::Target:       return "target";
    case Los::Water:        return "water";
    case Los::Blocked:      return "blocked";
    case Los::BlockedWorld: return "blocked-world";
    }
    return "?";
}

inline Los LosVerdict(bool hit, unsigned long long backPtr, unsigned short surface, const unsigned long long* targets18, int n)
{
    if (!hit) { return Los::Clear; }
    for (int i = 0; targets18 && i < n; ++i) {
        if (backPtr != 0 && targets18[i] == backPtr) { return Los::Target; }
    }
    if (surface == 3) { return Los::Water; }
    return backPtr ? Los::Blocked : Los::BlockedWorld;
}

// Positive control: a ray 1 that blocks (not the target, not water) leaves the search NO candidate, so the game's own list for
// that call must be empty. False = our replica disagrees with the game's ray.
inline bool Agree(Los ray1, int gameHits)
{
    return !((ray1 == Los::Blocked || ray1 == Los::BlockedWorld) && gameHits > 0);
}

}  // namespace swingshadow
}  // namespace dlvr
