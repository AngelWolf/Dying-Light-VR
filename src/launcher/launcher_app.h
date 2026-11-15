// The launcher's actions (spec docs/superpowers/specs/2026-11-15-launcher-design.md): find the game, read the release and
// the install, then Install / Update / Repair / Uninstall / Launch VR / Launch Flat / Collect logs / Roll back / save
// settings. Every decision is a tested header (launcher_*.h); this class gathers the facts, runs the plans through the
// real executor (RealOps, Task 6) and reports one line. The window (launcher_ui.cpp) and --auto (launcher_main.cpp) both
// drive it. NOT thread-safe: the window runs one action at a time on its worker and copies Snapshot() under its own lock.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "launcher_mode.h"
#include "launcher_release.h"
#include "launcher_schema.h"
#include "launcher_view.h"
#include "setup_gpu.h"
#include "setup_plan.h"
#include "setup_store.h"

namespace dlvr::launcher {

namespace fs = std::filesystem;

inline const char* kInstalledReleaseCopy = "dlvr/launcher_release.txt";   // the installed release's manifest

struct Snapshot {
    fs::path game;
    setup::Store store = setup::Store::Unknown;
    std::string storeNote;              // the "untested" notice / the Program Files refusal
    std::vector<fs::path> candidates;
    LauncherView view;
    std::string releaseName, releaseNotes, releaseWhy;   // the release we can install from ("" = none)
    std::string installedName;
    std::vector<std::string> differing;
    std::vector<SchemaRow> schema;
    bool schemaOk = false, schemaMatchesLayer = false;
    std::string ovr2xr;                 // dlvr/ovr2xr.txt as on disk
    setup::AaWord aaMethod = setup::AaWord::Fsr3;
    int aaQuality = 1;
    bool haveAa = false;
    std::vector<setup::AaOption> aaOptions;
    int aaDefault = 0;
    std::string gpuName, vdAdvice;
    bool havePrevious = false;
};

struct Result {
    bool ok = false;
    std::string message;
};

class LauncherApp {
public:
    // gameDir empty = discovery (the first candidate); releaseDir empty = GitHub (kRepoUrl) or nothing
    void Init(const fs::path& gameDir, const std::wstring& releaseDir);
    void SelectGame(const fs::path& game);
    void Refresh();
    const Snapshot& Snap() const { return s_; }

    Result Install();
    Result Update();
    Result Repair();
    Result Uninstall();
    Result RollBack();
    Result ToFlat();
    Result ToVr();
    Result Launch();                    // the store's command; call after ToFlat / ToVr succeeded
    Result SaveSettings(const std::vector<KeyValue>& edits);
    Result SaveGraphics(setup::AaWord method, int quality);
    Result CollectLogs(fs::path* zipOut);
    Result CheckForUpdates();           // GitHub only; "not set up yet" while kRepoUrl is empty
    // After a successful Update: swap in the release's launcher exe if it differs (launcher_update.h self-update steps).
    // *restarting = true: the new launcher is up and signalled - this process must exit now.
    Result SelfUpdate(bool* restarting);
    static void CleanupOldExe();        // a left-over DyingLightVR_Launcher.old.exe is deleted (OnStartStep)

private:
    bool Stage(std::string* why);       // extract + verify the release into a temp folder (Review Focus 5)
    Result RunPlan(const std::vector<LAction>& plan, const char* what, bool recheck);
    std::string RecordText(const Release& r, Mode m) const;

    Snapshot s_;
    setup::DetectedState detected_;
    bool nvidia_ = false;
    std::wstring releaseDir_;
    fs::path staged_;
    Release release_, installed_;
    bool haveRelease_ = false, haveInstalled_ = false, staged_ok_ = false;
    std::string epicApp_, gogId_;
    std::wstring galaxyExe_;
};

std::string AaLabel(const setup::AaOption& o);   // "DLSS Quality", "DLAA", "FSR 3.1 Native AA" (S02 item 5)

}  // namespace dlvr::launcher
