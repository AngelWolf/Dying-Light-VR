// pelvisturn - the lower body turns with the STICK when looking down (2026-11-19, trace docs/traces/20261119_1530_pelvis_stick_turn.md;
// Jay: "when you look directly down it still has the delay" -> "remove the delay from stick turning ONLY, but not from the headturn"
// -> "A"). INV-749: the game's pelvis direction holds while the look pitch <= -35 deg; here it does not hold while the right stick
// turns. See pelvisturn.cpp.
#pragma once

namespace dlvr {

// Swap PlayerFppVis vtable slot 270 (the stand-rotation gate +C8ADE0) for ours. Idempotent; camhook's 1 Hz thread. No key - a fix,
// always on; Steam only (no Epic rvas yet) - anything else refuses by name.
void PelvisTurnInstall();

}  // namespace dlvr
