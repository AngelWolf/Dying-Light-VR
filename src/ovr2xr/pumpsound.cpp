// pumpsound.cpp - the gamedll detour that drops the game's per-shot rack sound while a manual pump shotgun is held.
// Plan docs/superpowers/plans/2026-11-04-manual-reload-pump-shotgun.md Task 3.
//
// WHAT IS HOOKED (OUR gamedll 97046bba, plan Task 0): AnimActionPlaySound2DExternal's play slot 110 = gamedll +110D890,
//   void __fastcall(__int64* this, IModelObject*, a3, a4, int a5, DWORD* a6)  - six parameters, a5 / a6 on the stack.
//   m_SoundName = this + 40, a ttl::string_base<char> whose +0 is a char*. The first 16 bytes are
//   48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 55 (mov rax,rsp / mov [rax+10h],rbx / mov [rax+18h],rsi / mov [rax+20h],rdi / push rbp):
//   instruction boundaries at 3 7 11 15 16, no rip-relative operand, no branch target inside.
//
// R6 (ledger): the first stolen instruction reads rsp. InstallSingleGameDetour patches the entry with WriteAbsoluteJmp (a JMP, never a
//   CALL) to this replacement, so the replacement is entered exactly as the game function was, rsp untouched. The trampoline it
//   returns holds the 16 stolen bytes then an absolute jmp back to +110D8A0; our replacement CALLS the trampoline with the same six
//   parameters, so the game's body sees an ordinary call frame (return address on top, a5 / a6 where the callee expects them).
//
// R4 (ledger): where this build's address is not known, nothing is patched and ONE line names the build; the pump keeps the game's
//   sound. +110D890 is NOT on tools/egl_allowlist.tsv, so the Epic gamedll is refused by name before any byte is read (the crossbow
//   bolt-visibility detour's rule); the GOG build is judged by the head compare alone.
// R5 (ledger): the raw text of m_SoundName (case, ".wav", "_XX") is [UNKNOWN]; the compare is a case-insensitive PREFIX, and the first
//   name the detour sees is logged unconditionally.
#include <windows.h>

#include <cstdint>
#include <cstring>

#include "../common/log.h"
#include "boneblend.h"   // InstallSingleGameDetour - the shared verify / suspend / commit path
#include "camhook.h"     // BoneProbeCapturedModel / BoneProbeCapturedEntity - F7's "our model" test
#include "pushgrab.h"    // PushGrabSessionSolo - F7's co-op test
#include "pumpsound.h"
#include "pumpsound_logic.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

#if __has_include("../common/buildrva.h")
#include "../common/buildrva.h"
#define DLVR_PUMPSND_IS_EGL() (::dlvr::buildrva::ModuleBuild(::dlvr::buildrva::kGamedll) == ::dlvr::buildrva::kEgl)
#else
#define DLVR_PUMPSND_IS_EGL() false
#endif

namespace dlvr {
namespace {

constexpr uintptr_t kSoundPlayRva = 0x110D890;      // AnimActionPlaySound2DExternal play slot 110 (Steam gamedll 97046bba)
constexpr size_t    kNameOff      = 40;             // this + 40 = m_SoundName (ttl::string_base<char>; +0 = char*)
const unsigned char kSoundPlayHead[16] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55 };

using PlayFn = void(__fastcall*)(void*, void*, void*, void*, int, unsigned*);

PlayFn          g_playTrue    = nullptr;   // the trampoline (16 stolen bytes, then jmp +110D8A0)
volatile LONG   g_silenced    = 0;
volatile LONG   g_skipped     = 0;
volatile LONG   g_firstLogged = 0;         // the first name seen has been logged
volatile LONG   g_firstSkip   = 0;         // the first skip has been logged
LONG            g_state       = 0;         // 0 not tried, 1 live, -1 refused (written once, from the reload Resolve)

// Reads the NUL-terminated name into out (<= cap-1 chars) under __try. false = the pointer chain was unreadable.
// No C++ objects here: __try needs none to unwind.
bool ReadName(void* self, char* out, int cap)
{
    __try {
        const char* p = *reinterpret_cast<const char* const*>(reinterpret_cast<uintptr_t>(self) + kNameOff);
        if (!p) { out[0] = 0; return true; }
        int i = 0;
        for (; i < cap - 1 && p[i]; ++i) { out[i] = p[i]; }
        out[i] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return false; }
}

bool HeadReadable(uintptr_t a, unsigned char* out16)
{
    __try { memcpy(out16, reinterpret_cast<const void*>(a), 16); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// COST per game animscript sound: off (not silenced, first name already logged) = two volatile loads and the call-through; silenced =
// one guarded read of <= 31 chars and a <= 15 char compare. The very first call also reads and logs the name once.
// F7 (docs/AUDIT-COOP-PER-PLAYER-2026-11-13.md, trace 20261113_1810): is the sound's model ours - the captured body model or one of
// the captured body's two held-item models (entity + 0x1020 / + 0x1028, INV-173)? Which of the two a rack plays on is [UNKNOWN], so
// both count. COST: one accessor call + two guarded reads, only while a matched rack sound would be silenced.
volatile LONG g_remoteKept = 0, g_saidRemote = 0;
bool PumpModelIsOurs(void* model)
{
    if (!model) { return false; }
    if (model == BoneProbeCapturedModel()) { return true; }
    const uintptr_t ent = BoneProbeCapturedEntity();
    if (!ent) { return false; }
    __try {
        for (int h = 0; h < 2; ++h) {
            if (*reinterpret_cast<void* const*>(ent + 0x1020 + 8 * h) == model) { return true; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return false;
}

void __fastcall PlayHook(void* self, void* model, void* a3, void* a4, int a5, unsigned* a6)
{
    const bool silenced = g_silenced != 0;
    if (silenced || g_firstLogged == 0) {
        char name[32];
        const bool ok = ReadName(self, name, sizeof(name));
        if (InterlockedCompareExchange(&g_firstLogged, 1, 0) == 0) {
            // [PROBE: PUMPSND] gate=manual_reload  site=HEADSET  log-only=yes
            LOG("pumpsound: [PUMPSND] FIRST SOUND NAME \"%s\" (read %s; silenced %d) - the raw m_SoundName text of the first animscript "
                "sound the detour saw; the skip matches the case-insensitive prefix \"%s\".",
                name, ok ? "ok" : "FAILED", silenced ? 1 : 0, pumpsound::kSkipPrefix);
        }
        if (ok && pumpsound::PumpSoundSkip(name, silenced) && !PumpModelIsOurs(model) && PushGrabSessionSolo() == 0) {
            // F7 (docs/AUDIT-COOP-PER-PLAYER-2026-11-13.md, trace 20261113_1810): co-op and the sound is NOT on our body or our
            // held item -> another player's rack: left to the game.
            InterlockedIncrement(&g_remoteKept);
            if (InterlockedCompareExchange(&g_saidRemote, 1, 0) == 0) {
                // [PROBE: PUMPSND] gate=manual_reload  site=HEADSET  log-only=yes
                LOG("pumpsound: [PUMPSND] NOT SILENCED \"%s\" on model %p - another player's model in co-op (only our own rack is "
                    "dropped; logged once)", name, model);
            }
        } else if (ok && pumpsound::PumpSoundSkip(name, silenced)) {
            const LONG n = InterlockedIncrement(&g_skipped);
            if (InterlockedCompareExchange(&g_firstSkip, 1, 0) == 0) {
                // [PROBE: PUMPSND] gate=manual_reload  site=HEADSET  log-only=yes
                LOG("pumpsound: [PUMPSND] FIRST SKIP \"%s\" - the game's rack sound dropped while a manual pump is held (skip #%ld).",
                    name, static_cast<long>(n));
            }
            return;   // a plain void return: the function is void and the callers read nothing back from it
        }
    }
    g_playTrue(self, model, a3, a4, a5, a6);
}

}  // namespace

bool PumpSoundInstall()
{
    if (g_state != 0) { return g_state > 0; }
    g_state = -1;   // refused until proven otherwise
    if (DLVR_PUMPSND_IS_EGL()) {
        // [PROBE: PUMPSND] gate=manual_reload  site=FLAT  log-only=yes
        LOG("pumpsound: [PUMPSND] REFUSED - the Epic (EGL) gamedll is loaded and +%X is not on tools/egl_allowlist.tsv; no address is "
            "guessed. The pump keeps the game's own rack sound this launch.", static_cast<unsigned>(kSoundPlayRva));
        return false;
    }
    HMODULE gd = GetModuleHandleA("gamedll_x64_rwdi.dll");
    if (!gd) {
        // [PROBE: PUMPSND] gate=manual_reload  site=FLAT  log-only=yes
        LOG("pumpsound: [PUMPSND] REFUSED - gamedll_x64_rwdi.dll is not loaded; the pump keeps the game's own rack sound.");
        return false;
    }
    const uintptr_t at = reinterpret_cast<uintptr_t>(gd) + kSoundPlayRva;
    unsigned char got[16] = {};
    if (!HeadReadable(at, got) || memcmp(got, kSoundPlayHead, sizeof(got)) != 0) {
        // [PROBE: PUMPSND] gate=manual_reload  site=FLAT  log-only=yes
        LOG("pumpsound: [PUMPSND] REFUSED - gamedll +%X first 16 bytes %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X "
            "%02X %02X %02X do not match the Steam 97046bba head (48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 55) - a different gamedll "
            "build; nothing patched, the pump keeps the game's own rack sound.",
            static_cast<unsigned>(kSoundPlayRva), got[0], got[1], got[2], got[3], got[4], got[5], got[6], got[7], got[8], got[9],
            got[10], got[11], got[12], got[13], got[14], got[15]);
        return false;
    }
    int attempts = 0;
    if (!InstallSingleGameDetour(at, kSoundPlayHead, sizeof(kSoundPlayHead), reinterpret_cast<void*>(&PlayHook),
                                 reinterpret_cast<void**>(&g_playTrue), "pump rack sound", &attempts)) {
        // [PROBE: PUMPSND] gate=manual_reload  site=FLAT  log-only=yes
        LOG("pumpsound: [PUMPSND] REFUSED - the detour write at +%X was refused after %d attempt(s) (threads would not clear the "
            "range); the pump keeps the game's own rack sound.", static_cast<unsigned>(kSoundPlayRva), attempts);
        return false;
    }
    g_state = 1;
    // [PROBE: PUMPSND] gate=manual_reload  site=FLAT  log-only=yes
    LOG("pumpsound: [PUMPSND] DETOUR LIVE - AnimActionPlaySound2DExternal play slot +%X (16 bytes, %d attempt(s)); drops names starting "
        "\"%s\" (case-insensitive) while a manual pump is held.", static_cast<unsigned>(kSoundPlayRva), attempts,
        pumpsound::kSkipPrefix);
    return true;
}

void PumpSoundSetSilenced(bool on) { g_silenced = on ? 1 : 0; }

long PumpSoundSkipped() { return static_cast<long>(g_skipped); }

}  // namespace dlvr
