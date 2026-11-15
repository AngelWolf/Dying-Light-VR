// dlgview - the dialogue's camera point gets the same [EYEPOINT] view offset as on foot (2026-10-25, spec
// docs/superpowers/specs/2026-10-25-dialogue-view-offset-design.md; Jay: "Approved, go ahead and build it"). In a "B Skip" dialogue
// the view and the hands then agree as they do walking around. See dlgview.cpp.
#pragma once

#include <cstdint>

namespace dlvr {

// Swap DialogController's look interface slot 6 (the look position) for ours. Idempotent; camhook's 1 Hz thread. No key - follows
// the cam_eye_* switches; found by RTTI name + the slot's own bytes, so every build either arms or refuses by name.
void DlgViewInstall();

// DialogController's look-interface vtable (RTTI COL offset 24, absolute), once DlgViewInstall's scan found it; 0 before / if never.
// boneblend's scene stand-down (rig_cutscene_standdown) compares the player's active look vtable with it: equal = a dialogue owns the
// look (INV-608). Any thread; written once.
uintptr_t DlgViewLookVtable();

}  // namespace dlvr
