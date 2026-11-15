// WRIST HUD STAGE 0 PROBE - see wristprobe.h for why, what and the pre-registered readings. LOG-ONLY: the three import swaps
// call the original with the same arguments and return its result; every read is SEH-guarded; the only engine calls are the
// exported getters, from inside a call the game itself made, on the game's own thread.
#include "wristprobe.h"

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

// buildrva.h (BETA tree only): Steam rvas -> the Epic build's; DEV reads the Steam rva as is (holster.cpp's DLVR_HH_GD rule).
#if __has_include("../common/buildrva.h")
#include "../common/buildrva.h"
#define DLVR_WP_GD(rva) ::dlvr::buildrva::Gd(rva)
#else
#define DLVR_WP_GD(rva) (rva)
#endif
#include "../common/iathook.h"    // PatchIat
#include "../common/log.h"
#include "../common/wristleaf.h"  // stage 1: the leaves handed to the bootstrap (dlvr_WristLeaves)
#include "holster.h"               // HolsterWheelShowing - the router stands down while a selection wheel is open
#include "rtticam.h"               // RttiClassNameOf
#include "settings.h"              // DlvrFilePath
#include "uipanel.h"               // HudScreenSig - the router also stands down while the 3D HUD reads the HUD screen gone
#include "uipin.h"                 // GameMenuOpen - the router stands down while an in-game menu is open
#include "wristquad.h"             // OnThisLaunch - ui_wrist_hud at launch (STAGE 3a, replaces dlvr\wrist_route.txt)

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

namespace dlvr {
namespace wristprobe {

namespace {

using VisFn   = bool(__fastcall*)(void* element);
using TextFn  = void(__fastcall*)(void* text, const void* str);
using OpFn    = void(__fastcall*)(void* element, float opacity);
using CountFn = int(__fastcall*)(void* element);
using ChildFn = void*(__fastcall*)(void* element, int index);
using OutFn   = void*(__fastcall*)(void* element, float* out);   // mtx34 / vec3 returned through the hidden pointer (wristhud1)
using FloatFn = float(__fastcall*)(void* element);

const char* const kEngine   = "engine_x64_rwdi.dll";
const char* const kVisName  = "?IsActuallyVisible@IUIElement@@QEBA_NXZ";
const char* const kTextName = "?SetText@IUIText@@UEAAXAEBV?$string_base@D@ttl@@@Z";
const char* const kOpName   = "?SetOpacity@IUIElement@@UEAAXM@Z";
const char* const kCountName = "?GetChildrenCount@IUIElement@@QEAAHXZ";
const char* const kChildName = "?GetChildren@IUIElement@@QEAAPEAV1@H@Z";
const char* const kXformName = "?GetGlobalTransform@IUIElement@@QEAA?AVmtx34@@XZ";
const char* const kSizeName  = "?GetSize@IUIElement@@UEBA?AVvec3@@XZ";
const char* const kAOpName   = "?GetActualOpacity@IUIElement@@QEAAMXZ";
const char* const kIsVisName = "?IsVisible@IUIElement@@QEBA_NXZ";        // stage 1 walk: the element's OWN visible / opacity
const char* const kOpGetName = "?GetOpacity@IUIElement@@UEBAMXZ";

// The five pieces (INV-724: direct children of HUD_DI in OUR hud.xui, DataDLC24_2.pak).
constexpr int kPieces = 5;
const char* const kPiece[kPieces] = { "HudRadar", "health_wrap", "StackObjectives", "HudPrimaryWeaponIndicator",
                                      "HudSecondaryWeaponIndicator" };

// HUD manager (INV-642 / 643, holster.cpp kHh*): the same constants, read only.
constexpr uintptr_t kMgrRva   = 0x1C16358;
constexpr uintptr_t kRegOff   = 72;
constexpr uintptr_t kMaskOff  = 1368;
constexpr uintptr_t kGroupCnt = 0x548;
constexpr uintptr_t kModeOff  = 1964;   // HudMode cache (INV-643: written in +555D10, read by +550FC0)
constexpr uintptr_t kFlagsOff = 0x30;
constexpr int       kMaxId    = 0x88;

constexpr ULONGLONG kSnapEveryMs = 5000;
constexpr ULONGLONG kRetryMs     = 100;    // an element of another root: look again soon, not on every call
constexpr int       kThreads     = 8;
constexpr int       kDetailSnaps = 3;      // the first N snapshots also list every child / piece child
constexpr size_t    kBuf         = 32768;

enum : int { kVis = 0, kText = 1, kOp = 2, kKinds = 3 };

VisFn   g_vis = nullptr;
TextFn  g_text = nullptr;
OpFn    g_op = nullptr;
CountFn g_count = nullptr;
ChildFn g_child = nullptr;
OutFn   g_xform = nullptr;
OutFn   g_size = nullptr;
FloatFn g_aop = nullptr;
VisFn   g_isVis = nullptr;
FloatFn g_opGet = nullptr;
uintptr_t g_gd = 0;
bool g_probeOn = false;                   // dlvr\wrist_probe.txt (stage 0 lines)
bool g_routeOn = false;                   // ui_wrist_hud at launch (stage 1: publish the leaves; was dlvr\wrist_route.txt)

volatile LONG  g_started = 0;
volatile LONG  g_live = 0;                // 1 once the swaps are in
volatile DWORD g_tid[kThreads] = {};
volatile LONG  g_calls[kThreads][kKinds] = {};
volatile LONG  g_noSlot = 0;
volatile LONG  g_firstOnThread = 0;       // threads whose first call has been noted (count)
volatile LONG64 g_nextSnapMs = 0;
volatile LONG  g_snapState = 0;           // 0 free, 1 writing (game thread), 2 ready (printer thread)
volatile LONG  g_snaps = 0, g_otherRoot = 0, g_walkFail = 0, g_faults = 0;
char           g_otherName[64] = {};
char           g_buf[kBuf] = {};
size_t         g_len = 0;

// component registry as last printed (game thread writes, only inside the snapshot)
uintptr_t g_prevComp[kMaxId + 1] = {};
unsigned  g_prevMask[kMaxId + 1] = {};
unsigned  g_prevFlags[kMaxId + 1] = {};
uintptr_t g_clsPtr[kMaxId + 1] = {};
char      g_cls[kMaxId + 1][48] = {};
int       g_prevMode = -1000;
bool      g_regPrinted = false;

void Ap(const char* fmt, ...)
{
    if (g_len >= kBuf - 2) { return; }
    va_list ap;
    va_start(ap, fmt);
    const int n = _vsnprintf_s(g_buf + g_len, kBuf - g_len - 1, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n > 0) { g_len += static_cast<size_t>(n); } else { g_len = kBuf - 2; }
    g_buf[g_len++] = '\n';
    g_buf[g_len] = 0;
}

bool RdP(uintptr_t a, uintptr_t& out)
{
    __try { out = *reinterpret_cast<const uintptr_t*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { out = 0; return false; }
}
bool RdU(uintptr_t a, unsigned& out)
{
    __try { out = *reinterpret_cast<const unsigned*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { out = 0; return false; }
}
// A C string at p, printable ASCII only; "?" when unreadable / not text.
void RdStr(uintptr_t p, char* out, size_t n)
{
    out[0] = 0;
    if (!p || n < 2) { return; }
    size_t i = 0;
    __try {
        for (; i + 1 < n; ++i) {
            const char c = *reinterpret_cast<const char*>(p + i);
            if (!c) { break; }
            if (c < 0x20 || c > 0x7E) { i = 0; break; }
            out[i] = c;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { i = 0; }
    out[i] = 0;
    if (!i) { strcpy_s(out, n, "?"); }
}
// Mamfro's name read (ui.rs name_of): engine object = element+0x18, name = the string at [[object+8]].
void NameOf(uintptr_t el, char* out, size_t n)
{
    uintptr_t obj = 0, holder = 0, str = 0;
    if (!RdP(el + 0x18, obj) || !obj || !RdP(obj + 8, holder) || !holder || !RdP(holder, str)) { strcpy_s(out, n, "?"); return; }
    RdStr(str, out, n);
}
// The root by plain reads: parent object = object+528 (IsActuallyVisible's own walk, wristhud1), its wrapper at +16.
uintptr_t RootOf(uintptr_t el)
{
    uintptr_t e = el;
    for (int i = 0; i < 64; ++i) {
        uintptr_t obj = 0, par = 0, wrap = 0;
        if (!RdP(e + 0x18, obj) || !obj) { return 0; }
        if (!RdP(obj + 528, par)) { return 0; }
        if (!par) { return e; }
        if (!RdP(par + 16, wrap) || !wrap) { return 0; }
        e = wrap;
    }
    return 0;
}

bool CallCount(void* e, int& out)
{
    __try { out = g_count(e); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { out = 0; return false; }
}
bool CallChild(void* e, int i, uintptr_t& out)
{
    __try { out = reinterpret_cast<uintptr_t>(g_child(e, i)); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { out = 0; return false; }
}
bool CallVis(void* e, bool& out)
{
    __try { out = g_vis(e); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { out = false; return false; }
}
bool CallOut(OutFn f, void* e, float* out)
{
    __try { f(e, out); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallAop(void* e, float& out)
{
    __try { out = g_aop(e); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { out = -1.0f; return false; }
}

void ListChildren(uintptr_t parent, const char* label, int maxKids)
{
    int n = 0;
    if (!CallCount(reinterpret_cast<void*>(parent), n)) { Ap("  %s: children count READ FAULTED", label); ++g_faults; return; }
    char line[1400] = {};
    size_t at = 0;
    const int shown = n < maxKids ? n : maxKids;
    for (int i = 0; i < shown; ++i) {
        uintptr_t c = 0;
        char nm[64], cls[64] = {};
        if (!CallChild(reinterpret_cast<void*>(parent), i, c) || !c) { strcpy_s(nm, "NULL"); }
        else {
            NameOf(c, nm, sizeof(nm));
            if (!RttiClassNameOf(c, cls, sizeof(cls))) { strcpy_s(cls, "?"); }
        }
        const int k = _snprintf_s(line + at, sizeof(line) - at, _TRUNCATE, " %s(%s)", nm, cls);
        if (k <= 0) { break; }
        at += static_cast<size_t>(k);
    }
    Ap("  %s: %d children%s:%s", label, n, n > shown ? " (first ones only)" : "", line);
}

void ReadRegistry(bool detail)
{
    if (!g_gd) { Ap("COMPONENTS: gamedll base unknown - NOT READ"); return; }
    uintptr_t mgr = 0;
    if (!RdP(g_gd + DLVR_WP_GD(kMgrRva), mgr) || !mgr) { Ap("COMPONENTS: no HUD manager (the global reads 0) - NOT READ"); return; }
    unsigned mode = 0, groups = 0;
    const bool mOk = RdU(mgr + kModeOff, mode);
    RdU(mgr + kGroupCnt, groups);
    int changed = 0, regs = 0;
    char line[1400] = {};
    size_t at = 0;
    const bool full = !g_regPrinted;
    for (int id = 0; id <= kMaxId; ++id) {
        uintptr_t comp = 0;
        unsigned mask = 0, fl = 0;
        RdP(mgr + kRegOff + 8 * static_cast<uintptr_t>(id), comp);
        if (comp) { ++regs; RdU(mgr + kMaskOff + 4 * static_cast<uintptr_t>(id), mask); RdU(comp + kFlagsOff, fl); fl &= 0xFFu; }
        if (comp && g_clsPtr[id] != comp) {
            g_clsPtr[id] = comp;
            if (!RttiClassNameOf(comp, g_cls[id], sizeof(g_cls[id]))) { strcpy_s(g_cls[id], "?"); }
        }
        const bool diff = comp != g_prevComp[id] || mask != g_prevMask[id] || fl != g_prevFlags[id];
        if ((full && comp) || (!full && diff)) {
            ++changed;
            const int k = comp
                ? _snprintf_s(line + at, sizeof(line) - at, _TRUNCATE, " #%d %s mask %08X flags %02X%s", id, g_cls[id], mask, fl,
                              full ? "" : (g_prevComp[id] == comp ? "" : " (new component)"))
                : _snprintf_s(line + at, sizeof(line) - at, _TRUNCATE, " #%d GONE", id);
            if (k > 0) { at += static_cast<size_t>(k); }
            if (at > 1100) { Ap("COMPONENTS%s:%s", full ? " (all registered)" : " changed", line); at = 0; line[0] = 0; }
        }
        g_prevComp[id] = comp; g_prevMask[id] = mask; g_prevFlags[id] = fl;
    }
    if (at) { Ap("COMPONENTS%s:%s", full ? " (all registered)" : " changed", line); }
    const int modeNow = mOk ? static_cast<int>(mode) : -1;
    if (full || changed || modeNow != g_prevMode || detail) {
        Ap("COMPONENTS: %d registered, %d %s | HUD mode cache (manager +1964) %d%s (0 Full / 1 Limited / 2 Hidden), was %d | "
           "groups %u", regs, changed, full ? "listed" : "changed since the last snapshot", modeNow, mOk ? "" : " UNREADABLE",
           g_prevMode, groups);
    }
    g_prevMode = modeNow;
    g_regPrinted = true;
}

// On the game's calling thread, inside one of the swapped calls. Writes g_buf; the printer thread logs it.
void Snapshot(uintptr_t el, DWORD tid, int kind)
{
    const uintptr_t root = RootOf(el);
    if (!root) { InterlockedIncrement(&g_walkFail); g_nextSnapMs = static_cast<LONG64>(GetTickCount64() + kRetryMs); return; }
    char rn[64];
    NameOf(root, rn, sizeof(rn));
    if (strcmp(rn, "HUD_DI") != 0) {
        InterlockedIncrement(&g_otherRoot);
        strcpy_s(g_otherName, rn);
        g_nextSnapMs = static_cast<LONG64>(GetTickCount64() + kRetryMs);
        return;
    }
    if (InterlockedCompareExchange(&g_snapState, 1, 0) != 0) { return; }   // the last one is still being printed
    g_len = 0; g_buf[0] = 0;
    const long n = InterlockedIncrement(&g_snaps);
    const bool detail = n <= kDetailSnaps;
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    Ap("SNAPSHOT %ld on thread %lu (inside a %s call): root %p '%s' (other roots met since the last: %ld, last '%s'; walks "
       "failed %ld)", n, static_cast<unsigned long>(tid), kind == kVis ? "IsActuallyVisible" : kind == kText ? "SetText" : "SetOpacity",
       reinterpret_cast<void*>(root), rn, static_cast<long>(g_otherRoot), g_otherName, static_cast<long>(g_walkFail));
    g_otherRoot = 0; g_walkFail = 0;
    if (detail) { ListChildren(root, "HUD_DI", 80); }
    int cnt = 0;
    if (!CallCount(reinterpret_cast<void*>(root), cnt)) { Ap("  HUD_DI children count READ FAULTED"); ++g_faults; }
    bool found[kPieces] = {};
    for (int i = 0; i < cnt && i < 128; ++i) {
        uintptr_t c = 0;
        if (!CallChild(reinterpret_cast<void*>(root), i, c) || !c) { continue; }
        char nm[64];
        NameOf(c, nm, sizeof(nm));
        for (int p = 0; p < kPieces; ++p) {
            if (found[p] || strcmp(nm, kPiece[p]) != 0) { continue; }
            found[p] = true;
            bool vis = false;
            float aop = -1.0f, m[16] = {}, s[4] = {};
            const bool vOk = CallVis(reinterpret_cast<void*>(c), vis);
            const bool oOk = CallAop(reinterpret_cast<void*>(c), aop);
            const bool xOk = CallOut(g_xform, reinterpret_cast<void*>(c), m);
            const bool sOk = CallOut(g_size, reinterpret_cast<void*>(c), s);
            if (!vOk || !oOk || !xOk || !sOk) { ++g_faults; }
            Ap("  PIECE %s child #%d: visible %s | actual opacity %s%.3f | size %s(%.2f %.2f %.2f) | global transform %s[%.4g %.4g "
               "%.4g %.4g] [%.4g %.4g %.4g %.4g] [%.4g %.4g %.4g %.4g]", kPiece[p], i, vOk ? (vis ? "1" : "0") : "READ FAULTED",
               oOk ? "" : "READ FAULTED ", aop, sOk ? "" : "READ FAULTED ", s[0], s[1], s[2], xOk ? "" : "READ FAULTED ", m[0], m[1],
               m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
            if (detail) { ListChildren(c, kPiece[p], 24); }
        }
    }
    for (int p = 0; p < kPieces; ++p) {
        if (!found[p]) { Ap("  PIECE %s NOT FOUND among HUD_DI's %d children", kPiece[p], cnt); }
    }
    ReadRegistry(detail);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    Ap("SNAPSHOT %ld cost on the game's thread %.3f ms (reads + getters, no logging here) | read faults so far %ld", n,
       f.QuadPart ? static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart) : -1.0,
       static_cast<long>(g_faults));
    InterlockedExchange(&g_snapState, 2);
}

// ===== STAGE 1 (dlvr\wrist_route.txt): THE LEAVES, published for the bootstrap's draw routing (wristleaf.h) =====
// Mamfro's ui.rs snapshot / walk, on our path: inside a game UI call (any of the >= 9 job threads, INV-725), at most every
// kPublishMs: HUD_DI's direct children are LISTED AGAIN EVERY TIME (no element pointer is kept between snapshots - a level unload
// frees the tree, his quit-to-menu crash); the five pieces' widgets are walked every time, the others kRotate at a time in turn
// (their leaves only tell where a piece's draws END). A walk skips an element that is not visible or has opacity <= 0, with all
// under it; a leaf = an element with no children: its box from GetGlobalTransform + GetSize. Text = the wrapper's RTTI class name
// holds "Text" (cached per vtable). Published under an SRW lock; the bootstrap copies it once per HUD pass.
// 8 ms = Mamfro's SNAPSHOT_EVERY_MS (dl1/src/hud/ui.rs:68). It was 50 until RUN WS1f (my unchecked choice): an animating part of a
// piece moved off leaves up to ~6 frames old and blinked into view (Jay: "Mostly gone, blink on"). Trace
// docs/traces/20261112_1650_wrist_snapshot_8ms_blinkcount.md.
constexpr ULONGLONG kPublishMs  = 8;
constexpr int       kWidgetsMax = 96;
constexpr int       kPerWidget  = 192;
constexpr int       kRotate     = 4;
const char* const kPieceTag[kPieces] = { "minimap", "health", "quests", "weapon", "quick item" };

struct WidgetLeaves { int n; bool read; wristleaf::Leaf l[kPerWidget]; };
WidgetLeaves  g_wl[kWidgetsMax] = {};     // by child index of HUD_DI; reset when the child list changes length
int           g_wlCount = -1;
int           g_rotNext = 0;
volatile LONG g_pubBusy = 0;
volatile LONG64 g_nextPubMs = 0;
SRWLOCK       g_pubLock = SRWLOCK_INIT;
wristleaf::Header g_pubHdr = {};
wristleaf::Leaf   g_pub[wristleaf::kMaxLeaves] = {};
wristleaf::Header g_stageHdr = {};        // built here, copied under the lock
wristleaf::Leaf   g_stage[wristleaf::kMaxLeaves] = {};
volatile LONG g_pubs = 0, g_pubFaults = 0, g_pubTrunc = 0, g_pubOtherRoot = 0;
volatile LONG64 g_pubCostUs = 0, g_pubMaxUs = 0;
struct VtText { uintptr_t vt; int text; };
VtText g_vtText[128] = {};
// LAST GOOD per PIECE (2026-11-12, trace docs/traces/20261112_1600_wrist_snapshot_lastgood.md; RUN WS1b / WS1c: "Only the 5
// pieces" flash). A walk taken on a job thread mid-update can miss a piece; the snapshot then dropped it from shownMask and the
// router drew all of its draws as made until the next publish (50 ms). Now a piece's leaves are replaced only by a walk that
// still finds it and has at least half the last good count; kStreak walks in a row that disagree are taken as a real change
// (a quest done, a weapon holstered). Kept by PIECE, so the child-list reset (count changed) does not wipe them.
constexpr int kStreak = 3;
// HOLD OPAQUE (2026-11-12, design Q2 "Always shown"; trace docs/traces/20261112_1630_wrist_hold_opaque.md; Mamfro dl1/src/hud/ui.rs
// :165-180 + :417-439, his world_hud_opaque default true). The game fades the five pieces (health at full, the weapon / tool when
// idle, quests after a while); a fading piece's draws missed the leaves read 50 ms earlier and stayed in view (RUN WS1d capture 3).
// The game's SetOpacity calls reach the engine's setter only through the export (+56BA40 -> +56AA70; other callers: the UI editor and
// the load-time property loader - wristhud3 / wristhud4), so OpHook (our gamedll IAT swap) passes 1.0 for the five widgets' engine
// objects; each publish also sets a widget still below 1 back to 1 through the original export (a fade that went around it).
volatile LONG64 g_heldObj[kPieces] = {};  // the five widgets' engine objects (element+0x18), 0 = none known yet
volatile LONG g_fadesHeld = 0, g_corrected = 0;
// The one gamedll reader of GetActualOpacity: +4408D0 reads the element at [[gamedll+1C16470]+3568]+224 (wristhud5). Its name is
// logged once so it is known whether it is one of the five. 0x1C16470 is in the Epic table (buildrva_table.h -> EGL 0x1C153C0).
constexpr uintptr_t kActOpGlobalRva = 0x1C16470;
constexpr uintptr_t kActOpOff1 = 3568, kActOpOff2 = 224;
char g_actOpName[64] = {};
volatile LONG g_actOpRead = 0;           // 0 not yet, 1 read (name or why)
WidgetLeaves  g_good[kPieces] = {};
bool          g_goodHave[kPieces] = {};
int           g_streak[kPieces] = {};
volatile LONG g_rej[kPieces] = {}, g_chg[kPieces] = {};
// THE MINIMAP's BOX = THE HudRadar WIDGET's OWN RECT (trace docs/traces/20261113_1400_wrist_minimap_widget_box.md; RUNS A2 / M1 - the
// whole map gone ~170 ms on a turn, Jay's recording): its largest leaf is the streamed map-tile group, which drops out for >= 3 publishes
// at some headings (the box hold of trace 1345 only delayed it). The widget's rect does not move (INV-725: 780 x 780 at (3448, 500) in
// every snapshot). Read at each walk; a failed read = the largest leaf as before (counted).
float         g_mmWidget[4] = {};
bool          g_mmWidgetOk = false;
volatile LONG g_mmWidgetUsed = 0, g_mmFallback = 0;
// THE WEAPON BOX's BOX = THE HudPrimaryWeaponIndicator WIDGET's OWN RECT (trace docs/traces/20261113_1520_wrist_weapon_widget_box.md;
// RUN B3 - "the ammo counter will move after a shot": the union of its leaves doubled in width when a leaf right of the indicator came
// and went; INV-725: the widget is (3776, 2336) 384 x 192). Same path as the minimap's; the leaf outside the frame is cut off (Jay).
constexpr int kWeaponPiece = 3;   // kPiece[3] = "HudPrimaryWeaponIndicator"
float         g_wpWidget[4] = {};
bool          g_wpWidgetOk = false;
volatile LONG g_wpWidgetUsed = 0, g_wpFallback = 0;

bool CallIsVis(void* e, bool& out)
{
    __try { out = g_isVis(e); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { out = false; return false; }
}
bool CallOpGet(void* e, float& out)
{
    __try { out = g_opGet(e); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { out = 0.0f; return false; }
}
// The ORIGINAL exported setter (g_op = what the gamedll's IAT slot held, i.e. the engine export) - never our OpHook.
bool CallSetOpaque(void* e)
{
    __try { g_op(e, 1.0f); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The element GetActualOpacity is read for by gamedll +4408D0 (wristhud5), named once.
void ReadActOpElementOnce()
{
    if (g_actOpRead || !g_gd) { return; }
    uintptr_t glob = 0, a = 0, el = 0;
    if (!RdP(g_gd + DLVR_WP_GD(kActOpGlobalRva), glob) || !glob) { return; }   // not set up yet: asked again next publish
    if (!RdP(glob + kActOpOff1, a) || !a) { return; }
    if (!RdP(a + kActOpOff2, el) || !el) { return; }
    NameOf(el, g_actOpName, sizeof(g_actOpName));
    InterlockedExchange(&g_actOpRead, 1);
}
bool IsTextElem(uintptr_t el)
{
    uintptr_t vt = 0;
    if (!RdP(el, vt) || !vt) { return false; }
    for (VtText& v : g_vtText) {
        if (v.vt == vt) { return v.text != 0; }
        if (!v.vt) {
            char cls[64] = {};
            const bool named = RttiClassNameOf(el, cls, sizeof(cls));
            v.text = (named && strstr(cls, "Text")) ? 1 : 0;
            v.vt = vt;                      // a racing thread may class it twice: same answer
            return v.text != 0;
        }
    }
    char cls[64] = {};
    return RttiClassNameOf(el, cls, sizeof(cls)) && strstr(cls, "Text");   // table full: uncached (counted nowhere - 128 classes)
}
// An element's box, as ui.rs element_box: corner = translation, far = corner + size x the scale diagonal.
bool BoxOf(uintptr_t el, float at[2], float rect[4])
{
    float m[16] = {}, s[4] = {};
    if (!CallOut(g_xform, reinterpret_cast<void*>(el), m) || !CallOut(g_size, reinterpret_cast<void*>(el), s)) { return false; }
    at[0] = m[3]; at[1] = m[7];
    const float fx = m[3] + s[0] * m[0], fy = m[7] + s[1] * m[5];
    rect[0] = at[0] < fx ? at[0] : fx; rect[2] = at[0] < fx ? fx : at[0];
    rect[1] = at[1] < fy ? at[1] : fy; rect[3] = at[1] < fy ? fy : at[1];
    for (int i = 0; i < 4; ++i) { if (!(rect[i] > -1e7f && rect[i] < 1e7f)) { return false; } }
    return true;
}
void Walk(uintptr_t el, int piece, float above, WidgetLeaves& out, int depth)
{
    if (depth > 24 || out.n >= kPerWidget) { if (out.n >= kPerWidget) { InterlockedIncrement(&g_pubTrunc); } return; }
    bool vis = false;
    float op = 0.0f;
    if (!CallIsVis(reinterpret_cast<void*>(el), vis) || !CallOpGet(reinterpret_cast<void*>(el), op)) { InterlockedIncrement(&g_pubFaults); return; }
    if (!vis) { return; }
    const float o = above * op;
    if (!(o > 0.0f)) { return; }
    int cnt = 0;
    if (!CallCount(reinterpret_cast<void*>(el), cnt)) { InterlockedIncrement(&g_pubFaults); return; }
    if (cnt > 0) {
        for (int i = 0; i < cnt && i < 512; ++i) {
            uintptr_t c = 0, obj = 0;
            if (!CallChild(reinterpret_cast<void*>(el), i, c) || !c || !RdP(c + 0x18, obj) || !obj) { continue; }
            Walk(c, piece, o, out, depth + 1);
        }
        return;
    }
    wristleaf::Leaf lf = {};
    if (!BoxOf(el, lf.at, lf.rect)) { return; }
    lf.piece = static_cast<signed char>(piece);
    lf.text = IsTextElem(el) ? 1 : 0;
    out.l[out.n++] = lf;
}

void Publish(uintptr_t el)
{
    const uintptr_t root = RootOf(el);
    if (!root) { return; }
    char rn[64];
    NameOf(root, rn, sizeof(rn));
    if (strcmp(rn, "HUD_DI") != 0) { InterlockedIncrement(&g_pubOtherRoot); g_nextPubMs = static_cast<LONG64>(GetTickCount64() + 20); return; }
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    ReadActOpElementOnce();
    int cnt = 0;
    if (!CallCount(reinterpret_cast<void*>(root), cnt)) { InterlockedIncrement(&g_pubFaults); return; }
    if (cnt > kWidgetsMax) { cnt = kWidgetsMax; InterlockedIncrement(&g_pubTrunc); }
    if (cnt != g_wlCount) { for (int i = 0; i < kWidgetsMax; ++i) { g_wl[i].n = 0; g_wl[i].read = false; } g_wlCount = cnt; }
    int pieceOf[kWidgetsMax];
    for (int i = 0; i < kWidgetsMax; ++i) { pieceOf[i] = -1; }
    wristleaf::Header& h = g_stageHdr;
    memset(&h, 0, sizeof(h));
    float ra[2];
    if (!BoxOf(root, ra, h.root)) { h.root[0] = h.root[1] = h.root[2] = h.root[3] = 0.0f; }
    for (int i = 0; i < cnt; ++i) {
        uintptr_t c = 0, obj = 0;
        if (!CallChild(reinterpret_cast<void*>(root), i, c) || !c || !RdP(c + 0x18, obj) || !obj) { g_wl[i].n = 0; continue; }
        char nm[64];
        NameOf(c, nm, sizeof(nm));
        int piece = -1;
        for (int p = 0; p < kPieces; ++p) { if (strcmp(nm, kPiece[p]) == 0) { piece = p; break; } }
        pieceOf[i] = piece;
        if (piece == wristleaf::kMinimap) {   // the widget's own rect - the minimap's box (see g_mmWidget)
            float at[2];
            g_mmWidgetOk = BoxOf(c, at, g_mmWidget) && g_mmWidget[2] - g_mmWidget[0] >= 1.0f && g_mmWidget[3] - g_mmWidget[1] >= 1.0f;
        }
        if (piece == kWeaponPiece) {   // the widget's own rect - the weapon box's box (see g_wpWidget)
            float at[2];
            g_wpWidgetOk = BoxOf(c, at, g_wpWidget) && g_wpWidget[2] - g_wpWidget[0] >= 1.0f && g_wpWidget[3] - g_wpWidget[1] >= 1.0f;
        }
        if (piece >= 0) {
            // HOLD OPAQUE: this widget's engine object is held at the setter; a fade that went around it is set back here
            InterlockedExchange64(&g_heldObj[piece], static_cast<LONG64>(obj));
            float op = 1.0f;
            if (CallOpGet(reinterpret_cast<void*>(c), op) && op < 1.0f && CallSetOpaque(reinterpret_cast<void*>(c))) {
                InterlockedIncrement(&g_corrected);
            }
        }
        const bool due = piece >= 0 || !g_wl[i].read || ((i - g_rotNext + cnt) % cnt) < kRotate;
        if (!due) { continue; }
        g_wl[i].n = 0;
        Walk(c, piece, 1.0f, g_wl[i], 0);
        g_wl[i].read = true;
        if (piece < 0) { continue; }
        // LAST GOOD: this walk replaces the piece's leaves only if it found the piece and is not suddenly short
        const WidgetLeaves& w = g_wl[i];
        const bool ok = w.n > 0 && (!g_goodHave[piece] || w.n * 2 >= g_good[piece].n);
        if (ok) {
            g_good[piece] = w; g_goodHave[piece] = true; g_streak[piece] = 0;
        } else if (++g_streak[piece] >= kStreak) {   // kStreak walks in a row agree: a real change
            g_good[piece] = w; g_goodHave[piece] = w.n > 0; g_streak[piece] = 0;
            InterlockedIncrement(&g_chg[piece]);
        } else {
            InterlockedIncrement(&g_rej[piece]);
        }
    }
    if (cnt) { g_rotNext = (g_rotNext + kRotate) % cnt; }
    int n = 0;
    bool emitted[kPieces] = {};
    // one widget's leaves into the staging array in tree order (the last flag = its widget's last leaf), boxing the pieces
    auto emit = [&](const WidgetLeaves& w) {
        for (int k = 0; k < w.n; ++k) {
            if (n >= wristleaf::kMaxLeaves) { InterlockedIncrement(&g_pubTrunc); break; }
            wristleaf::Leaf lf = w.l[k];
            lf.last = (k + 1 == w.n) ? 1 : 0;
            g_stage[n++] = lf;
            const int p = lf.piece;
            if (p < 0) { continue; }
            // a leaf far outside the HUD's own box draws nothing (his OFF_SCREEN_SLACK 8)
            if (h.root[2] > h.root[0] && (lf.rect[0] < h.root[0] - 8 || lf.rect[1] < h.root[1] - 8 || lf.rect[2] > h.root[2] + 8 ||
                                          lf.rect[3] > h.root[3] + 8)) { continue; }
            float* b = h.pieceBox[p];
            if (!(h.shownMask & (1 << p))) { memcpy(b, lf.rect, sizeof(lf.rect)); h.shownMask |= (1 << p); }
            else if (p == wristleaf::kMinimap) {
                const float a0 = (b[2] - b[0]) * (b[3] - b[1]), a1 = (lf.rect[2] - lf.rect[0]) * (lf.rect[3] - lf.rect[1]);
                if (a1 > a0) { memcpy(b, lf.rect, sizeof(lf.rect)); }
            } else {
                if (lf.rect[0] < b[0]) { b[0] = lf.rect[0]; }
                if (lf.rect[1] < b[1]) { b[1] = lf.rect[1]; }
                if (lf.rect[2] > b[2]) { b[2] = lf.rect[2]; }
                if (lf.rect[3] > b[3]) { b[3] = lf.rect[3]; }
            }
        }
    };
    for (int i = 0; i < cnt; ++i) {
        const int p = pieceOf[i];
        if (p < 0) { emit(g_wl[i]); continue; }
        if (g_goodHave[p] && !emitted[p]) { emit(g_good[p]); emitted[p] = true; }
    }
    // a piece whose widget this listing did not reach (a read fault) keeps its last good leaves, at the end of the order
    for (int p = 0; p < kPieces; ++p) {
        if (g_goodHave[p] && !emitted[p]) { emit(g_good[p]); emitted[p] = true; }
    }
    // THE MINIMAP's BOX = the HudRadar widget's own rect (see g_mmWidget); a failed widget read keeps the largest leaf (counted)
    if (h.shownMask & (1 << wristleaf::kMinimap)) {
        if (g_mmWidgetOk) {
            memcpy(h.pieceBox[wristleaf::kMinimap], g_mmWidget, sizeof(g_mmWidget));
            InterlockedIncrement(&g_mmWidgetUsed);
        } else {
            InterlockedIncrement(&g_mmFallback);
        }
    }
    // THE WEAPON BOX's BOX = the HudPrimaryWeaponIndicator widget's own rect (see g_wpWidget); a failed read keeps the union (counted)
    if (h.shownMask & (1 << kWeaponPiece)) {
        if (g_wpWidgetOk) {
            memcpy(h.pieceBox[kWeaponPiece], g_wpWidget, sizeof(g_wpWidget));
            InterlockedIncrement(&g_wpWidgetUsed);
        } else {
            InterlockedIncrement(&g_wpFallback);
        }
    }
    h.count = n;
    h.takenMs = GetTickCount64();
    AcquireSRWLockExclusive(&g_pubLock);
    h.gen = g_pubHdr.gen + 1;
    g_pubHdr = h;
    memcpy(g_pub, g_stage, sizeof(wristleaf::Leaf) * static_cast<size_t>(n));
    ReleaseSRWLockExclusive(&g_pubLock);
    InterlockedIncrement(&g_pubs);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    const LONG64 us = f.QuadPart ? (t1.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart : 0;
    InterlockedExchangeAdd64(&g_pubCostUs, us);
    if (us > g_pubMaxUs) { g_pubMaxUs = us; }
}

void Note(int kind, void* el)
{
    const DWORD tid = GetCurrentThreadId();
    int slot = -1;
    for (int i = 0; i < kThreads; ++i) {
        const DWORD t = g_tid[i];
        if (t == tid) { slot = i; break; }
        if (t == 0) {
            if (InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&g_tid[i]), static_cast<LONG>(tid), 0) == 0) {
                slot = i; InterlockedIncrement(&g_firstOnThread); break;
            }
            if (g_tid[i] == tid) { slot = i; break; }
        }
    }
    if (slot < 0) { InterlockedIncrement(&g_noSlot); } else { InterlockedIncrement(&g_calls[slot][kind]); }
    if (!el) { return; }
    const ULONGLONG now = GetTickCount64();
    // [LOADBEARING: ui_wrist_hud] the leaf publish - the wrist HUD's only data source (CopyLeaves -> dlvr_WristLeaves -> the bootstrap's draw routing)
    if (g_routeOn && static_cast<LONG64>(now) >= g_nextPubMs && InterlockedCompareExchange(&g_pubBusy, 1, 0) == 0) {
        g_nextPubMs = static_cast<LONG64>(now + kPublishMs);
        Publish(reinterpret_cast<uintptr_t>(el));
        InterlockedExchange(&g_pubBusy, 0);
    }
    if (!g_probeOn || g_snapState != 0) { return; }
    if (static_cast<LONG64>(now) < g_nextSnapMs) { return; }
    g_nextSnapMs = static_cast<LONG64>(now + kSnapEveryMs);
    Snapshot(reinterpret_cast<uintptr_t>(el), tid, kind);
}

bool __fastcall VisHook(void* el)
{
    Note(kVis, el);
    return g_vis(el);
}
void __fastcall TextHook(void* t, const void* s)
{
    Note(kText, t);
    g_text(t, s);
}
void __fastcall OpHook(void* el, float o)
{
    Note(kOp, el);
    // HOLD OPAQUE: the game asks one of the five pieces' widgets for < 1 -> it gets 1.0 (Mamfro ui.rs set_opacity). Only with
    // dlvr\wrist_route.txt. COST per call: one aligned read + up to five compares.
    // [LOADBEARING: ui_wrist_hud] HOLD OPAQUE - without it the game's fades of the five pieces reach the wrist quads
    if (g_routeOn && o < 1.0f && el) {
        uintptr_t obj = 0;
        if (RdP(reinterpret_cast<uintptr_t>(el) + 0x18, obj) && obj) {
            for (int p = 0; p < kPieces; ++p) {
                if (static_cast<uintptr_t>(g_heldObj[p]) == obj) { o = 1.0f; InterlockedIncrement(&g_fadesHeld); break; }
            }
        }
    }
    g_op(el, o);
}

void PrintSnapshot()
{
    if (g_snapState != 2) { return; }
    const char* p = g_buf;
    while (*p) {
        const char* e = strchr(p, '\n');
        const size_t len = e ? static_cast<size_t>(e - p) : strlen(p);
        char line[1500];
        const size_t k = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
        memcpy(line, p, k);
        line[k] = 0;
        // [PROBE: WRISTPROBE] gate=dlvr\wrist_probe.txt  site=HEADSET  log-only=yes
        LOG("[WRISTPROBE] %s", line);
        if (!e) { break; }
        p = e + 1;
    }
    InterlockedExchange(&g_snapState, 0);
}

DWORD WINAPI ProbeThread(void*)
{
    // the gamedll is normally loaded by ovr_Initialize; wait up to 3 min for it otherwise
    HMODULE gd = nullptr, eng = nullptr;
    for (int i = 0; i < 720 && !(gd && eng); ++i) {
        gd = GetModuleHandleA("gamedll_x64_rwdi.dll");
        eng = GetModuleHandleA(kEngine);
        if (!(gd && eng)) { Sleep(250); }
    }
    if (!gd || !eng) {
        // [PROBE: WRISTPROBE] gate=dlvr\wrist_probe.txt  site=HEADSET  log-only=yes
        LOG("[WRISTPROBE] *** REFUSED: gamedll %s / engine %s not loaded after 3 min - nothing installed. NO VERDICT. ***",
            gd ? "loaded" : "NOT loaded", eng ? "loaded" : "NOT loaded");
        return 0;
    }
    g_gd = reinterpret_cast<uintptr_t>(gd);
    g_vis   = reinterpret_cast<VisFn>(GetProcAddress(eng, kVisName));
    g_text  = reinterpret_cast<TextFn>(GetProcAddress(eng, kTextName));
    g_op    = reinterpret_cast<OpFn>(GetProcAddress(eng, kOpName));
    g_count = reinterpret_cast<CountFn>(GetProcAddress(eng, kCountName));
    g_child = reinterpret_cast<ChildFn>(GetProcAddress(eng, kChildName));
    g_xform = reinterpret_cast<OutFn>(GetProcAddress(eng, kXformName));
    g_size  = reinterpret_cast<OutFn>(GetProcAddress(eng, kSizeName));
    g_aop   = reinterpret_cast<FloatFn>(GetProcAddress(eng, kAOpName));
    g_isVis = reinterpret_cast<VisFn>(GetProcAddress(eng, kIsVisName));
    g_opGet = reinterpret_cast<FloatFn>(GetProcAddress(eng, kOpGetName));
    if (g_routeOn && (!g_isVis || !g_opGet)) {
        // [PROBE: WRISTROUTE] gate=ui_wrist_hud (read at launch)  site=HEADSET  log-only=no
        LOG("[WRISTROUTE] layer: *** REFUSED: IUIElement::IsVisible %p / GetOpacity %p not exported - no leaves published. ***",
            reinterpret_cast<void*>(g_isVis), reinterpret_cast<void*>(g_opGet));
        g_routeOn = false;
    }
    if (!g_vis || !g_text || !g_op || !g_count || !g_child || !g_xform || !g_size || !g_aop) {
        // [PROBE: WRISTPROBE] gate=dlvr\wrist_probe.txt  site=HEADSET  log-only=yes
        LOG("[WRISTPROBE] *** REFUSED: an engine export is missing (IsActuallyVisible %p SetText %p SetOpacity %p GetChildrenCount "
            "%p GetChildren %p GetGlobalTransform %p GetSize %p GetActualOpacity %p) - nothing installed. NO VERDICT. ***",
            reinterpret_cast<void*>(g_vis), reinterpret_cast<void*>(g_text), reinterpret_cast<void*>(g_op),
            reinterpret_cast<void*>(g_count), reinterpret_cast<void*>(g_child), reinterpret_cast<void*>(g_xform),
            reinterpret_cast<void*>(g_size), reinterpret_cast<void*>(g_aop));
        return 0;
    }
    // the originals are valid BEFORE a slot goes live; PatchIat's return replaces them with whatever the slot held
    // [LOADBEARING: ui_wrist_hud] the three gamedll import swaps feed the leaf publish - NOT only the stage-0 probe; a probe strip must keep them
    void* o1 = PatchIat(gd, kEngine, kVisName, reinterpret_cast<void*>(&VisHook));
    if (o1) { g_vis = reinterpret_cast<VisFn>(o1); }
    void* o2 = PatchIat(gd, kEngine, kTextName, reinterpret_cast<void*>(&TextHook));
    if (o2) { g_text = reinterpret_cast<TextFn>(o2); }
    void* o3 = PatchIat(gd, kEngine, kOpName, reinterpret_cast<void*>(&OpHook));
    if (o3) { g_op = reinterpret_cast<OpFn>(o3); }
    InterlockedExchange(&g_live, (o1 || o2 || o3) ? 1 : 0);
    // [PROBE: WRISTPROBE] gate=dlvr\wrist_probe.txt  site=HEADSET  log-only=yes
    // [PROBE: UIMTX] gate=dlvr\ui_matrix_probe.txt  site=HEADSET  log-only=yes  (MENTION ONLY - the probe lives in bootstrap dlssflat.cpp)
    LOG("[WRISTPROBE] ON - gamedll import swaps: IsActuallyVisible %s, IUIText::SetText %s, SetOpacity %s | snapshot every %llu ms "
        "inside a game call whose element's root is HUD_DI; lines printed from this thread. Pair with [UIMTX] "
        "(dlvr\\ui_matrix_probe.txt) for the HUD's draws.", o1 ? "LIVE" : "*** IAT slot NOT FOUND ***",
        o2 ? "LIVE" : "*** IAT slot NOT FOUND ***", o3 ? "LIVE" : "*** IAT slot NOT FOUND ***",
        static_cast<unsigned long long>(kSnapEveryMs));
    if (!g_live) { return 0; }
    LONG prev[kThreads][kKinds] = {};
    LONG prevNoSlot = 0;
    ULONGLONG next = GetTickCount64() + 5000;
    bool saidFirst = false;
    for (;;) {
        Sleep(100);
        PrintSnapshot();
        const ULONGLONG now = GetTickCount64();
        if (now < next) { continue; }
        next = now + 5000;
        char line[1200] = {};
        size_t at = 0;
        long total = 0;
        for (int i = 0; i < kThreads; ++i) {
            const DWORD t = g_tid[i];
            if (!t) { continue; }
            LONG d[kKinds];
            for (int k = 0; k < kKinds; ++k) { const LONG c = g_calls[i][k]; d[k] = c - prev[i][k]; prev[i][k] = c; total += d[k]; }
            const int n = _snprintf_s(line + at, sizeof(line) - at, _TRUNCATE, " | thread %lu: visible %ld text %ld opacity %ld",
                                      static_cast<unsigned long>(t), static_cast<long>(d[0]), static_cast<long>(d[1]),
                                      static_cast<long>(d[2]));
            if (n > 0) { at += static_cast<size_t>(n); }
        }
        const LONG ns = g_noSlot;
        if (g_probeOn) {
            // [PROBE: WRISTPROBE] gate=dlvr\wrist_probe.txt  site=HEADSET  log-only=yes
            LOG("[WRISTPROBE] THREADS 5 s%s: calls %ld in all%s | calls from a thread beyond the %d slots %ld | snapshots so far %ld%s",
                saidFirst ? "" : " (first, logged unconditionally)", total, line, kThreads, static_cast<long>(ns - prevNoSlot),
                static_cast<long>(g_snaps), total ? "" : " | *** 0 CALLS - the swapped imports are not being called ***");
        }
        if (g_routeOn) {
            static LONG prevPubs = 0;
            static LONG64 prevCost = 0;
            const LONG pubs = g_pubs;
            const LONG64 cost = g_pubCostUs;
            wristleaf::Header h = {};
            AcquireSRWLockShared(&g_pubLock);
            h = g_pubHdr;
            ReleaseSRWLockShared(&g_pubLock);
            int perPiece[kPieces] = {}, texts = 0;
            AcquireSRWLockShared(&g_pubLock);
            for (int i = 0; i < h.count && i < wristleaf::kMaxLeaves; ++i) {
                if (g_pub[i].piece >= 0 && g_pub[i].piece < kPieces) { ++perPiece[g_pub[i].piece]; }
                texts += g_pub[i].text;
            }
            ReleaseSRWLockShared(&g_pubLock);
            char boxes[1600] = {};   // 5 pieces + the minimap / weapon widget segments (~1000 bytes)
            size_t bat = 0;
            static LONG prevRej[kPieces] = {}, prevChg[kPieces] = {}, prevMmUse = 0, prevMmFb = 0;
            {
                const LONG mu = g_mmWidgetUsed, mf = g_mmFallback;
                const int k = _snprintf_s(boxes + bat, sizeof(boxes) - bat, _TRUNCATE, " | minimap box = its WIDGET rect %ld publishes (latest "
                                          "(%.0f,%.0f)-(%.0f,%.0f)), FALLBACK to the largest leaf (widget not read) %ld",
                                          static_cast<long>(mu - prevMmUse), g_mmWidget[0], g_mmWidget[1], g_mmWidget[2], g_mmWidget[3],
                                          static_cast<long>(mf - prevMmFb));
                if (k > 0) { bat += static_cast<size_t>(k); }
                prevMmUse = mu; prevMmFb = mf;
            }
            {
                static LONG prevWpUse = 0, prevWpFb = 0;
                const LONG wu = g_wpWidgetUsed, wf = g_wpFallback;
                const int k = _snprintf_s(boxes + bat, sizeof(boxes) - bat, _TRUNCATE, " | weapon box = its WIDGET rect %ld publishes (latest "
                                          "(%.0f,%.0f)-(%.0f,%.0f)), FALLBACK to the union (widget not read) %ld",
                                          static_cast<long>(wu - prevWpUse), g_wpWidget[0], g_wpWidget[1], g_wpWidget[2], g_wpWidget[3],
                                          static_cast<long>(wf - prevWpFb));
                if (k > 0) { bat += static_cast<size_t>(k); }
                prevWpUse = wu; prevWpFb = wf;
            }
            for (int p = 0; p < kPieces; ++p) {
                const float* b = h.pieceBox[p];
                const LONG rj = g_rej[p], cg = g_chg[p];
                const int k = (h.shownMask & (1 << p))
                    ? _snprintf_s(boxes + bat, sizeof(boxes) - bat, _TRUNCATE, " | %s %d leaves box (%.0f,%.0f)-(%.0f,%.0f) walks "
                                  "rejected %ld, changes taken %ld", kPieceTag[p], perPiece[p], b[0], b[1], b[2], b[3],
                                  static_cast<long>(rj - prevRej[p]), static_cast<long>(cg - prevChg[p]))
                    : _snprintf_s(boxes + bat, sizeof(boxes) - bat, _TRUNCATE, " | %s NOT SHOWN (walks rejected %ld, changes taken %ld)",
                                  kPieceTag[p], static_cast<long>(rj - prevRej[p]), static_cast<long>(cg - prevChg[p]));
                if (k > 0) { bat += static_cast<size_t>(k); }
                prevRej[p] = rj; prevChg[p] = cg;
            }
            const LONG dp = pubs - prevPubs;
            // [PROBE: WRISTROUTE] gate=ui_wrist_hud (read at launch)  site=HEADSET  log-only=no
            LOG("[WRISTROUTE] layer 5 s%s: leaves published %ld times (cost avg %.3f ms, max %.3f ms) | latest gen %ld, %d leaves (%d text), "
                "root (%.0f,%.0f)-(%.0f,%.0f), age %llu ms%s | other roots %ld, faults %ld, truncated %ld%s",
                saidFirst ? "" : " (first, logged unconditionally)", static_cast<long>(dp),
                dp ? static_cast<double>(cost - prevCost) / 1000.0 / dp : 0.0, static_cast<double>(g_pubMaxUs) / 1000.0, h.gen, h.count,
                texts, h.root[0], h.root[1], h.root[2], h.root[3],
                h.takenMs ? static_cast<unsigned long long>(GetTickCount64() - h.takenMs) : 0ull, boxes,
                static_cast<long>(g_pubOtherRoot), static_cast<long>(g_pubFaults), static_cast<long>(g_pubTrunc),
                h.gen ? "" : " | *** NOTHING PUBLISHED YET ***");
            static LONG prevHeld = 0, prevCorr = 0;
            const LONG held = g_fadesHeld, corr = g_corrected;
            // [PROBE: WRISTROUTE] gate=ui_wrist_hud (read at launch)  site=HEADSET  log-only=no
            LOG("[WRISTROUTE] layer 5 s HOLD OPAQUE: the game's fades of the five pieces held at the setter %ld, corrections at a publish "
                "(a fade that went around the setter) %ld | the element gamedll +4408D0 reads GetActualOpacity of: %s",
                static_cast<long>(held - prevHeld), static_cast<long>(corr - prevCorr),
                g_actOpRead ? g_actOpName : "not read yet (its global / +3568 / +224 not set, or nothing published)");
            prevHeld = held; prevCorr = corr;
            prevPubs = pubs; prevCost = cost;
        }
        prevNoSlot = ns;
        saidFirst = true;
    }
}

}  // namespace

void Start()
{
    if (InterlockedExchange(&g_started, 1) != 0) { return; }
    char marker[MAX_PATH] = {}, route[MAX_PATH] = {};
    if (!DlvrFilePath("wrist_probe.txt", marker, sizeof(marker)) || !DlvrFilePath("wrist_route.txt", route, sizeof(route))) {
        // [PROBE: WRISTPROBE] gate=dlvr\wrist_probe.txt  site=HEADSET  log-only=yes
        LOG("[WRISTPROBE] off - could not build the dlvr\\wrist_probe.txt / wrist_route.txt paths.");
        return;
    }
    g_probeOn = GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES;
    // STAGE 3a (trace docs/traces/20261113_1040_wrist_stage3a_settings.md): the setting ui_wrist_hud, latched for this launch (the
    // FIRST OnThisLaunch call - this one, after LoadLayerConfig), replaces the marker dlvr\wrist_route.txt. A marker left on disk is
    // named so a stale one is not mistaken for the switch.
    // [LOADBEARING: ui_wrist_hud] the route latch + the thread start below: with ui_wrist_hud on, this file is the wrist HUD's plumbing, not a probe
    g_routeOn = wristquad::OnThisLaunch();
    const bool oldMarker = GetFileAttributesA(route) != INVALID_FILE_ATTRIBUTES;
    // [PROBE: WRISTROUTE] gate=ui_wrist_hud (read at launch)  site=HEADSET  log-only=no
    LOG("[WRISTROUTE] layer: %s (logged unconditionally) - ui_wrist_hud=%d at launch.%s", g_routeOn ? "ON - the HUD's leaves are "
        "published for the bootstrap's draw routing (dlvr_WristLeaves)" : "off", g_routeOn ? 1 : 0,
        oldMarker ? " (the old marker dlvr\\wrist_route.txt is on disk - NOT read any more; the setting decides)" : "");
    if (!g_probeOn && !g_routeOn) {
        // [PROBE: WRISTPROBE] gate=dlvr\wrist_probe.txt  site=HEADSET  log-only=yes
        LOG("[WRISTPROBE] off - marker %s absent (create it for the wrist HUD stage 0 read: UI call threads, the HUD_DI pieces, "
            "the HUD components). Nothing installed.", marker);
        return;
    }
    HANDLE th = CreateThread(nullptr, 0, ProbeThread, nullptr, 0, nullptr);
    // [PROBE: WRISTPROBE] gate=dlvr\wrist_probe.txt  site=HEADSET  log-only=yes
    LOG("[WRISTPROBE] %s - thread %s (it waits for the gamedll, then swaps three imports).", g_probeOn ? "stage 0 probe ON" :
        "stage 0 probe off (its lines not printed); the imports are swapped for stage 1", th ? "started" : "*** NOT started ***");
    if (th) { CloseHandle(th); }
}

// The bootstrap's copy of the latest leaves (wristleaf.h LeavesFn), once per HUD pass on the render thread. -1 = none.
int CopyLeaves(wristleaf::Header* hdr, wristleaf::Leaf* out, int max)
{
    // [LOADBEARING: ui_wrist_hud] the bootstrap's only copy of the leaves (via dlvr_WristLeaves, dlssflat.cpp g_leavesFn)
    if (!g_routeOn || !hdr || !out || max <= 0) { return -1; }
    AcquireSRWLockShared(&g_pubLock);
    const int n = g_pubHdr.gen ? (g_pubHdr.count < max ? g_pubHdr.count : max) : -1;
    if (n >= 0) {
        *hdr = g_pubHdr;
        memcpy(out, g_pub, sizeof(wristleaf::Leaf) * static_cast<size_t>(n));
    }
    ReleaseSRWLockShared(&g_pubLock);
    return n;
}

float UiScreenH()
{
    // [LOADBEARING: ui_wrist_hud] the wrist quads' metres-per-UI-pixel (wristquad.cpp MetresPerUiPxNow)
    AcquireSRWLockShared(&g_pubLock);
    const float h = g_pubHdr.gen ? g_pubHdr.root[3] : 0.0f;
    ReleaseSRWLockShared(&g_pubLock);
    return h > 0.0f ? h : 0.0f;
}

}  // namespace wristprobe
}  // namespace dlvr

// MENU / WHEEL STAND-DOWN (traces docs/traces/20261112_2135_wrist_menu_standdown.md + 20261112_2150_wrist_standdown_drop.md; Jay:
// "Skills tab specifically loses 3 of its sub tabs" -> "the original HUD flashing when opening menu or weapon wheel"): while an in-game
// menu (uipin::GameMenuOpen) or a selection wheel (HolsterWheelShowing) is open, the copy carries standDown = 1. The router then uses only
// its strict rules and DROPS what they give a piece (the game's HUD still drawn in the first / last frames of a menu or wheel - RUN WS2j
// video); everything else (the menu, the wheel) draws as made. The wrist quads hide meanwhile. COST: two volatile loads + GetTickCount64.
namespace {
volatile LONG g_standDown = -1;   // last state reported: -1 never, 0 routing, 1 stood down
volatile LONG g_standLines = 0, g_standCount = 0;
}
extern "C" __declspec(dllexport) int dlvr_WristLeaves(::dlvr::wristleaf::Header* hdr, ::dlvr::wristleaf::Leaf* out, int max)
{
    // [LOADBEARING: ui_wrist_hud] the export the bootstrap resolves by name (dlssflat.cpp GetProcAddress "dlvr_WristLeaves")
    const int n = ::dlvr::wristprobe::CopyLeaves(hdr, out, max);
    const bool menu = ::dlvr::uipin::GameMenuOpen();
    const bool wheel = ::dlvr::HolsterWheelShowing();
    // + the 3D HUD's HUD-screen signal: it leaves the visible list 26-30 ms (2-3 frames) before the menu state reads open (RUN WS2m,
    // all 9 openings; INV-539's menu-open gap) - the frames the Legend icon / map flashed on the eyes (trace 20261112_2235).
    const bool hudGone = ::dlvr::uipanel::HudScreenSig() == 0;
    const LONG now = (menu || wheel || hudGone) ? 1 : 0;
    if (hdr) { hdr->standDown = now ? 1 : 0; }
    if (InterlockedExchange(&g_standDown, now) != now) {
        const LONG c = InterlockedIncrement(&g_standCount);
        if (InterlockedIncrement(&g_standLines) <= 20) {
            // [PROBE: WRISTROUTE] gate=ui_wrist_hud (read at launch)  site=HEADSET  log-only=no
            LOG("[WRISTROUTE] layer: router %s (change %ld; first 20 logged) - in-game menu %d, selection wheel %d, HUD screen gone %d",
                now ? "STANDS DOWN (strict matches dropped, everything else as made, the wrist quads hide)" : "ROUTES again",
                static_cast<long>(c), menu ? 1 : 0, wheel ? 1 : 0, hudGone ? 1 : 0);
        }
    }
    return n;
}
