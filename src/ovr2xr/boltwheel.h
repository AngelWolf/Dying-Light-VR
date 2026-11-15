// boltwheel - the crossbow bolt wheel opened from the bolt holster, with no X sent (spec
// docs/superpowers/specs/2026-11-04-crossbow-bolt-wheel-design.md, plan docs/superpowers/plans/2026-11-04-crossbow-bolt-wheel.md Task 2).
// Two gamedll hooks: the input lookup reads the wheel's show action as held while our hold wants it, and the bolt wheel's own Update
// sends it its one open event. See boltwheel.cpp.
#pragma once

namespace dlvr {

// Install both hooks (the lookup detour first, then each wheel's vtable slot). Idempotent; camhook's 1 Hz thread. Per wheel, does
// nothing (one line) while its gate is off: the bolt wheel manual_reload, the arrow wheel bow_manual_nock + bow_manual_draw.
void BoltWheelInstall();

// Pad thread: the bolt drive wants the show action held (stamped now; stale after boltwheel::kWantFreshMs).
void BoltWheelSetWant(bool want);

// Both hooks are in - the bolt drive refuses to open without this.
bool BoltWheelLive();

// Open events sent so far (for the drive's lines).
long BoltWheelOpensSent();

// The bow's ARROW wheel from the right-shoulder quiver (2026-11-11, Jay: "Yes."), the same two hooks on HudArrowsSelection (show
// action 288). BoltWheelInstall installs it too, while bow_manual_nock + bow_manual_draw are on. Same threads as the bolt ones.
void ArrowWheelSetWant(bool want);
bool ArrowWheelLive();
long ArrowWheelOpensSent();

// The X block (2026-11-11, Jay: "I'd like holding X to no longer bring up the arrow/bolt wheels" -> "Yes, build it"): installed with
// each wheel's quiver hooks, the real X's show event is dropped before the wheel's input handler and its show action reads 0 without
// the quiver hold. True = the arrow wheel's block is in (holster.cpp then drives no X-held arrow wheel). BoltWheelInstall also keeps
// the "X Reload" prompt hidden while manual_reload is on (the prompt picker's reload candidate, ida/rn6).
bool ArrowWheelXBlocked();

}  // namespace dlvr
