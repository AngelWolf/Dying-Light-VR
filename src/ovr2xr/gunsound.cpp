// gunsound.cpp - the game's reload sounds at the player's own steps, each STARTED AT AN OFFSET so its click lands at the step.
// Contract: gunsound.h. Static reads (OUR engine e9fbfc2c + gamedll 97046bba): docs/RESEARCH-SOLID-GUNS-2026-11-02.md steps 1 + 6.
//
// THE CALL - our replica of the engine export IGSObject::PlaySound2D (+253EA0, read in full, step 1), with ONE addition. That export
// returns only the sound's id, never the handle, and the handle is where a start position lives (step 6), so we make the same calls:
//   mixer slot +0x200 NewSound(&out, name, 0, bus, 0, loop, &params)  - RETURNS HOLDING the engine's sound lock (a CRITICAL_SECTION)
//   handle (out+16): slot +24 (owner), +40 (owner), IGame +1256(category) ? slot +392(category), +184, +168,
//   >>> OURS: the start position through the handle's OWN SetPosition, slot 28 (`h+72 = (int)(s * 1000)`, ms) - the update applies h+72
//       with Channel::setPosition right after playSound(paused) and before setPaused(0), so the seek lands before any sample is heard.
//       2026-11-10 (INV-714): the old write into h+620 (slot 29's field) NEVER seeked - h+620 is the CURRENT POSITION, overwritten by
//       getPosition before the start step reads it; a 2.7 s write silenced the chainsaw sound (RUNS n3n4 / vol20 / CF1). Jay: "Fix for
//       all guns" <<<
//   slot +64 start(loop = false, 0, 0), slot +16 id;  then LeaveCriticalSection(the lock) - ALWAYS, sound or not (as the export does).
//   (slot 28 is called BEFORE slot +64 start, under the same lock, only when its bytes are the h+72 setter - see CheckSeekSlot.)
// The engine's mixer / FMOD do the rest exactly as for the game's own sounds (volume, bus, mixing).
//
// SAFETY (calibsound.cpp's rule): the engine calls are NOT inside __try - NewSound returns holding the lock, and a fault caught before
// the release would hang every later game sound. Everything we rely on is READ AND CHECKED BEFORE the lock is taken: PlayerDI+40 ->
// +32 -> +72 (the owner), the mixer global + its vtable (= CAudioFMOD, not the NOS mixer), its NewSound slot, the IGame global + its
// slot. Slot 28's own bytes are read (guarded reads only) and it is called only when they are the h+72 setter.
// BUILDS: IGSObject::PlaySound2D by NAME (the lock address is decoded from ITS code: the `lea rcx,[rip+d32]` before its final
// `call [LeaveCriticalSection]`, the import checked against kernel32's own address); the mixer / vtables / IGame via buildrva (existing
// entries: calibsound's mixer + CAudioFMOD + NOS, camhook's IGame); the seek slot checked from slot 28's bytes. No new rva.
#include "gunsound.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../common/log.h"

#if __has_include("../common/buildrva.h")
#include "../common/buildrva.h"
#define DLVR_GS_EN_RVA(rva) ::dlvr::buildrva::Eng(rva)
#else
#define DLVR_GS_EN_RVA(rva) (rva)
#endif

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

namespace dlvr {
namespace {

const char kExport[] = "?PlaySound2D@IGSObject@@QEAAHAEBV?$string_base@D@ttl@@_NMMMIMI@Z";
constexpr size_t    kExportScan = 0x1C0;      // the export is 445 bytes (sg3); its lock release sits near its end
constexpr uintptr_t kSteamCsRva = 0xA41158;   // the lock on Steam e9fbfc2c (sg3: LeaveCriticalSection(&stru_180A41158)) - LOGGED only
constexpr uintptr_t kMixerRva  = 0xA3EE58;    // the global sound system (RESEARCH-CALIB-AUDIO ROUTE A 2)
constexpr uintptr_t kFmodVtRva = 0x82A6F0;    // audio::fmod::CAudioFMOD
constexpr uintptr_t kNosVtRva  = 0x82C4B0;    // audio::CSoundMixerNOS (no sound)
constexpr uintptr_t kGameRva   = 0xA3F3B8;    // the IGame* global (camhook / uicam / uipin, buildrva family entry)
constexpr size_t    kNewSoundSlot = 0x200;    // mixer vtable: NewSound
constexpr size_t    kGameCatSlot  = 1256;     // IGame vtable: does it accept this category
constexpr size_t    kGsoOff    = 40;          // PlayerDI + 40 = IGSObject (handcoll.cpp kGsoOff, INV-659 addendum)
constexpr size_t    kOwnerA    = 32;          // IGSObject::PlaySound2D reads *(this+32) ...
constexpr size_t    kOwnerB    = 72;          // ... +72 (0 -> it returns 0)
constexpr unsigned  kBus       = 4;           // EGameVolumeSource::SoundPlayer (gamedll CEnumInfo::Add x5 - research step 1)
constexpr unsigned  kCategory  = 2;           // a9: the bit the reload scripts' 4th argument sets (+104 bit 0x2); research [UNKNOWN] (a)
constexpr long      kMaxLines  = 200;

using FnNewSound = void(__fastcall*)(uintptr_t mixer, void* out, const char* name, uint64_t a4, int bus, int a6, int loop, void* params);
using FnH1 = void(__fastcall*)(uintptr_t h, uintptr_t owner);
using FnH0 = void(__fastcall*)(uintptr_t h);
using FnHCat = void(__fastcall*)(uintptr_t h, unsigned cat);
using FnStart = void(__fastcall*)(uintptr_t h, int loop, float a3, float a4);
using FnId = int(__fastcall*)(uintptr_t h);
using FnSetPos = void(__fastcall*)(uintptr_t h, float seconds);   // slot 28: h+72 = (int)(seconds * 1000)
using FnGameCat = bool(__fastcall*)(uintptr_t game, unsigned cat, unsigned zero);

int       g_state = 0;   // 0 unchecked, 1 ready, -1 refused
uintptr_t g_eng = 0;
CRITICAL_SECTION* g_lock = nullptr;
long      g_lines = 0;
// The handle's seek slot 28 (SetPosition), checked from its bytes: -1 = not read yet, -2 = slot 28 is not the h+disp setter (no seek -
// sounds play from the start), >= 0 = the field it writes (h+72 on Steam e9fbfc2c, INV-714).
long      g_seekOff = -1;
uintptr_t g_seekVt = 0;    // the handle vtable slot 28 was checked on
uintptr_t g_seekFn = 0;    // slot 28's address once checked

struct Pending { char name[64]; float vol, startS; double due; bool on; };
Pending g_queue[4] = {};

double NowS()
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

bool ReadBytes(uintptr_t at, void* out, size_t n)
{
    __try { memcpy(out, reinterpret_cast<const void*>(at), n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void CheckOnce()
{
    g_state = -1;
    const HMODULE en = GetModuleHandleA("engine_x64_rwdi.dll");
    g_eng = reinterpret_cast<uintptr_t>(en);
    if (!en) {
        // [PROBE: GUNSND] gate=manual_reload  site=FLAT  log-only=yes
        LOG("gunsound: [GUNSND] REFUSED: engine_x64_rwdi.dll is not loaded - reload sounds are silent this session.");
        return;
    }
    const uintptr_t exp = reinterpret_cast<uintptr_t>(GetProcAddress(en, kExport));
    const uintptr_t leave = reinterpret_cast<uintptr_t>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "LeaveCriticalSection"));
    unsigned char code[kExportScan] = {};
    if (!exp || !leave || !ReadBytes(exp, code, sizeof(code))) {
        // [PROBE: GUNSND] gate=manual_reload  site=FLAT  log-only=yes
        LOG("gunsound: [GUNSND] REFUSED: %s - reload sounds are silent this session.", !exp ? "the engine does not export IGSObject::PlaySound2D"
            : !leave ? "kernel32 LeaveCriticalSection not found" : "the export's code is unreadable");
        return;
    }
    // The LAST `48 8D 0D d32` (lea rcx,[rip+d32]) followed within 12 bytes by `FF 15 d32` (call [rip+d32]) whose IAT slot holds
    // kernel32's LeaveCriticalSection = the lock the export releases.
    uintptr_t lock = 0;
    int pairs = 0;
    for (size_t i = 0; i + 7 <= sizeof(code); ++i) {
        if (code[i] != 0x48 || code[i + 1] != 0x8D || code[i + 2] != 0x0D) { continue; }
        int32_t dl = 0;
        memcpy(&dl, code + i + 3, 4);
        const uintptr_t target = exp + i + 7 + static_cast<intptr_t>(dl);
        for (size_t j = i + 7; j + 6 <= sizeof(code) && j <= i + 7 + 12; ++j) {
            if (code[j] != 0xFF || code[j + 1] != 0x15) { continue; }
            int32_t dc = 0;
            memcpy(&dc, code + j + 2, 4);
            uintptr_t iat = 0;
            if (ReadBytes(exp + j + 6 + static_cast<intptr_t>(dc), &iat, sizeof(iat)) && iat == leave) {
                lock = target;
                ++pairs;
            }
            break;
        }
    }
    if (!lock) {
        // [PROBE: GUNSND] gate=manual_reload  site=FLAT  log-only=yes
        LOG("gunsound: [GUNSND] REFUSED: no `lea rcx` + `call LeaveCriticalSection` pair in the first %u bytes of IGSObject::PlaySound2D "
            "(engine+%llX) - the sound lock is unknown; reload sounds are silent this session.", static_cast<unsigned>(kExportScan),
            static_cast<unsigned long long>(exp - g_eng));
        return;
    }
    g_lock = reinterpret_cast<CRITICAL_SECTION*>(lock);
    g_state = 1;
    // [PROBE: GUNSND] gate=manual_reload  site=FLAT  log-only=yes
    LOG("gunsound: [GUNSND] READY: our replica of IGSObject::PlaySound2D (engine+%llX) - the sound lock decoded from its code at "
        "engine+%llX (%s; %d lea/LeaveCriticalSection pair(s), the last used); bus %u (SoundPlayer), category %u, on PlayerDI+%u; "
        "each sound started at its offset (the handle field slot 29 reads, decoded at the first play).",
        static_cast<unsigned long long>(exp - g_eng), static_cast<unsigned long long>(lock - g_eng),
        lock - g_eng == kSteamCsRva ? "= Steam e9fbfc2c's" : "differs from Steam e9fbfc2c's +A41158 - another build", pairs, kBus, kCategory,
        static_cast<unsigned>(kGsoOff));
}

struct PrePlay { uintptr_t gso, ownerA, ownerB, mixer, mixVt, newSound, game, gameVt, gameCat; };
// Every read the call depends on, BEFORE the lock. Plain C data only (C2712).
bool ReadPrePlay(uintptr_t playerDI, PrePlay& p)
{
    p = PrePlay{};
    __try {
        p.gso = playerDI + kGsoOff;
        p.ownerA = *reinterpret_cast<const uintptr_t*>(p.gso + kOwnerA);
        p.ownerB = p.ownerA ? *reinterpret_cast<const uintptr_t*>(p.ownerA + kOwnerB) : 0;
        p.mixer = *reinterpret_cast<const uintptr_t*>(g_eng + DLVR_GS_EN_RVA(kMixerRva));
        p.mixVt = p.mixer ? *reinterpret_cast<const uintptr_t*>(p.mixer) : 0;
        p.newSound = p.mixVt ? *reinterpret_cast<const uintptr_t*>(p.mixVt + kNewSoundSlot) : 0;
        p.game = *reinterpret_cast<const uintptr_t*>(g_eng + DLVR_GS_EN_RVA(kGameRva));
        p.gameVt = p.game ? *reinterpret_cast<const uintptr_t*>(p.game) : 0;
        p.gameCat = p.gameVt ? *reinterpret_cast<const uintptr_t*>(p.gameVt + kGameCatSlot) : 0;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Slot 28 of the handle vtable on Steam e9fbfc2c (engine +1118A0, read 2026-11-10): `F3 0F 59 0D d32` (mulss xmm1,[rip+d32] = 1000.0)
// `F3 48 0F 2C C1` (cvttss2si rax,xmm1) `89 41 48` (mov [rcx+48h],eax) `C3` = h+72 = (int)(s * 1000). Checked once per handle vtable;
// guarded reads only. The disp8 is taken from the bytes (logged), never assumed.
void CheckSeekSlot(uintptr_t vt)
{
    if (vt == g_seekVt && g_seekOff != -1) { return; }
    g_seekVt = vt;
    g_seekOff = -2;
    g_seekFn = 0;
    uintptr_t fn = 0;
    unsigned char b[17] = {};
    if (ReadBytes(vt + 28 * 8, &fn, sizeof(fn)) && fn && ReadBytes(fn, b, sizeof(b)) && b[0] == 0xF3 && b[1] == 0x0F && b[2] == 0x59 &&
        b[3] == 0x0D && b[8] == 0xF3 && b[9] == 0x48 && b[10] == 0x0F && b[11] == 0x2C && b[12] == 0xC1 && b[13] == 0x89 && b[14] == 0x41 &&
        b[16] == 0xC3) {
        g_seekOff = b[15];
        g_seekFn = fn;
    }
}

bool PlayNow(uintptr_t playerDI, const char* name, float vol, float startS)
{
    if (g_state == 0) { CheckOnce(); }
    if (g_state != 1 || !playerDI || !name || !name[0]) { return false; }
    PrePlay p;
    const char* why = nullptr;
    if (!ReadPrePlay(playerDI, p)) {
        why = "a read faulted (PlayerDI+40+32 / its +72 / the mixer global / its vtable / NewSound / the IGame global)";
    } else if (!p.ownerA || !p.ownerB) {
        why = "the player's sound owner (IGSObject+32 -> +72) is null - the engine would return 0";
    } else if (!p.mixer) {
        why = "no sound system (the mixer global is null)";
    } else if (p.mixVt - g_eng == DLVR_GS_EN_RVA(kNosVtRva)) {
        why = "no sound system (NOS mixer)";
    } else if (p.mixVt - g_eng != DLVR_GS_EN_RVA(kFmodVtRva)) {
        why = "the mixer's vtable is not audio::fmod::CAudioFMOD";
    } else if (!p.newSound) {
        why = "the mixer's NewSound slot is null";
    }
    if (why) {
        if (++g_lines <= kMaxLines) {
            // [PROBE: GUNSND] gate=manual_reload  site=HEADSET  log-only=yes
            LOG("gunsound: [GUNSND] \"%s\" NOT played: %s.", name, why);
        }
        return false;
    }
    // The params NewSound reads (the export's own stack block, +0 .. +0x43): volume, bus, the owner, then the export's constants.
    alignas(16) unsigned char params[0x48] = {};
    memcpy(params + 0x00, &vol, 4);
    const unsigned bus = kBus;
    memcpy(params + 0x04, &bus, 4);
    memcpy(params + 0x08, &p.ownerB, 8);
    const int c2 = 2, c3 = 3;
    const float one = 1.0f;
    memcpy(params + 0x2C, &c2, 4);
    memcpy(params + 0x30, &one, 4);
    memcpy(params + 0x34, &one, 4);
    memcpy(params + 0x38, &c3, 4);
    memcpy(params + 0x3C, &one, 4);
    memcpy(params + 0x40, &one, 4);
    alignas(16) unsigned char out[32] = {};
    // ---- the engine's sound lock is HELD from here until LeaveCriticalSection below (NOT in __try - see the file header) ----
    reinterpret_cast<FnNewSound>(p.newSound)(p.mixer, out, name, 0, static_cast<int>(kBus), 0, 0, params);
    uintptr_t h = 0;
    memcpy(&h, out + 16, sizeof(h));
    int id = 0;
    const char* seek = "no handle";
    if (h) {
        const uintptr_t* vt = *reinterpret_cast<const uintptr_t* const*>(h);
        reinterpret_cast<FnH1>(vt[24 / 8])(h, p.ownerB);
        reinterpret_cast<FnH1>(vt[40 / 8])(h, p.ownerB);
        if (p.gameCat && reinterpret_cast<FnGameCat>(p.gameCat)(p.game, kCategory, 0)) {
            reinterpret_cast<FnHCat>(vt[392 / 8])(h, kCategory);
        }
        reinterpret_cast<FnH0>(vt[184 / 8])(h);
        reinterpret_cast<FnH0>(vt[168 / 8])(h);
        CheckSeekSlot(reinterpret_cast<uintptr_t>(vt));
        if (!(startS > 0.0f)) {
            seek = "from the start";
        } else if (g_seekOff >= 0 && g_seekFn) {
            reinterpret_cast<FnSetPos>(g_seekFn)(h, startS);   // h+72 ms - applied after playSound(paused), before unpause (INV-714)
            seek = "seek via slot 28";
        } else {
            seek = "from the start - slot 28 is NOT the `h+disp = (int)(s * 1000)` setter on this build (no seek)";
        }
        reinterpret_cast<FnStart>(vt[64 / 8])(h, 0, 0.0f, 0.0f);
        id = reinterpret_cast<FnId>(vt[16 / 8])(h);
    }
    LeaveCriticalSection(g_lock);
    // ---- the lock is released ----
    if (++g_lines <= kMaxLines) {
        // [PROBE: GUNSND] gate=manual_reload  site=HEADSET  log-only=yes
        LOG("gunsound: [GUNSND] play \"%s\" vol %.2f start %.4f s (%s; field +%ld) -> id %d%s", name, vol, startS, seek, g_seekOff, id,
            id == 0 ? " *** id 0 - NOT created (the bank not loaded, or the name does not resolve) ***" : "");
    }
    return id != 0;
}

}  // namespace

bool GunSoundPlay(uintptr_t playerDI, const char* name, float vol, float delayS, float startS)
{
    if (!(delayS > 0.0f)) { return PlayNow(playerDI, name, vol, startS); }
    for (Pending& q : g_queue) {
        if (q.on) { continue; }
        strncpy_s(q.name, sizeof(q.name), name ? name : "", _TRUNCATE);
        q.vol = vol;
        q.startS = startS;
        q.due = NowS() + delayS;
        q.on = true;
        return true;
    }
    if (++g_lines <= kMaxLines) {
        // [PROBE: GUNSND] gate=manual_reload  site=HEADSET  log-only=yes
        LOG("gunsound: [GUNSND] \"%s\" NOT queued: the 4-slot delay queue is full.", name ? name : "(null)");
    }
    return false;
}

void GunSoundTick(uintptr_t playerDI)
{
    double now = -1.0;
    for (Pending& q : g_queue) {
        if (!q.on) { continue; }
        if (now < 0.0) { now = NowS(); }
        if (now < q.due) { continue; }
        q.on = false;
        PlayNow(playerDI, q.name, q.vol, q.startS);
    }
}

}  // namespace dlvr
