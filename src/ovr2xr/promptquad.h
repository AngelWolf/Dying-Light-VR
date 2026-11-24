// promptquad.h - THE IN-WORLD PROMPT (part 2, option 3A): one quad in the scene, at the object the game's X prompt is for, carrying the
// game's own prompt image (glyph + word, per language) - trace docs/traces/20261123_1356_inworld_prompt_quad.md, INV-767 / INV-768.
// Data: DW/Data/optimized_DLC29_dx11.mp + DW/Data/dlvr_prompts_PC.rpack (tools/prompt_compose.py). BETA only.
#pragma once

namespace dlvr {

// [DEP: inworld_prompts] declared and documented in promptquad.cpp (the definition).
extern bool InworldPrompts;
// [DEP: ui_prompt_brightness] / [DEP: ui_prompt_size] declared and documented in promptquad.cpp (trace 20261123_1700).
extern float InworldPromptBrightness;
extern float InworldPromptSize;
// [DEP: ui_prompt_plate] declared and documented in promptquad.cpp (trace 20261123_1830).
extern float InworldPromptPlate;

// HUD thread, from looting.cpp SlotUpdateHook right after the game's own HudTriggerSlot::Update (+5B66D0) and BEFORE the lid
// branch: reads the X slot's visibility, its text key (IUIText::GetText - the key the game just set) and the object's world box,
// and publishes them (seqlocked). No game query of its own. COST: one bool test when off; else IsVisible + GetText + a 14-row key
// compare + one extents read, X slot only.
void PromptQuadOnSlot(void* slot, void* trigger);

// Game thread, the first statement of LidHandTick (BowDrawTick -> the player update): loads our texture pack once, keeps ONE prompt
// object per level (the game's own ModelObject spawn recipe), swaps its material when the prompt / language changes, places it
// facing the head and shows / hides it. COST: one seq read per tick; SetWorldXform + EnableRendering while shown; ReplaceMaterial
// on a change; CreateObject once per level.
void PromptQuadTick();

// ROUTE B (trace docs/traces/20261123_1535_inworld_prompt_routed.md): inworld_prompts latched at the first call (wristprobe::Start,
// after the layer's settings load) - the leaf publish and the bootstrap's router follow the launch value.
bool PromptQuadOnThisLaunch();
// true while the quad is showing with the live material swapped in and the bootstrap's live texture made writable - wristprobe puts
// the prompt's routeMask bit on only then, so the game's widget leaves the HUD only while our quad carries it. Any thread (globals).
bool PromptQuadRouteWanted();
// trace 20261124_0230: the feature is live now (on at launch, still on, the live texture exists) - routeMask bit 6; the bootstrap then
// routes the prompt off the flat HUD whenever its leaves are published, and the pin holds it still. Any thread (globals).
bool PromptQuadLive();

}  // namespace dlvr
