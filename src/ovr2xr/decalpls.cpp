// decalpls.cpp - DEV + BETA (ported 2026-11-07; Epic gamedll holds the same OnPostCreate code once, at +A82B40, found by bytes), under decal_seat (no key of its own: Jay's fix-without-new-key rule). Seats the map's DecalPLS ENTITY decals (graffiti
// panels + murals placed in <map>.exp, which the .sobj seat in decalseat.cpp never reaches). INV-697 (7)-(12).
// tools/decal_pls.py writes dlvr\decal_pls_<map>.bin: per DecalPLS with a CONFIDENT surface, its .exp translation (engine field 223, the LOCAL
// matrix = world for a parentless entity), the plane normal and the gap.
// WHERE: DecalPLS::OnPostCreate, gamedll +A836A0 = slot 117 of DecalPLS's IGSObject-view vtable (RTTI locator offset 40). Our engine runs it in
// the level's create pass (+2A84B0: every object's OnCreate, then every object's OnPostCreate) or at once for an object made later
// (+24EE10), both after the object's fields (matrix included) loaded. The game itself calls IControlObject::SetWorldPosition / SetWorldXform
// in 10 OnCreate functions of that same pass (StaticCarFlammable, CarSpawnPoint, ...: INV-697 (12)).
// WHAT: our slot runs the game's OnPostCreate first (its skin swap), then reads the object's world translation (IControlObject::GetWorldXform,
// the IControlObject view = this - 0x10), finds the row at that translation (within 1 mm), and moves it with the SAME rule as decalseat.cpp:
// new = pos - n * (gap - target), only when target < gap <= target + max (decal_seat_gap_um / decal_seat_max_mm). Read back after the move.
// Per frame: NOTHING. Once per DecalPLS at creation; the worker sleeps 1 s between summary checks (not on any game path).
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "camhook.h"
#include "rtti_vtable.h"  // RttiFindVtable
#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

namespace dlvr {
namespace decalpls {

constexpr char kClass[] = ".?AVDecalPLS@@";
constexpr uint32_t kViewOff = 40;     // the IGSObject view: RTTI locator offset 40 (dpl1_win_out.txt, vtable +1665958)
constexpr int kSlot = 117;            // DecalPLS::OnPostCreate in that vtable (dpl6 / dpl10: +A836A0, held by no other qword in the gamedll)
constexpr ptrdiff_t kIcoFromView = -0x10;   // the IControlObject view: +A836A0 calls its base through [rcx-10h] (dpl10 disassembly)
// +A836A0's first 23 bytes (whole instructions, none relative): push rbx / sub rsp,20h / mov rax,[rcx-10h] / mov rbx,rcx / add rcx,-10h /
// call [rax+1B8h]; and at +43 'mov rcx,[rax+0B40h]' (the level's PLSDecalsManager). Any other code in the slot = another build: REFUSED.
const unsigned char kHead[23] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x41, 0xF0, 0x48, 0x8B, 0xD9, 0x48, 0x83, 0xC1,
                                 0xF0, 0xFF, 0x90, 0xB8, 0x01, 0x00, 0x00};
const unsigned char kLvl[7] = {0x48, 0x8B, 0x88, 0x40, 0x0B, 0x00, 0x00};
constexpr size_t kLvlAt = 40;

typedef void(__fastcall* PostCreateFn)(char* view);
typedef void*(__fastcall* GetLevelFn)(const void* gsView);
typedef const char*(__fastcall* LevelNameFn)(void* level);
typedef const float*(__fastcall* GetWorldXformFn)(const void* ico);
typedef void(__fastcall* SetWorldPositionFn)(void* ico, const float* v);

PostCreateFn g_true = nullptr;
GetLevelFn g_getLevel = nullptr;
LevelNameFn g_levelName = nullptr;
GetWorldXformFn g_getWorld = nullptr;
SetWorldPositionFn g_setPos = nullptr;
volatile LONG g_started = 0;

#pragma pack(push, 1)
struct Header {
    char magic[4];
    unsigned version, entries, expBytes;
    char map[48];
};
struct Row {
    float pos[3];
    float n[3];
    float gap;
    unsigned expOff;
};
#pragma pack(pop)
static_assert(sizeof(Header) == 64, "decal_pls header is 64 bytes");
static_assert(sizeof(Row) == 32, "decal_pls row is 32 bytes");

SRWLOCK g_lock = SRWLOCK_INIT;
void* g_level = nullptr;     // the level the table below belongs to
char g_lvlName[64] = {};
char g_file[64] = {};        // the file in use, "" = none is this level's
Row* g_rows = nullptr;
unsigned g_nRows = 0;
// counters for the current level (under g_lock)
unsigned g_seen = 0, g_noRow = 0, g_ambig = 0, g_moved = 0, g_atOrBehind = 0, g_tooFar = 0, g_readBad = 0, g_nullWorld = 0;
float g_sumMove = 0.0f, g_maxMove = 0.0f;
volatile LONG g_events = 0;  // any change, for the worker's summary
unsigned g_logMoved = 0, g_logNoRow = 0;

bool LevelMatches(const char* level, const char* map)
{
    if (!level || !map || !map[0]) {
        return false;
    }
    const size_t ll = strlen(level), ml = strlen(map);
    if (ll < ml || _stricmp(level + ll - ml, map) != 0) {
        return false;
    }
    return ll == ml || level[ll - ml - 1] == '/' || level[ll - ml - 1] == '\\';
}

// Loads this level's file, if one is; under g_lock (exclusive). Names the outcome once per level.
void LoadFor(void* level)
{
    free(g_rows);
    g_rows = nullptr;
    g_nRows = 0;
    g_file[0] = '\0';
    g_level = level;
    const char* name = (level && g_levelName) ? g_levelName(level) : nullptr;
    strncpy_s(g_lvlName, name ? name : "(null)", _TRUNCATE);
    g_seen = g_noRow = g_ambig = g_moved = g_atOrBehind = g_tooFar = g_readBad = g_nullWorld = 0;
    g_sumMove = g_maxMove = 0.0f;
    g_logMoved = g_logNoRow = 0;
    char dir[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    if (char* s = strrchr(dir, '\\')) {
        s[1] = '\0';
    }
    char pattern[MAX_PATH] = {};
    sprintf_s(pattern, "%sdlvr\\decal_pls_*.bin", dir);
    WIN32_FIND_DATAA fd = {};
    HANDLE fh = FindFirstFileA(pattern, &fd);
    int files = 0;
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            ++files;
            char path[MAX_PATH] = {};
            sprintf_s(path, "%sdlvr\\%s", dir, fd.cFileName);
            FILE* f = nullptr;
            if (fopen_s(&f, path, "rb") != 0 || !f) {
                // [PROBE: DECALPLS] gate=decal_seat  site=FLAT (level load; the create pass is the same flat and VR)  log-only=no
                LOG("[DECALPLS] cannot open %s; skipped", path);
                continue;
            }
            Header h = {};
            const bool gotHeader = fread(&h, 1, sizeof(h), f) == sizeof(h);
            h.map[47] = '\0';
            if (!gotHeader || memcmp(h.magic, "DPL1", 4) != 0 || h.version != 1) {
                fclose(f);
                // [PROBE: DECALPLS] gate=decal_seat  site=FLAT  log-only=no
                LOG("[DECALPLS] REFUSED - %s: bad header (read %d, version %u); skipped", fd.cFileName, gotHeader ? 1 : 0, h.version);
                continue;
            }
            if (!LevelMatches(name, h.map)) {
                fclose(f);
                continue;   // another map's file: the outcome is named once below
            }
            Row* r = static_cast<Row*>(malloc(sizeof(Row) * (h.entries ? h.entries : 1)));
            const bool got = r && fread(r, sizeof(Row), h.entries, f) == h.entries && fgetc(f) == EOF;
            fclose(f);
            if (!got) {
                free(r);
                // [PROBE: DECALPLS] gate=decal_seat  site=FLAT  log-only=no
                LOG("[DECALPLS] REFUSED - %s (map '%s'): %u rows do not fit the file; this level's entity decals stay stock", fd.cFileName, h.map, h.entries);
                break;
            }
            g_rows = r;
            g_nRows = h.entries;
            strncpy_s(g_file, fd.cFileName, _TRUNCATE);
            // [PROBE: DECALPLS] gate=decal_seat  site=FLAT  log-only=no
            LOG("[DECALPLS] level '%s': %s loaded, %u rows (map '%s'); gap %d um, max %d mm", g_lvlName, g_file, g_nRows, h.map, DecalSeatGapUm, DecalSeatMaxMm);
            break;
        } while (FindNextFileA(fh, &fd));
        FindClose(fh);
    }
    if (!g_rows) {
        // [PROBE: DECALPLS] gate=decal_seat  site=FLAT  log-only=no
        LOG("[DECALPLS] level '%s': no dlvr\\decal_pls_*.bin is this level's (%d file(s) read); its entity decals stay stock", g_lvlName, files);
    }
}

void __fastcall Hook(char* view)
{
    g_true(view);   // the game's OnPostCreate first: base create + the level's skin swap
    if (!DecalSeat) {
        return;
    }
    void* level = g_getLevel(view);
    void* ico = view + kIcoFromView;
    const float* m = g_getWorld(ico);
    float p[3] = {}, np[3] = {}, mv = 0.0f;
    unsigned expOff = 0;
    bool move = false;
    // Decide under the lock (table + counters); the engine calls (move, read back) are made OUTSIDE it.
    AcquireSRWLockExclusive(&g_lock);
    if (level != g_level) {
        LoadFor(level);
    }
    ++g_seen;
    if (!m) {
        ++g_nullWorld;
    } else if (g_rows) {
        p[0] = m[3];
        p[1] = m[7];
        p[2] = m[11];
        int hit = -1, hits = 0;
        for (unsigned i = 0; i < g_nRows; ++i) {
            const Row& r = g_rows[i];
            if (fabsf(r.pos[0] - p[0]) <= 1e-3f && fabsf(r.pos[1] - p[1]) <= 1e-3f && fabsf(r.pos[2] - p[2]) <= 1e-3f) {
                hit = static_cast<int>(i);
                ++hits;
            }
        }
        if (hits == 0) {
            ++g_noRow;
            if (g_logNoRow++ < 3) {
                // [PROBE: DECALPLS] gate=decal_seat  site=FLAT  log-only=no
                LOG("[DECALPLS] no row at (%.4f %.4f %.4f) - a DecalPLS without a confident surface, or parented (world != its .exp matrix); left stock", p[0], p[1], p[2]);
            }
        } else if (hits > 1) {
            ++g_ambig;
        } else {
            const Row& r = g_rows[hit];
            const float targetM = DecalSeatGapUm * 1e-6f, maxM = DecalSeatMaxMm * 1e-3f;
            if (!(r.gap > targetM)) {
                ++g_atOrBehind;
            } else if (r.gap > targetM + maxM) {
                ++g_tooFar;
            } else {
                mv = r.gap - targetM;
                for (int k = 0; k < 3; ++k) {
                    np[k] = p[k] - r.n[k] * mv;
                }
                expOff = r.expOff;
                move = true;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (move) {
        g_setPos(ico, np);
        const float* m2 = g_getWorld(ico);
        const bool ok = m2 && fabsf(m2[3] - np[0]) <= 1e-4f && fabsf(m2[7] - np[1]) <= 1e-4f && fabsf(m2[11] - np[2]) <= 1e-4f;
        AcquireSRWLockExclusive(&g_lock);
        if (!ok) {
            ++g_readBad;
        }
        ++g_moved;
        g_sumMove += mv;
        if (mv > g_maxMove) {
            g_maxMove = mv;
        }
        const bool logIt = g_logMoved++ < 3;
        ReleaseSRWLockExclusive(&g_lock);
        if (logIt) {
            // [PROBE: DECALPLS] gate=decal_seat  site=FLAT  log-only=no
            LOG("[DECALPLS] moved .exp @%u by %.1f mm: (%.4f %.4f %.4f) -> (%.4f %.4f %.4f), read back %s (%.4f %.4f %.4f)", expOff, 1000.0f * mv,
                p[0], p[1], p[2], np[0], np[1], np[2], ok ? "OK" : "MISMATCH", m2 ? m2[3] : 0.0f, m2 ? m2[7] : 0.0f, m2 ? m2[11] : 0.0f);
        }
    }
    InterlockedIncrement(&g_events);
}

void Summary()
{
    AcquireSRWLockShared(&g_lock);
    // [PROBE: DECALPLS] gate=decal_seat  site=FLAT  log-only=no
    LOG("[DECALPLS] SUMMARY level '%s' (%s): %u DecalPLS created; %u moved onto their surface (mean %.1f mm, max %.1f mm, read-back mismatches %u); left: %u at "
        "or behind %.1f mm, %u further than %d mm, %u with no row, %u ambiguous, %u without a world matrix. Rows in file %u",
        g_lvlName, g_file[0] ? g_file : "no file", g_seen, g_moved, g_moved ? 1000.0f * g_sumMove / g_moved : 0.0f, 1000.0f * g_maxMove, g_readBad,
        g_atOrBehind, DecalSeatGapUm * 1e-3f, g_tooFar, DecalSeatMaxMm, g_noRow, g_ambig, g_nullWorld, g_nRows);
    ReleaseSRWLockShared(&g_lock);
}

DWORD WINAPI Thread(LPVOID)
{
    // [PROBE: DECALPLS] gate=decal_seat (this line prints every launch, ON or off)  site=FLAT  log-only=no
    LOG("[DECALPLS] worker up (logged unconditionally) - decal_seat=%d; the DecalPLS OnPostCreate slot is swapped when it is first on", DecalSeat ? 1 : 0);
    while (!DecalSeat) {
        Sleep(500);
    }
    HMODULE engine = GetModuleHandleA("engine_x64_rwdi.dll");
    HMODULE game = nullptr;
    for (int i = 0; i < 1200 && !(game = GetModuleHandleA("gamedll_x64_rwdi.dll")); ++i) {
        Sleep(100);
    }
    if (!engine || !game) {
        // [PROBE: DECALPLS] gate=decal_seat  site=FLAT  log-only=no
        LOG("[DECALPLS] REFUSED - %s not mapped after 120 s; entity decals stay stock", !engine ? "engine_x64_rwdi.dll" : "gamedll_x64_rwdi.dll");
        return 0;
    }
    g_getLevel = reinterpret_cast<GetLevelFn>(GetProcAddress(engine, "?GetLevel@IGSObject@@QEBAPEAVILevel@@XZ"));
    g_levelName = reinterpret_cast<LevelNameFn>(GetProcAddress(engine, "?GetLevelName@ILevel@@QEAAPEBDXZ"));
    g_getWorld = reinterpret_cast<GetWorldXformFn>(GetProcAddress(engine, "?GetWorldXform@IControlObject@@QEBAAEBVmtx34@@XZ"));
    g_setPos = reinterpret_cast<SetWorldPositionFn>(GetProcAddress(engine, "?SetWorldPosition@IControlObject@@QEAAXAEBVvec3@@@Z"));
    if (!g_getLevel || !g_levelName || !g_getWorld || !g_setPos) {
        // [PROBE: DECALPLS] gate=decal_seat  site=FLAT  log-only=no
        LOG("[DECALPLS] REFUSED - engine export missing: GetLevel %p, GetLevelName %p, GetWorldXform %p, SetWorldPosition %p; entity decals stay stock",
            reinterpret_cast<void*>(g_getLevel), reinterpret_cast<void*>(g_levelName), reinterpret_cast<void*>(g_getWorld), reinterpret_cast<void*>(g_setPos));
        return 0;
    }
    const uintptr_t gd = reinterpret_cast<uintptr_t>(game);
    int stage = 0;
    const uintptr_t vt = RttiFindVtable(gd, kClass, kViewOff, &stage);
    if (!vt) {
        // [PROBE: DECALPLS] gate=decal_seat  site=FLAT  log-only=no
        LOG("[DECALPLS] REFUSED - DecalPLS vtable (locator offset %u) not found by name (stage %d: 0 no name, 1 no locator, 2 no vtable, -1 faulted); entity decals stay stock",
            kViewOff, stage);
        return 0;
    }
    const uintptr_t slot = vt + 8 * kSlot;
    const uintptr_t have = *reinterpret_cast<const uintptr_t*>(slot);
    const unsigned char* code = reinterpret_cast<const unsigned char*>(have);
    if (have < gd || memcmp(code, kHead, sizeof(kHead)) != 0 || memcmp(code + kLvlAt, kLvl, sizeof(kLvl)) != 0) {
        // [PROBE: DECALPLS] gate=decal_seat  site=FLAT  log-only=no
        LOG("[DECALPLS] REFUSED - vtable +%zX slot %d holds +%zX, whose code is not DecalPLS::OnPostCreate as read on gamedll 97046bba (another build?); entity decals stay stock",
            vt - gd, kSlot, have - gd);
        return 0;
    }
    g_true = reinterpret_cast<PostCreateFn>(have);
    DWORD old = 0;
    // [LOADBEARING: DecalPLS seat hook] (decal_seat) DecalPLS IGSObject-view vtable (RTTI locator 40, OURS +1665958) slot 117 -> Hook
    //      (+A836A0). Without it the map's entity graffiti + murals (slums 259 by the shipped rule) stay floating off their walls (INV-697 (7)).
    if (!VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), PAGE_READWRITE, &old)) {
        // [PROBE: DECALPLS] gate=decal_seat  site=FLAT  log-only=no
        LOG("[DECALPLS] REFUSED - VirtualProtect on the slot failed (error %lu); entity decals stay stock", GetLastError());
        return 0;
    }
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot), reinterpret_cast<PVOID>(&Hook));
    DWORD unused = 0;
    VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), old, &unused);
    const bool live = *reinterpret_cast<const uintptr_t*>(slot) == reinterpret_cast<uintptr_t>(&Hook);
    // [PROBE: DECALPLS] gate=decal_seat  site=FLAT  log-only=no
    LOG("[DECALPLS] %s - DecalPLS vtable +%zX slot %d (was +%zX, read back %s). A level already loaded keeps its entity decals: load a save after this",
        live ? "LIVE" : "NOT INSTALLED", vt - gd, kSlot, have - gd, live ? "ours" : "NOT ours");
    if (!live) {
        return 0;
    }
    // Summary: once the create calls stop for 3 s (a level's create pass is one burst), name the totals once.
    LONG seenAt = 0;
    int quiet = 0;
    for (;;) {
        Sleep(1000);
        const LONG ev = g_events;
        if (ev != seenAt) {
            seenAt = ev;
            quiet = 0;
        } else if (++quiet == 3 && ev != 0) {
            Summary();
        }
    }
}

}  // namespace decalpls

void StartDecalPlsOnce()
{
    if (InterlockedCompareExchange(&decalpls::g_started, 1, 0) == 0) {
        CreateThread(nullptr, 0, decalpls::Thread, nullptr, 0, nullptr);
    }
}

}  // namespace dlvr
