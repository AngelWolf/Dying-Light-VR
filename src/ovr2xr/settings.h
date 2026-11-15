// The settings registry.
//
// Every knob is described once, here, and three things are generated from that
// one description: the config-file parser, the config-file writer, and the
// overlay UI. They used to be a hand-written sscanf chain, a hand-written text
// file and nothing respectively, which is exactly the arrangement where a knob
// gets added to two of the three and quietly does nothing in the third.
#pragma once

#include <cstddef>

#include "xrloader.h"

namespace dlvr {

enum class SetType { Bool, Int, Float };

struct Setting {
    const char* key;      // name in ovr2xr.txt
    SetType type;
    void* value;          // bool*, int* or float*
    float lo, hi;         // range for the slider, and for clamping on load
    const char* label;    // overlay label
    const char* help;     // overlay tooltip - the "why", not the "what"
    const char* const* items;  // optional: names for an enum-valued int
    int itemCount;
    const char* group;    // overlay section heading
    bool restart;         // the game reads this once at startup
    // LOCKED (2026-10-07, Jay: "force enabled and hide the setting toggle ... that is now considered loadbearing"): the
    // file's value is ignored (logged when it differs) and the compiled value stands; the overlay does not draw it. The key
    // stays in the table so the parser and writer still know it - a key dropped from the table is a dead-key BLOCK (D-156).
    // Every entry not using S_BOOL_LOCKED zero-initialises this to false.
    bool locked;
    // FOLLOWS A MASTER (2026-10-08, D-184, Jay: hide afw_alt_eye, "rebuild follows the master"): non-null = this bool takes
    // the master's value at load and after every overlay edit (SyncFollowers), its file value is never read, and it is
    // hidden like a locked one (S_BOOL_FOLLOWS also sets `locked`). The key stays in the table and in the file (D-156).
    // Every entry not using S_BOOL_FOLLOWS zero-initialises this to nullptr.
    const bool* follows;
};

// Writes the file if anything changed. Called by the overlay after every edit,
// so the file always matches what is on screen.
void SaveSettingsIfChanged();

// The registry. Stable for the life of the process.
const Setting* AllSettings(int& count);

// Builds "<game dir>\dlvr\<leaf>", resolved relative to THIS DLL rather than
// the exe, and creates the folder if it is missing. Exposed because the overlay
// writes the DLSS marker files there and must not rebuild this path itself: on
// a fresh install nothing else creates dlvr\, and every writer in the mod
// degrades silently when it is absent.
bool DlvrFilePath(const char* leaf, char* out, size_t cch);

// Applies one "key=value" line if it names a known setting. Returns false if
// the key is not ours, so the caller can keep its own special cases.
bool ApplySettingLine(const char* line);

// Rewrites the value of every known key in dlvr\ovr2xr.txt, in place, leaving
// comments and layout untouched. Anything not already present is appended.
// Preserving the file is the point: those comments record why each value is
// what it is, and a UI that flattened them would destroy the reasoning behind
// its own defaults.
bool WriteSettingsFile(char* whyNot, size_t whyNotBytes);

// The overlay's own settings, by reference so the overlay can rebind itself and
// have the change land in the same table everything else is saved from.
int&   OverlayKey();
int&   OverlayPad();
float& OverlayDistance();
float& OverlayWidth();
bool&  OverlayEnabled();
int&   OverlayTextSize();   // overlay_text_size: the mod menu's text, percent of the built-in size (BETA menu, 2026-11-12)
bool&  OverlayAdvanced();   // overlay_advanced: the menu's Basic / Advanced button (BETA menu, 2026-11-12)

// Horizon lock, and the head pose we last reported to the game - the camera tab
// needs the latter to subtract our own contribution from the engine's camera.
bool&  HorizonLock();
int    CamUpAxis();
void   PullCamHookOp();   // cam_hook_op: the overlay's int mirrored into CamHookOp (was PullCamConvention)
const XrQuaternionf& LastReportedHead();

// False until the engine has asked us for this frame's head pose. The camera
// hook uses it to tell whether the head it is about to apply belongs to this
// frame or the last one.
bool HeadSampledThisFrame();

}  // namespace dlvr
