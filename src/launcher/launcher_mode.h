// The VR / flat switch and the launcher's executor (spec docs/superpowers/specs/2026-11-15-launcher-design.md section 8).
// Jay 2026-11-15: the flat button "would disable the mod until turned back on by 'Launching in VR'"; "Move everything".
// Pure and header-only over an IFileOps interface - tested with an in-memory fake by
// src_beta/src/tests/launcher_mode_test.cpp; the real one is launcher_sys.cpp.
//
// Every move is md5-checked (before == after). The game is re-checked before EVERY action; a failure stops at once,
// reverses the completed moves in reverse order and names the file and the OS's own error (the setup's Delete lost it:
// docs/audit/S02_setup_PARKED.md item 2).
#pragma once

#include <set>
#include <string>
#include <vector>

#include "launcher_plan.h"

namespace dlvr::launcher {

// Paths are game-relative with forward slashes; "<docs>/video.scr" = Documents\DyingLight\out\settings\video.scr
// (setup_plan.h kVideoScrPath). A path ending in '/' is a folder (Remove deletes the tree).
struct IFileOps {
    virtual ~IFileOps() = default;
    virtual bool GameRunning() = 0;
    virtual bool CopyFromRelease(const std::string& releasePath, const std::string& to, std::string* why) = 0;
    virtual bool Copy(const std::string& from, const std::string& to, std::string* why) = 0;
    virtual bool Move(const std::string& from, const std::string& to, std::string* why) = 0;   // replaces `to`
    virtual bool Write(const std::string& path, const std::string& text, std::string* why) = 0;
    virtual bool Remove(const std::string& path, std::string* why) = 0;   // an absent path is success
    virtual std::string Md5(const std::string& path) = 0;                  // "" = absent / unreadable
    virtual bool RunVcRedist(std::string* why) = 0;
};

struct ExecResult {
    bool ok = true;
    std::string failedPath;
    std::string why;
    std::vector<std::string> rolledBack;   // the moves reversed, newest first
};

// video.scr without its ACTIVE OculusEnabled() line(s): a line that, trimmed, is exactly "OculusEnabled()" - the
// bootstrap's own test (dllmain.cpp:1711-1721). The "!OculusEnabled()" reference line and every other byte stay.
inline std::string RemoveOculusEnabled(const std::string& scr)
{
    std::string out;
    size_t p = 0;
    while (p < scr.size()) {
        size_t e = scr.find('\n', p);
        const size_t next = e == std::string::npos ? scr.size() : e + 1;
        std::string body = scr.substr(p, (e == std::string::npos ? scr.size() : e) - p);
        if (!body.empty() && body.back() == '\r') { body.pop_back(); }
        const size_t a = body.find_first_not_of(" \t");
        const size_t b = body.find_last_not_of(" \t");
        const bool active = a != std::string::npos && body.substr(a, b - a + 1) == "OculusEnabled()";
        if (!active) { out += scr.substr(p, next - p); }
        p = next;
    }
    return out;
}

namespace detail {

// The mod's own data files in DW\ (not in a release; written at runtime or by pakgen) - moved with the mod (Jay: "Move
// everything").
inline const char* const kModDwData[] = {"DW/dlvr_binds.scr", "DW/dlvr_rl_sizes.mth", "DW/dlvr_pp.mth"};

inline bool Has(const std::set<std::string>& presentLower, const std::string& p) { return presentLower.count(LowerPath(p)) > 0; }

struct Undo {
    std::string from, to;   // the move that was made; undoing moves `to` back to `from`
};

inline bool RunOne(const LAction& a, IFileOps& ops, std::vector<Undo>* undo, std::string* why)
{
    switch (a.kind) {
        case LAct::RunVcRedist:
            return ops.RunVcRedist(why);
        case LAct::Copy: {
            if (!ops.CopyFromRelease(a.relPath, a.dest, why)) { return false; }
            const std::string got = ops.Md5(a.dest);
            if (!a.md5.empty() && got != a.md5) {
                *why = "md5 after copying is " + (got.empty() ? std::string("unreadable") : got) + ", the release says " + a.md5;
                return false;
            }
            return true;
        }
        case LAct::SaveStock: {
            const std::string before = ops.Md5(a.relPath);
            if (!ops.Copy(a.relPath, a.dest, why)) { return false; }
            if (ops.Md5(a.dest) != before || before.empty()) { *why = "md5 of the saved copy differs from the game's file"; return false; }
            return true;
        }
        case LAct::Move: {
            const std::string before = ops.Md5(a.relPath);
            if (before.empty()) { *why = "the file to move is missing or unreadable"; return false; }
            if (!ops.Move(a.relPath, a.dest, why)) { return false; }
            undo->push_back({a.relPath, a.dest});
            const std::string after = ops.Md5(a.dest);
            if (after != before) { *why = "md5 changed in the move (" + before + " -> " + after + ")"; return false; }
            return true;
        }
        case LAct::WriteText:
            return ops.Write(a.relPath, a.text, why);
        case LAct::Delete:
            return ops.Remove(a.relPath, why);
    }
    *why = "unknown action";
    return false;
}

inline ExecResult Run(const std::vector<LAction>& actions, IFileOps& ops, bool afterRecheck)
{
    ExecResult r;
    std::vector<Undo> undo;
    for (const auto& a : actions) {
        if (a.afterRecheck != afterRecheck) { continue; }
        std::string why;
        bool good;
        if (ops.GameRunning()) {
            good = false;
            why = "DyingLightGame.exe is running - close the game; nothing more was changed";
        } else {
            good = RunOne(a, ops, &undo, &why);
        }
        if (good) { continue; }
        r.ok = false;
        r.failedPath = a.relPath;
        r.why = why;
        for (auto it = undo.rbegin(); it != undo.rend(); ++it) {
            std::string w2;
            if (ops.Move(it->to, it->from, &w2)) {
                r.rolledBack.push_back(it->from);
            } else {
                r.why += "; could NOT put back " + it->from + " (" + w2 + ")";
            }
        }
        return r;
    }
    return r;
}

}  // namespace detail

// Every action not marked afterRecheck, in order.
inline ExecResult ExecuteWithRollback(const std::vector<LAction>& actions, IFileOps& ops) { return detail::Run(actions, ops, false); }

// After the 5 s wait: every copied file must still hold its md5 and every moved file must still exist (antivirus
// quarantine). Returns the paths that failed, by the action's relPath.
inline std::vector<std::string> Recheck(const std::vector<LAction>& actions, IFileOps& ops)
{
    std::vector<std::string> bad;
    for (const auto& a : actions) {
        if (a.afterRecheck) { continue; }
        if (a.kind == LAct::Copy && !a.md5.empty() && ops.Md5(a.dest) != a.md5) { bad.push_back(a.relPath); }
        if ((a.kind == LAct::Move || a.kind == LAct::SaveStock) && ops.Md5(a.dest).empty()) { bad.push_back(a.relPath); }
    }
    return bad;
}

// The actions marked afterRecheck (the record), once Recheck came back empty.
inline ExecResult ExecuteAfterRecheck(const std::vector<LAction>& actions, IFileOps& ops) { return detail::Run(actions, ops, true); }

// VR -> flat. presentLower = the game folder's files (lower-case); stockPaths = the files under dlvr/stock/ (relative to it).
inline std::vector<LAction> PlanToFlat(const Release& r, const std::set<std::string>& presentLower,
                                       const std::vector<std::string>& stockPaths, const std::string& videoScrText,
                                       const std::string& recordText)
{
    std::vector<LAction> out;
    std::set<std::string> moved;
    auto out1 = [&](const std::string& p) {
        if (!detail::Has(presentLower, p) || !moved.insert(detail::LowerPath(p)).second) { return; }
        out.push_back({LAct::Move, p, std::string(kFlatDir) + p, "", "", false});
    };
    for (const auto& f : r.files) {
        if (detail::LowerPath(f.relPath).compare(0, 5, "dlvr/") != 0) { out1(f.relPath); }
    }
    for (const char* p : detail::kModDwData) { out1(p); }
    for (const auto& s : stockPaths) { out.push_back({LAct::Move, std::string(kStockDir) + s, s, "", "", false}); }
    if (!videoScrText.empty()) {
        const std::string cleaned = RemoveOculusEnabled(videoScrText);
        if (cleaned != videoScrText) { out.push_back({LAct::WriteText, setup::kVideoScrPath, "", "", cleaned, false}); }
    }
    out.push_back({LAct::WriteText, kLauncherRecord, "", "", recordText, false});
    return out;
}

// flat -> VR. flatPaths = the files under dlvr/flat_off/ (relative to it). A game path that holds a file (the stock
// one) is moved to dlvr/stock/ first. video.scr is not written: the bootstrap re-adds OculusEnabled() on attach.
inline std::vector<LAction> PlanToVr(const std::set<std::string>& flatPaths, const std::set<std::string>& presentLower,
                                     const std::string& recordText)
{
    std::vector<LAction> out;
    for (const auto& p : flatPaths) {
        if (detail::Has(presentLower, p)) { out.push_back({LAct::Move, p, std::string(kStockDir) + p, "", "", false}); }
    }
    for (const auto& p : flatPaths) { out.push_back({LAct::Move, std::string(kFlatDir) + p, p, "", "", false}); }
    out.push_back({LAct::WriteText, kLauncherRecord, "", "", recordText, false});
    return out;
}

}  // namespace dlvr::launcher
