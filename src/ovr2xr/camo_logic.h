// The camouflage gesture - the rules (docs/superpowers/specs/2026-11-07-camouflage-gesture-design.md, plan
// docs/superpowers/plans/2026-11-07-camouflage-gesture.md Task 1). Pure: no Windows, no game memory, no logging, so
// src/src/tests/camo_logic_test.cpp compiles it on its own. camogesture.cpp runs WipeStep on the pad thread (the hand in the head's
// frame, from OpenXR poses) and Decide / LimbOwners on the game thread.
//
// HEAD FRAME (OpenXR): +x right, +y up, -z forward. The face zone: z in [-fwdM, +kBehindM], |x| <= radiusM, |y| <= radiusM (judged in
// the head's frame). A WIPE = while inside, the hand's REAL travel (tracking space) along the head's right direction AT ENTRY spans
// >= sweepM within maxS of entering; it fires once per entry (leaving the zone re-arms). Final review I1: the travel is the hand's own,
// so turning or nodding the head with the hand still is never a wipe.
#pragma once

#include <cmath>

namespace camo {

constexpr float kBehindM = 0.05f;     // how far behind the head's centre the zone reaches (a hand at the cheek still counts)
constexpr float kSweepEpsM = 1e-4f;   // float slack on the sweep comparison
constexpr int   kMaxLimbs = 128;

struct WipeParams {
    float fwdM;      // camo_wipe_fwd_m
    float radiusM;   // camo_wipe_radius_m
    float sweepM;    // camo_wipe_sweep_m
    float maxS;      // camo_wipe_max_s
};

struct WipeState {   // zero-init = outside
    bool   inside;
    bool   fired;
    float  entryTrk[3];   // the hand at entry, tracking space
    float  axis[3];       // the head's right direction at entry, tracking space (unit)
    float  minX, maxX;    // the hand's travel along `axis` since entry
    double entryT;
};

// The head's right direction (+x of the head frame) in tracking space, for an orientation q (x, y, z, w). A degenerate q gives (1,0,0).
inline void HeadRight(const float q[4], float out[3])
{
    const float n2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (!std::isfinite(n2) || n2 < 0.5f || n2 > 2.0f) {
        out[0] = 1.0f; out[1] = 0.0f; out[2] = 0.0f;
        return;
    }
    const float inv = 1.0f / n2;   // R * (1,0,0) = first column of the rotation matrix (scaled for a non-unit q)
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    out[0] = (w * w + x * x - y * y - z * z) * inv;
    out[1] = 2.0f * (x * y + w * z) * inv;
    out[2] = 2.0f * (x * z - w * y) * inv;
}

// p (tracking space) in the frame of a head at headPos with orientation headQ (x, y, z, w). False on a non-finite input or a quaternion
// that is not close to unit length (|q|^2 outside 0.5..2).
inline bool HeadLocal(const float headPos[3], const float headQ[4], const float p[3], float out[3])
{
    float n2 = headQ[0] * headQ[0] + headQ[1] * headQ[1] + headQ[2] * headQ[2] + headQ[3] * headQ[3];
    if (!std::isfinite(n2) || n2 < 0.5f || n2 > 2.0f) { return false; }
    const float inv = 1.0f / std::sqrt(n2);
    // the inverse rotation = the conjugate: vector part negated
    const float ux = -headQ[0] * inv, uy = -headQ[1] * inv, uz = -headQ[2] * inv, w = headQ[3] * inv;
    const float v[3] = { p[0] - headPos[0], p[1] - headPos[1], p[2] - headPos[2] };
    if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) { return false; }
    // v' = v + 2w (u x v) + 2 u x (u x v)
    const float cx = uy * v[2] - uz * v[1], cy = uz * v[0] - ux * v[2], cz = ux * v[1] - uy * v[0];
    const float dx = uy * cz - uz * cy, dy = uz * cx - ux * cz, dz = ux * cy - uy * cx;
    out[0] = v[0] + 2.0f * (w * cx + dx);
    out[1] = v[1] + 2.0f * (w * cy + dy);
    out[2] = v[2] + 2.0f * (w * cz + dz);
    return true;
}

inline bool InZone(const WipeParams& prm, const float h[3])
{
    return h[2] >= -prm.fwdM && h[2] <= kBehindM && std::fabs(h[0]) <= prm.radiusM && std::fabs(h[1]) <= prm.radiusM;
}

// One pad-thread sample at time tS: handHead = the hand in the head's frame (the zone test), handTrk = the same hand in tracking space,
// headRight = the head's right direction in tracking space now (HeadRight; used at entry only). True ONCE per zone entry, on the sample
// whose travel along the entry right direction reaches sweepM, provided that is within maxS of entering.
inline bool WipeStep(WipeState& s, const WipeParams& prm, const float handHead[3], const float handTrk[3], const float headRight[3],
                     double tS)
{
    if (!InZone(prm, handHead)) {
        s.inside = false;
        return false;
    }
    if (!s.inside) {
        s.inside = true;
        s.fired = false;
        for (int k = 0; k < 3; ++k) {
            s.entryTrk[k] = handTrk[k];
            s.axis[k] = headRight[k];
        }
        s.minX = 0.0f;
        s.maxX = 0.0f;
        s.entryT = tS;
        return false;
    }
    const float x = (handTrk[0] - s.entryTrk[0]) * s.axis[0] + (handTrk[1] - s.entryTrk[1]) * s.axis[1] +
                    (handTrk[2] - s.entryTrk[2]) * s.axis[2];
    if (s.fired) { return false; }
    if (x < s.minX) { s.minX = x; }
    if (x > s.maxX) { s.maxX = x; }
    if (tS - s.entryT > static_cast<double>(prm.maxS)) { return false; }
    if (s.maxX - s.minX + kSweepEpsM >= prm.sweepM) {
        s.fired = true;
        return true;
    }
    return false;
}

enum class Outcome { Start, NotHolding, NotInfected, UsedUp, NotReady, ModeBlocked, NoDuration, Unreadable };

struct Facts {
    bool  holding;    // the hand holds a corpse part or a severed limb (body grab)
    bool  readable;   // every game read behind the facts below succeeded
    bool  infected;   // the corpse (or the limb's recorded owner) is an infected one
    bool  used;       // the corpse's once-flag AI+5713 is clear (or the limb's own use is spent when its corpse is gone)
    bool  ready;      // the game's own corpse conditions: module+408 > 1.5, not dissolved, renderable (a limb whose corpse is gone: true)
    bool  modeOk;     // CanUseCamouflage(true) and not the trigger offer's GameDI gate
    float durationS;  // the game's duration for this player
};

inline Outcome Decide(const Facts& f)
{
    if (!f.holding) { return Outcome::NotHolding; }
    if (!f.readable) { return Outcome::Unreadable; }
    if (!f.infected) { return Outcome::NotInfected; }
    if (f.used) { return Outcome::UsedUp; }
    if (!f.ready) { return Outcome::NotReady; }
    if (!f.modeOk) { return Outcome::ModeBlocked; }
    if (!(f.durationS > 0.0f)) { return Outcome::NoDuration; }
    return Outcome::Start;
}

inline const char* OutcomeName(Outcome o)
{
    switch (o) {
    case Outcome::Start: return "started";
    case Outcome::NotHolding: return "not holding";
    case Outcome::Unreadable: return "unreadable";
    case Outcome::NotInfected: return "not infected";
    case Outcome::UsedUp: return "used up";
    case Outcome::NotReady: return "not ready";
    case Outcome::ModeBlocked: return "mode blocked";
    case Outcome::NoDuration: return "no duration";
    }
    return "?";
}

// Severed limb -> the AI it was cut from, recorded when it is cut (our relicts carry no owner, INV-704). Fixed size, oldest evicted.
// Not thread-safe: camogesture.cpp guards it.
class LimbOwners {
public:
    // limbId / ownerId = the limb's and the owner's engine object ids at the cut (body grab's id lookup re-validates each before it is
    // trusted again - final review I2: a later object at the same address has another id, so a stale entry is never inherited).
    void Record(const void* limb, int limbId, const void* owner, int ownerId, bool infected)
    {
        if (!limb) { return; }
        Entry* e = FindEntry(limb);
        if (!e) {
            e = &m_e[m_next];   // the ring's oldest slot (or an empty one); a live entry there is evicted
            m_next = (m_next + 1) % kMaxLimbs;
            if (!e->limb) { ++m_count; }
        }
        e->limb = limb;
        e->limbId = limbId;
        e->owner = owner;
        e->ownerId = ownerId;
        e->infected = infected;
        e->ownUsed = false;
    }
    bool Find(const void* limb, int* limbId, const void** owner, int* ownerId, bool* infected, bool* ownUsed) const
    {
        for (int i = 0; i < kMaxLimbs; ++i) {
            if (limb && m_e[i].limb == limb) {
                *limbId = m_e[i].limbId;
                *owner = m_e[i].owner;
                *ownerId = m_e[i].ownerId;
                *infected = m_e[i].infected;
                *ownUsed = m_e[i].ownUsed;
                return true;
            }
        }
        return false;
    }
    void MarkOwnUsed(const void* limb)
    {
        if (Entry* e = FindEntry(limb)) { e->ownUsed = true; }
    }
    void Forget(const void* limb)
    {
        if (Entry* e = FindEntry(limb)) {
            *e = Entry{};
            --m_count;
        }
    }
    int Count() const { return m_count; }

private:
    struct Entry {
        const void* limb = nullptr;
        int limbId = 0;
        const void* owner = nullptr;
        int ownerId = 0;
        bool infected = false;
        bool ownUsed = false;
    };
    Entry* FindEntry(const void* limb)
    {
        for (int i = 0; i < kMaxLimbs; ++i) {
            if (limb && m_e[i].limb == limb) { return &m_e[i]; }
        }
        return nullptr;
    }
    Entry m_e[kMaxLimbs];
    int m_next = 0;
    int m_count = 0;
};

}  // namespace camo
