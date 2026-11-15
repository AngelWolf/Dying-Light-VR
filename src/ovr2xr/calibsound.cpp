// calibsound.cpp - the calibration screen's sounds through the engine's gui::IObject::PlaySound2D (route A). Contract,
// threading and cost: calibsound.h. Every RVA and byte string below was read from OUR engine_x64_rwdi.dll e9fbfc2c
// (md5 == deployed 2026-10-14); the static read is docs/RESEARCH-CALIB-AUDIO-2026-10-14.md section ROUTE A.
#include "calibsound.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../common/buildrva.h"   // the allowlisted EGL rvas for the checks below
#include "../common/log.h"
#include "uipin.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

namespace dlvr {

namespace {

// ---- step 1, the one-time checks (OUR engine e9fbfc2c)
const char kPlayExport[] = "?PlaySound2D@IObject@gui@@QEAAHAEBV?$string_base@D@ttl@@_NMMMIM@Z";
constexpr uintptr_t kPlayRva = 0x27C820;                  // gui::IObject::PlaySound2D (RESEARCH "How a menu sound is played")
const unsigned char kPlayExpect[16] = { 0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x73, 0x18, 0x49,
                                        0x89, 0x7B, 0x20, 0x55, 0x49, 0x8D, 0x6B, 0xC1 };
constexpr uintptr_t kGetterRva = 0x1635F0;                // CUIManager+8 vtable slot 0: mov rax, [rcx+80h]; ret (ROUTE A 3)
const unsigned char kGetterExpect[8] = { 0x48, 0x8B, 0x81, 0x80, 0x00, 0x00, 0x00, 0xC3 };

// ---- step 2, the per-play checks
constexpr uintptr_t kUiVt2Rva   = 0x890400;               // CUIManager's second vtable (at object +8), cs11
constexpr size_t    kUiVt2Off   = 8;
constexpr size_t    kUiGetField = 0x88;                   // what the getter returns: (UI system + 8) + 0x80
constexpr uintptr_t kMixerRva   = 0xA3EE58;               // the global sound system PlaySound2D tests first (ROUTE A 2)
constexpr uintptr_t kFmodVtRva  = 0x82A6F0;               // audio::fmod::CAudioFMOD
constexpr uintptr_t kNosVtRva   = 0x82C4B0;               // audio::CSoundMixerNOS (audio safe mode: no sound)

struct TtlString { const char* p; uint32_t size, cap; };  // ttl::string_base<char>: PlaySound2D reads only +0 (ROUTE A 1)
using FnPlay2D = int(__fastcall*)(void* self, const TtlString* name, bool loop, float vol, float f2, float f3,
                                   unsigned bus, float f4);

enum : int { kUnchecked = 0, kReady = 1, kRefused = 2 };
int       g_state = kUnchecked;                           // submit thread only
char      g_refusal[200] = {};
FnPlay2D  g_play = nullptr;
uintptr_t g_eng = 0;

alignas(16) uint8_t s_cobj[64];                           // stand-in gui::CObject: +40 = the UI system
alignas(16) uint8_t s_iobj[64];                           // stand-in gui::IObject: +24 = &s_cobj

// A guarded copy of `n` bytes (no C++ objects in a __try function - MSVC C2712).
bool ReadBytes(uintptr_t at, void* out, size_t n)
{
    __try {
        memcpy(out, reinterpret_cast<const void*>(at), n);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void Hex(const unsigned char* b, size_t n, char* out, size_t cap)
{
    size_t used = 0;
    out[0] = 0;
    for (size_t i = 0; i < n && used + 4 < cap; ++i) {
        used += static_cast<size_t>(snprintf(out + used, cap - used, i ? " %02X" : "%02X", b[i]));
    }
}

// Step 1. Runs once (the first play); the result is kept for the session. Logs the verdict once.
void CheckOnce()
{
    g_state = kRefused;
    const HMODULE mod = GetModuleHandleA("engine_x64_rwdi.dll");
    g_eng = reinterpret_cast<uintptr_t>(mod);
    if (!g_eng) {
        strncpy_s(g_refusal, sizeof(g_refusal), "engine module: engine_x64_rwdi.dll is not loaded", _TRUNCATE);
    } else {
        const uintptr_t exp = reinterpret_cast<uintptr_t>(GetProcAddress(mod, kPlayExport));
        unsigned char got[16] = {};
        char hex[64];
        if (!exp) {
            strncpy_s(g_refusal, sizeof(g_refusal), "PlaySound2D export: GetProcAddress returned null", _TRUNCATE);
        } else if (exp - g_eng != ::dlvr::buildrva::Eng(kPlayRva)) {
            snprintf(g_refusal, sizeof(g_refusal), "PlaySound2D export: at engine+%llX, not engine+27C820",
                     static_cast<unsigned long long>(exp - g_eng));
        } else if (!ReadBytes(exp, got, sizeof(kPlayExpect))) {
            strncpy_s(g_refusal, sizeof(g_refusal), "PlaySound2D prologue: engine+27C820 not readable", _TRUNCATE);
        } else if (memcmp(got, kPlayExpect, sizeof(kPlayExpect)) != 0) {
            Hex(got, sizeof(kPlayExpect), hex, sizeof(hex));
            snprintf(g_refusal, sizeof(g_refusal), "PlaySound2D prologue: engine+27C820 reads %s (not engine e9fbfc2c)", hex);
        } else if (!ReadBytes(g_eng + ::dlvr::buildrva::Eng(kGetterRva), got, sizeof(kGetterExpect))) {
            strncpy_s(g_refusal, sizeof(g_refusal), "UI manager getter: engine+1635F0 not readable", _TRUNCATE);
        } else if (memcmp(got, kGetterExpect, sizeof(kGetterExpect)) != 0) {
            Hex(got, sizeof(kGetterExpect), hex, sizeof(hex));
            snprintf(g_refusal, sizeof(g_refusal), "UI manager getter: engine+1635F0 reads %s (not engine e9fbfc2c)", hex);
        } else {
            g_play = reinterpret_cast<FnPlay2D>(exp);
            g_state = kReady;
        }
    }
    if (g_state == kReady) {
        // [PROBE: CALIBSND] gate=none (the calibration screen)  site=HEADSET  log-only=yes
        LOG("[CALIBSND] checks passed (once per launch): PlaySound2D export = engine+27C820, its first 16 bytes and the UI "
            "manager getter engine+1635F0 match engine e9fbfc2c - the game's menu sounds are played this session");
    } else {
        // [PROBE: CALIBSND] gate=none (the calibration screen)  site=HEADSET  log-only=yes
        LOG("[CALIBSND] REFUSED: %s - silent this session", g_refusal);
    }
}

struct PrePlay {
    uintptr_t vt2, slot0, field, mixer, mixVt;
};

// Step 2's reads, every one BEFORE the call. Plain C data only (C2712). False = a read faulted (p says how far it got).
bool ReadPrePlay(uintptr_t ui, PrePlay& p)
{
    p = PrePlay{};
    __try {
        p.vt2 = *reinterpret_cast<const uintptr_t*>(ui + kUiVt2Off);
        p.slot0 = p.vt2 ? *reinterpret_cast<const uintptr_t*>(p.vt2) : 0;
        p.field = *reinterpret_cast<const uintptr_t*>(ui + kUiGetField);    // what the getter will read
        p.mixer = *reinterpret_cast<const uintptr_t*>(g_eng + ::dlvr::buildrva::Eng(kMixerRva));
        p.mixVt = p.mixer ? *reinterpret_cast<const uintptr_t*>(p.mixer) : 0;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

}  // namespace

void CalibSoundPlay(CalibSnd s)
{
    const char* const name = s == CalibSnd::Click ? "click" : "menu_timer_loop";
    if (g_state == kUnchecked) { CheckOnce(); }
    if (g_state != kReady) {
        // [PROBE: CALIBSND] gate=none (the calibration screen)  site=HEADSET  log-only=yes
        LOG("[CALIBSND] \"%s\" not played (refused: %s)", name, g_refusal);
        return;
    }
    // Re-read every play - never cached across a level change.
    const char* cls = "none";
    const uintptr_t ui = uipin::UiSystemForSound(&cls);
    if (!ui) {
        // [PROBE: CALIBSND] gate=none (the calibration screen)  site=HEADSET  log-only=yes
        LOG("[CALIBSND] \"%s\" no level UI system - not played (level walk: %s)", name, cls);
        return;
    }
    PrePlay p;
    const char* why = nullptr;
    if (!ReadPrePlay(ui, p)) {
        why = "a read faulted (UI system +8 / its vtable / +88 / the mixer engine+A3EE58 / its vtable)";
    } else if (p.vt2 - g_eng != ::dlvr::buildrva::Eng(kUiVt2Rva)) {
        why = "UI system +8 vtable is not engine+890400 (not a CUIManager)";
    } else if (p.slot0 - g_eng != ::dlvr::buildrva::Eng(kGetterRva)) {
        why = "UI system +8 vtable slot 0 is not engine+1635F0";
    } else if (!p.mixer) {
        why = "no sound system (engine+A3EE58 is null)";
    } else if (p.mixVt - g_eng == ::dlvr::buildrva::Eng(kNosVtRva)) {
        why = "no sound system (NOS mixer)";
    } else if (p.mixVt - g_eng != ::dlvr::buildrva::Eng(kFmodVtRva)) {
        why = "the mixer's vtable is not engine+82A6F0 (audio::fmod::CAudioFMOD)";
    }
    if (why) {
        // [PROBE: CALIBSND] gate=none (the calibration screen)  site=HEADSET  log-only=yes
        LOG("[CALIBSND] \"%s\" %s - not played (ui system %p, level %s, +8 vtable engine+%llX, slot 0 engine+%llX, mixer %p "
            "vtable engine+%llX)",
            name, why, reinterpret_cast<void*>(ui), cls, static_cast<unsigned long long>(p.vt2 - g_eng),
            static_cast<unsigned long long>(p.slot0 - g_eng), reinterpret_cast<void*>(p.mixer),
            static_cast<unsigned long long>(p.mixVt - g_eng));
        return;
    }
    memset(s_cobj, 0, sizeof(s_cobj));
    memset(s_iobj, 0, sizeof(s_iobj));
    memcpy(s_cobj + 40, &ui, 8);
    const uintptr_t c = reinterpret_cast<uintptr_t>(s_cobj);
    memcpy(s_iobj + 24, &c, 8);
    const uint32_t len = static_cast<uint32_t>(strlen(name));
    const TtlString nm{ name, len, len };
    // The engine call is deliberately NOT inside __try: the mixer's NewSound (CAudioFMOD +0x200) takes the engine's
    // CSoundMutex and returns HOLDING it, and PlaySound2D's last act is the LeaveCriticalSection (ROUTE A 2). A fault caught
    // in between would leave the engine's sound lock held forever and hang every later sound in the game. Everything the
    // call reads on our side (the stand-ins, the UI system's +8 vtable / slot 0 / +88, the mixer and its vtable) is
    // checked above instead; the rest is the engine's own state.
    const int id = g_play(s_iobj, &nm, false, 1.0f, 0.0f, 0.0f, 8u, 1.0f);   // the game's own highlight call's values
    // [PROBE: CALIBSND] gate=none (the calibration screen)  site=HEADSET  log-only=yes
    LOG("[CALIBSND] play \"%s\" -> id %d (ui system %p, level %s)%s", name, id, reinterpret_cast<void*>(ui), cls,
        id == 0 ? " *** id 0 - NOT created ***" : "");
}

}  // namespace dlvr
