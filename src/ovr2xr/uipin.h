// MENU PIN, stage 1 - the main menu and the pause menu stay where they are when the head moves (backlog 5 / 6).
// Design, evidence and what is still unknown: docs/SPEC-UI-MENU-PIN-2026-10-11.md (Jay 2026-10-11: "Stage 1 as scoped.
// 2.0m sounds fine for now. Switch off default is fine, toggle is fine. If the main-menu half fails, we'll fix it first.")
//
// WHY: every ordinary UI draw builds its per-eye camera in engine sub_1802E6BF0 from a UI camera that never moves with
// the head (INV-529, measured) - so the scene turns with the head and the menu does not.
//
// WHAT: one byte-verified detour on that function. For a call whose SOURCE camera is the UI camera of the game's menu
// level (class MainMenuModuleDI, INV-528 / INV-529) the copy the engine just made is replaced:
//   main menu  (the menu level's UI camera is a world camera, determinant +1): the SCENE's camera (engine+AAAE40, the
//              one the 3D eyes are built on, INV-261), each eye offset along its right axis - the game's own 3D menu
//              screens then sit on their world objects;
//   pause menu (screen MenuInGame visible; the UI camera is the pixel default): the UI camera moved, in its own units,
//              by the scene camera's motion since the menu opened (uipin_logic.h), at ui_pin_distance_m.
// Then the camera's inverse matrix is rewritten and the engine's own sub_180764270 rebuilds its derived fields, as the
// original does. The HUD's UI camera is a different pointer and is never touched; any other in-game menu (inventory,
// map, shop) passes through unchanged in stage 1.
//
// COST: OFF and never switched on = nothing installed, one bool test per submit. Switched on: per submit a walk of the
// game's level array by plain reads (a few dozen pointer reads); per call of the engine function one bool test and one
// pointer compare; per MATCHED call three 48-byte reads, two 3x3 multiplies, one 48-byte write, a transpose and the
// engine's sub_180764270 a second time (INV-529: 31-68 matched calls a frame at the main menu, ~190-240 in the pause
// menu). The rule's own time is measured (1 matched call in 64) and printed.
#pragma once

#include <cstddef>
#include <cstdint>

namespace dlvr {

// 2026-10-11: the settings ui_pin_menus / ui_pin_distance_m and the pause-menu rule described above were REMOVED (Jay). What
// remains runs under ui_panel_menus (uipanel.h): MenuState() and the main-menu rule (fed from camhook's written cameras).

// ui_panel_main_menu_turn - see the [DEP:] marker at its definition in uipin.cpp.
extern bool UiPanelMainMenuTurn;

namespace uipin {

// Once per submitted frame (ovr_SubmitFrame). Installs the detour the first time ui_panel_menus is found on.
void Tick();

// True once the detour on engine +2E6BF0 is live (the UICAM probe cannot hold the same address).
bool Installed();

// Which menu the game shows now, from the game's level array (INV-529; plain reads, no detour, independent of
// ui_pin_menus): 0 = none, 1 = the main menu (the menu level's UI camera is a world camera), 2 = an in-game menu (pause,
// inventory, map, shop - the menu level is listed with the pixel UI camera), -1 = not readable this frame.
// `pauseScreen` (optional) = the pause menu's own screen, MenuInGame, is among the visible ones.
// Submit thread only. COST: the level walk - a few dozen pointer reads.
int MenuState(bool* pauseScreen = nullptr);

// GAME-MENU INPUT PASS-THROUGH (spec docs/superpowers/specs/2026-10-25-game-menu-input-passthrough-design.md; Jay: "We need to
// detect the menu state and allow the inputs regardless of throwables etc." -> "All 3."): MenuState() published for the pad thread.
// PublishMenuState: once per submit, right after Tick() (ovr2xr.cpp) - submit thread. COST: one level walk per submit.
void PublishMenuState();
// Any thread: the last published MenuState() was 2 (an in-game menu: pause, inventory, map, shop) and it was published <= 250 ms
// ago. Never published / stale / unreadable = false = today's behaviour. COST: two volatile loads and GetTickCount64.
bool GameMenuOpen();
// RIG / WEAPON PAUSE FREEZE (boneblend.cpp PauseFreezeOn; trace docs/traces/20261113_1750_pause_freeze.md): the last published
// HudHiddenForPause() was 1 (the game paused for a menu / dialog / end screen) and it was published <= 250 ms ago. Never published /
// stale / 0 / -1 = false = the rig solves as before. Any thread. COST: two volatile loads and GetTickCount64.
bool GamePausedForMenu();
// The last published value (-2 = never) and its age in ms, for log lines.
int PublishedMenuState(unsigned* ageMs);

// HUD PANEL (uipanel.cpp, ui_hud_panel): is the in-game HUD showing - a level of class LevelDI is in the game's level
// array with a screen named HUD in its visible list (INV-529)? 1 = yes, 0 = no (a menu, a load, the main menu), -1 = not
// readable this frame. Plain reads, no detour, independent of ui_panel_menus. Submit thread only.
// COST: the level walk - a few dozen pointer reads.
int HudShowing();

// MENU-OPEN PAUSE HOLD (uipanel.cpp RenderHud; trace docs/traces/20261113_1100_menuopen_pause_hold.md): does the game's HUD manager
// (gamedll global +1C16358) have the PauseGame hide reason (1) in its hide list - i.e. the game paused to open a menu, dialog or end
// screen? 1 = yes, 0 = no (or the HUD is not hidden by any reason), -1 = not readable (no gamedll, no manager, a fault). Reads only,
// guarded. Any thread (the game thread edits the list; a torn read faults into -1). COST: <= 19 guarded pointer reads.
int HudHiddenForPause();

// LOADING-SCREEN VOID (loadscreen.cpp, amendment 1): how many screens the first level whose RTTI class is `cls` shows in its
// visible list. 0 = no such level, or none visible; -1 = not readable this frame. Plain reads, no setting. Submit thread only.
// COST: the level walk with one RTTI class read per listed level (a few dozen pointer reads).
int LevelScreensShowing(const char* cls);

// CALIBRATION SCREEN SOUND (calibsound.cpp): the UI system (CUIManager, level +0x568) of the MainMenuModuleDI level when one
// is listed (the main menu and every in-game menu, INV-529), else of the LevelDI level, else 0. Re-read on every call (never
// cached - a level change is picked up). `levelClass` (optional) = the class whose level matched, "none" or "read fault".
// SEH-guarded plain reads. Submit thread only. COST: up to two level walks - called on a sound event only, never per frame.
uintptr_t UiSystemForSound(const char** levelClass = nullptr);

// The game's levels and their visible screens as text, for a log line at a CHANGE of the signal only (not per frame).
void DescribeLevels(char* out, size_t cap);

}  // namespace uipin
}  // namespace dlvr
