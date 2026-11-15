// Standalone unit test for src/src/ovr2xr/boltwheel_logic.h - NOT part of the DLL build.
// docs/superpowers/plans/2026-11-04-crossbow-bolt-wheel.md Task 1. Exit code = number of failures.
#include "boltwheel_logic.h"

#include <cstdio>

using namespace dlvr::boltwheel;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

// Review Focus 5: only the bolt wheel's show action (310) is ever forced, and only with a fresh want.
static void ForceHeldOnly310()
{
    static_assert(kShowAction == 310, "INV-678: HudBoltSelection's show action");
    CHECK(ForceHeld(310, true));
    CHECK(!ForceHeld(310, false));
    CHECK(!ForceHeld(288, true));   // the arrow wheel's show action
    CHECK(!ForceHeld(21, true));    // _ACTION_RELOAD
}

// The arrow wheel from the bow's quiver (2026-11-11, Jay: "Yes."): HudArrowsSelection's show action is 288 (OUR gamedll +284320,
// ida/wh4); each wheel's want forces ONLY its own show action - the bolt want never 288, the arrow want never 310.
static void ForceHeldEachWheelOnlyItsOwn()
{
    static_assert(kArrowShowAction == 288, "wh4: HudArrowsSelection slot 201 returns 288");
    CHECK(ForceHeldFor(288, kArrowShowAction, true));
    CHECK(!ForceHeldFor(288, kArrowShowAction, false));
    CHECK(!ForceHeldFor(310, kArrowShowAction, true));   // the bolt wheel's id under the arrow want
    CHECK(!ForceHeldFor(288, kShowAction, true));        // the arrow wheel's id under the bolt want
    CHECK(ForceHeldFor(310, kShowAction, true));
    CHECK(ForceHeld(310, true) == ForceHeldFor(310, kShowAction, true));
}

// B2 / Review Focus 3: a want older than 200 ms is stale.
static void WantExpiresAfter200()
{
    static_assert(kWantFreshMs == 200, "plan B2");
    CHECK(WantFresh(true, 1000, 1200));
    CHECK(!WantFresh(true, 1000, 1201));
    CHECK(!WantFresh(false, 1000, 1000));
    CHECK(WantFresh(true, 1000, 999));   // a stamp written (other thread) just after this thread read its clock: still fresh
}

// B3 / Review Focus 4: the open event at most once per hold, never onto an open wheel, never without a want.
static void OpenEventOncePerHold()
{
    CHECK(OpenEventDue(true, false, false));
    CHECK(!OpenEventDue(true, true, false));
    CHECK(!OpenEventDue(true, false, true));
    CHECK(!OpenEventDue(false, false, false));
}

// B1 / Review Focus 1: once pressed, the show action stays down until it has been down kNoSelectNextMs (250).
static void NoSelectNextHoldsTo250()
{
    static_assert(kNoSelectNextMs == 250, "plan B1");
    NoSelectNext n;
    CHECK(!n.Update(false, 0));      // idle: nothing held
    CHECK(n.Update(true, 0));
    CHECK(n.Update(true, 50));
    CHECK(n.Update(false, 100));     // released at 100 - still held for the game
    CHECK(n.Update(false, 249));
    CHECK(!n.Update(false, 250));    // 250 ms down: let go
    CHECK(!n.Update(false, 260));
    // a new press at 300, released at 600: down >= 250 already - let go at once
    CHECK(n.Update(true, 300));
    CHECK(n.Update(true, 590));
    CHECK(!n.Update(false, 600));
    // a press that restarts inside the extension: the extension follows the NEW press
    CHECK(n.Update(true, 700));
    CHECK(n.Update(false, 720));
    CHECK(n.Update(true, 730));      // pressed again at 730 while still extended
    CHECK(n.Update(false, 760));
    CHECK(n.Update(false, 979));     // 730 + 249
    CHECK(!n.Update(false, 980));
}

// RUN BW1 bug 1 (Jay: "Pulling bolt from the shoulder can trigger the weapon wheel whilst pulling the bolt back to the crossbow"): the
// carry keeps the grip down, so the hold must END when the hand leaves the holster zone before the wheel is open; once open it may.
static void HoldEndsOnZoneExitBeforeOpen()
{
    // args: armed (a take press started it), take press now, grip down, take live, in zone, wheel open
    CHECK(BoltHoldStep(false, true, true, true, true, false));     // a take press in the zone starts it
    CHECK(BoltHoldStep(true, false, true, true, true, false));     // held in the zone: continues
    CHECK(!BoltHoldStep(true, false, true, true, false, false));   // the hand left the zone, wheel not open yet: ENDS (the carry)
    CHECK(!BoltHoldStep(false, false, true, true, true, false));   // ended stays ended - back in the zone needs a new press
    CHECK(BoltHoldStep(true, false, true, true, false, true));     // the wheel is open: leaving the zone does not end it
    CHECK(!BoltHoldStep(true, false, false, true, true, true));    // the grip let go: ends
    CHECK(!BoltHoldStep(true, false, true, false, true, true));    // the take no longer live: ends
    CHECK(!BoltHoldStep(false, false, true, true, false, false));  // nothing armed, no press: nothing
}

// Review 2026-11-05 finding 1 (Jay: "No flicker, wheel at 0.7 s"): the show action goes down only from the drive's OPEN phase, so a
// release in the Lead phase (300-500 ms) sends no open event and holds nothing - no flicker, no select-next; NoSelectNext's 250 ms then
// counts from the OPEN edge (the game's press time is our open event, INV-640 / INV-678).
static void ShowDownOnlyFromOpen()
{
    // args: the drive's button down (Lead / Open / confirming), the wheel Open or confirming
    CHECK(!ShowDownFromOpen(true, false));   // Lead: down for the drive, NOT for the game
    CHECK(ShowDownFromOpen(true, true));     // Open / confirming
    CHECK(!ShowDownFromOpen(false, false));  // idle
    CHECK(!ShowDownFromOpen(false, true));   // cannot happen (open implies down) - never forced
    // a Lead-phase release (the slow take): nothing ever reaches NoSelectNext
    NoSelectNext n;
    CHECK(!n.Update(ShowDownFromOpen(true, false), 300));
    CHECK(!n.Update(ShowDownFromOpen(true, false), 450));
    CHECK(!n.Update(ShowDownFromOpen(false, false), 460));
    // a hold to OPEN at 500, released at 510: held to 500 + 250
    CHECK(n.Update(ShowDownFromOpen(true, true), 500));
    CHECK(n.Update(ShowDownFromOpen(false, false), 510));
    CHECK(n.Update(ShowDownFromOpen(false, false), 749));
    CHECK(!n.Update(ShowDownFromOpen(false, false), 750));
}

// X detached from the quiver wheels (2026-11-11, Jay: "I'd like holding X to no longer bring up the arrow/bolt wheels" -> "Yes, build
// it"): with a wheel's X block live, its OWN show event is dropped (only its own id - the slot steering ids pass), and the lookup reads
// its show action 0 without our want, 1 with it; with the block off the game's own value is read (-1) unless our want forces 1.
static void XBlockDropsOnlyTheShowEvent()
{
    CHECK(DropShowEvent(288, kArrowShowAction, true));
    CHECK(!DropShowEvent(288, kArrowShowAction, false));   // block not live: X opens the wheel as before
    CHECK(!DropShowEvent(310, kArrowShowAction, true));    // the bolt wheel's id at the arrow wheel: not ours to drop
    CHECK(!DropShowEvent(289, kArrowShowAction, true));    // a slot / steering id: passes
    CHECK(DropShowEvent(310, kShowAction, true));
    CHECK(!DropShowEvent(288, kShowAction, true));
}
static void XBlockLookupReads()
{
    CHECK(ShowLookup(288, kArrowShowAction, true, true) == 1);     // our quiver hold: held
    CHECK(ShowLookup(288, kArrowShowAction, true, false) == 1);    // our quiver hold, block not live: held (as before)
    CHECK(ShowLookup(288, kArrowShowAction, false, true) == 0);    // X held, no quiver hold, block live: released
    CHECK(ShowLookup(288, kArrowShowAction, false, false) == -1);  // block not live: the game's own value
    CHECK(ShowLookup(310, kArrowShowAction, true, true) == -1);    // another id: never touched
    CHECK(ShowLookup(310, kShowAction, false, true) == 0);
    CHECK(ShowLookup(55, kShowAction, true, true) == -1);
    // the old rule is the "held" half of the new one
    const int ids[] = { 288, 310, 55 };
    const int shows[] = { kShowAction, kArrowShowAction };
    for (int id : ids) {
        for (int show : shows) {
            for (int fresh = 0; fresh < 2; ++fresh) {
                CHECK(ForceHeldFor(id, show, fresh != 0) == (ShowLookup(id, show, fresh != 0, false) == 1));
            }
        }
    }
}

// The reload prompt (2026-11-11, Jay: "hide the 'PRESS X/SQUARE TO RELOAD' text" -> "Reload prompt only"): the picker's reload
// candidate immediate (OUR gamedll +5A8F44, ida/rn6) is 0 (no candidate - priority 100 sorts it behind every real prompt, and alone it
// is "none") while manual_reload is on, the game's 1 when off; the site is accepted only holding one of the two.
static void ReloadPromptImmediate()
{
    static_assert(kPromptImmGame == 1 && kPromptImmNone == 0, "rn6: mov dword ptr [r15+rax*4], 1 at +5A8F40");
    CHECK(PromptImmWanted(true) == kPromptImmNone);
    CHECK(PromptImmWanted(false) == kPromptImmGame);
    CHECK(PromptSiteOk(kPromptImmGame));
    CHECK(PromptSiteOk(kPromptImmNone));
    CHECK(!PromptSiteOk(2));
    CHECK(!PromptSiteOk(0x18));
}

// The hint / grapple prompt patches (2026-11-11 (5), Jay: "GRAPPLE PROMPT/HOLD X TO CHANGE ARROWS" -> bolts too -> "Yes"; ida/hp1-hp10):
// every table entry's context holds the GAME bytes at its patch offset, the patch is the same length, and a jz -> jmp keeps the rel32
// (the jmp starts one byte later and ends where the jz did, so the same rel32 reaches the same target).
static void PatchTableIsConsistent()
{
    static_assert(kPatchCount == 5, "arrow jnz / jz, bolt jnz / jz, grapple");
    for (int i = 0; i < kPatchCount; ++i) {
        const PatchSite& s = kPatches[i];
        CHECK(s.at + s.len <= s.ctxLen);
        for (unsigned k = 0; k < s.len; ++k) { CHECK(s.ctx[s.at + k] == s.game[k]); }
        CHECK(s.game[0] != s.ours[0] || (s.len > 1 && s.game[1] != s.ours[1]));
        if (s.len == 2 && s.game[0] == 0x0F && s.game[1] == 0x84) {   // jz rel32 -> nop; jmp rel32
            CHECK(s.ours[0] == 0x90 && s.ours[1] == 0xE9);
        }
    }
}
// The site check: game bytes -> 1, our bytes -> 2, anything else (or a context byte off) -> 0; the patched bytes are not context.
static void PatchSiteStateReads()
{
    const PatchSite& s = kPatches[0];
    unsigned char got[32] = {};
    for (unsigned k = 0; k < s.ctxLen; ++k) { got[k] = s.ctx[k]; }
    CHECK(PatchSiteState(s, got) == 1);
    for (unsigned k = 0; k < s.len; ++k) { got[s.at + k] = s.ours[k]; }
    CHECK(PatchSiteState(s, got) == 2);
    got[s.at] = 0xCC;
    CHECK(PatchSiteState(s, got) == 0);
    for (unsigned k = 0; k < s.len; ++k) { got[s.at + k] = s.game[k]; }
    got[0] ^= 0xFF;                       // a context byte off
    CHECK(PatchSiteState(s, got) == 0);
    // a half-written site (first byte ours, second the game's) is neither - refused, never "fixed up"
    for (unsigned k = 0; k < s.ctxLen; ++k) { got[k] = s.ctx[k]; }
    if (s.len == 2) {
        got[s.at] = s.ours[0];
        CHECK(PatchSiteState(s, got) == 0);
    }
}

int main()
{
    PatchTableIsConsistent();
    PatchSiteStateReads();
    XBlockDropsOnlyTheShowEvent();
    XBlockLookupReads();
    ReloadPromptImmediate();
    ShowDownOnlyFromOpen();
    HoldEndsOnZoneExitBeforeOpen();
    ForceHeldOnly310();
    ForceHeldEachWheelOnlyItsOwn();
    WantExpiresAfter200();
    OpenEventOncePerHold();
    NoSelectNextHoldsTo250();
    std::printf("boltwheel_logic_test: %d failure(s)\n", g_fail);
    return g_fail;
}
