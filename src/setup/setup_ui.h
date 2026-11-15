// The setup window (docs/superpowers/specs/2026-10-17-installer-design.md 8.2): one dark window with its own title bar,
// the header (title.png, status, the Install / Update / Installed button) and the step cards, after the Titanfall 2 VR
// installer Jay pointed at. Dear ImGui on D3D11. The install runs on a worker thread with a progress bar in the mod card.
#pragma once

#include <string>
#include <vector>

#include "setup_settings.h"
#include "setup_sys.h"

namespace dlvr::setup {

struct UiArgs {
    DiscoveryOptions discovery;
    fs::path forcedGame;          // --game-dir
    fs::path docs;                // Documents (video.scr)
    fs::path steamUserdata;       // controller-layout profiles
    Payload* payload = nullptr;
    std::vector<ManifestEntry> manifest;
    std::string package;          // from the manifest's "# package=" line
    std::string manifestMd5;
    std::string titlePng;         // resource 102
    Gpu gpuOverride;              // --gpu (tests)
    std::vector<std::string> retiredKeys;
    std::vector<ResetRow> resets;  // resource 103 / <payload>\resets.tsv - keep installs move old defaults (empty = none)
};

int RunUi(const UiArgs& a);

// --shot: one frame of a fixed fake state (ready, installing, installed, update, failed, programfiles-epic,
// epic-untested, layout-warning, two-candidates) rendered off-screen to a PNG at the given scale. height > 0 clamps the
// window to that many pixels - the small-screen layout (review focus 1). Exit 0 = written.
int RenderShot(const UiArgs& a, const std::string& state, const std::wstring& png, float scale, int height);

}  // namespace dlvr::setup
