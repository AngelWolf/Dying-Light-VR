// Right-shoulder holster - docs/SPEC-SHOULDER-HOLSTER-2026-09-29.md.
// Reach the right hand over the right shoulder and squeeze right grip: the weapon switch fires ON GRIP (Jay
// 2026-09-30, was on release) and that press never reaches the game as R1 (jump). Haptic pulse on zone entry and
// on the switch. The zone/press logic is holster_logic.h (unit-tested); this file does the side effects.
#include "holster.h"

#include "armfit.h"   // ArmFitRestHand - holster_hip_from_calib's resting hand
#include "controllers.h"
#include "headpose.h"
#include "holster_logic.h"
#include "holster_place_logic.h"   // placing a holster by hand (plan 2026-10-30-holster-place-by-hand)
#include "wheel_logic.h"   // the selection wheels (plan 2026-10-27-vr-selection-wheels)
#include "boltwheel.h"         // BoltWheelSetWant / BoltWheelLive - the crossbow bolt wheel's hooks (plan 2026-11-04-crossbow-bolt-wheel)
#include "boltwheel_logic.h"   // boltwheel::NoSelectNext (plan ruling B1)
#include "bownock.h"           // BowNockQuiverHold - the arrow wheel's quiver hold (2026-11-11)
#include "uipanel.h"       // uipanel::WheelFrame - the wheel as drawn (D12)
#include "rtticam.h"       // RttiClassNameOf - the selection wheels' own HUD components are never hidden (INV-642)
// buildrva.h: Steam rvas -> the Epic build's (tools/egl_allowlist.tsv); any other build refuses by name at the byte check.
#if __has_include("../common/buildrva.h")
#include "../common/buildrva.h"
#define DLVR_HH_GD(rva) ::dlvr::buildrva::Gd(rva)
#else
#define DLVR_HH_GD(rva) (rva)
#endif
#include "bodygrab.h"   // BodyGrab / BodyGrabReachM / BodyGrabHitboxMarginM - the hip yield's log line
#include "looting.h"    // LootYieldAt / LootByHand / LootReachM - the hip yield (spec 2026-10-26 §2.4)
#include "lidhand.h"    // LidHandHandInZone - the hips yield to a lid / chest item first (trace 20261119_1240)

#include <windows.h>

#include <cstdarg>
#include <cstdio>

#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)
#include "periodic_log.h"   // LOG_WINDOW - the [HOLSTERBODY] 5 s line (2026-11-08 log trim)

namespace dlvr {

extern bool ManualReload;   // manual_reload (reload.cpp) - the left hip is the ammo pouch only while it is on

// [DEP: holster_enable] required-for=the right-shoulder holster (HolsterEnable) - SPEC-SHOULDER-HOLSTER-2026-09-29
//      tree=INERT. Was DEFAULT OFF. Off: HolsterPoll returns false at its first line and the vr_grip block is unchanged.
//      HEADSET: needs the right GRIP pose and the real head pose (GetHeadPose). Needs vr_grip=1 (refuses by name).
//      Reads no bones - does NOT need the three rig master enables.
bool HolsterEnable = true;   // DEFAULT ON 2026-10-01 (Jay: Tier A ships on, docs/SHIP_DEFAULTS.md)
// [DEP: holster_center_x] [DEP: holster_center_y] [DEP: holster_center_z] required-for=the zone centre
//      (HolsterCenterX / HolsterCenterY / HolsterCenterZ), head-yaw frame, metres, +X right +Y up +Z behind.
//      tree=INERT. A GUESS until run 1's [HOLSTER] PRESS lines measure where the hand actually goes.
float HolsterCenterX = 0.15f;
float HolsterCenterY = -0.15f;
float HolsterCenterZ = 0.10f;
// [DEP: holster_radius] required-for=the zone's enter radius (HolsterRadius), exit = x1.2. tree=INERT. GUESS.
float HolsterRadius = 0.15f;
// [DEP: holster_haptic] required-for=the holster pulse amplitude (HolsterHaptic), 0 = no pulses. tree=INERT.
float HolsterHaptic = 0.6f;
// [DEP: holster_tap_ms] required-for=the weapon-switch output (HolsterTapMs): how long D-pad right is held
//      when the holster fires (on grip). Jay 2026-09-29: over half a second brings up the weapon wheel, under is a tap - so the
//      registered range stops at 450. tree=INERT (only acts with holster_enable=1).
int HolsterTapMs = 100;

// RIGHT HIP - Jay 2026-10-04: "We also want an additional holster on the RIGHT HIP, which is the one we will set
// to DPAD LEFT" (the left hip moved to D-pad down in the same change). Right grip at the right hip taps D-pad
// left (item swap) ON GRIP and that press is not R1. Same zone maths and press machine as the shoulder; a
// shoulder/right-hip pair whose exit spheres touch is REFUSED by name.
// [DEP: holster_rhip_enable] required-for=the right-hip holster (HolsterRHipEnable): right grip at the right hip
//      taps D-pad left (item swap) instead of R1. tree=INERT. With it and holster_enable both 0, HolsterPoll
//      returns false at its first line and R1 is unchanged. HEADSET (right GRIP pose + real head pose). Needs
//      vr_grip=1 (refuses by name). Reads no bones.
bool HolsterRHipEnable = true;   // DEFAULT ON 2026-10-04 (Jay asked for it for the review build; NOT yet run)
// [DEP: holster_rhip_center_x] [DEP: holster_rhip_center_y] [DEP: holster_rhip_center_z] required-for=the right
//      hip zone centre (HolsterRHipCenterX / HolsterRHipCenterY / HolsterRHipCenterZ), head-yaw frame, metres,
//      +X right +Y up +Z behind. tree=INERT. The left hip's centre mirrored in X - a GUESS until a run's
//      [HOLSTER] PRESS right hip lines measure where the hand goes.
float HolsterRHipCenterX = 0.20f;
float HolsterRHipCenterY = -0.60f;
float HolsterRHipCenterZ = 0.00f;
// [DEP: holster_rhip_radius] required-for=the right hip zone's enter radius (HolsterRHipRadius), exit = x1.2.
//      tree=INERT. Same value as the left hip.
float HolsterRHipRadius = 0.15f;
// [DEP: holster_rhip_tap_ms] required-for=the item-swap output (HolsterRHipTapMs): how long D-pad left is held
//      when the right hip fires. D-pad left = _ACTION_QUICK_SECONDARY (our Data0.pak inputs_pad.scr:94, no
//      HoldTime); the 100 ms tap is INV-311's measured item swap. tree=INERT.
int HolsterRHipTapMs = 100;
// SELECTION WHEELS (spec docs/superpowers/specs/2026-10-27-vr-selection-wheels-design.md; no switch of their own - D10, Jay: "Ride
// them"). The shoulder's switch now fires on the RELEASE of a short press (D7, Jay 2026-10-27: "Wait for release") - superseding
// 2026-09-30's "on grip" - because a press held to holster_wheel_hold_ms opens the game's weapon wheel instead.
// [DEP: holster_wheel_hold_ms] required-for=how long a holster grip is held before the game's wheel opens (HolsterWheelHoldMs);
//      shorter = today's tap at the release. Jay: "Same for all three, 3/4 second" -> after run W1 "0.5s would be better".
//      Tuning (Tier D). tree=INERT.
int HolsterWheelHoldMs = 500;
// [DEP: holster_wheel_centre_m] required-for=the centre radius ON THE WHEEL AS DRAWN (the eye -> left hand line on the HUD panel,
//      D12) inside which the selecting hand cancels (HolsterWheelCentreM; D5 "centre cancels and keeps the current one"), metres on
//      the panel. Tuning (Tier D), my default (the drawn wheel is ~0.15 m in radius at ui_hud_distance_m 0.5, scale 1.5). tree=INERT.
float HolsterWheelCentreM = 0.04f;

bool ScLeftIsBow();   // boneblend.cpp: the LEFT hand's item is a BowVis (the arrow wheel's gate)

namespace {

holster::Machine g_machine;

// Right hip: its own Machine on the same right grip as the shoulder. 0x80 = D-pad left, MEASURED (INV-311).
constexpr unsigned kDpadLeftBit = 0x80u;
holster::Machine g_rhipMachine;
holster::TapPulse g_rhipTap;
holster::RateGate g_rhipEnterGate;
holster::RateGate g_rhipExitGate;
long g_rhipTaps = 0;
long g_rhipTapPolls = 0;

// Session counters (INV-10: every call site that can produce the event counts it).
long g_polls = 0;
long g_suppressedPolls = 0;
long g_pressHolster = 0;
long g_pressNormal = 0;
long g_switches = 0;
long g_cancels = 0;
long g_noHead = 0;
long g_noGrip = 0;
long g_pulseRefused = 0;

// 10 s window: only printed when a zone event happened inside it.
ULONGLONG g_windowStart = 0;
bool g_windowHadEvent = false;

// Review M1: a grip pose flapping behind the shoulder would otherwise buzz and log EXIT+ENTER every frame.
// At most one ENTER (log + pulse) and one EXIT log per kEdgeGapMs; the lines carry how many were skipped.
constexpr unsigned long long kEdgeGapMs = 500;
holster::RateGate g_enterGate;
holster::RateGate g_exitGate;

ULONGLONG g_pressStart = 0;
bool g_firstHapticResultLogged = false;
bool g_firstHapticFailLogged = false;

// THE WEAPON-SWITCH OUTPUT - spec §4. Jay 2026-09-29: "We'll do it as the right Dpad tap for now." D-pad
// right = _ACTION_QUICK_PRIMARY (shipped inputs_pad.scr:93); Jay: tap = switch, hold (> ~0.5 s) = wheel.
// Sony bit 0x20 = D-pad right, MEASURED in the headset (INV-299: 27/27 taps switched weapon).
// This is the only call site; dlvr_FilterScePad ORs the bit while g_tap is active.
constexpr unsigned kDpadRightBit = 0x20u;
holster::TapPulse g_tap;
long g_tapPolls = 0;
long g_taps = 0;

// Hip yield (spec 2026-10-26-roomscale-physical-crouch-design §2.4, plan Task 4; Jay: "Let the body under hand win then", severed limbs
// "Same rule should apply"): the two HIP zones only - the shoulder and the temple never yield. One per hip. The press edge is the
// Machine's own test (kGripThreshold on an active squeeze); LootYieldAt is asked only on that edge and while the press is Pending.
struct HipYield {
    bool   prevDown = false;
    bool   asked = false;   // LootYieldAt answered for the current press and its line is not written yet
    float  dist = -1.0f;
    bool   limb = false;
    bool   lid = false;     // yielded to a lid / chest item under the hand (trace 20261119_1215)
    double waitS = -1.0;
};
HipYield g_rhipYield;
HipYield g_lhipYield;
long     g_hipYieldLines = 0;
constexpr long kMaxHipYieldLines = 400;   // INV-9: one line per hip press (human rate); the cap guards a flickering squeeze

// Before the hip Machine's Update: sets in.yield. COST: LootYieldAt's (looting.cpp) on a press edge and while Pending (<= 150 ms);
// otherwise one compare.
void HipYieldAsk(HipYield& y, int hand, const holster::Machine& m, holster::Input& in)
{
    const bool down = in.squeezeActive && in.squeeze >= holster::kGripThreshold;
    const bool edge = down && !y.prevDown;
    y.prevDown = down;
    if (edge) { y.asked = false; }
    if (!edge && m.state() != holster::Press::Pending) { return; }
    // The lid first (trace 20261119_1240): a hand in a lid's edge zone / an item's zone, or holding one, wins on BOTH hips - asked of
    // lidhand directly, not through loot by hand (lids stand on their own). hand 0 = LEFT here, lidhand side 1 = left.
    y.lid = LidHandHandInZone(hand == 0 ? 1 : 0);
    if (y.lid) {
        y.dist = 0.0f;
        y.limb = false;
        y.waitS = -1.0;
        in.yield = holster::Yield::Yes;
    } else {
        in.yield = LootYieldAt(hand, edge, &y.dist, &y.limb, &y.waitS);
    }
    y.asked = true;
}

// After it: one line per resolved hip-zone press that asked - yielded to the body, or the holster fired.
void HipYieldLog(HipYield& y, const char* side, const holster::Step& s)
{
    if (!y.asked) { return; }
    if (!s.yielded && !s.pressHolster) {
        if (s.pressNormal) { y.asked = false; }   // a press outside the zone: not a hip press, no line
        return;
    }
    y.asked = false;
    const long k = ++g_hipYieldLines;
    if (k > kMaxHipYieldLines) { return; }
    const float reach = (BodyGrab && BodyGrabReachM > LootReachM) ? BodyGrabReachM : LootReachM;
    // [PROBE: HIPYIELD] gate=holster_rhip_enable,holster_hip_enable  site=HEADSET  log-only=yes
    LOG("holster: [HIPYIELD] %s hip press: %s at %.3f m (reach %.3f, limb margin %.3f%s) -> %s after %.0f ms%s%s", side,
        s.yielded ? (y.lid ? "lid / chest item" : y.limb ? "severed limb" : "corpse") : "nothing", y.dist, reach, BodyGrabHitboxMarginM,
        BodyGrab ? "" : ", body_grab off",
        s.yielded ? (y.lid ? "yielded to the lid / item (hand-open lids)" : "yielded to the body")
                  : (LootByHand ? "holster fires" : "loot_by_hand off"),
        y.waitS < 0.0 ? -1.0 : y.waitS * 1000.0, k == 1 ? " | first line, logged unconditionally" : "",
        k == kMaxHipYieldLines ? " | LAST HIPYIELD line this session" : "");
}

void HolsterFireSwitch(ULONGLONG now)
{
    int ms = HolsterTapMs;
    if (ms < 20) { ms = 20; }
    if (ms > 450) { ms = 450; }
    g_tap.Start(now, static_cast<unsigned long long>(ms));
    g_tapPolls = 0;
    ++g_taps;
}

void Pulse(float amplitude, int durationMs)
{
    if (HolsterHaptic <= 0.0f) {
        return;
    }
    if (!PulseController(kRightHand, amplitude, durationMs)) {
        ++g_pulseRefused;
    }
}

}  // namespace

// ===== BODY-LOCK (docs/superpowers/specs/2026-10-20-holster-body-lock-design.md) =====
// [LOADBEARING: holster body-lock] the right shoulder and both hips hang off the body: anchor = the real neck point for all three
// (the hips were the drawn pelvis x/z until run 1 showed it turning with the look) + an offset latched once in the body frame,
// whose yaw is holster_logic.h FacingStep's (2026-11-07: a slow head average + a standing rest square-up; until then the hand
// line). Each zone feeds its
// Machine LocalOffsetYaw(VirtualHead) - the same frame and centres as today at the latch moment. Missing anything -> today's
// head-relative offset for that poll. No new key (Jay: "No New key, this will be an over-arching fix to the holsters"). Both polls
// call BodyFrameUpdate; it runs once per poll cycle. COST: a few dozen flops, one seqlock read, one ArmFitRestHand, two
// HolsterHipCentre and one exclusive SRW lock (the snapshot HolsterZoneLocal reads) per cycle.
// Review fixes 2026-10-20: the latch is taken from the level-head eye (I1); a hand at the shoulder freezes the facing (I2); which
// zones hold a hand is measured here for every zone, enabled or not (I3, M2). (I5's "a hand line past 70 deg is held" retired
// 2026-11-07: the facing is now kept within 70 deg of the head.)
// The bow's quiver, the throwables' ready zone and the swing's holster check read the same frame through HolsterZoneLocal.
namespace {

enum BodyZone { kZoneShoulder = kHolsterShoulder, kZoneRHip = kHolsterRHip, kZoneLHip = kHolsterLHip, kZoneLChest = kHolsterLChest,
                kZoneCount = 4 };

// The placement zones (holster_place_logic.h: 0 shoulder, 1 right hip, 2 left hip, 3 head, 4 left chest) -> the body zone that
// holds their latch, or -1 (the temple is head-relative, never body-locked). EVERY place -> body crossing goes through this
// (plan 2026-11-01-manual-reload-pistol Task 3: with a 4th body zone, place 3 = head is no longer body 3).
int BodyOfPlace(int placeZone)
{
    if (placeZone >= 0 && placeZone <= 2) { return placeZone; }
    if (placeZone == 4) { return kZoneLChest; }
    return -1;
}

struct BodyFrame {
    double            lastS = -1.0;
    holster::Facing   facing;
    holster::Latch    latch[kZoneCount];
    holster::Vec3     anchor[kZoneCount] = {};
    bool              anchorOk[kZoneCount] = {};
    bool              in[kZoneCount] = {};   // the hand inside the zone's exit sphere (review I3/M2: measured here, every zone)
    float             pel[3] = {};
    double            pelAtS = -1.0;
    unsigned long long model = 0;
    long              calibSeq = -2;
    bool              saidFirst = false;
    // 5 s window
    ULONGLONG         winStart = 0;
    long              nDrift = 0, nRest = 0, nFrozen = 0, nLimit = 0;   // FacingStep's reasons (2026-11-07)
    holster::FacingWhy lastWhy = holster::FacingWhy::Drift;
    long              onBody[kZoneCount] = {}, offNoLatch[kZoneCount] = {}, offNoAnchor[kZoneCount] = {};
    long              pelFresh = 0, pelHeld = 0, pelNone = 0;
    float             maxOffHeadDeg = 0.0f;
    // Review I4: the fresh pelvis minus the neck point in the facing frame, mm - how far the drawn pelvis moves under the body
    // (no longer an anchor; kept as the measurement that retired it).
    long              pnN = 0;
    float             pnMin[2] = {}, pnMax[2] = {};   // [0] = x (+right), [1] = z (+behind)
    // Neck-motion check, sampled only while the facing is steady: [0..1] neck x/z mm, [2..3] eye x/z mm (the control), [4] head
    // yaw off the facing, deg - all in the body frame.
    long              nmN = 0;
    float             nmMin[5] = {}, nmMax[5] = {};
    // 2026-11-16 (trace 20261116_1450_hip_anchor_facing_v2): the hips hang off holster_logic.h HipStep's point, kept in the
    // REFERENCE frame (appSpace shifts undone through GetAppBase); hipAnchor = its anchor in appSpace for this cycle.
    holster::HipPoint hip;
    holster::Vec3     hipAnchor{ 0.0f, 0.0f, 0.0f };
    bool              hipAnchorOk = false;
    holster::Vec3     neckNow{ 0.0f, 0.0f, 0.0f };   // this cycle's neck point (SnapPublish's anchorFromNeck)
    bool              saidHip = false, saidHipRefused = false;
    long              turnLines = 0, sitLines = 0;
    // v2.1 (trace 20261116_1545_standing_posture): standing / seated from the floor; standing lets a held turn be followed.
    holster::Posture  posture;
    bool              saidPosture = false, saidNoFloor = false;
    long              postureLines = 0, postureWinLines = 0, heldFollows = 0;
    ULONGLONG         postureWinStart = 0;
    float             eyeMin = 1e9f, eyeMax = -1e9f;
};
BodyFrame g_bf;
volatile LONG g_bfResetAsked = 0;
// 1 while a holster is being placed by hand (HolsterPlaceBegin .. the end of HolsterPlaceTick, render thread). Read by
// BodyFrameUpdate (scePad filter thread) to hold the facing, and by HolsterPlacing() from either thread.
volatile LONG g_placing = 0;

// What HolsterZoneLocal reads (other threads): written once per BodyFrameUpdate under the exclusive lock.
struct BodySnap {
    double         atS = -1.0;
    float          yaw = 0.0f;
    bool           latched[kZoneCount] = {};
    holster::Vec3  eyeFromAnchor[kZoneCount] = {};
    holster::Vec3  anchorFromNeck[kZoneCount] = {};   // anchor - neck (appSpace): 0 for the shoulder / chest, the hip point for the hips
};
SRWLOCK g_snapLock = SRWLOCK_INIT;
BodySnap g_snap;
volatile LONG g_consumer[3][2] = {};   // [caller][0 = body-locked, 1 = head-relative] answers, per 5 s window

double NowS()
{
    static LARGE_INTEGER f = {};
    if (f.QuadPart == 0) { QueryPerformanceFrequency(&f); }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

void BodyFrameClear(BodyFrame& b)
{
    holster::FacingReset(b.facing);
    for (int z = 0; z < kZoneCount; ++z) { b.latch[z] = holster::Latch{}; }
    b.hip = holster::HipPoint{};
    b.hipAnchorOk = false;
}

void SnapPublish(const BodyFrame& b, double now, bool valid)
{
    AcquireSRWLockExclusive(&g_snapLock);
    g_snap.atS = valid ? now : -1.0;
    g_snap.yaw = b.facing.yaw;
    for (int z = 0; z < kZoneCount; ++z) {
        g_snap.latched[z] = b.latch[z].have;
        g_snap.eyeFromAnchor[z] = b.latch[z].eyeFromAnchor;
        g_snap.anchorFromNeck[z] = valid ? holster::Vec3{ b.anchor[z].x - b.neckNow.x, b.anchor[z].y - b.neckNow.y,
                                                          b.anchor[z].z - b.neckNow.z }
                                         : holster::Vec3{ 0.0f, 0.0f, 0.0f };
    }
    ReleaseSRWLockExclusive(&g_snapLock);
}

// The zone's offset from the frame as it stood (body-locked when anchored and latched, else head-relative) - no counting.
holster::Vec3 ZoneLocalNoCount(const BodyFrame& b, int zone, const holster::Vec3& eye, float headYaw, const holster::Vec3& palm)
{
    if (b.anchorOk[zone] && b.latch[zone].have) {
        return holster::LocalOffsetYaw(holster::VirtualHead(b.anchor[zone], b.facing.yaw, b.latch[zone]), b.facing.yaw, palm);
    }
    return holster::LocalOffsetYaw(eye, headYaw, palm);
}

// Hysteresis like the Machines: enter inside r, leave past r * kExitScale. Review I3/M2: every zone, enabled or not, every cycle.
bool ZoneInStep(bool was, const holster::Vec3& local, const float c[3], float r)
{
    const float dx = local.x - c[0], dy = local.y - c[1], dz = local.z - c[2];
    const float d2 = dx * dx + dy * dy + dz * dz;
    const float e = was ? r * holster::kExitScale : r;
    return d2 <= e * e;
}

void BodyFrameUpdate()
{
    const double now = NowS();
    BodyFrame& b = g_bf;
    if (b.lastS >= 0.0 && now - b.lastS < 0.002) { return; }   // the other poll of this cycle already ran it
    const float dt = b.lastS < 0.0 ? 0.0f : static_cast<float>(now - b.lastS);
    b.lastS = now;

    XrPosef head{}, gr{}, gl{};
    const bool headOk = GetHeadPose(head);
    const bool rOk = GetControllerGripPose(kRightHand, gr);
    const bool lOk = GetControllerGripPose(kLeftHand, gl);
    float sqR = 0.0f, sqL = 0.0f;
    const bool sqRok = GetControllerSqueeze(kRightHand, sqR);
    const bool sqLok = GetControllerSqueeze(kLeftHand, sqL);

    // Starting over: appSpace re-centred / turned (HolsterBodyReset) or a new calibration. NOT a new player object any more
    // (2026-11-16): the facing and the hip point live in tracking space - a death or a load does not turn the real body; that trigger
    // was the D-254 pelvis anchor's left-over (tester 20261115_235734: 9 player objects, the facing restarted at the head each time).
    float rest[3];
    long seq = -1;
    ArmFitRestHand(0, rest, &seq);
    float pel[3];
    unsigned long long pModel = 0, ageMs = 0;
    const bool pelPub = HolsterPelvisTracking(pel, &pModel, &ageMs);
    if (InterlockedExchange(&g_bfResetAsked, 0) || seq != b.calibSeq) {
        BodyFrameClear(b);
        b.calibSeq = seq;
    }
    if (pelPub) { b.model = pModel; }

    // The pelvis (the 5 s line's pelvis - neck field only, since the hips moved to the neck point): fresh (<= 250 ms), else the
    // last good one for up to 1 s, else none.
    if (pelPub && ageMs <= 250) {
        b.pel[0] = pel[0]; b.pel[1] = pel[1]; b.pel[2] = pel[2];
        b.pelAtS = now - static_cast<double>(ageMs) * 0.001;
        ++b.pelFresh;
    } else if (b.pelAtS >= 0.0 && now - b.pelAtS <= 1.0) {
        ++b.pelHeld;
    } else {
        ++b.pelNone;
    }

    if (!headOk) {
        for (int z = 0; z < kZoneCount; ++z) { b.anchorOk[z] = false; b.in[z] = false; }
        SnapPublish(b, now, false);
        return;
    }
    const holster::Vec3 eye{ head.position.x, head.position.y, head.position.z };
    const XrVector3f nv = NeckOffsetM();
    const holster::Vec3 n{ nv.x, nv.y, nv.z };
    const holster::Vec3 neck = holster::NeckPoint(eye, head.orientation.x, head.orientation.y, head.orientation.z,
                                                  head.orientation.w, n);
    const float headYaw = holster::HeadYaw(head.orientation.x, head.orientation.y, head.orientation.z, head.orientation.w);

    // Anchors first (they do not depend on the facing). Every body zone hangs off the real neck point (Jay 2026-10-20, run 1 of the
    // review build: the hips "kept moving relative to the head turns" - the drawn pelvis turns with the look under
    // comfort_body_follows_head, measured 20-48 cm swings of pelvis - neck inside one 5 s window). The pelvis is still read, for
    // the 5 s line's pelvis - neck field only.
    // 2026-11-16: the hips hang off the hip point (HipStep, below) - here the previous cycle's anchor, for the in-zone test; this
    // cycle's is set after the facing, before the Machines run.
    b.neckNow = neck;
    for (int z = 0; z < kZoneCount; ++z) {
        b.anchor[z] = ((z == kZoneRHip || z == kZoneLHip) && b.hipAnchorOk) ? b.hipAnchor : neck;
        b.anchorOk[z] = true;
    }

    // Which zones hold a hand, in the frame as it stands (review I3/M2: measured here for every zone, enabled or not, not taken
    // from a Machine that may be off or may not have run this cycle).
    const holster::Vec3 l{ gl.position.x, gl.position.y, gl.position.z };
    const holster::Vec3 r{ gr.position.x, gr.position.y, gr.position.z };
    {
        const float sc[3] = { HolsterCenterX, HolsterCenterY, HolsterCenterZ };
        float rc[3], lc[3];
        HolsterHipCentre(0, rc);
        HolsterHipCentre(1, lc);
        b.in[kZoneShoulder] = rOk && ZoneInStep(b.in[kZoneShoulder], ZoneLocalNoCount(b, kZoneShoulder, eye, headYaw, r), sc,
                                                HolsterRadius);
        b.in[kZoneRHip] = rOk && ZoneInStep(b.in[kZoneRHip], ZoneLocalNoCount(b, kZoneRHip, eye, headYaw, r), rc,
                                            HolsterRHipRadius);
        b.in[kZoneLHip] = lOk && ZoneInStep(b.in[kZoneLHip], ZoneLocalNoCount(b, kZoneLHip, eye, headYaw, l), lc,
                                            HolsterHipRadius);
        const float cc[3] = { HolsterLChestCenterX, HolsterLChestCenterY, HolsterLChestCenterZ };
        b.in[kZoneLChest] = lOk && ZoneInStep(b.in[kZoneLChest], ZoneLocalNoCount(b, kZoneLChest, eye, headYaw, l), cc,
                                              HolsterLChestRadius);
    }

    // The facing (2026-11-07, Jay "Go with 6" / "Go ahead"; holster_logic.h FacingStep): a slow average of the head's yaw, squared
    // up to the hand line only while a STANDING player's hands hang at rest, kept within 70 deg of the head. Frozen while a hand is
    // at the shoulder or exactly one hand is at a hip (a zone never moves under the hand), and while a holster is being placed by
    // hand (plan 2026-10-30 Task 3 step 4, spec goal 3). A squeezed grip no longer freezes it - it only blocks the square-up.
    // The left chest counts as a left-side body zone for the freeze (a hand at the chest = a hand at the body, like the hip).
    // COST: one volatile read, a few dozen flops, one exp.
    // 2026-11-16 (trace 20261116_1450_hip_anchor_facing_v2, RUN HC1 replay): FacingStepV2 - held inside a 30 deg band, a head held
    // past it for 0.5 s followed only when BOTH hands confirm a body turn (rigid, near the body - which also lifts the zone freeze),
    // held while the head is pitched below -35 deg, no 70 deg clamp; the rest square-up and the zone / placing freezes as before.
    const bool zoneFreeze = holster::FreezeFacing(b.in[kZoneShoulder], b.in[kZoneRHip], b.in[kZoneLHip] || b.in[kZoneLChest]);
    const bool squeezed = (sqRok && sqR >= holster::kGripThreshold) || (sqLok && sqL >= holster::kGripThreshold);
    const float headPitch = holster::HeadPitch(head.orientation.x, head.orientation.y, head.orientation.z, head.orientation.w);
    // v2.1 posture (trace 20261116_1545_standing_posture): standing / seated from the eye's height above the runtime's floor.
    // COST: one Guard enter (GetEyeAboveFloor), a few compares.
    float eyeFloor = 0.0f;
    const bool floorOk = GetEyeAboveFloor(&eyeFloor);
    const bool postureChanged = holster::PostureStep(b.posture, floorOk, eyeFloor, now);
    if (floorOk) {
        if (eyeFloor < b.eyeMin) { b.eyeMin = eyeFloor; }
        if (eyeFloor > b.eyeMax) { b.eyeMax = eyeFloor; }
    }
    if (!b.saidPosture && floorOk) {
        b.saidPosture = true;
        // [PROBE: HOLSTERBODY] gate=holster_enable,holster_rhip_enable,holster_hip_enable  site=HEADSET  log-only=no
        LOG("holster: [HOLSTERBODY] POSTURE FIRST (logged unconditionally) | eye %.3f m above the floor | STANDING after %.1f s above "
            "%.2f m, SEATED after %.1f s below %.2f m | standing: a head held %.1f s past the band is followed as a body turn",
            eyeFloor, holster::kPostureHoldS, holster::kStandEyeM, holster::kPostureHoldS, holster::kSeatEyeM, holster::kStandHoldS);
    } else if (!b.saidNoFloor && !floorOk) {
        b.saidNoFloor = true;
        // [PROBE: HOLSTERBODY] gate=holster_enable,holster_rhip_enable,holster_hip_enable  site=HEADSET  log-only=no
        LOG("holster: [HOLSTERBODY] POSTURE: NO FLOOR (yet) - GetEyeAboveFloor has no STAGE location: SEATED (the hands-only facing) "
            "until it has (logged once)");
    }
    if (postureChanged && b.postureLines < 400) {
        ++b.postureLines;
        // [PROBE: HOLSTERBODY] gate=holster_enable,holster_rhip_enable,holster_hip_enable  site=HEADSET  log-only=no
        LOG("holster: [HOLSTERBODY] POSTURE -> %s #%ld | eye %.3f m above the floor%s", b.posture.standing ? "STANDING" : "SEATED",
            b.postureLines, floorOk ? eyeFloor : -1.0f, floorOk ? "" : " (no floor)");
    }
    {
        const ULONGLONG pw = GetTickCount64();
        if (b.postureWinStart == 0) { b.postureWinStart = pw; }
        if (pw - b.postureWinStart >= 10000 && b.postureWinLines < 400) {
            b.postureWinStart = pw;   // stamped before the LOG (INV-9)
            ++b.postureWinLines;
            // [PROBE: HOLSTERBODY] gate=holster_enable,holster_rhip_enable,holster_hip_enable  site=HEADSET  log-only=no
            LOG("holster: [HOLSTERBODY] POSTURE 10 s | %s | eye above the floor %.3f..%.3f m%s | held-turn follows %ld (run total)",
                b.posture.standing ? "STANDING" : "SEATED", b.eyeMin < 1e8f ? b.eyeMin : -1.0f, b.eyeMax > -1e8f ? b.eyeMax : -1.0f,
                floorOk ? "" : " (no floor now)", b.heldFollows);
            b.eyeMin = 1e9f;
            b.eyeMax = -1e9f;
        }
    }
    const bool wasFollowing = b.facing.following;
    b.lastWhy = holster::FacingStepV2(b.facing, neck, l, r, lOk, rOk, headYaw, headPitch, zoneFreeze, g_placing != 0, squeezed, now,
                                      dt, b.posture.standing);
    if (b.facing.following && !wasFollowing) {
        const bool byHands = b.facing.lastConfirmed;
        if (!byHands) { ++b.heldFollows; }
        if (b.turnLines < 200) {
            ++b.turnLines;
            // [PROBE: HOLSTERBODY] gate=holster_enable,holster_rhip_enable,holster_hip_enable  site=HEADSET  log-only=no
            LOG("holster: [HOLSTERBODY] BODY TURN #%ld - %s: the facing follows from %+.1f deg to the head at %+.1f deg | hands from the "
                "neck L %.2f m R %.2f m | %s%s",
                b.turnLines, byHands ? "both hands turned with the head" : "STANDING, the head held past the band",
                b.facing.yaw * 57.2957795f, headYaw * 57.2957795f,
                std::sqrt((l.x - neck.x) * (l.x - neck.x) + (l.z - neck.z) * (l.z - neck.z)),
                std::sqrt((r.x - neck.x) * (r.x - neck.x) + (r.z - neck.z) * (r.z - neck.z)),
                b.posture.standing ? "standing" : "seated", b.turnLines == 1 ? " | first line, logged unconditionally" : "");
        }
    }
    switch (b.lastWhy) {
        case holster::FacingWhy::Drift:  ++b.nDrift; break;
        case holster::FacingWhy::Rest:   ++b.nRest; break;
        case holster::FacingWhy::Frozen: ++b.nFrozen; break;
        case holster::FacingWhy::Limit:  ++b.nLimit; break;
    }
    const float offHead = std::fabs(holster::WrapPi(b.facing.yaw - headYaw)) * 57.2957795f;
    if (offHead > b.maxOffHeadDeg) { b.maxOffHeadDeg = offHead; }

    // THE HIP POINT (2026-11-16, trace 20261116_1450_hip_anchor_facing_v2): HipStep runs in the REFERENCE frame - roomscale shifts
    // appSpace itself every step, so the neck and the facing go through the appSpace base (ToWorldYaw by the base yaw + the base
    // position) and the anchor comes back the inverse way. No base = the hips stay on the neck point (today's), said once.
    // COST: one Guard enter (GetAppBase, as GetHeadPose), a few dozen flops, one exp, one sqrt.
    {
        float base[3] = {};
        float baseYawDeg = 0.0f;
        if (GetAppBase(base, &baseYawDeg)) {
            const float by = baseYawDeg * 0.0174532925f;
            const holster::Vec3 nw = holster::ToWorldYaw(neck, by);
            const holster::Vec3 neckRef{ nw.x + base[0], nw.y + base[1], nw.z + base[2] };
            const bool hadHip = b.hip.have;   // SAT DOWN only for a hip point that existed before this step (a restart is not a sit)
            const float refBefore = hadHip ? b.hip.ref : 0.0f;
            const holster::Vec3 aRef = holster::HipStep(b.hip, neckRef, holster::WrapPi(b.facing.yaw + by), dt);
            b.hipAnchor = holster::LocalOffsetYaw({ base[0], base[1], base[2] }, by, aRef);
            b.hipAnchorOk = true;
            b.anchor[kZoneRHip] = b.hipAnchor;
            b.anchor[kZoneLHip] = b.hipAnchor;
            if (!b.saidHip) {
                b.saidHip = true;
                // [PROBE: HOLSTERBODY] gate=holster_enable,holster_rhip_enable,holster_hip_enable  site=HEADSET  log-only=no
                LOG("holster: [HOLSTERBODY] HIP POINT FIRST (logged unconditionally) | appSpace base (%.3f, %.3f, %.3f) yaw %.1f deg | "
                    "neck (reference) (%.3f, %.3f, %.3f) | hip anchor - neck (%.3f, %.3f, %.3f) m (0 at the start = today's hips) | "
                    "the hips hold through a lean, drop + move back in a squat, re-centre on sitting, follow a step",
                    base[0], base[1], base[2], baseYawDeg, neckRef.x, neckRef.y, neckRef.z, b.hipAnchor.x - neck.x,
                    b.hipAnchor.y - neck.y, b.hipAnchor.z - neck.z);
            }
            if (hadHip && b.hip.ref < refBefore - 0.01f && b.sitLines < 200) {
                ++b.sitLines;
                // [PROBE: HOLSTERBODY] gate=holster_enable,holster_rhip_enable,holster_hip_enable  site=HEADSET  log-only=no
                LOG("holster: [HOLSTERBODY] SAT DOWN #%ld - the neck dropped %.2f m with it behind the body axis: the hips' standing "
                    "height is now this one (%.3f m, reference frame)%s",
                    b.sitLines, refBefore - b.hip.ref, b.hip.ref, b.sitLines == 1 ? " | first line, logged unconditionally" : "");
            }
        } else if (!b.saidHipRefused) {
            b.saidHipRefused = true;
            // [PROBE: HOLSTERBODY] gate=holster_enable,holster_rhip_enable,holster_hip_enable  site=HEADSET  log-only=no
            LOG("holster: [HOLSTERBODY] HIP POINT REFUSED - GetAppBase has no session yet: the hips hang off the neck point (today's) "
                "until it has (logged once)");
        }
    }

    // Review I4: how far the fresh pelvis moves from the neck point in the body frame (x right, z behind), mm.
    if (pelPub && ageMs <= 250) {
        const holster::Vec3 pn = holster::LocalOffsetYaw(neck, b.facing.yaw, { b.pel[0], b.pel[1], b.pel[2] });
        const float v[2] = { pn.x * 1000.0f, pn.z * 1000.0f };
        for (int k = 0; k < 2; ++k) {
            if (b.pnN == 0 || v[k] < b.pnMin[k]) { b.pnMin[k] = v[k]; }
            if (b.pnN == 0 || v[k] > b.pnMax[k]) { b.pnMax[k] = v[k]; }
        }
        ++b.pnN;
    }

    // Neck-motion check (Jay 2026-10-20, with the hips moving to the neck point): while the facing holds steady, how far the neck
    // point and - as the control - the eye move in the body frame, against how far the head turns. A neck that stays put while the
    // eye swings says head_neck_pivot_* matches the real pivot; a neck that swings with the eye says it does not.
    if (holster::FacingSteady(b.facing, now)) {
        const holster::Vec3 zero{ 0.0f, 0.0f, 0.0f };
        const holster::Vec3 nk = holster::LocalOffsetYaw(zero, b.facing.yaw, neck);
        const holster::Vec3 ey = holster::LocalOffsetYaw(zero, b.facing.yaw, eye);
        const float v[5] = { nk.x * 1000.0f, nk.z * 1000.0f, ey.x * 1000.0f, ey.z * 1000.0f,
                             holster::WrapPi(headYaw - b.facing.yaw) * 57.2957795f };
        for (int k = 0; k < 5; ++k) {
            if (b.nmN == 0 || v[k] < b.nmMin[k]) { b.nmMin[k] = v[k]; }
            if (b.nmN == 0 || v[k] > b.nmMax[k]) { b.nmMax[k] = v[k]; }
        }
        ++b.nmN;
    }

    // The one-time latch per zone, from the LEVEL-head eye (review I1: a head pitched at the latch moment bakes in nothing).
    const holster::Vec3 levelEye = holster::LevelEye(neck, headYaw, n);
    static const char* const kName[kZoneCount] = { "right shoulder", "right hip", "left hip", "left chest" };
    for (int z = 0; z < kZoneCount; ++z) {
        // 2026-11-16: the latch is eye-from-NECK for every zone (it was b.anchor[z] = the neck for every zone): a latch taken during a
        // lean can never bake the lean into a hip; the hip point replaces the neck only when the latch is used.
        if (b.anchorOk[z] && holster::LatchTry(b.latch[z], b.facing, now, headYaw, levelEye, neck)) {
            // [PROBE: HOLSTERBODY] gate=holster_enable,holster_rhip_enable,holster_hip_enable  site=HEADSET  log-only=no
            LOG("holster: [HOLSTERBODY] LATCHED %s - level-head eye from its anchor (%.3f, %.3f, %.3f) m in the body frame | facing %+.1f deg "
                "(from the hands), head %+.1f deg | anchor = %s",
                kName[z], b.latch[z].eyeFromAnchor.x, b.latch[z].eyeFromAnchor.y, b.latch[z].eyeFromAnchor.z,
                b.facing.yaw * 57.2957795f, headYaw * 57.2957795f, "the neck point");
        }
    }
    SnapPublish(b, now, true);

    if (!b.saidFirst) {
        b.saidFirst = true;
        // [PROBE: HOLSTERBODY] gate=holster_enable,holster_rhip_enable,holster_hip_enable  site=HEADSET  log-only=no
        LOG("holster: [HOLSTERBODY] FIRST (logged unconditionally) | n (%.1f, %.1f, %.1f) mm | pelvis %s | facing v2 starts at the "
            "head yaw %+.1f deg: held inside %.0f deg of the head, follows a head held past it for %.1f s only when both hands turn "
            "with it, held while the head is pitched below -%.0f deg (squared up to the hand line over %.1f s only for hands at rest "
            "%.2f m below the neck) | the hips hang off the hip point | the temple stays head-relative",
            nv.x * 1000.0f, nv.y * 1000.0f, nv.z * 1000.0f, pelPub ? "published" : "NOT published yet", headYaw * 57.2957795f,
            holster::kBandR * 57.2957795f, holster::kBandHoldS, holster::kPitchFreezeR * 57.2957795f, holster::kRestTauS,
            holster::kRestDropM);
    }
    const ULONGLONG nowMs = GetTickCount64();
    if (b.winStart == 0) { b.winStart = nowMs; }
    if (nowMs - b.winStart >= 5000) {
        b.winStart = nowMs;   // stamped before the LOG, never behind a gate (INV-9)
        // [PROBE: HOLSTERBODY] gate=holster_enable,holster_rhip_enable,holster_hip_enable  site=HEADSET  log-only=no
        long cons[3][2];
        for (int c = 0; c < 3; ++c) {
            for (int k = 0; k < 2; ++k) { cons[c][k] = InterlockedExchange(&g_consumer[c][k], 0); }
        }
        LOG_PERIODIC("holster: [HOLSTERBODY] 5 s | facing drift %ld rest square-up %ld frozen %ld limit-pulled %ld, max %.1f deg off the "
            "head, now %+.1f (%s) | on body / no latch / no anchor: shoulder %ld/%ld/%ld, right hip %ld/%ld/%ld, left hip "
            "%ld/%ld/%ld | pelvis fresh %ld held %ld none %ld | latched %d%d%d | in zone now %d%d%d | pelvis - neck, body frame, "
            "n %ld: x %+.0f..%+.0f mm, z %+.0f..%+.0f mm | others body/head: bow %ld/%ld, throwable %ld/%ld, swing %ld/%ld | "
            "NECK MOTION, facing steady, body frame, n %ld: neck x %+.0f..%+.0f z %+.0f..%+.0f mm, eye x %+.0f..%+.0f z "
            "%+.0f..%+.0f mm, head yaw off the facing %+.1f..%+.1f deg",
            b.nDrift, b.nRest, b.nFrozen, b.nLimit, b.maxOffHeadDeg, b.facing.yaw * 57.2957795f,
            b.lastWhy == holster::FacingWhy::Rest ? "rest" : b.lastWhy == holster::FacingWhy::Frozen ? "frozen" :
            b.lastWhy == holster::FacingWhy::Limit ? "limit" : "drift", b.onBody[0], b.offNoLatch[0], b.offNoAnchor[0], b.onBody[1], b.offNoLatch[1],
            b.offNoAnchor[1], b.onBody[2], b.offNoLatch[2], b.offNoAnchor[2], b.pelFresh, b.pelHeld, b.pelNone,
            b.latch[0].have ? 1 : 0, b.latch[1].have ? 1 : 0, b.latch[2].have ? 1 : 0, b.in[0] ? 1 : 0, b.in[1] ? 1 : 0,
            b.in[2] ? 1 : 0, b.pnN, b.pnMin[0], b.pnMax[0], b.pnMin[1], b.pnMax[1], cons[0][0], cons[0][1], cons[1][0],
            cons[1][1], cons[2][0], cons[2][1], b.nmN, b.nmMin[0], b.nmMax[0], b.nmMin[1], b.nmMax[1], b.nmMin[2], b.nmMax[2],
            b.nmMin[3], b.nmMax[3], b.nmMin[4], b.nmMax[4]);
        b.nDrift = b.nRest = b.nFrozen = b.nLimit = 0;
        b.maxOffHeadDeg = 0.0f;
        b.pelFresh = b.pelHeld = b.pelNone = 0;
        b.pnN = 0;
        b.nmN = 0;
        for (int z = 0; z < kZoneCount; ++z) { b.onBody[z] = b.offNoLatch[z] = b.offNoAnchor[z] = 0; }
    }
}

// The zone's offset for its Machine: body-locked when its anchor and latch are ready, else `today` (head-relative) untouched.
void BodyLocal(int zone, const XrPosef& grip, holster::Vec3& local)
{
    BodyFrame& b = g_bf;
    if (!b.anchorOk[zone]) { ++b.offNoAnchor[zone]; return; }
    if (!b.latch[zone].have) { ++b.offNoLatch[zone]; return; }
    ++b.onBody[zone];
    const holster::Vec3 v = holster::VirtualHead(b.anchor[zone], b.facing.yaw, b.latch[zone]);
    local = holster::LocalOffsetYaw(v, b.facing.yaw, { grip.position.x, grip.position.y, grip.position.z });
}

// [HOLSTERPLACE] CHECK (Task 3 review I1): the independent test of a placement - the POLL's own hand-to-centre distance for a
// just-stored zone, from the local its Machine uses (BodyLocal / the head LocalOffset) and the centre it was given. Hand-off:
// HolsterPlaceTick (render thread) sets g_placeCheck[zone] = 2 at STORED; each poll that runs that zone (scePad thread)
// decrements it and the one that takes it to 0 logs ONE line - the first poll after STORED is skipped because it may have read
// the centre floats before the store. A zone that is off, or a poll without both poses, does not count down. Idle COST: one
// volatile read per zone per poll.
// `zone` = the PLACEMENT zone (holster_place_logic.h 0..4).
volatile LONG g_placeCheck[5] = {};
long          g_placeCheckLines = 0;
void PlaceCheckPoll(int zone, const holster::Vec3& local, const holster::Vec3& centre)
{
    if (g_placeCheck[zone] == 0) { return; }
    if (InterlockedDecrement(&g_placeCheck[zone]) > 0) { return; }
    InterlockedExchange(&g_placeCheck[zone], 0);   // never below 0 (a racing re-store sets it back to 2)
    static const char* const kName[5] = { "right shoulder", "right hip", "left hip", "head (temple)", "left chest" };
    const long k = ++g_placeCheckLines;
    if (k > 400) { return; }   // INV-9 cap; human rate (one per placement)
    const float dx = local.x - centre.x, dy = local.y - centre.y, dz = local.z - centre.z;
    const int bz = BodyOfPlace(zone);
    const bool body = bz >= 0 && g_bf.anchorOk[bz] && g_bf.latch[bz].have;   // BodyLocal's own test
    // [PROBE: HOLSTERPLACE] gate=holster_enable,holster_rhip_enable,holster_hip_enable,holster_head_enable  site=HEADSET  log-only=no
    LOG("holster: [HOLSTERPLACE] CHECK %s - the poll's own hand-to-centre distance %.1f mm, frame %s | poll local (%.3f, %.3f, "
        "%.3f) centre (%.3f, %.3f, %.3f) | 2nd poll after STORED%s",
        kName[zone], std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0f, zone == 3 ? "head (the temple)" : (body ? "body" : "head"),
        local.x, local.y, local.z, centre.x, centre.y, centre.z, k == 1 ? " | first CHECK line, logged unconditionally" : "");
}

// ===== SELECTION WHEELS (plan 2026-10-27-vr-selection-wheels Task 3) =====
// THE GAME SIDE (INV-640): the wheel opens on its show button (D-pad right = QUICK_PRIMARY), is visible after 0.2 s held, and
// commits the picked slot when that button is RELEASED; its stick cursor is sticky (centring never clears a pick). So: the button
// goes down kWheelLeadMs before HolsterWheelHoldMs (the game's 0.2 s lands at the hold), the right stick is steered at full
// deflection toward the slot the hand points at - or, at the centre, toward the slot the player already has (D11) - and after the
// grip lets go the steer is held kWheelConfirmMs more before the button is released, so the last pick registers.
constexpr unsigned  kWheelLeadMs = 200;          // the game's own visibility time (INV-640)
constexpr ULONGLONG kWheelConfirmMs = 150;       // my call: the cursor's rate per event is not established (INV-640 NOT established)
constexpr ULONGLONG kWheelCheckMs = 400;         // after the button is up: read the game's current slot once, for the AFTER line
constexpr long      kWheelMaxLines = 400;        // INV-9

struct WheelDrive {
    const char* name;   // for the [WHEEL] lines
    int which;          // HolsterWheelCurrentSlot's index
    wheel::HoldMachine m;
    wheel::SlotPicker pick;
    bool confirming = false;
    ULONGLONG confirmUntil = 0;
    wheel::Slot target = wheel::Slot::None;
    ULONGLONG openAt = 0;
    bool checkPending = false;
    ULONGLONG checkAt = 0;
    int expect = -1;
    int lastCur = -1;
    long opens = 0, taps = 0, gameTaps = 0, confirms = 0, centreCancels = 0, aborts = 0;
    bool gameSeen = false;      // the game's own wheel was active (flags bit 0) at some poll of this opening (WheelGameGate)
    ULONGLONG gameSeenAt = 0;
    long heldBack = 0;          // polls of this opening the stick was NOT taken because the game's wheel was not showing
    long offWheel = 0;          // polls of this opening the hand pointed off the drawn wheel (the pick left alone; trace 20261116_1328)
    Hand hand = kLeftHand;      // the hand over the wheel (D12: the left; the crossbow bolt wheel: the right - Jay "Right hand")
};
WheelDrive g_wWeapons{ "weapons", 0 };
WheelDrive g_wThrow{ "throwables", 1 };   // the right hip, D-pad left = QUICK_SECONDARY (plan Task 4)
// The arrow wheel (plan Task 5, Jay 2026-10-27: "C for now, but make it still selectable with the left hand like the others"): the
// R2-at-the-quiver opener is PARKED (INV-641: no spare input; X carries interact / reload / swap / Survivor Sense on foot). The
// player opens the game's own arrow wheel with the REAL X held, the bow out; the layer only steers it from the left hand and holds X
// kWheelConfirmMs past the release. No tap of ours - a short X press is the game's own.
WheelDrive g_wArrows{ "arrows", 2 };
// The crossbow bolt wheel (plan 2026-11-04-crossbow-bolt-wheel Task 3; Jay: opener "1" = hold the left grip at the bolt holster,
// "Right hand", R2 "Hold off"): opened by a hold that STARTS with a take press in the bolt holster zone (ruling B4), the crossbow take
// live; no X - its show action is held by boltwheel.cpp's hooks (BoltWheelSetWant), never by a pad bit. Steered by the RIGHT hand.
WheelDrive g_wBolts = [] { WheelDrive w{ "bolts", 3 }; w.hand = kRightHand; return w; }();
boltwheel::NoSelectNext g_boltNsn;   // ruling B1: the show action stays down >= 250 ms once pressed (no "select next")
volatile LONG g_boltHold = 0;        // the left poll's "bolt holster hold is down" (B4), read by the right poll
volatile LONG g_boltTakeLive = 0;    // the left poll's copy of "the crossbow take is live" (the right poll aborts an open wheel on 0)
volatile LONG g_boltWheelOpen = 0;   // the right poll's "the bolt wheel is open / confirming" (the left poll's hold may then leave the zone)
bool g_boltHoldArmed = false;        // left poll only: a take press started the current grip
volatile LONG g_boltHoldsR2 = 0;     // HolsterBoltWheelHoldsR2 (dlvr_FilterScePad)
// Right poll only (review 2026-11-05): where the 250 ms NoSelectNext tail steers - the confirm's target if the hold ended in a confirm,
// else the current slot (an abort / menu never steers to a slot the hand only hovered).
wheel::Slot g_boltTailTarget = wheel::Slot::None;
// The bow's arrow wheel from the right-shoulder quiver (2026-11-11, Jay: "We need to have the arrow selection on the right shoulders
// quiver for the regular bow, same as we do the crossbow" -> "Yes."): the bolt wheel's machine on HudArrowsSelection. Opened by the
// quiver hold (BowNockQuiverHold: RT pressed at the shoulder, kept there holster_wheel_hold_ms), the bow out; no X - its show action
// 288 is held by boltwheel.cpp's hooks (ArrowWheelSetWant). Steered by the LEFT hand (the X-held arrow wheel's hand). The game's
// current slot / wheel component are the X wheel's (index 2). No R2 hold-off: with the bow the game reads R2 = 0 unless the string was grabbed (ovr2xr.cpp
// dlvr_FilterScePad, BowDrawPadByte), and the right hand is at the shoulder, not on the string.
WheelDrive g_wArrowsQ{ "arrows (quiver)", 2 };
boltwheel::NoSelectNext g_arrowNsn;        // ruling B1, as the bolt wheel's
wheel::Slot g_arrowTailTarget = wheel::Slot::None;
volatile LONG g_arrowQWheelOpen = 0;       // HolsterArrowWheelOpen

// Review 2026-11-05 finding 3: every early return of the left poll ends the bolt holster hold and publishes the take's liveness, so a
// put-away while that poll returns early (every left zone off / vr_grip=0 / zones overlapping) cannot leave the hold - and with it the
// forced show action and the R2 withhold - stuck on. COST: two interlocked stores.
void BoltHoldOff(bool takeLive)
{
    g_boltHoldArmed = false;
    InterlockedExchange(&g_boltHold, 0);
    InterlockedExchange(&g_boltTakeLive, takeLive ? 1 : 0);
}
volatile LONG g_wheelHoldsX = 0;   // dlvr_FilterScePad ORs Square (X, 0x8000) while set
volatile LONG g_wheelShowing = 0;  // uipanel's RenderHud shows only the wheel while set (Task 6, D13)
long g_wheelLines = 0;
bool g_wheelSaidFirst = false;
// The stick the wheel owns, for dlvr_FilterScePad (another call on the same pad thread, but read through volatiles to stay safe).
volatile LONG g_wheelStickOn = 0;
volatile LONG g_wheelStickSlot = 0;   // static_cast<LONG>(wheel::Slot)
// THE STICK ONLY WHILE THE GAME SHOWS ITS WHEEL (Jay 2026-10-28, run P2: holding X for the arrow wheel by a trap - "It took priority
// on the trap activation, so didn't open the arrow wheel till after I activated the trap" - and the character spun: our side was
// open, the game's wheel was not, and the full stick deflection we wrote is the turn). The game's own component per drive (0
// HudPrimaryWeaponSelection, 1 HudSecondaryWeaponSelection, 2 HudArrowsSelection, 3 HudBoltSelection) and its HUD registry id,
// published by HolsterHudHideTick's HIDE scan (game thread); read on the pad thread by WheelGameGate.
volatile LONG64 g_gameWheelComp[4] = {};
volatile LONG   g_gameWheelId[4] = { -1, -1, -1, -1 };
int GameWheelActive(int which);   // 1 active, 0 not (or not resolved yet), 2 the HUD hide is refused - the old behaviour (below)

void WheelLine(const char* fmt, ...)
{
    if (g_wheelLines >= kWheelMaxLines) { return; }
    ++g_wheelLines;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    // [PROBE: WHEEL] gate=holster_enable,holster_rhip_enable  site=HEADSET  log-only=no
    LOG("holster: [WHEEL] %s%s", buf, g_wheelLines == kWheelMaxLines ? "  (line cap reached - counted only from here)" : "");
}

int SlotIndex(wheel::Slot s)
{
    switch (s) {
    case wheel::Slot::Up: return 0;
    case wheel::Slot::Right: return 1;
    case wheel::Slot::Down: return 2;
    case wheel::Slot::Left: return 3;
    case wheel::Slot::None: break;
    }
    return -1;
}

// D12 (Jay, after run W1: "hand over the wheel as I see it", superseding the body centre): where the line from the eye through the
// steering hand (the LEFT; the RIGHT for the crossbow bolt wheel) crosses the wheel as drawn on the HUD panel (uipanel::WheelFrame), as
// an offset from the wheel's centre along the panel's right / up, metres on the panel. Returns 1 = ON the wheel as drawn; 0 = OFF it
// (the line misses the drawn quad: parallel to the panel, pointing away, or crossing outside its edges - Jay 2026-11-16 "Keep last
// pick": the caller then leaves the pick alone; before this, a low hand's near-parallel line crossed metres away and picked "Down",
// tester logs 20261115_235734, trace 20261116_1328_wheel_offwheel); -1 = LOST (no panel shown lately, no head or grip pose).
// COST: one head + one grip pose read, one shared lock, a dozen flops, per poll while a wheel is open.
int WheelHandOffset(Hand hand, float& dx, float& dy)
{
    float c[3], rgt[3], up[3], n[3], ext[4];
    XrPosef head{}, lg{};
    if (!uipanel::WheelFrame(c, rgt, up, n, ext) || !GetHeadPose(head) || !GetControllerGripPose(hand, lg)) { return -1; }
    const float e[3] = { head.position.x, head.position.y, head.position.z };
    const float d[3] = { lg.position.x - e[0], lg.position.y - e[1], lg.position.z - e[2] };
    const float dn = d[0] * n[0] + d[1] * n[1] + d[2] * n[2];   // < 0: toward the panel (its normal faces the viewer)
    if (dn > -1e-4f) { return 0; }
    const float t = ((c[0] - e[0]) * n[0] + (c[1] - e[1]) * n[1] + (c[2] - e[2]) * n[2]) / dn;
    if (t <= 0.0f) { return 0; }
    float q[3];
    for (int i = 0; i < 3; ++i) { q[i] = e[i] + t * d[i] - c[i]; }
    dx = q[0] * rgt[0] + q[1] * rgt[1] + q[2] * rgt[2];
    dy = q[0] * up[0] + q[1] * up[1] + q[2] * up[2];
    if (!(dx >= ext[0] && dx <= ext[1] && dy >= ext[2] && dy <= ext[3])) { return 0; }   // NaN fails too -> off
    return 1;
}

// One wheel, once per poll. held = the press that opens it is down. Returns true while its show button must be held; tapOut = send
// the old short tap now (a release before the lead).
bool WheelDriveStep(WheelDrive& w, bool held, ULONGLONG now, bool menu, bool& tapOut, bool& steerOut, bool playerButton = false)
{
    tapOut = false;
    steerOut = false;
    if (!g_wheelSaidFirst) {
        g_wheelSaidFirst = true;
        WheelLine("FIRST (logged unconditionally): hold %d ms (button down %u ms before it), centre radius %.3f m on the wheel as "
                  "drawn (the eye -> left hand line on the HUD panel, D12), confirm steer %llu ms | current slots (0 up 1 right 2 "
                  "down 3 left, -1 unread): weapons %d throwables %d arrows %d bolts %d",
                  HolsterWheelHoldMs, kWheelLeadMs, HolsterWheelCentreM,
                  static_cast<unsigned long long>(kWheelConfirmMs), HolsterWheelCurrentSlot(0), HolsterWheelCurrentSlot(1),
                  HolsterWheelCurrentSlot(2), HolsterWheelCurrentSlot(3));
    }
    const char* abortWhy = nullptr;
    if (menu && (w.m.ButtonDown() || w.confirming)) {
        w.m.Abort();
        abortWhy = "an in-game menu opened";
    }
    // playerButton: the player presses the show button themselves (the arrow wheel's real X) - the game's own 0.2 s is the hold,
    // the button is "down" from the first poll, and a short press is the game's, never a tap of ours.
    const unsigned holdMs = playerButton ? kWheelLeadMs
                                         : static_cast<unsigned>(HolsterWheelHoldMs < 300 ? 300 : (HolsterWheelHoldMs > 2000 ? 2000 : HolsterWheelHoldMs));
    const wheel::Out o = w.m.Update(held, now, holdMs, kWheelLeadMs);
    const int cur = HolsterWheelCurrentSlot(w.which);
    switch (o) {
    case wheel::Out::Tap:
        if (playerButton) { break; }   // a one-poll X press: the game's own, nothing of ours
        tapOut = true;
        ++w.taps;
        WheelLine("TAP %s #%ld - released before %u ms: the short tap, as before", w.name, w.taps, holdMs - kWheelLeadMs);
        break;
    case wheel::Out::TapByGame:
        if (playerButton) { break; }   // a short X press: the game's own
        ++w.gameTaps;
        // review 2026-11-05: the bolt wheel's show action goes down only at OPEN - nothing reached the game; the quiver's arrow wheel
        // is driven the same way (2026-11-11).
        if (&w == &g_wBolts || &w == &g_wArrowsQ) {
            WheelLine("TAP %s #%ld - released between %u and %u ms (before OPEN): no open event, nothing sent to the game", w.name,
                      w.gameTaps, holdMs - kWheelLeadMs, holdMs);
            break;
        }
        WheelLine("TAP %s #%ld BY THE GAME - released between %u and %u ms: the button was down < %u ms, the game's own 'select "
                  "next'", w.name, w.gameTaps, holdMs - kWheelLeadMs, holdMs, kWheelLeadMs);
        break;
    case wheel::Out::Open:
        ++w.opens;
        w.openAt = now;
        w.pick.Reset();
        w.offWheel = 0;
        w.lastCur = cur;
        w.gameSeen = false;
        w.gameSeenAt = 0;
        w.heldBack = 0;
        WheelLine("OPEN %s #%ld after %u ms | current slot %d", w.name, w.opens, holdMs, cur);
        // Place-by-hand (spec 2.6): no poll pulse while a placement runs - the STORED buzz is the only one. COST: one volatile read.
        if (g_placing == 0) { Pulse(HolsterHaptic, 40); }
        break;
    case wheel::Out::Confirm: {
        const wheel::Slot hand = w.pick.Committed();
        w.target = wheel::TargetSlot(hand, cur);
        w.confirming = true;
        w.confirmUntil = now + kWheelConfirmMs;
        w.expect = SlotIndex(w.target);
        if (hand == wheel::Slot::None) { ++w.centreCancels; } else { ++w.confirms; }
        WheelLine("CLOSE %s: %s | open %llu ms | steering to %s (index %d) for %llu ms, then the button goes up | off the wheel %ld "
                  "poll(s)", w.name,
                  hand == wheel::Slot::None ? "CANCEL (hand at the centre or pose lost) - back to the current slot" : "CONFIRM",
                  static_cast<unsigned long long>(now - w.openAt), wheel::SlotName(w.target), w.expect,
                  static_cast<unsigned long long>(kWheelConfirmMs), w.offWheel);
        break;
    }
    case wheel::Out::Cancel:
        ++w.aborts;
        w.confirming = false;
        WheelLine("CLOSE %s: ABORTED (%s) - button up and stick released at once", w.name, abortWhy ? abortWhy : "aborted");
        break;
    default:
        break;
    }
    bool steer = false;
    if (w.m.Open()) {
        float dx = 0.0f, dy = 0.0f;
        const int where = WheelHandOffset(w.hand, dx, dy);
        const bool ok = where >= 0;
        const wheel::Slot before = w.pick.Committed();
        // OFF the drawn wheel (where 0): the pick is left alone (Jay "Keep last pick"); ON: as before; LOST: None at once, as before.
        if (where == 0) { ++w.offWheel; }
        const wheel::Slot now2 = where == 0 ? before : w.pick.Update(ok, dx, dy, HolsterWheelCentreM);
        if (now2 != before) {
            WheelLine("SLOT %s -> %s (hand %+.3f, %+.3f m%s) | current slot %d", w.name, wheel::SlotName(now2), dx, dy,
                      ok ? "" : ", POSE LOST", cur);
        }
        w.target = wheel::TargetSlot(now2, cur);
        steer = w.target != wheel::Slot::None;
    } else if (w.confirming) {
        if (now >= w.confirmUntil) {
            w.confirming = false;
            w.checkPending = true;
            w.checkAt = now + kWheelCheckMs;
        } else {
            steer = w.target != wheel::Slot::None;
        }
    }
    if (w.checkPending && now >= w.checkAt) {
        w.checkPending = false;
        char seen[160];
        if (w.gameSeen) {
            _snprintf_s(seen, sizeof(seen), _TRUNCATE, "game wheel shown %llu ms after OPEN, stick held back %ld poll(s) before",
                        static_cast<unsigned long long>(w.gameSeenAt - w.openAt), w.heldBack);
        } else {
            _snprintf_s(seen, sizeof(seen), _TRUNCATE, "game wheel NEVER shown - the button went to something else (a trap?); the "
                        "stick was left alone %ld poll(s)", w.heldBack);
        }
        WheelLine("AFTER %s: the game's current slot %d -> %d (steered to %d)%s | %s", w.name, w.lastCur, cur, w.expect,
                  cur == w.expect ? "" : "  *** NOT WHAT WAS STEERED - the cursor did not reach the slot in time, or the slot is empty ***",
                  seen);
    }
    steerOut = steer;
    return w.m.ButtonDown() || w.confirming;
}

// The steering wheel's stick, only while the game shows its own wheel (see g_gameWheelComp). COST per poll while steering: up to
// four guarded reads (manager, registry slot, flags) - no RTTI, no lock.
bool WheelGameGate(WheelDrive& w, ULONGLONG now)
{
    const int a = GameWheelActive(w.which);
    if (a == 0) { ++w.heldBack; return false; }
    if (a == 1 && !w.gameSeen) {
        w.gameSeen = true;
        w.gameSeenAt = now;
    }
    return true;
}

// Two wheels, one right stick: the caller publishes the one that steers (a press starts in one zone only, so at most one is open).
void WheelPublishStick(const WheelDrive* w)
{
    InterlockedExchange(&g_wheelStickSlot, static_cast<LONG>(w ? w->target : wheel::Slot::None));
    InterlockedExchange(&g_wheelStickOn, w ? 1 : 0);
}

}  // namespace

void HolsterBodyReset() { InterlockedExchange(&g_bfResetAsked, 1); }

bool HolsterWheelHoldsX() { return g_wheelHoldsX != 0; }
bool HolsterWheelShowing() { return g_wheelShowing != 0; }
bool HolsterBoltWheelHoldsR2() { return g_boltHoldsR2 != 0; }
bool HolsterArrowWheelOpen() { return g_arrowQWheelOpen != 0; }

// ===== Task 6 v3 (D13; Jay after run Q2: the wide box "brought the entire HUD back", then "A"): the rest of the HUD hidden by the
// game's OWN mechanism (INV-642, our gamedll): the HUD manager's per-component hide mask + SetHudEnabled, exactly as the group switch
// sub_180556200 does it, with bit 31 as ours. =====
namespace {
using SetHudEnabledFn = char(__fastcall*)(void* component, char enable);
constexpr uintptr_t kHhMgrRva = 0x1C16358;        // the HUD manager global (INV-570)
constexpr uintptr_t kHhSetEnRva = 0x113E930;      // SetHudEnabled (INV-642)
constexpr uintptr_t kHhRegOff = 72;               // components at +72 + 8 x id
constexpr uintptr_t kHhMaskOff = 1368;            // hide masks at +1368 + 4 x id
constexpr uintptr_t kHhGroupCountOff = 0x548;     // the group count (sub_180556200's bound)
// INV-643 (Jay 2026-10-28: "override the games HUD states for the weapon wheel" -> "All of them. Go ahead"): the gameplay HUD option
// Hidden is group 13 (Inverted - every component but 11, the wheels included, gets mask bit 13). While a wheel is open, bit 13 ALONE
// is lifted from the selection wheels (other groups' bits stay), and put back at the restore only while group 13 still hides.
// Limited (group 12, NoNavigation) never holds a wheel. The group switch rewrites members only on its own state edge, so the lift
// is not re-set per frame.
constexpr uintptr_t kHhGroupsOff = 0x540;         // group records, 48 bytes each; byte 0 = the last enable passed (0 = hiding)
constexpr int       kHhHiddenGroup = 13;          // OUR sub_180555120: 13 = "Hidden"
constexpr unsigned  kHhHiddenBit = 1u << kHhHiddenGroup;
constexpr uintptr_t kHhFlagsOff = 0x30;           // a component's flags byte (+48): bit 0 active, 1 enabled, 2 wanted, 3 blocked
constexpr uintptr_t kHhIdOff = 0x2C;              // a component's own id (sub_180560D90)
constexpr int       kHhMaxId = 0x88;
constexpr unsigned  kHhOurBit = 0x80000000u;      // bit 31: groups use bit (1 << group index)
constexpr ULONGLONG kHhFadeHoldMs = 400;          // = uipanel's kWheelFadeHoldMs: the game's HideHud fade plays on the wheel first
const unsigned char kHhSetEnBytes[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x0F, 0xB6, 0x41, 0x30 };
int             g_hhState = 0;                    // 0 = not resolved, 1 = live, -1 = refused (named once)
SetHudEnabledFn g_hhSetEn = nullptr;
uintptr_t       g_hhGd = 0;
bool            g_hhHiding = false;
ULONGLONG       g_hhLastShowMs = 0;
uintptr_t       g_hhMine[kHhMaxId + 1] = {};      // the component each id had when OUR bit was set (0 = not ours)
uintptr_t       g_hhLifted[kHhMaxId + 1] = {};    // the wheel each id had when WE lifted the Hidden group's bit (0 = not lifted)
uintptr_t       g_hhClsPtr[kHhMaxId + 1] = {};    // RTTI cache: the pointer last classed per id ...
bool            g_hhIsWheel[kHhMaxId + 1] = {};   // ... and whether it is a selection wheel
int             g_hhWheelKind[kHhMaxId + 1] = {}; // ... which: 0 weapons, 1 throwables, 2 arrows (the drives' order), 3 bolts
bool            g_hhSaidPublish = false;          // the stick gate's "fewer than 3 wheels registered" line, once
long            g_hhLines = 0;

void HhLine(const char* fmt, ...)
{
    if (g_hhLines >= kWheelMaxLines) { return; }
    ++g_hhLines;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    // [PROBE: HUDHIDE] gate=holster_enable,holster_rhip_enable  site=HEADSET  log-only=no
    LOG("holster: [HUDHIDE] %s", buf);
}
bool HhPtr(uintptr_t a, uintptr_t& out)
{
    __try { out = *reinterpret_cast<const uintptr_t*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { out = 0; return false; }
}
bool HhU32(uintptr_t a, unsigned& out)
{
    __try { out = *reinterpret_cast<const unsigned*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { out = 0; return false; }
}
bool HhPutU32(uintptr_t a, unsigned v)
{
    __try { *reinterpret_cast<unsigned*>(a) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool HhSetEnabled(uintptr_t comp, bool enable)
{
    __try { g_hhSetEn(reinterpret_cast<void*>(comp), enable ? 1 : 0); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void HhResolve()
{
    g_hhState = -1;
    const HMODULE gd = GetModuleHandleA("gamedll_x64_rwdi.dll");
    if (!gd) { HhLine("REFUSED - gamedll_x64_rwdi.dll is not loaded; the HUD is NOT hidden around the wheels"); return; }
    g_hhGd = reinterpret_cast<uintptr_t>(gd);
    const uintptr_t fn = g_hhGd + DLVR_HH_GD(kHhSetEnRva);
    bool same = true;
    __try { same = memcmp(reinterpret_cast<const void*>(fn), kHhSetEnBytes, sizeof(kHhSetEnBytes)) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { same = false; }
    if (!same) {
        HhLine("REFUSED - the HUD enable function's bytes are not this build's; the HUD is NOT hidden around the wheels");
        return;
    }
    g_hhSetEn = reinterpret_cast<SetHudEnabledFn>(fn);
    g_hhState = 1;
    HhLine("LIVE (logged unconditionally) - the HUD enable function byte-checked");
}
bool HhIsWheel(int id, uintptr_t comp)
{
    if (g_hhClsPtr[id] != comp) {
        g_hhClsPtr[id] = comp;
        char cls[64] = {};
        const bool named = RttiClassNameOf(comp, cls, sizeof(cls));
        g_hhWheelKind[id] = !named ? -1
                          : strcmp(cls, "HudPrimaryWeaponSelection") == 0   ? 0
                          : strcmp(cls, "HudSecondaryWeaponSelection") == 0 ? 1
                          : strcmp(cls, "HudArrowsSelection") == 0          ? 2
                          : strcmp(cls, "HudBoltSelection") == 0            ? 3 : -1;
        g_hhIsWheel[id] = g_hhWheelKind[id] >= 0;
    }
    return g_hhIsWheel[id];
}

// The game's own wheel for a drive: 1 = its component is registered and ACTIVE (flags +48 bit 0, set by its re-activate / ShowHud -
// INV-643 slot 182), 0 = not active, not registered at the id the HIDE scan saw, or not published yet (the scan runs on the game
// thread a tick after our OPEN), 2 = the HUD hide is refused this session (no manager / bytes) - the stick behaves as before then,
// said once. Pad thread. COST: up to four SEH-guarded reads.
bool g_gateSaidRefused = false;
int GameWheelActive(int which)
{
    if (g_hhState == -1) {
        if (!g_gateSaidRefused) {
            g_gateSaidRefused = true;
            WheelLine("STICK GATE OFF - the HUD hide is refused this session, so the game's wheel cannot be read: the stick steers "
                      "from our OPEN as before (a trap or other use of the button can turn the view)");
        }
        return 2;
    }
    if (which < 0 || which > 3) { return 0; }
    const uintptr_t comp = static_cast<uintptr_t>(g_gameWheelComp[which]);
    const LONG id = g_gameWheelId[which];
    if (!comp || id < 0 || id > kHhMaxId || !g_hhGd) { return 0; }
    uintptr_t mgr = 0, reg = 0;
    if (!HhPtr(g_hhGd + DLVR_HH_GD(kHhMgrRva), mgr) || !mgr) { return 0; }
    if (!HhPtr(mgr + kHhRegOff + 8 * static_cast<uintptr_t>(id), reg) || reg != comp) { return 0; }
    unsigned f = 0;
    if (!HhU32(comp + kHhFlagsOff, f)) { return 0; }
    return (f & 1u) ? 1 : 0;
}
}  // namespace

void HolsterHudHideTick()
{
    const ULONGLONG now = GetTickCount64();
    if (g_wheelShowing) { g_hhLastShowMs = now; }
    const bool want = g_hhLastShowMs != 0 && now - g_hhLastShowMs <= kHhFadeHoldMs;
    if (want == g_hhHiding) { return; }
    g_hhHiding = want;
    if (g_hhState == 0) { HhResolve(); }
    if (g_hhState != 1) { return; }
    uintptr_t mgr = 0;
    if (!HhPtr(g_hhGd + DLVR_HH_GD(kHhMgrRva), mgr) || !mgr) {
        HhLine("%s SKIPPED - no HUD manager (the global reads 0)", want ? "HIDE" : "RESTORE");
        return;
    }
    if (want) {
        // The stick gate's components first (WheelGameGate), before any refusal below - a refused hide must not leave the wheels
        // unsteerable. The gate re-checks the registry slot on every read.
        int published = 0;
        for (int id = 0; id <= kHhMaxId; ++id) {
            uintptr_t comp = 0;
            if (!HhPtr(mgr + kHhRegOff + 8 * static_cast<uintptr_t>(id), comp) || !comp) { continue; }
            if (!HhIsWheel(id, comp)) { continue; }
            const int kind = g_hhWheelKind[id];
            if (kind >= 0 && kind <= 3) {
                InterlockedExchange64(&g_gameWheelComp[kind], static_cast<LONG64>(comp));
                InterlockedExchange(&g_gameWheelId[kind], id);
                ++published;
            }
        }
        if (published < 4 && !g_hhSaidPublish) {
            g_hhSaidPublish = true;
            HhLine("STICK GATE - only %d of the 4 steered wheels (weapons / throwables / arrows / bolts) are registered: a wheel not "
                   "found is never steered", published);
        }
        unsigned groups = 0;
        if (!HhU32(mgr + kHhGroupCountOff, groups) || groups >= 31) {
            HhLine("HIDE REFUSED - the HUD group count reads %u (>= 31 would share our mask bit 31); nothing hidden", groups);
            return;
        }
        // layout check first, before any write: every registered component's own id must equal its slot
        int bad = 0, regs = 0;
        for (int id = 0; id <= kHhMaxId; ++id) {
            uintptr_t comp = 0;
            unsigned own = 0;
            if (!HhPtr(mgr + kHhRegOff + 8 * static_cast<uintptr_t>(id), comp) || !comp) { continue; }
            ++regs;
            if (!HhU32(comp + kHhIdOff, own) || own != static_cast<unsigned>(id)) { ++bad; }
        }
        if (bad) {
            HhLine("HIDE REFUSED - %d of %d registered components fail the id check (their own id != their slot): the registry layout "
                   "is not the expected one; nothing hidden", bad, regs);
            return;
        }
        int hid = 0, wheels = 0, already = 0, fail = 0, lifted = 0;
        char wl[160] = {};                        // each wheel: id, its mask and flags BEFORE the lift (INV-643 NOT established)
        size_t wn = 0;
        for (int id = 0; id <= kHhMaxId; ++id) {
            uintptr_t comp = 0;
            if (!HhPtr(mgr + kHhRegOff + 8 * static_cast<uintptr_t>(id), comp) || !comp) { continue; }
            if (HhIsWheel(id, comp)) {
                ++wheels;
                unsigned wm = 0, wf = 0;
                const bool okM = HhU32(mgr + kHhMaskOff + 4 * static_cast<uintptr_t>(id), wm);
                HhU32(comp + kHhFlagsOff, wf);
                if (wn < sizeof(wl) - 32) {
                    const int k = _snprintf_s(wl + wn, sizeof(wl) - wn, _TRUNCATE, " %d:%08X/%02X", id, wm, wf & 0xFFu);
                    if (k > 0) { wn += static_cast<size_t>(k); }
                }
                if (okM && groups > static_cast<unsigned>(kHhHiddenGroup) && (wm & kHhHiddenBit)) {
                    const unsigned nm = wm & ~kHhHiddenBit;
                    if (HhPutU32(mgr + kHhMaskOff + 4 * static_cast<uintptr_t>(id), nm) && HhSetEnabled(comp, nm == 0)) {
                        g_hhLifted[id] = comp;
                        ++lifted;
                    } else {
                        ++fail;
                    }
                }
                continue;
            }
            unsigned mask = 0;
            if (!HhU32(mgr + kHhMaskOff + 4 * static_cast<uintptr_t>(id), mask)) { ++fail; continue; }
            if (mask & kHhOurBit) { continue; }
            if (mask) { ++already; }   // a game group hides it already: our bit is added so its restore stays the game's
            if (!HhPutU32(mgr + kHhMaskOff + 4 * static_cast<uintptr_t>(id), mask | kHhOurBit) || !HhSetEnabled(comp, false)) {
                ++fail;
                continue;
            }
            g_hhMine[id] = comp;
            ++hid;
        }
        HhLine("HIDE - %d component(s) disabled (%d already hidden by a game group), %d selection wheel(s) left shown, %d failed | "
               "%d registered, %u groups | wheels id:mask/flags%s | %d lifted from the HUD option's Hidden group (bit 13)",
               hid, already, wheels, fail, regs, groups, wn ? wl : " none", lifted);
    } else {
        // the Hidden group's lift first: put bit 13 back only while group 13 still hides (the player may have chosen Full meanwhile)
        int relif = 0, dropped = 0;
        uintptr_t grp = 0;
        unsigned gstate = 1;
        const bool haveG = HhPtr(mgr + kHhGroupsOff, grp) && grp &&
                           HhU32(grp + 48 * static_cast<uintptr_t>(kHhHiddenGroup), gstate);
        const bool stillHidden = haveG && (gstate & 0xFFu) == 0;
        for (int id = 0; id <= kHhMaxId; ++id) {
            if (!g_hhLifted[id]) { continue; }
            uintptr_t comp = 0;
            HhPtr(mgr + kHhRegOff + 8 * static_cast<uintptr_t>(id), comp);
            if (stillHidden && comp == g_hhLifted[id]) {
                unsigned mask = 0;
                HhU32(mgr + kHhMaskOff + 4 * static_cast<uintptr_t>(id), mask);
                HhPutU32(mgr + kHhMaskOff + 4 * static_cast<uintptr_t>(id), mask | kHhHiddenBit);
                HhSetEnabled(comp, false);
                ++relif;
            } else {
                ++dropped;
            }
            g_hhLifted[id] = 0;
        }
        if (relif || dropped) {
            HhLine("RESTORE - Hidden group: %d wheel(s) hidden again, %d left as they are%s", relif, dropped,
                   !dropped ? "" : !haveG ? " (the group record did not read)"
                                 : stillHidden ? " (the wheel was re-registered meanwhile)" : " (the HUD option is no longer Hidden)");
        }
        int shown = 0, kept = 0, moved = 0;
        for (int id = 0; id <= kHhMaxId; ++id) {
            if (!g_hhMine[id]) { continue; }
            unsigned mask = 0;
            uintptr_t comp = 0;
            HhU32(mgr + kHhMaskOff + 4 * static_cast<uintptr_t>(id), mask);
            HhPutU32(mgr + kHhMaskOff + 4 * static_cast<uintptr_t>(id), mask & ~kHhOurBit);
            HhPtr(mgr + kHhRegOff + 8 * static_cast<uintptr_t>(id), comp);
            if (comp != g_hhMine[id]) { ++moved; }
            if (comp) {
                const bool on = (mask & ~kHhOurBit) == 0;
                HhSetEnabled(comp, on);
                if (on) { ++shown; } else { ++kept; }
            }
            g_hhMine[id] = 0;
        }
        HhLine("RESTORE - %d component(s) enabled, %d kept hidden by a game group, %d re-registered meanwhile", shown, kept, moved);
    }
}

bool HolsterWheelStick(float& x, float& y)
{
    if (!g_wheelStickOn) { return false; }
    wheel::StickFor(static_cast<wheel::Slot>(g_wheelStickSlot), x, y);
    return true;
}

bool HolsterZoneLocal(int zone, int caller, const float headPos[3], const float headQ[4], const float grip[3], float out[3])
{
    zone = zone < 0 ? 0 : (zone >= kZoneCount ? kZoneCount - 1 : zone);
    caller = caller < 0 ? 0 : (caller > 2 ? 2 : caller);
    const holster::Vec3 eye{ headPos[0], headPos[1], headPos[2] };
    const holster::Vec3 palm{ grip[0], grip[1], grip[2] };
    holster::Vec3 local = holster::LocalOffset(eye, headQ[0], headQ[1], headQ[2], headQ[3], palm);
    BodySnap s;
    AcquireSRWLockShared(&g_snapLock);
    s = g_snap;
    ReleaseSRWLockShared(&g_snapLock);
    // Fresh (the holster polls ran within 250 ms - they stop when every zone is off or vr_grip=0) and latched. Every zone's anchor is
    // the neck point, from the caller's own head pose.
    const bool body = s.atS >= 0.0 && NowS() - s.atS <= 0.25 && s.latched[zone];
    if (body) {
        const XrVector3f nv = NeckOffsetM();
        const holster::Vec3 neck = holster::NeckPoint(eye, headQ[0], headQ[1], headQ[2], headQ[3], { nv.x, nv.y, nv.z });
        // 2026-11-16: the zone's anchor = the caller's neck + the published anchor - neck (0 for the shoulder; the hip point for the
        // hips - the throwables' right-hip ready zone).
        const holster::Vec3 an{ neck.x + s.anchorFromNeck[zone].x, neck.y + s.anchorFromNeck[zone].y,
                                neck.z + s.anchorFromNeck[zone].z };
        holster::Latch L;
        L.have = true;
        L.eyeFromAnchor = s.eyeFromAnchor[zone];
        local = holster::LocalOffsetYaw(holster::VirtualHead(an, s.yaw, L), s.yaw, palm);
    }
    InterlockedIncrement(&g_consumer[caller][body ? 0 : 1]);
    out[0] = local.x;
    out[1] = local.y;
    out[2] = local.z;
    return body;
}

bool HolsterPoll(bool vrGripOn, bool& outDpadRight, bool& outDpadLeft, bool& outOwnsGrip, bool gameMenu)
{
    outDpadRight = false;
    outDpadLeft = false;
    outOwnsGrip = false;
    InterlockedExchange(&g_wheelStickOn, 0);   // a poll that returns early never leaves the stick owned
    InterlockedExchange(&g_wheelHoldsX, 0);    // ... nor X held
    InterlockedExchange(&g_wheelShowing, 0);   // ... nor the HUD swapped for the wheel
    InterlockedExchange(&g_boltHoldsR2, 0);    // ... nor R2 withheld for the bolt wheel (its want expires by itself, ruling B2)
    InterlockedExchange(&g_boltWheelOpen, 0);  // ... nor the bolt hold allowed out of its zone
    InterlockedExchange(&g_arrowQWheelOpen, 0);  // ... nor the quiver hold (its want expires by itself, as the bolt one's)
    const bool shoulderOn = HolsterEnable;
    const bool rhipOn = HolsterRHipEnable;
    if (!shoulderOn && !rhipOn) {
        return false;
    }
    if (!vrGripOn) {
        static bool s_refusedLogged = false;
        if (!s_refusedLogged) {
            s_refusedLogged = true;
            // [PROBE: HOLSTER] gate=holster_enable,holster_rhip_enable  site=HEADSET  log-only=yes
            LOG("holster: [HOLSTER] REFUSED - vr_grip=0. The right-hand holsters live inside the grip -> R1 block, "
                "so they do nothing. Set vr_grip=1 to use them.");
        }
        return false;
    }
    const holster::Params params{ { HolsterCenterX, HolsterCenterY, HolsterCenterZ }, HolsterRadius };
    float rc[3];
    HolsterHipCentre(0, rc);   // holster_hip_from_calib: the calibrated belt centre, else holster_rhip_center_*
    const holster::Params rhip{ { rc[0], rc[1], rc[2] }, HolsterRHipRadius };
    const int rhipMs = HolsterRHipTapMs < 20 ? 20 : (HolsterRHipTapMs > 450 ? 450 : HolsterRHipTapMs);
    static bool s_armedLogged = false;
    if (!s_armedLogged) {
        s_armedLogged = true;
        // [PROBE: HOLSTER] gate=holster_enable,holster_rhip_enable  site=HEADSET  log-only=yes
        LOG("holster: [HOLSTER] ARMED (first poll, logged unconditionally) | fires ON GRIP | shoulder %s centre "
            "(%.3f, %.3f, %.3f) m, head-yaw frame +X right +Y up +Z behind | radius %.3f, exit %.3f | haptic %.2f | "
            "switch output: D-pad right (bit 0x%02X, measured INV-299) tap %d ms | right hip %s centre (%.3f, %.3f, "
            "%.3f) r %.3f -> D-pad left (bit 0x%02X, measured INV-311, item swap) tap %d ms",
            shoulderOn ? "ON" : "off", HolsterCenterX, HolsterCenterY, HolsterCenterZ, HolsterRadius,
            HolsterRadius * holster::kExitScale, HolsterHaptic, kDpadRightBit, HolsterTapMs,
            rhipOn ? "ON" : "off", rhip.centre.x, rhip.centre.y, rhip.centre.z, rhip.radius, kDpadLeftBit, rhipMs);
    }
    if (shoulderOn && rhipOn && holster::ZonesOverlap(params, rhip)) {
        static bool s_overlapLogged = false;
        if (!s_overlapLogged) {
            s_overlapLogged = true;
            // [PROBE: HOLSTER] gate=holster_enable,holster_rhip_enable  site=HEADSET  log-only=yes
            LOG("holster: [HOLSTER] REFUSED - the shoulder and right hip zones OVERLAP (exit spheres touch), so one "
                "press could fire both. Neither zone is active; R1 is unchanged. Move holster_center_* / "
                "holster_rhip_center_* apart or shrink the radii.");
        }
        return false;
    }

    const ULONGLONG now = GetTickCount64();
    ++g_polls;

    holster::Input in{};
    in.squeezeActive = GetControllerSqueeze(kRightHand, in.squeeze);
    XrPosef head{};
    XrPosef grip{};
    const bool headOk = GetHeadPose(head);
    const bool gripOk = GetControllerGripPose(kRightHand, grip);
    if (!headOk) { ++g_noHead; }
    if (!gripOk) { ++g_noGrip; }
    in.poseOk = headOk && gripOk;
    if (in.poseOk) {
        in.local = holster::LocalOffset({ head.position.x, head.position.y, head.position.z },
                                        head.orientation.x, head.orientation.y, head.orientation.z,
                                        head.orientation.w,
                                        { grip.position.x, grip.position.y, grip.position.z });
    }
    // Body-lock: each zone gets its own offset (body-locked when ready, else the head-relative one above).
    BodyFrameUpdate();
    holster::Input inH = in;
    if (in.poseOk) {
        // the shoulder's lines below print the offset its Machine used; a zone that is off is not counted (review M3)
        if (shoulderOn) { BodyLocal(kZoneShoulder, grip, in.local); }
        if (rhipOn) { BodyLocal(kZoneRHip, grip, inH.local); }
        // [HOLSTERPLACE] CHECK after a placement (review I1). Idle COST: two volatile reads.
        if (shoulderOn) { PlaceCheckPoll(kZoneShoulder, in.local, params.centre); }
        if (rhipOn) { PlaceCheckPoll(kZoneRHip, inH.local, rhip.centre); }
    }
    // A zone that is off never updates its Machine: its Step stays all-false, so nothing below fires for it.
    if (rhipOn) { HipYieldAsk(g_rhipYield, 1, g_rhipMachine, inH); }   // the right HIP only (spec §2.4)
    const holster::Step st = shoulderOn ? g_machine.Update(in, params) : holster::Step{};
    const holster::Step sr = rhipOn ? g_rhipMachine.Update(inH, rhip) : holster::Step{};
    if (rhipOn) { HipYieldLog(g_rhipYield, "R", sr); }
    const bool rhipPending = rhipOn && g_rhipMachine.state() == holster::Press::Pending;

    if (st.entered) {
        g_windowHadEvent = true;
        if (g_enterGate.Allow(now, kEdgeGapMs)) {
            const bool gripUp = g_machine.state() == holster::Press::Idle;
            // [PROBE: HOLSTER] gate=holster_enable  site=HEADSET  log-only=yes
            LOG("holster: [HOLSTER] ENTER local (%.3f, %.3f, %.3f) dist %.3f r %.3f | grip %s -> %s | %ld more "
                "ENTER(s) inside the 500 ms gate since the last line (no pulse, no line)",
                in.local.x, in.local.y, in.local.z, st.dist, HolsterRadius, gripUp ? "up" : "HELD",
                gripUp ? "pulse" : "no pulse (a held grip entering stays a normal press)", g_enterGate.TakeSkipped());
            // Place-by-hand (spec 2.6): no poll pulse while a placement runs. COST: one volatile read.
            if (gripUp && g_placing == 0) {
                Pulse(HolsterHaptic * 0.5f, 20);
            }
        }
    }
    if (st.exited) {
        g_windowHadEvent = true;
        if (g_exitGate.Allow(now, kEdgeGapMs)) {
            // [PROBE: HOLSTER] gate=holster_enable  site=HEADSET  log-only=yes
            LOG("holster: [HOLSTER] EXIT %s | local (%.3f, %.3f, %.3f) dist %.3f exit-r %.3f | head %s grip %s | %ld "
                "more EXIT(s) inside the 500 ms gate since the last line",
                in.poseOk ? "by distance" : "POSE LOST", in.local.x, in.local.y, in.local.z, st.dist,
                HolsterRadius * holster::kExitScale, headOk ? "valid" : "MISSING", gripOk ? "valid" : "MISSING",
                g_exitGate.TakeSkipped());
        }
    }
    // One press edge reaches both Machines; it is NORMAL (R1) only when neither zone claimed it - a right-hip press waiting for the
    // severed-limb reading (Pending) has claimed it for now; its outcome is the HIPYIELD line.
    const bool pressNormal = !st.pressHolster && !sr.pressHolster && !rhipPending && (st.pressNormal || sr.pressNormal);
    if (st.pressHolster || sr.pressHolster || pressNormal) {
        g_windowHadEvent = true;
        g_pressStart = now;
    }
    if (st.pressHolster) {
        ++g_pressHolster;
        // [PROBE: HOLSTER] gate=holster_enable,holster_rhip_enable  site=HEADSET  log-only=yes
        LOG("holster: [HOLSTER] PRESS holster (tap at the release, or the wheel if held; R1 suppressed for this press) | local (%.3f, %.3f, %.3f) "
            "dist %.3f r %.3f inZone %d | squeeze %.2f | head %s grip %s",
            in.local.x, in.local.y, in.local.z, st.dist, HolsterRadius, st.inZone ? 1 : 0, in.squeeze,
            headOk ? "valid" : "MISSING", gripOk ? "valid" : "MISSING");
    } else if (pressNormal) {
        ++g_pressNormal;
        // [PROBE: HOLSTER] gate=holster_enable,holster_rhip_enable  site=HEADSET  log-only=yes
        LOG("holster: [HOLSTER] PRESS normal (R1 as before) | local shoulder (%.3f, %.3f, %.3f) right hip (%.3f, %.3f, %.3f) | "
            "dist shoulder %.3f right hip %.3f (-1 = zone off or no pose) | squeeze %.2f | head %s grip %s",
            in.local.x, in.local.y, in.local.z, inH.local.x, inH.local.y, inH.local.z, shoulderOn ? st.dist : -1.0f,
            rhipOn ? sr.dist : -1.0f, in.squeeze,
            headOk ? "valid" : "MISSING", gripOk ? "valid" : "MISSING");
    }
    // 2026-10-27 (D7, Jay: "Wait for release"): the switch no longer fires ON GRIP (2026-09-30) - a short press switches at its
    // RELEASE and a press held to holster_wheel_hold_ms opens the game's weapon wheel (WheelDriveStep below, the [WHEEL] lines).
    if (st.releaseSwitch) {
        // [PROBE: HOLSTER] gate=holster_enable  site=HEADSET  log-only=yes
        LOG("holster: [HOLSTER] RELEASE | held %llu ms (the [WHEEL] line says tap or wheel) | local (%.3f, %.3f, %.3f) pose %s",
            static_cast<unsigned long long>(now - g_pressStart), in.local.x, in.local.y, in.local.z,
            in.poseOk ? "valid" : "MISSING");
    }
    if (st.cancelled) {
        ++g_cancels;
        // [PROBE: HOLSTER] gate=holster_enable  site=HEADSET  log-only=yes
        LOG("holster: [HOLSTER] RELEASE - the squeeze action went INACTIVE mid-press (treated as a release) | held %llu ms",
            static_cast<unsigned long long>(now - g_pressStart));
    }
    // The weapon wheel (plan 2026-10-27 Task 3). held = a press that started in the shoulder zone is still down.
    bool weaponTap = false, weaponSteer = false;
    const bool weaponsHeld = shoulderOn && g_machine.state() == holster::Press::Holster;
    if (WheelDriveStep(g_wWeapons, weaponsHeld, now, gameMenu, weaponTap, weaponSteer)) { outDpadRight = true; }
    if (weaponTap) {
        ++g_switches;
        // [PROBE: HOLSTER] gate=holster_enable  site=HEADSET  log-only=yes
        LOG("holster: [HOLSTER] SWITCH ON RELEASE: D-pad right (0x%02X) tap %d ms, tap #%ld | local (%.3f, %.3f, %.3f)",
            kDpadRightBit, HolsterTapMs, g_taps + 1, in.local.x, in.local.y, in.local.z);
        HolsterFireSwitch(now);
        if (g_placing == 0) { Pulse(HolsterHaptic, 40); }   // spec 2.6, as above. COST: one volatile read.
    }

    // RIGHT HIP (Jay 2026-10-04): zone edges (log + light pulse) and, on a press IN the zone, the D-pad left
    // tap + firm pulse. Same shape as the left zones' PollZone, on the right hand.
    if (sr.entered) {
        g_windowHadEvent = true;
        if (g_rhipEnterGate.Allow(now, kEdgeGapMs)) {
            const bool gripUp = g_rhipMachine.state() == holster::Press::Idle;
            // [PROBE: HOLSTER] gate=holster_enable,holster_rhip_enable  site=HEADSET  log-only=yes
            LOG("holster: [HOLSTER] ENTER right hip | local (%.3f, %.3f, %.3f) dist %.3f r %.3f | grip %s -> %s | "
                "%ld more ENTER(s) inside the 500 ms gate since the last line",
                inH.local.x, inH.local.y, inH.local.z, sr.dist, rhip.radius, gripUp ? "up" : "HELD",
                gripUp ? "pulse" : "no pulse (a held grip entering stays R1)", g_rhipEnterGate.TakeSkipped());
            if (gripUp && g_placing == 0) {   // spec 2.6, as above. COST: one volatile read.
                Pulse(HolsterHaptic * 0.5f, 20);
            }
        }
    }
    if (sr.exited) {
        g_windowHadEvent = true;
        if (g_rhipExitGate.Allow(now, kEdgeGapMs)) {
            // [PROBE: HOLSTER] gate=holster_enable,holster_rhip_enable  site=HEADSET  log-only=yes
            LOG("holster: [HOLSTER] EXIT right hip %s | local (%.3f, %.3f, %.3f) dist %.3f exit-r %.3f | head %s "
                "grip %s | %ld more EXIT(s) inside the 500 ms gate since the last line",
                in.poseOk ? "by distance" : "POSE LOST", inH.local.x, inH.local.y, inH.local.z, sr.dist,
                rhip.radius * holster::kExitScale, headOk ? "valid" : "MISSING", gripOk ? "valid" : "MISSING",
                g_rhipExitGate.TakeSkipped());
        }
    }
    // 2026-10-27 (plan Task 4, D7): the item swap fires at the RELEASE of a short press; held to holster_wheel_hold_ms it opens the
    // game's throwables wheel instead (WheelDriveStep). A press the hip yield handed to a body never reaches pressHolster, so it
    // never starts the wheel (Review Focus 5).
    if (sr.pressHolster) {
        // [PROBE: HOLSTER] gate=holster_enable,holster_rhip_enable  site=HEADSET  log-only=yes
        LOG("holster: [HOLSTER] PRESS right hip (item swap at the release, or the throwables wheel if held; R1 withheld for this "
            "press) | local (%.3f, %.3f, %.3f) dist %.3f r %.3f | squeeze %.2f | head %s grip %s",
            inH.local.x, inH.local.y, inH.local.z, sr.dist, rhip.radius, in.squeeze,
            headOk ? "valid" : "MISSING", gripOk ? "valid" : "MISSING");
    }
    if (sr.releaseSwitch || sr.cancelled) {
        // [PROBE: HOLSTER] gate=holster_enable,holster_rhip_enable  site=HEADSET  log-only=yes
        LOG("holster: [HOLSTER] RELEASE right hip%s | held %llu ms (the [WHEEL] line says tap or wheel)",
            sr.cancelled ? " - squeeze action went INACTIVE" : "",
            static_cast<unsigned long long>(now - g_pressStart));
    }
    bool throwTap = false, throwSteer = false;
    const bool throwHeld = rhipOn && g_rhipMachine.state() == holster::Press::Holster;
    if (WheelDriveStep(g_wThrow, throwHeld, now, gameMenu, throwTap, throwSteer)) { outDpadLeft = true; }
    if (throwTap) {
        g_rhipTap.Start(now, static_cast<unsigned long long>(rhipMs));
        g_rhipTapPolls = 0;
        ++g_rhipTaps;
        // [PROBE: HOLSTER] gate=holster_enable,holster_rhip_enable  site=HEADSET  log-only=yes
        LOG("holster: [HOLSTER] ITEM SWAP ON RELEASE: D-pad left (0x%02X) tap %d ms, tap #%ld", kDpadLeftBit, rhipMs, g_rhipTaps);
        if (g_placing == 0) { Pulse(HolsterHaptic, 40); }   // spec 2.6, as above. COST: one volatile read.
    }
    // The arrow wheel (Task 5, C): the real X held with the bow out (ScLeftIsBow - the rig's own left-hand item test).
    // COST: one controller button read + one class compare per poll.
    // 2026-11-11 (Jay: "I'd like holding X to no longer bring up the arrow/bolt wheels" -> "Yes, build it"): with the arrow wheel's X
    // block in (boltwheel.cpp - X never opens the game's wheel), this drive never starts: no HUD hide, no pulse, no X hold-on.
    // COST: + one volatile read per poll.
    bool arrowTap = false, arrowSteer = false, xDown = false;
    const bool arrowsHeld = !ArrowWheelXBlocked() && GetControllerButton(kButtonX, xDown) && xDown && ScLeftIsBow();
    const bool holdX = WheelDriveStep(g_wArrows, arrowsHeld, now, gameMenu, arrowTap, arrowSteer, true);
    InterlockedExchange(&g_wheelHoldsX, holdX ? 1 : 0);
    // The crossbow bolt wheel (plan 2026-11-04-crossbow-bolt-wheel Task 3): the left grip held at the bolt holster (g_boltHold, the
    // left poll's B4 rule) with both hooks live. No pad bit: the show action is held by BoltWheelSetWant. The crossbow put away (the
    // take no longer live) or the hooks refused while it is down -> Abort (button up at once, no confirm steer; Review Focus 2).
    // Review 2026-11-05 finding 1 (Jay: "No flicker, wheel at 0.7 s"): the GAME's show action goes down only from the drive's OPEN
    // (holster_wheel_hold_ms), not its Lead - so the open event is sent at OPEN and the game shows the wheel 0.2 s later; a release in
    // Lead (the slow take) sends nothing (no flicker, no select-next). ShowDownFromOpen, tested.
    // Ruling B1: once the show action went down it stays down >= 250 ms from the OPEN edge (NoSelectNext), so a release inside the
    // game's 0.2 s "select next" window changes nothing; meanwhile it steers to the confirm's target (a confirm) or the current slot.
    // COST: two volatile reads + the drive step (as each wheel) + one interlocked store per poll; a hook call only via the want stamp.
    bool boltTap = false, boltSteer = false;
    const bool boltLive = g_boltTakeLive != 0 && BoltWheelLive();
    if (!boltLive && (g_wBolts.m.ButtonDown() || g_wBolts.confirming)) {
        g_wBolts.m.Abort();
        g_wBolts.confirming = false;
    }
    const bool boltDown = WheelDriveStep(g_wBolts, boltLive && g_boltHold != 0, now, gameMenu, boltTap, boltSteer);
    const bool boltOpenDown = boltwheel::ShowDownFromOpen(boltDown, g_wBolts.m.Open() || g_wBolts.confirming);
    if (boltOpenDown) {
        g_boltTailTarget = g_wBolts.confirming ? g_wBolts.target
                                               : wheel::TargetSlot(wheel::Slot::None, HolsterWheelCurrentSlot(3));
    }
    const bool boltKeep = g_boltNsn.Update(boltOpenDown, now);
    if (boltKeep && !boltOpenDown) {
        g_wBolts.target = g_boltTailTarget;
        boltSteer = g_wBolts.target != wheel::Slot::None;
    }
    BoltWheelSetWant(boltKeep);
    InterlockedExchange(&g_boltWheelOpen, (g_wBolts.m.Open() || g_wBolts.confirming) ? 1 : 0);
    // R2 is held off through Lead too (as before the change): the hold at the holster, the wheel open, and the 250 ms tail.
    InterlockedExchange(&g_boltHoldsR2, (boltKeep || boltDown) ? 1 : 0);
    // The bow's arrow wheel from the quiver (2026-11-11, Jay: "Yes."): the bolt wheel's sequence on the arrow wheel - the quiver hold
    // (BowNockQuiverHold, the bow's own RT + shoulder zone) with the bow out (ScLeftIsBow, the X wheel's gate) and both hooks live;
    // the show action goes down for the game only from OPEN (ShowDownFromOpen), stays down >= 250 ms (NoSelectNext), and the bow put
    // away / the hooks gone while it is down -> Abort. No X, no R2 hold-off (see g_wArrowsQ).
    // COST: one volatile read + one class compare + the drive step (as each wheel) + two interlocked stores per poll.
    bool arrowQTap = false, arrowQSteer = false;
    const bool arrowQLive = ArrowWheelLive() && ScLeftIsBow();
    if (!arrowQLive && (g_wArrowsQ.m.ButtonDown() || g_wArrowsQ.confirming)) {
        g_wArrowsQ.m.Abort();
        g_wArrowsQ.confirming = false;
    }
    const bool arrowQDown = WheelDriveStep(g_wArrowsQ, arrowQLive && BowNockQuiverHold(), now, gameMenu, arrowQTap, arrowQSteer);
    const bool arrowQOpenDown = boltwheel::ShowDownFromOpen(arrowQDown, g_wArrowsQ.m.Open() || g_wArrowsQ.confirming);
    if (arrowQOpenDown) {
        g_arrowTailTarget = g_wArrowsQ.confirming ? g_wArrowsQ.target
                                                  : wheel::TargetSlot(wheel::Slot::None, HolsterWheelCurrentSlot(2));
    }
    const bool arrowQKeep = g_arrowNsn.Update(arrowQOpenDown, now);
    if (arrowQKeep && !arrowQOpenDown) {
        g_wArrowsQ.target = g_arrowTailTarget;
        arrowQSteer = g_wArrowsQ.target != wheel::Slot::None;
    }
    ArrowWheelSetWant(arrowQKeep);
    InterlockedExchange(&g_arrowQWheelOpen, (g_wArrowsQ.m.Open() || g_wArrowsQ.confirming) ? 1 : 0);
    // Task 6 (D13): a wheel is showing from its OPEN to the end of its confirm steer - the HUD panel shows only the wheel meanwhile.
    auto showing = [](const WheelDrive& w) { return w.m.Open() || w.confirming; };
    InterlockedExchange(&g_wheelShowing,
                        (showing(g_wWeapons) || showing(g_wThrow) || showing(g_wArrows) || showing(g_wBolts) || showing(g_wArrowsQ))
                            ? 1 : 0);
    // The stick only while the game shows that wheel (WheelGameGate - Jay 2026-10-28, the trap spin).
    WheelDrive* steerer = weaponSteer ? &g_wWeapons
                        : (throwSteer ? &g_wThrow
                                      : (arrowSteer ? &g_wArrows
                                                    : (boltSteer ? &g_wBolts : (arrowQSteer ? &g_wArrowsQ : nullptr))));
    if (steerer && !WheelGameGate(*steerer, now)) { steerer = nullptr; }
    WheelPublishStick(steerer);

    if (st.suppressR1 || sr.suppressR1) {
        ++g_suppressedPolls;
    }

    // The weapon-switch tap: held for HolsterTapMs from the release, then let go. The end is logged with how
    // many polls actually carried the bit - 0 would mean the game never saw the press.
    if (g_tap.Active(now)) {
        outDpadRight = true;
        ++g_tapPolls;
    } else if (g_tap.TakeEnded(now)) {
        // [PROBE: HOLSTER] gate=holster_enable  site=HEADSET  log-only=yes
        LOG("holster: [HOLSTER] TAP END #%ld - D-pad right (0x%02X) was OR'd on %ld poll(s)", g_taps,
            kDpadRightBit, g_tapPolls);
    }
    if (g_rhipTap.Active(now)) {
        outDpadLeft = true;
        ++g_rhipTapPolls;
    } else if (g_rhipTap.TakeEnded(now)) {
        // [PROBE: HOLSTER] gate=holster_enable,holster_rhip_enable  site=HEADSET  log-only=yes
        LOG("holster: [HOLSTER] TAP END right hip #%ld - D-pad left (0x%02X) was OR'd on %ld poll(s)", g_rhipTaps,
            kDpadLeftBit, g_rhipTapPolls);
    }

    // Haptic results arrive a frame later (ControllersSync applies them). First result and first failure
    // are logged the moment they appear; after that they are only counted (window line).
    long hOk = 0;
    long hFail = 0;
    int hLast = 0;
    if (GetHapticStats(kRightHand, hOk, hFail, hLast)) {
        if (!g_firstHapticResultLogged && (hOk + hFail) > 0) {
            g_firstHapticResultLogged = true;
            // [PROBE: HOLSTER] gate=holster_enable  site=HEADSET  log-only=yes
            LOG("holster: [HOLSTER] FIRST HAPTIC RESULT XrResult %d (ok %ld fail %ld)", hLast, hOk, hFail);
        }
        if (!g_firstHapticFailLogged && hFail > 0) {
            g_firstHapticFailLogged = true;
            // [PROBE: HOLSTER] gate=holster_enable  site=HEADSET  log-only=yes
            LOG("holster: [HOLSTER] *** HAPTIC FAILED *** XrResult %d - pulses are not reaching the controller",
                hLast);
        }
    }

    if (g_windowStart == 0) {
        g_windowStart = now;
    } else if (now - g_windowStart >= 10000) {
        if (g_windowHadEvent) {
            // [PROBE: HOLSTER] gate=holster_enable  site=HEADSET  log-only=yes
            LOG("holster: [HOLSTER] window (session totals) | polls %ld | presses holster %ld normal %ld | switches "
                "%ld cancelled %ld | right hip taps %ld | suppressed-polls %ld | no-head %ld no-grip %ld | haptic ok "
                "%ld fail %ld last %d, refused %ld",
                g_polls, g_pressHolster, g_pressNormal, g_switches, g_cancels, g_rhipTaps, g_suppressedPolls,
                g_noHead, g_noGrip, hOk, hFail, hLast, g_pulseRefused);
        }
        g_windowStart = now;
        g_windowHadEvent = false;
    }

    outOwnsGrip = st.ownsGrip || sr.ownsGrip;
    return st.suppressR1 || sr.suppressR1;
}

// =====================================================================================================
// LEFT HAND - hip (heal) + left temple (torch). Jay 2026-09-30: "Another 2 holsters. Contextual based on
// location as to not override the base control tied to LEFT GRIP. One on the left hip, to swap between items
// (LEFT DPAD) and one against the left side of the players head to Activate/Deactivate the Torch (UP DPAD)".
// Jay 2026-10-04: "The left hip holster, we have that currently set to DPAD LEFT, we want it as DPAD DOWN" -
// the item swap (D-pad left) moved to the new RIGHT hip (HolsterPoll above).
// Jay chose ON GRIP: the tap fires on the press edge in the zone. L1 is withheld for that whole press.
// Same zone maths and press machine as the shoulder (holster_logic.h), one Machine per zone on the one left
// grip; a pair whose exit spheres touch is REFUSED by name (a press could otherwise start in both).
// =====================================================================================================

// [DEP: holster_hip_enable] required-for=the left-hip holster (HolsterHipEnable): left grip at the hip taps
//      D-pad down (heal; was D-pad left until 2026-10-04) instead of L1. tree=INERT. Was DEFAULT OFF. Off: with it and holster_head_enable both 0,
//      HolsterLeftPoll returns false at its first line and L1 is unchanged. HEADSET (left GRIP pose + real
//      head pose). Needs vr_grip=1 (refuses by name). Reads no bones.
bool HolsterHipEnable = true;   // DEFAULT ON 2026-10-01 (Jay: Tier A ships on)
// [DEP: holster_hip_center_x] [DEP: holster_hip_center_y] [DEP: holster_hip_center_z] required-for=the hip
//      zone centre (HolsterHipCenterX / HolsterHipCenterY / HolsterHipCenterZ), head-yaw frame, metres, +X right
//      +Y up +Z behind. tree=INERT. A GUESS until the first run's [HOLSTER-L] PRESS lines measure it.
float HolsterHipCenterX = -0.20f;
float HolsterHipCenterY = -0.60f;
float HolsterHipCenterZ = 0.00f;
// [DEP: holster_hip_radius] required-for=the hip zone's enter radius (HolsterHipRadius), exit = x1.2.
//      tree=INERT. GUESS.
float HolsterHipRadius = 0.15f;
// [DEP: holster_hip_tap_ms] required-for=the left hip's output (HolsterHipTapMs): how long D-pad down is held.
//      D-pad down = _ACTION_HEAL (our Data0.pak inputs_pad.scr:16), no HoldTime - same AddAction shape as
//      D-pad right/left, whose 100 ms taps are measured (INV-299/311). Whether a 100 ms tap of D-pad down
//      heals is [UNKNOWN] until a run. tree=INERT.
int HolsterHipTapMs = 100;
// [DEP: holster_head_enable] required-for=the left-temple holster (HolsterHeadEnable): left grip at the left
//      side of the head taps D-pad up (torch) instead of L1. tree=INERT. Was DEFAULT OFF. HEADSET. Needs vr_grip=1.
bool HolsterHeadEnable = true;   // DEFAULT ON 2026-10-01 (Jay: Tier A ships on)
// [DEP: holster_head_center_x] [DEP: holster_head_center_y] [DEP: holster_head_center_z] required-for=the
//      temple zone centre (HolsterHeadCenterX / HolsterHeadCenterY / HolsterHeadCenterZ), head-yaw frame.
//      tree=INERT. GUESS.
float HolsterHeadCenterX = -0.12f;
float HolsterHeadCenterY = 0.00f;
float HolsterHeadCenterZ = 0.00f;
// [DEP: holster_head_radius] required-for=the temple zone's enter radius (HolsterHeadRadius). tree=INERT. GUESS.
float HolsterHeadRadius = 0.10f;
// [DEP: holster_head_tap_ms] required-for=the torch output (HolsterHeadTapMs): how long D-pad up is held.
//      D-pad up also carries _ACTION_FAST_EQUIP_ITEM { HoldTime(0.1) } (shipped inputs_pad.scr:22), so the
//      tap stays under 100 ms (registered range 20-90). Whether the game registers a tap this short is
//      [UNKNOWN] until a run. tree=INERT.
int HolsterHeadTapMs = 50;
// [DEP: holster_lchest_enable] required-for=the left-chest holster (HolsterLChestEnable): left grip at the left chest taps D-pad
//      down (HEAL - moved here from the left hip, spec 2026-11-01-manual-reload-design goal 2, Jay: "Heal on left chest, grabbed by
//      left hand"; "Chest, always" = not tied to manual_reload). Body-locked like the hips (the neck-point frame). tree=INERT.
//      Tier B (the layout Jay set). HEADSET. Needs vr_grip=1. The heal TAP length is holster_hip_tap_ms (unchanged key).
bool  HolsterLChestEnable = true;
// [DEP: holster_lchest_center_x] [DEP: holster_lchest_center_y] [DEP: holster_lchest_center_z] required-for=the left-chest zone
//      centre (HolsterLChestCenterX / Y / Z), body frame, metres, +X right +Y up +Z behind. tree=INERT. Tier D - a start value
//      clear of the hip and temple defaults (holster_place_logic_test LeftDefaultsDoNotOverlap); placed by hand.
float HolsterLChestCenterX = -0.12f;
float HolsterLChestCenterY = -0.30f;
float HolsterLChestCenterZ = -0.12f;
// [DEP: holster_lchest_radius] required-for=the left-chest zone's enter radius (HolsterLChestRadius), exit = x1.2. tree=INERT. Tier D.
float HolsterLChestRadius = 0.08f;

namespace {

// MEASURED in the headset 2026-09-30 (INV-311): 0x80 = D-pad left (12/12 item swaps), 0x10 = D-pad up (14/14
// torch toggles, no fast-equip at 50 ms). Down = 0x40 is the public layout's remaining D-pad bit (the other
// three measured to that layout) and is NOT yet measured in the headset - the left hip's first run measures it.
constexpr unsigned kDpadDownBit = 0x40u;
constexpr unsigned kDpadUpBit = 0x10u;

struct LeftZone {
    const char* name;      // "hip" / "head"
    const char* output;    // what the tap does, for the log
    unsigned bit;
    holster::Machine machine;
    holster::TapPulse tap;
    holster::RateGate enterGate;
    holster::RateGate exitGate;
    long taps = 0;
    long tapPolls = 0;
};
LeftZone g_hip{ "hip", "the ammo pouch (no tap)", 0u };   // spec 2026-11-01 goal 2: the pouch - its press fires NO tap
LeftZone g_head{ "head", "D-pad up (torch)", kDpadUpBit };
LeftZone g_chest{ "chest", "D-pad down (heal)", kDpadDownBit };
volatile LONG g_pouchHeld = 0;   // HolsterLeftPouchHeld(): the left grip held in the hip zone
// The crossbow's left-shoulder take (plan 2026-11-03-manual-reload-crossbow Task 4). g_xbowTakeLive is written by reload.cpp's tick
// (HolsterSetXbowTakeLive), read by the scePad poll; g_leftShoulderIn is written by the poll, read by reload.cpp. The rest is the
// poll thread's own.
volatile LONG g_xbowTakeLive = 0;
volatile LONG g_leftShoulderIn = 0;
bool g_takeZoneWas = false;    // the take zone's hysteresis state
bool g_takePrevDown = false;   // the left grip's press edge, for the take line (independent of which Machines are enabled)
// Jay after RUN XB1 (2026-11-03: "Why did the quiver not have a pulse to begin with, it's needed" -> "Add all 3 fixes"): the quiver
// zone pulses on ENTER like every other holster zone (half strength, 20 ms, only with the grip up; holster_haptic, no new key), with
// the same ENTER / EXIT lines and 500 ms line gates. Pad thread.
holster::RateGate g_takeEnterGate, g_takeExitGate;
bool g_takeSaidLive = false;   // the first-rise line, once per session
long g_takePresses = 0;
const char* volatile g_leftRefusal = nullptr;   // HolsterLeftTakeRefusal (crossbow plan Task 5): the poll's last refusal, a literal

long g_lPolls = 0;
long g_lSuppressedPolls = 0;
long g_lPressNormal = 0;
long g_lNoHead = 0;
long g_lNoGrip = 0;
long g_lPulseRefused = 0;
ULONGLONG g_lWindowStart = 0;
bool g_lWindowHadEvent = false;
ULONGLONG g_lPressStart = 0;
bool g_lFirstHapticResultLogged = false;
bool g_lFirstHapticFailLogged = false;

void PulseLeft(float amplitude, int durationMs)
{
    if (HolsterHaptic <= 0.0f) {
        return;
    }
    if (!PulseController(kLeftHand, amplitude, durationMs)) {
        ++g_lPulseRefused;
    }
}

int ClampMs(int ms, int lo, int hi)
{
    return ms < lo ? lo : (ms > hi ? hi : ms);
}

// One zone's poll: zone edges (log + light pulse) and, on a press IN the zone, the tap + firm pulse.
// Returns the Machine's step so the caller can combine suppression and log a normal press once.
holster::Step PollZone(LeftZone& z, bool enabled, const holster::Input& in, const holster::Params& p, int tapMs,
                       ULONGLONG now, bool headOk, bool gripOk)
{
    if (!enabled) {
        return holster::Step{};
    }
    const holster::Step st = z.machine.Update(in, p);
    if (st.entered) {
        g_lWindowHadEvent = true;
        if (z.enterGate.Allow(now, kEdgeGapMs)) {
            const bool gripUp = z.machine.state() == holster::Press::Idle;
            // [PROBE: HOLSTER-L] gate=holster_hip_enable,holster_head_enable  site=HEADSET  log-only=yes
            LOG("holster: [HOLSTER-L] ENTER %s | local (%.3f, %.3f, %.3f) dist %.3f r %.3f | grip %s -> %s | %ld "
                "more ENTER(s) inside the 500 ms gate since the last line",
                z.name, in.local.x, in.local.y, in.local.z, st.dist, p.radius, gripUp ? "up" : "HELD",
                gripUp ? "pulse" : "no pulse (a held grip entering stays L1)", z.enterGate.TakeSkipped());
            if (gripUp && g_placing == 0) {   // spec 2.6: no poll pulse while a placement runs. COST: one volatile read.
                PulseLeft(HolsterHaptic * 0.5f, 20);
            }
        }
    }
    if (st.exited) {
        g_lWindowHadEvent = true;
        if (z.exitGate.Allow(now, kEdgeGapMs)) {
            // [PROBE: HOLSTER-L] gate=holster_hip_enable,holster_head_enable  site=HEADSET  log-only=yes
            LOG("holster: [HOLSTER-L] EXIT %s %s | local (%.3f, %.3f, %.3f) dist %.3f exit-r %.3f | head %s grip %s "
                "| %ld more EXIT(s) inside the 500 ms gate since the last line",
                z.name, in.poseOk ? "by distance" : "POSE LOST", in.local.x, in.local.y, in.local.z, st.dist,
                p.radius * holster::kExitScale, headOk ? "valid" : "MISSING", gripOk ? "valid" : "MISSING",
                z.exitGate.TakeSkipped());
        }
    }
    if (st.pressHolster) {
        g_lWindowHadEvent = true;
        if (z.bit != 0) {   // the pouch (bit 0) fires no tap - reload.cpp reads the held press (HolsterLeftPouchHeld)
            z.tap.Start(now, static_cast<unsigned long long>(tapMs));
            z.tapPolls = 0;
        }
        ++z.taps;
        // [PROBE: HOLSTER-L] gate=holster_hip_enable,holster_head_enable  site=HEADSET  log-only=yes
        LOG("holster: [HOLSTER-L] PRESS %s -> FIRE %s (0x%02X) tap %d ms, tap #%ld, L1 withheld for this press | "
            "local (%.3f, %.3f, %.3f) dist %.3f r %.3f | squeeze %.2f | head %s grip %s",
            z.name, z.output, z.bit, tapMs, z.taps, in.local.x, in.local.y, in.local.z, st.dist, p.radius,
            in.squeeze, headOk ? "valid" : "MISSING", gripOk ? "valid" : "MISSING");
        if (g_placing == 0) { PulseLeft(HolsterHaptic, 40); }   // spec 2.6, as above. COST: one volatile read.
    }
    if (st.releaseSwitch || st.cancelled) {
        // [PROBE: HOLSTER-L] gate=holster_hip_enable,holster_head_enable  site=HEADSET  log-only=yes
        LOG("holster: [HOLSTER-L] RELEASE %s%s | held %llu ms (the tap already fired on the grip)", z.name,
            st.cancelled ? " - squeeze action went INACTIVE" : "",
            static_cast<unsigned long long>(now - g_lPressStart));
    }
    return st;
}

// The tap: bit held from the press for tapMs; the end logs how many polls carried it (0 = the game never saw it).
bool TapTick(LeftZone& z, ULONGLONG now)
{
    if (z.tap.Active(now)) {
        ++z.tapPolls;
        return true;
    }
    if (z.tap.TakeEnded(now)) {
        // [PROBE: HOLSTER-L] gate=holster_hip_enable,holster_head_enable  site=HEADSET  log-only=yes
        LOG("holster: [HOLSTER-L] TAP END %s #%ld - %s (0x%02X) was OR'd on %ld poll(s)", z.name, z.taps, z.output,
            z.bit, z.tapPolls);
    }
    return false;
}

}  // namespace

bool HolsterLeftPoll(bool vrGripOn, bool& outDpadDown, bool& outDpadUp, bool& outOwnsGrip)
{
    outDpadDown = false;
    outDpadUp = false;
    outOwnsGrip = false;
    // Spec 2026-11-01 goal 2: the hip is the ammo POUCH - live only while manual_reload is on ("with manual reload OFF the left hip
    // is simply unused"); heal is the CHEST zone. COST: one bool read each.
    const bool hipOn = HolsterHipEnable && ManualReload;
    const bool headOn = HolsterHeadEnable;
    const bool chestOn = HolsterLChestEnable;
    // The crossbow's left-shoulder take (Task 4): it keeps the poll running even with every left zone off. COST: one volatile read.
    // Crossbow plan Task 5: also gated by ManualReload here - reload.cpp's tick stops running when manual_reload is switched off, so
    // its last "live" must not outlive the switch (one bool read).
    const bool takeLive = g_xbowTakeLive != 0 && ManualReload;
    if (!hipOn && !headOn && !chestOn && !takeLive) {
        InterlockedExchange(&g_pouchHeld, 0);
        g_takeZoneWas = false;
        InterlockedExchange(&g_leftShoulderIn, 0);
        g_takePrevDown = true;   // review M3: a grip already held when the take turns live is not a press edge (no stale take line)
        BoltHoldOff(takeLive);
        return false;
    }
    if (!vrGripOn) {
        g_takeZoneWas = false;
        InterlockedExchange(&g_leftShoulderIn, 0);
        BoltHoldOff(takeLive);
        g_leftRefusal = "vr_grip=0";
        static bool s_refusedLogged = false;
        if (!s_refusedLogged) {
            s_refusedLogged = true;
            // [PROBE: HOLSTER-L] gate=holster_hip_enable,holster_head_enable  site=HEADSET  log-only=yes
            LOG("holster: [HOLSTER-L] REFUSED - vr_grip=0. The left holsters live inside the grip -> L1 block, so "
                "they do nothing. Set vr_grip=1 to use them.");
        }
        return false;
    }
    float lc[3];
    HolsterHipCentre(1, lc);   // holster_hip_from_calib: the calibrated belt centre, else holster_hip_center_*
    const holster::Params hip{ { lc[0], lc[1], lc[2] }, HolsterHipRadius };
    const holster::Params head{ { HolsterHeadCenterX, HolsterHeadCenterY, HolsterHeadCenterZ }, HolsterHeadRadius };
    const holster::Params chest{ { HolsterLChestCenterX, HolsterLChestCenterY, HolsterLChestCenterZ }, HolsterLChestRadius };
    const int hipMs = ClampMs(HolsterHipTapMs, 20, 450);   // the HEAL tap length (now fired by the chest)
    const int headMs = ClampMs(HolsterHeadTapMs, 20, 90);

    static bool s_armedLogged = false;
    if (!s_armedLogged) {
        s_armedLogged = true;
        // [PROBE: HOLSTER-L] gate=holster_hip_enable,holster_head_enable,holster_lchest_enable  site=HEADSET  log-only=yes
        LOG("holster: [HOLSTER-L] ARMED (first poll, logged unconditionally) | fires ON GRIP | chest %s centre (%.3f, %.3f, %.3f) "
            "r %.3f -> D-pad down 0x%02X (heal) tap %d ms | hip %s (the ammo POUCH, no tap; needs manual_reload, now %d) centre "
            "(%.3f, %.3f, %.3f) r %.3f | head %s centre (%.3f, %.3f, %.3f) r %.3f -> D-pad up 0x%02X tap %d ms | head-yaw frame "
            "+X right +Y up +Z behind, exit x%.1f | haptic %.2f (left hand) | bits 0x10 / 0x40 measured (INV-311 / INV-382)",
            chestOn ? "ON" : "off", chest.centre.x, chest.centre.y, chest.centre.z, chest.radius, kDpadDownBit, hipMs,
            hipOn ? "ON" : "off", ManualReload ? 1 : 0, hip.centre.x, hip.centre.y, hip.centre.z, hip.radius,
            headOn ? "ON" : "off", head.centre.x, head.centre.y, head.centre.z, head.radius, kDpadUpBit, headMs,
            holster::kExitScale, HolsterHaptic);
    }
    // Every left-hand pair whose exit spheres touch is REFUSED by name (a press could otherwise start in two zones).
    const char* overlap = nullptr;
    if (hipOn && headOn && holster::ZonesOverlap(hip, head)) { overlap = "the hip and head"; }
    else if (chestOn && hipOn && holster::ZonesOverlap(chest, hip)) { overlap = "the chest and hip"; }
    else if (chestOn && headOn && holster::ZonesOverlap(chest, head)) { overlap = "the chest and head"; }
    if (overlap) {
        static bool s_overlapLogged = false;
        if (!s_overlapLogged) {
            s_overlapLogged = true;
            // [PROBE: HOLSTER-L] gate=holster_hip_enable,holster_head_enable,holster_lchest_enable  site=HEADSET  log-only=yes
            LOG("holster: [HOLSTER-L] REFUSED - %s zones OVERLAP (exit spheres touch), so one press could fire both. No left "
                "zone is active; L1 is unchanged. Move holster_hip_center_* / holster_head_center_* / holster_lchest_center_* "
                "apart or shrink the radii.", overlap);
        }
        InterlockedExchange(&g_pouchHeld, 0);
        g_takeZoneWas = false;   // the crossbow take is refused with them (fails closed)
        InterlockedExchange(&g_leftShoulderIn, 0);
        BoltHoldOff(takeLive);
        g_leftRefusal = overlap;   // a string literal above
        return false;
    }
    g_leftRefusal = nullptr;

    const ULONGLONG now = GetTickCount64();
    ++g_lPolls;

    holster::Input in{};
    in.squeezeActive = GetControllerSqueeze(kLeftHand, in.squeeze);
    XrPosef hp{};
    XrPosef grip{};
    const bool headOk = GetHeadPose(hp);
    const bool gripOk = GetControllerGripPose(kLeftHand, grip);
    if (!headOk) { ++g_lNoHead; }
    if (!gripOk) { ++g_lNoGrip; }
    in.poseOk = headOk && gripOk;
    if (in.poseOk) {
        in.local = holster::LocalOffset({ hp.position.x, hp.position.y, hp.position.z }, hp.orientation.x,
                                        hp.orientation.y, hp.orientation.z, hp.orientation.w,
                                        { grip.position.x, grip.position.y, grip.position.z });
    }

    // Body-lock: the hip hangs off the body; the temple (flashlight) keeps the head-relative offset (Jay: "Temple for the
    // flashlight only").
    BodyFrameUpdate();
    holster::Input inHip = in;
    if (in.poseOk && hipOn) { BodyLocal(kZoneLHip, grip, inHip.local); }
    holster::Input inChest = in;
    if (in.poseOk && chestOn) { BodyLocal(kZoneLChest, grip, inChest.local); }
    // [HOLSTERPLACE] CHECK after a placement (review I1). Idle COST: three volatile reads. Zones are PLACEMENT indices here.
    if (in.poseOk && hipOn) { PlaceCheckPoll(holster::place::kLHip, inHip.local, hip.centre); }
    if (in.poseOk && headOn) { PlaceCheckPoll(holster::place::kHead, in.local, head.centre); }
    if (in.poseOk && chestOn) { PlaceCheckPoll(holster::place::kLChest, inChest.local, chest.centre); }

    // The crossbow's left-shoulder TAKE zone (plan 2026-11-03-manual-reload-crossbow Task 4): the LEFT hand against the right-shoulder
    // zone mirrored in x, in the SHOULDER's frame (its body latch when latched, else head-relative - ZoneLocalNoCount, which does not
    // touch the shoulder's 5 s counters) with its hysteresis. The right hand's shoulder Machine (weapon swap, the wheel hold) is not
    // touched. Ruling T0-b: holster::ZonesOverlap is NOT called on this pair (the exit spheres overlap 8 mm; the frames differ) - the
    // take owns the press through LeftChestFires instead. COST: not live = the one bool test below and nothing else; live = one
    // ZoneLocalNoCount (a few dozen flops) + one TakeZoneStep per poll.
    const holster::Params takeZone = holster::MirrorX({ { HolsterCenterX, HolsterCenterY, HolsterCenterZ }, HolsterRadius });
    holster::Vec3 takeLocal{ 0.0f, 0.0f, 0.0f };
    float takeDist = -1.0f;
    if (takeLive) {
        if (in.poseOk) {
            const float hy = holster::HeadYaw(hp.orientation.x, hp.orientation.y, hp.orientation.z, hp.orientation.w);
            takeLocal = ZoneLocalNoCount(g_bf, kZoneShoulder, { hp.position.x, hp.position.y, hp.position.z }, hy,
                                         { grip.position.x, grip.position.y, grip.position.z });
            const float dx = takeLocal.x - takeZone.centre.x, dy = takeLocal.y - takeZone.centre.y,
                        dz = takeLocal.z - takeZone.centre.z;
            takeDist = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        const bool takeWasIn = g_takeZoneWas;
        g_takeZoneWas = holster::TakeZoneStep(true, g_takeZoneWas, in.poseOk, takeLocal, takeZone);
        InterlockedExchange(&g_leftShoulderIn, g_takeZoneWas ? 1 : 0);
        // The quiver's ENTER pulse + edge lines (Jay after RUN XB1). COST: two bool compares per poll; a pulse / line per edge only.
        if (!takeWasIn && g_takeZoneWas) {
            g_lWindowHadEvent = true;
            const bool gripUp = !(in.squeezeActive && in.squeeze >= holster::kGripThreshold);
            if (g_takeEnterGate.Allow(now, kEdgeGapMs)) {
                // [PROBE: HOLSTER-L] gate=manual_reload  site=HEADSET  log-only=yes
                LOG("holster: [HOLSTER-L] ENTER quiver (crossbow take) | local (%.3f, %.3f, %.3f) dist %.3f r %.3f | grip %s -> %s | %ld "
                    "more ENTER(s) inside the 500 ms gate since the last line",
                    takeLocal.x, takeLocal.y, takeLocal.z, takeDist, takeZone.radius, gripUp ? "up" : "HELD",
                    gripUp ? "pulse" : "no pulse (the grip is already held)", g_takeEnterGate.TakeSkipped());
            }
            if (gripUp && g_placing == 0) { PulseLeft(HolsterHaptic * 0.5f, 20); }   // as the other left zones' enter pulse
        } else if (takeWasIn && !g_takeZoneWas) {
            g_lWindowHadEvent = true;
            if (g_takeExitGate.Allow(now, kEdgeGapMs)) {
                // [PROBE: HOLSTER-L] gate=manual_reload  site=HEADSET  log-only=yes
                LOG("holster: [HOLSTER-L] EXIT quiver (crossbow take) %s | local (%.3f, %.3f, %.3f) dist %.3f exit-r %.3f | %ld more "
                    "EXIT(s) inside the 500 ms gate since the last line",
                    in.poseOk ? "by distance" : "POSE LOST", takeLocal.x, takeLocal.y, takeLocal.z, takeDist,
                    takeZone.radius * holster::kExitScale, g_takeExitGate.TakeSkipped());
            }
        }
        if (!g_takeSaidLive) {
            g_takeSaidLive = true;
            // [PROBE: HOLSTER-L] gate=manual_reload  site=HEADSET  log-only=yes
            LOG("holster: [HOLSTER-L] CROSSBOW TAKE LIVE (first rise this session, logged unconditionally) | the LEFT hand against "
                "the right shoulder mirrored in x: centre (%.3f, %.3f, %.3f) r %.3f exit %.3f | the shoulder's frame (%s) | in the "
                "zone, a left press heals nothing, toggles no torch and is not the pouch (LeftChestFires) | now: dist %.3f (-1 = no pose) in %d",
                takeZone.centre.x, takeZone.centre.y, takeZone.centre.z, takeZone.radius, takeZone.radius * holster::kExitScale,
                (g_bf.anchorOk[kZoneShoulder] && g_bf.latch[kZoneShoulder].have) ? "body-locked" : "head-relative, not latched yet",
                takeDist, g_takeZoneWas ? 1 : 0);
        }
    } else if (g_takeZoneWas) {
        g_takeZoneWas = false;
        InterlockedExchange(&g_leftShoulderIn, 0);
    }
    inHip.takeLive = takeLive;
    inHip.takeZoneIn = g_takeZoneWas;
    inChest.takeLive = takeLive;
    inChest.takeZoneIn = g_takeZoneWas;
    // Ruling T4-a (review I1): the temple (torch) is gated like the chest (holster::LeftTempleFires, the same rule in the Machine).
    // Its own copy: in.local stays the temple's head-relative offset, and the normal-press line below still reads `in`.
    holster::Input inHead = in;
    inHead.takeLive = takeLive;
    inHead.takeZoneIn = g_takeZoneWas;
    // The take press: the grip's own edge (not a Machine's, so it is seen with every left zone off).
    const bool lDown = in.squeezeActive && in.squeeze >= holster::kGripThreshold;
    const bool takePress = lDown && !g_takePrevDown && takeLive && g_takeZoneWas;
    g_takePrevDown = lDown;
    // The crossbow bolt wheel's hold (plan 2026-11-04-crossbow-bolt-wheel ruling B4, REVISED after RUN BW1 - Jay: "Pulling bolt from
    // the shoulder can trigger the weapon wheel whilst pulling the bolt back to the crossbow" -> "Yes" to the fix): it STARTS with a take
    // press in the bolt holster zone and ENDS on a grip release, the take no longer live, or the hand leaving the zone BEFORE the bolt
    // wheel is open (an ordinary take carries the bolt away with the grip still down); once open, leaving the zone does not end it.
    // boltwheel::BoltHoldStep (tested). The take itself is untouched (spec C1 / C2). COST: a few bool tests + two interlocked ops a poll.
    g_boltHoldArmed = boltwheel::BoltHoldStep(g_boltHoldArmed, takePress, lDown, takeLive, g_takeZoneWas, g_boltWheelOpen != 0);
    InterlockedExchange(&g_boltHold, g_boltHoldArmed ? 1 : 0);
    InterlockedExchange(&g_boltTakeLive, takeLive ? 1 : 0);

    if (hipOn) { HipYieldAsk(g_lhipYield, 0, g_hip.machine, inHip); }   // the left HIP only - the temple never yields (spec §2.4)
    const holster::Step sh = PollZone(g_hip, hipOn, inHip, hip, hipMs, now, headOk, gripOk);
    const holster::Step sd = PollZone(g_head, headOn, inHead, head, headMs, now, headOk, gripOk);
    const holster::Step sc = PollZone(g_chest, chestOn, inChest, chest, hipMs, now, headOk, gripOk);
    if (hipOn) { HipYieldLog(g_lhipYield, "L", sh); }
    InterlockedExchange(&g_pouchHeld, (hipOn && g_hip.machine.state() == holster::Press::Holster) ? 1 : 0);
    const bool hipPending = hipOn && g_hip.machine.state() == holster::Press::Pending;
    if (sh.pressHolster || sd.pressHolster || sc.pressHolster || sh.pressNormal || sd.pressNormal || sc.pressNormal) {
        g_lPressStart = now;
    }
    if (takePress) {
        g_lWindowHadEvent = true;
        ++g_takePresses;
        // [PROBE: HOLSTER-L] gate=manual_reload  site=HEADSET  log-only=yes
        LOG("holster: [HOLSTER-L] PRESS at the LEFT SHOULDER - the crossbow take owns it: chest (heal) %s, hip (pouch) %s, head %s "
            "| take press #%ld | local (%.3f, %.3f, %.3f) dist %.3f r %.3f | chest dist %.3f (-1 = off or no pose) | squeeze %.2f",
            sc.pressHolster ? "FIRED" : "not fired", sh.pressHolster ? "FIRED" : "not fired",
            sd.pressHolster ? "FIRED" : "not fired", g_takePresses, takeLocal.x, takeLocal.y,
            takeLocal.z, takeDist, takeZone.radius, chestOn ? sc.dist : -1.0f, in.squeeze);
    }
    if (!takePress && !sh.pressHolster && !sd.pressHolster && !sc.pressHolster && !hipPending &&
        (sh.pressNormal || sd.pressNormal || sc.pressNormal)) {
        g_lWindowHadEvent = true;
        ++g_lPressNormal;
        // [PROBE: HOLSTER-L] gate=holster_hip_enable,holster_head_enable  site=HEADSET  log-only=yes
        LOG("holster: [HOLSTER-L] PRESS normal (L1 as before) | local hip (%.3f, %.3f, %.3f) head (%.3f, %.3f, %.3f) | dist hip "
            "%.3f head %.3f (-1 = zone off or no pose) | squeeze %.2f | head %s grip %s",
            inHip.local.x, inHip.local.y, inHip.local.z, in.local.x, in.local.y, in.local.z, hipOn ? sh.dist : -1.0f,
            headOn ? sd.dist : -1.0f, in.squeeze,
            headOk ? "valid" : "MISSING", gripOk ? "valid" : "MISSING");
    }
    const bool suppressL1 = sh.suppressR1 || sd.suppressR1 || sc.suppressR1;
    if (suppressL1) {
        ++g_lSuppressedPolls;
    }
    outDpadDown = TapTick(g_chest, now);   // heal (spec 2026-11-01 goal 2); the hip (pouch) never taps
    outDpadUp = TapTick(g_head, now);

    long hOk = 0;
    long hFail = 0;
    int hLast = 0;
    if (GetHapticStats(kLeftHand, hOk, hFail, hLast)) {
        if (!g_lFirstHapticResultLogged && (hOk + hFail) > 0) {
            g_lFirstHapticResultLogged = true;
            // [PROBE: HOLSTER-L] gate=holster_hip_enable,holster_head_enable  site=HEADSET  log-only=yes
            LOG("holster: [HOLSTER-L] FIRST HAPTIC RESULT (left) XrResult %d (ok %ld fail %ld)", hLast, hOk, hFail);
        }
        if (!g_lFirstHapticFailLogged && hFail > 0) {
            g_lFirstHapticFailLogged = true;
            // [PROBE: HOLSTER-L] gate=holster_hip_enable,holster_head_enable  site=HEADSET  log-only=yes
            LOG("holster: [HOLSTER-L] *** HAPTIC FAILED (left) *** XrResult %d - pulses are not reaching the "
                "controller", hLast);
        }
    }

    if (g_lWindowStart == 0) {
        g_lWindowStart = now;
    } else if (now - g_lWindowStart >= 10000) {
        if (g_lWindowHadEvent) {
            // [PROBE: HOLSTER-L] gate=holster_hip_enable,holster_head_enable  site=HEADSET  log-only=yes
            LOG("holster: [HOLSTER-L] window (session totals) | polls %ld | presses chest (heal) %ld hip (pouch) %ld head %ld | "
                "presses normal %ld | L1-suppressed polls %ld | no-head %ld no-grip %ld | haptic ok %ld fail %ld last %d, refused "
                "%ld",
                g_lPolls, g_chest.taps, g_hip.taps, g_head.taps, g_lPressNormal, g_lSuppressedPolls, g_lNoHead, g_lNoGrip, hOk,
                hFail, hLast, g_lPulseRefused);
        }
        g_lWindowStart = now;
        g_lWindowHadEvent = false;
    }

    outOwnsGrip = sh.ownsGrip || sd.ownsGrip || sc.ownsGrip;
    return suppressL1;
}

bool HolsterLeftPouchHeld() { return g_pouchHeld != 0; }
// The crossbow's left-shoulder take (Task 4). COST: one interlocked write / one volatile read.
void HolsterSetXbowTakeLive(bool on) { InterlockedExchange(&g_xbowTakeLive, on ? 1 : 0); }
bool HolsterLeftShoulderIn() { return g_leftShoulderIn != 0; }
const char* HolsterLeftTakeRefusal() { return g_leftRefusal; }

// =====================================================================================================
// THE HIP ZONES FROM THE CALIBRATION - Jay 2026-10-15: "Tuning, using the calibration" / "At the belt. Go about 10-15cm UP
// from the resting hand by side pose in the calibration data. Don't go further forward by more than 4cm" / radius: "leave the
// radius for now". The resting hand = the arm calibration's DOWN pose ("BOTH ARMS STRAIGHT DOWN BY YOUR SIDES"), its grip
// point measured in THIS file's frame (calibscreen.cpp, raw head, head yaw removed) and saved as r_/l_rest_hand_*_m.
// Only the CENTRE moves; radius, exit scale, press machine and outputs are unchanged.
// =====================================================================================================

// [DEP: holster_hip_from_calib] required-for=the hip zones placed from the calibration (HolsterHipFromCalib): the right hip
//      (item swap) and left hip (heal) centres = BeltCentre(that arm's resting hand). The LEFT centre is also the throwables'
//      ready zone (throwable.cpp reads HolsterHipCentre(1)). tree=INERT. DEFAULT ON 2026-10-15 - Tier A, VERIFIED by Jay
//      (hipcalib_run1, layer 4e19efc1: "Done, perfect"; right hip 29 fires / left 10 at the belt). Off, or a side without a
//      plausible rest hand, = the fixed holster_rhip_center_* / holster_hip_center_*, logged by name - so a player who has not
//      run the calibration screen since 2026-10-15 gets today's fixed zones. HEADSET (grip + head poses; the rest hand needs a
//      calibration-screen run). Reads no bones.
bool HolsterHipFromCalib = true;
// [DEP: holster_hip_belt_up_cm] required-for=how far above the resting hand the belt zone sits (HolsterHipBeltUpCm), cm,
//      clamped 10..15 at use (Jay: "10-15cm UP"). tree=INERT (only with holster_hip_from_calib=1). DEFAULT 12.5 = the middle.
float HolsterHipBeltUpCm = 12.5f;
// [DEP: holster_hip_belt_fwd_cm] required-for=how far forward of the resting hand the belt zone sits (HolsterHipBeltFwdCm),
//      cm, clamped 0..4 at use (Jay: "Don't go further forward by more than 4cm"). tree=INERT (only with
//      holster_hip_from_calib=1). DEFAULT 0.
float HolsterHipBeltFwdCm = 0.0f;

namespace {

// Per side: what the last log line said, so the source is logged once and on every change (never per poll). Callers: the polls
// (scePad filter thread) AND, since the place-by-hand plan (Task 3), the overlay's HolsterZoneParams / HolsterZoneWorld /
// HolsterRadiusMax / HolsterPlaceTick (render thread). These fields are plain, so two threads can at worst repeat or miss one
// [HIPCALIB] line - the centre returned is computed fresh on every call and is never wrong.
struct HipSource {
    bool  said = false;
    int   src = 99;          // 1 calibration, 2 switch off, 0 no calibration, -1 no rest hand, -2 rest hand implausible,
                             // 3 placed by hand (the fixed centre differs from the compiled default - the calibration is not read)
    long  seq = -1;          // the calibration's change counter at the last line
    float up = -1.0f, fwd = -1.0f;
    ULONGLONG dirtyAt = 0;   // a slider moved: logged once it has been still for 1 s (a drag is one line, not dozens)
};
HipSource g_hipSource[2];

}  // namespace

void HolsterHipCentre(int side, float out[3])
{
    side = side ? 1 : 0;
    const holster::Vec3 fixedC = side ? holster::Vec3{ HolsterHipCenterX, HolsterHipCenterY, HolsterHipCenterZ }
                                      : holster::Vec3{ HolsterRHipCenterX, HolsterRHipCenterY, HolsterRHipCenterZ };
    holster::Vec3 c = fixedC;
    holster::Vec3 rest{ 0.0f, 0.0f, 0.0f };
    long seq = -1;
    int src = 2;
    const float upCm = HolsterHipBeltUpCm;
    const float fwdCm = HolsterHipBeltFwdCm;
    // Placed by hand (plan 2026-10-30 Task 3 step 2, spec goal 5; no new key): a fixed centre that differs from the compiled default
    // by more than 0.5 mm on any axis was put there by the player, so it wins over the calibration - a re-run calibration does not
    // move it. A fixed centre AT the default keeps today's behaviour exactly. COST: six float compares.
    if (holster::place::PlacedByHand(side ? holster::place::kLHip : holster::place::kRHip, fixedC)) {
        src = 3;
    } else if (HolsterHipFromCalib) {
        float r3[3] = {};
        const int r = ArmFitRestHand(side, r3, &seq);
        if (r == 1) {
            rest = { r3[0], r3[1], r3[2] };
            if (holster::RestHandPlausible(rest, side == 1)) {
                c = holster::BeltCentre(rest, upCm, fwdCm);
                src = 1;
            } else {
                src = -2;
            }
        } else {
            src = r;   // 0 = no calibration, -1 = no rest hand in it
        }
    }
    out[0] = c.x;
    out[1] = c.y;
    out[2] = c.z;

    HipSource& h = g_hipSource[side];
    const ULONGLONG now = GetTickCount64();
    const bool srcChanged = !h.said || src != h.src || seq != h.seq;
    if (src == 1 && !srcChanged && (upCm != h.up || fwdCm != h.fwd)) {
        h.up = upCm;
        h.fwd = fwdCm;
        h.dirtyAt = now;
    }
    const bool settled = h.dirtyAt != 0 && now - h.dirtyAt >= 1000;
    if (!srcChanged && !settled) { return; }
    h.said = true;
    h.src = src;
    h.seq = seq;
    h.up = upCm;
    h.fwd = fwdCm;
    h.dirtyAt = 0;
    const char* sn = side ? "LEFT hip (heal + throwables' ready zone)" : "RIGHT hip (item swap)";
    if (src == 1) {
        // [PROBE: HIPCALIB] gate=holster_hip_from_calib  site=HEADSET  log-only=no
        LOG("holster: [HIPCALIB] %s centre (%.3f, %.3f, %.3f) m FROM THE CALIBRATION: resting hand (%.3f, %.3f, %.3f) + up "
            "%.1f cm, forward %.1f cm (asked %.1f / %.1f; clamped 10..15 / 0..4) | fixed centre would be (%.3f, %.3f, %.3f) | "
            "head-yaw frame +X right +Y up +Z behind | calibration #%ld",
            sn, c.x, c.y, c.z, rest.x, rest.y, rest.z, (c.y - rest.y) * 100.0f, (rest.z - c.z) * 100.0f, upCm, fwdCm,
            fixedC.x, fixedC.y, fixedC.z, seq);
    } else {
        static const char* const kWhy[6] = {
            "the rest hand read from the file is NOT a hand hanging at this side (5-55 cm out, 35-120 cm below the eyes, "
            "within 35 cm forward / back) - recalibrate",
            "this side's calibration has NO rest hand (dlvr\\arm_fit.txt written before 2026-10-15, or the down pose took too "
            "few raw samples) - run the calibration screen once ('Calibrate my arms' at the top of the menu)",
            "no valid arm calibration is loaded (dlvr\\arm_fit.txt missing or refused)",
            "",
            "holster_hip_from_calib=0",
            "the calibration is not read (this hip was placed; its centre back at the compiled default returns it to the "
            "calibration)" };
        const int wi = src + 2;   // -2 .. 3 -> 0 .. 5
        const holster::Vec3 dc = holster::place::DefaultCentre(side ? holster::place::kLHip : holster::place::kRHip);
        // [PROBE: HIPCALIB] gate=holster_hip_from_calib  site=HEADSET  log-only=no
        LOG("holster: [HIPCALIB] %s centre (%.3f, %.3f, %.3f) m = %s%s - %s | compiled default (%.3f, %.3f, %.3f)", sn, c.x, c.y,
            c.z, src == 3 ? "PLACED BY HAND (fixed centre differs from the default)" : "the FIXED setting",
            (src == 3 && HolsterHipFromCalib) ? " - overrides holster_hip_from_calib=1" : "",
            (wi >= 0 && wi <= 5) ? kWhy[wi] : "?", dc.x, dc.y, dc.z);
    }
}

// =====================================================================================================
// PLACING A HOLSTER BY HAND - plan docs/superpowers/plans/2026-10-30-holster-place-by-hand.md Task 3 (spec goal 2). The player
// presses Place for a zone, puts that zone's hand where the holster should be and pulls that hand's trigger; the hand's offset in
// the zone's OWN frame becomes the zone's centre. The rules (range, overlap, defaults, partner, hand) are holster_place_logic.h.
// THREADS: HolsterPlaceBegin / HolsterPlaceTick / HolsterPlaceDefault, HolsterZoneParams / HolsterRadiusMax /
// HolsterSameHandOverlap / HolsterZoneWorld run on the RENDER thread (the overlay); HolsterPoll / HolsterLeftPoll /
// BodyFrameUpdate on the scePad filter thread. Shared: g_placing (volatile LONG, either thread) and the body snapshot g_snap (read
// under g_snapLock, never g_bf). The centre floats are written with plain float stores, exactly as the overlay's sliders write
// them; the polls read them once per poll, so a poll sees the old or the new value of each axis (as with a slider drag).
// Persisting is the overlay's SaveSettingsIfChanged() - nothing here writes the file.
// =====================================================================================================

namespace {

constexpr float kPlaceTrigReleased = 0.4f;   // the trigger must read below this after Begin ...
constexpr float kPlaceTrigPulled = 0.6f;     // ... before reaching this counts as the pull
constexpr long  kMaxPlaceLines = 400;        // INV-9: human-rate events; the cap guards a page that loops Begin
const char* const kPlaceName[holster::place::kZones] = { "right shoulder", "right hip", "left hip (pouch)", "head (temple)",
                                                         "left chest (heal)" };

// Render thread only (Begin / Tick / Default).
struct PlaceState {
    int       zone = -1;
    ULONGLONG startMs = 0;
    bool      released = false;   // the trigger has read < kPlaceTrigReleased since Begin
};
PlaceState g_place;
long       g_placeLines = 0;

// The zone's frame: origin + yaw such that local = LocalOffsetYaw(origin, yaw, palm) and world = origin + ToWorldYaw(local, yaw).
// Zones 0-2: EXACTLY HolsterZoneLocal's frame (the snapshot under g_snapLock; fresh <= 250 ms and latched -> the virtual head from
// the caller's neck point and the snapshot's yaw), else the raw head + head yaw (= LocalOffset). Zone 3 (the temple): always the
// raw head + head yaw - the frame HolsterLeftPoll gives it (holster.cpp `in.local = holster::LocalOffset(hp ...)`), never
// body-locked. COST: one shared SRW lock, one NeckPoint, a few dozen flops.
// stale (review I2): the snapshot says the zone IS latched but is older than 250 ms or was published without a head pose - the
// polls' BodyLocal would use the latch (it has no freshness test) while this frame would fall back to the head, so a placement
// is refused instead (HolsterPlaceTick). ageMs = the snapshot's age, -1 = published invalid / never.
struct PlaceFrame {
    holster::Vec3 origin;
    float         yaw;
    bool          body;
    bool          stale;
    double        ageMs;
};
PlaceFrame PlaceFrameOf(int zone, const XrPosef& head)
{
    const holster::Vec3 eye{ head.position.x, head.position.y, head.position.z };
    const float qx = head.orientation.x, qy = head.orientation.y, qz = head.orientation.z, qw = head.orientation.w;
    PlaceFrame f{ eye, holster::HeadYaw(qx, qy, qz, qw), false, false, -1.0 };
    const int bz = BodyOfPlace(zone);   // `zone` is a PLACEMENT index; the temple (3) has no body latch
    if (bz < 0) { return f; }
    BodySnap s;
    AcquireSRWLockShared(&g_snapLock);
    s = g_snap;
    ReleaseSRWLockShared(&g_snapLock);
    const double age = s.atS >= 0.0 ? NowS() - s.atS : -1.0;
    f.ageMs = age < 0.0 ? -1.0 : age * 1000.0;
    if (s.latched[bz] && !(age >= 0.0 && age <= 0.25)) { f.stale = true; }
    if (s.atS >= 0.0 && NowS() - s.atS <= 0.25 && s.latched[bz]) {
        const XrVector3f nv = NeckOffsetM();
        const holster::Vec3 neck = holster::NeckPoint(eye, qx, qy, qz, qw, { nv.x, nv.y, nv.z });
        // 2026-11-16: anchor = the neck + the published anchor - neck (the hip point for the hips; 0 otherwise).
        const holster::Vec3 an{ neck.x + s.anchorFromNeck[bz].x, neck.y + s.anchorFromNeck[bz].y, neck.z + s.anchorFromNeck[bz].z };
        holster::Latch L;
        L.have = true;
        L.eyeFromAnchor = s.eyeFromAnchor[bz];
        f.origin = holster::VirtualHead(an, s.yaw, L);
        f.yaw = s.yaw;
        f.body = true;
    }
    return f;
}

// The three stored centre floats of a zone (the settings the sliders write), and its radius.
void PlaceCentreRefs(int zone, float*& x, float*& y, float*& z, float*& r)
{
    switch (zone) {
        case holster::place::kRHip: x = &HolsterRHipCenterX; y = &HolsterRHipCenterY; z = &HolsterRHipCenterZ; r = &HolsterRHipRadius; break;
        case holster::place::kLHip: x = &HolsterHipCenterX; y = &HolsterHipCenterY; z = &HolsterHipCenterZ; r = &HolsterHipRadius; break;
        case holster::place::kHead: x = &HolsterHeadCenterX; y = &HolsterHeadCenterY; z = &HolsterHeadCenterZ; r = &HolsterHeadRadius; break;
        case holster::place::kLChest: x = &HolsterLChestCenterX; y = &HolsterLChestCenterY; z = &HolsterLChestCenterZ; r = &HolsterLChestRadius; break;
        default:                    x = &HolsterCenterX; y = &HolsterCenterY; z = &HolsterCenterZ; r = &HolsterRadius; break;
    }
}

// Which axes are outside the zone's slider range, for the REFUSED line: "x +0.912 m outside -0.80..+0.80" (each failing axis).
void PlaceRangeText(int zone, const holster::Vec3& v, char* buf, size_t n)
{
    const holster::place::Range rg = holster::place::RangeOf(zone);
    const float a[3] = { v.x, v.y, v.z };
    const float lo[3] = { rg.lo.x, rg.lo.y, rg.lo.z };
    const float hi[3] = { rg.hi.x, rg.hi.y, rg.hi.z };
    static const char* const kAxis[3] = { "x (right)", "y (up)", "z (behind)" };
    size_t used = 0;
    buf[0] = '\0';
    for (int k = 0; k < 3 && used < n; ++k) {
        int w = 0;
        if (!std::isfinite(a[k])) {
            w = _snprintf_s(buf + used, n - used, _TRUNCATE, "%s%s is not a number", used ? "; " : "", kAxis[k]);
        } else if (a[k] < lo[k] || a[k] > hi[k]) {
            w = _snprintf_s(buf + used, n - used, _TRUNCATE, "%s%s %+.3f m outside %+.2f..%+.2f", used ? "; " : "", kAxis[k],
                            a[k], lo[k], hi[k]);
        }
        if (w < 0) { break; }   // truncated
        used += static_cast<size_t>(w);
    }
    if (used == 0) { _snprintf_s(buf, n, _TRUNCATE, "no axis outside the range (?)"); }
}

// One numbered [HOLSTERPLACE] line: false once the session cap is reached. first = this is the session's first line.
bool PlaceLineAllowed(long& k)
{
    k = ++g_placeLines;
    return k <= kMaxPlaceLines;
}

void PlaceEnd()
{
    g_place.zone = -1;
    g_place.released = false;
    InterlockedExchange(&g_placing, 0);
}

}  // namespace

bool HolsterZoneParams(int zone, float centre[3], float* radius, bool* enabled)
{
    if (zone < 0 || zone >= holster::place::kZones) { return false; }
    float r = 0.0f;
    bool on = false;
    switch (zone) {
        case holster::place::kShoulder:
            centre[0] = HolsterCenterX; centre[1] = HolsterCenterY; centre[2] = HolsterCenterZ;
            r = HolsterRadius; on = HolsterEnable;
            break;
        case holster::place::kRHip:
            HolsterHipCentre(0, centre);   // the effective centre: placed by hand, the calibration, or the fixed setting
            r = HolsterRHipRadius; on = HolsterRHipEnable;
            break;
        case holster::place::kLHip:
            HolsterHipCentre(1, centre);
            r = HolsterHipRadius; on = HolsterHipEnable && ManualReload;   // the pouch: live only with manual reload
            break;
        case holster::place::kLChest:
            centre[0] = HolsterLChestCenterX; centre[1] = HolsterLChestCenterY; centre[2] = HolsterLChestCenterZ;
            r = HolsterLChestRadius; on = HolsterLChestEnable;
            break;
        default:
            centre[0] = HolsterHeadCenterX; centre[1] = HolsterHeadCenterY; centre[2] = HolsterHeadCenterZ;
            r = HolsterHeadRadius; on = HolsterHeadEnable;
            break;
    }
    if (radius) { *radius = r; }
    if (enabled) { *enabled = on; }
    return true;
}

float HolsterRadiusMax(int zone)
{
    if (zone < 0 || zone >= holster::place::kZones) { return 0.0f; }
    float c[3];
    HolsterZoneParams(zone, c, nullptr, nullptr);
    int pz[2] = { -1, -1 };
    const int n = holster::place::PartnersOf(zone, pz);   // EVERY same-hand zone (the left hand has three)
    holster::Params ps[2] = {};
    bool on[2] = {};
    for (int i = 0; i < n; ++i) {
        float pc[3], pr = 0.0f;
        HolsterZoneParams(pz[i], pc, &pr, &on[i]);
        ps[i] = holster::Params{ { pc[0], pc[1], pc[2] }, pr };
    }
    const holster::place::Range rg = holster::place::RangeOf(zone);
    // Ruling R5: 1 mm under the true limit, so a slider parked at its cap never reads as overlapping by one ulp.
    float r = holster::place::RadiusMaxAll(zone, { c[0], c[1], c[2] }, ps, on, n) - 0.001f;
    if (r < rg.rMin) { r = rg.rMin; }
    return r;
}

bool HolsterSameHandOverlap(int hand)
{
    // Every same-hand pair: right = shoulder / right hip; left = hip / head / chest (spec 2026-11-01 goal 2).
    static const int kRight[1][2] = { { holster::place::kShoulder, holster::place::kRHip } };
    static const int kLeft[3][2] = { { holster::place::kLHip, holster::place::kHead },
                                     { holster::place::kLChest, holster::place::kLHip },
                                     { holster::place::kLChest, holster::place::kHead } };
    const int (*pairs)[2] = hand ? kLeft : kRight;
    const int np = hand ? 3 : 1;
    for (int i = 0; i < np; ++i) {
        float ac[3], bc[3], ar = 0.0f, br = 0.0f;
        bool aOn = false, bOn = false;
        HolsterZoneParams(pairs[i][0], ac, &ar, &aOn);
        HolsterZoneParams(pairs[i][1], bc, &br, &bOn);
        if (aOn && bOn &&
            holster::ZonesOverlap(holster::Params{ { ac[0], ac[1], ac[2] }, ar }, holster::Params{ { bc[0], bc[1], bc[2] }, br })) {
            return true;
        }
    }
    return false;
}

bool HolsterZoneWorld(int zone, float out[3])
{
    if (zone < 0 || zone >= holster::place::kZones) { return false; }
    XrPosef head{};
    if (!GetHeadPose(head)) { return false; }
    float c[3];
    HolsterZoneParams(zone, c, nullptr, nullptr);
    const PlaceFrame f = PlaceFrameOf(zone, head);
    const holster::Vec3 w = holster::ToWorldYaw({ c[0], c[1], c[2] }, f.yaw);
    out[0] = f.origin.x + w.x;
    out[1] = f.origin.y + w.y;
    out[2] = f.origin.z + w.z;
    return true;
}

bool HolsterPlacing() { return g_placing != 0; }

void HolsterPlaceBegin(int zone)
{
    long k = 0;
    if (zone < 0 || zone >= holster::place::kZones) {
        if (PlaceLineAllowed(k)) {
            // [PROBE: HOLSTERPLACE] gate=holster_enable,holster_rhip_enable,holster_hip_enable,holster_head_enable  site=HEADSET  log-only=no
            LOG("holster: [HOLSTERPLACE] BEGIN IGNORED - zone %d is not 0..4%s", zone,
                k == 1 ? " | first line, logged unconditionally" : "");
        }
        return;
    }
    if (g_placing != 0) {
        if (PlaceLineAllowed(k)) {
            // [PROBE: HOLSTERPLACE] gate=holster_enable,holster_rhip_enable,holster_hip_enable,holster_head_enable  site=HEADSET  log-only=no
            LOG("holster: [HOLSTERPLACE] BEGIN %s IGNORED - placing the %s is still running%s", kPlaceName[zone],
                (g_place.zone >= 0 && g_place.zone < holster::place::kZones) ? kPlaceName[g_place.zone] : "?",
                k == 1 ? " | first line, logged unconditionally" : "");
        }
        return;
    }
    g_place.zone = zone;
    g_place.startMs = GetTickCount64();
    g_place.released = false;
    InterlockedExchange(&g_placing, 1);
    float c[3], r = 0.0f;
    bool on = false;
    HolsterZoneParams(zone, c, &r, &on);
    const int hand = holster::place::HandOf(zone);
    float t = -1.0f;
    const bool tOk = GetControllerTrigger(hand ? kLeftHand : kRightHand, t);
    if (PlaceLineAllowed(k)) {
        // [PROBE: HOLSTERPLACE] gate=holster_enable,holster_rhip_enable,holster_hip_enable,holster_head_enable  site=HEADSET  log-only=no
        LOG("holster: [HOLSTERPLACE] BEGIN %s (zone %d, %s) | %s hand's trigger %s %.2f - released below %.1f then pulled to %.1f "
            "places it | centre now (%.3f, %.3f, %.3f) r %.3f | the facing is held while placing%s",
            kPlaceName[zone], zone, on ? "ON" : "off", hand ? "LEFT" : "RIGHT", tOk ? "reads" : "NOT READABLE", t,
            kPlaceTrigReleased, kPlaceTrigPulled, c[0], c[1], c[2], r, k == 1 ? " | first line, logged unconditionally" : "");
    }
}

// COST while not placing: one volatile read. While placing: one trigger read per frame; the pull adds two pose reads, one shared
// SRW lock, up to two HolsterHipCentre calls and a few dozen flops, once.
int HolsterPlaceTick(bool menuEdge)
{
    if (g_placing == 0) { return -1; }
    const int zone = g_place.zone;
    if (zone < 0 || zone >= holster::place::kZones) { PlaceEnd(); return kPlaceCancelled; }   // cannot happen; never leave it set
    const char* const zn = kPlaceName[zone];
    long k = 0;
    const unsigned long long heldMs = static_cast<unsigned long long>(GetTickCount64() - g_place.startMs);
    if (menuEdge) {
        PlaceEnd();
        if (PlaceLineAllowed(k)) {
            // [PROBE: HOLSTERPLACE] gate=holster_enable,holster_rhip_enable,holster_hip_enable,holster_head_enable  site=HEADSET  log-only=no
            LOG("holster: [HOLSTERPLACE] CANCELLED %s - the menu button, after %llu ms; the centre is unchanged%s", zn, heldMs,
                k == 1 ? " | first line, logged unconditionally" : "");
        }
        return kPlaceCancelled;
    }
    const int handIx = holster::place::HandOf(zone);
    const Hand hand = handIx ? kLeftHand : kRightHand;
    float t = 0.0f;
    if (!GetControllerTrigger(hand, t)) { return -1; }   // unreadable: neither released nor pulled - keep waiting
    if (!g_place.released) {
        if (t < kPlaceTrigReleased) { g_place.released = true; }
        return -1;
    }
    if (t < kPlaceTrigPulled) { return -1; }

    // The pull. The hand's offset in the zone's own frame, against the partner's EFFECTIVE params now.
    XrPosef head{}, grip{};
    const bool headOk = GetHeadPose(head);
    const bool gripOk = GetControllerGripPose(hand, grip);
    const long gf = ControllerGripFlags(hand);
    const bool posTracked = gf >= 0 && (gf & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) != 0;
    const bool tracked = headOk && gripOk && posTracked;
    float c[3], r = 0.0f, pc[3] = {}, pr = 0.0f;
    bool on = false;
    HolsterZoneParams(zone, c, &r, &on);
    // EVERY same-hand zone (the left hand has three since 2026-11-01); pz / pc / pr name the one that overlaps, for the line.
    int pzs[2] = { -1, -1 };
    const int np = holster::place::PartnersOf(zone, pzs);
    holster::Params partners[2] = {};
    bool pOns[2] = {};
    for (int i = 0; i < np; ++i) {
        float q[3], qr = 0.0f;
        HolsterZoneParams(pzs[i], q, &qr, &pOns[i]);
        partners[i] = holster::Params{ { q[0], q[1], q[2] }, qr };
    }
    int pz = pzs[0];
    PlaceFrame f{ { 0.0f, 0.0f, 0.0f }, 0.0f, false, false, -1.0 };
    holster::Vec3 local{ 0.0f, 0.0f, 0.0f };
    if (headOk) {
        f = PlaceFrameOf(zone, head);
        local = holster::LocalOffsetYaw(f.origin, f.yaw, { grip.position.x, grip.position.y, grip.position.z });
    }
    holster::place::Verdict v = holster::place::CheckAll(zone, tracked, local, r, partners, pOns, np);
    for (int i = 0; i < np; ++i) {   // which partner it overlaps (for the REFUSED line)
        if (pOns[i] && holster::ZonesOverlap(holster::Params{ local, r }, partners[i])) {
            pz = pzs[i];
            pc[0] = partners[i].centre.x; pc[1] = partners[i].centre.y; pc[2] = partners[i].centre.z;
            pr = partners[i].radius;
            break;
        }
    }
    // Review I2: a latched zone whose body frame is not updating - refused as kPlaceNotRunning (its own page text), named.
    const bool staleRefuse = v == holster::place::Verdict::Ok && f.stale;
    if (staleRefuse) { v = holster::place::Verdict::Untracked; }
    const char* const frame = zone == holster::place::kHead ? "head (the temple is always head-relative)"
                              : (f.body ? "body (latched; the facing held while placing)"
                              : (f.stale ? "STALE body frame (latched, but the snapshot is not fresh)"
                                         : "head (this zone is not latched to the body yet - head-relative, as its poll uses now)"));
    const char* const first = " | first line, logged unconditionally";

    if (v != holster::place::Verdict::Ok) {
        PlaceEnd();
        int res = kPlaceUntracked;
        char why[384];
        if (staleRefuse) {
            res = kPlaceNotRunning;
            // What the render thread can tell about why neither poll ran BodyFrameUpdate (their early returns, read-only here).
            const bool rAny = HolsterEnable || HolsterRHipEnable;
            const bool lAny = (HolsterHipEnable && ManualReload) || HolsterHeadEnable || HolsterLChestEnable;
            const char* const rWhy = !rAny ? "both right zones off (HolsterPoll returns at its first line)"
                                   : (HolsterSameHandOverlap(0) ? "the shoulder and right hip overlap (HolsterPoll refuses)"
                                                                : "should run - unless vr_grip=0");
            const char* const lWhy = !lAny ? "every left zone off (HolsterLeftPoll returns at its first line)"
                                   : (HolsterSameHandOverlap(1) ? "two left zones overlap (HolsterLeftPoll refuses)"
                                                                : "should run - unless vr_grip=0");
            if (f.ageMs < 0.0) {
                _snprintf_s(why, sizeof(why), _TRUNCATE,
                            "the body frame is not updating (the last snapshot had no head pose) - right poll: %s; left poll: %s",
                            rWhy, lWhy);
            } else {
                _snprintf_s(why, sizeof(why), _TRUNCATE, "the body frame is not updating (stale %.0f ms) - right poll: %s; left poll: %s",
                            f.ageMs, rWhy, lWhy);
            }
        } else if (v == holster::place::Verdict::Untracked) {
            _snprintf_s(why, sizeof(why), _TRUNCATE, "controller not tracked (head pose %s, grip pose %s, grip position %s, flags %ld)",
                        headOk ? "ok" : "MISSING", gripOk ? "ok" : "MISSING", posTracked ? "tracked" : "NOT TRACKED", gf);
        } else if (v == holster::place::Verdict::OutOfRange) {
            res = kPlaceOutOfRange;
            char ax[192];
            PlaceRangeText(zone, local, ax, sizeof(ax));
            _snprintf_s(why, sizeof(why), _TRUNCATE, "outside the slider range: %s", ax);
        } else {
            res = kPlaceOverlaps;
            const float dx = local.x - pc[0], dy = local.y - pc[1], dz = local.z - pc[2];
            _snprintf_s(why, sizeof(why), _TRUNCATE,
                        "overlaps the %s zone (exit spheres touch: centres %.3f m apart, reach (%.3f + %.3f) x %.1f)", kPlaceName[pz],
                        std::sqrt(dx * dx + dy * dy + dz * dz), r, pr, holster::kExitScale);
        }
        if (PlaceLineAllowed(k)) {
            // [PROBE: HOLSTERPLACE] gate=holster_enable,holster_rhip_enable,holster_hip_enable,holster_head_enable  site=HEADSET  log-only=no
            LOG("holster: [HOLSTERPLACE] REFUSED %s - %s | hand local (%.3f, %.3f, %.3f) | centre kept (%.3f, %.3f, %.3f) | frame %s | "
                "trigger %.2f after %llu ms%s",
                zn, why, local.x, local.y, local.z, c[0], c[1], c[2], frame, t, heldMs, k == 1 ? first : "");
        }
        return res;
    }

    float *px = nullptr, *py = nullptr, *pzp = nullptr, *pr2 = nullptr;
    PlaceCentreRefs(zone, px, py, pzp, pr2);
    const float fixedWas[3] = { *px, *py, *pzp };
    *px = local.x;    // plain float stores, as the overlay's sliders write these settings (see the block comment)
    *py = local.y;
    *pzp = local.z;
    float nc[3];
    HolsterZoneParams(zone, nc, nullptr, nullptr);   // the EFFECTIVE centre now (a hip: through HolsterHipCentre's rule)
    const float ex = local.x - nc[0], ey = local.y - nc[1], ez = local.z - nc[2];
    const float distAfterMm = std::sqrt(ex * ex + ey * ey + ez * ez) * 1000.0f;
    InterlockedExchange(&g_placeCheck[zone], 2);   // the poll's own CHECK line (review I1), after the stores above
    PlaceEnd();
    if (handIx) { PulseLeft(HolsterHaptic, 40); } else { Pulse(HolsterHaptic, 40); }
    if (PlaceLineAllowed(k)) {
        // [PROBE: HOLSTERPLACE] gate=holster_enable,holster_rhip_enable,holster_hip_enable,holster_head_enable  site=HEADSET  log-only=no
        LOG("holster: [HOLSTERPLACE] STORED %s centre (%.3f, %.3f, %.3f) was (%.3f, %.3f, %.3f) (fixed setting was (%.3f, %.3f, "
            "%.3f)) | read-back %.1f mm (self-check, 0 by construction - NOT evidence; the poll's CHECK line is%s) | frame %s | "
            "zone %s, r %.3f | trigger %.2f after %llu ms%s",
            zn, local.x, local.y, local.z, c[0], c[1], c[2], fixedWas[0], fixedWas[1], fixedWas[2], distAfterMm,
            distAfterMm > 1.0f ? "; NOT 0: the stored value is not the one in use" : "", frame, on ? "ON" : "off", r, t, heldMs,
            k == 1 ? first : "");
    }
    return kPlaceStored;
}

void HolsterPlaceDefault(int zone)
{
    if (zone < 0 || zone >= holster::place::kZones) { return; }
    float *px = nullptr, *py = nullptr, *pz = nullptr, *pr = nullptr;
    PlaceCentreRefs(zone, px, py, pz, pr);
    const float was[4] = { *px, *py, *pz, *pr };
    const holster::Vec3 d = holster::place::DefaultCentre(zone);
    *px = d.x;   // plain float stores, as the sliders write them
    *py = d.y;
    *pz = d.z;
    *pr = holster::place::DefaultRadius(zone);
    long k = 0;
    if (PlaceLineAllowed(k)) {
        // [PROBE: HOLSTERPLACE] gate=holster_enable,holster_rhip_enable,holster_hip_enable,holster_head_enable  site=HEADSET  log-only=no
        LOG("holster: [HOLSTERPLACE] DEFAULT %s centre (%.3f, %.3f, %.3f) r %.3f, was (%.3f, %.3f, %.3f) r %.3f%s%s", kPlaceName[zone],
            d.x, d.y, d.z, *pr, was[0], was[1], was[2], was[3],
            (zone == holster::place::kRHip || zone == holster::place::kLHip) ? " | a hip follows holster_hip_from_calib again (when on and calibrated)" : "",
            k == 1 ? " | first line, logged unconditionally" : "");
    }
}

// =====================================================================================================
// HIPCAP - the hip-anchor redesign's standing recording (trace docs/traces/20261116_1400_hipcap_probe.md; Jay 2026-11-16: "I'd
// like it to be bulletproof" -> "I'll do it"). Every archived capture is seated (D-330); this one is replayed offline before any anchor
// build. Log-only: one line per located display time to dlvr_hipcap.log, beside dlvr_ovr2xr.log.
// [DEP: diag_hip_capture] required-for=[HIPCAP] the standing recording the hip-anchor redesign is replayed on (DiagHipCapture): the
//      head, both GRIP poses with squeeze / trigger, the appSpace base (roomscale / recentre shift appSpace itself) and the body
//      snapshot (facing yaw, latches), per located frame. tree=INERT (log-only: writes nothing the game or the layer reads). HEADSET
//      (a tracked head and grips). DEFAULT OFF - a diagnostic, never shipped on.
bool DiagHipCapture = false;

namespace {
volatile LONG64 g_hcLastT = 0;           // the last display time written (LocateViews re-locates the same time on workers)
volatile LONG   g_hcLines = 0;
volatile LONG64 g_hcRepeat = 0;          // repeated display times skipped
volatile LONG64 g_hcInvalid = 0;         // locates without both valid bits
volatile LONG   g_hcSaidFirst = 0, g_hcSaidWait = 0, g_hcSaidCap = 0;
volatile LONG64 g_hcNextSummaryMs = 0;
constexpr LONG  kHcMaxLines = 150000;    // ~28 min at 90 Hz (INV-9)
}  // namespace

void HolsterHipCapture(long long t, bool headValid, const float hp[3], const float hq[4], const float base[3], float baseYawDeg)
{
    if (!DiagHipCapture) { return; }
    const ULONGLONG nowMs = GetTickCount64();
    const LONG64 due = g_hcNextSummaryMs;
    if (due == 0) {
        InterlockedCompareExchange64(&g_hcNextSummaryMs, static_cast<LONG64>(nowMs + 60000), 0);
    } else if (static_cast<LONG64>(nowMs) >= due &&
               InterlockedCompareExchange64(&g_hcNextSummaryMs, static_cast<LONG64>(nowMs + 60000), due) == due) {
        const LONG written = g_hcLines < kHcMaxLines ? g_hcLines : kHcMaxLines;
        // [PROBE: HIPCAP] gate=diag_hip_capture  site=HEADSET  log-only=yes
        LOG("holster: [HIPCAP] 60 s | lines written to dlvr_hipcap.log %ld (cap %ld) | repeated display times skipped %lld | "
            "locates with the head not tracked %lld", static_cast<long>(written), static_cast<long>(kHcMaxLines),
            static_cast<long long>(g_hcRepeat), static_cast<long long>(g_hcInvalid));
    }
    if (!headValid) {
        InterlockedIncrement64(&g_hcInvalid);
        if (InterlockedExchange(&g_hcSaidWait, 1) == 0) {
            // [PROBE: HIPCAP] gate=diag_hip_capture  site=HEADSET  log-only=yes
            LOG("holster: [HIPCAP] WAITING - diag_hip_capture=1 but the head is not tracked yet (a locate without both valid bits) - "
                "no line is written to dlvr_hipcap.log until it is (logged once)");
        }
        return;
    }
    if (InterlockedExchange64(&g_hcLastT, static_cast<LONG64>(t)) == static_cast<LONG64>(t)) {
        InterlockedIncrement64(&g_hcRepeat);
        return;
    }
    const LONG n = InterlockedIncrement(&g_hcLines);
    if (n > kHcMaxLines) {
        if (InterlockedExchange(&g_hcSaidCap, 1) == 0) {
            // [PROBE: HIPCAP] gate=diag_hip_capture  site=HEADSET  log-only=yes
            LOG("holster: [HIPCAP] CAP - %ld lines written to dlvr_hipcap.log; the capture stops for this launch (INV-9) - restart "
                "the game to record again", static_cast<long>(kHcMaxLines));
        }
        return;
    }
    // [0] = LEFT, [1] = RIGHT. Located just before this, same display time and appSpace (ControllersLocate). -1 = not read.
    XrPosef g[2] = {};
    bool gOk[2] = { false, false };
    float sq[2] = { -1.0f, -1.0f }, tr[2] = { -1.0f, -1.0f };
    for (int h = 0; h < 2; ++h) {
        const Hand hand = h ? kRightHand : kLeftHand;
        gOk[h] = GetControllerGripPose(hand, g[h]);
        if (!GetControllerSqueeze(hand, sq[h])) { sq[h] = -1.0f; }
        if (!GetControllerTrigger(hand, tr[h])) { tr[h] = -1.0f; }
    }
    BodySnap s;
    AcquireSRWLockShared(&g_snapLock);
    s = g_snap;
    ReleaseSRWLockShared(&g_snapLock);
    const double ageMs = s.atS >= 0.0 ? (NowS() - s.atS) * 1000.0 : -1.0;
    const holster::Vec3& eS = s.eyeFromAnchor[kZoneShoulder];
    const holster::Vec3& eR = s.eyeFromAnchor[kZoneRHip];
    const holster::Vec3& eL = s.eyeFromAnchor[kZoneLHip];
    // Fields: display time ns | b = appSpace base x y z, yaw deg | h = head x y z, qx qy qz qw (appSpace) | L / R = grip valid,
    // x y z, qx qy qz qw, squeeze, trigger | s = snapshot age ms, facing yaw rad | z = latched + eyeFromAnchor: shoulder, right hip,
    // left hip.
    LogTo("dlvr_hipcap.log",
          "%lld|b %.4f %.4f %.4f %.3f|h %.4f %.4f %.4f %.5f %.5f %.5f %.5f|"
          "L %d %.4f %.4f %.4f %.5f %.5f %.5f %.5f %.3f %.3f|R %d %.4f %.4f %.4f %.5f %.5f %.5f %.5f %.3f %.3f|"
          "s %.1f %.5f|z %d %.4f %.4f %.4f %d %.4f %.4f %.4f %d %.4f %.4f %.4f",
          t, base[0], base[1], base[2], baseYawDeg, hp[0], hp[1], hp[2], hq[0], hq[1], hq[2], hq[3],
          gOk[0] ? 1 : 0, g[0].position.x, g[0].position.y, g[0].position.z, g[0].orientation.x, g[0].orientation.y,
          g[0].orientation.z, g[0].orientation.w, sq[0], tr[0],
          gOk[1] ? 1 : 0, g[1].position.x, g[1].position.y, g[1].position.z, g[1].orientation.x, g[1].orientation.y,
          g[1].orientation.z, g[1].orientation.w, sq[1], tr[1],
          ageMs, s.yaw,
          s.latched[kZoneShoulder] ? 1 : 0, eS.x, eS.y, eS.z, s.latched[kZoneRHip] ? 1 : 0, eR.x, eR.y, eR.z,
          s.latched[kZoneLHip] ? 1 : 0, eL.x, eL.y, eL.z);
    if (InterlockedExchange(&g_hcSaidFirst, 1) == 0) {
        // [PROBE: HIPCAP] gate=diag_hip_capture  site=HEADSET  log-only=yes
        LOG("holster: [HIPCAP] FIRST SAMPLE (logged unconditionally) -> dlvr_hipcap.log beside this log, one line per located frame "
            "| t %lld | base (%.3f, %.3f, %.3f) yaw %.1f deg | head (%.3f, %.3f, %.3f) | grips valid L %d R %d | snapshot age %.0f ms, "
            "facing %.1f deg, latched shoulder %d right hip %d left hip %d", t, base[0], base[1], base[2], baseYawDeg, hp[0], hp[1],
            hp[2], gOk[0] ? 1 : 0, gOk[1] ? 1 : 0, ageMs, s.yaw * 57.2957795f, s.latched[kZoneShoulder] ? 1 : 0,
            s.latched[kZoneRHip] ? 1 : 0, s.latched[kZoneLHip] ? 1 : 0);
    }
}

}  // namespace dlvr
