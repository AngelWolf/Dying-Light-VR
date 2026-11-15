// The setup program's decisions: which questions to ask, and the ordered list of file actions for an install,
// update or removal (docs/superpowers/specs/2026-10-17-installer-design.md 5.2, 5.7, 5.8, 5.9, 5.10). Pure and
// header-only - tested by src/src/tests/setup_plan_test.cpp. setup_main.cpp only EXECUTES what these return.
//
// Jay 2026-10-17: keep or reset ("ask the user if they'd like to keep them or not"); on reset, keep the arm calibration
// or not; on keep, "Only ask if there's been new options added"; video.scr "Overwrite"; remove = "Remove the mod files &
// tell them to verify". The rest is the spec's 5.10 calls (user markers kept on Keep, caches kept on Reset, the DLSS dll
// only for DLSS).
#pragma once

#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "setup_gpu.h"
#include "setup_manifest.h"
#include "setup_settings.h"
#include "setup_videoscr.h"

namespace dlvr::setup {

// The video.scr action's path: not under the game folder - the executor maps it to
// Documents\DyingLight\out\settings\video.scr.
inline const char* kVideoScrPath = "<docs>/video.scr";

struct DetectedState {
    bool modPresent = false;            // LibOVRRT64_1.dll or dlvr\ in the game folder
    bool haveOvr2xr = false;
    std::string ovr2xrText;             // dlvr\ovr2xr.txt as found
    bool haveWritten = false;
    std::string writtenText;            // dlvr\setup_settings_written.txt - what the last Setup wrote (keep-reset ADDENDUM 1)
    std::string aaMethodText;           // dlvr\aa_method.txt as found ("" = absent)
    std::string dlssQualityText;        // dlvr\dlss_quality.txt as found ("" = absent)
    std::set<std::string> present;      // game-relative paths found, lower-case, forward slashes
    bool nvidiaGpu = false;             // the PC's main GPU is NVIDIA: nvngx_dlss.dll always goes on (spec 5.10)
};

// A VC++ runtime version (the registry's Major / Minor / Bld, or the bundled vc_redist's file version).
struct VcVersion {
    unsigned major, minor, bld;
};

// The installed runtime is older than the bundled one (an unreadable registry reads as 0.0.0 = older): an old 2015/2017
// runtime also sets Installed=1 but lacks files the VS 2022 DLLs need (review finding 4).
inline bool VersionLess(const VcVersion& a, const VcVersion& b)
{
    if (a.major != b.major) { return a.major < b.major; }
    if (a.minor != b.minor) { return a.minor < b.minor; }
    return a.bld < b.bld;
}

struct Answers {
    bool keep = false;          // keep the old settings (only meaningful when the mod is present)
    bool keepArmFit = true;     // on reset: keep dlvr\arm_fit.txt
    AaWord method = AaWord::Fsr3;
    int quality = 1;
    bool snap = true;
    bool follow = true;         // comfort_body_follows_head (SPEC-BODY-FOLLOWS-HEAD-2026-10-18; BETA 2026-10-19, default ON)
    bool afw = false;           // afw_enable - Jay 2026-10-18: an installer switch, default OFF
    bool hudPanel = true;
    bool haveGraphics = false;  // each: the user answered that question this run
    bool haveTurn = false;
    bool haveFollow = false;
    bool haveAfw = false;
    bool haveHud = false;
};

// Order matters: PlanInstall emits every RunVcRedist, then Copy, then WriteText, then Delete.
enum class ActKind { RunVcRedist = 0, Copy = 1, WriteText = 2, Delete = 3 };

struct Action {
    ActKind kind;
    std::string relPath;   // game-relative, forward slashes; kVideoScrPath for video.scr; "dlvr/" = the whole folder
    std::string text;      // WriteText content
    std::string md5;       // Copy: the manifest md5 the copied file must have
    bool afterRecheck = false;   // Execute runs it only after the 5 s re-check passed (the install record)
};

// The install record (setup_record.h). Deleted as the FIRST action of every install, reset and remove, and written
// only after the 5 s re-check: a failed or interrupted run never leaves a matching record (review finding 1).
inline const char* kRecordPath = "dlvr/setup_installed.txt";

struct Outcome {
    bool ok;
    std::string failedPath;   // the first failed action's path
};

namespace detail {

inline std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

inline std::string BaseName(const std::string& p)
{
    const size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

// The four markers the in-game overlay writes - the USER's, never overwritten on Keep.
inline bool IsUserMarker(const std::string& lowerPath)
{
    return lowerPath == "dlvr/aa_method.txt" || lowerPath == "dlvr/dlss_quality.txt" ||
           lowerPath == "dlvr/dlss_preset.txt" || lowerPath == "dlvr/dlss_sharpness.txt";
}

// Never shipped (tools/make_nv_pkg_20261017.py FORBIDDEN, by file name). BETA: hud_layer.txt SHIPS (Jay 2026-10-19: "add the
// HUD layer marker for the beta") - the AFW HUD layer then exists whatever afw_enable is at launch (BETA-FINDINGS-2026-10-19).
// BETA: extra_args.txt SHIPS too, holding "-nologos" (Jay 2026-10-20: "Ship -nologos in extra_args.txt for BETA") - the game
// skips its intro / press-start / save-icon screens, as on Jay's own install (BETA-FINDINGS-2026-10-19 10a).
inline bool IsForbidden(const std::string& relPath)
{
    static const char* kForbidden[] = {
        "enable_submit_drawn_image.txt", "eye_res.txt", "hmd_cache.txt",
        "steam_appid.txt", "lp_camera_cfg.txt", "datadlc17_2.pak", "vr_video.scr", "jitter_watch.txt",
        "diag_rendlayer.txt", "diag_lumread.txt", "thread_census.txt", "chainprobe.txt", "dlss_gputime.txt",
        "dlss_frametime.txt", "dlss_ui_probe.txt", "rl_import_test.txt", "fs_source_probe.txt", "dlvr_rl_sizes.mth",
        "dlvr_loose_probe.txt", "aa_native.txt", "aa_taa.txt", "pak_lock_test.txt", "dlvr_pp.mth.tmp", "arm_fit.txt",
        "gpu_frame.txt"};
    const std::string b = Lower(BaseName(relPath));
    for (const char* f : kForbidden) {
        if (b == f) {
            return true;
        }
    }
    return false;
}

inline bool KeptMethodIsDlss(std::string aaMethodText)
{
    if (aaMethodText.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        aaMethodText.erase(0, 3);
    }
    std::string w;
    for (char c : aaMethodText) {
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            if (!w.empty()) { break; }
            continue;
        }
        w += c;
    }
    return Lower(w) == "dlss";
}

}  // namespace detail

// "graphics", "turn", "follow", "afw", "hud" - all five on a fresh install or a reset; on Keep only those whose answer is missing.
inline std::vector<std::string> QuestionsToAsk(const DetectedState& s, bool keep)
{
    if (!s.modPresent || !keep) {
        return {"graphics", "turn", "follow", "afw", "hud"};
    }
    std::vector<std::string> q;
    if (s.present.count("dlvr/aa_method.txt") == 0) {
        q.push_back("graphics");
    }
    if (!s.haveOvr2xr || !HasKey(s.ovr2xrText, "comfort_turn_mode")) {
        q.push_back("turn");
    }
    if (!s.haveOvr2xr || !HasKey(s.ovr2xrText, "comfort_body_follows_head")) {
        q.push_back("follow");
    }
    if (!s.haveOvr2xr || !HasKey(s.ovr2xrText, "afw_enable")) {
        q.push_back("afw");
    }
    if (!s.haveOvr2xr || !HasKey(s.ovr2xrText, "ui_hud_panel")) {
        q.push_back("hud");
    }
    return q;
}

// What the existing install's files say (spec 8.5: the dropdowns show it). haveX = a value was found and read.
struct OnDisk {
    bool haveAa = false;
    AaWord method = AaWord::Fsr3;
    int quality = 1;
    bool haveTurn = false;
    bool snap = true;
    bool haveFollow = false;
    bool follow = true;
    bool haveAfw = false;
    bool afw = false;
    bool haveHud = false;
    bool hudPanel = true;
};

namespace detail {

inline std::string FirstWord(std::string t)
{
    if (t.compare(0, 3, "\xEF\xBB\xBF") == 0) { t.erase(0, 3); }
    std::string w;
    for (char c : t) {
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            if (!w.empty()) { break; }
            continue;
        }
        w += c;
    }
    return Lower(w);
}

// The value of a non-comment key= line, "" if absent.
inline std::string KeyValue(const std::string& text, const std::string& key)
{
    std::string t = text;
    if (t.compare(0, 3, "\xEF\xBB\xBF") == 0) { t.erase(0, 3); }
    size_t pos = 0;
    while (pos < t.size()) {
        size_t e = t.find('\n', pos);
        if (e == std::string::npos) { e = t.size(); }
        std::string line = t.substr(pos, e - pos);
        pos = e + 1;
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        if (!line.empty() && line[0] != '#' && Lower(line.substr(0, key.size())) == Lower(key) &&
            line.size() > key.size() && line[key.size()] == '=') {
            return line.substr(key.size() + 1);
        }
    }
    return "";
}

}  // namespace detail

inline OnDisk ReadOnDisk(const DetectedState& s)
{
    OnDisk d;
    const std::string m = detail::FirstWord(s.aaMethodText);
    const std::string q = detail::FirstWord(s.dlssQualityText);
    const bool qOk = q.size() == 1 && q[0] >= '0' && q[0] <= '3';
    if ((m == "dlss" || m == "fsr3" || m == "fsr4") && qOk) {
        d.haveAa = true;
        d.method = m == "dlss" ? AaWord::Dlss : m == "fsr4" ? AaWord::Fsr4 : AaWord::Fsr3;
        d.quality = q[0] - '0';
    }
    if (s.haveOvr2xr) {
        const std::string t = detail::KeyValue(s.ovr2xrText, "comfort_turn_mode");
        if (t == "0" || t == "1") { d.haveTurn = true; d.snap = (t == "1"); }
        const std::string f = detail::KeyValue(s.ovr2xrText, "comfort_body_follows_head");
        if (f == "0" || f == "1") { d.haveFollow = true; d.follow = (f == "1"); }
        const std::string w = detail::KeyValue(s.ovr2xrText, "afw_enable");
        if (w == "0" || w == "1") { d.haveAfw = true; d.afw = (w == "1"); }
        const std::string h = detail::KeyValue(s.ovr2xrText, "ui_hud_panel");
        if (h == "0" || h == "1") { d.haveHud = true; d.hudPanel = (h == "1"); }
    }
    return d;
}

// The window's dropdown values -> Answers (spec 8.5): on a reset or fresh install every answer is written; on keep, only
// a value the file lacks or the user changed.
inline Answers AnswersFromUi(const OnDisk& disk, bool keep, AaWord m, int q, bool snap, bool follow, bool afw, bool panel)
{
    Answers a;
    a.keep = keep;
    a.method = m;
    a.quality = q;
    a.snap = snap;
    a.follow = follow;
    a.afw = afw;
    a.hudPanel = panel;
    a.haveGraphics = !keep || !disk.haveAa || disk.method != m || disk.quality != q;
    a.haveTurn = !keep || !disk.haveTurn || disk.snap != snap;
    a.haveFollow = !keep || !disk.haveFollow || disk.follow != follow;
    a.haveAfw = !keep || !disk.haveAfw || disk.afw != afw;
    a.haveHud = !keep || !disk.haveHud || disk.hudPanel != panel;
    return a;
}

// Throws std::runtime_error if the manifest names a never-shipped file. recordText non-empty = write
// dlvr\setup_installed.txt as the last write (spec 8.5). `resets` (the packager's old-default table) is applied on a KEEP
// install only, before the answered keys, and each line it moves is reported in `moved`
// (docs/superpowers/specs/2026-10-25-keep-settings-old-default-reset-design.md).
inline std::vector<Action> PlanInstall(const DetectedState& s, const Answers& a, const std::vector<ManifestEntry>& m,
                                       const std::vector<std::string>& retiredKeys, bool vcRedistNeeded,
                                       const std::string& recordText = "", const std::vector<ResetRow>& resets = {},
                                       std::vector<Moved>* moved = nullptr)
{
    const bool keep = s.modPresent && a.keep;
    // The DLSS dll goes on an NVIDIA card whatever the method (the game's AA tab can switch to DLSS later), and on any
    // install whose chosen / kept method is DLSS.
    const bool dlss = s.nvidiaGpu ||
                      (a.haveGraphics ? (a.method == AaWord::Dlss) : (keep && detail::KeptMethodIsDlss(s.aaMethodText)));
    std::vector<Action> out;

    if (!recordText.empty()) {   // the old record goes FIRST: from here until the re-check passes, no record matches
        out.push_back({ActKind::Delete, kRecordPath, "", ""});
    }
    if (vcRedistNeeded) {
        out.push_back({ActKind::RunVcRedist, "vc_redist.x64.exe", "", ""});
    }
    for (const auto& e : m) {
        if (detail::IsForbidden(e.relPath)) {
            throw std::runtime_error("the manifest lists a file that is never shipped: " + e.relPath);
        }
        if (e.dlssOnly && !dlss) {
            continue;
        }
        const std::string low = detail::Lower(e.relPath);
        if (keep && detail::IsUserMarker(low) && s.present.count(low)) {
            continue;
        }
        out.push_back({ActKind::Copy, e.relPath, "", e.md5});
    }

    std::vector<KeyValue> answered;
    if (a.haveTurn) { answered.push_back({"comfort_turn_mode", a.snap ? "1" : "0"}); }
    if (a.haveFollow) { answered.push_back({"comfort_body_follows_head", a.follow ? "1" : "0"}); }
    if (a.haveAfw) { answered.push_back({"afw_enable", a.afw ? "1" : "0"}); }
    if (a.haveHud) { answered.push_back({"ui_hud_panel", a.hudPanel ? "1" : "0"}); }
    std::string settings;
    if (keep && s.haveOvr2xr) {
        const std::string reset =
            ApplyOldDefaultResets(s.ovr2xrText, resets,
                                  {"comfort_turn_mode", "comfort_body_follows_head", "afw_enable", "ui_hud_panel"}, moved,
                                  s.haveWritten ? &s.writtenText : nullptr);
        settings = MergeSettings(reset, answered, retiredKeys);
    } else {
        settings = FreshSettings(answered);
    }
    out.push_back({ActKind::WriteText, "dlvr/ovr2xr.txt", settings, ""});
    out.push_back({ActKind::WriteText, kWrittenCopyPath, settings, ""});   // the next keep install's "changed since" reference
    if (a.haveGraphics) {
        out.push_back({ActKind::WriteText, "dlvr/aa_method.txt", std::string(AaWordText(a.method)) + "\r\n", ""});
        out.push_back({ActKind::WriteText, "dlvr/dlss_quality.txt", std::to_string(a.quality) + "\r\n", ""});
    }
    out.push_back({ActKind::WriteText, kVideoScrPath, VideoScrText(), ""});
    if (!recordText.empty()) {
        out.push_back({ActKind::WriteText, kRecordPath, recordText, "", true});   // only after the re-check passed
    }

    auto del = [&](const char* p) {
        if (s.present.count(p)) {
            out.push_back({ActKind::Delete, p, "", ""});
        }
    };
    if (!keep) {
        del("dlvr/dlss_sharpness.txt");
        del("dlvr/gpu_frame.txt");
        if (!a.keepArmFit) {
            del("dlvr/arm_fit.txt");
        }
    }
    if (!dlss) {
        del("nvngx_dlss.dll");
    }
    return out;
}

// The spec 5.8 list (README section 11) - each file that is present - then the whole dlvr folder.
inline std::vector<Action> PlanRemove(const DetectedState& s)
{
    static const char* kModFiles[] = {"LibOVRRT64_1.dll", "xinput1_3.dll", "nvngx_dlss.dll",
                                      "amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_upscaler_dx12.dll",
                                      "amd_fidelityfx_license.md", "DW/Data3.pak", "DW/dlvr_pp.mth",
                                      "DW_DLC24/DataDLC24_2.pak"};
    std::vector<Action> out;
    out.push_back({ActKind::Delete, kRecordPath, "", ""});   // first: an interrupted remove never reads as Installed
    for (const char* f : kModFiles) {
        if (s.present.count(detail::Lower(f))) {
            out.push_back({ActKind::Delete, f, "", ""});
        }
    }
    out.push_back({ActKind::Delete, "dlvr/", "", ""});
    return out;
}

// ok only if every action succeeded; a short result list counts the missing ones as failed.
inline Outcome Summarise(const std::vector<Action>& actions, const std::vector<bool>& actionOk)
{
    for (size_t i = 0; i < actions.size(); ++i) {
        if (i >= actionOk.size() || !actionOk[i]) {
            return {false, actions[i].relPath};
        }
    }
    return {true, ""};
}

}  // namespace dlvr::setup
