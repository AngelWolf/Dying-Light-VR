// Physical breakout - shove a grabbing zombie off with both hands (spec docs/superpowers/specs/2026-10-24-physical-breakout-design.md).
#pragma once

namespace dlvr {

extern bool  BreakoutPhysical;        // breakout_physical - master. DEFAULT ON (was OFF, Tier C).
extern float BreakoutShoveSpeedMps;   // breakout_shove_speed_mps - the speed each hand must reach for a shove

void BreakoutPoll();                   // scePad poll thread: the shove detector
void BreakoutTick(void* playerVis);    // player-update tail (boneblend.cpp HookTail, game thread): the game call
bool BreakoutPlayerGrabbed();          // true while the game holds the player in a grab (melee throw guard)
bool BreakoutMeleeGateClosed();        // true while grabbed AND past the grab's first 500 ms AND not yet won (phase 6-8) - the swing hit gate

}  // namespace dlvr
