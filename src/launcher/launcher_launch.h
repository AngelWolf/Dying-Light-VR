// How the launcher starts the game, per store (spec docs/superpowers/specs/2026-11-15-launcher-design.md sections 8 and
// 12.5). Steam through its URI; Epic through the Epic launcher's URI - the exe alone cannot be used, because the game takes
// the Epic user name from -epicusername, which only the Epic launcher supplies (src_beta/src/bootstrap/dllmain.cpp:548);
// GOG through Galaxy when installed, else the exe (GOG builds carry no launcher DRM). The Epic and GOG commands follow those
// launchers' conventions and are UNTESTED (no Epic / GOG install to check). Pure and header-only - tested by
// src_beta/src/tests/launcher_launch_test.cpp.
#pragma once

#include <string>

#include "launcher_json.h"
#include "setup_store.h"

namespace dlvr::launcher {

struct LaunchCmd {
    std::wstring file;      // empty = cannot launch; `why` says what to do
    std::wstring args;
    bool viaShell = false;  // a URI: ShellExecuteW; else CreateProcessW(file, args)
    std::string why;
};

namespace detail {

inline std::wstring Widen(const std::string& s)
{
    std::wstring w;
    for (unsigned char c : s) { w.push_back(static_cast<wchar_t>(c)); }   // AppName / ids are ASCII
    return w;
}

inline std::wstring JoinExe(const std::wstring& dir)
{
    if (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) { return dir + L"DyingLightGame.exe"; }
    return dir + L"\\DyingLightGame.exe";
}

}  // namespace detail

inline LaunchCmd LaunchFor(setup::Store store, const std::wstring& gameDir, const std::string& epicAppName,
                           const std::string& gogId, const std::wstring& galaxyExe)
{
    LaunchCmd c;
    switch (store) {
        case setup::Store::Steam:
            c.file = L"steam://rungameid/239140";
            c.viaShell = true;
            return c;
        case setup::Store::Epic:
            if (epicAppName.empty()) {
                c.why = "The Epic Games launcher's entry for Dying Light was not found, so the launcher cannot start it. Start "
                        "the game from the Epic Games launcher.";
                return c;
            }
            c.file = L"com.epicgames.launcher://apps/" + detail::Widen(epicAppName) + L"?action=launch&silent=true";
            c.viaShell = true;
            return c;
        case setup::Store::Gog:
            if (!galaxyExe.empty() && !gogId.empty()) {
                c.file = galaxyExe;
                c.args = L"/command=runGame /gameId=" + detail::Widen(gogId) + L" /path=\"" + gameDir + L"\"";
                return c;
            }
            break;
        case setup::Store::Unknown:
            break;
    }
    c.file = detail::JoinExe(gameDir);
    return c;
}

// The main game's AppName from one Epic manifest (*.item). A DLC manifest (AppName != MainGameAppName), another game, or
// malformed JSON is false.
inline bool EpicAppName(const std::string& manifestJson, std::string* appName)
{
    std::string j = manifestJson;
    if (j.compare(0, 3, "\xEF\xBB\xBF") == 0) { j.erase(0, 3); }
    json::Value v;
    std::string why;
    if (!json::Parse(j, &v, &why)) { return false; }
    const std::string name = setup::detail::LowerAscii(v.Get("DisplayName").Str());
    const std::string app = v.Get("AppName").Str();
    const std::string main = v.Get("MainGameAppName").Str();
    if (name.find("dying light") == std::string::npos || app.empty()) { return false; }
    if (!main.empty() && main != app) { return false; }
    *appName = app;
    return true;
}

// "goggame-<digits>.info" -> the digits.
inline bool GogIdFromInfoName(const std::string& fileName, std::string* id)
{
    const std::string pre = "goggame-", suf = ".info";
    if (fileName.size() <= pre.size() + suf.size() || fileName.compare(0, pre.size(), pre) != 0 ||
        fileName.compare(fileName.size() - suf.size(), suf.size(), suf) != 0) {
        return false;
    }
    const std::string d = fileName.substr(pre.size(), fileName.size() - pre.size() - suf.size());
    for (char c : d) {
        if (c < '0' || c > '9') { return false; }
    }
    *id = d;
    return true;
}

}  // namespace dlvr::launcher
