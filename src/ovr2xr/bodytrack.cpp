// bodytrack.cpp - THE BODY-TRACKED ELBOW's OpenXR side. See bodytrack.h; the maths and the design note are in bodytrack_logic.h.
#include "bodytrack.h"

#include "bodytrack_logic.h"
#include "xrloader.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

namespace dlvr {

// [DEP: rig_elbow_body_tracking] required-for=the body-tracked elbow (RigElbowBodyTracking): when the OpenXR runtime offers
//      XR_FB_body_tracking (VDXR with Virtual Desktop's "Forward tracking data" on), the tracked elbow sets which way each game
//      elbow points (boneblend.cpp SolveArmIk, after the bow steer, before the rate limit); the hands stay on the controllers and
//      the arm lengths stay the game's. Faded (0.3 s) to the normal elbow whenever the arm's sample is not usable. Read every solve
//      (live from the menu), but the extension must be on the instance, which is decided at launch whatever the setting.
//      2026-11-08, Jay: "I'd like to add it as an additional option for headsets that DO support it. Mainly just for the elbows"
//      -> the design in chat -> "Yes" (OFF by default, Tier C until verified in the headset). LOCKED OFF + HIDDEN 2026-11-08 after
//      RUN BT1 / BT2 (VD forwarded no arm joint positions; Jay: "unfinished & hidden"). tree=INERT while 0 (one bool test
//      per locate and per solve); LOADBEARING while 1 and a body is tracked (it supplies the elbow pole, which every arm bone's roll,
//      the hand and the weapon inherit). HEADSET.
bool RigElbowBodyTracking = false;

namespace {

PFN_xrCreateBodyTrackerFB   g_create   = nullptr;
PFN_xrDestroyBodyTrackerFB  g_destroy  = nullptr;
PFN_xrLocateBodyJointsFB    g_locate   = nullptr;
XrSession                   g_session  = XR_NULL_HANDLE;
XrBodyTrackerFB             g_tracker  = XR_NULL_HANDLE;
bool                        g_extOn    = false;   // the extension is on the instance
bool                        g_supported = false;  // XrSystemBodyTrackingPropertiesFB says so
bool                        g_createFailed = false;
SRWLOCK                     g_lock     = SRWLOCK_INIT;
XrTime                      g_lastTime = 0;

// published, under g_lock
BodyArm g_arm[2] = {};
bool    g_have = false;
uint64_t g_seq = 0;

// glitch guard state (locate side, under g_lock exclusive)
float  g_prevE[2][3] = {};
XrTime g_prevEChange[2] = { 0, 0 };
bool   g_prevHave[2] = { false, false };

// 5 s line, under g_lock exclusive
struct Stats {
    uint64_t locates, active, located[2], usable[2], inactive, noJoint[2], glitch[2], straight[2];
    double   confSum, upperSum[2], lowerSum[2], shSum[2];
    uint64_t lenN[2];
};
Stats     g_st = {};
ULONGLONG g_lastLine = 0;
bool      g_saidFirst = false, g_saidOff = false, g_saidLocateFail = false;

// R: shoulder (ARM_UPPER), elbow (ARM_LOWER), wrist (HAND_WRIST), plus SHOULDER for the self-check; L the same.
constexpr int kJ[2][4] = {
    { XR_BODY_JOINT_RIGHT_ARM_UPPER_FB, XR_BODY_JOINT_RIGHT_ARM_LOWER_FB, XR_BODY_JOINT_RIGHT_HAND_WRIST_FB, XR_BODY_JOINT_RIGHT_SHOULDER_FB },
    { XR_BODY_JOINT_LEFT_ARM_UPPER_FB,  XR_BODY_JOINT_LEFT_ARM_LOWER_FB,  XR_BODY_JOINT_LEFT_HAND_WRIST_FB,  XR_BODY_JOINT_LEFT_SHOULDER_FB },
};

const char* kExt = "XR_FB_body_tracking";

inline void P3(const XrBodyJointLocationFB& j, float o[3]) { o[0] = j.pose.position.x; o[1] = j.pose.position.y; o[2] = j.pose.position.z; }

}  // namespace

bool BodyTrackAddExtension(const char** exts, uint32_t* count, uint32_t cap)
{
    g_extOn = false;
    uint32_t n = 0;
    if (!xrEnumerateInstanceExtensionProperties_ ||
        XR_FAILED(xrEnumerateInstanceExtensionProperties_(nullptr, 0, &n, nullptr)) || n == 0 || n > 4096) {
        // [PROBE: BODYTRACK] gate=none  site=HEADSET  log-only=no (the feature's own status, unconditional)
        LOG("[BODYTRACK] the runtime's extension list could not be read (%u entries) - %s is NOT requested; body-tracked elbows are "
            "unavailable this launch", n, kExt);
        return false;
    }
    XrExtensionProperties* props = new XrExtensionProperties[n];
    for (uint32_t i = 0; i < n; ++i) { props[i] = XrExtensionProperties{ XR_TYPE_EXTENSION_PROPERTIES }; }
    bool listed = false;
    if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties_(nullptr, n, &n, props))) {
        for (uint32_t i = 0; i < n; ++i) {
            if (strcmp(props[i].extensionName, kExt) == 0) { listed = true; break; }
        }
    }
    delete[] props;
    if (listed && *count < cap) {
        exts[(*count)++] = kExt;
        g_extOn = true;
    }
    // [PROBE: BODYTRACK] gate=none  site=HEADSET  log-only=no (the feature's own status, unconditional)
    LOG("[BODYTRACK] %s %s (rig_elbow_body_tracking=%d) - %s", kExt,
        g_extOn ? "LISTED by the runtime - requested on the instance" : (listed ? "listed but NO ROOM in the request" :
        "NOT LISTED by the runtime - not requested"), RigElbowBodyTracking ? 1 : 0,
        g_extOn ? "the elbows can follow a tracked body when the setting is on" : "body-tracked elbows are unavailable this launch");
    return g_extOn;
}

void BodyTrackInit(XrInstance instance, XrSystemId systemId, XrSession session)
{
    g_session = session;
    if (!g_extOn) {
        // [PROBE: BODYTRACK] gate=none  site=HEADSET  log-only=no
        LOG("[BODYTRACK] INIT skipped - %s is not on the instance (see the line above)", kExt);
        return;
    }
    XrSystemBodyTrackingPropertiesFB bp{ XR_TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_FB };
    XrSystemProperties sp{ XR_TYPE_SYSTEM_PROPERTIES };
    sp.next = &bp;
    const XrResult pr = xrGetSystemProperties_(instance, systemId, &sp);
    g_supported = XR_SUCCEEDED(pr) && bp.supportsBodyTracking == XR_TRUE;
    const bool f1 = XR_SUCCEEDED(xrGetInstanceProcAddr_(instance, "xrCreateBodyTrackerFB", reinterpret_cast<PFN_xrVoidFunction*>(&g_create)));
    const bool f2 = XR_SUCCEEDED(xrGetInstanceProcAddr_(instance, "xrDestroyBodyTrackerFB", reinterpret_cast<PFN_xrVoidFunction*>(&g_destroy)));
    const bool f3 = XR_SUCCEEDED(xrGetInstanceProcAddr_(instance, "xrLocateBodyJointsFB", reinterpret_cast<PFN_xrVoidFunction*>(&g_locate)));
    if (!(f1 && f2 && f3 && g_create && g_destroy && g_locate)) {
        g_supported = false;
    }
    // [PROBE: BODYTRACK] gate=none  site=HEADSET  log-only=no
    LOG("[BODYTRACK] INIT - system properties %d, supportsBodyTracking %d, entry points %d/%d/%d -> %s (rig_elbow_body_tracking=%d; "
        "Virtual Desktop: \"Forward tracking data\" must be on for VDXR to report a body)",
        static_cast<int>(pr), static_cast<int>(bp.supportsBodyTracking), f1 ? 1 : 0, f2 ? 1 : 0, f3 ? 1 : 0,
        g_supported ? "SUPPORTED - the tracker is created on the first frame with the setting on" : "NOT SUPPORTED - the elbows stay on "
        "the normal rule", RigElbowBodyTracking ? 1 : 0);
}

void BodyTrackLocate(XrTime predictedDisplayTime, XrSpace appSpace)
{
    if (!RigElbowBodyTracking) {
        if (!g_saidOff && g_supported) {
            g_saidOff = true;
            // [PROBE: BODYTRACK] gate=rig_elbow_body_tracking  site=HEADSET  log-only=no
            LOG("[BODYTRACK] OFF (rig_elbow_body_tracking=0) - supported here, nothing is located; switch it on in VR Comfort");
        }
        return;
    }
    if (!g_supported || g_createFailed || appSpace == XR_NULL_HANDLE || !predictedDisplayTime) { return; }
    if (!TryAcquireSRWLockExclusive(&g_lock)) { return; }   // another thread is locating this frame
    if (predictedDisplayTime == g_lastTime) { ReleaseSRWLockExclusive(&g_lock); return; }
    g_lastTime = predictedDisplayTime;
    if (g_tracker == XR_NULL_HANDLE) {
        XrBodyTrackerCreateInfoFB ci{ XR_TYPE_BODY_TRACKER_CREATE_INFO_FB };
        ci.bodyJointSet = XR_BODY_JOINT_SET_DEFAULT_FB;
        const XrResult cr = g_create(g_session, &ci, &g_tracker);
        if (XR_FAILED(cr)) { g_tracker = XR_NULL_HANDLE; g_createFailed = true; }
        // [PROBE: BODYTRACK] gate=rig_elbow_body_tracking  site=HEADSET  log-only=no
        LOG("[BODYTRACK] tracker create -> %d %s", static_cast<int>(cr),
            XR_FAILED(cr) ? "FAILED - the elbows stay on the normal rule this launch" : "(default joint set, 70 joints)");
        if (g_createFailed) { ReleaseSRWLockExclusive(&g_lock); return; }
    }
    XrBodyJointLocationFB j[XR_BODY_JOINT_COUNT_FB] = {};
    XrBodyJointLocationsFB locs{ XR_TYPE_BODY_JOINT_LOCATIONS_FB };
    locs.jointCount = XR_BODY_JOINT_COUNT_FB;
    locs.jointLocations = j;
    XrBodyJointsLocateInfoFB li{ XR_TYPE_BODY_JOINTS_LOCATE_INFO_FB };
    li.baseSpace = appSpace;
    li.time = predictedDisplayTime;
    const XrResult lr = g_locate(g_tracker, &li, &locs);
    Stats& st = g_st;
    ++st.locates;
    BodyArm out[2] = {};
    const bool ok = XR_SUCCEEDED(lr);
    if (!ok && !g_saidLocateFail) {
        g_saidLocateFail = true;
        // [PROBE: BODYTRACK] gate=rig_elbow_body_tracking  site=HEADSET  log-only=no
        LOG("[BODYTRACK] xrLocateBodyJointsFB FAILED %d (first time) - this frame has no body", static_cast<int>(lr));
    }
    const bool active = ok && locs.isActive == XR_TRUE && locs.confidence > 0.0f;
    if (active) { ++st.active; st.confSum += locs.confidence; } else { ++st.inactive; }
    constexpr XrSpaceLocationFlags kPos = XR_SPACE_LOCATION_POSITION_VALID_BIT;
    float firstUp[2] = { -1, -1 }, firstLo[2] = { -1, -1 }, firstSh[2] = { -1, -1 };
    for (int a = 0; a < 2 && active; ++a) {
        const bool have = (j[kJ[a][0]].locationFlags & kPos) && (j[kJ[a][1]].locationFlags & kPos) && (j[kJ[a][2]].locationFlags & kPos);
        if (!have) { ++st.noJoint[a]; g_prevHave[a] = false; continue; }
        ++st.located[a];
        float S[3], E[3], W[3];
        P3(j[kJ[a][0]], S); P3(j[kJ[a][1]], E); P3(j[kJ[a][2]], W);
        // the glitch guard: against the elbow's last DISTINCT position (a body sample repeats across display times)
        bool glitch = false;
        if (g_prevHave[a]) {
            const bool moved = E[0] != g_prevE[a][0] || E[1] != g_prevE[a][1] || E[2] != g_prevE[a][2];
            if (moved) {
                const float dt = static_cast<float>(static_cast<double>(predictedDisplayTime - g_prevEChange[a]) * 1e-9);
                glitch = bodytrack::IsGlitch(g_prevE[a], E, dt);
                g_prevEChange[a] = predictedDisplayTime;
            }
        } else {
            g_prevEChange[a] = predictedDisplayTime;
        }
        g_prevE[a][0] = E[0]; g_prevE[a][1] = E[1]; g_prevE[a][2] = E[2];
        g_prevHave[a] = true;
        float up = -1, lo = -1, perp = 0;
        float u[3];
        const bool off = bodytrack::ElbowOffset(S, E, W, u, &perp, &up, &lo);
        if (up >= 0.0f && lo >= 0.0f) {
            st.upperSum[a] += up; st.lowerSum[a] += lo; ++st.lenN[a];
            firstUp[a] = up; firstLo[a] = lo;
            if (j[kJ[a][3]].locationFlags & kPos) {
                float Sh[3]; P3(j[kJ[a][3]], Sh);
                const float d[3] = { S[0]-Sh[0], S[1]-Sh[1], S[2]-Sh[2] };
                firstSh[a] = bodytrack::Len3(d);
                st.shSum[a] += firstSh[a];
            }
        }
        if (glitch) { ++st.glitch[a]; continue; }
        if (!off) { ++st.straight[a]; continue; }
        out[a].usable = true;
        out[a].dirApp[0] = u[0]; out[a].dirApp[1] = u[1]; out[a].dirApp[2] = u[2];
        out[a].perpM = perp;
        ++st.usable[a];
    }
    if (!active) { g_prevHave[0] = g_prevHave[1] = false; }
    ++g_seq;
    for (int a = 0; a < 2; ++a) { out[a].seq = g_seq; g_arm[a] = out[a]; }
    g_have = true;
    // the lines below are written AFTER the lock is released (LogTo takes its own lock; the rig's readers must not wait on a file write)
    const bool sayFirst = !g_saidFirst;
    g_saidFirst = true;
    const ULONGLONG now = GetTickCount64();
    const bool sayLine = now - g_lastLine >= 5000;
    const Stats snap = st;
    if (sayLine) { g_lastLine = now; st = Stats{}; }
    ReleaseSRWLockExclusive(&g_lock);
    if (sayFirst) {
        // [PROBE: BODYTRACK] gate=rig_elbow_body_tracking  site=HEADSET  log-only=no (first sample, unconditional)
        LOG("[BODYTRACK] FIRST LOCATE - result %d, active %d, confidence %.2f | R: located %d usable %d, upper (arm_upper->arm_lower) "
            "%.3f m, lower (arm_lower->wrist) %.3f m, shoulder->arm_upper %.3f m | L: located %d usable %d, upper %.3f m, lower %.3f m, "
            "shoulder->arm_upper %.3f m (-1 = not located; a real upper arm and forearm are each ~0.2-0.35 m)",
            static_cast<int>(lr), active ? 1 : 0, static_cast<double>(ok ? locs.confidence : 0.0f),
            snap.located[0] ? 1 : 0, out[0].usable ? 1 : 0, static_cast<double>(firstUp[0]), static_cast<double>(firstLo[0]),
            static_cast<double>(firstSh[0]),
            snap.located[1] ? 1 : 0, out[1].usable ? 1 : 0, static_cast<double>(firstUp[1]), static_cast<double>(firstLo[1]),
            static_cast<double>(firstSh[1]));
    }
    if (sayLine) {
        auto mean = [](double s, uint64_t n) { return n ? s / static_cast<double>(n) : -1.0; };
        // [PROBE: BODYTRACK] gate=rig_elbow_body_tracking  site=HEADSET  log-only=no
        LOG("[BODYTRACK] 5 s - locates %llu, body active %llu (confidence mean %.2f), inactive %llu | R: located %llu, usable %llu, "
            "glitch %llu, straight arm %llu, joint missing %llu, upper %.3f m, lower %.3f m, shoulder->arm_upper %.3f m | L: located "
            "%llu, usable %llu, glitch %llu, straight arm %llu, joint missing %llu, upper %.3f m, lower %.3f m, shoulder->arm_upper "
            "%.3f m",
            snap.locates, snap.active, mean(snap.confSum, snap.active), snap.inactive,
            snap.located[0], snap.usable[0], snap.glitch[0], snap.straight[0], snap.noJoint[0],
            mean(snap.upperSum[0], snap.lenN[0]), mean(snap.lowerSum[0], snap.lenN[0]), mean(snap.shSum[0], snap.lenN[0]),
            snap.located[1], snap.usable[1], snap.glitch[1], snap.straight[1], snap.noJoint[1],
            mean(snap.upperSum[1], snap.lenN[1]), mean(snap.lowerSum[1], snap.lenN[1]), mean(snap.shSum[1], snap.lenN[1]));
    }
}

bool BodyTrackGetArm(int arm, BodyArm& out)
{
    if (arm < 0 || arm > 1) { return false; }
    AcquireSRWLockShared(&g_lock);
    const bool have = g_have;
    if (have) { out = g_arm[arm]; }
    ReleaseSRWLockShared(&g_lock);
    return have;
}

void BodyTrackShutdown()
{
    AcquireSRWLockExclusive(&g_lock);
    if (g_tracker != XR_NULL_HANDLE && g_destroy) { g_destroy(g_tracker); }
    g_tracker = XR_NULL_HANDLE;
    g_have = false;
    g_lastTime = 0;
    g_prevHave[0] = g_prevHave[1] = false;
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace dlvr
