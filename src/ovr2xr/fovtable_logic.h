#pragma once
// First-launch FOV table (docs/BETA-FINDINGS-2026-10-19.md F1; Jay, 2026-10-19: "Table + wide fallback").
//
// ovr_GetHmdDesc has to answer before any OpenXR view exists, so a launch with no hmd_cache.txt used to hand the game
// a small symmetric tan(0.8) frustum - black bars all session (two testers). The OpenXR system name IS known by then
// (xrGetSystemProperties in ovr_Create), so it picks a row here instead.
//
// The name does NOT identify one lens: "Meta Quest 3" (VDXR) measured three different FOVs and "SteamVR/OpenXR :
// oculus" two, and "lighthouse" is SteamVR's tracking system, not one headset. So each row is the LARGEST FOV measured
// under that name - max over every set and both eyes (eye 1 mirrored), plus the 0.0005 print error, rounded UP to 0.01.
// Too large is safe: the game renders a frustum CONTAINING the lens and the compositor drops the excess (cost: fewer pixels
// per degree, this one launch). Too small is the black-bar bug - and since symmetric_fov 0 is the default (trace 20261124_2050)
// EVERY side must fit on its own: the row's nose side is no longer widened to its outer one. An unknown name gets the wide row,
// the max of all rows. fovtable_logic_test.cpp holds every measured set and checks each fits its row.
//
// Values are LibOVR tangents for eye 0 (LEFT): l = outer, r = inner. Eye 1 is the mirror.

#include <cstring>

namespace dlvr::fovtable {

struct Tans { float l, r, u, d; };

struct Row { const char* name; Tans eye0; };

// Sources (decoded tester logs, backups/): Quest 3 = Jay + seven testers; oculus = Jay + one tester; playstation_vr2 =
// one tester (3 dp); lighthouse = one tester (Index); cv = one tester.
constexpr Row kRows[] = {
    { "Meta Quest 3",                     { 1.38f, 1.01f, 1.12f, 1.43f } },
    { "SteamVR/OpenXR : oculus",          { 1.38f, 1.01f, 1.12f, 1.43f } },
    { "SteamVR/OpenXR : playstation_vr2", { 1.85f, 0.95f, 1.33f, 1.33f } },
    { "SteamVR/OpenXR : lighthouse",      { 1.55f, 1.16f, 1.41f, 1.42f } },
    { "SteamVR/OpenXR : cv",              { 1.69f, 1.21f, 1.20f, 1.73f } },
};

constexpr Tans kWide = { 1.85f, 1.21f, 1.41f, 1.73f };   // max of every row, component by component

inline Tans ForEye(const Tans& eye0, int eye)
{
    return eye == 1 ? Tans{ eye0.r, eye0.l, eye0.u, eye0.d } : eye0;
}

// Fills out for the eye. Returns the matched row's name, or nullptr when the wide fallback was used.
inline const char* Lookup(const char* systemName, int eye, Tans& out)
{
    if (systemName) {
        for (const Row& r : kRows) {
            if (std::strcmp(systemName, r.name) == 0) {
                out = ForEye(r.eye0, eye);
                return r.name;
            }
        }
    }
    out = ForEye(kWide, eye);
    return nullptr;
}

}  // namespace dlvr::fovtable
