// Roomscale - Task 7a (the log-only probe) and 7b (the walk body). See roomscale.h.
//
// WHAT IT READS (FINDINGS-ROOMSCALE-STATIC-2026-10-27, OUR engine e9fbfc2c / gamedll 97046bba, Steam rvas - the DEV tree has no
// buildrva table, so any other build REFUSES by name at the vtable / prologue check):
//   PlayerDI = *(*(*(GameDI + 1344) + 184) + 88), GameDI = qword gamedll+0x1C16348 (the CScriptCondition chain, rs4);
//   its control object = PlayerDI + 0x18; IControlObject::PhysicsBind (engine export) -> CPhysicsBind vfunc +0xB0 = the walk
//   interface; full object = interface - 0x700, its vtable must be CODEPhysicsWalkFly's engine +86DC38 (rs1/rs2);
//   wanted velocity full+0xA70, additional velocity +0xA94, move-by-position +0x7E8, lock +0x850, physics off +0x2B1, walk state
//   (u16) +0x9C0; the main body *(full+0x710) (iface slot 88 = [iface+0x10]) and its position body+0xBC; the controller group
//   PlayerDI+3324 (S5); the control object's world position (IControlObject::GetWorldPosition).
// 7b WRITES one thing: the player walk body's wanted velocity, inside the engine's own pre-step (+3D6F50, full vtable slot 9,
// (full, dt in xmm1)), as the game's value + roomscale's extra on x / z (Mamfro's merge rule: a value other than the one we last
// wrote is the game's new one). The engine's own collision then stops the body at walls.
// THE SHARE (plan amendment 2026-10-27, ledgered): the view is shifted only by what ROOMSCALE moved, never by stick walking. Each
// pre-step measures the step before it: body delta - the game's wanted velocity x that step's dt, projected on that step's extra
// and clamped to [0, |extra| x dt]. Mamfro's public code measures each step at the next one the same way; its roomscale crate
// (monaka_vr/crates/body) is not public, so the share rule is ours.
#include "roomscale.h"

#include "boneblend.h"   // InstallSingleGameDetour - the shared verify / suspend / commit install
#include "camhook.h"
#include "headpose.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

// The Epic (EGL) build: the GameDI global and the walk vtable are on tools/egl_allowlist.tsv (family 10) and translated here; the
// pre-step detour address is handed over as the Steam rva - InstallSingleGameDetour translates it itself (buildrva::Abs), so it must
// not be translated twice. The DEV tree has no buildrva.h: Steam rvas only (any other build refuses by name at the vtable check).
#if __has_include("../common/buildrva.h")
#include "../common/buildrva.h"
#define DLVR_RS_GD(rva) ::dlvr::buildrva::Gd(rva)
#define DLVR_RS_EN(rva) ::dlvr::buildrva::Eng(rva)
#else
#define DLVR_RS_GD(rva) (rva)
#define DLVR_RS_EN(rva) (rva)
#endif

namespace dlvr {

// [DEP: diag_roomscale_probe] required-for=[ROOMSCALE] - Task 7a's log-only probe of the walk body, its guards, the controller group
//      and the app -> world map (plan 2026-10-27-roomscale-physical-crouch, amended). tree=INERT (log-only, writes nothing).
//      HEADSET for the map (a tracked head + the rendered camera); the walk-body / group reads are flat-valid structurally. DEFAULT OFF.
bool DiagRoomscaleProbe = false;
// [DEP: roomscale_enable] required-for=roomscale (RoomscaleEnable) - real steps move the character through the walk body's own
//      physics (plan Task 7b; spec §2.2, Jay: "Add roomscale in", "New key for both"). tree=LOADBEARING while 1 (the pre-step detour
//      writes the player's wanted velocity; ovr2xr.cpp shifts appSpace by roomscale's measured share). 0 = the detour, if installed,
//      passes every step straight through and the drift bleed (roomscale_bleed_mps) runs instead. HEADSET. DEFAULT ON 2026-10-27 -
//      Tier A (Jay verified: run 4 "View stayed on my body, stick walking felt normal"; ticked "Roomscale" at session end).
bool  RoomscaleEnable = true;
// [DEP: roomscale_lean_max_m] required-for=how far the view may lean past a body that cannot follow (a wall, a stand-down state)
//      before the rest is absorbed into the origin (RoomscaleLeanMaxM; Jay: "That's correct" to the wall rule). tree=INERT at
//      roomscale_enable=0. Tier D.
float RoomscaleLeanMaxM = 0.25f;

namespace {

constexpr uintptr_t kGameDiRva = 0x1C16348;    // gamedll qword_181C16348 (GameDI)
constexpr uintptr_t kWalkFullVtRva = 0x86DC38;  // engine CODEPhysicsWalkFly full-object vtable (RTTI, rs1)
constexpr uintptr_t kPreStepRva = 0x3D6F50;     // engine CODEPhysicsWalkFly pre-step (full vtable slot 9) - rs1 / rs2
constexpr unsigned char kPreStepBytes[18] = {   // movss [rsp+10h], xmm1 / push rbp rsi rdi r12 r14 / lea rbp, [rsp-70h]
    0xF3, 0x0F, 0x11, 0x4C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0x90 };   // no RIP-relative
constexpr const char* kPhysicsBindExport = "?PhysicsBind@IControlObject@@QEAAPEAVIPhysicsBind@@XZ";
constexpr const char* kWorldPosExport = "?GetWorldPosition@IControlObject@@QEBA?AVvec3@@XZ";

using PhysicsBindFn = void*(__fastcall*)(void* control);
using GetterFn = void*(__fastcall*)(void* self);
using WorldPosFn = float*(__fastcall*)(void* control, float* out);
using PreStepFn = void(__fastcall*)(void* full, float dt);

int        g_state = 0;   // 0 not tried, 1 ready, -1 refused (named once)
uintptr_t  g_gd = 0, g_en = 0;
PhysicsBindFn g_bind = nullptr;
WorldPosFn g_worldPos = nullptr;
ULONGLONG  g_lastMs = 0;
long       g_lines = 0;
constexpr long kMaxLines = 2400;   // INV-9: 2 a second -> 20 minutes; the cap guards a forgotten switch
bool       g_vtSaid = false;
uintptr_t  g_lastVt = 0;
bool       g_havePrev = false;
float      g_prevHead[2] = {}, g_prevCam[2] = {};

// 7b shared state. g_playerFull / g_group: written by the game thread, read by the detour and the render thread.
volatile uintptr_t g_playerFull = 0;
volatile LONG      g_group = -99;
PreStepFn          g_origPreStep = nullptr;
volatile LONG      g_hook = 0;          // 0 not tried, 1 live, -1 refused
SRWLOCK            g_lock = SRWLOCK_INIT;
float              g_cmd[2] = {};       // render -> detour: world x/z velocity
float              g_moved[2] = {};     // detour -> render: world x/z roomscale share, accumulated
long               g_steps = 0, g_commanded = 0, g_guarded = 0, g_gameChanged = 0;
// detour-only (one writer: the physics step of the player's body)
bool               g_stepPrev = false;
float              g_prevPos[2] = {}, g_prevGame[2] = {}, g_prevExtra[2] = {}, g_prevDt = 0.0f;
float              g_gameWanted[3] = {}, g_written[3] = { NAN, NAN, NAN };
bool               g_wroteLast = false;

bool RdQ(uintptr_t a, uintptr_t* out)
{
    __try { *out = *reinterpret_cast<const uintptr_t*>(a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool RdF(uintptr_t a, float* out)
{
    __try { *out = *reinterpret_cast<const float*>(a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool RdB(uintptr_t a, unsigned char* out)
{
    __try { *out = *reinterpret_cast<const unsigned char*>(a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool RdW(uintptr_t a, unsigned short* out)
{
    __try { *out = *reinterpret_cast<const unsigned short*>(a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool RdI(uintptr_t a, int* out)
{
    __try { *out = *reinterpret_cast<const int*>(a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool WrF3(uintptr_t a, const float v[3])
{
    __try { std::memcpy(reinterpret_cast<void*>(a), v, 12); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* CallBind(void* control)
{
    __try { return g_bind(control); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
void* CallGetter(uintptr_t fn, void* self)
{
    __try { return reinterpret_cast<GetterFn>(fn)(self); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
bool CallWorldPos(void* control, float out[3])
{
    __try { g_worldPos(control, out); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool Resolve()
{
    if (g_state != 0) { return g_state == 1; }
    const HMODULE gd = GetModuleHandleA("gamedll_x64_rwdi.dll");
    const HMODULE en = GetModuleHandleA("engine_x64_rwdi.dll");
    if (!gd || !en) { return false; }   // not mapped yet: tried again next update (no line - the ARMED / REFUSED line follows)
    g_gd = reinterpret_cast<uintptr_t>(gd);
    g_en = reinterpret_cast<uintptr_t>(en);
    g_bind = reinterpret_cast<PhysicsBindFn>(GetProcAddress(en, kPhysicsBindExport));
    g_worldPos = reinterpret_cast<WorldPosFn>(GetProcAddress(en, kWorldPosExport));
    if (!g_bind || !g_worldPos) {
        g_state = -1;
        // [PROBE: ROOMSCALE] gate=diag_roomscale_probe,roomscale_enable  site=FLAT  log-only=yes
        LOG("[ROOMSCALE] REFUSED - the engine does not export %s%s - roomscale and its probe stay off this launch",
            g_bind ? "" : kPhysicsBindExport, g_worldPos ? "" : kWorldPosExport);
        return false;
    }
    g_state = 1;
    // [PROBE: ROOMSCALE] gate=diag_roomscale_probe,roomscale_enable  site=FLAT  log-only=yes
    LOG("[ROOMSCALE] ARMED (logged unconditionally) - roomscale_enable %d, probe %d | PhysicsBind %p, GetWorldPosition %p | GameDI "
        "gamedll+%llX, walk full vtable expected engine+%llX (Steam), pre-step engine+%llX",
        RoomscaleEnable ? 1 : 0, DiagRoomscaleProbe ? 1 : 0, reinterpret_cast<void*>(g_bind), reinterpret_cast<void*>(g_worldPos),
        static_cast<unsigned long long>(kGameDiRva), static_cast<unsigned long long>(kWalkFullVtRva),
        static_cast<unsigned long long>(kPreStepRva));
    return true;
}

// The player's walk body, resolved from scratch (no cached entity - U-50). Returns the full object or 0 with *why.
uintptr_t ResolveWalk(uintptr_t* playerOut, int* groupOut, uintptr_t* vtOut, const char** why)
{
    *playerOut = 0;
    *vtOut = 0;
    *why = "no player";
    uintptr_t gdi = 0, a = 0, b = 0, player = 0;
    if (!(RdQ(g_gd + DLVR_RS_GD(kGameDiRva), &gdi) && gdi && RdQ(gdi + 1344, &a) && a && RdQ(a + 184, &b) && b && RdQ(b + 88, &player) &&
          player)) {
        return 0;
    }
    *playerOut = player;
    RdI(player + 3324, groupOut);
    void* bind = CallBind(reinterpret_cast<void*>(player + 0x18));
    uintptr_t bvt = 0, getter = 0;
    if (!bind) { *why = "PhysicsBind returned null"; return 0; }
    if (!RdQ(reinterpret_cast<uintptr_t>(bind), &bvt) || !RdQ(bvt + 0xB0, &getter) || !getter) {
        *why = "the bind's walk getter (vfunc +0xB0) is unreadable";
        return 0;
    }
    void* iface = CallGetter(getter, bind);
    if (!iface) { *why = "the bind has no walk physics (getter returned null)"; return 0; }
    const uintptr_t full = reinterpret_cast<uintptr_t>(iface) - 0x700;
    uintptr_t vt = 0;
    if (!RdQ(full, &vt)) { *why = "the walk object is unreadable"; return 0; }
    *vtOut = vt;
    if (vt - g_en != DLVR_RS_EN(kWalkFullVtRva)) {
        *why = "the walk object's vtable is not CODEPhysicsWalkFly's (Steam +86DC38 / its Epic translation) - REFUSED";
        return 0;
    }
    *why = "ok";
    return full;
}

// The pre-step detour: every CODEPhysicsWalkFly body steps through here (NPCs too) - anything but the player's body is one compare
// and the original. COST for the player's body: ~12 guarded reads, one 12-byte write, one SRW lock, a dozen flops per physics step.
void __fastcall PreStepDetour(void* full, float dt)
{
    const uintptr_t me = reinterpret_cast<uintptr_t>(full);
    if (me == 0 || me != g_playerFull) {
        g_origPreStep(full, dt);
        return;
    }
    uintptr_t body = 0;
    float pos[2] = { NAN, NAN };
    if (RdQ(me + 0x710, &body) && body) { RdF(body + 0xBC, &pos[0]); RdF(body + 0xC4, &pos[1]); }
    float wanted[3] = { NAN, NAN, NAN };
    for (int i = 0; i < 3; ++i) { RdF(me + 0xA70 + 4 * i, &wanted[i]); }
    unsigned char mbp = 1, lock = 1, off = 1;
    RdB(me + 0x7E8, &mbp);
    RdB(me + 0x850, &lock);
    RdB(me + 0x2B1, &off);
    const bool guarded = mbp || lock || off;

    // The share of the LAST step roomscale caused: body delta - the game's own wanted x dt, along the extra, clamped.
    float shareX = 0.0f, shareZ = 0.0f;
    if (g_stepPrev && std::isfinite(pos[0]) && std::isfinite(pos[1])) {
        const float ex = g_prevExtra[0], ez = g_prevExtra[1];
        const float elen = std::sqrt(ex * ex + ez * ez);
        if (elen > 1e-4f && g_prevDt > 0.0f) {
            const float rx = (pos[0] - g_prevPos[0]) - g_prevGame[0] * g_prevDt;
            const float rz = (pos[1] - g_prevPos[1]) - g_prevGame[1] * g_prevDt;
            float along = (rx * ex + rz * ez) / elen;
            const float maxAlong = elen * g_prevDt;
            along = along < 0.0f ? 0.0f : (along > maxAlong ? maxAlong : along);
            shareX = ex / elen * along;
            shareZ = ez / elen * along;
        }
    }

    // The game's own wanted velocity: a value other than the one we wrote last is the game's new one.
    bool changed = false;
    if (std::isfinite(wanted[0]) && std::isfinite(wanted[2])) {
        const bool ours = g_wroteLast && wanted[0] == g_written[0] && wanted[1] == g_written[1] && wanted[2] == g_written[2];
        if (!ours) {
            changed = g_wroteLast;
            g_gameWanted[0] = wanted[0]; g_gameWanted[1] = wanted[1]; g_gameWanted[2] = wanted[2];
        }
    }
    float cmd[2] = {};
    AcquireSRWLockExclusive(&g_lock);
    g_moved[0] += shareX;
    g_moved[1] += shareZ;
    if (RoomscaleEnable && !guarded) { cmd[0] = g_cmd[0]; cmd[1] = g_cmd[1]; }
    ++g_steps;
    if (guarded) { ++g_guarded; }
    if (cmd[0] != 0.0f || cmd[1] != 0.0f) { ++g_commanded; }
    if (changed) { ++g_gameChanged; }
    ReleaseSRWLockExclusive(&g_lock);

    // Write game + extra (x, z; y the game's) - or give the game its own value back when there is nothing to add.
    g_wroteLast = false;
    if (std::isfinite(g_gameWanted[0]) && (cmd[0] != 0.0f || cmd[1] != 0.0f || wanted[0] != g_gameWanted[0] ||
                                           wanted[2] != g_gameWanted[2])) {
        const float w[3] = { g_gameWanted[0] + cmd[0], g_gameWanted[1], g_gameWanted[2] + cmd[1] };
        if (WrF3(me + 0xA70, w)) {
            g_written[0] = w[0]; g_written[1] = w[1]; g_written[2] = w[2];
            g_wroteLast = cmd[0] != 0.0f || cmd[1] != 0.0f;
            if (!g_wroteLast) { g_written[0] = g_written[1] = g_written[2] = NAN; }
        }
    }
    g_stepPrev = std::isfinite(pos[0]) && std::isfinite(pos[1]);
    g_prevPos[0] = pos[0]; g_prevPos[1] = pos[1];
    g_prevGame[0] = g_gameWanted[0]; g_prevGame[1] = g_gameWanted[2];
    g_prevExtra[0] = cmd[0]; g_prevExtra[1] = cmd[1];
    g_prevDt = dt;
    g_origPreStep(full, dt);
}

void InstallOnce()
{
    if (g_hook != 0) { return; }
    int attempts = 0;
    void* tramp = nullptr;
    const bool ok = InstallSingleGameDetour(g_en + kPreStepRva, kPreStepBytes, sizeof(kPreStepBytes),
                                            reinterpret_cast<void*>(&PreStepDetour), &tramp, "roomscale walk pre-step", &attempts);
    if (ok) {
        g_origPreStep = reinterpret_cast<PreStepFn>(tramp);
        InterlockedExchange(&g_hook, 1);
    } else {
        InterlockedExchange(&g_hook, -1);
    }
    // [PROBE: ROOMSCALE] gate=roomscale_enable  site=FLAT  log-only=no
    LOG("[ROOMSCALE] pre-step detour (engine+%llX, %zu bytes verified) %s after %d attempt(s)%s",
        static_cast<unsigned long long>(kPreStepRva), sizeof(kPreStepBytes), ok ? "LIVE" : "REFUSED", attempts,
        ok ? "" : " - its bytes differ from engine e9fbfc2c's, or the write was refused; roomscale does nothing this launch");
}

void ProbeLine(uintptr_t player, int group, uintptr_t full, uintptr_t vt, const char* walkWhy)
{
    float world[3] = { NAN, NAN, NAN };
    float wanted[3] = { NAN, NAN, NAN }, extra[3] = { NAN, NAN, NAN };
    unsigned char mbp = 0xFF, lock = 0xFF, off = 0xFF;
    unsigned short wstate = 0xFFFF;
    if (player) { CallWorldPos(reinterpret_cast<void*>(player + 0x18), world); }
    if (full) {
        for (int i = 0; i < 3; ++i) { RdF(full + 0xA70 + 4 * i, &wanted[i]); RdF(full + 0xA94 + 4 * i, &extra[i]); }
        RdB(full + 0x7E8, &mbp);
        RdB(full + 0x850, &lock);
        RdB(full + 0x2B1, &off);
        RdW(full + 0x9C0, &wstate);
    }
    if (!g_vtSaid || vt != g_lastVt) {
        g_vtSaid = true;
        g_lastVt = vt;
        // [PROBE: ROOMSCALE] gate=diag_roomscale_probe,roomscale_enable  site=FLAT  log-only=yes
        LOG("[ROOMSCALE] walk body %s | PlayerDI %p, full object %p, vtable engine+%llX", walkWhy, reinterpret_cast<void*>(player),
            reinterpret_cast<void*>(full), vt ? static_cast<unsigned long long>(vt - g_en) : 0ULL);
    }
    if (!DiagRoomscaleProbe || g_lines >= kMaxLines) { return; }
    XrPosef head{};
    float hx = NAN, hz = NAN;
    if (GetHeadPose(head)) {
        const XrVector3f rep = NeckPivotReportedPos(head);
        hx = rep.x;
        hz = rep.z;
    }
    const float fold = HeadFollowFoldedDeg();
    float cam[12] = {};
    unsigned camAge = 0;
    const bool haveCam = CamHookGetRenderedViewMatrix(cam, &camAge);
    float yaw = NAN;
    const bool haveYaw = CamHookGetOpenXrToGameYaw(yaw);
    // The pair since the last line: the head's app delta, the camera's world delta, and the yaw's prediction of the world delta
    // (world = Ry(yaw) * app, x/z - the hypothesis 7a tests; the sign convention printed is the formula used, not a finding).
    float dhx = NAN, dhz = NAN, dcx = NAN, dcz = NAN, px = NAN, pz = NAN;
    if (haveCam && std::isfinite(hx) && g_havePrev) {
        dhx = hx - g_prevHead[0];
        dhz = hz - g_prevHead[1];
        dcx = cam[3] - g_prevCam[0];
        dcz = cam[11] - g_prevCam[1];
        if (haveYaw) {
            const float c = std::cos(yaw), s = std::sin(yaw);
            px = c * dhx + s * dhz;
            pz = -s * dhx + c * dhz;
        }
    }
    if (haveCam && std::isfinite(hx)) {
        g_havePrev = true;
        g_prevHead[0] = hx; g_prevHead[1] = hz;
        g_prevCam[0] = cam[3]; g_prevCam[1] = cam[11];
    }
    ++g_lines;
    // [PROBE: ROOMSCALE] gate=diag_roomscale_probe  site=HEADSET  log-only=yes
    LOG("[ROOMSCALE] 0.5 s #%ld | group %d | control world (%.3f, %.3f, %.3f) | walk %s: wanted (%.2f, %.2f, %.2f) extra (%.2f, "
        "%.2f, %.2f) move-by-pos %u lock %u physics-off %u state 0x%04X | head app (%.3f, %.3f) fold %.1f deg | camera world (%.3f, "
        "%.3f, %.3f) age %u ms%s | yaw %.2f deg%s | delta head app (%.3f, %.3f) camera world (%.3f, %.3f) Ry(yaw) predicts (%.3f, "
        "%.3f)%s",
        g_lines, group, world[0], world[1], world[2], walkWhy, wanted[0], wanted[1], wanted[2], extra[0], extra[1], extra[2],
        static_cast<unsigned>(mbp), static_cast<unsigned>(lock), static_cast<unsigned>(off), static_cast<unsigned>(wstate), hx, hz,
        fold, cam[3], cam[7], cam[11], camAge, haveCam ? "" : " (NO CAMERA)", haveYaw ? yaw * 57.29578f : 0.0f,
        haveYaw ? "" : " (NO YAW)", dhx, dhz, dcx, dcz, px, pz, g_lines == kMaxLines ? " | LAST ROOMSCALE line" : "");
}

}  // namespace

// COST: both off = two bool tests. On = one tick read per update; every 0.25 s the walk body resolved from scratch (~10 guarded
// reads, two engine calls - PhysicsBind and the bind's walk getter, the game's own); with the probe also GetWorldPosition and three
// layer getters every 0.5 s. The detour install runs once. Game thread (HookTail).
void RoomscaleProbeTick()
{
    if (!DiagRoomscaleProbe && !RoomscaleEnable) { return; }
    if (!Resolve()) { return; }
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastMs < 250) { return; }
    g_lastMs = now;
    static ULONGLONG s_lastProbeMs = 0;
    const bool probeDue = DiagRoomscaleProbe && now - s_lastProbeMs >= 500;
    if (probeDue) { s_lastProbeMs = now; }
    uintptr_t player = 0, vt = 0;
    int group = -99;
    const char* why = "no player";
    const uintptr_t full = ResolveWalk(&player, &group, &vt, &why);
    g_playerFull = full;
    InterlockedExchange(&g_group, group);
    if (RoomscaleEnable && full && g_hook == 0) { InstallOnce(); }
    if (probeDue || !g_vtSaid || vt != g_lastVt) { ProbeLine(player, group, full, vt, why); }
}

bool RoomscaleLive() { return g_hook == 1 && g_playerFull != 0; }
int  RoomscaleGroup() { return static_cast<int>(g_group); }
bool RoomscaleOnFoot(int group) { return (group >= 0 && group <= 6) || group == 25; }

void RoomscaleTakeMoved(float* wx, float* wz)
{
    AcquireSRWLockExclusive(&g_lock);
    *wx = g_moved[0];
    *wz = g_moved[1];
    g_moved[0] = g_moved[1] = 0.0f;
    ReleaseSRWLockExclusive(&g_lock);
}

void RoomscaleSetCommand(float vx, float vz)
{
    AcquireSRWLockExclusive(&g_lock);
    g_cmd[0] = vx;
    g_cmd[1] = vz;
    ReleaseSRWLockExclusive(&g_lock);
}

void RoomscaleTakeStepStats(long* steps, long* commanded, long* guarded, long* gameChanged)
{
    AcquireSRWLockExclusive(&g_lock);
    *steps = g_steps; *commanded = g_commanded; *guarded = g_guarded; *gameChanged = g_gameChanged;
    g_steps = g_commanded = g_guarded = g_gameChanged = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace dlvr
