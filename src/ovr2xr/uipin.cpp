// MENU PIN, stage 1 - see uipin.h (what, why, cost) and docs/SPEC-UI-MENU-PIN-2026-10-11.md. The maths is uipin_logic.h.
#include "uipin.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "../common/buildrva.h"   // the allowlisted EGL rvas for the engine slots read here
#include "../common/log.h"
#include "boneblend.h"     // InstallSingleGameDetour - the shared verify / suspend / commit path
#include "camhook.h"       // CamHookGetRenderedViewMatrix - the head-rotated camera, for the main menu
#include "uipanel.h"       // UiPanelMenus - the panel arms the main-menu rule too
#include "uipin_logic.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)
#include "periodic_log.h"   // LOG_WINDOW - the 1 s status lines (2026-11-08 log trim)

namespace dlvr {

// REMOVED 2026-10-11 (Jay: "Remove ui_pin_menus & ui_panel_main_menu. Also remove ui_pin_distance_m slider."): the settings
// ui_pin_menus / ui_pin_distance_m and the PAUSE-MENU camera rule they alone switched (it moved one box only, INV-530; the
// panel does that job now). What this file still does, both under ui_panel_menus (uipanel.h): MenuState() for the panel,
// and the MAIN-MENU rule (verified by Jay: "Main menu now latches without dropping"). Dependencies traced before the
// removal: the three names were read only in this file, uipanel.cpp and the settings table (INV-536).

// [DEP: ui_panel_main_menu_turn] required-for=the main menu turning with the head side to side (UiPanelMainMenuTurn; INV-537 add. 1-3): while the main menu is up, camhook adds the head's yaw at ONE camera copy - the view's cached render camera, the root of the chain - which this file names each submit (CamHookSetMainMenuTurnRoot). Needs ui_panel_menus (it arms this file's level walk and detour) and horizon lock / camhook levelling as in every normal session. In game it does nothing. VERIFIED by Jay 2026-10-11 on v4 (layer afe777f8: "No culling, still turns with head and the direction is right", then "that was much better" / "The switch itself is good. Ship on & Hide") - Tier A, LOCKED ON and hidden (S_BOOL_LOCKED). Also needs declare_rendered_orientation + head_yaw_decouple (both locked on) for the frames to be declared with the root's yaw (INV-537 add. 8-9). Known, left alone on his word: a stale capture for ~2 s after a long frame (add. 9); a 1-2 frame text shift on a page change.  tree=INERT
bool UiPanelMainMenuTurn = true;

namespace uipin {

namespace {

typedef uintptr_t (*Fn4)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef uintptr_t (*FnRebuild)(uintptr_t);

// OUR engine e9fbfc2c. +2E6BF0: ghidra-project/ida/ui28_engine_out.txt (four whole instructions, 15 bytes, no
// RIP-relative operand - the bytes the UICAM probe ran on for two headset sessions). +764270: the camera's rebuild
// of its derived fields (INV-260), called, never patched - its first bytes are checked before the first call.
constexpr uintptr_t kRva         = 0x2E6BF0;
const unsigned char kExpect[15]  = { 0x48, 0x8B, 0xC4, 0x53, 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00, 0x0F, 0x29, 0x70, 0xE8 };
constexpr size_t    kStolen      = 15;
constexpr uintptr_t kRebuildRva  = 0x764270;
const unsigned char kRebuildExpect[16] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8D, 0x41, 0x10, 0x48, 0x8B, 0xD9, 0x48, 0x8D, 0x51 };
constexpr uintptr_t kSceneCamRva = 0xAAAE40;   // the camera the 3D eyes are built on, as camhook left it (INV-261)
constexpr uintptr_t kSceneRootRva  = 0xAAABF0; // sub_1802B4EF0's per-frame copy of the view's cached camera (INV-261)
constexpr uintptr_t kSceneViewRva  = 0xAADDA8; // the CLevelView sub_1802B4EF0 last set the scene up from (U-52)
constexpr size_t    kViewCachedCam = 0x90398;  // CLevelView: pointer to its cached render camera (INV-261, INV-537 add. 3)

constexpr size_t kCamView    = 0x10;      // CCamera: the inverse (world -> camera) matrix
constexpr size_t kCamMatrix  = 0x40;      // CCamera: the world matrix (INV-260)
// the levels (INV-528 (A), INV-529)
constexpr uintptr_t kGameSlot = 0xA3F3B8;
constexpr size_t kGameImpl = 8, kGameLevels = 224, kGameLevelCnt = 232;
constexpr size_t kLevelILevel = 168, kLevelUi = 0x568;
constexpr size_t kUiCam = 5608, kUiVisArr = 424, kUiVisCnt = 432;
constexpr size_t kScrName = 968;
const char kMenuClass[]   = "MainMenuModuleDI";
const char kPauseScreen[] = "MenuInGame";

constexpr float kRigidTol       = 0.02f;
constexpr float kMainMenuReach  = 2.0f;   // the scene camera must be this near the menu's own camera (metres), or it is not the menu's
constexpr int   kTimedEvery     = 64;
constexpr int   kFirstLines     = 6;      // "first matched call" lines per session, per mode

enum : LONG { kNone = 0, kMainMenu = 1, kPause = 2 };

struct Snap {
    LONG  mode;
    float dist;
    Mat34 s0;
};

Fn4            g_true = nullptr;
FnRebuild      g_rebuild = nullptr;
uintptr_t      g_eng = 0;
volatile LONG  g_installed = 0, g_installFailed = 0;

// ---- what the per-frame walk publishes for the job threads (a seqlock: odd while it is being written)
volatile LONG64 g_snapCam = 0;            // the menu level's UI camera; 0 = no rule this frame
volatile LONG   g_seq = 0;
Snap            g_snap = {};

// ---- counters (any thread)
volatile LONG g_calls[3] = {};            // [mode] matched calls the rule rewrote
volatile LONG g_passDet = 0;              // matched camera, but its handedness is not this mode's (a menu changing state)
volatile LONG g_refBusy = 0, g_refSrc = 0, g_refScene = 0, g_refFar = 0, g_refOut = 0, g_faults = 0;
volatile LONG g_refNoView = 0;            // main menu: no camera written at the menu position yet (nothing to hold either)
// the main menu's latch: the last good head-rotated camera, kept while the main menu is up
SRWLOCK       g_holdLock = SRWLOCK_INIT;
Mat34         g_hold = {};
bool          g_holdValid = false;
volatile LONG g_mmFresh = 0, g_mmHeld = 0;   // matched calls drawn from a fresh write / from the held one
volatile LONG64 g_viewCacheCam = 0;          // the menu view's cached render camera (skipped by the latch), 0 = none
constexpr float    kMenuViewTolM = 0.5f;     // a written camera counts as the menu's within this of its position (eyes sit 3 cm off)
constexpr unsigned kMenuViewFreshMs = 250;
volatile LONG g_timed = 0;
volatile LONG64 g_timedTicks = 0;
volatile LONG g_firstSaid[3] = {};
double        g_tickUs = 0.0;

bool ReadMat(uintptr_t at, Mat34& m)
{
    __try {
        memcpy(m.m, reinterpret_cast<const void*>(at), sizeof(m.m));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The cached render camera of the view the engine last set its scene up from; 0 = not readable / none.
uintptr_t ReadTurnRoot()
{
    __try {
        const uintptr_t view = *reinterpret_cast<const uintptr_t*>(g_eng + ::dlvr::buildrva::Eng(kSceneViewRva));
        return view ? *reinterpret_cast<const uintptr_t*>(view + kViewCachedCam) : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool ReadSnap(Snap& out)
{
    const LONG a = g_seq;
    if (a & 1) { return false; }
    out = g_snap;
    return g_seq == a;
}

float TurnDeg(const Mat34& rel)      // how far a relative transform has turned the view direction
{
    float c = rel.m[10];
    c = c > 1.0f ? 1.0f : c < -1.0f ? -1.0f : c;
    return std::acos(c) * 57.29578f;
}

// One matched call: the engine's copy `out` of the menu's UI camera `cam` is replaced. True when it was rewritten.
bool Apply(uintptr_t cam, uintptr_t out)
{
    Snap sn;
    if (!ReadSnap(sn)) { InterlockedIncrement(&g_refBusy); return false; }
    if (sn.mode == kNone) { return false; }
    Mat34 src, eng, scene;
    if (!ReadMat(cam + kCamMatrix, src) || !ReadMat(out + kCamMatrix, eng)) {
        InterlockedIncrement(&g_faults);
        return false;
    }
    if (sn.mode == kMainMenu) {
        // v2 (INV-534 (4)): at the main menu engine+AAAE40 has NO head in it when the UI is collected (run 1: it equalled
        // the menu's own camera exactly). The head-rotated camera is the one camhook writes: its yaw anchor's matrix as
        // written, at most 150 ms old. COST: a 12-float seqlock read + one tick read per matched call.
        // v3 - THE PERMANENT LATCH (INV-535 add. 1; Jay: "We want the latch behaviour to be permanent"): not the yaw
        // anchor's matrix (the anchor is re-decided for 2 s at a time here) but the newest camera camhook wrote AT THE MENU
        // CAMERA's OWN POSITION - its rotation only, on the menu camera's position. And once one has been seen it is KEPT:
        // a second with no fresh write uses the last good rotation instead of dropping back to the game's drawing.
        const float sp[3] = { src.m[3], src.m[7], src.m[11] };
        unsigned age = 0;
        Mat34 w;
        const bool fresh = CamHookGetWrittenViewNear(sp, kMenuViewTolM, w.m, &age, static_cast<uintptr_t>(g_viewCacheCam)) &&
                           age <= kMenuViewFreshMs &&
                           IsRigid(w, kRigidTol) && Det(w) > 0.0f;
        AcquireSRWLockExclusive(&g_holdLock);
        if (fresh) { g_hold = w; g_holdValid = true; }
        const bool have = g_holdValid;
        if (have) { scene = g_hold; }
        ReleaseSRWLockExclusive(&g_holdLock);
        if (!have) { InterlockedIncrement(&g_refNoView); return false; }
        InterlockedIncrement(fresh ? &g_mmFresh : &g_mmHeld);
        scene.m[3] = sp[0]; scene.m[7] = sp[1]; scene.m[11] = sp[2];
    } else {
        return false;            // only the main menu has a rule (the pause-menu rule was removed 2026-10-11)
    }
    if (!IsRigid(src, kRigidTol)) { InterlockedIncrement(&g_refSrc); return false; }
    if (!IsRigid(scene, kRigidTol) || !(Det(scene) > 0.0f)) { InterlockedIncrement(&g_refScene); return false; }
    const float e = EyeOffsetOf(src, eng);
    const float det = Det(src);
    Mat34 n;
    float k = 1.0f;
    if (sn.mode == kMainMenu) {
        if (!(det > 0.0f)) { InterlockedIncrement(&g_passDet); return false; }
        const float dx = scene.m[3] - src.m[3], dy = scene.m[7] - src.m[7], dz = scene.m[11] - src.m[11];
        if (!(dx * dx + dy * dy + dz * dz < kMainMenuReach * kMainMenuReach)) { InterlockedIncrement(&g_refFar); return false; }
        n = MainMenu(scene, e);
    } else {
        return false;
    }
    if (!IsRigid(n, kRigidTol)) { InterlockedIncrement(&g_refOut); return false; }
    const Mat34 inv = Inverse(n);
    __try {
        memcpy(reinterpret_cast<void*>(out + kCamMatrix), n.m, sizeof(n.m));
        memcpy(reinterpret_cast<void*>(out + kCamView), inv.m, sizeof(inv.m));
        g_rebuild(out);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_faults); return false; }
    InterlockedIncrement(&g_calls[sn.mode]);
    if (g_firstSaid[sn.mode] < kFirstLines && InterlockedIncrement(&g_firstSaid[sn.mode]) <= kFirstLines) {
        const Mat34 rel = sn.mode == kPause ? Relative(sn.s0, scene) : Relative(src, scene);
        // [PROBE: UIPIN] gate=ui_panel_menus  site=HEADSET  log-only=no
        LOG("[UIPIN] FIRST %s calls (logged unconditionally, %d a session): thread %lu, eye offset %.4f | menu UI camera "
            "det %.3f pos (%.3f %.3f %.3f) back (%.3f %.3f %.3f) | scene camera pos (%.3f %.3f %.3f) back (%.3f %.3f %.3f) "
            "| %s: turned %.3f deg, moved (%.4f %.4f %.4f) m | units per metre %.3f | engine's copy pos (%.3f %.3f %.3f) "
            "-> ours pos (%.3f %.3f %.3f) back (%.3f %.3f %.3f)",
            sn.mode == kPause ? "PAUSE-MENU" : "MAIN-MENU", kFirstLines, static_cast<unsigned long>(GetCurrentThreadId()), e,
            det, src.m[3], src.m[7], src.m[11], src.m[2], src.m[6], src.m[10], scene.m[3], scene.m[7], scene.m[11],
            scene.m[2], scene.m[6], scene.m[10],
            sn.mode == kPause ? "scene camera since the menu opened" : "scene camera against the menu's own camera (= the head)",
            TurnDeg(rel), rel.m[3], rel.m[7], rel.m[11], k, eng.m[3], eng.m[7], eng.m[11], n.m[3], n.m[7], n.m[11], n.m[2],
            n.m[6], n.m[10]);
    }
    return true;
}

uintptr_t Detour(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    const uintptr_t r = g_true(a, b, c, d);
    if (!UiPanelMenus || !b || static_cast<uintptr_t>(g_snapCam) != b) { return r; }
    const LONG n = g_calls[kMainMenu] + g_calls[kPause];
    if ((n % kTimedEvery) != 0) {
        Apply(b, c);
        return r;
    }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    const bool did = Apply(b, c);
    QueryPerformanceCounter(&t1);
    if (did) {
        InterlockedIncrement(&g_timed);
        InterlockedAdd64(&g_timedTicks, t1.QuadPart - t0.QuadPart);
    }
    return r;
}

// ---- the per-frame walk (submit thread only)

// The class name of the object at `obj` from its vtable's RTTI (x64 complete object locator). Caller holds the guard.
bool ClassIs(uintptr_t obj, const char* want)
{
    const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(obj);
    if (!vt) { return false; }
    const uintptr_t col = *reinterpret_cast<const uintptr_t*>(vt - 8);
    if (!col || *reinterpret_cast<const uint32_t*>(col) != 1) { return false; }
    const uint32_t tdRva = *reinterpret_cast<const uint32_t*>(col + 12);
    const uint32_t selfRva = *reinterpret_cast<const uint32_t*>(col + 20);
    const char* name = reinterpret_cast<const char*>(col - selfRva + tdRva + 16);   // ".?AVName@@"
    const size_t n = strlen(want);
    return name[0] == '.' && strncmp(name + 4, want, n) == 0 && name[4 + n] == '@';
}

struct Found {
    uintptr_t level, ui, cam;
    bool pause;          // the pause menu's screen is among the visible ones
    int  levels;         // non-null levels walked, -1 = the game object was not readable
};

// The menu level, by plain reads. The class is looked up only for a level pointer (with its game-side vtable) not
// seen before; the answer is remembered.
bool FindMenuLevel(Found& f)
{
    static uintptr_t s_knownLevel = 0, s_knownVt = 0;
    static bool s_knownIsMenu = false;
    f = Found{};
    __try {
        const uintptr_t game = *reinterpret_cast<const uintptr_t*>(g_eng + ::dlvr::buildrva::Eng(kGameSlot));
        const uintptr_t impl = game ? *reinterpret_cast<const uintptr_t*>(game + kGameImpl) : 0;
        if (!impl) { f.levels = -1; return false; }
        const uintptr_t arr = *reinterpret_cast<const uintptr_t*>(impl + kGameLevels);
        const unsigned  n   = *reinterpret_cast<const unsigned*>(impl + kGameLevelCnt);
        if (!arr || n > 64) { f.levels = arr ? -1 : 0; return false; }
        for (unsigned i = 0; i < n; ++i) {
            const uintptr_t lv = *reinterpret_cast<const uintptr_t*>(arr + 8ull * i);
            if (!lv) { continue; }
            ++f.levels;
            const uintptr_t il = *reinterpret_cast<const uintptr_t*>(lv + kLevelILevel);
            if (!il || *reinterpret_cast<const uintptr_t*>(il + 8) != lv) { continue; }
            const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(il);
            bool isMenu;
            if (lv == s_knownLevel && vt == s_knownVt) {
                isMenu = s_knownIsMenu;
            } else {
                isMenu = ClassIs(il, kMenuClass);
                if (isMenu) { s_knownLevel = lv; s_knownVt = vt; s_knownIsMenu = true; }
            }
            if (!isMenu) { continue; }
            f.level = lv;
            f.ui = *reinterpret_cast<const uintptr_t*>(lv + kLevelUi);
            if (!f.ui) { return false; }
            f.cam = *reinterpret_cast<const uintptr_t*>(f.ui + kUiCam);
            const uintptr_t scr = *reinterpret_cast<const uintptr_t*>(f.ui + kUiVisArr);
            const unsigned  ns  = *reinterpret_cast<const unsigned*>(f.ui + kUiVisCnt);
            for (unsigned k = 0; scr && k < ns && k < 64; ++k) {
                const uintptr_t e = *reinterpret_cast<const uintptr_t*>(scr + 8ull * k);
                const char* nm = e ? *reinterpret_cast<const char* const*>(e + kScrName) : nullptr;
                if (nm && strcmp(nm, kPauseScreen) == 0) { f.pause = true; break; }
            }
            return f.cam != 0;
        }
        return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { f = Found{}; f.levels = -1; return false; }
}

void Publish(uintptr_t cam, LONG mode, const Mat34* s0, float dist)
{
    InterlockedIncrement(&g_seq);                 // odd: being written
    g_snap.mode = mode;
    g_snap.dist = dist;
    if (s0) { g_snap.s0 = *s0; }
    InterlockedIncrement(&g_seq);
    InterlockedExchange64(&g_snapCam, static_cast<LONG64>(mode == kNone ? 0 : cam));
}

void Install()
{
    const HMODULE mod = GetModuleHandleA("engine_x64_rwdi.dll");
    g_eng = reinterpret_cast<uintptr_t>(mod);
    if (!g_eng) {
        InterlockedExchange(&g_installFailed, 1);
        // [PROBE: UIPIN] gate=ui_panel_menus  site=HEADSET  log-only=no
        LOG("[UIPIN] *** REFUSED: engine_x64_rwdi.dll is not loaded - the menus are NOT pinned this session. ***");
        return;
    }
    bool rebuildOk = false;
    __try {
        rebuildOk = memcmp(reinterpret_cast<const void*>(g_eng + ::dlvr::buildrva::Eng(kRebuildRva)), kRebuildExpect, sizeof(kRebuildExpect)) == 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { rebuildOk = false; }
    if (!rebuildOk) {
        InterlockedExchange(&g_installFailed, 1);
        LOG("[UIPIN] *** REFUSED: engine+764270 (the camera rebuild) does not start with the bytes of engine e9fbfc2c - "
            "another engine build; the menus are NOT pinned this session. ***");
        return;
    }
    g_rebuild = reinterpret_cast<FnRebuild>(g_eng + ::dlvr::buildrva::Eng(kRebuildRva));
    LARGE_INTEGER fq;
    QueryPerformanceFrequency(&fq);
    g_tickUs = fq.QuadPart > 0 ? 1000000.0 / static_cast<double>(fq.QuadPart) : 0.0;
    int attempts = 0;
    // g_true is written by InstallDetour BEFORE the jump is committed, so a thread entering the detour the instant it
    // goes live already has the trampoline.
    const bool ok = InstallSingleGameDetour(g_eng + kRva, kExpect, kStolen, reinterpret_cast<void*>(&Detour),
                                            reinterpret_cast<void**>(&g_true), "UI per-eye camera (+2E6BF0), menu pin", &attempts);
    if (ok) { InterlockedExchange(&g_installed, 1); } else { InterlockedExchange(&g_installFailed, 1); }
    LOG("[UIPIN] %s - detour at engine+2E6BF0: %s (commit attempts %d). The MAIN MENU's screens (level class %s) are drawn "
        "from the head-rotated camera camhook writes, so they stay on their scene objects. Every other menu and the HUD untouched "
        "by this detour (the in-game menus are the panel's).",
        ok ? "ON (ui_panel_menus=1)" : "*** NOT INSTALLED - the main menu follows the head this session ***",
        ok ? "LIVE" : attempts == 0 ? "REFUSED - prologue mismatch (not engine e9fbfc2c?)" : "NOT COMMITTED - threads busy",
        attempts, kMenuClass);
}

}  // namespace

bool Installed() { return g_installed != 0; }

// For the menu panel (uipanel.cpp): the same level walk, no detour needed. Submit thread only (FindMenuLevel's cache).
int MenuState(bool* pauseScreen)
{
    if (pauseScreen) { *pauseScreen = false; }
    if (!g_eng) {
        g_eng = reinterpret_cast<uintptr_t>(GetModuleHandleA("engine_x64_rwdi.dll"));
        if (!g_eng) { return -1; }
    }
    Found f = {};
    if (!FindMenuLevel(f)) { return f.levels < 0 ? -1 : 0; }
    if (pauseScreen) { *pauseScreen = f.pause; }
    Mat34 camM = {};
    if (!ReadMat(f.cam + kCamMatrix, camM) || !IsRigid(camM, kRigidTol)) { return -1; }
    return Det(camM) > 0.0f ? 1 : 2;
}

// Game-menu input pass-through (uipin.h): the submit thread publishes, the pad thread reads.
namespace {
volatile LONG   g_pubMenuState = -2;   // -2 = never published
volatile LONG64 g_pubMenuMs = 0;
volatile LONG   g_pubPause = -2;       // HudHiddenForPause() at the last publish; -2 = never (trace 20261113_1750_pause_freeze)
}

void PublishMenuState()
{
    const int s = MenuState();
    InterlockedExchange(&g_pubMenuState, s);
    // [LOADBEARING: pause_freeze] the game's PauseGame hide reason, once per submit, for the rig's freeze (boneblend.cpp PauseFreezeOn).
    InterlockedExchange(&g_pubPause, HudHiddenForPause());
    InterlockedExchange64(&g_pubMenuMs, static_cast<LONG64>(GetTickCount64()));
}

bool GamePausedForMenu()
{
    if (InterlockedCompareExchange(&g_pubPause, 0, 0) != 1) { return false; }
    const LONG64 t = InterlockedCompareExchange64(&g_pubMenuMs, 0, 0);
    const ULONGLONG now = GetTickCount64();
    return t != 0 && now >= static_cast<ULONGLONG>(t) && now - static_cast<ULONGLONG>(t) <= 250;
}

int PublishedMenuState(unsigned* ageMs)
{
    const LONG s = InterlockedCompareExchange(&g_pubMenuState, 0, 0);
    const LONG64 t = InterlockedCompareExchange64(&g_pubMenuMs, 0, 0);
    if (ageMs) {
        const ULONGLONG now = GetTickCount64();
        *ageMs = t == 0 ? 0xFFFFFFFFu : static_cast<unsigned>(now > static_cast<ULONGLONG>(t) ? now - static_cast<ULONGLONG>(t) : 0);
    }
    return static_cast<int>(s);
}

bool GameMenuOpen()
{
    unsigned age = 0;
    return PublishedMenuState(&age) == 2 && age <= 250;
}

// ---- HUD PANEL (uipanel.cpp, ui_hud_panel; docs/SPEC-UI-HUD-PANEL-2026-10-11.md §4.2 / §10 U2): "the HUD is showing".
// The same level array FindMenuLevel walks: a level whose class is LevelDI with a screen named HUD in its visible list
// (INV-529: LevelDI shows HUD + HUD_Overlay in play, drops HUD while a menu is up; a load lists no LevelDI).
namespace {

const char kPlayClass[] = "LevelDI";
const char kHudScreen[] = "HUD";

// The RTTI class name of the object at `obj` into `out` (without the ".?AV" / "@@"). Caller holds the guard.
void ClassNameOf(uintptr_t obj, char* out, size_t cap)
{
    out[0] = 0;
    const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(obj);
    if (!vt) { return; }
    const uintptr_t col = *reinterpret_cast<const uintptr_t*>(vt - 8);
    if (!col || *reinterpret_cast<const uint32_t*>(col) != 1) { return; }
    const uint32_t tdRva = *reinterpret_cast<const uint32_t*>(col + 12);
    const uint32_t selfRva = *reinterpret_cast<const uint32_t*>(col + 20);
    const char* name = reinterpret_cast<const char*>(col - selfRva + tdRva + 16);   // ".?AVName@@"
    if (name[0] != '.') { return; }
    size_t n = 0;
    for (const char* p = name + 4; *p && *p != '@' && n + 1 < cap; ++p) { out[n++] = *p; }
    out[n] = 0;
}

// Appends `s` to the text in `out` if it fits whole (a plain function: the callers hold an SEH guard).
void Append(char* out, size_t cap, size_t& used, const char* s)
{
    const size_t n = strlen(s);
    if (used + n + 1 >= cap) { return; }
    memcpy(out + used, s, n);
    used += n;
    out[used] = 0;
}

}  // namespace

// Submit thread only (the remembered level). COST: the level walk - a few dozen pointer reads; the class of a level is
// read from its own RTTI (five reads and a compare) and the play level's is remembered.
int HudShowing()
{
    static uintptr_t s_playLevel = 0, s_playVt = 0;
    if (!g_eng) {
        g_eng = reinterpret_cast<uintptr_t>(GetModuleHandleA("engine_x64_rwdi.dll"));
        if (!g_eng) { return -1; }
    }
    __try {
        const uintptr_t game = *reinterpret_cast<const uintptr_t*>(g_eng + ::dlvr::buildrva::Eng(kGameSlot));
        const uintptr_t impl = game ? *reinterpret_cast<const uintptr_t*>(game + kGameImpl) : 0;
        if (!impl) { return -1; }
        const uintptr_t arr = *reinterpret_cast<const uintptr_t*>(impl + kGameLevels);
        const unsigned  n   = *reinterpret_cast<const unsigned*>(impl + kGameLevelCnt);
        if (!arr) { return 0; }
        if (n > 64) { return -1; }
        for (unsigned i = 0; i < n; ++i) {
            const uintptr_t lv = *reinterpret_cast<const uintptr_t*>(arr + 8ull * i);
            if (!lv) { continue; }
            const uintptr_t il = *reinterpret_cast<const uintptr_t*>(lv + kLevelILevel);
            if (!il || *reinterpret_cast<const uintptr_t*>(il + 8) != lv) { continue; }
            const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(il);
            if (!(lv == s_playLevel && vt == s_playVt)) {
                if (!ClassIs(il, kPlayClass)) { continue; }
                s_playLevel = lv; s_playVt = vt;
            }
            const uintptr_t ui = *reinterpret_cast<const uintptr_t*>(lv + kLevelUi);
            if (!ui) { return 0; }
            const uintptr_t scr = *reinterpret_cast<const uintptr_t*>(ui + kUiVisArr);
            const unsigned  ns  = *reinterpret_cast<const unsigned*>(ui + kUiVisCnt);
            for (unsigned k = 0; scr && k < ns && k < 64; ++k) {
                const uintptr_t e = *reinterpret_cast<const uintptr_t*>(scr + 8ull * k);
                const char* nm = e ? *reinterpret_cast<const char* const*>(e + kScrName) : nullptr;
                if (nm && strcmp(nm, kHudScreen) == 0) { return 1; }
            }
            return 0;
        }
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// MENU-OPEN PAUSE HOLD (trace docs/traces/20261113_1100_menuopen_pause_hold.md; IDA ghidra-project/ida/hhr5..hhr20, our gamedll
// 97046bba): the HUD manager HUDDIManager / BtzHudManager at [gamedll + 0x1C16358] keeps its hide reasons in a list -
// HideUnhideRequest (+555D10): head +1928, hide counter +1960; each node { int reason; node* next; node* prev }, the list pointer
// points at the node's `next` field, so the reason is at pointer - 8 and the next pointer at pointer + 0. Reason 1 = PauseGame
// (+94D00), added ONLY by GameDI::PauseGame (+44C410), which opens a menu / dialog / end screen for every mode. Epic: the same
// functions by fingerprint, the global via buildrva (allowlisted, buildrva_table.h). Nothing written; guarded reads.
#if __has_include("../common/buildrva.h")
#define DLVR_UP_GD(rva) ::dlvr::buildrva::Gd(rva)
#else
#define DLVR_UP_GD(rva) (rva)
#endif
namespace {
constexpr uintptr_t kHudMgrRva    = 0x1C16358;   // the HUD manager global (holster.cpp kHhMgrRva, INV-570)
constexpr uintptr_t kHideHeadOff  = 1928;        // reason list head (HideUnhideRequest +555D10)
constexpr uintptr_t kHideCountOff = 1960;        // hide counter (0 = the HUD is not hidden by any reason)
constexpr int       kReasonPause  = 1;           // EHudHideReason PauseGame
constexpr int       kMaxReasons   = 16;          // ten reasons exist; a longer walk = not this list
}  // namespace

int HudHiddenForPause()
{
    static uintptr_t s_gd = 0;
    if (!s_gd) {
        s_gd = reinterpret_cast<uintptr_t>(GetModuleHandleA("gamedll_x64_rwdi.dll"));
        if (!s_gd) { return -1; }
    }
    __try {
        const uintptr_t mgr = *reinterpret_cast<const uintptr_t*>(s_gd + DLVR_UP_GD(kHudMgrRva));
        if (!mgr) { return -1; }
        if (*reinterpret_cast<const unsigned*>(mgr + kHideCountOff) == 0) { return 0; }
        uintptr_t p = *reinterpret_cast<const uintptr_t*>(mgr + kHideHeadOff);
        for (int i = 0; p && i < kMaxReasons; ++i) {
            if (*reinterpret_cast<const int*>(p - 8) == kReasonPause) { return 1; }
            p = *reinterpret_cast<const uintptr_t*>(p);
        }
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// LOADING-SCREEN VOID (loadscreen.cpp, amendment 1): the visible screens of the first level of class `cls`. The same walk as
// HudShowing; nothing remembered. Submit thread only.
int LevelScreensShowing(const char* cls)
{
    if (!cls) { return -1; }
    if (!g_eng) {
        g_eng = reinterpret_cast<uintptr_t>(GetModuleHandleA("engine_x64_rwdi.dll"));
        if (!g_eng) { return -1; }
    }
    __try {
        const uintptr_t game = *reinterpret_cast<const uintptr_t*>(g_eng + ::dlvr::buildrva::Eng(kGameSlot));
        const uintptr_t impl = game ? *reinterpret_cast<const uintptr_t*>(game + kGameImpl) : 0;
        if (!impl) { return -1; }
        const uintptr_t arr = *reinterpret_cast<const uintptr_t*>(impl + kGameLevels);
        const unsigned  n   = *reinterpret_cast<const unsigned*>(impl + kGameLevelCnt);
        if (!arr) { return 0; }
        if (n > 64) { return -1; }
        for (unsigned i = 0; i < n; ++i) {
            const uintptr_t lv = *reinterpret_cast<const uintptr_t*>(arr + 8ull * i);
            if (!lv) { continue; }
            const uintptr_t il = *reinterpret_cast<const uintptr_t*>(lv + kLevelILevel);
            if (!il || *reinterpret_cast<const uintptr_t*>(il + 8) != lv) { continue; }
            if (!ClassIs(il, cls)) { continue; }
            const uintptr_t ui = *reinterpret_cast<const uintptr_t*>(lv + kLevelUi);
            if (!ui) { return 0; }
            const uintptr_t scr = *reinterpret_cast<const uintptr_t*>(ui + kUiVisArr);
            const unsigned  ns  = *reinterpret_cast<const unsigned*>(ui + kUiVisCnt);
            int shown = 0;
            for (unsigned k = 0; scr && k < ns && k < 64; ++k) {
                const uintptr_t e = *reinterpret_cast<const uintptr_t*>(scr + 8ull * k);
                if (e && *reinterpret_cast<const char* const*>(e + kScrName)) { ++shown; }
            }
            return shown;
        }
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// CALIBRATION SCREEN SOUND (calibsound.cpp, build D task 6): the UI system the game's own PlaySound2D reaches from a widget
// (IObject +24 -> CObject +40, RESEARCH-CALIB-AUDIO ROUTE A 3/4). The same walk as HudShowing, two passes: the
// MainMenuModuleDI level (the main menu and every in-game menu, INV-529) first, then the LevelDI level; its level +0x568.
// NOTHING is remembered across calls - a level change between two plays is read fresh (the plan's review focus).
// Submit thread only. COST: up to two level walks with an RTTI class read per listed level (a few dozen pointer reads) -
// called only on a calibration-screen sound event, never per frame.
uintptr_t UiSystemForSound(const char** levelClass)
{
    if (levelClass) { *levelClass = "none"; }
    if (!g_eng) {
        g_eng = reinterpret_cast<uintptr_t>(GetModuleHandleA("engine_x64_rwdi.dll"));
        if (!g_eng) { return 0; }
    }
    const char* const passes[2] = { kMenuClass, kPlayClass };
    __try {
        const uintptr_t game = *reinterpret_cast<const uintptr_t*>(g_eng + ::dlvr::buildrva::Eng(kGameSlot));
        const uintptr_t impl = game ? *reinterpret_cast<const uintptr_t*>(game + kGameImpl) : 0;
        if (!impl) { return 0; }
        const uintptr_t arr = *reinterpret_cast<const uintptr_t*>(impl + kGameLevels);
        const unsigned  n   = *reinterpret_cast<const unsigned*>(impl + kGameLevelCnt);
        if (!arr || n > 64) { return 0; }
        for (int pass = 0; pass < 2; ++pass) {
            for (unsigned i = 0; i < n; ++i) {
                const uintptr_t lv = *reinterpret_cast<const uintptr_t*>(arr + 8ull * i);
                if (!lv) { continue; }
                const uintptr_t il = *reinterpret_cast<const uintptr_t*>(lv + kLevelILevel);
                if (!il || *reinterpret_cast<const uintptr_t*>(il + 8) != lv) { continue; }
                if (!ClassIs(il, passes[pass])) { continue; }
                const uintptr_t ui = *reinterpret_cast<const uintptr_t*>(lv + kLevelUi);
                if (!ui) { continue; }
                if (levelClass) { *levelClass = passes[pass]; }
                return ui;
            }
        }
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (levelClass) { *levelClass = "read fault"; }
        return 0;
    }
}

// For the log line at a change of the HUD signal only (never per frame): "Class: Screen Screen | Class: (none) | ...".
void DescribeLevels(char* out, size_t cap)
{
    if (!out || cap < 8) { return; }
    out[0] = 0;
    if (!g_eng) { strncpy_s(out, cap, "engine not loaded", _TRUNCATE); return; }
    size_t used = 0;
    __try {
        const uintptr_t game = *reinterpret_cast<const uintptr_t*>(g_eng + ::dlvr::buildrva::Eng(kGameSlot));
        const uintptr_t impl = game ? *reinterpret_cast<const uintptr_t*>(game + kGameImpl) : 0;
        if (!impl) { Append(out, cap, used, "game object not readable"); return; }
        const uintptr_t arr = *reinterpret_cast<const uintptr_t*>(impl + kGameLevels);
        const unsigned  n   = *reinterpret_cast<const unsigned*>(impl + kGameLevelCnt);
        if (!arr || n > 64) { Append(out, cap, used, arr ? "level count not sane" : "no level array"); return; }
        int listed = 0;
        for (unsigned i = 0; i < n; ++i) {
            const uintptr_t lv = *reinterpret_cast<const uintptr_t*>(arr + 8ull * i);
            if (!lv) { continue; }
            if (listed++) { Append(out, cap, used, " | "); }
            const uintptr_t il = *reinterpret_cast<const uintptr_t*>(lv + kLevelILevel);
            char cls[64] = "?";
            if (il && *reinterpret_cast<const uintptr_t*>(il + 8) == lv) { ClassNameOf(il, cls, sizeof(cls)); }
            Append(out, cap, used, cls[0] ? cls : "?");
            Append(out, cap, used, ":");
            const uintptr_t ui = *reinterpret_cast<const uintptr_t*>(lv + kLevelUi);
            const uintptr_t scr = ui ? *reinterpret_cast<const uintptr_t*>(ui + kUiVisArr) : 0;
            const unsigned  ns  = ui ? *reinterpret_cast<const unsigned*>(ui + kUiVisCnt) : 0;
            int shown = 0;
            for (unsigned k = 0; scr && k < ns && k < 64; ++k) {
                const uintptr_t e = *reinterpret_cast<const uintptr_t*>(scr + 8ull * k);
                const char* nm = e ? *reinterpret_cast<const char* const*>(e + kScrName) : nullptr;
                if (!nm) { continue; }
                char one[48];
                strncpy_s(one, sizeof(one), nm, _TRUNCATE);
                Append(out, cap, used, " ");
                Append(out, cap, used, one);
                ++shown;
            }
            if (!shown) { Append(out, cap, used, " (no visible screen)"); }
        }
        if (!listed) { Append(out, cap, used, "no levels listed"); }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { strncpy_s(out, cap, "*** read fault while listing the levels ***", _TRUNCATE); }
}

void Tick()
{
    // ui_panel_menus arms this file - for the MAIN MENU only (its screens stay on their scene objects under the head; the
    // panel handles the in-game menus).
    const bool on = UiPanelMenus;
    if (!g_installed) {
        if (!on || g_installFailed) { return; }
        Install();
        if (!g_installed) { return; }
    }
    static LONG s_mode = kNone;
    static Mat34 s_s0 = {};
    static uintptr_t s_cam = 0;
    static long s_opens = 0, s_noAnchor = 0, s_frames = 0;
    static int s_levels = 0;
    static ULONGLONG s_next = 0;
    static uintptr_t s_turnRoot = 0;
    static long s_turnNoRoot = 0;
    ++s_frames;

    LONG mode = kNone;
    Found f = {};
    Mat34 camM = {}, scene = {};
    if (on && FindMenuLevel(f)) {
        if (ReadMat(f.cam + kCamMatrix, camM) && IsRigid(camM, kRigidTol)) {
            mode = Det(camM) > 0.0f ? kMainMenu : kNone;
        }
    }
    s_levels = f.levels;
    // the main menu's latch: camhook records every camera it writes only while the main menu is up; leaving it drops
    // the held rotation (the next main menu starts from a fresh one)
    CamHookWantWrittenViews(mode == kMainMenu);
    // ui_panel_main_menu_turn (INV-537 add. 3): tell camhook which ONE camera gets the head's yaw - the cached render
    // camera of the view the engine last set its scene up from (engine+AADDA8, written in sub_1802B4EF0; the cache
    // pointer is view+0x90398). Only while the main menu is up and the switch is on; anything else clears it.
    {
        uintptr_t root = 0;
        // v3 (INV-537 add. 6): back to v1's object - the view's CACHED render camera, the one copy BOTH the cull side
        // (AAABF0) and the eyes (AAAE40) are made from, which is why v1 turned the picture with no culling fault. v1 only
        // worked while the menu camera moved because camhook itself stops levelling a camera whose arriving matrix is
        // still; camhook now levels this one object on every copy while it is named here. (v2's AAABF0 was the cull
        // side only - add. 5.)
        if (mode == kMainMenu && UiPanelMainMenuTurn) {
            root = ReadTurnRoot();
            if (!root) { ++s_turnNoRoot; }
        }
        InterlockedExchange64(&g_viewCacheCam, 0);     // nothing is skipped by the latch: the cached camera carries the yaw
        CamHookSetMainMenuTurnRoot(root);
        s_turnRoot = root;
    }
    if (mode != kMainMenu && s_mode == kMainMenu) {
        AcquireSRWLockExclusive(&g_holdLock);
        g_holdValid = false;
        ReleaseSRWLockExclusive(&g_holdLock);
    }
    if (mode != s_mode || f.cam != s_cam) {
        Publish(f.cam, mode, nullptr, 0.0f);
        if (mode != s_mode) {
            // [PROBE: UIPIN] gate=ui_panel_menus  site=HEADSET  log-only=no
            LOG("[UIPIN] mode %s -> %s (menu level %p, UI camera %p, levels listed %d)",
                s_mode == kPause ? "PAUSE-MENU" : s_mode == kMainMenu ? "MAIN-MENU" : "none",
                mode == kPause ? "PAUSE-MENU" : mode == kMainMenu ? "MAIN-MENU" : "none", reinterpret_cast<void*>(f.level),
                reinterpret_cast<void*>(f.cam), f.levels);
            if (mode == kMainMenu) {
                long c = 0, r = 0;
                uintptr_t cullAt = 0, viewAt = 0;
                CamHookMainMenuViewSwitchStats(&c, &r, &cullAt, &viewAt);
                // [PROBE: UIPIN] gate=ui_panel_menus + ui_panel_main_menu_turn  site=HEADSET  log-only=no
                LOG("[UIPIN] main menu page switch catch: %s (root %p, cull copy at %p, scene view at %p)%s", s_turnRoot ?
                    "ARMED" : "*** NOT ARMED - no root named (ui_panel_main_menu_turn off, or no scene view at engine+AADDA8) ***",
                    reinterpret_cast<void*>(s_turnRoot), reinterpret_cast<void*>(cullAt), reinterpret_cast<void*>(viewAt),
                    (s_turnRoot && !viewAt) ? " *** engine addresses NOT resolved - no catch ***" : "");
            }
        }
        s_mode = mode;
        s_cam = f.cam;
    }

    const ULONGLONG now = GetTickCount64();
    if (now < s_next) { return; }
    s_next = now + 1000;
    if (s_mode == kNone && !on) { return; }      // switched off again: nothing to say
    {
        static long long s_wvLast = 0;
        const long long wv = CamHookWrittenViewCount();
        const LONG nv = InterlockedExchange(&g_refNoView, 0), fr = InterlockedExchange(&g_mmFresh, 0),
                   hd = InterlockedExchange(&g_mmHeld, 0);
        if (s_mode == kMainMenu || nv || fr || hd) {
            // [PROBE: UIPIN] gate=ui_panel_menus  site=HEADSET  log-only=no
            LOG_PERIODIC("[UIPIN] main menu latch, 1 s: calls drawn from a FRESH camera write %ld, from the HELD last good one %ld, LEFT AS "
                "THE GAME DREW THEM (no camera written at the menu position yet) %ld | cameras camhook wrote this second %lld",
                static_cast<long>(fr), static_cast<long>(hd), static_cast<long>(nv), wv - s_wvLast);
        }
        s_wvLast = wv;
        if (UiPanelMainMenuTurn && (s_mode == kMainMenu || s_turnNoRoot)) {
            long hits = 0;
            float added = 0.0f, engYaw = 0.0f;
            CamHookMainMenuTurnStats(&hits, &added, &engYaw);
            long dOk = 0, dNoTag = 0, dNoYaw = 0;
            float dMax = 0.0f;
            CamHookMainMenuDeclStats(&dOk, &dNoTag, &dNoYaw, &dMax);
            // [PROBE: UIPIN] gate=ui_panel_menus + ui_panel_main_menu_turn  site=HEADSET  log-only=no
            LOG_PERIODIC("[UIPIN] main menu head turn, 1 s: root camera %p (the view's cached render camera) | head yaw added at the root "
                "%ld time(s), of them %ld on a STILL page (levelled only because it is the root) | last added %.2f deg | the "
                "root's OWN heading before the head %.2f deg (must stay put while the head turns - a value that creeps means the "
                "yaw is stacking) | submits with no root (no scene view at engine+AADDA8) %ld | SUBMITS DECLARED WITH THE ROOT'S "
                "YAW %ld (largest declared-vs-located yaw %.2f deg), left on the located pose: no tagged capture %ld, tagged "
                "capture without the yaw %ld (needs declare_rendered_orientation=1)%s",
                reinterpret_cast<void*>(s_turnRoot), hits, CamHookMainMenuTurnForced(), static_cast<double>(added),
                static_cast<double>(engYaw), s_turnNoRoot, dOk, static_cast<double>(dMax), dNoTag, dNoYaw,
                hits ? "" : " | *** added 0 times: camhook did not level that camera this second - the main menu still rides the head ***");
            s_turnNoRoot = 0;
        }
        {
            long c = 0, r = 0;
            uintptr_t cullAt = 0, viewAt = 0;
            CamHookMainMenuViewSwitchStats(&c, &r, &cullAt, &viewAt);
            if (c || r) {
                // [PROBE: UIPIN] gate=ui_panel_menus + ui_panel_main_menu_turn  site=HEADSET  log-only=no
                LOG("[UIPIN] main menu page switch, 1 s: CAUGHT %ld (the new page's camera given the head yaw in the same frame), "
                    "REFUSED %ld (cull copy source was not the drawn view's camera - not caught) | root now %p",
                    c, r, reinterpret_cast<void*>(s_turnRoot));
            }
        }
    }
    const LONG cm = InterlockedExchange(&g_calls[kMainMenu], 0), cp = InterlockedExchange(&g_calls[kPause], 0);
    const LONG timed = InterlockedExchange(&g_timed, 0);
    const LONG64 ticks = InterlockedExchange64(&g_timedTicks, 0);
    float turn = 0.0f, mv[3] = {};
    if (s_mode == kPause && ReadMat(g_eng + ::dlvr::buildrva::Eng(kSceneCamRva) + kCamMatrix, scene) && IsRigid(scene, kRigidTol)) {
        const Mat34 rel = Relative(s_s0, scene);
        turn = TurnDeg(rel); mv[0] = rel.m[3]; mv[1] = rel.m[7]; mv[2] = rel.m[11];
    }
    // [PROBE: UIPIN] gate=ui_panel_menus  site=HEADSET  log-only=no
    LOG_PERIODIC("[UIPIN] 1 s: mode %s | submits %ld | calls rewritten: main menu %ld, pause menu %ld | rule cost avg %.3f us over %ld "
        "timed (1 in %d) | left as the game drew them: handedness not this mode's %ld, snapshot busy %ld | REFUSED: menu "
        "camera not rigid %ld, scene camera not rigid / mirrored %ld, scene camera over %.1f m from the menu's %ld, result "
        "not rigid %ld, read / write faults %ld | pause opens %ld, opens with no usable scene camera %ld | since the pause "
        "menu opened: view turned %.2f deg, moved (%.3f %.3f %.3f) m | levels listed %d",
        s_mode == kPause ? "PAUSE-MENU" : s_mode == kMainMenu ? "MAIN-MENU" : "none (no menu level, or a menu stage 1 leaves alone)",
        s_frames, static_cast<long>(cm), static_cast<long>(cp),
        timed ? static_cast<double>(ticks) * g_tickUs / static_cast<double>(timed) : 0.0, static_cast<long>(timed), kTimedEvery,
        static_cast<long>(InterlockedExchange(&g_passDet, 0)), static_cast<long>(InterlockedExchange(&g_refBusy, 0)),
        static_cast<long>(InterlockedExchange(&g_refSrc, 0)), static_cast<long>(InterlockedExchange(&g_refScene, 0)),
        static_cast<double>(kMainMenuReach), static_cast<long>(InterlockedExchange(&g_refFar, 0)),
        static_cast<long>(InterlockedExchange(&g_refOut, 0)), static_cast<long>(g_faults), s_opens, s_noAnchor,
        turn, mv[0], mv[1], mv[2], s_levels);
    s_frames = 0;
}

}  // namespace uipin
}  // namespace dlvr
