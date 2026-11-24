// The launcher's install / repair / remove plans (spec docs/superpowers/specs/2026-11-15-launcher-design.md sections 6,
// 12.2, 12.4). Built on the setup's planner (setup_plan.h, unchanged): the launcher adds the stock-file save, the
// pre-launcher-install refusal, the runtime-written files on removal and its own record. Pure and header-only - tested
// by src_beta/src/tests/launcher_plan_test.cpp; the launcher's executor (launcher_mode.h) runs the lists in order.
#pragma once

#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "fixed_data3_md5.h"
#include "launcher_release.h"
#include "setup_plan.h"

namespace dlvr::launcher {

// Copy: relPath = the file in the release (files.zip), dest = where it goes (game-relative). SaveStock: relPath = the
// game file, dest = its copy under dlvr/stock/. Move: relPath -> dest (dest replaced). md5 = what the result must hold.
enum class LAct { RunVcRedist, Copy, WriteText, Delete, SaveStock, Move };

struct LAction {
    LAct kind;
    std::string relPath;
    std::string dest;
    std::string md5;
    std::string text;
    bool afterRecheck = false;   // only after the 5 s quarantine re-check passed (the record)
};

inline const char* kLauncherRecord = "dlvr/launcher_installed.txt";
inline const char* kStockDir = "dlvr/stock/";
inline const char* kFlatDir = "dlvr/flat_off/";

// Files the mod writes while running (read from the BETA source 2026-11-15, trace docs/traces/20261115_1925_launcher_t5.md).
inline const char* const kRuntimeWritten[] = {
    "DW/dlvr_binds.scr", "DW/dlvr_rl_sizes.mth", "dlvr_bootstrap.log", "dlvr_engine.log", "dlvr_ovr2xr.log",
    "dlvr_armcap.log", "dlvr_swingshadow.log", "dlvr_vehrec.bin", "dlvr_vehroot.bin"};
// ...and by name prefix (the AFW panel captures, dlssflat.cpp:8053 / 8055).
inline const char* const kRuntimePrefixes[] = {"dlvr_hud_panel_", "dlvr_menu_panel_"};
inline const char* kCaptureDir = "dlvr_capture/";

struct InstallInputs {
    setup::DetectedState s;
    setup::Answers answers;
    Release release;
    std::map<std::string, std::string> diskMd5;   // lower-case path -> md5, for release paths already on disk
    bool launcherRecord = false;                  // dlvr/launcher_installed.txt was read
    std::vector<std::string> retiredKeys;
    bool vcNeeded = false;
    std::string recordText;                       // ReleaseRecordText(...)
    std::vector<setup::ResetRow> resets;
};

namespace detail {

inline LAction FromSetup(const setup::Action& a)
{
    switch (a.kind) {
        case setup::ActKind::RunVcRedist: return {LAct::RunVcRedist, a.relPath, "", "", "", false};
        case setup::ActKind::Copy: return {LAct::Copy, a.relPath, a.relPath, a.md5, "", false};
        case setup::ActKind::WriteText: return {LAct::WriteText, a.relPath, "", "", a.text, a.afterRecheck};
        case setup::ActKind::Delete: break;
    }
    return {LAct::Delete, a.relPath, "", "", "", false};
}

// The Data3.pak of every public release: this build's, then each OLDER one (add the old pin here whenever kFixedData3Md5 moves).
// For the fresh-install refusal only - the bootstrap's own verdict stays exactly this build's (fixed_data3_md5.h).
inline bool IsModData3(const std::string& md5)
{
    return md5 == kFixedData3Md5 || md5 == "394248e75c65b83ad2f26151a9b3566a";   // 1.0 - 1.065
}

// Which game-folder paths the scan (launcher_sys.cpp Detect) keeps - lower-case, forward slashes. EVERY folder a release puts a file in must be
// here: Remove, switch to flat and Update's keep-previous only act on what the scan saw (1.07 is the first with DW\Data files).
inline bool ScanWants(const std::string& r)
{
    const auto directlyIn = [&r](const char* dir) {
        const std::string d = dir;
        return r.compare(0, d.size(), d) == 0 && r.find('/', d.size()) == std::string::npos;
    };
    return r.find('/') == std::string::npos || r.compare(0, 5, "dlvr/") == 0 || directlyIn("dw/") || directlyIn("dw/data/") ||
           directlyIn("dw_dlc24/");
}

}  // namespace detail

// Empty + *refusal set = nothing may be done (the reason is for the user, naming what to do).
inline std::vector<LAction> PlanLauncherInstall(const InstallInputs& in, std::string* refusal)
{
    refusal->clear();
    if (in.s.modPresent && !in.launcherRecord) {
        *refusal = "Dying Light VR is already installed here by an older installer or by hand. Press Uninstall, then "
                   "verify the game files in your store (Steam: Properties -> Installed Files -> Verify integrity of game "
                   "files), then install again.";
        return {};
    }
    std::vector<LAction> out;
    out.push_back({LAct::Delete, kLauncherRecord, "", "", "", false});
    if (!in.launcherRecord) {
        // a fresh install onto a verified game: keep every game file ours replaces, BEFORE it is replaced
        for (const auto& f : in.release.files) {
            const auto it = in.diskMd5.find(detail::LowerPath(f.relPath));
            if (it == in.diskMd5.end()) { continue; }
            // checked BEFORE the same-as-ours skip: the release's Data3 IS kFixedData3Md5, so "ours" here means not stock
            if (detail::LowerPath(f.relPath) == "dw/data3.pak" && detail::IsModData3(it->second)) {
                *refusal = "DW\\Data3.pak in the game folder is the mod's, not the game's own. Verify the game files in your "
                           "store, then install again.";
                return {};
            }
            if (it->second == f.md5) { continue; }
            out.push_back({LAct::SaveStock, f.relPath, std::string(kStockDir) + f.relPath, it->second, "", false});
        }
    }
    std::vector<setup::Action> base;
    try {
        base = setup::PlanInstall(in.s, in.answers, in.release.files, in.retiredKeys, in.vcNeeded, "", in.resets);
    } catch (const std::exception& e) {
        *refusal = e.what();
        return {};
    }
    for (const auto& a : base) { out.push_back(detail::FromSetup(a)); }
    out.push_back({LAct::WriteText, kLauncherRecord, "", "", in.recordText, true});
    return out;
}

// differing = MatchInstalled's list. A path not in the release is ignored.
inline std::vector<LAction> PlanRepair(const Release& r, const std::vector<std::string>& differing, bool flatMode)
{
    std::vector<LAction> out;
    for (const auto& d : differing) {
        for (const auto& f : r.files) {
            if (detail::LowerPath(f.relPath) != detail::LowerPath(d)) { continue; }
            out.push_back({LAct::Copy, f.relPath, flatMode ? std::string(kFlatDir) + f.relPath : f.relPath, f.md5, "", false});
        }
    }
    return out;
}

// stockLower = paths saved under dlvr/stock/ (relative to it, lower-case). In VR mode the mod's files are at their game
// paths and each stock file is moved back after ours is deleted; in flat mode the game paths already hold the stock
// files (and the mod's are under dlvr/flat_off/, removed with dlvr/), so no release path is touched.
inline std::vector<LAction> PlanLauncherRemove(const setup::DetectedState& s, const Release& r,
                                               const std::set<std::string>& stockLower, bool flatMode)
{
    std::vector<LAction> out;
    out.push_back({LAct::Delete, kLauncherRecord, "", "", "", false});
    std::set<std::string> done;
    auto del = [&](const std::string& path) {
        const std::string low = detail::LowerPath(path);
        if (s.present.count(low) && done.insert(low).second) { out.push_back({LAct::Delete, path, "", "", "", false}); }
    };
    if (!flatMode) {
        for (const auto& f : r.files) {
            if (detail::LowerPath(f.relPath).compare(0, 5, "dlvr/") != 0) { del(f.relPath); }
        }
        for (const auto& a : setup::PlanRemove(s)) {
            if (a.relPath != setup::kRecordPath && a.relPath != "dlvr/") { del(a.relPath); }
        }
    }
    for (const char* p : kRuntimeWritten) { del(p); }
    for (const auto& p : s.present) {
        if (p.find('/') != std::string::npos) { continue; }
        for (const char* pre : kRuntimePrefixes) {
            if (p.compare(0, std::char_traits<char>::length(pre), pre) == 0) { del(p); }
        }
    }
    out.push_back({LAct::Delete, kCaptureDir, "", "", "", false});
    if (!flatMode) {
        for (const auto& f : r.files) {
            const std::string low = detail::LowerPath(f.relPath);
            if (stockLower.count(low)) {
                out.push_back({LAct::Move, std::string(kStockDir) + f.relPath, f.relPath, "", "", false});
            }
        }
    }
    out.push_back({LAct::Delete, "dlvr/", "", "", "", false});
    return out;
}

}  // namespace dlvr::launcher
