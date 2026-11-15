// invkeep.cpp - INVENTORY KEEP-BUILT: opening the inventory with nothing changed since its last build keeps the built list instead of
// deleting and re-cloning every button (the open stutter and the wait before the menu is on its quad - INV-526). A FIX, no key, always on
// (Jay 2026-11-14: "I'd say this is more a fix than a feature"; "Both" = phase 1 + phase 2). Trace
// docs/traces/20261114_1415_inventory_keep_built.md; facts INV-526 addendum 2026-11-14 (ida/inv1..inv34, OUR gamedll 97046bba).
//
// THE GAME SIDE: MenuInventory show (vtable slot 154, +8D08C0) -> +8D6910 -> +8D6400(a1, tab 0, force 1) -> `call +8D1460` (backpack
//   rebuild, at +8D66A2) and `call +8D41C0` (quick-slot rebuild, at +8D66AA) - each the only call to its function. Each rebuild CLEARS its
//   UIListBox (every button queued for DetachFromParent + DeleteThisRecursive at frame end) and clones one button per item. The close
//   keeps the buttons (ShowScreen hides the old screen with 0), so only the open's forced rebuild throws them away.
// WHAT IS HOOKED (all found by RTTI name or translated by buildrva; every target byte- or address-checked, refused by name otherwise):
//   (1) the two CALL SITES in +8D6400 -> E8 rel32 to a stub page within reach -> BackpackHook / QuickHook (+8D1460's entry has a jz rel32
//       at +3, so no inline detour). PatchGameBytes = byte check + every other thread suspended.
//   (2) MenuInventory slot 154 -> ShowHook: marks "this rebuild is an OPEN" (thread-local) around the original.
//   (3) MainMenuModule / MainMenuModuleDI slot 184 (ShowScreen +1201A10) -> ShowScreenHook: the original FIRST, then PHASE 2 - once per
//       level, a hidden build of each loaded MenuInventory screen so the first open finds a fingerprint.
// THE RULE: on an OPEN, same MenuInventory, both lists holding buttons, fingerprint == the one taken at its last full build -> neither
//   rebuild runs (everything else +8D6400 / +8D08C0 do still runs). Anything else -> the game's rebuild + a new fingerprint.
// THE FINGERPRINT (read 2): the player inventory's six item groups (the rebuild's own enumeration: container +2360 vtable +120, categories
//   4 1 8 3 0 9) - each item pointer, +264, +272..+275, +276, its elemental values (item+88 vtable +0 has / +128 value, types 16..0) and
//   the sort test +BC2A40; plus the 8 quick slots (+10648 / +10656, +11048 / +11056). Read only; every read / call inside __try.
#include "invkeep.h"

#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "../common/log.h"
#include "boneblend.h"    // PatchGameBytes - the shared byte-check + suspend + write path
#include "rtti_vtable.h"  // RttiFindVtable

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

#if __has_include("../common/buildrva.h")
#include "../common/buildrva.h"
#define DLVR_IK_GD(rva) ::dlvr::buildrva::Gd(rva)
#else
#define DLVR_IK_GD(rva) (rva)
#endif

namespace dlvr {
namespace {

// Steam rvas (OUR gamedll 97046bba); the EGL ones come from common/buildrva_table.h (tools/egl_allowlist.tsv, all exact).
constexpr uintptr_t kBackpackRva    = 0x8D1460;   // the backpack rebuild
constexpr uintptr_t kQuickRva       = 0x8D41C0;   // the quick-slot rebuild
constexpr uintptr_t kBackpackCallRva = 0x8D66A2;  // +8D6400's `call +8D1460`
constexpr uintptr_t kQuickCallRva   = 0x8D66AA;   // +8D6400's `call +8D41C0`
constexpr uintptr_t kShowRva        = 0x8D08C0;   // MenuInventory show = slot 154
constexpr uintptr_t kShowScreenRva  = 0x1201A10;  // MainMenuModule ShowScreen = slot 184
constexpr uintptr_t kTabRebuildRva  = 0x8D6400;   // MenuInventory tab rebuild (called for the pre-build)
constexpr uintptr_t kSortTestRva    = 0xBC2A40;   // the inventory sort's per-item test
constexpr uintptr_t kPlayerGlobalRva = 0x1C16358; // -> +1920 = the player inventory
constexpr uintptr_t kGameGlobalRva  = 0x1C16348;  // -> +1344 -> +184 -> +88 = the sort test's owner
constexpr uintptr_t kInvIdRva[3]    = { 0x1BA6180, 0x1BA61CC, 0x1BA61DC };   // the inventory screen ids (+7D5000)
constexpr uintptr_t kHideRva        = 0x8D1280;   // MenuInventory hide(clear) = slot 155 (round 2, trace 20261114_1630)
constexpr uintptr_t kClearRva       = 0x1169420;  // the UIListBox clear = its +88 vtable slot 4
constexpr int kShowSlot = 154, kHideSlot = 155, kShowScreenSlot = 184, kGetScreenSlot = 183, kClearSlot = 4;
constexpr int kCats[6] = { 4, 1, 8, 3, 0, 9 };

typedef void (__fastcall* ShowFn)(void*, void*);
typedef void (__fastcall* ShowScreenFn)(void*, unsigned, void*, unsigned char, int, char);
typedef void (__fastcall* BackpackFn)(void*, void*, int);
typedef void (__fastcall* QuickFn)(void*);
typedef void (__fastcall* TabRebuildFn)(void*, int, char);
typedef void* (__fastcall* GetScreenFn)(void*, int, bool);
typedef void (__fastcall* EnumFn)(void*, void*, int);
typedef unsigned char (__fastcall* HasFn)(void*, unsigned);
typedef int (__fastcall* ValFn)(void*, unsigned);
typedef char (__fastcall* SortFn)(void*, void*);
typedef void (__fastcall* HideFn)(void*, unsigned char, void*);
typedef void (__fastcall* ClearFn)(void*);

struct GameVec { void** d; uint32_t n; uint32_t cap; };   // the enumeration's out vector (data, count, capacity), freed with free

uintptr_t     g_gd = 0;
int           g_state = 0;            // 0 not tried / waiting, 1 live, -1 refused (final)
ShowFn        g_showTrue = nullptr;
ShowScreenFn  g_ssTrue = nullptr;
BackpackFn    g_backpackTrue = nullptr;
QuickFn       g_quickTrue = nullptr;
TabRebuildFn  g_tabRebuild = nullptr;
SortFn        g_sortTest = nullptr;
uintptr_t     g_invVt = 0;
bool          g_ssLive = false;

thread_local int  t_open = 0;         // ShowHook is on the stack of this thread
thread_local int  t_skip = 0;         // BackpackHook skipped in this +8D6400 call -> QuickHook skips too
thread_local int  t_building = 0;     // BackpackHook ran the original -> QuickHook stores the pending fingerprint
thread_local uint64_t t_pendFp = 0;
thread_local int  t_pendOk = 0;
thread_local int  t_pendItems = 0;
// ROUND 2 (trace 20261114_1630): the menu close's hide(1) clears both lists; these two +88 sub-objects are kept on this thread meanwhile
thread_local void* t_keepB = nullptr;
thread_local void* t_keepQ = nullptr;
HideFn        g_hideTrue = nullptr;
ClearFn       g_clearTrue = nullptr;
int           g_clearHook = 0;        // 0 not yet, 1 our ClearHook is in the lists' +88 vtable slot 4, -1 refused (named once)
volatile LONG g_closeKept = 0, g_closeLines = 0;

struct FpRec { void* inv; uint64_t fp; int items; };
FpRec         g_fp[4] = {};
void*         g_level = nullptr;      // the player inventory the table belongs to (a new one = a new level: the table is dropped)
bool          g_prebuilt = false;
volatile LONG g_kept = 0, g_rebuilt = 0, g_lines = 0, g_preTries = 0;

inline uint64_t Mix(uint64_t h, uint64_t v)
{
    for (int i = 0; i < 8; ++i) { h ^= (v >> (8 * i)) & 0xFF; h *= 1099511628211ull; }
    return h;
}

void* PlayerInventory()
{
    __try {
        const uintptr_t g = *reinterpret_cast<const uintptr_t*>(g_gd + DLVR_IK_GD(kPlayerGlobalRva));
        return g ? *reinterpret_cast<void* const*>(g + 1920) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* SortOwner()
{
    __try {
        const uintptr_t g = *reinterpret_cast<const uintptr_t*>(g_gd + DLVR_IK_GD(kGameGlobalRva));
        const uintptr_t a = g ? *reinterpret_cast<const uintptr_t*>(g + 1344) : 0;
        const uintptr_t b = a ? *reinterpret_cast<const uintptr_t*>(a + 184) : 0;
        if (!b || !*reinterpret_cast<const uintptr_t*>(b + 80)) { return nullptr; }
        return *reinterpret_cast<void* const*>(b + 88);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// One item into the hash: pointer, the four state words, the elemental values, the sort test. False = a fault.
bool MixItem(uint64_t& h, uintptr_t p, void* owner)
{
    __try {
        h = Mix(h, p);
        // bit 0x08 of +264 = the "seen" mark the list focus handler sets (+8D3FE0); the built button never shows it (round 2)
        h = Mix(h, *reinterpret_cast<const uint32_t*>(p + 264) & ~0x08u);
        h = Mix(h, *reinterpret_cast<const uint32_t*>(p + 272));
        h = Mix(h, *reinterpret_cast<const uint32_t*>(p + 276));
        void* q = reinterpret_cast<void*>(p + 88);
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(q);
        const HasFn has = *reinterpret_cast<const HasFn*>(vt + 0);
        const ValFn val = *reinterpret_cast<const ValFn*>(vt + 128);
        for (int t = 16; t >= 0; --t) {
            if (has(q, static_cast<unsigned>(t))) { h = Mix(h, 0x100u + t); h = Mix(h, static_cast<uint32_t>(val(q, static_cast<unsigned>(t)))); }
        }
        if (owner && g_sortTest) { h = Mix(h, g_sortTest(owner, q) ? 1u : 0u); }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool EnumGroup(void* cont, int cat, GameVec& v)
{
    __try {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(cont);
        const EnumFn fn = *reinterpret_cast<const EnumFn*>(vt + 120);
        fn(cont, &v, cat);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool MixQuick(uint64_t& h, uintptr_t inv)
{
    __try {
        for (int set = 0; set < 2; ++set) {
            const uintptr_t arr = *reinterpret_cast<const uintptr_t*>(inv + (set ? 11048 : 10648));
            const uint32_t n = *reinterpret_cast<const uint32_t*>(inv + (set ? 11056 : 10656));
            for (uint32_t j = 0; j < 4; ++j) {
                const uintptr_t it = (j < n && arr) ? *reinterpret_cast<const uintptr_t*>(arr + 24ull * j) : 0;
                h = Mix(h, it);
            }
        }
        h = Mix(h, *reinterpret_cast<const uintptr_t*>(inv + 10672));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The whole fingerprint. False = no inventory or a fault (never compared then: the game rebuilds).
bool Fingerprint(uint64_t& out, int& items)
{
    items = 0;
    void* invp = PlayerInventory();
    if (!invp) { return false; }
    const uintptr_t inv = reinterpret_cast<uintptr_t>(invp);
    void* cont = nullptr;
    __try { cont = *reinterpret_cast<void* const*>(inv + 2360); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!cont) { return false; }
    void* owner = SortOwner();
    uint64_t h = 1469598103934665603ull;
    bool ok = true;
    for (int c = 0; c < 6 && ok; ++c) {
        GameVec v = {};
        if (!EnumGroup(cont, kCats[c], v)) { ok = false; break; }
        h = Mix(h, 0xC0000000u + kCats[c]);
        h = Mix(h, v.n);
        for (uint32_t i = 0; i < v.n && ok; ++i) {
            const uintptr_t p = v.d ? reinterpret_cast<uintptr_t>(v.d[i]) : 0;
            if (p && !MixItem(h, p, owner)) { ok = false; }
            ++items;
        }
        free(v.d);   // the game's realloc'd buffer - one ucrtbase heap (both DLLs import api-ms-win-crt-heap)
    }
    if (ok && !MixQuick(h, inv)) { ok = false; }
    out = h;
    return ok;
}

FpRec* FindFp(void* inv)
{
    for (FpRec& r : g_fp) { if (r.inv == inv) { return &r; } }
    return nullptr;
}

void StoreFp(void* inv, uint64_t fp, int items)
{
    FpRec* r = FindFp(inv);
    if (!r) { for (FpRec& s : g_fp) { if (!s.inv) { r = &s; break; } } }
    if (!r) { r = &g_fp[0]; }
    r->inv = inv; r->fp = fp; r->items = items;
}

// A new player inventory = a new level: the table and the pre-build start again.
void LevelCheck()
{
    void* inv = PlayerInventory();
    if (inv && inv != g_level) {
        g_level = inv;
        memset(g_fp, 0, sizeof(g_fp));
        g_prebuilt = false;
    }
}

bool ListsHoldButtons(void* a1)
{
    __try {
        const uintptr_t b = *reinterpret_cast<const uintptr_t*>(reinterpret_cast<uintptr_t>(a1) + 1152);
        const uintptr_t q = *reinterpret_cast<const uintptr_t*>(reinterpret_cast<uintptr_t>(a1) + 1160);
        return b && q && *reinterpret_cast<const uint32_t*>(b + 128) > 0 && *reinterpret_cast<const uint32_t*>(q + 128) > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Decision(bool kept, const char* why, uint64_t fp, int items)
{
    const LONG k = kept ? InterlockedIncrement(&g_kept) : g_kept;
    const LONG r = kept ? g_rebuilt : InterlockedIncrement(&g_rebuilt);
    const LONG n = InterlockedIncrement(&g_lines);
    if (n <= 30 || n % 50 == 0) {
        // [PROBE: INVKEEP] gate=none (a fix, always on)  site=FLAT  log-only=no
        LOG("invkeep: [INVKEEP] %s %s (fingerprint %016llX, %d items) | so far kept %ld, rebuilt %ld%s", kept ? "KEPT the built list -" :
            "REBUILT -", why, static_cast<unsigned long long>(fp), items, static_cast<long>(k), static_cast<long>(r),
            n == 30 ? " (first 30 lines printed, then every 50th)" : "");
    }
}

void __fastcall BackpackHook(void* a1, void* list, int a3)
{
    t_skip = 0;
    t_building = 0;
    LevelCheck();
    uint64_t fp = 0;
    int items = 0;
    const bool fpOk = Fingerprint(fp, items);
    if (t_open) {
        const FpRec* r = FindFp(a1);
        const char* why = nullptr;
        if (!fpOk) { why = "the fingerprint could not be read"; }
        else if (!r) { why = "no build of this screen yet"; }
        else if (r->fp != fp) { why = "the inventory changed"; }
        else if (!ListsHoldButtons(a1)) { why = "a list holds no buttons"; }
        if (!why) {
            t_skip = 1;
            Decision(true, "opened with nothing changed", fp, items);
            return;
        }
        Decision(false, why, fp, items);
    }
    t_pendFp = fp;
    t_pendOk = fpOk ? 1 : 0;
    t_pendItems = items;
    t_building = 1;
    g_backpackTrue(a1, list, a3);
}

void __fastcall QuickHook(void* a1)
{
    if (t_skip) { t_skip = 0; return; }
    g_quickTrue(a1);
    if (t_building && t_pendOk) { StoreFp(a1, t_pendFp, t_pendItems); }
    t_building = 0;
}

void __fastcall ShowHook(void* a1, void* a2)
{
    ++t_open;
    g_showTrue(a1, a2);
    --t_open;
}

// PHASE 2: after the game's own ShowScreen, once per level - each loaded MenuInventory screen built hidden (the open flag is false, so
// the call-site hooks build + fingerprint). Nothing missing -> done; anything missing -> the next ShowScreen tries again.
void PreBuild(void* menu)
{
    if (g_prebuilt || !g_tabRebuild) { return; }
    LevelCheck();
    if (!PlayerInventory()) { return; }
    const LONG tries = InterlockedIncrement(&g_preTries);
    int built = 0, found = 0;
    const char* why = nullptr;
    __try {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(menu);
        const GetScreenFn get = *reinterpret_cast<const GetScreenFn*>(vt + 8 * kGetScreenSlot);
        for (int i = 0; i < 3; ++i) {
            const int id = *reinterpret_cast<const int*>(g_gd + DLVR_IK_GD(kInvIdRva[i]));
            if (id < 0) { continue; }
            void* scr = get(menu, id, false);   // false = never load: an unloaded screen is skipped
            if (!scr || *reinterpret_cast<const uintptr_t*>(scr) != g_invVt) { continue; }
            ++found;
            const uintptr_t s = reinterpret_cast<uintptr_t>(scr);
            if (!*reinterpret_cast<const uintptr_t*>(s + 1152) || !*reinterpret_cast<const uintptr_t*>(s + 1160)) { continue; }
            if (FindFp(scr)) { ++built; continue; }
            g_tabRebuild(scr, 0, 1);
            if (FindFp(scr)) { ++built; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault while reading the menu module / screens";
    }
    if (built > 0 && built == found) { g_prebuilt = true; }
    if (tries <= 10 || g_prebuilt) {
        // [PROBE: INVKEEP] gate=none (a fix, always on)  site=FLAT  log-only=no
        LOG("invkeep: [INVKEEP] pre-build try %ld: inventory screens found %d, built + fingerprinted %d%s%s", static_cast<long>(tries), found,
            built, g_prebuilt ? " - DONE for this level (the first open keeps it if nothing changes)" : " - retried at the next ShowScreen",
            why ? why : "");
    }
}

void __fastcall ShowScreenHook(void* a1, unsigned a2, void* a3, unsigned char a4, int a5, char a6)
{
    g_ssTrue(a1, a2, a3, a4, a5, a6);
    if (!t_open) { PreBuild(a1); }
}

// One page within rel32 reach of the gamedll (ovr2xr.cpp AllocNearExec's walk).
unsigned char* AllocNear(uintptr_t base)
{
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    for (uintptr_t d = 0x01000000; d < 0x60000000; d += 0x00100000) {
        const uintptr_t cand[2] = { (base + 0x01000000 + d) & ~(gran - 1), base > d ? ((base - d) & ~(gran - 1)) : 0 };
        for (uintptr_t c : cand) {
            if (!c) { continue; }
            void* p = VirtualAlloc(reinterpret_cast<void*>(c), 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) { return static_cast<unsigned char*>(p); }
        }
    }
    return nullptr;
}

bool ReadQ(uintptr_t a, uintptr_t& out)
{
    __try { out = *reinterpret_cast<const uintptr_t*>(a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { out = 0; return false; }
}

// `site` must be E8 rel32 resolving to `callee`. Fills the 5 live bytes.
bool CallSiteIs(uintptr_t site, uintptr_t callee, unsigned char live[5])
{
    __try {
        memcpy(live, reinterpret_cast<const void*>(site), 5);
        int32_t rel = 0;
        memcpy(&rel, live + 1, 4);
        return live[0] == 0xE8 && site + 5 + static_cast<intptr_t>(rel) == callee;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SwapSlot(uintptr_t slot, void* repl)
{
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), PAGE_READWRITE, &old)) { return false; }
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot), repl);
    DWORD unused = 0;
    VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), old, &unused);
    uintptr_t back = 0;
    return ReadQ(slot, back) && back == reinterpret_cast<uintptr_t>(repl);
}

// ROUND 2 (trace 20261114_1630): the lists' +88 vtable slot 4. A list this thread marked "keep" (inside the close's hide) is not cleared;
// every other list and call goes to the game's clear.
void __fastcall ClearHook(void* list88)
{
    if (list88 && (list88 == t_keepB || list88 == t_keepQ)) { return; }
    g_clearTrue(list88);
}

// The clear hook goes into the vtable the list object actually carries (a subclass would carry its own), once, and only if that slot
// holds the clear. True = ClearHook is in place.
bool EnsureClearHook(void* list88)
{
    if (g_clearHook != 0) { return g_clearHook == 1; }
    uintptr_t vt = 0, have = 0;
    const char* why = nullptr;
    if (!ReadQ(reinterpret_cast<uintptr_t>(list88), vt) || !vt || !ReadQ(vt + 8 * kClearSlot, have)) { why = "the list's +88 vtable could not be read"; }
    else if (have == reinterpret_cast<uintptr_t>(&ClearHook)) { g_clearHook = 1; return true; }
    else if (have != reinterpret_cast<uintptr_t>(g_clearTrue)) { why = "the list's +88 vtable slot 4 is not the UIListBox clear"; }
    else if (!SwapSlot(vt + 8 * kClearSlot, reinterpret_cast<void*>(&ClearHook))) { why = "the slot swap failed"; }
    g_clearHook = why ? -1 : 1;
    // [LOADBEARING: inventory keep-built close keep] the inventory lists' +88 vtable slot 4 (the clear +1169420) -> ClearHook: the menu
    //      close's hide(1) no longer empties them. Without it every open after a close rebuilds (RUN IK1: "a list holds no buttons").
    // [PROBE: INVKEEP] gate=none (a fix, always on)  site=FLAT  log-only=no
    LOG("invkeep: [INVKEEP] close keep %s (logged once) - the lists' +88 vtable +%zX slot %d %s", why ? "*** REFUSED ***" : "LIVE",
        static_cast<size_t>(vt ? vt - g_gd : 0), kClearSlot, why ? why : "-> ClearHook (the close's hide keeps both lists)");
    return !why;
}

// MenuInventory slot 155 - hide(clear). With clear set (the menu close: HideAllUIScreens -> slot 156), this screen's two lists are
// marked "keep" for the duration of the game's hide; every other reset it makes still runs.
void __fastcall HideHook(void* a1, unsigned char clear, void* a3)
{
    void* b = nullptr;
    void* q = nullptr;
    if (clear) {
        uintptr_t lb = 0, lq = 0;
        if (ReadQ(reinterpret_cast<uintptr_t>(a1) + 1152, lb) && ReadQ(reinterpret_cast<uintptr_t>(a1) + 1160, lq) && lb && lq &&
            EnsureClearHook(reinterpret_cast<void*>(lb + 88))) {
            b = reinterpret_cast<void*>(lb + 88);
            q = reinterpret_cast<void*>(lq + 88);
        }
    }
    t_keepB = b;
    t_keepQ = q;
    g_hideTrue(a1, clear, a3);
    t_keepB = nullptr;
    t_keepQ = nullptr;
    if (b) {
        const LONG k = InterlockedIncrement(&g_closeKept);
        const LONG n = InterlockedIncrement(&g_closeLines);
        if (n <= 20 || n % 50 == 0) {
            // [PROBE: INVKEEP] gate=none (a fix, always on)  site=FLAT  log-only=no
            LOG("invkeep: [INVKEEP] close: both lists KEPT through the hide (closes kept so far %ld)%s", static_cast<long>(k),
                n == 20 ? " (first 20 lines printed, then every 50th)" : "");
        }
    }
}

void Refuse(const char* why)
{
    g_state = -1;
    // [PROBE: INVKEEP] gate=none (a fix, always on)  site=FLAT  log-only=no
    LOG("invkeep: [INVKEEP] *** NOT INSTALLED: %s - every inventory open rebuilds as before ***", why);
}

}  // namespace

void InvKeepInstall()
{
    if (g_state != 0) { return; }
    const uintptr_t gd = reinterpret_cast<uintptr_t>(GetModuleHandleA("gamedll_x64_rwdi.dll"));
    if (!gd) { return; }   // retried by camhook's 1 Hz thread
    g_gd = gd;
    const uintptr_t backpack = gd + DLVR_IK_GD(kBackpackRva), quick = gd + DLVR_IK_GD(kQuickRva);
    const uintptr_t siteB = gd + DLVR_IK_GD(kBackpackCallRva), siteQ = gd + DLVR_IK_GD(kQuickCallRva);
    const uintptr_t show = gd + DLVR_IK_GD(kShowRva), showScreen = gd + DLVR_IK_GD(kShowScreenRva);
    unsigned char liveB[5] = {}, liveQ[5] = {};
    if (!CallSiteIs(siteB, backpack, liveB) || !CallSiteIs(siteQ, quick, liveQ)) {
        char why[200];
        _snprintf_s(why, sizeof(why), _TRUNCATE, "the tab rebuild's two calls at gamedll +%zX / +%zX are not `call +%zX` / `call +%zX` "
                    "(%02X ... / %02X ...) - an unknown gamedll build", static_cast<size_t>(siteB - gd), static_cast<size_t>(siteQ - gd),
                    static_cast<size_t>(backpack - gd), static_cast<size_t>(quick - gd), liveB[0], liveQ[0]);
        Refuse(why);
        return;
    }
    int st = 0, stM = 0, stD = 0;
    const uintptr_t invVt = RttiFindVtable(gd, ".?AVMenuInventory@@", 0, &st);
    const uintptr_t mmVt = RttiFindVtable(gd, ".?AVMainMenuModule@@", 0, &stM);
    const uintptr_t mdVt = RttiFindVtable(gd, ".?AVMainMenuModuleDI@@", 0, &stD);
    uintptr_t haveShow = 0;
    if (!invVt || !ReadQ(invVt + 8 * kShowSlot, haveShow) || haveShow != show) {
        char why[200];
        _snprintf_s(why, sizeof(why), _TRUNCATE, "MenuInventory's vtable (stage %d) slot %d does not hold the show function gamedll +%zX "
                    "(holds +%zX)", st, kShowSlot, static_cast<size_t>(show - gd), static_cast<size_t>(haveShow ? haveShow - gd : 0));
        Refuse(why);
        return;
    }
    unsigned char* page = AllocNear(gd);
    if (!page) { Refuse("no free page within rel32 reach of the gamedll for the two stubs"); return; }
    const void* targets[2] = { reinterpret_cast<void*>(&BackpackHook), reinterpret_cast<void*>(&QuickHook) };
    for (int i = 0; i < 2; ++i) {   // mov rax, imm64 ; jmp rax
        unsigned char* s = page + 16 * i;
        s[0] = 0x48; s[1] = 0xB8;
        const uintptr_t t = reinterpret_cast<uintptr_t>(targets[i]);
        memcpy(s + 2, &t, 8);
        s[10] = 0xFF; s[11] = 0xE0;
    }
    FlushInstructionCache(GetCurrentProcess(), page, 32);
    g_backpackTrue = reinterpret_cast<BackpackFn>(backpack);
    g_quickTrue = reinterpret_cast<QuickFn>(quick);
    g_tabRebuild = reinterpret_cast<TabRebuildFn>(gd + DLVR_IK_GD(kTabRebuildRva));
    g_sortTest = reinterpret_cast<SortFn>(gd + DLVR_IK_GD(kSortTestRva));
    g_showTrue = reinterpret_cast<ShowFn>(show);
    g_invVt = invVt;
    const uintptr_t stubs[2] = { reinterpret_cast<uintptr_t>(page), reinterpret_cast<uintptr_t>(page + 16) };
    const uintptr_t sites[2] = { siteB, siteQ };
    const unsigned char* lives[2] = { liveB, liveQ };
    int attempts[2] = {};
    bool patched[2] = {};
    for (int i = 0; i < 2; ++i) {
        const intptr_t rel = static_cast<intptr_t>(stubs[i]) - static_cast<intptr_t>(sites[i] + 5);
        if (rel > INT32_MAX || rel < INT32_MIN) { break; }
        unsigned char repl[5] = { 0xE8 };
        const int32_t r32 = static_cast<int32_t>(rel);
        memcpy(repl + 1, &r32, 4);
        // [LOADBEARING: inventory keep-built call sites] +8D6400's `call +8D1460` / `call +8D41C0` (+8D66A2 / +8D66AA) -> BackpackHook /
        //      QuickHook. Without them every open deletes and re-clones the whole list (the stutter, INV-526).
        patched[i] = PatchGameBytes(sites[i], lives[i], repl, 5, &attempts[i]);
        if (!patched[i]) { break; }
    }
    if (!patched[0] || !patched[1]) {
        // a half install is never left: the backpack site goes back if the quick one failed
        if (patched[0]) {
            unsigned char cur[5] = {};
            memcpy(cur, reinterpret_cast<const void*>(siteB), 5);
            PatchGameBytes(siteB, cur, liveB, 5, &attempts[0]);
        }
        char why[160];
        _snprintf_s(why, sizeof(why), _TRUNCATE, "the call-site writes were refused (backpack %d after %d attempt(s), quick %d after %d)",
                    patched[0] ? 1 : 0, attempts[0], patched[1] ? 1 : 0, attempts[1]);
        Refuse(why);
        return;
    }
    // [LOADBEARING: inventory keep-built show slot] MenuInventory vtable slot 154 -> ShowHook: marks the rebuild as an OPEN. Without it
    //      the call-site hooks never skip (every rebuild is treated as an in-menu action).
    const bool showOk = SwapSlot(invVt + 8 * kShowSlot, reinterpret_cast<void*>(&ShowHook));
    // ROUND 2: MenuInventory slot 155 (hide) -> HideHook, only if it holds +8D1280; the clear's own slot goes in at the first close.
    g_clearTrue = reinterpret_cast<ClearFn>(gd + DLVR_IK_GD(kClearRva));
    uintptr_t haveHide = 0;
    bool hideOk = false;
    if (ReadQ(invVt + 8 * kHideSlot, haveHide) && haveHide == gd + DLVR_IK_GD(kHideRva)) {
        g_hideTrue = reinterpret_cast<HideFn>(haveHide);
        // [LOADBEARING: inventory keep-built hide slot] MenuInventory slot 155 -> HideHook: marks the two lists "keep" inside the close's
        //      hide(1). Without it the close empties them and the next open rebuilds.
        hideOk = SwapSlot(invVt + 8 * kHideSlot, reinterpret_cast<void*>(&HideHook));
    }
    // [PROBE: INVKEEP] gate=none (a fix, always on)  site=FLAT  log-only=no
    LOG("invkeep: [INVKEEP] hide slot %d %s (holds +%zX, expected +%zX)", kHideSlot, hideOk ? "swapped - the close keeps the lists" :
        "*** NOT swapped - the close empties the lists as before; opens after a close rebuild ***",
        static_cast<size_t>(haveHide ? haveHide - gd : 0), static_cast<size_t>(DLVR_IK_GD(kHideRva)));
    // [LOADBEARING: inventory keep-built pre-build] MainMenuModule / MainMenuModuleDI slot 184 (ShowScreen) -> ShowScreenHook: the hidden
    //      build once per level. Without it the first open after a load rebuilds as before (phase 1 still works).
    uintptr_t haveM = 0, haveD = 0;
    const bool mOk = mmVt && ReadQ(mmVt + 8 * kShowScreenSlot, haveM) && haveM == showScreen;
    const bool dOk = mdVt && ReadQ(mdVt + 8 * kShowScreenSlot, haveD) && haveD == showScreen;
    if (mOk && dOk) {
        g_ssTrue = reinterpret_cast<ShowScreenFn>(showScreen);
        g_ssLive = SwapSlot(mmVt + 8 * kShowScreenSlot, reinterpret_cast<void*>(&ShowScreenHook)) &&
                   SwapSlot(mdVt + 8 * kShowScreenSlot, reinterpret_cast<void*>(&ShowScreenHook));
    }
    g_state = showOk ? 1 : -1;
    // [PROBE: INVKEEP] gate=none (a fix, always on)  site=FLAT  log-only=no
    LOG("invkeep: [INVKEEP] %s (logged unconditionally) - call sites gamedll +%zX / +%zX -> stubs %p (%d / %d attempt(s)); MenuInventory "
        "show slot %d at vtable +%zX %s; pre-build ShowScreen slot %d (MainMenuModule stage %d, DI stage %d) %s",
        showOk ? "LIVE" : "*** NOT LIVE - the show slot swap failed: every open rebuilds as before ***", static_cast<size_t>(siteB - gd),
        static_cast<size_t>(siteQ - gd), page, attempts[0], attempts[1], kShowSlot, static_cast<size_t>(invVt - gd),
        showOk ? "swapped" : "NOT swapped", kShowScreenSlot, stM, stD,
        g_ssLive ? "swapped (both)" : (mOk && dOk ? "NOT swapped - the first open after a load rebuilds" :
                                     "NOT found holding ShowScreen - the first open after a load rebuilds"));
}

}  // namespace dlvr
