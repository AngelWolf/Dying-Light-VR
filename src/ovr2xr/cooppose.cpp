// cooppose.cpp - the co-op arm-pose channel's SENDER ([COOPPOSE]). See cooppose.h; plan 2026-11-13-coop-arm-pose-channel Task 4;
// trace docs/traces/20261113_1850_coop_sender.md.
//
// The frame (trace, "THE SEND FRAME"): the solve maps tracking -> world as T = anchor + Ry(bodyYaw) x (ctl - headRep). The sample
// carries Ry(bodyYaw - rootYaw) x (ctl - headRep) and qY(bodyYaw - rootYaw) x ctl, so a receiver that rebuilds with ITS anchor and
// ITS root yaw gets the local formula back (coopnet_logic_test BodyFrameMatchesSolve). The head's rotation goes the same way; the head
// position is the eye anchor relative to the root (world, root yaw removed) - informational for the receiver.
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

// The EGL rva of CorrectItemPosition (the receiver's item re-place, INV-736 (b)): the BETA tree translates it; the DEV tree has no
// buildrva.h - Steam rvas only (camhook.cpp / bodygrab.cpp's pattern).
#if __has_include("../common/buildrva.h")
#include "../common/buildrva.h"
#define DLVR_COOP_GD_RVA(rva) ::dlvr::buildrva::Gd(rva)
#else
#define DLVR_COOP_GD_RVA(rva) (rva)
#endif
#include "../common/log.h"
#include "boneblend.h"     // RightHandItemKind, HeldItemNow, RigGamePlayerObject
#include "camhook.h"       // HeadYawDecoupleFlipLatched - the sign of the fold's yaw (INV-736 (c))
#include "controllers.h"   // GetControllerFingerWeights, kLeftHand / kRightHand
#include "coopik_logic.h"  // the receiver's pure solve + HeadBeyondLook (spec 8.4-8.8)
#include "coopnet.h"       // CoopPose, CoopNetQueueSend, CoopNetNowMs, CoopNetBuildId, CoopNetPeers, CoopNetLatest, coop::
#include "cooppose.h"
#include "headpose.h"      // LastReportedHead - the head the engine was given (spec 8.8)
#include "periodic_log.h"  // LOG_PERIODIC - the receiver's 5 s line
#include "reload.h"        // ReloadLockOn
#include "twohand.h"       // TwoHandGripping
#include "uipin.h"         // GamePausedForMenu - the pause signal pause_freeze uses

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

namespace dlvr {

namespace {

constexpr ULONGLONG kStaleMs = 500;   // a hand not published for this long is sent as "no data" (handState bit 7)

// handState bits (plan Task 4 ruling; the receiver, Task 6, reads them)
constexpr uint8_t kHsKindMask   = 0x03;   // 0 empty / 1 WeaponVis (melee) / 2 any other item / 3 not known
constexpr uint8_t kHsTwoHand    = 0x04;   // LEFT: gripping a two-handed weapon (TwoHandGripping)
constexpr uint8_t kHsReloadLock = 0x08;   // LEFT: a manual-reload step owns the hand (ReloadLockOn)
constexpr uint8_t kHsNoData     = 0x80;   // no solve published this hand within kStaleMs
static_assert(kHsKindMask == coopik::kKindMask && kHsTwoHand == coopik::kTwoHand && kHsReloadLock == coopik::kReloadLock &&
              kHsNoData == coopik::kNoData, "the sender's handState bits must be the receiver's (coopik_logic.h)");

struct Store {
    float     head[7];       // anchor rel. root (root yaw removed) + head rotation in the body frame
    float     hand[2][7];    // [0] left, [1] right - body frame
    ULONGLONG handMs[2];     // GetTickCount64 of the hand's last publish, 0 = never
    bool      haveRot[2];    // hand[h][3..6] is the hand BONE's rotation (else the controller's - sent with handState bit 0x40)
    float     rootYaw;
    float     rootPos[3];    // element 0's world translation - the receiver's position key (trace 20261113_2035)
    bool      haveHead;
    float     elbow[2][3];   // item 5 (trace 20261115_1735): the bend direction, body frame; valid when elbowOk
    bool      elbowOk[2];    // false = the last solve's arm was straight (< kStraightM off its shoulder->wrist line)
    ULONGLONG elbowMs[2];    // GetTickCount64 of the elbow's last publish, 0 = never
};
constexpr float kStraightM = 0.01f;   // item 5: an elbow closer than this to the shoulder->wrist line has no bend side (my value)
SRWLOCK g_lock = SRWLOCK_INIT;
Store   g_store = {};

volatile LONG g_seq       = 0;
volatile LONG g_saidFirst = 0;
volatile LONG g_saidNoPub = 0;
volatile LONG g_saidNoObj = 0;
volatile LONG g_saidNoNet = 0;

// ?GetNetID@IGSObject@@QEBA_KXZ - in OUR engine (RVA 2538C0) three plain loads: [[[this+20h]+78h]+8], -1 when none.
typedef uint64_t (*GetNetIdFn)(const void* self);

GetNetIdFn ResolveGetNetId()
{
    static GetNetIdFn s_fn = nullptr;
    static bool s_tried = false;
    if (!s_tried) {
        s_tried = true;
        HMODULE eng = GetModuleHandleA("engine_x64_rwdi.dll");
        if (eng) { s_fn = reinterpret_cast<GetNetIdFn>(GetProcAddress(eng, "?GetNetID@IGSObject@@QEBA_KXZ")); }
    }
    return s_fn;
}

bool SafeGetNetId(GetNetIdFn fn, uintptr_t obj, uint64_t* out)
{
    __try {
        *out = fn(reinterpret_cast<const void*>(obj));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint8_t KindBits(int k)
{
    if (k == 0 || k == 1 || k == 2) { return static_cast<uint8_t>(k); }
    return 3;
}

}  // namespace

void CoopPosePublishSide(bool isLeft, float bodyYaw, float rootYaw, const float rootPos[3], const float anchorRaw[3],
                         const float headQ[4], const float ctlPos[3], const float ctlQ[4], const float headRep[3],
                         const float wristBack[3], const float* handRot9)
{
    const float d = bodyYaw - rootYaw;   // Ry(-rootYaw) x Ry(bodyYaw)
    const float off[3] = { ctlPos[0] - headRep[0], ctlPos[1] - headRep[1], ctlPos[2] - headRep[2] };
    float hand[7];
    coop::RotYVec(d, off, hand);
    // INV-736 (a) build: the WRIST target, as the solve builds T (anchor + Ry(bodyYaw)(ctl - headRep) - wristBack), in the body frame.
    float wb[3];
    coop::RotYVec(-rootYaw, wristBack, wb);
    hand[0] -= wb[0]; hand[1] -= wb[1]; hand[2] -= wb[2];
    // and the hand BONE's rotation (the partner's hand bone has the same authored frame - INV-736 (a)); the controller's when none yet.
    const bool haveRot = handRot9 != nullptr;
    if (haveRot) {
        float qy[4], my[9], b[9];
        coopik::QYaw(-rootYaw, qy);
        coopik::MatFromQ(qy, my);
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                b[r * 3 + c] = my[r * 3] * handRot9[c] + my[r * 3 + 1] * handRot9[3 + c] + my[r * 3 + 2] * handRot9[6 + c];
            }
        }
        coopik::QFromMat(b, hand + 3);
    } else {
        coop::RotYQuat(d, ctlQ, hand + 3);
    }
    const float rel[3] = { anchorRaw[0] - rootPos[0], anchorRaw[1] - rootPos[1], anchorRaw[2] - rootPos[2] };
    float head[7];
    coop::RotYVec(-rootYaw, rel, head);
    // spec 8.8: the head BEYOND the game's own look - real x inverse(yaw + pitch of the head the look shows), in the body frame.
    // LastReportedHead() is the same read the solve makes (boneblend.cpp SolveArmIk, the C3 mapping). Trace 20261113_2045.
    // INV-736 (c): with the decouple active, body-follows-head has put `folded` degrees of the head's yaw INTO the look the game shows
    // and sends - so that yaw is added back to the reported head before "beyond" is taken. folded 0 / decouple off = today's formula.
    const XrQuaternionf& rq = LastReportedHead();
    const float rep[4] = { rq.x, rq.y, rq.z, rq.w };
    const float foldYaw = HeadYawDecoupleActive() ? coopik::FoldedLookYawRad(HeadFollowFoldedDeg(), HeadYawDecoupleFlipLatched()) : 0.0f;
    float look[4];
    coopik::LookWithFold(rep, foldYaw, look);
    coopik::HeadBeyondLook(headQ, look, d, head + 3);
    const int h = isLeft ? 0 : 1;
    const ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_lock);
    std::memcpy(g_store.hand[h], hand, sizeof(hand));
    std::memcpy(g_store.head, head, sizeof(head));
    g_store.haveRot[h] = haveRot;
    g_store.handMs[h] = now ? now : 1;
    g_store.rootYaw = rootYaw;
    g_store.rootPos[0] = rootPos[0]; g_store.rootPos[1] = rootPos[1]; g_store.rootPos[2] = rootPos[2];
    g_store.haveHead = true;
    ReleaseSRWLockExclusive(&g_lock);
}

// Item 5 (trace 20261115_1735_coop_arms_world): the elbow THIS solve placed, as a bend direction in the body frame (same yaw as the hands).
// Read-only on the solve - S / E / wrist are copied, nothing of the caller's is written.
void CoopPosePublishElbow(bool isLeft, float rootYaw, const float S[3], const float E[3], const float wrist[3])
{
    float w[3], b[3] = { 0.0f, 0.0f, 0.0f };
    const bool ok = coopik::BendDir(S, E, wrist, kStraightM, w);
    if (ok) { coop::RotYVec(-rootYaw, w, b); }
    const int h = isLeft ? 0 : 1;
    const ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_lock);
    std::memcpy(g_store.elbow[h], b, sizeof(b));
    g_store.elbowOk[h] = ok;
    g_store.elbowMs[h] = now ? now : 1;
    ReleaseSRWLockExclusive(&g_lock);
}

// [LOADBEARING: coop_pose sender] the only producer of this player's pose samples (plan Task 4). COST: OFF = one bool test per solve
// pair; ON = see the trace's COST line.
void CoopPoseCaptureLocal()
{
    if (!CoopPose) { return; }
    Store s;
    AcquireSRWLockShared(&g_lock);
    s = g_store;
    ReleaseSRWLockShared(&g_lock);
    const ULONGLONG now = GetTickCount64();
    // Paused for a menu (pause_freeze holds the arms where they were): the last solved hands are sent as current - Jay "Sure. Put it into
    // task 5" (trace 20261113_2015). Read directly, not through PauseFreezeOn (its edge log has a shared static).
    const bool paused = uipin::GamePausedForMenu();
    bool fresh[2];
    for (int h = 0; h < 2; ++h) { fresh[h] = s.handMs[h] != 0 && (paused || now - s.handMs[h] <= kStaleMs); }
    if (!s.haveHead || (!fresh[0] && !fresh[1])) {
        if (InterlockedCompareExchange(&g_saidNoPub, 1, 0) == 0) {
            // [PROBE: COOPPOSE] gate=coop_pose  site=HEADSET  log-only=yes
            LOG("cooppose: [COOPPOSE] NOT SENDING YET - no arm solve published within %llu ms (head %d, left %llu, right %llu ms ago; "
                "0 = never) - logged once",
                kStaleMs, s.haveHead ? 1 : 0, s.handMs[0] ? now - s.handMs[0] : 0ull, s.handMs[1] ? now - s.handMs[1] : 0ull);
        }
        return;
    }

    // netId: re-read when the player object changes, or while it reads "none"
    static uintptr_t s_obj = 0;
    static uint64_t  s_net = ~0ull;
    int why = 0;
    const uintptr_t pd = RigGamePlayerObject(&why);
    if (!pd) {
        if (InterlockedCompareExchange(&g_saidNoObj, 1, 0) == 0) {
            // [PROBE: COOPPOSE] gate=coop_pose  site=HEADSET  log-only=yes
            LOG("cooppose: [COOPPOSE] NOT SENDING - no local player object (RigGamePlayerObject why %d) - logged once", why);
        }
        return;
    }
    if (pd != s_obj || s_net == ~0ull) {
        s_obj = pd;
        uint64_t id = ~0ull;
        GetNetIdFn fn = ResolveGetNetId();
        const bool ok = fn && SafeGetNetId(fn, pd + 40, &id);
        s_net = ok ? id : ~0ull;
        if (!ok && InterlockedCompareExchange(&g_saidNoNet, 1, 0) == 0) {
            // [PROBE: COOPPOSE] gate=coop_pose  site=HEADSET  log-only=yes
            LOG("cooppose: [COOPPOSE] netId NOT READ - %s (engine_x64_rwdi.dll ?GetNetID@IGSObject@@QEBA_KXZ, object %p+40); sending "
                "with netId -1 - logged once",
                fn ? "the call faulted" : "the export is missing", reinterpret_cast<void*>(pd));
        }
    }

    coop::PoseSample ps{};
    ps.seq = static_cast<uint32_t>(InterlockedIncrement(&g_seq));
    ps.sendMs = CoopNetNowMs();
    std::memcpy(ps.head, s.head, sizeof(ps.head));
    std::memcpy(ps.hand, s.hand, sizeof(ps.hand));
    GetControllerFingerWeights(kLeftHand, &ps.finger[0]);
    GetControllerFingerWeights(kRightHand, &ps.finger[1]);
    const int rk = RightHandItemKind(nullptr);
    char cls[48] = {};
    const uintptr_t leftItem = HeldItemNow(1, cls, sizeof(cls));
    const int lk = rk < 0 ? -1 : (leftItem == 0 ? 0 : (std::strcmp(cls, "WeaponVis") == 0 ? 1 : 2));
    ps.handState[0] = KindBits(lk);
    ps.handState[1] = KindBits(rk);
    if (TwoHandGripping()) { ps.handState[0] |= kHsTwoHand; }
    if (ReloadLockOn()) { ps.handState[0] |= kHsReloadLock; }
    for (int h = 0; h < 2; ++h) {
        if (!fresh[h]) { ps.handState[h] |= kHsNoData; }
        if (!s.haveRot[h]) { ps.handState[h] |= coopik::kNoRot; }   // INV-736 build: hand[3..6] is the controller's, not the bone's
        // item 5: the elbow, under the hands' own freshness rule (paused = the frozen solve's last); straight / stale -> bit 0x20
        const bool elbowFresh = s.elbowMs[h] != 0 && (paused || now - s.elbowMs[h] <= kStaleMs);
        if (elbowFresh && s.elbowOk[h]) {
            std::memcpy(ps.elbow[h], s.elbow[h], sizeof(ps.elbow[h]));
        } else {
            ps.handState[h] |= coopik::kNoElbow;
        }
    }
    ps.netId = s_net;
    ps.build = CoopNetBuildId();
    std::memcpy(ps.rootPos, s.rootPos, sizeof(ps.rootPos));
    ps.rootYaw = s.rootYaw;
    CoopNetQueueSend(ps);

    if (InterlockedCompareExchange(&g_saidFirst, 1, 0) == 0) {
        // [PROBE: COOPPOSE] gate=coop_pose  site=HEADSET  log-only=yes
        LOG("cooppose: [COOPPOSE] FIRST LOCAL SAMPLE (logged unconditionally) - netId %llu, player %p, root yaw %+.1f deg, head rel root "
            "%+.3f %+.3f %+.3f m, left hand %+.3f %+.3f %+.3f state 0x%02X, right hand %+.3f %+.3f %+.3f state 0x%02X, elbow bend (body "
            "frame) L %+.2f %+.2f %+.2f%s R %+.2f %+.2f %+.2f%s, protocol %u, build %08X",
            static_cast<unsigned long long>(ps.netId), reinterpret_cast<void*>(pd), s.rootYaw * 57.2957795f,
            ps.head[0], ps.head[1], ps.head[2], ps.hand[0][0], ps.hand[0][1], ps.hand[0][2], ps.handState[0],
            ps.hand[1][0], ps.hand[1][1], ps.hand[1][2], ps.handState[1], ps.elbow[0][0], ps.elbow[0][1], ps.elbow[0][2],
            (ps.handState[0] & coopik::kNoElbow) ? " (NONE - straight / not published)" : "", ps.elbow[1][0], ps.elbow[1][1],
            ps.elbow[1][2], (ps.handState[1] & coopik::kNoElbow) ? " (NONE - straight / not published)" : "",
            static_cast<unsigned>(coop::kCoopProto), ps.build);
    }
}

// ===================== THE RECEIVER (spec 8.3-8.8; trace docs/traces/20261113_2045_coop_receiver.md) =====================
// Runs at the EXIT of PlayerTppVis slot 129 (+C532D0, finalgate::HookVis2) - after the game's own hand-on-gun IK (CorrectHandPosition
// x2) and its "stand" mixer IK (INV-733). Poses the FREE arm(s) (coopik::HandIsOurs) and adds the head beyond the game's look. All
// geometry is taken relative to each bone's own current world + local, so no parent / axis convention is assumed.
namespace {
namespace rx {

typedef const float* (*GetMtxFn)(void* model, int index);
typedef void (*SetMtxFn)(void* model, int index, const float* m34);
typedef int (*ElemIdFn)(void* model, const char* name);
typedef const char* (*ElemNameFn)(void* model, int index);
typedef int (*ElemParentFn)(void* model, int index);

struct Api {
    GetMtxFn     local  = nullptr;
    GetMtxFn     world  = nullptr;
    SetMtxFn     set    = nullptr;
    ElemIdFn     id     = nullptr;
    ElemNameFn   name   = nullptr;
    ElemParentFn parent = nullptr;
    GetNetIdFn   net    = nullptr;
    bool         ok     = false;
};

const Api& ApiGet()
{
    static const Api a = [] {
        Api r;
        HMODULE eng = GetModuleHandleA("engine_x64_rwdi.dll");
        if (eng) {
            r.local  = reinterpret_cast<GetMtxFn>(GetProcAddress(eng, "?GetElementLocalMatrix@IModelObject@@QEBAAEBVmtx34@@H@Z"));
            r.world  = reinterpret_cast<GetMtxFn>(GetProcAddress(eng, "?GetElementWorldMatrix@IModelObject@@QEBAAEBVmtx34@@H@Z"));
            r.set    = reinterpret_cast<SetMtxFn>(GetProcAddress(eng, "?SetElementLocalMatrix@IModelObject@@QEAAXHAEBVmtx34@@@Z"));
            r.id     = reinterpret_cast<ElemIdFn>(GetProcAddress(eng, "?GetElementID@IModelObject@@QEBAHPEBD@Z"));
            r.name   = reinterpret_cast<ElemNameFn>(GetProcAddress(eng, "?GetElementNameCStr@IModelObject@@QEBAPEBDH@Z"));
            r.parent = reinterpret_cast<ElemParentFn>(GetProcAddress(eng, "?GetElementParent@IModelObject@@QEBAHH@Z"));
        }
        r.net = ResolveGetNetId();
        r.ok = r.local && r.world && r.set && r.id && r.name && r.parent && r.net;
        return r;
    }();
    return a;
}

// ---- SEH-guarded engine calls (no C++ objects with destructors in these frames) ----
bool SafeMtx(GetMtxFn fn, uintptr_t model, int e, float out[12])
{
    if (e < 0) { return false; }
    __try {
        const float* m = fn(reinterpret_cast<void*>(model), e);
        if (!m) { return false; }
        for (int i = 0; i < 12; ++i) { out[i] = m[i]; }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeSet(SetMtxFn fn, uintptr_t model, int e, const float m[12])
{
    if (e < 0) { return false; }
    __try { fn(reinterpret_cast<void*>(model), e, m); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
int SafeId(ElemIdFn fn, uintptr_t model, const char* nm)
{
    __try { return fn(reinterpret_cast<void*>(model), nm); } __except (EXCEPTION_EXECUTE_HANDLER) { return -9999; }
}
const char* SafeName(ElemNameFn fn, uintptr_t model, int e)
{
    __try { return fn(reinterpret_cast<void*>(model), e); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
int SafeParent(ElemParentFn fn, uintptr_t model, int e)
{
    __try { return fn(reinterpret_cast<void*>(model), e); } __except (EXCEPTION_EXECUTE_HANDLER) { return -9999; }
}
uintptr_t SafePtr(uintptr_t a)
{
    __try { return *reinterpret_cast<const uintptr_t*>(a); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

enum { kRoot, kClavL, kUpL, kForeL, kHandL, kClavR, kUpR, kForeR, kHandR, kNeck, kNeck1, kHead, kElCount };
const char* const kElName[kElCount] = { "bip01", "l_clavicle", "l_upperarm", "l_forearm", "l_hand",
                                        "r_clavicle", "r_upperarm", "r_forearm", "r_hand", "neck", "neck1", "head" };
constexpr int   kMaxBodies  = 3;
// (kWristBackM 0.097 m along the reach REMOVED by the INV-736 build: protocol 3 sends the wrist target itself, stepped back by the
// sender's own INV-161 vector.)
constexpr float kHandEaseMs = 150.0f;        // spec 8.2: a hand changing owner eases, no snap
constexpr float kHeadMaxRad = 1.3089969f;    // 75 deg - spec 8.6 "a human neck range" (my value)

struct Body {
    uintptr_t model = 0;
    uint64_t  netId = ~0ull;
    int       el[kElCount] = {};
    bool      chain3 = false;      // neck -> neck1 -> head verified by GetElementParent
    bool      armOk[2] = {};       // [0] left, [1] right: upperarm + forearm + hand resolved
    bool      handRotOk[2] = {};   // the hand's parent IS that forearm (GetElementParent) - the hand local is written against it
    bool      headOk = false;
    float     wHand[2] = {};
    double    lastMs = 0.0;
    bool      saidNoPeer = false;
    bool      saidFirstPose = false;
    int       peer = -1;           // the CoopNetPeers slot bound to this body by POSITION (trace 20261113_2035), -1 = none
    // ---- trace 20261115_1735_coop_arms_world (all reset by Bind's `b = Body{}`) ----
    float     lastPos[3] = {};     // item 2: this body's root at its last update - the "another unbound body nearer" test reads it
    bool      posOk = false;
    int       bindSeq = 0;         // item 4: the order this body was bound in (1 = first since launch), 0 = never
    bool      heldOk[2] = {};      // item 5: this body's last good SENT bend direction per arm (body frame); reset on a peer change
    float     held[2][3] = {};
    // item 3: per-body CHECK counters since this body was taken into the slot
    long long posed = 0;
    long long lagN = 0;  float lagSum = 0.0f, lagMax = 0.0f, lagLast = 0.0f;   // sent yaw - drawn yaw (the drawn-body lag), deg
    long long hwN = 0;   float hwMax = 0.0f, hwLast = -1.0f;                   // rebuild: hand world after write vs ours, mm
    long long h2tN[2] = {};   float h2tSum[2] = {}, h2tMax[2] = {};            // hand to target, mm (weight 1, reach <= 1)
    long long reachOver[2] = {};                                               // measured frames with reach > 1 (target out of reach)
    long long elbowSrc[3] = {};                                                // pole from: sent / held / rule (both arms)
    long long bendN = 0; float bendSum = 0.0f, bendMax = 0.0f;                 // achieved bend vs the pole given, deg
};
constexpr float kBindM = 2.0f;        // bind: a peer's sent root within this of the body's root (my value - logged at bind)
constexpr float kSwapMarginM = 1.0f;  // item 2: release a bound peer to an UNBOUND body this much nearer (my value - logged at use)
SRWLOCK g_rxLock = SRWLOCK_INIT;
Body    g_body[kMaxBodies];
int     g_bindSeq = 0;                // under g_rxLock

volatile LONG   g_saidApi = 0;
volatile LONG64 g_poses = 0, g_noPeer = 0, g_noModel = 0, g_zeroW = 0, g_ikFail = 0, g_readFail = 0;

void Compose(const float R[9], const float t[3], float m[12])
{
    m[0] = R[0]; m[1] = R[1]; m[2]  = R[2]; m[3]  = t[0];
    m[4] = R[3]; m[5] = R[4]; m[6]  = R[5]; m[7]  = t[1];
    m[8] = R[6]; m[9] = R[7]; m[10] = R[8]; m[11] = t[2];
}
void Pos(const float m[12], float p[3]) { p[0] = m[3]; p[1] = m[7]; p[2] = m[11]; }
void Sub3(const float a[3], const float b[3], float o[3]) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
// parent world of a bone = W x L^-1 (from the bone's OWN current world + local - no hierarchy lookup)
bool ParentWorld(const float W[12], const float L[12], float P[12])
{
    static const float I[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
    float Li[12];
    if (!coopik::LocalFromWorld(L, I, Li)) { return false; }
    coopik::WorldFromLocal(W, Li, P);
    return true;
}
// A frame rigidly attached to `oldRef` follows it to `newRef`: newX = newRef x (oldRef^-1 x X)
bool Follow(const float oldRef[12], const float newRef[12], const float X[12], float out[12])
{
    float rel[12];
    if (!coopik::LocalFromWorld(oldRef, X, rel)) { return false; }
    coopik::WorldFromLocal(newRef, rel, out);
    return true;
}

void Bind(Body& b, uintptr_t model, const Api& a)
{
    b = Body{};
    b.model = model;
    int missing = 0;
    char miss[160] = {};
    size_t used = 0;
    for (int i = 0; i < kElCount; ++i) {
        const int e = SafeId(a.id, model, kElName[i]);
        const char* back = e >= 0 ? SafeName(a.name, model, e) : nullptr;
        b.el[i] = (e >= 0 && back && _stricmp(back, kElName[i]) == 0) ? e : -1;
        if (b.el[i] < 0) {
            ++missing;
            if (used + 16 < sizeof(miss)) { used += _snprintf_s(miss + used, sizeof(miss) - used, _TRUNCATE, " %s", kElName[i]); }
        }
    }
    const int pN1 = SafeParent(a.parent, model, b.el[kNeck1]);
    const int pH  = SafeParent(a.parent, model, b.el[kHead]);
    b.chain3 = b.el[kNeck] >= 0 && b.el[kNeck1] >= 0 && b.el[kHead] >= 0 && pN1 == b.el[kNeck] && pH == b.el[kNeck1];
    b.armOk[0] = b.el[kUpL] >= 0 && b.el[kForeL] >= 0 && b.el[kHandL] >= 0;
    b.armOk[1] = b.el[kUpR] >= 0 && b.el[kForeR] >= 0 && b.el[kHandR] >= 0;
    b.headOk = b.el[kHead] >= 0;
    const int pHL = SafeParent(a.parent, model, b.el[kHandL]);
    const int pHR = SafeParent(a.parent, model, b.el[kHandR]);
    b.handRotOk[0] = b.armOk[0] && pHL == b.el[kForeL];
    b.handRotOk[1] = b.armOk[1] && pHR == b.el[kForeR];
    // [PROBE: COOPPOSE] gate=coop_pose  site=HEADSET  log-only=no
    LOG("cooppose: [COOPPOSE] BOUND model %p (a partner's PlayerTppVis): elements bip01 %d | L clav %d up %d fore %d hand %d | R clav %d "
        "up %d fore %d hand %d | neck %d neck1 %d head %d; head chain %s (parents of neck1 / head: %d / %d); arms L %s R %s; hand rotation "
        "L %s R %s (hand parents %d / %d, forearms %d / %d); %d missing:%s",
        reinterpret_cast<void*>(model), b.el[kRoot], b.el[kClavL], b.el[kUpL], b.el[kForeL], b.el[kHandL], b.el[kClavR], b.el[kUpR],
        b.el[kForeR], b.el[kHandR], b.el[kNeck], b.el[kNeck1], b.el[kHead], b.chain3 ? "neck>neck1>head (split over 3)" : "NOT a chain (head only)",
        pN1, pH, b.armOk[0] ? "ok" : "REFUSED", b.armOk[1] ? "ok" : "REFUSED", b.handRotOk[0] ? "ok" : "REFUSED (parent is not the forearm)",
        b.handRotOk[1] ? "ok" : "REFUSED (parent is not the forearm)", pHL, pHR, b.el[kForeL], b.el[kForeR], missing, missing ? miss : " none");
    // the spec 5a items 3 / 4 dump: the TPP rest LOCALS as the game has them at bind (before any write of ours on this model)
    const int dump[] = { kUpL, kForeL, kHandL, kUpR, kForeR, kHandR, kNeck, kNeck1, kHead };
    for (int k : dump) {
        float m[12];
        if (!SafeMtx(a.local, model, b.el[k], m)) { continue; }
        // [PROBE: COOPPOSE] gate=coop_pose  site=HEADSET  log-only=yes
        LOG("cooppose: [COOPPOSE] REST %s el %d local | %+.4f %+.4f %+.4f %+.4f | %+.4f %+.4f %+.4f %+.4f | %+.4f %+.4f %+.4f %+.4f",
            kElName[k], b.el[k], m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
    }
}

// One arm. s = 0 left / 1 right (the sample's index). Returns true when it wrote.
// INV-736 build: T is the sender's WRIST target (protocol 3), and the hand BONE turns to the sent rotation when the hand hangs off the
// forearm (b.handRotOk) and the sample carries a bone rotation (bit 0x40 clear). *handRotOut = the hand local was written;
// *handErrMm = |hand world read back after our writes - the hand position our locals put it at| (the INV-736 (b) rebuild check), -1 none.
// Trace 20261115_1735 item 1: rootYaw is the SENDER's (ps.rootYaw) - the frame the sample was built in; rootPos stays the drawn body's.
// Item 5: the elbow pole = the sender's bend (sent / held / the rule - *elbowSrc 0 / 1 / 2). Item 3: *h2tMm = |hand world read back -
// T| and *bendDeg = the achieved bend vs the pole given (-1 = not measured: weight < 1, or the rule pole for the bend).
bool PoseArm(Body& b, const Api& a, const coop::PoseSample& ps, int s, float w, const float rootPos[3], float rootYaw,
             float* reachOut, bool* handRotOut, float* handErrMm, int* elbowSrc, float* h2tMm, float* bendDeg)
{
    *handRotOut = false;
    *handErrMm = -1.0f;
    *elbowSrc = -1;
    *h2tMm = -1.0f;
    *bendDeg = -1.0f;
    const int up = b.el[s ? kUpR : kUpL], fo = b.el[s ? kForeR : kForeL], ha = b.el[s ? kHandR : kHandL];
    const int otherUp = b.el[s ? kUpL : kUpR];
    float Uw[12], Ul[12], Fw[12], Fl[12], Hw[12], Hl[12], Ow[12];
    if (!SafeMtx(a.world, b.model, up, Uw) || !SafeMtx(a.local, b.model, up, Ul) || !SafeMtx(a.world, b.model, fo, Fw) ||
        !SafeMtx(a.local, b.model, fo, Fl) || !SafeMtx(a.world, b.model, ha, Hw) || !SafeMtx(a.local, b.model, ha, Hl)) {
        InterlockedIncrement64(&g_readFail);
        return false;
    }
    float S[3], F[3], H[3], T[3];
    Pos(Uw, S); Pos(Fw, F); Pos(Hw, H);
    coopik::TargetWorld(rootPos, rootYaw, ps.head, ps.hand[s], T);   // protocol 3: already the wrist (sender's INV-161 step-back)
    float uv[3], fv[3];
    Sub3(F, S, uv); Sub3(H, F, fv);
    const float lu = coopik::Len3(uv), lf = coopik::Len3(fv);
    float pole[3] = { 0.0f, -1.0f, 0.0f };   // down ...
    float O[3];
    if (SafeMtx(a.world, b.model, otherUp, Ow)) {   // ... and out, away from the other shoulder
        float out[3];
        Pos(Ow, O);
        Sub3(S, O, out);
        if (coopik::Norm3(out)) { for (int i = 0; i < 3; ++i) { pole[i] += 0.6f * out[i]; } }
    }
    // item 5: the sender's own bend replaces that rule whenever there is one (this sample's, else this body's last good one)
    *elbowSrc = coopik::ChooseElbowPole((ps.handState[s] & coopik::kNoElbow) == 0, ps.elbow[s], rootYaw, &b.heldOk[s], b.held[s], pole);
    float E[3], Hn[3];
    if (!coopik::TwoBoneIk(S, lu, lf, T, pole, E, Hn)) { InterlockedIncrement64(&g_ikFail); return false; }
    // upper arm: swing its F direction onto E
    float Ur[9], nUr[9], toE[3];
    coopik::Rot34(Uw, Ur);
    Sub3(E, S, toE);
    if (!coopik::SwingTo(Ur, uv, toE, nUr)) { InterlockedIncrement64(&g_ikFail); return false; }
    float nU[12];
    Compose(nUr, S, nU);
    // forearm carried by the new upper arm, then swing its hand direction onto Hn
    float nF0[12], nH0[12];
    if (!Follow(Uw, nU, Fw, nF0) || !Follow(Fw, nF0, Hw, nH0)) { InterlockedIncrement64(&g_ikFail); return false; }
    float Fr[9], nFr[9], fp[3], hp[3], cur[3], want[3];
    coopik::Rot34(nF0, Fr);
    Pos(nF0, fp); Pos(nH0, hp);
    Sub3(hp, fp, cur); Sub3(Hn, fp, want);
    if (!coopik::SwingTo(Fr, cur, want, nFr)) { InterlockedIncrement64(&g_ikFail); return false; }
    float nF[12];
    Compose(nFr, fp, nF);
    // locals against each bone's OWN parent (derived, then carried rigidly by the upper arm for the forearm)
    float Pu[12], Pf[12], nPf[12], nUl[12], nFl[12];
    if (!ParentWorld(Uw, Ul, Pu) || !ParentWorld(Fw, Fl, Pf) || !Follow(Uw, nU, Pf, nPf) ||
        !coopik::LocalFromWorld(Pu, nU, nUl) || !coopik::LocalFromWorld(nPf, nF, nFl)) {
        InterlockedIncrement64(&g_ikFail);
        return false;
    }
    float oU[12], oF[12];
    coopik::BlendLocal(Ul, nUl, w, oU);
    coopik::BlendLocal(Fl, nFl, w, oF);
    if (!SafeSet(a.set, b.model, up, oU) || !SafeSet(a.set, b.model, fo, oF)) { InterlockedIncrement64(&g_readFail); return false; }
    // the hand: carried by the new forearm, then turned to the sent bone rotation (INV-736 (a): same authored frame as the sender's)
    float nH1[12];
    const bool carried = Follow(Fw, nF, Hw, nH1);
    if (carried && b.handRotOk[s] && (ps.handState[s] & coopik::kNoRot) == 0) {
        float Hr[9], hp2[3], nHw[12], nHl[12], oH[12];
        coopik::HandWorldRot(rootYaw, ps.hand[s] + 3, Hr);
        Pos(nH1, hp2);
        Compose(Hr, hp2, nHw);
        if (coopik::LocalFromWorld(nF, nHw, nHl)) {
            nHl[3] = Hl[3]; nHl[7] = Hl[7]; nHl[11] = Hl[11];   // the bone's own offset from the forearm stays the game's
            coopik::BlendLocal(Hl, nHl, w, oH);
            if (SafeSet(a.set, b.model, ha, oH)) { *handRotOut = true; }
            else { InterlockedIncrement64(&g_readFail); }
        } else {
            InterlockedIncrement64(&g_ikFail);
        }
    }
    // the rebuild check: does a world read now see our locals on this model (what CorrectItemPosition will read next)?
    float Hchk[12];
    if (carried && w >= 1.0f && SafeMtx(a.world, b.model, ha, Hchk)) {
        float hp3[3], hc[3], dv[3];
        Pos(nH1, hp3); Pos(Hchk, hc); Sub3(hc, hp3, dv);
        *handErrMm = coopik::Len3(dv) * 1000.0f;
        // item 3: where the placed hand ended vs the target it was aimed at; the bend it got vs the pole it was given
        float tv[3];
        Sub3(hc, T, tv);
        *h2tMm = coopik::Len3(tv) * 1000.0f;
        float Fchk[12];
        if (*elbowSrc != 2 && SafeMtx(a.world, b.model, fo, Fchk)) {
            float fc[3];
            Pos(Fchk, fc);
            *bendDeg = coopik::BendAngleDeg(S, fc, hc, pole);
        }
    }
    float st[3];
    Sub3(T, S, st);
    *reachOut = coopik::Len3(st) / (lu + lf);
    return true;
}

// INV-736 (b): CorrectItemPosition(vis, hand) - our gamedll +C62AC0 (EGL rva via buildrva), `void __fastcall(void* entity, int hand)`
// (boneblend.cpp kCorrectItemRva's disassembly note). Resolved once; nullptr = gamedll not loaded.
typedef void(__fastcall* CorrectItemFn)(void* entity, int hand);
CorrectItemFn ResolveCorrectItem()
{
    static CorrectItemFn s_fn = nullptr;
    static bool s_tried = false;
    if (!s_tried) {
        s_tried = true;
        HMODULE gd = GetModuleHandleA("gamedll_x64_rwdi.dll");
        if (gd) { s_fn = reinterpret_cast<CorrectItemFn>(reinterpret_cast<uintptr_t>(gd) + DLVR_COOP_GD_RVA(0xC62AC0)); }
    }
    return s_fn;
}
bool SafeCorrectItem(CorrectItemFn fn, void* vis, int hand)
{
    __try { fn(vis, hand); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
volatile LONG64 g_itemCalls = 0, g_itemFaults = 0, g_handRot = 0;
// (the global yaw / rebuild CHECK totals moved into rx::Body - item 3 of trace 20261115_1735: with two partners "last" mixed bodies)

// Item 3 (trace 20261115_1735_coop_arms_world): the 30 s CHECK - a header line, then ONE LINE PER SLOT holding a model; a body with no
// posed sample says NO DATA. Under g_rxLock (the counters are written only under it). Called before the unbound early return, so an
// unbound body is still named.
void CheckLines(double now)
{
    int used = 0;
    for (int i = 0; i < kMaxBodies; ++i) { if (g_body[i].model) { ++used; } }
    // [PROBE: COOPPOSE] gate=coop_pose  site=HEADSET  log-only=yes
    LOG("cooppose: [COOPPOSE] CHECK (30 s) - %d of %d body slot(s) hold a partner model; run totals: hand rotations %lld | item re-place "
        "calls %lld, faults %lld | arms placed on the SENDER's yaw, elbow from the sender (protocol %u); per-body lines follow",
        used, kMaxBodies, static_cast<long long>(g_handRot), static_cast<long long>(g_itemCalls), static_cast<long long>(g_itemFaults),
        static_cast<unsigned>(coop::kCoopProto));
    for (int i = 0; i < kMaxBodies; ++i) {
        const Body& b = g_body[i];
        if (!b.model) { continue; }
        const double ago = b.lastMs > 0.0 ? (now - b.lastMs) / 1000.0 : -1.0;
        if (b.posed == 0) {
            // [PROBE: COOPPOSE] gate=coop_pose  site=HEADSET  log-only=yes
            LOG("cooppose: [COOPPOSE] CHECK body %d model %p (bind #%d, peer #%d, seen %.1f s ago) NO DATA - no posed sample since this "
                "body was taken in (peer -1 = unbound)", i, reinterpret_cast<void*>(b.model), b.bindSeq, b.peer, ago);
            continue;
        }
        const float h2tMean[2] = { b.h2tN[0] ? b.h2tSum[0] / static_cast<float>(b.h2tN[0]) : -1.0f,
                                   b.h2tN[1] ? b.h2tSum[1] / static_cast<float>(b.h2tN[1]) : -1.0f };
        // [PROBE: COOPPOSE] gate=coop_pose  site=HEADSET  log-only=yes
        LOG("cooppose: [COOPPOSE] CHECK body %d model %p (bind #%d, peer #%d, posed %lld, seen %.1f s ago; totals since taken in) - "
            "drawn-body lag (sent yaw - drawn yaw): n %lld, mean |d| %.1f, max |d| %.1f, last %+.1f deg | rebuild (hand world after write "
            "vs ours): n %lld, max %.1f, last %.1f mm | hand to target L: n %lld, mean %.1f, max %.1f mm, reach > 1 %lld | R: n %lld, mean "
            "%.1f, max %.1f mm, reach > 1 %lld | elbow pole from sent %lld, held %lld, rule %lld | bend vs pole: n %lld, mean %.1f, max "
            "%.1f deg (-1 = none measured)",
            i, reinterpret_cast<void*>(b.model), b.bindSeq, b.peer, b.posed, ago, b.lagN,
            b.lagN ? b.lagSum / static_cast<float>(b.lagN) : 0.0f, b.lagMax, b.lagLast, b.hwN, b.hwMax, b.hwLast,
            b.h2tN[0], h2tMean[0], b.h2tN[0] ? b.h2tMax[0] : -1.0f, b.reachOver[0], b.h2tN[1], h2tMean[1],
            b.h2tN[1] ? b.h2tMax[1] : -1.0f, b.reachOver[1], b.elbowSrc[0], b.elbowSrc[1], b.elbowSrc[2], b.bendN,
            b.bendN ? b.bendSum / static_cast<float>(b.bendN) : -1.0f, b.bendN ? b.bendMax : -1.0f);
    }
}

// The head: the sample's head[3..6] = the turn beyond the game's look (body frame) -> world -> split over the chain, pre-multiplied.
bool PoseHead(const Body& b, const Api& a, const coop::PoseSample& ps, float w, float rootYaw, float* degOut)
{
    float qr[4], qri[4], t[4], Dw[4], p[4];
    coopik::QYaw(rootYaw, qr);
    coopik::QYaw(-rootYaw, qri);
    coopik::QMul(qr, ps.head + 3, t);
    coopik::QMul(t, qri, Dw);
    const int n = b.chain3 ? 3 : 1;
    coopik::HeadSplit(Dw, n, kHeadMaxRad, p);
    *degOut = 2.0f * std::acos(p[3] > 1.0f ? 1.0f : (p[3] < -1.0f ? -1.0f : p[3])) * 57.2957795f * static_cast<float>(n);
    float M[9];
    coopik::MatFromQ(p, M);
    const int chain[3] = { b.el[kNeck], b.el[kNeck1], b.el[kHead] };
    const int first = b.chain3 ? 0 : 2;
    float prevOld[12] = {}, prevNew[12] = {};
    for (int i = first; i < 3; ++i) {
        float W[12], L[12], P[12], nP[12], nW[12], nL[12], o[12];
        if (!SafeMtx(a.world, b.model, chain[i], W) || !SafeMtx(a.local, b.model, chain[i], L) || !ParentWorld(W, L, P)) {
            InterlockedIncrement64(&g_readFail);
            return false;
        }
        if (i == first) { std::memcpy(nP, P, sizeof(nP)); }
        else if (!Follow(prevOld, prevNew, P, nP)) { return false; }
        coopik::WorldFromLocal(nP, L, nW);   // carried by the (turned) parent
        float R[9], nR[9], pos[3];
        coopik::Rot34(nW, R);
        Pos(nW, pos);
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) { nR[r * 3 + c] = M[r * 3] * R[c] + M[r * 3 + 1] * R[3 + c] + M[r * 3 + 2] * R[6 + c]; }
        }
        Compose(nR, pos, nW);
        if (!coopik::LocalFromWorld(nP, nW, nL)) { return false; }
        coopik::BlendLocal(L, nL, w, o);
        if (!SafeSet(a.set, b.model, chain[i], o)) { InterlockedIncrement64(&g_readFail); return false; }
        std::memcpy(prevOld, W, sizeof(W));
        std::memcpy(prevNew, nW, sizeof(nW));
    }
    return true;
}

}  // namespace rx
}  // namespace

// [LOADBEARING: coop_pose receiver] the only writer of a partner's arm / head pose (spec 8.3). COST: OFF = one bool test (and
// +C532D0 runs only in co-op); ON = the trace's COST line.
void CoopPoseApplyRemote(void* vis)
{
    if (!CoopPose || !vis) { return; }
    const rx::Api& a = rx::ApiGet();
    if (!a.ok) {
        if (InterlockedCompareExchange(&rx::g_saidApi, 1, 0) == 0) {
            // [PROBE: COOPPOSE] gate=coop_pose  site=HEADSET  log-only=yes
            LOG("cooppose: [COOPPOSE] RECEIVER REFUSED - an engine export is missing (local %p world %p set %p id %p name %p parent %p "
                "netid %p); partners' bodies are left to the game - logged once",
                reinterpret_cast<void*>(a.local), reinterpret_cast<void*>(a.world), reinterpret_cast<void*>(a.set),
                reinterpret_cast<void*>(a.id), reinterpret_cast<void*>(a.name), reinterpret_cast<void*>(a.parent),
                reinterpret_cast<void*>(a.net));
        }
        return;
    }
    const uintptr_t model = rx::SafePtr(reinterpret_cast<uintptr_t>(vis) + 0x50);   // INV-524: the PlayerDI the rig reads elements from
    if (!model) { InterlockedIncrement64(&rx::g_noModel); return; }
    uint64_t netId = ~0ull;
    if (!SafeGetNetId(a.net, model + 40, &netId)) { netId = ~0ull; }
    const double now = CoopNetNowMs();

    AcquireSRWLockExclusive(&rx::g_rxLock);
    int slot = -1, oldest = 0;
    for (int i = 0; i < rx::kMaxBodies; ++i) {
        if (rx::g_body[i].model == model) { slot = i; break; }
        if (rx::g_body[i].lastMs < rx::g_body[oldest].lastMs) { oldest = i; }
    }
    if (slot < 0) { slot = oldest; rx::Bind(rx::g_body[slot], model, a); }
    rx::Body& b = rx::g_body[slot];
    const double dtMs = b.lastMs > 0.0 ? now - b.lastMs : 0.0;
    b.lastMs = now;
    b.netId = netId;   // diagnostic only - a per-machine replica id, never the key (INV-734)

    // THE POSITION KEY (after RUN CO1, trace 20261113_2035): the peer whose sent body root is nearest this body's root.
    float rootW[12];
    if (!rx::SafeMtx(a.world, model, b.el[rx::kRoot], rootW)) {
        InterlockedIncrement64(&rx::g_readFail);
        ReleaseSRWLockExclusive(&rx::g_rxLock);
        return;
    }
    const float bodyPos[3] = { rootW[3], rootW[7], rootW[11] };
    uint64_t puid[3] = {};
    const int np = CoopNetPeers(puid, 3);
    coop::PoseSample smp[3] = {};
    float wb[3] = {}, pos[3][3] = {}, dist[3] = { -1.0f, -1.0f, -1.0f };
    bool have[3] = {}, taken[3] = {};
    for (int p = 0; p < np && p < 3; ++p) {
        have[p] = CoopNetLatest(p, now, &smp[p], &wb[p]);
        if (!have[p]) { continue; }
        for (int c = 0; c < 3; ++c) { pos[p][c] = smp[p].rootPos[c]; }
        const float dx = pos[p][0] - bodyPos[0], dy = pos[p][1] - bodyPos[1], dz = pos[p][2] - bodyPos[2];
        dist[p] = std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    // Item 2 (trace 20261115_1735): a peer held by another body seen in the last second is not free; for each peer, the nearest OTHER
    // body that is UNBOUND (seen in the last second) - the only thing besides staleness that takes a bound peer away. Live = a sample and
    // a buffer weight above 0 (the existing stale test: no packet for the timeout, then the ease).
    float dOther[3] = { -1.0f, -1.0f, -1.0f };
    bool live[3] = {};
    for (int p = 0; p < np && p < 3; ++p) { live[p] = have[p] && wb[p] > 0.0f; }
    for (int j = 0; j < rx::kMaxBodies; ++j) {
        const rx::Body& o = rx::g_body[j];
        if (j == slot || !o.model || !(now - o.lastMs < 1000.0)) { continue; }
        if (o.peer >= 0 && o.peer < 3) { taken[o.peer] = true; continue; }
        if (!o.posOk) { continue; }
        for (int p = 0; p < np && p < 3; ++p) {
            if (!have[p]) { continue; }
            const float dx = pos[p][0] - o.lastPos[0], dy = pos[p][1] - o.lastPos[1], dz = pos[p][2] - o.lastPos[2];
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dOther[p] < 0.0f || d < dOther[p]) { dOther[p] = d; }
        }
    }
    b.lastPos[0] = bodyPos[0]; b.lastPos[1] = bodyPos[1]; b.lastPos[2] = bodyPos[2];
    b.posOk = true;
    int peer = b.peer, lostWhy = 0;
    if (peer >= 0) {
        const bool inRange = peer < np && peer < 3;
        if (!coop::KeepBoundPeer(inRange && live[peer], inRange && taken[peer], inRange ? dist[peer] : -1.0f,
                                 inRange ? dOther[peer] : -1.0f, rx::kBindM, rx::kSwapMarginM, &lostWhy)) {
            if (inRange) { taken[peer] = true; }   // released: not re-picked by this body in the same update
            peer = -1;
        }
    }
    if (peer < 0) { peer = coop::NearestPeer(bodyPos, pos, live, taken, np < 3 ? np : 3, rx::kBindM); }
    if (peer != b.peer) {
        static const char* const kWhy[4] = { "-", "no live samples (buffer weight 0)", "held by another body",
                                             "another unbound body nearer by more than the margin" };
        const int was = b.peer;
        if (peer >= 0) { b.bindSeq = ++rx::g_bindSeq; }
        // [PROBE: COOPPOSE] gate=coop_pose  site=HEADSET  log-only=yes
        LOG("cooppose: [COOPPOSE] %s model %p (netId %llu, root %+.2f %+.2f %+.2f) peer #%d -> #%d%s%s - %d peer(s), sent-root distances "
            "%.2f / %.2f / %.2f m, nearest other unbound body %.2f / %.2f / %.2f m (-1 = none; bind <= %.1f m, no keep limit, release to a "
            "body >= %.1f m nearer), bind #%d",
            peer >= 0 ? "PEER BOUND" : "PEER LOST", reinterpret_cast<void*>(model), static_cast<unsigned long long>(netId), bodyPos[0],
            bodyPos[1], bodyPos[2], was, peer, lostWhy ? " released: " : "", lostWhy ? kWhy[lostWhy < 4 ? lostWhy : 0] : "", np, dist[0],
            dist[1], dist[2], dOther[0], dOther[1], dOther[2], rx::kBindM, rx::kSwapMarginM, b.bindSeq);
        b.peer = peer;
        b.saidNoPeer = false;
        b.heldOk[0] = b.heldOk[1] = false;   // item 5: a held elbow belongs to the player it came from
    }
    // item 3: the 30 s CHECK - here, before the unbound return, so an unbound body is still named. The stamp moves first (INV-9).
    static ULONGLONG s_lastCheck = 0;
    const ULONGLONG nowC = GetTickCount64();
    if (nowC - s_lastCheck >= 30000) {
        s_lastCheck = nowC;
        rx::CheckLines(now);
    }
    if (peer < 0) {
        InterlockedIncrement64(&rx::g_noPeer);
        if (!b.saidNoPeer) {
            b.saidNoPeer = true;
            // [PROBE: COOPPOSE] gate=coop_pose  site=HEADSET  log-only=yes
            LOG("cooppose: [COOPPOSE] NO PEER for model %p (root %+.2f %+.2f %+.2f) - %d peer(s), sent-root distances %.2f / %.2f / %.2f m "
                "(-1 = no sample yet; bind <= %.1f m); this body stays the game's - logged once per unbound spell",
                reinterpret_cast<void*>(model), bodyPos[0], bodyPos[1], bodyPos[2], np, dist[0], dist[1], dist[2], rx::kBindM);
        }
        ReleaseSRWLockExclusive(&rx::g_rxLock);
        return;
    }
    const coop::PoseSample ps = smp[peer];
    const float wBuf = wb[peer];
    // per-hand ease (spec 8.2). INV-736 build: BOTH hands follow the controllers whenever the sender has data (Jay "Yes, it should" -
    // replaces spec 8.2's free-hand-only HandIsOurs gate).
    const float step = dtMs > 0.0 ? static_cast<float>(dtMs) / rx::kHandEaseMs : 1.0f;
    bool ours[2];
    for (int s = 0; s < 2; ++s) {
        ours[s] = coopik::HandHasData(ps.handState[s]);
        float& wh = b.wHand[s];
        wh = ours[s] ? (wh + step > 1.0f ? 1.0f : wh + step) : (wh - step < 0.0f ? 0.0f : wh - step);
    }
    // Item 1 (trace 20261115_1735): POSITION = the drawn body's root (the shoulders stay attached); YAW = the SENDER's (ps.rootYaw, the
    // frame the sample's hands were built in - cooppose.cpp CoopPosePublishSide). The drawn body's own yaw is only measured (the lag).
    const float rootPos[3] = { rootW[3], rootW[7], rootW[11] };
    const float drawnYaw = std::atan2(rootW[0], rootW[8]);   // the sender's own formula (Task 4: atan2 of element 0's column 0, x / z)
    const float placeYaw = ps.rootYaw;
    float reach[2] = { -1.0f, -1.0f }, headDeg = -1.0f, hwErr[2] = { -1.0f, -1.0f }, h2t[2] = { -1.0f, -1.0f }, bend[2] = { -1.0f, -1.0f };
    int elbowSrc[2] = { -1, -1 };
    bool handRot[2] = {};
    bool wrote = false, armWrote = false;
    for (int s = 0; s < 2; ++s) {
        const float w = wBuf * b.wHand[s];
        if (!(w > 0.0f) || !b.armOk[s]) { continue; }
        const bool ok = rx::PoseArm(b, a, ps, s, w, rootPos, placeYaw, &reach[s], &handRot[s], &hwErr[s], &elbowSrc[s], &h2t[s], &bend[s]);
        wrote |= ok;
        armWrote |= ok;
        if (handRot[s]) { InterlockedIncrement64(&rx::g_handRot); }
    }
    // item 3: this body's numbers - the drawn-body lag (sent yaw - drawn yaw; was "root yaw sent - here"), the rebuild check, hand to
    // target (reach > 1 counted apart), the elbow source and the bend check
    float yawD = ps.rootYaw - drawnYaw;
    while (yawD > 3.14159265f) { yawD -= 6.28318531f; }
    while (yawD < -3.14159265f) { yawD += 6.28318531f; }
    const float yawDeg = yawD * 57.2957795f;
    ++b.lagN;
    b.lagSum += std::fabs(yawDeg);
    if (std::fabs(yawDeg) > b.lagMax) { b.lagMax = std::fabs(yawDeg); }
    b.lagLast = yawDeg;
    for (int s = 0; s < 2; ++s) {
        if (hwErr[s] >= 0.0f) {
            ++b.hwN;
            b.hwLast = hwErr[s];
            if (hwErr[s] > b.hwMax) { b.hwMax = hwErr[s]; }
        }
        if (h2t[s] >= 0.0f) {
            if (reach[s] > 1.0f) { ++b.reachOver[s]; }
            else {
                ++b.h2tN[s];
                b.h2tSum[s] += h2t[s];
                if (h2t[s] > b.h2tMax[s]) { b.h2tMax[s] = h2t[s]; }
            }
        }
        if (elbowSrc[s] >= 0 && elbowSrc[s] < 3) { ++b.elbowSrc[elbowSrc[s]]; }
        if (bend[s] >= 0.0f) {
            ++b.bendN;
            b.bendSum += bend[s];
            if (bend[s] > b.bendMax) { b.bendMax = bend[s]; }
        }
    }
    if (!wrote) { InterlockedIncrement64(&rx::g_zeroW); }
    else {
        InterlockedIncrement64(&rx::g_poses);
        ++b.posed;
        if (!b.saidFirstPose) {
            b.saidFirstPose = true;
            static const char* const kSrc[4] = { "not posed", "SENT", "held", "rule (no sent bend yet)" };
            // [PROBE: COOPPOSE] gate=coop_pose  site=HEADSET  log-only=yes
            LOG("cooppose: [COOPPOSE] FIRST POSE (logged unconditionally) model %p netId %llu bind #%d - buffer weight %.2f, hands followed L "
                "%d R %d (state 0x%02X / 0x%02X, eased %.2f / %.2f), reach / full L %.2f R %.2f (-1 = not posed), hand rotation L %d R %d, "
                "hand world after write vs ours L %.1f R %.1f mm, hand to target L %.1f R %.1f mm (-1 = not checked: weight < 1), elbow pole "
                "L %s R %s, bend vs pole L %.1f R %.1f deg, item re-place via CorrectItemPosition %p (%s), head left to the game (%.1f), "
                "arms PLACED on the sent yaw %+.1f deg, drawn body yaw %+.1f deg (drawn-body lag %+.1f)",
                reinterpret_cast<void*>(model), static_cast<unsigned long long>(netId), b.bindSeq, wBuf, ours[0] ? 1 : 0, ours[1] ? 1 : 0,
                ps.handState[0], ps.handState[1], b.wHand[0], b.wHand[1], reach[0], reach[1], handRot[0] ? 1 : 0, handRot[1] ? 1 : 0,
                hwErr[0], hwErr[1], h2t[0], h2t[1], kSrc[elbowSrc[0] + 1], kSrc[elbowSrc[1] + 1], bend[0], bend[1],
                reinterpret_cast<void*>(rx::ResolveCorrectItem()),
                rx::ResolveCorrectItem() ? "calls follow this line, counted in CHECK" : "NOT RESOLVED - gamedll_x64_rwdi.dll not loaded", headDeg,
                placeYaw * 57.2957795f, drawnYaw * 57.2957795f, yawDeg);
        }
    }
    ReleaseSRWLockExclusive(&rx::g_rxLock);
    // INV-736 (b): the game placed this body's held items (CorrectItemPosition, hands 0 and 1, at +C5336B) on the hand it had BEFORE our
    // write - place them again from ours, exactly as the game calls it, on the same thread, OUTSIDE our lock (game code never runs under
    // g_rxLock). COST: two calls of the game's own per-frame function.
    if (armWrote) {
        rx::CorrectItemFn ci = rx::ResolveCorrectItem();
        for (int h = 0; h < 2 && ci; ++h) {
            if (rx::SafeCorrectItem(ci, vis, h)) { InterlockedIncrement64(&rx::g_itemCalls); }
            else { InterlockedIncrement64(&rx::g_itemFaults); }
        }
    }
    // 5 s throttle (LOG_PERIODIC is only the diag_periodic_logs switch, not a timer); the stamp moves BEFORE the gate is read, so
    // turning the switch off never makes this log more (INV-9).
    static volatile LONG64 s_lastRxLog = 0;
    const LONG64 nowTick = static_cast<LONG64>(GetTickCount64());
    const LONG64 prevTick = s_lastRxLog;
    if (nowTick - prevTick < 5000 || InterlockedCompareExchange64(&s_lastRxLog, nowTick, prevTick) != prevTick) { return; }
    // [PROBE: COOPPOSE] gate=coop_pose,diag_periodic_logs  site=HEADSET  log-only=yes
    LOG_PERIODIC("cooppose: [COOPPOSE] receiver - posed %lld, no peer %lld, no model %lld, nothing to write %lld, IK refused %lld, "
                 "read / write failed %lld (run totals)",
                 static_cast<long long>(rx::g_poses), static_cast<long long>(rx::g_noPeer), static_cast<long long>(rx::g_noModel),
                 static_cast<long long>(rx::g_zeroW), static_cast<long long>(rx::g_ikFail), static_cast<long long>(rx::g_readFail));
}

}  // namespace dlvr
