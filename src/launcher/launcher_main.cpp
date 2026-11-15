// The Dying Light VR launcher (spec docs/superpowers/specs/2026-11-15-launcher-design.md; plan
// docs/superpowers/plans/2026-11-15-launcher.md). Supersedes the setup program. This file only reads the command line
// and starts the window; every decision is in launcher_*.h.
//
//   DyingLightVR_Launcher.exe                                 the window
//   --release <dir>                                           a local release folder in place of GitHub
//   --game-dir <dir>                                          skip discovery
//   --shot <state> <png> [--scale <s>] [--work <w>x<h>]       render one state to a PNG (states: blank, play, vr_comfort,
//                                                              config, controls, support)
#include <windows.h>
#include <objbase.h>

#include <cwchar>
#include <filesystem>
#include <string>

#include "launcher_app.h"
#include "launcher_ui.h"
#include "launcher_update.h"

// --auto "game=<dir>;release=<dir>;action=<install|update|repair|flat|vr|remove|logs|rollback>" - window-less, for the
// end-to-end test on a FAKE game folder. The result goes to launcher.log; exit 0 = ok.
static int RunAuto(const std::wstring& spec)
{
    std::wstring game, release, action;
    size_t p = 0;
    while (p <= spec.size()) {
        size_t e = spec.find(L';', p);
        if (e == std::wstring::npos) { e = spec.size(); }
        const std::wstring kv = spec.substr(p, e - p);
        const size_t eq = kv.find(L'=');
        if (eq != std::wstring::npos) {
            const std::wstring k = kv.substr(0, eq), v = kv.substr(eq + 1);
            if (k == L"game") { game = v; } else if (k == L"release") { release = v; } else if (k == L"action") { action = v; }
        }
        p = e + 1;
    }
    if (game.empty()) { dlvr::launcher::Log("--auto: refused - game=<dir> is required (never discovered: tests use a fake folder)"); return 2; }
    dlvr::launcher::LauncherApp app;
    app.Init(game, release);
    dlvr::launcher::Result r;
    std::filesystem::path zip;
    if (action == L"install") { r = app.Install(); }
    else if (action == L"update") { r = app.Update(); }
    else if (action == L"repair") { r = app.Repair(); }
    else if (action == L"flat") { r = app.ToFlat(); }
    else if (action == L"vr") { r = app.ToVr(); }
    else if (action == L"remove") { r = app.Uninstall(); }
    else if (action == L"logs") { r = app.CollectLogs(&zip); }
    else if (action == L"rollback") { r = app.RollBack(); }
    else if (action == L"selfupdate") {   // Update, then swap in the release's launcher (the new one opens its window)
        r = app.Update();
        if (r.ok) {
            bool restarting = false;
            const dlvr::launcher::Result s = app.SelfUpdate(&restarting);
            r = {s.ok, r.message + " | self-update: " + (s.message.empty() ? std::string("not needed") : s.message) +
                           (restarting ? " [restarting]" : "")};
        }
    }
    else { dlvr::launcher::Log("--auto: unknown action"); return 2; }
    dlvr::launcher::Log(std::string("--auto result: ") + (r.ok ? "OK - " : "FAILED - ") + r.message);
    return r.ok ? 0 : 1;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    dlvr::launcher::UiArgs args;
    std::wstring shotState, shotPng, autoSpec;
    float scale = 1.0f;
    int workW = 2560, workH = 1400;
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--release" && i + 1 < argc) {
            args.releaseDir = argv[++i];
        } else if (a == L"--game-dir" && i + 1 < argc) {
            args.gameDir = argv[++i];
        } else if (a == L"--shot" && i + 2 < argc) {
            shotState = argv[++i];
            shotPng = argv[++i];
        } else if (a == L"--after-update") {
            args.afterUpdate = true;
        } else if (a == L"--auto" && i + 1 < argc) {
            autoSpec = argv[++i];
        } else if (a == L"--scale" && i + 1 < argc) {
            scale = static_cast<float>(_wtof(argv[++i]));
        } else if (a == L"--work" && i + 1 < argc) {
            if (swscanf_s(argv[++i], L"%dx%d", &workW, &workH) != 2) { workW = 2560; workH = 1400; }
        } else {
            dlvr::launcher::Log("command line: unknown argument ignored");
        }
    }
    if (argv) { LocalFree(argv); }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int rc = 0;
    {   // opened directly from a release folder: that folder is the release (launcher_update.h ReleaseDirFor)
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const std::filesystem::path dir = std::filesystem::path(exe).parent_path();
        std::error_code ec;
        args.releaseDir = dlvr::launcher::ReleaseDirFor(args.releaseDir, dir.wstring(), std::filesystem::exists(dir / L"release.txt", ec));
    }
    if (!autoSpec.empty()) {
        rc = RunAuto(autoSpec);
    } else if (!shotState.empty()) {
        std::string st;
        for (wchar_t c : shotState) { st += static_cast<char>(c); }
        rc = dlvr::launcher::RenderShot(args, st, shotPng, scale, workW, workH);
    } else {
        rc = dlvr::launcher::RunUi(args);
    }
    CoUninitialize();
    return rc;
}
