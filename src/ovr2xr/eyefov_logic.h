#pragma once
// One frustum for both eyes (INV-597; docs/BETA-FINDINGS-2026-10-19.md 7f; Jay, 2026-10-19: "Build it.").
//
// With symmetric_fov=2 GameEyeFov symmetrises EACH eye from its own real FOV. On a headset whose eyes are mirror images
// (Quest 3, PSVR2) the two results are identical; on one whose eyes are not (Steam Frame: eye0 1.7260, eye1 1.6938) they
// differ, and the AFW rebuild - which draws the held eye with the FRESH eye's projection (dlssflat.cpp AfwRebuildRun) - puts
// every rebuilt frame 1.9 % off in zoom. Handing both eyes the frustum that contains both makes the rebuild's one-projection
// assumption true on every headset. A superset, so no edge is lost; the narrower eye renders a little beyond its lens.

namespace dlvr::eyefov {

struct Tans { float l, r, u, d; };

inline float Max(float a, float b) { return a > b ? a : b; }

// The smallest frustum containing both eyes' frustums. Symmetric in its arguments, so each eye computing
// Shared(own, other) lands on the identical frustum.
inline Tans Shared(const Tans& a, const Tans& b)
{
    return Tans{ Max(a.l, b.l), Max(a.r, b.r), Max(a.u, b.u), Max(a.d, b.d) };
}

inline bool Differ(const Tans& a, const Tans& b)
{
    return a.l != b.l || a.r != b.r || a.u != b.u || a.d != b.d;
}

// symmetric_fov: 0 = as the runtime gives it, 1 = up / down equal, 2 = left / right too (the smallest centred frustum that contains it).
inline Tans Symmetrise(Tans t, int mode)
{
    if (mode >= 1) { t.u = t.d = Max(t.u, t.d); }
    if (mode >= 2) { t.l = t.r = Max(t.l, t.r); }
    return t;
}

// Grow (never shrink) to the eye texture's shape (width / height), so the tangent-derived and shape-derived readings of the frustum
// are the same frustum. A degenerate frustum or shape comes back as it is.
inline Tans GrowToAspect(Tans t, float aspect)
{
    const float tw = t.l + t.r, th = t.u + t.d;
    if (!(tw > 0.0f) || !(th > 0.0f) || !(aspect > 0.0f)) { return t; }
    if (tw / th > aspect) {
        const float k = (tw / aspect) / th;   // widen vertically
        t.u *= k;
        t.d *= k;
    } else {
        const float k = (th * aspect) / tw;   // widen horizontally
        t.l *= k;
        t.r *= k;
    }
    return t;
}

// ---- THE LIVE EYE FOV (trace docs/traces/20261125_1040_live_eye_fov.md; Jay 2026-11-25) ----------------------------------------
// The FOV the engine is asked to draw each eye with (ovr_GetRenderDesc, every frame). It starts as the one saved by the previous
// launch - FROZEN, because the live one is not known yet - and switches ONCE to the live one. A headset driver may change the FOV at every
// start (Sboys3's burn-in prevention: a tester's 2026-11-24 launch drew 0.5 deg short on every side and the SteamVR panel crop was refused).
// The switch must never be SEEN: frames recorded before it and submitted after it would be declared with the wrong FOV. So it
// happens only while no game image is on the display, and the game image is kept off it for kHoldSubmits more submits (at most 2
// frames in flight, + 1 for AFW's held eye). Every call is made under the layer's frame lock.
inline bool Plausible(const Tans& t)
{
    const float v[4] = { t.l, t.r, t.u, t.d };
    for (float x : v) {
        if (!(x >= 0.2f) || !(x <= 6.0f)) { return false; }   // NaN and infinity fail here
    }
    return true;
}

inline bool Close(const Tans& a, const Tans& b, float eps)
{
    const float d[4] = { a.l - b.l, a.r - b.r, a.u - b.u, a.d - b.d };
    for (float x : d) {
        if (!(x <= eps) || !(x >= -eps)) { return false; }
    }
    return true;
}

constexpr int   kHoldSubmits = 4;
constexpr float kSameTan = 1e-4f;   // under 0.006 deg: the saved file's rounding, not a different FOV

struct LiveFov {
    bool haveStart = false;    // start[] holds the FOV handed out before the switch
    bool latched = false;      // the switch is done (for the launch)
    bool saidWait = false, saidRefused = false;
    bool everShown = false;    // a game image has been submitted since launch
    bool lastHidden = false;   // the last submit's game image was not submitted
    int  hold = 0;             // submits still to be kept off the display after the switch
    Tans start[2] = {};
    Tans live[2] = {};
    bool useLive = false;
};

enum class LatchResult { kNone, kRefused, kWaiting, kStillWaiting, kSame, kSwitched };

inline bool Ready(const LiveFov& f) { return f.haveStart; }

inline void SetStart(LiveFov& f, const Tans start[2])
{
    if (f.haveStart) { return; }
    f.haveStart = true;
    f.start[0] = start[0];
    f.start[1] = start[1];
}

// The FOV eye `eye` is drawn with and declared with now. Only after SetStart.
inline const Tans& Used(const LiveFov& f, int eye) { return f.useLive ? f.live[eye & 1] : f.start[eye & 1]; }

// Asked before each answer. `located` = the runtime's views are valid now; live[] = what would be handed out from them.
inline LatchResult TryLatch(LiveFov& f, bool located, const Tans live[2])
{
    if (!f.haveStart || f.latched || !located) { return LatchResult::kNone; }
    if (!Plausible(live[0]) || !Plausible(live[1])) {
        if (f.saidRefused) { return LatchResult::kNone; }
        f.saidRefused = true;
        return LatchResult::kRefused;
    }
    if (Close(live[0], f.start[0], kSameTan) && Close(live[1], f.start[1], kSameTan)) {
        f.latched = true;      // the startup numbers stay: nothing changes, nothing to hide
        return LatchResult::kSame;
    }
    if (f.everShown && !f.lastHidden) {
        if (f.saidWait) { return LatchResult::kStillWaiting; }
        f.saidWait = true;
        return LatchResult::kWaiting;
    }
    f.latched = true;
    f.useLive = true;
    f.live[0] = live[0];
    f.live[1] = live[1];
    f.hold = kHoldSubmits;
    return LatchResult::kSwitched;
}

// Once per submit: would this submit show the game image? Returns whether it may.
inline bool NoteSubmit(LiveFov& f, bool wouldShow)
{
    bool show = wouldShow;
    if (f.hold > 0) {
        --f.hold;
        show = false;
    }
    if (show) { f.everShown = true; }
    f.lastHidden = !show;
    return show;
}

// Meta's PC OpenXR runtime (Quest Link / Air Link) does not map a projection view's declared fov onto the display - it spreads
// the image over its OWN field of view (Venture PR #79, measured on "Oculus" 1.208.0 + Quest 3; OpenXR rendering.adoc says a
// runtime "will map the view and FOV to the system display appropriately"). A symmetrised frustum then lands squeezed and
// off-centre per eye = the cross-eye a tester reported at symmetric_fov=2, gone at 0 (Jay 2026-11-16: "A"). "Oculus" is the
// only name on record for that runtime (tester logs 20261115_210537, another tester 2026-10-18). Since 2026-11-24 each view is cut to that
// runtime's own window at submit (fovcrop_logic.h CropScaled; Jay: "Go ahead with 1") instead of forcing symmetric_fov to 0.
inline bool RuntimeStretchesToOwnFov(const char* runtimeName)
{
    static const char kName[] = "Oculus";
    if (!runtimeName) {
        return false;
    }
    for (int i = 0; kName[i]; ++i) {
        if (runtimeName[i] != kName[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace dlvr::eyefov
