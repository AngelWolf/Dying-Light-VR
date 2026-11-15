// calibsound.h - the calibration screen's sounds: the game's OWN menu sounds from menu.csb, played through the engine export
// gui::IObject::PlaySound2D (docs/superpowers/plans/2026-10-14-calibration-screen-build-d.md task 6, route A;
// docs/RESEARCH-CALIB-AUDIO-2026-10-14.md section ROUTE A). Nothing of the game's is copied: the engine plays its own bank.
//
// HOW: a stand-in gui::IObject (+24 -> a stand-in gui::CObject whose +40 = the current level's UI system, read fresh by
// uipin::UiSystemForSound on every play) is passed as `this`; the engine then gives the sound the same UI-manager value a
// real widget of that UI would (ROUTE A 3). The game's own highlight call's arguments: (name, false, 1.0, 0, 0, bus 8, 1.0).
//
// GUARDS: on OUR engine build only (e9fbfc2c) - the export's address and first 16 bytes and the UI manager's getter bytes
// are checked once, at the first play; any mismatch = one REFUSED line and silence for the session, never a call. Per play
// the UI system, its +8 vtable + slot 0 and the mixer (FMOD, not the no-sound mixer) are checked BEFORE the call.
//
// THREADING: the SUBMIT THREAD only (the calibration screen's render path). No locking beyond that.
// COST: nothing until a screen event calls CalibSoundPlay. Per play: up to two level walks (a few dozen pointer reads), six
// guarded reads and the engine call (the engine's own sound lookup + start under its sound lock). Plays are rare - a handful
// per calibration. Every play is logged with the id the engine returned.
#pragma once

namespace dlvr {

enum class CalibSnd { Click, PoseDone };

// Submit thread. Click = "click", PoseDone = "menu_timer_loop". Logs every play with the returned id (id 0 = not created).
void CalibSoundPlay(CalibSnd s);

}  // namespace dlvr
