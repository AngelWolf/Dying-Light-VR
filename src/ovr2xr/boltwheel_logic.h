#pragma once
// The crossbow bolt wheel from the bolt holster - the pure rules (spec docs/superpowers/specs/2026-11-04-crossbow-bolt-wheel-design.md,
// plan docs/superpowers/plans/2026-11-04-crossbow-bolt-wheel.md Task 1). Header-only, no Windows headers: boltwheel.cpp (the hooks) and
// holster.cpp (the drive) use it, src/src/tests/boltwheel_logic_test.cpp tests it.
//
// THE GAME SIDE (INV-678, OUR gamedll 97046bba): HudBoltSelection opens on ONE input event of its show action 310 and stays open only
// while the input lookup sub_181110BC0(im, 310, target) reads exactly 1.0; a close with the show action down < 0.2 s and no pick is
// the game's "select next" (INV-640) - a bolt-type change nobody chose, which NoSelectNext prevents (plan ruling B1).
namespace dlvr::boltwheel {

constexpr int kShowAction = 310;            // HudBoltSelection vfunc +1608 (+52CED0) returns it
constexpr unsigned kNoSelectNextMs = 250;   // plan B1: the game's 0.2 s (INV-640) + 50 ms margin, my call
constexpr unsigned kWantFreshMs = 200;      // plan B2: a want older than this is stale (a stalled pad thread releases 310)

// The pad thread's want, stamped at setMs, read at nowMs (another thread's clock: a stamp just after nowMs is fresh).
inline bool WantFresh(bool want, unsigned long long setMs, unsigned long long nowMs)
{
    if (!want) { return false; }
    return nowMs <= setMs || nowMs - setMs <= kWantFreshMs;
}

// The arrow wheel from the bow's quiver (2026-11-11, Jay: "Yes."): HudArrowsSelection is the same HudItemSelectionBase (shared Update
// +575C70) with show action 288 (OUR gamedll 97046bba, vtable +14CA538 slot 201 = +284320 returns 288 - ida/wh4_gamedll_out.txt).
constexpr int kArrowShowAction = 288;

// The lookup detour's rule, per wheel: only that wheel's show action, only with that wheel's fresh want.
inline bool ForceHeldFor(int actionId, int showAction, bool wantFresh) { return wantFresh && actionId == showAction; }

// The lookup detour's rule: only 310, only with a fresh want.
inline bool ForceHeld(int actionId, bool wantFresh) { return ForceHeldFor(actionId, kShowAction, wantFresh); }

// X detached from the quiver wheels (2026-11-11, Jay: "I'd like holding X to no longer bring up the arrow/bolt wheels" -> "Yes, build
// it"). The real X opens a wheel through ONE show event to its input handler (INV-678: the dispatcher calls the target's vfunc +48 =
// OnInputGameController +575830, which opens only for the wheel's own show id). With the wheel's X block live (its quiver hooks are in):
// that wheel's own show event is dropped - every other id (the slot / steering ids) passes; our own open calls the handler directly.
inline bool DropShowEvent(int actionId, int showAction, bool xBlocked) { return xBlocked && actionId == showAction; }

// The lookup detour's read of a show action: 1 = forced held (our fresh want), 0 = forced released (the X block live, no want - X can
// never hold a wheel open), -1 = the game's own value (another id, or the block not live and no want - as before the block).
inline int ShowLookup(int actionId, int showAction, bool wantFresh, bool xBlocked)
{
    if (actionId != showAction) { return -1; }
    if (wantFresh) { return 1; }
    return xBlocked ? 0 : -1;
}

// Loot by hand's search hold (2026-11-22 (71), Jay: Survivor Sense must not fire while a hand searches; INV-765, trace
// 20261122_1620): TriggerController::Update holds a trigger while the lookup reads action 20 > 0 for the player's target (player +
// 2288 - PlayerDI slot 324 +BA0F20). Forced held only for that id, a fresh want, and the want's own target (never a 0 target).
constexpr int kTriggerHoldAction = 20;   // ida/ctn2: sub_1800A8FE0 table +139FB18 [20, 126, 127, 128], index 0
inline bool TriggerHoldLookup(int actionId, bool wantFresh, unsigned long long target, unsigned long long wantTarget)
{
    return actionId == kTriggerHoldAction && wantFresh && wantTarget != 0 && target == wantTarget;
}

// The reload prompt (2026-11-11, Jay: "hide the 'PRESS X/SQUARE TO RELOAD' text" -> "Reload prompt only"). OUR gamedll 97046bba
// (ida/rn1..rn6): HudReloadNotifyDI's Update +5A7FF0 shows the prompt type its picker +5A8CE0 returns (0 = hidden); the picker collects
// candidates, sorts them by priority +5A7CB0 (ascending; reload 1 -> 3, an unknown type -> 100) and returns the first. The reload
// candidate is the store `mov dword ptr [r15+rax*4], 1` at +5A8F40; its immediate byte +5A8F44 set to 0 makes the candidate "none":
// priority 100 puts it behind every real prompt (heal 2, UV 4, hints 6 ... still show), and alone it returns 0 = no prompt.
constexpr unsigned char kPromptImmGame = 1;   // the game's byte: the reload candidate
constexpr unsigned char kPromptImmNone = 0;   // ours: no candidate
inline unsigned char PromptImmWanted(bool manualReload) { return manualReload ? kPromptImmNone : kPromptImmGame; }
// The site is accepted (and written) only while it holds one of the two - anything else is not this code.
inline bool PromptSiteOk(unsigned char imm) { return imm == kPromptImmGame || imm == kPromptImmNone; }

// Two more prompts hidden (2026-11-11 (5), Jay: "GRAPPLE PROMPT/HOLD X TO CHANGE ARROWS" -> the combat Grapple skill's prompt + "Yes"
// to the bolts too -> "Yes" to the plan). OUR gamedll 97046bba, ida/hp1-hp10 (engine: 0 hits for every anchor); every context below is
// byte-identical in Steam and EGL (one hit each; the jz context hits exactly our two sites in each):
//  - the tutorial hints Tutorial_Hints_ArrowTypes / ChangeArrow ("Hold X to change Arrows") and BoltTypes / ChangeBolt ("...Bolts"):
//    TutorialManager's bow / crossbow hint update +F9F6A0 enters each block only if vfunc +544 (can show) passes for one of its two ids
//    (manager dwords 47 / 48, 45 / 46); its jnz is nopped and its jz made a jmp to the block's own skip target (+F9FD5D / +F9FF19,
//    both starting `mov rax, [r14]`; no jump lands inside a patched window) = the game's "cannot show" path.
//  - the Grapple prompt = HUD component HudNageWaza (vtable +14F4900 slot 178 = Update +57E290): `mov dil, 1` (show) -> 0.
struct PatchSite {
    const char* name;        // for the log line
    int gate;                // 0 = the arrow wheel's (bow_manual_nock + bow_manual_draw), 1 = manual_reload, 2 = enemy_push_grab
    unsigned rva;            // the first patched byte (Steam; EGL via buildrva)
    unsigned char ctxLen;    // bytes checked around it ...
    unsigned char at;        // ... the patched bytes start at ctx[at]
    unsigned char len;       // patched length
    unsigned char ctx[24];   // the game's bytes, ctx[0] at rva - at
    unsigned char game[2];
    unsigned char ours[2];
};
constexpr int kPatchCount = 5;
constexpr PatchSite kPatches[kPatchCount] = {
    { "arrow hints (gate jnz)", 0, 0xF9FBB6, 22, 7, 2,
      { 0x0F, 0x28, 0x74, 0x24, 0x50, 0x84, 0xC0, 0x75, 0x1B, 0x49, 0x8B, 0x06, 0x49, 0x8B, 0xCE, 0x41, 0x8B, 0x96, 0xBC, 0x00, 0x00, 0x00 },
      { 0x75, 0x1B }, { 0x90, 0x90 } },
    { "arrow hints (gate jz -> jmp)", 0, 0xF9FBCD, 17, 8, 2,
      { 0xFF, 0x90, 0x20, 0x02, 0x00, 0x00, 0x84, 0xC0, 0x0F, 0x84, 0x8A, 0x01, 0x00, 0x00, 0x48, 0x8B, 0xCE },
      { 0x0F, 0x84 }, { 0x90, 0xE9 } },
    { "bolt hints (gate jnz)", 1, 0xF9FD72, 23, 8, 2,
      { 0xFF, 0x90, 0x20, 0x02, 0x00, 0x00, 0x84, 0xC0, 0x75, 0x1B, 0x49, 0x8B, 0x06, 0x49, 0x8B, 0xCE, 0x41, 0x8B, 0x96, 0xB4, 0x00, 0x00,
        0x00 },
      { 0x75, 0x1B }, { 0x90, 0x90 } },
    { "bolt hints (gate jz -> jmp)", 1, 0xF9FD89, 17, 8, 2,
      { 0xFF, 0x90, 0x20, 0x02, 0x00, 0x00, 0x84, 0xC0, 0x0F, 0x84, 0x8A, 0x01, 0x00, 0x00, 0x48, 0x8B, 0xCE },
      { 0x0F, 0x84 }, { 0x90, 0xE9 } },
    { "grapple prompt (show byte)", 2, 0x57E2CE, 17, 11, 1,
      { 0x80, 0xB8, 0x8C, 0x02, 0x00, 0x00, 0x00, 0x74, 0x05, 0x40, 0xB7, 0x01, 0xEB, 0x03, 0x40, 0x32, 0xFF },
      { 0x01, 0x00 }, { 0x00, 0x00 } },
};
// 1 = the site holds the game's bytes, 2 = ours, 0 = neither (a context byte off, or a half / foreign write) - never written then.
inline int PatchSiteState(const PatchSite& s, const unsigned char* got)
{
    for (unsigned k = 0; k < s.ctxLen; ++k) {
        if ((k < s.at || k >= static_cast<unsigned>(s.at + s.len)) && got[k] != s.ctx[k]) { return 0; }
    }
    bool g = true, o = true;
    for (unsigned k = 0; k < s.len; ++k) {
        if (got[s.at + k] != s.game[k]) { g = false; }
        if (got[s.at + k] != s.ours[k]) { o = false; }
    }
    return g ? 1 : (o ? 2 : 0);
}

// The Update wrapper's rule (plan B3): one open event per hold, never onto an open wheel.
inline bool OpenEventDue(bool wantFresh, bool wheelOpen, bool sentThisHold) { return wantFresh && !wheelOpen && !sentThisHold; }

// The bolt holster hold (plan ruling B4, revised after RUN BW1 - Jay: "Pulling bolt from the shoulder can trigger the weapon wheel
// whilst pulling the bolt back to the crossbow"): it STARTS with a take press in the zone and ENDS on a grip release, the take no longer
// live, or the hand leaving the zone BEFORE the wheel is open (an ordinary take carries the bolt away with the grip still down). Once
// the wheel is open, leaving the zone does not end it. Returns the new "armed".
inline bool BoltHoldStep(bool armed, bool takePress, bool gripDown, bool takeLive, bool inZone, bool wheelOpen)
{
    if (takePress) { armed = true; }
    if (!gripDown || !takeLive) { return false; }
    if (!inZone && !wheelOpen) { return false; }
    return armed;
}

// Review 2026-11-05 finding 1 (Jay: "No flicker, wheel at 0.7 s"): the show action goes down for the GAME only from the drive's OPEN
// phase (open or confirming), not its Lead phase - the game starts its 0.2 s at our open event (INV-640 / INV-678), so an event sent in
// Lead and released before OPEN either flickers the wheel or selects next. A Lead-phase release now sends nothing.
inline bool ShowDownFromOpen(bool driveButtonDown, bool openOrConfirming) { return driveButtonDown && openOrConfirming; }

// Plan B1: true while the show action must stay down - while the drive holds it, and after the drive lets go until it has been down
// kNoSelectNextMs since its press edge. A new press edge restarts the count.
class NoSelectNext {
public:
    bool Update(bool buttonDown, unsigned long long nowMs)
    {
        if (buttonDown && !wasDown_) { downAt_ = nowMs; pressed_ = true; }
        wasDown_ = buttonDown;
        if (buttonDown) { return true; }
        if (!pressed_) { return false; }
        if (nowMs - downAt_ < kNoSelectNextMs) { return true; }
        pressed_ = false;
        return false;
    }

private:
    unsigned long long downAt_ = 0;
    bool wasDown_ = false;
    bool pressed_ = false;
};

}  // namespace dlvr::boltwheel
