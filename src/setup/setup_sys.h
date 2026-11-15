// The setup program's contact with the PC: finding the game (Steam, Epic, GOG), the GPU, the game process, the VC++
// runtime, the controller-layout profile, and EXECUTING an action list (docs/superpowers/specs/2026-10-17-installer-
// design.md 5, 8.4). Every decision is made by the tested headers; this file only finds things and does what they say.
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "setup_payload.h"
#include "setup_plan.h"
#include "setup_store.h"

namespace dlvr::setup {

namespace fs = std::filesystem;

// The log (%TEMP%\DyingLightVR_Setup.log, or --log): every finding and every action, one line each.
void SetLogPath(const std::wstring& path);
std::wstring LogPath();
void Log(const std::string& s);

std::wstring W(const std::string& utf8);
std::string U(const std::wstring& w);
bool ReadAll(const fs::path& p, std::string* out);

struct Candidate {
    fs::path folder;
    Store store;
};

struct DiscoveryOptions {
    fs::path epicManifests;      // --epic-manifests (default %ProgramData%\Epic\EpicGamesLauncher\Data\Manifests)
    bool useRegistry = true;     // --no-registry: no Steam / GOG registry reads (tests)
};

// Every Dying Light folder found (DyingLightGame.exe present), de-duplicated, Steam first.
std::vector<Candidate> FindGames(const DiscoveryOptions& o);
Store StoreOfFolder(const fs::path& game);
bool UnderProgramFiles(const fs::path& game);
// 1 running, 0 not, -1 could not tell (counts as running).
int GameRunning();

struct Gpu {
    std::string name;
    unsigned vendor = 0;
};
Gpu BestGpu();

VcVersion InstalledVcRuntime();
VcVersion BundledVcRuntime(Payload& p);   // the embedded vc_redist's file version (0.0.0 if unreadable)
std::string VerText(const VcVersion& v);

DetectedState Detect(const fs::path& game);
// "" when every Steam profile is Default (or none exists yet); else the warning. Epic / GOG: "could not check".
std::string LayoutWarning(Store store, const fs::path& steamUserdata);

// Shared between the install worker and the window (review finding 3: the file name was an unguarded std::string).
class Progress {
public:
    std::atomic<int> done{0};
    std::atomic<int> total{0};
    void SetCurrent(const std::string& s)
    {
        std::lock_guard<std::mutex> l(m_);
        current_ = s;
    }
    std::string Current()
    {
        std::lock_guard<std::mutex> l(m_);
        return current_;
    }

private:
    std::mutex m_;
    std::string current_;
};

// Every manifest file is on disk with its md5 - except what is not ours to hold fixed: the four user markers (spec 5.10)
// and DW\dlvr_pp.mth (pakgen rewrites it at every launch, INV-592); a dlss-only entry counts only if present.
bool FilesIntact(const fs::path& game, const std::vector<ManifestEntry>& m);

// Executes the actions (setup_plan.h order), md5-checking each copied file, then re-checks every copied file 5 s later.
// Never stops silently: the Outcome names the first failed action (with its reason) or every file that vanished.
Outcome Execute(const std::vector<Action>& acts, const fs::path& game, const fs::path& docs, Payload& payload,
                Progress* progress);

}  // namespace dlvr::setup
