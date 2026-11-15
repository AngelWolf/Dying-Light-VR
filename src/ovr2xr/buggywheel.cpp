// buggywheel.cpp - the buggy's hands: turn the key to start it, grip the wheel to steer.
// Spec docs/superpowers/specs/2026-11-10-buggy-wheel-and-key-design.md (approved by Jay 2026-11-10 (14)); plan
// docs/superpowers/plans/2026-11-10-buggy-wheel-and-key.md Task 2. Facts: INV-715, INV-716 + addenda 1-5 (OUR gamedll 97046bba).
// The decisions are buggywheel_logic.h (unit-tested); this file only reads the game, feeds a Frame, and carries Out back.
//
// WHAT IS HOOKED:
//   (1) the steer-input writer +D99730 (VehicleController slot 16 -> +D94EE0 -> here; the ONLY writer of the buggy's raw throttle /
//       brake / turn besides the ctor and the no-driver event, INV-716 add. 2): a 15-byte detour (mov [rsp+8],rbx / mov [rsp+10h],rsi /
//       push rdi / sub rsp,50h - no rip-relative operand, no branch in or into it, add. 5). The ORIGINAL RUNS FIRST; then, only for the
//       local player's buggy (the controller's player == RigGamePlayerObject AND the game's own +ECCE30 "in a vehicle" - the exact
//       test +D99730 used to decide to write), the vis hook's fresh decision is applied: raw throttle / brake 0 (the key is needed),
//       raw turn (a hand grips the wheel - hand wins, Jay), and once, the game's own engine start timer (the key's quarter turn).
//       Nothing else is ever written. The raw values then pass through the game's own smoothing (+10630E0, Jay: "Should be fine
//       with the smoothing").
//   (2) CarBuggyVis's vtable (by RTTI name, never by rva) slot 129 = +108A9E0, accepted only if the slot's target starts with its
//       24 known bytes. The ORIGINAL RUNS FIRST (it poses the wheel); then, for the buggy (1) accepted: read the wheel, the engine,
//       the hands; Step; place the wheel at the gripped angle (the game's own rotate +131E490 on a copy of the saved neutral,
//       vis+3728); publish the decision for (1) and the hand pins for the rig's solve.
// COST: (1) the original + two guarded pointer reads, one virtual call, one call of +ECCE30, a few guarded stores - per controller
// update. (2) the original + one element-world read, two small game calls (+10848D0, +105C730), two rig seqlock reads, two squeeze
// reads, one grip pose read, and while gripped one SetElementLocalMatrix - per buggy vis update. OFF = one bool test in each.
#include "buggywheel.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "../common/log.h"
#include "boneblend.h"        // InstallSingleGameDetour, RigGamePlayerObject, RigGripWorldRead
#include "buggywheel_logic.h"
#include "controllers.h"      // GetControllerSqueeze, GetControllerGripPose, PulseController
#include "rtti_vtable.h"      // RttiFindVtable
#include "twist_logic.h"      // reload::TwistWindow::TwistDeg - the grip's twist about its own Z
#include "twohand.h"          // RigTwoHandHaptic - the grab pulse strength (my call: reuse)

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

#if __has_include("../common/buildrva.h")
#include "../common/buildrva.h"
#define DLVR_BUG_GD(rva) ::dlvr::buildrva::Gd(rva)
#else
#define DLVR_BUG_GD(rva) (rva)
#endif

namespace dlvr {

// [DEP: buggy_hand_steer] required-for=the buggy's key turn + gripped-wheel steering (BuggyHandSteer): both buggywheel.cpp hooks
//      act only while it is on; off = the buggy exactly as the game drives it (nothing written). VR Comfort tab. Shipped ON (Tier C,
//      Jay's pick 2026-11-10 (14)). tree=needs the rig running for the hands (RigGripWorldRead). HEADSET.
bool BuggyHandSteer = true;

namespace {

namespace bw = ::dlvr::buggywheel;

// ---- the game (OUR gamedll 97046bba; EGL through buildrva where allowlisted) ----
constexpr uintptr_t kInputRva = 0xD99730;      // +D99730 the steer-input writer (INV-715)
const unsigned char kInputHead[15] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x50 };
constexpr uintptr_t kRotRva = 0x131E490;       // +131E490 rotate a matrix about X by xmm1 DEGREES (add. 5)
const unsigned char kRotHead[9] = { 0x40, 0x53, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00 };   // then rip-relative - not compared
constexpr uintptr_t kReduceRva = 0x10848D0;    // +10848D0 the wheel's speed reduction (wheel module = vis+3664)
const unsigned char kReduceHead[16] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0xF9, 0x0F, 0x29, 0x74 };
constexpr uintptr_t kCanStartRva = 0x105C730;  // +105C730 can the engine be on (a driver + fuel; engine module = CarBuggy+11416)
const unsigned char kCanStartHead[16] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48 };
constexpr uintptr_t kInVehRva = 0xECCE30;      // +ECCE30 the player is in a vehicle (+D99730's own write test)
const unsigned char kInVehHead[16] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x41, 0x28, 0x48, 0x8B, 0xD9, 0x48, 0x83, 0xC1 };
constexpr size_t    kVisSlot = 129;            // CarBuggyVis vtable slot 129 = +108A9E0 (bgy40, control +BFC170 = PlayerFppVis 129)
const unsigned char kVisHead[24] = { 0x48, 0x8B, 0xC4, 0x55, 0x48, 0x8D, 0x6C, 0x24, 0x80, 0x48, 0x81, 0xEC,
                                     0x80, 0x01, 0x00, 0x00, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18 };
const char          kVisClass[] = ".?AVCarBuggyVis@@";
// THE INSTANT START (Jay 2026-11-10: "I want us turning the key to be the instant startup"): BuggyEngineSoundsModule's
// IVehicleEngineSoundParams vtable (by RTTI; bgy41: +1803720, this-offset 0) slot 1 = IsTurningOn +1083360 = CarBuggy timer +12068 > 0
// (bgy41; names from the Mac 2022 symbols). The ignition sound player +1074270 (bgy38) plays buggy_ignition_start.wav only while it is
// true (and CanBeTurnedOn, not IsOn); the start ANIM's test +D97C40 (bgy9) reads the timer itself. So the key makes IsTurningOn true
// for the local buggy until the sound update has asked once, THEN writes timer = 0: +105A5B0 turns the engine on at the next update
// (timer >= 0 -> -= dt -> < 0 -> on), the anim is never requested (timer never > 0), and the start sound plays out (on IsOn the player
// only clears its handle +344).
constexpr size_t    kSndSlot = 1;
const unsigned char kSndHead[18] = { 0x48, 0x8B, 0x41, 0x38, 0x0F, 0x57, 0xC0, 0x0F, 0x2F, 0x80, 0x24, 0x2F, 0x00, 0x00, 0x0F, 0x92, 0xC0, 0xC3 };
const char          kSndClass[] = ".?AVBuggyEngineSoundsModule@@";
constexpr uintptr_t kSndCar = 56;              // the module's CarBuggy (+1083360: mov rax, [rcx+38h])
constexpr unsigned long long kSndWaitMs = 200;  // no sound update asked by then: write the timer anyway (a silent instant start)

// CarBuggy fields (INV-715 / INV-716)
constexpr uintptr_t kRawThrottle = 12232, kRawBrake = 12236, kRawTurn = 12244;
constexpr uintptr_t kEngine = 11416, kEngOn = 648, kEngTimer = 652;
constexpr uintptr_t kParamIdx = 13736, kParamCount = 13776, kParamTable = 13768, kParamDefault = 712, kParamStride = 10656;
constexpr uintptr_t kPStartTime = 6072, kPMaxRot = 4464;
// the controller: +104 non-zero -> +112 = the vehicle (+D99730 / +D97C40)
constexpr uintptr_t kCtrlHas = 104, kCtrlVeh = 112;
// CarBuggyVis: +3672 = the CarBuggy (also its IModelObject); +3664 = the wheel module; +3680 bump, +3712 sprung turn; +3728 neutral
constexpr uintptr_t kVisCar = 3672, kVisWheelMod = 3664, kVisBump = 3680, kVisSprung = 3712, kVisNeutral = 3728;

// the rim radius MEASURED on the buggy mesh (2026-11-10: steering wheel geometry group 54, rim 0.145 - 0.182 m, mean 0.164 m;
// backups/20261110_buggy_hand_renders/pose/wheel.obj); the grab band +/- 0.10 m covers 0.064 - 0.264 m.
constexpr float kRimRadiusM = 0.164f;
constexpr float kTickAmp = 0.15f, kStartAmp = 1.0f;
constexpr int   kGrabMs = 40, kTickMs = 10, kStartMs = 80;
constexpr unsigned long long kFreshMs = 150;

using InputFn = char(__fastcall*)(void* ctrl);
using VisFn = void(__fastcall*)(void* vis, long long a2, double a3);
using SndFn = bool(__fastcall*)(void* module);
using RotFn = void(__fastcall*)(float* m, float deg);
using ReduceFn = float(__fastcall*)(void* wheelModule);
using CanStartFn = char(__fastcall*)(void* engine);
using InVehFn = char(__fastcall*)(void* player);
using GetIdFn = int(__fastcall*)(void* model, const char* name);
using GetWorldFn = const float*(__fastcall*)(void* model, int id);
using SetLocalFn = void(__fastcall*)(void* model, int id, const float* m);

InputFn    g_inputTrue = nullptr;
VisFn      g_visTrue = nullptr;
SndFn      g_sndTrue = nullptr;
// the instant start: the CarBuggy waiting for its ignition sound (0 = none), when, and whether the sound update has asked
volatile LONG64 g_pendCar = 0, g_pendMs = 0;
volatile LONG   g_sndSaw = 0, g_instLogs = 0;
RotFn      g_rot = nullptr;
ReduceFn   g_reduce = nullptr;
CanStartFn g_canStart = nullptr;
InVehFn    g_inVeh = nullptr;
GetIdFn    g_getId = nullptr;
GetWorldFn g_getWorld = nullptr;
SetLocalFn g_setLocal = nullptr;

volatile LONG g_state = 0;            // 0 not tried, 1 live, -1 refused (camhook's 1 Hz thread)
bool g_saidOff = false;

// (1) <- (2): the decision, stamped. (1) -> (2): the buggy (1) last accepted as the local driver's.
volatile LONG64 g_accepted = 0;
volatile LONG   g_hold = 0, g_writeTurn = 0, g_wantStart = 0;
volatile LONG   g_rawTurnBits = 0;
volatile LONG64 g_outMs = 0;

// the pins, per RIG side (0 = right, 1 = left), seqlock
// rim / grabN (RUN BK10's drawn-grip line only): the rim frame at the pin (columns axial, radial-out, tangent; world) and the grab's
// number for that rig side - read by BuggyNoteDrawnGrip, nothing else.
struct Pin { float pos[3]; float rot[9]; float rim[9]; LONG mode; LONG active; LONG grabN; LONG64 ms; };   // mode: BuggyPinMode
volatile LONG g_pinSeq[2] = {};
Pin g_pin[2] = {};

// probe state
volatile LONG g_firstInput = 0, g_firstSample = 0, g_gateMiss = 0, g_startLogs = 0, g_events = 0, g_axisChecks = 0;

bool ReadPtr(uintptr_t a, uintptr_t& v)
{
    __try { v = *reinterpret_cast<const volatile uintptr_t*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadF(uintptr_t a, float& v)
{
    __try { v = *reinterpret_cast<const volatile float*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadI(uintptr_t a, int& v)
{
    __try { v = *reinterpret_cast<const volatile int*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadB(uintptr_t a, unsigned char& v)
{
    __try { v = *reinterpret_cast<const volatile unsigned char*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool WriteF(uintptr_t a, float v)
{
    __try { *reinterpret_cast<volatile float*>(a) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool HeadIs(uintptr_t a, const unsigned char* want, size_t n, unsigned char* got)
{
    __try { memcpy(got, reinterpret_cast<const void*>(a), n); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return memcmp(got, want, n) == 0;
}
bool CallPlayer(void* ctrl, uintptr_t& player)   // the controller's own player: vfunc +32 (as +D99730 reads it)
{
    __try {
        using Fn = uintptr_t(__fastcall*)(void*);
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(ctrl);
        player = reinterpret_cast<Fn>(*reinterpret_cast<const uintptr_t*>(vt + 32))(ctrl);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallInVeh(uintptr_t player, bool& in)
{
    __try { in = g_inVeh(reinterpret_cast<void*>(player)) != 0; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallCanStart(uintptr_t engine, bool& can)
{
    __try { can = g_canStart(reinterpret_cast<void*>(engine)) != 0; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallReduce(uintptr_t wheelModule, float& r)
{
    __try { r = g_reduce(reinterpret_cast<void*>(wheelModule)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallWorld(uintptr_t model, int id, float out[12])
{
    __try {
        const float* w = g_getWorld(reinterpret_cast<void*>(model), id);
        if (!w) { return false; }
        for (int i = 0; i < 12; ++i) { out[i] = w[i]; }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallId(uintptr_t model, int& id)
{
    __try { id = g_getId(reinterpret_cast<void*>(model), "steeringwheel"); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// RUN BK11 (Jay: "2. That sounds about right though, 2.5cm looks to be about the difference"): IS THE WHEEL DRAWN AT FULL SIZE? The
// model file (dlc_wl_ve_buggy.nodes.json): steeringwheel node 109 has world column lengths (1, 0.849, 0.849) - the rim's plane is its
// Y/Z - and its geometry child node 112 (kind 4, the mesh) has a local that cancels 109's rotation AND scale (world = unit, at 109's
// origin). The pin assumes the rim drawn at r 0.164 (full size). This reads both LIVE: 109's column lengths (CONTROL: the file says
// 1 / 0.849 / 0.849 before we place the wheel), id+3's origin vs 109's (IDENTITY: the file says 0 mm - else it is not node 112 and its
// numbers mean nothing) and its column lengths (0.849 on Y/Z = the rim drawn 2.5 cm small). Event-only, <= 8 lines. COST: two engine
// world reads per line.
void LogWheelScale(uintptr_t car, int id, const char* when)
{
    static volatile LONG s_lines = 0;
    if (InterlockedIncrement(&s_lines) > 8) { return; }
    float A[12] = {}, G[12] = {};
    const bool okA = CallWorld(car, id, A);
    const bool okG = CallWorld(car, id + 3, G);
    auto len = [](const float* m, int c) { return std::sqrt(m[c] * m[c] + m[4 + c] * m[4 + c] + m[8 + c] * m[8 + c]); };
    const float dO = okA && okG ? 1000.0f * std::sqrt((G[3] - A[3]) * (G[3] - A[3]) + (G[7] - A[7]) * (G[7] - A[7]) + (G[11] - A[11]) * (G[11] - A[11]))
                                : -1.0f;
    // [PROBE: BUGGY] gate=buggy_hand_steer  site=HEADSET  log-only=yes
    LOG("buggywheel: [BUGGY] WHEEL SCALE (%s; <= 8) - element %d (steeringwheel) %s column lengths x %.4f y %.4f z %.4f (file: 1 / 0.849 / "
        "0.849) | element %d %s: origin %.1f mm from %d's (file node 112: 0 mm - if not ~0 it is NOT the mesh node) column lengths x "
        "%.4f y %.4f z %.4f (1 = the rim drawn full size, r 0.164; 0.849 = drawn at r 0.139, 2.5 cm in) | x columns: %d (%+.3f %+.3f "
        "%+.3f), %d (%+.3f %+.3f %+.3f).", when, id, okA ? "read" : "*** NOT READ ***", okA ? len(A, 0) : 0.0f, okA ? len(A, 1) : 0.0f,
        okA ? len(A, 2) : 0.0f, id + 3, okG ? "read" : "*** NOT READ ***", dO, id, okG ? len(G, 0) : 0.0f, okG ? len(G, 1) : 0.0f,
        okG ? len(G, 2) : 0.0f, id, A[0], A[4], A[8], id + 3, G[0], G[4], G[8]);
}

bool PlaceWheel(uintptr_t vis, uintptr_t model, int id, float deg)
{
    __try {
        float m[12];
        memcpy(m, reinterpret_cast<const void*>(vis + kVisNeutral), sizeof(m));
        g_rot(m, deg);
        g_setLocal(reinterpret_cast<void*>(model), id, m);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

uintptr_t Params(uintptr_t car)   // the buggy's params block, exactly as +10630E0 / +10848D0 pick it
{
    int idx = -1, n = 0;
    uintptr_t table = 0;
    if (!ReadI(car + kParamIdx, idx) || !ReadI(car + kParamCount, n)) { return 0; }
    if (idx < 0 || idx >= n) { return car + kParamDefault; }
    if (!ReadPtr(car + kParamTable, table) || !table) { return 0; }
    return table + kParamStride * static_cast<uintptr_t>(idx);
}

unsigned long long NowMs() { return GetTickCount64(); }

void PublishPin(int rigSide, bool active, const float pos[3], const float rot[9], int mode, const float rim[9] = nullptr,
                LONG grabN = 0)
{
    InterlockedIncrement(&g_pinSeq[rigSide]);
    MemoryBarrier();
    g_pin[rigSide].active = active ? 1 : 0;
    if (active) {
        for (int i = 0; i < 3; ++i) { g_pin[rigSide].pos[i] = pos[i]; }
        for (int i = 0; i < 9; ++i) { g_pin[rigSide].rot[i] = rot[i]; }
        for (int i = 0; i < 9; ++i) { g_pin[rigSide].rim[i] = rim ? rim[i] : 0.0f; }
        g_pin[rigSide].mode = mode;
        g_pin[rigSide].grabN = grabN;
    }
    g_pin[rigSide].ms = static_cast<LONG64>(NowMs());
    MemoryBarrier();
    InterlockedIncrement(&g_pinSeq[rigSide]);
}

// ---- small vector helpers (row-major 3x3, axes as COLUMNS - the rig's convention) ----
void Norm(float v[3])
{
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}
void Cross(const float a[3], const float b[3], float o[3])
{
    o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
}
// B = the wheel's body-fixed frame: columns axis, ref, axis x ref (does not spin with the wheel; moves with the buggy)
void BodyFrame(const float axis[3], const float ref[3], float B[9])
{
    float c[3];
    Cross(axis, ref, c);
    for (int r = 0; r < 3; ++r) { B[3 * r + 0] = axis[r]; B[3 * r + 1] = ref[r]; B[3 * r + 2] = c[r]; }
}
void MulT(const float B[9], const float v[3], float o[3])   // B^T v
{
    for (int c = 0; c < 3; ++c) { o[c] = B[c] * v[0] + B[3 + c] * v[1] + B[6 + c] * v[2]; }
}
void Mul(const float B[9], const float v[3], float o[3])    // B v
{
    for (int r = 0; r < 3; ++r) { o[r] = B[3 * r] * v[0] + B[3 * r + 1] * v[1] + B[3 * r + 2] * v[2]; }
}
void Mat(const float A[9], const float B[9], float o[9])
{
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) { o[3 * r + c] = A[3 * r] * B[c] + A[3 * r + 1] * B[3 + c] + A[3 * r + 2] * B[6 + c]; }
    }
}
void MatT(const float A[9], const float B[9], float o[9])   // A^T B
{
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) { o[3 * r + c] = A[r] * B[c] + A[3 + r] * B[3 + c] + A[6 + r] * B[6 + c]; }
    }
}
void Rx(float deg, float o[9])   // right-handed about the body frame's x (= the column axis)
{
    const float a = deg * 0.0174532925f, c = std::cos(a), s = std::sin(a);
    const float m[9] = { 1, 0, 0, 0, c, -s, 0, s, c };
    for (int i = 0; i < 9; ++i) { o[i] = m[i]; }
}

// THE PASS HOLD (RUN BK12; Jay BK11: "I felt like I had to break my actual wrist to get the right placement" -> "Yes" to the hand's
// rotation set against the rim from his passes). Per rig side (0 = RIGHT, 1 = LEFT): the DRAWN hand's axes in the rim frame at the
// angle grabbed (columns x / y / z in (axial, radial-out, tangent = +turn); row-major). From the [BUGGY] DRAWN GRIP lines of the grabs
// Jay passed (scratchpad pass_hold.py, chordal mean re-orthonormalised, det +1.0000): RIGHT = BK11 #23 (10:19:24, "The last grab on the
// right hand") + #4 (10:17:45) + #6 (10:17:54) ("Yes" - the first few) - 14.1 / 6.5 / 8.2 deg from the mean; LEFT = BK10 #8 (09:58:10,
// "2nd grab on the left hand specifically was a pass"). Cross-check only: the left pass mirrored onto the right is 16.1 deg from it.
const float kWheelHoldInRim[2][9] = {
    { -0.939213f, +0.255500f, +0.229344f, +0.291874f, +0.242412f, +0.925227f, +0.180800f, +0.935925f, -0.302251f },   // RIGHT
    { -0.831222f, -0.268107f, +0.487020f, +0.536143f, -0.154890f, +0.829795f, -0.147039f, +0.950856f, +0.272492f },   // LEFT
};

// ---- per-hand grab memory, in the body frame (vis hook thread only) ----
struct Grab { bool on = false; bool wheel = false; bool hold = false; float alignAxial = 0.0f, alignR = 0.0f; float localPos[3] = {};
              float localRot[9] = {};
              float wheelAtGrab = 0.0f;
              float thGrab = 0.0f; LONG grabN = 0; };   // RUN BK10's drawn-grip line: the angle grabbed (deg) + this grab's number
// THE WHEEL ALIGNMENT (RUN BK4 -> BK5, Jay: "use that as the point of where the wheel is for alignment, not to actually force those
// specific spots"): kBuggyHold[rig side][0..2] = the clip's frame-5 grip point relative to the body element 24 (tools/buggy_hand_poses.py,
// control C4 + a negative control). NOT USED since RUN BK6: its handholder is the clip's POSED one, ~7 cm from the rig's grip point; the
// wheel pin is the rim point and boneblend applies the clip's rim-in-hand offset (kWheelRimFromGrip, buggy_poses.inc).
#include "buggy_hold.inc"
Grab g_grab[2];                 // by LOGIC hand (bw::kLeft / kRight)
bw::State g_logic;
uintptr_t g_lastCar = 0;
int g_wheelId = -1;
bool g_haveRoll = false;
float g_lastQ[4] = {};
float g_rollAcc = 0.0f;
bool g_wasActive = false;
// the axis self-check: the wheel's world basis at a reference shown angle
bool g_haveRef = false;
float g_refW[12] = {};
float g_refShown = 0.0f;

void ClearOutputs()
{
    InterlockedExchange(&g_hold, 0);
    InterlockedExchange(&g_writeTurn, 0);
    InterlockedExchange(&g_wantStart, 0);
    InterlockedExchange64(&g_pendCar, 0);
    for (int s = 0; s < 2; ++s) { PublishPin(s, false, nullptr, nullptr, false); }
    for (int i = 0; i < 2; ++i) { g_grab[i] = Grab{}; }
    g_logic.Reset();
    g_haveRoll = false;
    g_wasActive = false;
}

// COST: the original + (local driver only) two guarded reads, one virtual call, +ECCE30, up to four guarded stores.
char __fastcall InputHook(void* ctrl)
{
    const char r = g_inputTrue(ctrl);
    if (!BuggyHandSteer) { return r; }
    const uintptr_t c = reinterpret_cast<uintptr_t>(ctrl);
    uintptr_t has = 0, car = 0, player = 0;
    if (!ReadPtr(c + kCtrlHas, has) || !has || !ReadPtr(c + kCtrlVeh, car) || !car) { return r; }
    if (!CallPlayer(ctrl, player)) { return r; }
    int why = 0;
    const uintptr_t local = RigGamePlayerObject(&why);
    bool inVeh = false;
    const bool okCall = player && CallInVeh(player, inVeh);
    const bool accept = local && player == local && okCall && inVeh;
    if (InterlockedCompareExchange(&g_firstInput, 1, 0) == 0) {
        // [PROBE: BUGGY] gate=buggy_hand_steer  site=HEADSET  log-only=yes
        LOG("buggywheel: [BUGGY] FIRST INPUT UPDATE (logged unconditionally) - controller %p, vehicle %p, its player %p, the rig's local "
            "player %p (read %d), in a vehicle %s -> %s.", ctrl, reinterpret_cast<void*>(car), reinterpret_cast<void*>(player),
            reinterpret_cast<void*>(local), why, okCall ? (inVeh ? "yes" : "no") : "*** CALL FAULTED ***",
            accept ? "THE LOCAL DRIVER's BUGGY" : "not the local driver - nothing written");
    }
    if (!accept) {
        if (static_cast<uintptr_t>(g_accepted) == car) { InterlockedExchange64(&g_accepted, 0); }
        if (player && local && player != local && InterlockedIncrement(&g_gateMiss) <= 3) {
            // [PROBE: BUGGY] gate=buggy_hand_steer  site=HEADSET  log-only=yes
            LOG("buggywheel: [BUGGY] GATE: the controller's player %p is not the rig's local player %p (miss %ld of 3 logged) - this "
                "buggy is not written.", reinterpret_cast<void*>(player), reinterpret_cast<void*>(local), static_cast<long>(g_gateMiss));
        }
        return r;
    }
    InterlockedExchange64(&g_accepted, static_cast<LONG64>(car));
    if (NowMs() - static_cast<unsigned long long>(g_outMs) > kFreshMs) { return r; }   // no fresh decision: nothing written
    if (g_hold) {
        WriteF(car + kRawThrottle, 0.0f);
        WriteF(car + kRawBrake, 0.0f);
    }
    if (g_writeTurn) {
        LONG bits = g_rawTurnBits;
        float t = 0.0f;
        memcpy(&t, &bits, sizeof(t));
        WriteF(car + kRawTurn, t);
    }
    if (InterlockedExchange(&g_wantStart, 0)) {
        const uintptr_t eng = car + kEngine;
        unsigned char on = 1;
        float timer = 0.0f, startT = 0.0f;
        bool can = false;
        const uintptr_t p = Params(car);
        const bool read = ReadB(eng + kEngOn, on) && ReadF(eng + kEngTimer, timer) && CallCanStart(eng, can) && p &&
                          ReadF(p + kPStartTime, startT);
        const bool go = read && on == 0 && timer < 0.0f && can;
        if (go) {
            // THE INSTANT START: wait for the ignition sound's one ask (IsTurningOnHook), then timer = 0 (below). No sound hook = now.
            InterlockedExchange(&g_sndSaw, 0);
            InterlockedExchange64(&g_pendMs, static_cast<LONG64>(NowMs()));
            InterlockedExchange64(&g_pendCar, static_cast<LONG64>(car));
        }
        if (InterlockedIncrement(&g_startLogs) <= 20) {
            // [PROBE: BUGGY] gate=buggy_hand_steer  site=HEADSET  log-only=yes
            LOG("buggywheel: [BUGGY] KEY -> %s (engine on %u, timer %.3f, can start %s, the game's own start time %.3f s - NOT used; "
                "read %s; ignition sound hook %s).", go ? "INSTANT START requested" : "*** NOT STARTED ***", static_cast<unsigned>(on),
                timer, can ? "yes" : "no", startT, read ? "ok" : "FAILED", g_sndTrue ? "live" : "*** NOT LIVE - a silent start ***");
        }
    }
    // [LOADBEARING: buggy hand steer hooks] (buggy_hand_steer) THE INSTANT START's write: timer = 0 once the ignition sound has asked
    //      IsTurningOn (or kSndWaitMs passed, or no sound hook) - the game's countdown +105A5B0 then turns the engine on next update.
    if (static_cast<uintptr_t>(g_pendCar) == car) {
        const unsigned long long waited = NowMs() - static_cast<unsigned long long>(g_pendMs);
        const bool saw = g_sndSaw != 0;
        if (saw || !g_sndTrue || waited > kSndWaitMs) {
            InterlockedExchange64(&g_pendCar, 0);
            const uintptr_t eng = car + kEngine;
            unsigned char on = 1;
            float timer = 0.0f;
            const bool ok = ReadB(eng + kEngOn, on) && ReadF(eng + kEngTimer, timer) && on == 0 && timer < 0.0f;
            if (ok) { WriteF(eng + kEngTimer, 0.0f); }
            if (InterlockedIncrement(&g_instLogs) <= 20) {
                // [PROBE: BUGGY] gate=buggy_hand_steer  site=HEADSET  log-only=yes
                LOG("buggywheel: [BUGGY] INSTANT START %s - the ignition sound %s after %llu ms; engine on %u, timer %.3f before the "
                    "write (the start anim needs timer > 0 - never).", ok ? "-> timer 0 WRITTEN (the engine comes on next update)" :
                    "*** NOT WRITTEN (the engine was already on / starting) ***", saw ? "ASKED (it plays)" :
                    (g_sndTrue ? "*** NEVER ASKED (timed out - a silent start) ***" : "hook not live (silent)"), waited,
                    static_cast<unsigned>(on), timer);
            }
        }
    }
    return r;
}

void Pulse(int logicHand, float amp, int ms)
{
    if (amp > 0.0f) { PulseController(logicHand == bw::kLeft ? kLeftHand : kRightHand, amp, ms); }
}

// THE INSTANT START's sound: BuggyEngineSoundsModule::IsTurningOn (vtable slot 1). For the ONE buggy waiting on its key start it
// answers true (and records that the sound update asked); every other call is the game's own answer. COST: the original + one
// volatile load (+ one guarded read while a start is pending).
bool __fastcall IsTurningOnHook(void* module)
{
    const bool r = g_sndTrue(module);
    const uintptr_t pend = static_cast<uintptr_t>(g_pendCar);
    if (!pend) { return r; }
    uintptr_t car = 0;
    if (!ReadPtr(reinterpret_cast<uintptr_t>(module) + kSndCar, car) || car != pend) { return r; }
    InterlockedExchange(&g_sndSaw, 1);
    return true;
}

// COST: see the file header.
void __fastcall VisHook(void* vis, long long a2, double a3)
{
    g_visTrue(vis, a2, a3);
    if (!BuggyHandSteer) {
        if (g_wasActive) { ClearOutputs(); }
        return;
    }
    const uintptr_t v = reinterpret_cast<uintptr_t>(vis);
    uintptr_t car = 0;
    if (!ReadPtr(v + kVisCar, car) || !car || car != static_cast<uintptr_t>(g_accepted)) {
        if (g_wasActive && car == g_lastCar) { ClearOutputs(); }   // our buggy, no longer the local driver's
        return;
    }
    if (car != g_lastCar) {
        g_lastCar = car;
        g_wheelId = -1;
        g_haveRef = false;
        if (!CallId(car, g_wheelId)) { g_wheelId = -1; }
    }
    if (g_wheelId < 0) { return; }
    g_wasActive = true;

    bw::Frame f{};
    f.enabled = true;
    f.localDriver = true;
    const uintptr_t eng = car + kEngine;
    unsigned char on = 1;
    ReadB(eng + kEngOn, on);
    f.engineOn = on != 0;
    ReadF(eng + kEngTimer, f.timer);
    bool can = false;
    CallCanStart(eng, can);
    f.canStart = can;

    float W[12];
    if (!CallWorld(car, g_wheelId, W)) { return; }
    f.wheelCentre[0] = W[3]; f.wheelCentre[1] = W[7]; f.wheelCentre[2] = W[11];
    float axis[3] = { W[0], W[4], W[8] };   // the element's X column - the axis +131E490 rotates about (ruling; self-check below)
    Norm(axis);
    const float up[3] = { 0.0f, 1.0f, 0.0f };
    float ref[3];
    Cross(axis, up, ref);
    if (std::sqrt(ref[0] * ref[0] + ref[1] * ref[1] + ref[2] * ref[2]) < 0.1f) { ref[0] = W[1]; ref[1] = W[5]; ref[2] = W[9]; }
    Norm(ref);
    for (int i = 0; i < 3; ++i) { f.wheelAxis[i] = axis[i]; f.wheelRef[i] = ref[i]; }
    f.rimRadiusM = kRimRadiusM;

    const uintptr_t p = Params(car);
    float maxRot = 0.0f, reduce = 0.0f, bump = 0.0f, sprung = 0.0f;
    if (p) { ReadF(p + kPMaxRot, maxRot); }
    CallReduce(v + kVisWheelMod, reduce);
    ReadF(v + kVisBump, bump);
    ReadF(v + kVisSprung, sprung);
    f.maxWheelDeg = std::fabs(maxRot * reduce);
    const float shown = maxRot * sprung + bump;
    f.gameWheelDeg = shown;
    // THE KEY POINT (Jay: "The marked spot is fine"): bw::kKeyRel on the body element 24's LIVE world. Control: the same formula's
    // steering-wheel point vs the live steeringwheel element (W's translation) - over bw::kKeyFrameTolM the key stays OFF (no hold, no
    // key grab: the engine starts the game's way) and the FIRST SAMPLE / KEY FRAME lines say so. COST: one element-world read.
    float body[12] = {};
    bool haveBody = false;   // element 24's live world, read here; also the wheel hold's frame (the pins, below)
    {
        float B[12], wp[3];
        float dWheel = -1.0f;
        if (CallWorld(car, bw::kBodyElem, B)) {
            bw::BodyPoint(B, bw::kWheelRel, wp);
            dWheel = std::sqrt((wp[0] - W[3]) * (wp[0] - W[3]) + (wp[1] - W[7]) * (wp[1] - W[7]) + (wp[2] - W[11]) * (wp[2] - W[11]));
            if (dWheel <= bw::kKeyFrameTolM) {
                bw::BodyPoint(B, bw::kKeyRel, f.keyPos);
                f.haveKeyPoint = true;
                for (int k = 0; k < 12; ++k) { body[k] = B[k]; }
                haveBody = true;   // the same control as the key: a body frame that puts the wheel where it is
            }
        }
        static long s_kfSaid = 0;
        if (s_kfSaid < 3 && (s_kfSaid == 0 || !f.haveKeyPoint)) {
            ++s_kfSaid;
            // [PROBE: BUGGY] gate=buggy_hand_steer  site=HEADSET  log-only=yes
            LOG("buggywheel: [BUGGY] KEY FRAME (first unconditionally, then refusals, <= 3) - body element %d %s; predicted wheel vs the "
                "live steeringwheel %.1f mm (limit %.0f) -> key %s at (%.3f %.3f %.3f).", bw::kBodyElem,
                dWheel < 0.0f ? "NOT READ" : "read", dWheel * 1000.0f, bw::kKeyFrameTolM * 1000.0f,
                f.haveKeyPoint ? "ON" : "*** OFF - the engine starts the game's way ***", f.keyPos[0], f.keyPos[1], f.keyPos[2]);
        }
    }

    // the hands (logic 0 = left, 1 = right; rig side 0 = right, 1 = left; controllers kLeftHand 0 / kRightHand 1)
    float cW[2][9] = {};
    LARGE_INTEGER qf, qn;
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&qn);
    for (int i = 0; i < 2; ++i) {
        const int rigSide = 1 - i;
        long long qpc = 0;
        const bool got = RigGripWorldRead(rigSide, f.hand[i].pos, cW[i], &qpc);
        const double ageMs = qf.QuadPart > 0 ? 1000.0 * static_cast<double>(qn.QuadPart - qpc) / static_cast<double>(qf.QuadPart) : 1e9;
        float sq = 0.0f;
        const bool sqOk = GetControllerSqueeze(i == bw::kLeft ? kLeftHand : kRightHand, sq);
        f.hand[i].tracked = got && ageMs >= 0.0 && ageMs < static_cast<double>(kFreshMs);
        f.hand[i].squeeze = sqOk ? sq : 0.0f;
        for (int k = 0; k < 3; ++k) { f.hand[i].pinch[k] = f.hand[i].pos[k]; }
    }
    // RUN BK3: the key is grabbed by the fingertips (the right hand's pinch point); no hand basis yet = the palm, as before.
    {
        float pinch[3];
        if (f.hand[bw::kRight].tracked && RigGripPinchRead(pinch)) {
            for (int k = 0; k < 3; ++k) { f.hand[bw::kRight].pinch[k] = pinch[k]; }
        }
    }
    XrPosef gp{};
    if (GetControllerGripPose(kRightHand, gp)) {
        const float q[4] = { gp.orientation.x, gp.orientation.y, gp.orientation.z, gp.orientation.w };
        if (g_haveRoll) { g_rollAcc += reload::TwistWindow::TwistDeg(g_lastQ, q); }
        for (int k = 0; k < 4; ++k) { g_lastQ[k] = q[k]; }
        g_haveRoll = true;
    }
    f.hand[bw::kRight].rollDeg = g_rollAcc;

    const bw::Out o = g_logic.Step(f);

    // the decision for the input hook
    LONG bits = 0;
    memcpy(&bits, &o.rawTurn, sizeof(bits));
    InterlockedExchange(&g_rawTurnBits, bits);
    InterlockedExchange(&g_hold, o.holdInputs ? 1 : 0);
    InterlockedExchange(&g_writeTurn, o.writeTurn ? 1 : 0);
    if (o.startTimer) { InterlockedExchange(&g_wantStart, 1); }
    InterlockedExchange64(&g_outMs, static_cast<LONG64>(NowMs()));

    if (InterlockedCompareExchange(&g_firstSample, 1, 0) == 0) {
        // [PROBE: BUGGY] gate=buggy_hand_steer  site=HEADSET  log-only=yes
        LOG("buggywheel: [BUGGY] FIRST SAMPLE (logged unconditionally) - buggy %p, wheel element %d at (%.3f %.3f %.3f), column axis "
            "(%.3f %.3f %.3f), full lock %.1f deg (max rot %.1f x reduction %.3f), shown %.1f deg (sprung %.3f, bump %.2f), engine on "
            "%d, timer %.3f, can start %s, key point %s, hands tracked L %d / R %d.", reinterpret_cast<void*>(car), g_wheelId,
            f.wheelCentre[0], f.wheelCentre[1], f.wheelCentre[2], axis[0], axis[1], axis[2], f.maxWheelDeg, maxRot, reduce, shown, sprung,
            bump, f.engineOn ? 1 : 0, f.timer, f.canStart ? "yes" : "no", f.haveKeyPoint ? "yes" : "NONE (the KEY FRAME check refused it) - the key is off",
            f.hand[0].tracked ? 1 : 0, f.hand[1].tracked ? 1 : 0);
        LogWheelScale(car, g_wheelId, "FIRST SAMPLE, before we place the wheel");
    }

    // the axis self-check (ruling: the column = the element's X): while NOT gripped, the wheel's own rotation between a reference
    // and now should be about our axis by (shown - refShown). Valid only while the buggy itself does not turn (parked, stick steer).
    if (!o.wheelGripped) {
        if (!g_haveRef) { for (int i = 0; i < 12; ++i) { g_refW[i] = W[i]; } g_refShown = shown; g_haveRef = true; }
        else if (std::fabs(shown - g_refShown) > 20.0f && g_axisChecks < 3) {
            const float R0[9] = { g_refW[0], g_refW[1], g_refW[2], g_refW[4], g_refW[5], g_refW[6], g_refW[8], g_refW[9], g_refW[10] };
            const float R1[9] = { W[0], W[1], W[2], W[4], W[5], W[6], W[8], W[9], W[10] };
            float d[9];
            float R0T[9];
            for (int r = 0; r < 3; ++r) { for (int c = 0; c < 3; ++c) { R0T[3 * r + c] = R0[3 * c + r]; } }
            Mat(R1, R0T, d);   // world rotation from ref to now
            const float tr = (d[0] + d[4] + d[8] - 1.0f) * 0.5f;
            const float ang = std::acos(tr > 1.0f ? 1.0f : (tr < -1.0f ? -1.0f : tr)) * 57.2957795f;
            float ax[3] = { d[7] - d[5], d[2] - d[6], d[3] - d[1] };
            Norm(ax);
            const float dot = ax[0] * axis[0] + ax[1] * axis[1] + ax[2] * axis[2];
            InterlockedIncrement(&g_axisChecks);
            // [PROBE: BUGGY] gate=buggy_hand_steer  site=HEADSET  log-only=yes
            LOG("buggywheel: [BUGGY] AXIS CHECK %ld of 3 - the wheel turned %.1f deg in world about an axis whose dot with our column "
                "axis is %+.3f, while the game's shown angle moved %+.1f deg. PASS = angle ~|moved| and dot ~+1 (dot ~-1 = the sign is "
                "reversed; neither = the axis is wrong). Valid only if the buggy did not turn meanwhile.",
                static_cast<long>(g_axisChecks), ang, dot, shown - g_refShown);
            g_haveRef = false;
        }
    } else {
        g_haveRef = false;
    }

    // the wheel you see, at the gripped angle
    if (o.wheelGripped) { PlaceWheel(v, car, g_wheelId, o.wheelDeg); }

    // RUN BK1's gap (Jay: "having both hands on the wheel caused it to fight itself a little" - only grab events were logged): once a
    // second while gripped, each hand's angle about the column (where it is NOW) and grip, the wheel and the raw turn. <= 600 lines.
    if (o.wheelGripped) {
        static ULONGLONG s_gNext = 0;
        static long s_gLines = 0;
        const ULONGLONG gNow = NowMs();
        if (gNow >= s_gNext && s_gLines < 600) {
            s_gNext = gNow + 1000;
            ++s_gLines;
            float ang[2], rad[2];
            for (int i = 0; i < 2; ++i) {
                const float rel[3] = { f.hand[i].pos[0] - f.wheelCentre[0], f.hand[i].pos[1] - f.wheelCentre[1],
                                       f.hand[i].pos[2] - f.wheelCentre[2] };
                ang[i] = bw::SignedAngleAbout(axis, ref, rel);
                const float ax = rel[0] * axis[0] + rel[1] * axis[1] + rel[2] * axis[2];
                const float r2 = rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2] - ax * ax;
                rad[i] = r2 > 0.0f ? std::sqrt(r2) : 0.0f;
            }
            // [PROBE: BUGGY] gate=buggy_hand_steer  site=HEADSET  log-only=yes
            LOG("buggywheel: [BUGGY] GRIP 1 s #%ld - wheel %.1f deg (full lock %.1f), raw turn %.3f | LEFT %s angle %+.1f deg r %.3f m "
                "tracked %d squeeze %.2f | RIGHT %s angle %+.1f deg r %.3f m tracked %d squeeze %.2f", s_gLines, o.wheelDeg, f.maxWheelDeg,
                o.rawTurn, o.pinned[bw::kLeft] ? "GRIPS" : "free ", ang[0], rad[0], f.hand[0].tracked ? 1 : 0, f.hand[0].squeeze,
                o.pinned[bw::kRight] ? "GRIPS" : "free ", ang[1], rad[1], f.hand[1].tracked ? 1 : 0, f.hand[1].squeeze);
            if (s_gLines <= 5) { LogWheelScale(car, g_wheelId, "GRIPPED - our wheel placement writes element 109"); }
        }
    }

    // the hand pins, in the wheel's body frame
    float B[9];
    BodyFrame(axis, ref, B);
    for (int i = 0; i < 2; ++i) {
        const int rigSide = 1 - i;
        const unsigned ev = o.events[i];
        if (ev & (bw::kGrabWheel | bw::kGrabKey)) {
            Grab& g = g_grab[i];
            g.on = true;
            g.wheel = (ev & bw::kGrabWheel) != 0;
            const float rel[3] = { f.hand[i].pos[0] - f.wheelCentre[0], f.hand[i].pos[1] - f.wheelCentre[1], f.hand[i].pos[2] - f.wheelCentre[2] };
            float loc[3];
            MulT(B, rel, loc);
            // RUN BK3 (Jay: "few cm of leeway where the hands could float"): a wheel grip lands ON the rim at the angle grabbed. The
            // key's localPos is unused - its pin is latched on the key point itself (below).
            // RUN BK6 (Jay: "Hands aren't on the wheel. There's an offset"): the pin is the RIM point at the angle grabbed (the mesh rim
            // tube's centre: depth kRimAxialM, radius kRimRadiusM); the hand keeps its own rotation and the arm solve puts the hand's
            // own rim point (kWheelRimFromGrip, from the clip's frame 5 - Jay: "use that as the point of where the wheel is for
            // alignment") on it (kPinRim). BK5's HoldAlign used the clip's POSED handholder, ~7 cm from the rig's grip point - the offset.
            g.hold = false;
            g.alignAxial = bw::kRimAxialM;
            g.alignR = f.rimRadiusM;
            if (g.wheel) { bw::RimGrabLocalAt(loc, g.alignR, g.alignAxial, g.localPos); }
            else { for (int k = 0; k < 3; ++k) { g.localPos[k] = loc[k]; } }
            MatT(B, cW[i], g.localRot);
            g.wheelAtGrab = o.wheelDeg;
            g.thGrab = std::atan2(loc[2], loc[1]) * 57.2957795f;
            ++g.grabN;
            // RUN BK9 (Jay: "Seems to be dependant on position/rotation of the hands prior to putting them on the wheel"): the hand's
            // pose AT THE GRAB against the rim at the angle grabbed - the controller before the rim snap (axial / radial off the rim)
            // and the rig grip basis (cW, what the pin carries) in the rim's own frame: columns = the hand's x / y / z in (axial,
            // radial-out, tangent = +turn). "vs first" = the rotation between this grab's hand-in-rim and this hand's FIRST wheel grab
            // this launch. Event-only (a grab): two 3x3 products + one acos. COST: nothing per frame.
            if (g.wheel) {
                static float s_firstRim[2][9];
                static bool  s_haveFirst[2] = { false, false };
                static long  s_poseLines = 0;
                const float rr = std::sqrt(loc[1] * loc[1] + loc[2] * loc[2]);
                const float th = std::atan2(loc[2], loc[1]) * 57.2957795f;
                float Rr[9], hr[9];
                Rx(th, Rr);
                MatT(Rr, g.localRot, hr);
                const char* vsTxt = "FIRST (the reference)";
                float vsDeg = 0.0f;
                if (!s_haveFirst[i]) {
                    for (int k = 0; k < 9; ++k) { s_firstRim[i][k] = hr[k]; }
                    s_haveFirst[i] = true;
                } else {
                    float dR[9];
                    MatT(s_firstRim[i], hr, dR);
                    const float tr = (dR[0] + dR[4] + dR[8] - 1.0f) * 0.5f;
                    vsDeg = std::acos(tr > 1.0f ? 1.0f : (tr < -1.0f ? -1.0f : tr)) * 57.2957795f;
                    vsTxt = "deg";
                }
                if (++s_poseLines <= 200) {
                    // [PROBE: BUGGY] gate=buggy_hand_steer  site=HEADSET  log-only=yes
                    LOG("buggywheel: [BUGGY] %s GRAB POSE #%ld (first unconditionally, <= 200) - wheel %.1f deg; controller at the grab: "
                        "angle %+.1f deg about the column, axial %+.4f m (rim %+.4f -> off %+.4f), radius %.4f m (rim %.4f -> off %+.4f); "
                        "hand-in-rim (axial radial tangent): x (%+.3f %+.3f %+.3f) y (%+.3f %+.3f %+.3f) z (%+.3f %+.3f %+.3f); vs this "
                        "hand's first wheel grab %.1f %s.", i == bw::kLeft ? "LEFT" : "RIGHT", s_poseLines, o.wheelDeg, th, loc[0],
                        g.alignAxial, loc[0] - g.alignAxial, rr, g.alignR, rr - g.alignR, hr[0], hr[3], hr[6], hr[1], hr[4], hr[7],
                        hr[2], hr[5], hr[8], vsDeg, vsTxt);
                }
                // THE PASS HOLD (kPinRimHold): the DRAWN hand's rotation, in the body frame = Rx(the angle grabbed) x the hold - the
                // controller's rotation (logged above) is not used for a wheel grip. The pin below turns it with the wheel.
                Mat(Rr, kWheelHoldInRim[rigSide], g.localRot);
            }
            Pulse(i, RigTwoHandHaptic, kGrabMs);
        }
        if (ev & bw::kTick) { Pulse(i, kTickAmp, kTickMs); }
        if (ev & bw::kStart) { Pulse(i, kStartAmp, kStartMs); }
        if (ev != bw::kNone && InterlockedIncrement(&g_events) <= 200) {
            // [PROBE: BUGGY] gate=buggy_hand_steer  site=HEADSET  log-only=yes
            LOG("buggywheel: [BUGGY] %s hand:%s%s%s%s - wheel %.1f deg (full lock %.1f), raw turn %.3f, key roll %.1f, engine on %d, "
                "timer %.3f; wheel pin %s (depth %.4f m, radius %.4f m).", i == bw::kLeft ? "LEFT" : "RIGHT",
                (ev & bw::kGrabWheel) ? " GRAB WHEEL" : "",
                (ev & bw::kGrabKey) ? " GRAB KEY" : "", (ev & bw::kTick) ? " TICK" : "", (ev & bw::kStart) ? " START" : "",
                o.wheelDeg, f.maxWheelDeg, o.rawTurn, g_rollAcc, f.engineOn ? 1 : 0, f.timer,
                !(ev & bw::kGrabWheel) ? "-" : "= the rim point, the palm's grip centre on it, the hand in the PASS HOLD (kPinRimHold)",
                g_grab[i].alignAxial, g_grab[i].alignR);
        }
        if (!o.pinned[i]) {
            if (g_grab[i].on) {
                g_grab[i].on = false;
                PublishPin(rigSide, false, nullptr, nullptr, kPinKey);
            }
            continue;
        }
        const Grab& g = g_grab[i];
        float R[9];
        Rx(g.wheel ? (o.wheelDeg - g.wheelAtGrab) : 0.0f, R);
        float lp[3], wp[3], BR[9], lr[9], wr[9];
        Mul(R, g.localPos, lp);
        Mul(B, lp, wp);
        float pos[3] = { f.wheelCentre[0] + wp[0], f.wheelCentre[1] + wp[1], f.wheelCentre[2] + wp[2] };
        // RUN BK3 (Jay: "make the hand LATCH TO THE KEY ONCE GRIPPED"): the key pin IS the key point (the arm solve then sets the pinch on
        // it - boneblend's kKeyPinchFromGrip step), every update, so the hand rides the key with the buggy. The wrist's twist stays live.
        if (!g.wheel) { for (int k = 0; k < 3; ++k) { pos[k] = f.keyPos[k]; } }
        Mat(B, R, BR);
        for (int k = 0; k < 9; ++k) { lr[k] = g.localRot[k]; }
        Mat(BR, lr, wr);
        float Rt[9], rimW[9];   // the rim frame at the pin, turned with the wheel (RUN BK10's drawn-grip line only)
        Rx(g.thGrab, Rt);
        Mat(BR, Rt, rimW);
        PublishPin(rigSide, true, pos, wr, g.wheel ? kPinRimHold : kPinKey, rimW, g.grabN);
    }
}

void Refuse(const char* why)
{
    g_state = -1;
    // [PROBE: BUGGY] gate=buggy_hand_steer  site=FLAT  log-only=yes
    LOG("buggywheel: [BUGGY] REFUSED - %s. The buggy drives exactly as the game does this launch (nothing hooked).", why);
}

template <typename T>
bool Resolve(uintptr_t gd, uintptr_t rva, const unsigned char* head, size_t n, T& out, const char* name)
{
    const uintptr_t at = gd + DLVR_BUG_GD(rva);
    unsigned char got[24] = {};
    if (!HeadIs(at, head, n, got)) {
        char why[220];
        _snprintf_s(why, sizeof(why), _TRUNCATE, "%s at gamedll +%zX: first %zu bytes (%02X %02X %02X %02X ...) are not this build's",
                    name, static_cast<size_t>(at - gd), n, got[0], got[1], got[2], got[3]);
        Refuse(why);
        return false;
    }
    out = reinterpret_cast<T>(at);
    return true;
}

}  // namespace

void BuggyWheelInstall()
{
    if (g_state != 0) { return; }
    if (!BuggyHandSteer) {
        if (!g_saidOff) {
            g_saidOff = true;
            // [PROBE: BUGGY] gate=buggy_hand_steer  site=FLAT  log-only=yes
            LOG("buggywheel: [BUGGY] OFF (buggy_hand_steer=0) - nothing hooked; the buggy drives as the game does.");
        }
        return;
    }
    const uintptr_t gd = reinterpret_cast<uintptr_t>(GetModuleHandleA("gamedll_x64_rwdi.dll"));
    const HMODULE eng = GetModuleHandleA("engine_x64_rwdi.dll");
    if (!gd || !eng) { return; }   // not mapped yet - the 1 Hz caller retries
    g_getId = reinterpret_cast<GetIdFn>(GetProcAddress(eng, "?GetElementID@IModelObject@@QEBAHPEBD@Z"));
    g_getWorld = reinterpret_cast<GetWorldFn>(GetProcAddress(eng, "?GetElementWorldMatrix@IModelObject@@QEBAAEBVmtx34@@H@Z"));
    g_setLocal = reinterpret_cast<SetLocalFn>(GetProcAddress(eng, "?SetElementLocalMatrix@IModelObject@@QEAAXHAEBVmtx34@@@Z"));
    if (!g_getId || !g_getWorld || !g_setLocal) {
        char why[160];
        _snprintf_s(why, sizeof(why), _TRUNCATE, "engine export missing (GetElementID %s, GetElementWorldMatrix %s, SetElementLocalMatrix %s)",
                    g_getId ? "ok" : "MISSING", g_getWorld ? "ok" : "MISSING", g_setLocal ? "ok" : "MISSING");
        Refuse(why);
        return;
    }
    if (!Resolve(gd, kRotRva, kRotHead, sizeof(kRotHead), g_rot, "the wheel rotate +131E490") ||
        !Resolve(gd, kReduceRva, kReduceHead, sizeof(kReduceHead), g_reduce, "the wheel speed reduction +10848D0") ||
        !Resolve(gd, kCanStartRva, kCanStartHead, sizeof(kCanStartHead), g_canStart, "the engine can-start test +105C730") ||
        !Resolve(gd, kInVehRva, kInVehHead, sizeof(kInVehHead), g_inVeh, "the in-a-vehicle test +ECCE30")) {
        return;
    }
    // (1) the input detour FIRST: with no vis slot it writes nothing (no fresh decision).
    const uintptr_t at = gd + DLVR_BUG_GD(kInputRva);
    unsigned char got[24] = {};
    if (!HeadIs(at, kInputHead, sizeof(kInputHead), got)) {
        char why[200];
        _snprintf_s(why, sizeof(why), _TRUNCATE, "the steer-input writer's first 15 bytes at gamedll +%zX (%02X %02X %02X %02X ...) are "
                    "not this build's", static_cast<size_t>(at - gd), got[0], got[1], got[2], got[3]);
        Refuse(why);
        return;
    }
    int attempts = 0;
    // [LOADBEARING: buggy hand steer hooks] (buggy_hand_steer) the steer-input writer +D99730 detour -> InputHook. Without it the key
    //      cannot hold the engine off or start it, and a gripped wheel does not steer.
    if (!InstallSingleGameDetour(at, kInputHead, sizeof(kInputHead), reinterpret_cast<void*>(&InputHook),
                                 reinterpret_cast<void**>(&g_inputTrue), "buggy steer input", &attempts)) {
        char why[120];
        _snprintf_s(why, sizeof(why), _TRUNCATE, "the steer-input detour write was refused after %d attempt(s)", attempts);
        Refuse(why);
        return;
    }
    // (2) the vis slot.
    int st = 0;
    const uintptr_t vt = RttiFindVtable(gd, kVisClass, 0, &st);
    if (!vt) {
        char why[160];
        _snprintf_s(why, sizeof(why), _TRUNCATE, "CarBuggyVis's vtable was not found by name (stage %d; 0 no name, 1 no locator, 2 no "
                    "vtable, -1 faulted) - the input detour stays in and writes nothing", st);
        Refuse(why);
        return;
    }
    const uintptr_t slot = vt + 8 * kVisSlot;
    uintptr_t have = 0;
    if (!ReadPtr(slot, have) || !have || !HeadIs(have, kVisHead, sizeof(kVisHead), got)) {
        char why[220];
        _snprintf_s(why, sizeof(why), _TRUNCATE, "CarBuggyVis slot %zu at gamedll +%zX holds +%zX, whose first 24 bytes are not the "
                    "wheel update's - the input detour stays in and writes nothing", kVisSlot, static_cast<size_t>(slot - gd),
                    static_cast<size_t>(have - gd));
        Refuse(why);
        return;
    }
    g_visTrue = reinterpret_cast<VisFn>(have);
    DWORD old = 0;
    // [LOADBEARING: buggy hand steer hooks] (buggy_hand_steer) CarBuggyVis vtable slot 129 -> VisHook. Without it nothing reads the
    //      hands: no key, no gripped wheel, no hand pins.
    if (!VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), PAGE_READWRITE, &old)) {
        char why[120];
        _snprintf_s(why, sizeof(why), _TRUNCATE, "VirtualProtect on CarBuggyVis slot %zu failed (error %lu)", kVisSlot, GetLastError());
        Refuse(why);
        return;
    }
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot), reinterpret_cast<PVOID>(&VisHook));
    DWORD unused = 0;
    VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), old, &unused);
    uintptr_t back = 0;
    ReadPtr(slot, back);
    g_state = back == reinterpret_cast<uintptr_t>(&VisHook) ? 1 : -1;
    // [PROBE: BUGGY] gate=buggy_hand_steer  site=FLAT  log-only=yes
    LOG("buggywheel: [BUGGY] %s (logged unconditionally) - steer-input detour at gamedll +%zX (15 bytes, %d attempt(s)); CarBuggyVis "
        "slot %zu at gamedll +%zX (vtable +%zX, was +%zX, read back %s); rotate +%zX, reduction +%zX, can-start +%zX, in-vehicle +%zX. "
        "Key: right of the gauges (body element %d) - grip it with the right hand's fingertips (within %.2f m) and turn %.0f deg "
        "clockwise to start; the throttle and brake are held until then. Wheel: grip the rim (radius %.3f m +/- %.2f) to steer; the "
        "hand lands on the rim.",
        g_state == 1 ? "ARMED" : "*** NOT ARMED ***", static_cast<size_t>(at - gd), attempts, kVisSlot, static_cast<size_t>(slot - gd),
        static_cast<size_t>(vt - gd), static_cast<size_t>(have - gd), g_state == 1 ? "OK" : "NOT OUR FUNCTION",
        static_cast<size_t>(reinterpret_cast<uintptr_t>(g_rot) - gd), static_cast<size_t>(reinterpret_cast<uintptr_t>(g_reduce) - gd),
        static_cast<size_t>(reinterpret_cast<uintptr_t>(g_canStart) - gd), static_cast<size_t>(reinterpret_cast<uintptr_t>(g_inVeh) - gd),
        bw::kBodyElem, bw::kKeyGrabRadiusM, bw::kQuarterDeg, kRimRadiusM, bw::kGrabRadiusM);
    if (g_state != 1) { return; }
    // (3) the ignition sound's IsTurningOn slot (the instant start keeps the game's start sound). Refused = a silent instant start.
    {
        int sst = 0;
        const uintptr_t svt = RttiFindVtable(gd, kSndClass, 0, &sst);
        const uintptr_t sslot = svt ? svt + 8 * kSndSlot : 0;
        uintptr_t shave = 0;
        unsigned char sgot[24] = {};
        const char* why = nullptr;
        DWORD sold = 0;
        if (!svt) { why = "BuggyEngineSoundsModule's vtable not found by name"; }
        else if (!ReadPtr(sslot, shave) || !shave || !HeadIs(shave, kSndHead, sizeof(kSndHead), sgot)) { why = "slot 1 is not IsTurningOn's 18 bytes"; }
        else if (!VirtualProtect(reinterpret_cast<void*>(sslot), sizeof(void*), PAGE_READWRITE, &sold)) { why = "VirtualProtect failed"; }
        if (!why) {
            g_sndTrue = reinterpret_cast<SndFn>(shave);
            // [LOADBEARING: buggy hand steer hooks] (buggy_hand_steer) BuggyEngineSoundsModule vtable slot 1 -> IsTurningOnHook: the
            //      instant key start's ignition sound. Without it the instant start is silent (the engine still starts).
            InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(sslot), reinterpret_cast<PVOID>(&IsTurningOnHook));
            DWORD su = 0;
            VirtualProtect(reinterpret_cast<void*>(sslot), sizeof(void*), sold, &su);
            uintptr_t sback = 0;
            ReadPtr(sslot, sback);
            if (sback != reinterpret_cast<uintptr_t>(&IsTurningOnHook)) { g_sndTrue = nullptr; why = "the slot did not read back as ours"; }
        }
        // [PROBE: BUGGY] gate=buggy_hand_steer  site=FLAT  log-only=yes
        LOG("buggywheel: [BUGGY] INSTANT START SOUND %s (logged unconditionally) - BuggyEngineSoundsModule vtable %s slot %zu at gamedll "
            "+%zX (was +%zX, RTTI stage %d)%s%s. The key starts the engine at once (timer 0, no start anim)%s.",
            g_sndTrue ? "HOOKED" : "*** NOT HOOKED ***", svt ? "found" : "NOT FOUND", kSndSlot, sslot ? static_cast<size_t>(sslot - gd) : 0,
            shave ? static_cast<size_t>(shave - gd) : 0, sst, why ? " - " : "", why ? why : "",
            g_sndTrue ? " with the game's ignition sound" : " - SILENTLY (no ignition sound)");
    }
}

bool BuggyHandPin(int side, float pos[3], float cWorld[9], int* mode)
{
    if (side != 0 && side != 1) { return false; }
    for (int attempt = 0; attempt < 8; ++attempt) {
        const LONG s0 = g_pinSeq[side];
        if (s0 & 1) { continue; }
        MemoryBarrier();
        const Pin p = g_pin[side];
        MemoryBarrier();
        if (g_pinSeq[side] != s0) { continue; }
        if (!p.active || NowMs() - static_cast<unsigned long long>(p.ms) > kFreshMs) { return false; }
        for (int i = 0; i < 3; ++i) { pos[i] = p.pos[i]; }
        for (int i = 0; i < 9; ++i) { cWorld[i] = p.rot[i]; }
        if (mode) { *mode = static_cast<int>(p.mode); }
        return true;
    }
    return false;
}

void BuggyNoteDrawnGrip(int side, const float grip[3], const float drawnW[9], float askedM, float drawnM)
{
    if (side != 0 && side != 1) { return; }
    static volatile LONG   s_seen[2] = { 0, 0 };
    static volatile LONG64 s_start[2] = { 0, 0 };
    static volatile LONG   s_logged[2] = { 0, 0 };
    static volatile LONG   s_lines = 0;
    Pin p{};
    bool ok = false;
    for (int attempt = 0; attempt < 8 && !ok; ++attempt) {
        const LONG s0 = g_pinSeq[side];
        if (s0 & 1) { continue; }
        MemoryBarrier();
        p = g_pin[side];
        MemoryBarrier();
        ok = g_pinSeq[side] == s0;
    }
    if (!ok || !p.active || (p.mode != kPinRim && p.mode != kPinRimHold)) { return; }   // torn 8 times: the next solve reads again
    const LONG64 now = static_cast<LONG64>(NowMs());
    if (InterlockedExchange(&s_seen[side], p.grabN) != p.grabN) {   // a new grab: start its 200 ms
        InterlockedExchange64(&s_start[side], now);
        InterlockedExchange(&s_logged[side], 0);
        return;
    }
    if (s_logged[side] || now - s_start[side] < 200) { return; }
    if (InterlockedCompareExchange(&s_logged[side], 1, 0) != 0) { return; }
    if (InterlockedIncrement(&s_lines) > 200) { return; }
    const float off[3] = { grip[0] - p.pos[0], grip[1] - p.pos[1], grip[2] - p.pos[2] };
    float c[3], h[9];
    MulT(p.rim, off, c);
    MatT(p.rim, drawnW, h);
    const float mag = std::sqrt(off[0] * off[0] + off[1] * off[1] + off[2] * off[2]);
    // [PROBE: BUGGY] gate=buggy_hand_steer  site=HEADSET  log-only=yes
    LOG("buggywheel: [BUGGY] %s DRAWN GRIP #%ld (200 ms into wheel grab %ld; first unconditionally, <= 200) - the DRAWN palm's grip "
        "centre vs the wheel pin: axial %+.1f radial %+.1f tangent %+.1f mm (|%.1f| mm); arm asked %.4f m, drawn %.4f m (short %+.1f mm); "
        "drawn hand-in-rim (axial radial tangent): x (%+.3f %+.3f %+.3f) y (%+.3f %+.3f %+.3f) z (%+.3f %+.3f %+.3f).",
        side == 0 ? "RIGHT" : "LEFT", static_cast<long>(s_lines), static_cast<long>(p.grabN), 1000.0f * c[0], 1000.0f * c[1],
        1000.0f * c[2], 1000.0f * mag, askedM, drawnM, 1000.0f * (askedM - drawnM), h[0], h[3], h[6], h[1], h[4], h[7], h[2], h[5],
        h[8]);
}

}  // namespace dlvr
