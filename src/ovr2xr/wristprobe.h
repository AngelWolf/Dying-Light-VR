// WRIST HUD STAGE 0 PROBE - LOG-ONLY (2026-11-12; design docs/superpowers/specs/2026-11-12-wrist-hud-design.md §10 stage 0,
// statics INV-724; Jay: "Build stage 0").
//
// WHY THIS EXISTS
//
// The design routes each HUD piece's draws to its own image (Mamfro's dying-light-games-vr, dl1/src/hud - used with the
// author's permission). Two of its inputs are NOT established on our path, plus one the HUD-mode setting needs:
//   (a) which THREAD the game calls the engine's UI element functions on (Mamfro takes his tree snapshot inside hooks on
//       IsActuallyVisible / IUIText::SetText / SetOpacity "on the game thread"; the engine's element state is double-buffered
//       per calling thread, INV-724) - and whether a snapshot taken from inside such a call reads the tree correctly;
//   (b) where each piece sits as the engine reports it (global transform, size, visible, opacity) - the units are NOT read;
//   (c) which HUD COMPONENT (id, class, hide mask) draws each piece - needed for ui_wrist_hud_mode KEEP in Limited / Hidden
//       (INV-642 / INV-643: groups 12 NoNavigation and 13 Hidden).
// The fourth open, where the in-play HUD's TEXT draws, is answered by the bootstrap's existing [UIMTX] probe (marker
// dlvr\ui_matrix_probe.txt) in the same run.
//
// WHAT IT DOES
//
// Three gamedll IMPORT-TABLE swaps (common/iathook.h PatchIat - no code patching), each a pass-through that calls the original
// with the same arguments: ?IsActuallyVisible@IUIElement, ?SetText@IUIText, ?SetOpacity@IUIElement (all imported by our gamedll
// 97046bba, INV-724). Every call: its thread counted (8 slots; a ninth thread is counted, never dropped). At most every 5 s,
// INSIDE one such call on the calling thread: from the element the game passed, walk up the parents by plain SEH-guarded reads
// (engine object +0x18, parent object +528, its wrapper +16, name = C string at [[object+8]]) to the root; when the root is named
// HUD_DI, read its direct children through the engine's own exports and, for the five pieces, their transform / size / visible
// / opacity and their children's names + RTTI classes; then the HUD manager's component registry (gamedll +1C16358: components
// +72 + 8 x id, masks +1368 + 4 x id, flags +0x30, HUD mode cache +1964). The snapshot is written into a buffer; the LINES are
// printed from the probe's own thread (no log call on the game thread).
//
// PASSING VALUES, STATED IN ADVANCE (one run, HUD option Full then Limited then Hidden, a quest active, health below full)
//   SNAPSHOT lines name thread T for the snapshot, and T carries most calls in the 5 s THREADS line   -> (a) answered: T
//   root 'HUD_DI' found + all five pieces FOUND with visible 1 in Full                                -> the read works on ours
//   piece transforms: translation column near the layout positions (INV-724 table, DataDLC24_2) in some unit -> (b) units
//   COMPONENTS table: classes HudRadar / HudHealthIndicator / HudTrackQuest / HudPrimaryWeaponIndicator ... with ids; masks
//       gaining bit 12 (0x1000) in Limited on HudRadar (+ HudBounty), bit 13 (0x2000) in Hidden on the pieces    -> (c)
//   FAIL shapes, each named in its line: 'IAT slot NOT FOUND', 'root is not HUD_DI', 'piece NOT FOUND', 'READ FAULTED',
//       'no HUD manager'.
// Gate: marker dlvr\wrist_probe.txt (presence = on). Off prints one line naming it; nothing is installed.
#pragma once

namespace dlvr {
namespace wristprobe {

// Called from ovr_Initialize. Starts at most once; logs one line either way. The import swaps wait (own thread) for the gamedll.
void Start();

// The UI-screen height of the latest published HUD root (its bottom, root[3] - the same value the bootstrap's router uses as its
// screen height). 0 = nothing published yet (callers refuse). One shared SRW lock. (UI headset sweep, trace 20261113_2125.)
float UiScreenH();

}  // namespace wristprobe
}  // namespace dlvr
