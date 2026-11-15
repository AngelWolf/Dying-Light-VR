// See launcher_app.h.
#include "launcher_app.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <chrono>
#include <cstdio>
#include <set>
#include <thread>

#include "launcher_launch.h"
#include "launcher_logs.h"
#include "launcher_net.h"
#include "launcher_plan.h"
#include "launcher_sys.h"
#include "launcher_ui.h"
#include "launcher_unzip.h"
#include "launcher_update.h"
#include "launcher_zip.h"
#include "setup_sys.h"

#pragma comment(lib, "version.lib")

namespace dlvr::launcher {

namespace {

std::string ReadText(const fs::path& p)
{
    std::string s;
    ReadAllBytes(p, &s);
    return s;
}

setup::VcVersion FileVersion(const fs::path& f)
{
    DWORD h = 0;
    const DWORD n = GetFileVersionInfoSizeW(f.c_str(), &h);
    if (n == 0) { return {0, 0, 0}; }
    std::vector<unsigned char> buf(n);
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!GetFileVersionInfoW(f.c_str(), 0, n, buf.data()) || !VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&ffi), &len) || !ffi) {
        return {0, 0, 0};
    }
    return {HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS), HIWORD(ffi->dwFileVersionLS)};
}

std::vector<std::string> Lines(const std::string& t)
{
    std::vector<std::string> out;
    size_t p = 0;
    while (p < t.size()) {
        size_t e = t.find('\n', p);
        if (e == std::string::npos) { e = t.size(); }
        std::string l = t.substr(p, e - p);
        p = e + 1;
        if (!l.empty() && l.back() == '\r') { l.pop_back(); }
        if (!l.empty() && l[0] != '#') { out.push_back(l); }
    }
    return out;
}

std::wstring RegString(HKEY root, const wchar_t* key, const wchar_t* value)
{
    wchar_t buf[MAX_PATH] = {};
    DWORD sz = sizeof(buf);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) { return L""; }
    return buf;
}

std::string Stamp()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[32];
    std::snprintf(b, sizeof(b), "%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

std::string Target(const std::string& rel, bool flat)
{
    return (flat && detail::LowerPath(rel).compare(0, 5, "dlvr/") != 0) ? std::string(kFlatDir) + rel : rel;
}

}  // namespace

std::string AaLabel(const setup::AaOption& o)
{
    static const char* kQ[] = {"Native AA", "Quality", "Balanced", "Performance"};
    const char* q = (o.quality >= 0 && o.quality <= 3) ? kQ[o.quality] : "?";
    if (o.method == setup::AaWord::Dlss) { return o.quality == 0 ? "DLAA" : std::string("DLSS ") + q; }
    return std::string(o.method == setup::AaWord::Fsr4 ? "FSR 4 " : "FSR 3.1 ") + q;
}

void LauncherApp::Init(const fs::path& gameDir, const std::wstring& releaseDir)
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    setup::SetLogPath((fs::path(exe).parent_path() / L"launcher.log").wstring());   // the setup's discovery code logs to the same file
    releaseDir_ = releaseDir;
    s_.candidates.clear();
    for (const auto& c : setup::FindGames(setup::DiscoveryOptions{})) { s_.candidates.push_back(c.folder); }
    const setup::Gpu g = setup::BestGpu();
    const setup::Recommendation rec = setup::Recommend(g.name, g.vendor);
    s_.gpuName = g.name;
    s_.vdAdvice = rec.vdAdvice ? rec.vdAdvice : "";
    s_.aaOptions = setup::OptionsFor(rec, g.name, g.vendor, &s_.aaDefault);
    nvidia_ = g.vendor == 0x10DE;
    if (!releaseDir_.empty()) {
        const fs::path d(releaseDir_);
        std::string why;
        haveRelease_ = ParseRelease(ReadText(d / L"release.txt"), ReadText(d / L"notes.txt"), &release_, &why);
        s_.releaseWhy = haveRelease_ ? "" : why;
        Log("release folder " + W2U(releaseDir_) + ": " + (haveRelease_ ? release_.name : "REFUSED - " + why));
    } else if (std::string(kRepoUrl).empty()) {
        s_.releaseWhy = "Updates are not set up yet (the mod's repository is not published).";
    } else {
        s_.releaseWhy = "Press Check for updates to get the latest version.";
    }
    SelectGame(!gameDir.empty() ? gameDir : (s_.candidates.empty() ? fs::path() : s_.candidates[0]));
}

void LauncherApp::SelectGame(const fs::path& game)
{
    s_.game = game;
    staged_ok_ = false;
    epicApp_.clear();
    gogId_.clear();
    galaxyExe_.clear();
    if (game.empty()) {
        Log("game: Dying Light not found - choose its folder");
        Refresh();
        return;
    }
    s_.store = setup::StoreOfFolder(game);
    s_.view.programFiles = setup::UnderProgramFiles(game);
    s_.storeNote = s_.view.programFiles ? setup::ProgramFilesRefusal(s_.store, W2U(game.wstring())) : setup::UntestedNotice(s_.store);
    if (s_.store == setup::Store::Epic) {
        wchar_t pd[MAX_PATH] = {};
        GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH);
        const fs::path dir = fs::path(pd) / L"Epic" / L"EpicGamesLauncher" / L"Data" / L"Manifests";
        std::error_code ec;
        for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            if (it->path().extension() != L".item") { continue; }
            const std::string j = ReadText(it->path());
            std::string loc, app;
            if (setup::EpicManifestDyingLight(j, &loc) && EpicAppName(j, &app) &&
                fs::equivalent(fs::u8path(loc), game, ec)) {
                epicApp_ = app;
            }
        }
        Log("epic: AppName " + (epicApp_.empty() ? std::string("NOT FOUND") : epicApp_));
    } else if (s_.store == setup::Store::Gog) {
        std::error_code ec;
        for (auto it = fs::directory_iterator(game, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            GogIdFromInfoName(W2U(it->path().filename().wstring()), &gogId_);
        }
        const std::wstring client = RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\GOG.com\\GalaxyClient\\paths", L"client");
        if (!client.empty() && fs::exists(fs::path(client) / L"GalaxyClient.exe")) { galaxyExe_ = (fs::path(client) / L"GalaxyClient.exe").wstring(); }
        Log("gog: id " + (gogId_.empty() ? std::string("NOT FOUND") : gogId_) + ", Galaxy " + (galaxyExe_.empty() ? "not installed" : "found"));
    }
    Log("game folder: " + W2U(game.wstring()) + " [" + setup::StoreTag(s_.store) + "]" + (s_.view.programFiles ? " - PROGRAM FILES, refused" : ""));
    Refresh();
}

void LauncherApp::Refresh()
{
    LauncherView& v = s_.view;
    v.haveGame = !s_.game.empty() && fs::exists(s_.game / L"DyingLightGame.exe");
    v.gameRunning = GameIsRunning();
    v.haveRelease = haveRelease_;
    s_.releaseName = haveRelease_ ? release_.name : "";
    s_.releaseNotes = haveRelease_ ? release_.notes : "";
    if (!v.haveGame) { v.match = InstallMatch::None; return; }
    int unreadable = 0;
    detected_ = Detect(s_.game, &unreadable);
    const std::string rec = ReadText(s_.game / L"dlvr" / L"launcher_installed.txt");
    v.mode = ModeFromRecord(rec);
    if (v.mode == Mode::Unknown) { v.mode = Mode::Vr; }
    std::string why;
    haveInstalled_ = !rec.empty() && ParseRelease(ReadText(s_.game / L"dlvr" / L"launcher_release.txt"), "", &installed_, &why);
    s_.installedName = haveInstalled_ ? installed_.name : "";
    const bool flat = v.mode == Mode::Flat;
    if (haveInstalled_) {
        std::map<std::string, std::string> disk;
        for (const auto& f : installed_.files) {
            const std::string h = Md5File(s_.game / fs::u8path(Target(f.relPath, flat)));
            if (!h.empty()) { disk[detail::LowerPath(f.relPath)] = h; }
        }
        v.match = MatchInstalled(installed_, disk, true, &s_.differing);
    } else {
        s_.differing.clear();
        v.match = detected_.modPresent ? InstallMatch::Foreign : InstallMatch::None;
    }
    v.newerRelease = haveRelease_ && haveInstalled_ && release_.name != installed_.name;
    // the settings schema: the installed one (it matches the installed layer), else the release's
    std::string schemaText = ReadText(s_.game / L"dlvr" / L"launcher_schema.json");
    if (schemaText.empty() && !releaseDir_.empty()) { schemaText = ReadText(fs::path(releaseDir_) / L"settings_schema.json"); }
    std::string md5;
    s_.schemaOk = !schemaText.empty() && ParseSchema(schemaText, &s_.schema, &md5, &why);
    const std::string layer = Md5File(s_.game / fs::u8path(Target("LibOVRRT64_1.dll", flat)));
    s_.schemaMatchesLayer = s_.schemaOk && SchemaMatches(md5, layer);
    s_.ovr2xr = detected_.ovr2xrText;
    const setup::OnDisk d = setup::ReadOnDisk(detected_);
    s_.haveAa = d.haveAa;
    s_.aaMethod = d.method;
    s_.aaQuality = d.quality;
    s_.havePrevious = fs::exists(s_.game / L"dlvr" / L"previous" / L"release.txt");
}

std::string LauncherApp::RecordText(const Release& r, Mode m) const
{
    return ReleaseRecordText(r, m == Mode::Flat ? "flat" : "vr", setup::StoreTag(s_.store));
}

bool LauncherApp::Stage(std::string* why)
{
    if (staged_ok_) { return true; }
    if (!haveRelease_ || releaseDir_.empty()) { *why = "there is no release to install from"; return false; }
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    staged_ = fs::path(tmp) / L"dlvr_launcher_stage" / fs::u8path(release_.name);
    std::error_code ec;
    fs::remove_all(staged_, ec);
    fs::create_directories(staged_, ec);
    std::string zip;
    if (!ReadAllBytes(fs::path(releaseDir_) / L"files.zip", &zip)) { *why = "files.zip could not be read"; return false; }
    std::vector<ZipEntry> es;
    if (!ZipEntries(zip, &es, why)) { return false; }
    std::map<std::string, std::string> md5s;
    for (const auto& e : es) {
        std::string data;
        if (!ZipExtract(zip, e, &data, why)) { return false; }
        const fs::path out = staged_ / fs::u8path(e.name);
        fs::create_directories(out.parent_path(), ec);
        RealOps tmpOps(staged_, staged_);
        if (!tmpOps.Write(e.name, data, why)) { return false; }
        md5s[detail::LowerPath(e.name)] = Md5File(out);
    }
    const auto bad = StagedProblems(release_, md5s);
    if (!bad.empty()) {
        *why = "the release's files.zip does not match release.txt: " + bad[0] + (bad.size() > 1 ? " and " + std::to_string(bad.size() - 1) + " more" : "");
        return false;
    }
    fs::copy_file(fs::path(releaseDir_) / L"vc_redist.x64.exe", staged_ / L"vc_redist.x64.exe", fs::copy_options::overwrite_existing, ec);
    staged_ok_ = true;
    Log("staged " + release_.name + ": " + std::to_string(es.size()) + " files, every md5 == release.txt");
    return true;
}

Result LauncherApp::RunPlan(const std::vector<LAction>& plan, const char* what, bool recheck)
{
    RealOps ops(s_.game, staged_);
    Log(std::string(what) + ": " + std::to_string(plan.size()) + " actions");
    ExecResult x = ExecuteWithRollback(plan, ops);
    if (!x.ok) {
        std::string m = std::string(what) + " did not complete: " + x.failedPath + " - " + x.why;
        if (!x.rolledBack.empty()) { m += ". " + std::to_string(x.rolledBack.size()) + " moved file(s) were put back."; }
        Log("FAIL " + m);
        Refresh();
        return {false, m};
    }
    if (recheck) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        const auto gone = Recheck(plan, ops);
        if (!gone.empty()) {
            std::string m = std::string(what) + " did not complete: these files were removed or changed 5 seconds after copying - "
                                                "check your antivirus:";
            for (const auto& g : gone) { m += "\n  " + g; }
            Log("FAIL " + m);
            Refresh();
            return {false, m};
        }
        x = ExecuteAfterRecheck(plan, ops);
        if (!x.ok) { Refresh(); return {false, std::string(what) + ": the record could not be written - " + x.why}; }
    }
    Log(std::string(what) + ": done");
    Refresh();
    return {true, std::string(what) + ": done."};
}

Result LauncherApp::Install()
{
    if (!CanWrite(s_.view)) { return {false, s_.view.gameRunning ? "Close Dying Light (DyingLightGame.exe) first." : "Nothing can be changed in this folder."}; }
    std::string why;
    if (!Stage(&why)) { return {false, "Install refused: " + why}; }
    InstallInputs in;
    in.s = detected_;
    in.s.nvidiaGpu = nvidia_;
    in.release = release_;
    std::vector<std::string> paths;
    for (const auto& f : release_.files) { paths.push_back(f.relPath); }
    in.diskMd5 = DiskMd5(s_.game, paths);
    in.launcherRecord = haveInstalled_;
    in.answers.keep = true;
    // Only the graphics question is answered (the GPU recommendation, when no choice is on disk); every other setting
    // starts at the shipped compiled default and is changed on the VR Comfort / Config tabs.
    if (!s_.haveAa && !s_.aaOptions.empty()) {
        in.answers.haveGraphics = true;
        in.answers.method = s_.aaOptions[static_cast<size_t>(s_.aaDefault)].method;
        in.answers.quality = s_.aaOptions[static_cast<size_t>(s_.aaDefault)].quality;
    }
    in.retiredKeys = Lines(ReadText(fs::path(releaseDir_) / L"retired.txt"));
    in.resets = setup::ParseResetTable(ReadText(fs::path(releaseDir_) / L"resets.tsv"));
    const setup::VcVersion have = setup::InstalledVcRuntime(), bundled = FileVersion(staged_ / L"vc_redist.x64.exe");
    in.vcNeeded = setup::VersionLess(have, bundled);
    Log("vc runtime: installed " + setup::VerText(have) + ", bundled " + setup::VerText(bundled) + (in.vcNeeded ? " -> install" : " -> fine"));
    in.recordText = RecordText(release_, Mode::Vr);
    std::string refusal;
    std::vector<LAction> plan = PlanLauncherInstall(in, &refusal);
    if (!refusal.empty()) { Log("install refused: " + refusal); return {false, refusal}; }
    plan.push_back({LAct::WriteText, kInstalledReleaseCopy, "", "", ReleaseManifestText(release_), true});
    plan.push_back({LAct::WriteText, "dlvr/launcher_schema.json", "", "", ReadText(fs::path(releaseDir_) / L"settings_schema.json"), true});
    return RunPlan(plan, "Install", true);
}

Result LauncherApp::Update()
{
    if (!haveInstalled_) { return Install(); }
    if (!CanWrite(s_.view)) { return {false, "Close Dying Light (DyingLightGame.exe) first."}; }
    std::string why;
    if (!Stage(&why)) { return {false, "Update refused: " + why}; }
    UpdateInputs in;
    in.from = installed_;
    in.to = release_;
    in.ovr2xrText = detected_.ovr2xrText;
    in.writtenText = detected_.writtenText;
    in.resets = setup::ParseResetTable(ReadText(fs::path(releaseDir_) / L"resets.tsv"));
    in.retiredKeys = Lines(ReadText(fs::path(releaseDir_) / L"retired.txt"));
    in.presentLower = detected_.present;
    for (const auto& f : ListFiles(s_.game / L"dlvr" / L"flat_off")) { in.presentLower.insert(detail::LowerPath(std::string(kFlatDir) + f)); }
    in.flatMode = s_.view.mode == Mode::Flat;
    std::vector<LAction> plan = PlanUpdate(in);
    plan.push_back({LAct::WriteText, kLauncherRecord, "", "", RecordText(release_, s_.view.mode), true});
    plan.push_back({LAct::WriteText, kInstalledReleaseCopy, "", "", ReleaseManifestText(release_), true});
    plan.push_back({LAct::WriteText, "dlvr/launcher_schema.json", "", "", ReadText(fs::path(releaseDir_) / L"settings_schema.json"), true});
    return RunPlan(plan, "Update", true);
}

Result LauncherApp::Repair()
{
    if (!haveInstalled_) { return {false, "Nothing to repair - the launcher did not install the mod here."}; }
    if (!haveRelease_ || release_.name != installed_.name) {
        return {false, "Repair needs the files of " + installed_.name + " - use Update instead."};
    }
    if (!CanWrite(s_.view)) { return {false, "Close Dying Light (DyingLightGame.exe) first."}; }
    std::string why;
    if (!Stage(&why)) { return {false, "Repair refused: " + why}; }
    const auto plan = PlanRepair(installed_, s_.differing, s_.view.mode == Mode::Flat);
    if (plan.empty()) { return {true, "Every file is correct - nothing to repair."}; }
    return RunPlan(plan, "Repair", true);
}

Result LauncherApp::Uninstall()
{
    if (!s_.view.haveGame || s_.view.gameRunning) { return {false, "Close Dying Light (DyingLightGame.exe) first."}; }
    std::set<std::string> stock;
    for (const auto& f : ListFiles(s_.game / L"dlvr" / L"stock")) { stock.insert(detail::LowerPath(f)); }
    const Release& r = haveInstalled_ ? installed_ : release_;
    const auto plan = PlanLauncherRemove(detected_, r, stock, s_.view.mode == Mode::Flat);
    Result res = RunPlan(plan, "Uninstall", false);
    if (res.ok) {
        res.message = "Uninstalled. Now verify the game files in your store (Steam: Properties -> Installed Files -> Verify "
                      "integrity of game files).";
    }
    return res;
}

Result LauncherApp::RollBack()
{
    if (!haveInstalled_ || !s_.havePrevious) { return {false, "There is no previous version to go back to."}; }
    if (!CanWrite(s_.view)) { return {false, "Close Dying Light (DyingLightGame.exe) first."}; }
    Release prev;
    std::string why;
    if (!ParseRelease(ReadText(s_.game / L"dlvr" / L"previous" / L"release.txt"), "", &prev, &why)) { return {false, "Roll back: " + why}; }
    std::set<std::string> present;
    for (const auto& f : ListFiles(s_.game / L"dlvr" / L"previous")) { present.insert(detail::LowerPath(std::string(kPreviousDir) + f)); }
    std::vector<LAction> plan = PlanRollback(prev, installed_, present, s_.view.mode == Mode::Flat, RecordText(prev, s_.view.mode));
    plan.push_back({LAct::WriteText, kInstalledReleaseCopy, "", "", ReleaseManifestText(prev), false});
    return RunPlan(plan, "Roll back", false);
}

Result LauncherApp::ToFlat()
{
    if (!haveInstalled_ && detected_.modPresent) {
        return {false, "The mod here was installed by an older installer - the launcher cannot switch it off. Uninstall it first."};
    }
    if (s_.view.mode == Mode::Flat || !haveInstalled_) { return {true, "The mod is off."}; }
    if (!CanWrite(s_.view)) { return {false, "Close Dying Light (DyingLightGame.exe) first."}; }
    std::vector<std::string> stock = ListFiles(s_.game / L"dlvr" / L"stock");
    const std::string scr = ReadText(VideoScrPath());
    return RunPlan(PlanToFlat(installed_, detected_.present, stock, scr, RecordText(installed_, Mode::Flat)), "Switch to flat", false);
}

Result LauncherApp::ToVr()
{
    if (s_.view.mode != Mode::Flat) { return {true, "The mod is on."}; }
    if (!CanWrite(s_.view)) { return {false, "Close Dying Light (DyingLightGame.exe) first."}; }
    std::set<std::string> flat;
    for (const auto& f : ListFiles(s_.game / L"dlvr" / L"flat_off")) { flat.insert(f); }
    return RunPlan(PlanToVr(flat, detected_.present, RecordText(installed_, Mode::Vr)), "Switch to VR", false);
}

Result LauncherApp::Launch()
{
    const LaunchCmd c = LaunchFor(s_.store, s_.game.wstring(), epicApp_, gogId_, galaxyExe_);
    if (c.file.empty()) { return {false, c.why}; }
    Log("launch: " + W2U(c.file) + " " + W2U(c.args));
    if (c.viaShell) {
        const HINSTANCE h = ShellExecuteW(nullptr, L"open", c.file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(h) <= 32) { return {false, "Could not start the game (" + W2U(c.file) + ")."}; }
    } else {
        std::wstring cmd = L"\"" + c.file + L"\" " + c.args;
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, s_.game.c_str(), &si, &pi)) {
            return {false, "Could not start " + W2U(c.file)};
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return {true, "Starting Dying Light..."};
}

Result LauncherApp::SaveSettings(const std::vector<KeyValue>& edits)
{
    if (!CanWrite(s_.view)) { return {false, "Close Dying Light (DyingLightGame.exe) first - the game saves its own settings over these on exit."}; }
    const std::string out = ApplyEdits(s_.ovr2xr, s_.schema, edits);
    if (out == s_.ovr2xr) { return {true, ""}; }
    RealOps ops(s_.game, staged_);
    std::string why;
    if (!ops.Write("dlvr/ovr2xr.txt", out, &why)) { return {false, "Could not save the settings: " + why}; }
    s_.ovr2xr = out;
    detected_.ovr2xrText = out;
    return {true, "Saved."};
}

Result LauncherApp::SaveGraphics(setup::AaWord method, int quality)
{
    if (!CanWrite(s_.view)) { return {false, "Close Dying Light (DyingLightGame.exe) first."}; }
    RealOps ops(s_.game, staged_);
    std::string why;
    if (!ops.Write("dlvr/aa_method.txt", std::string(setup::AaWordText(method)) + "\r\n", &why) ||
        !ops.Write("dlvr/dlss_quality.txt", std::to_string(quality) + "\r\n", &why)) {
        return {false, "Could not save the graphics choice: " + why};
    }
    Refresh();
    return {true, "Saved."};
}

Result LauncherApp::CollectLogs(fs::path* zipOut)
{
    std::vector<std::pair<std::string, fs::path>> files;   // (name in zip, file)
    auto newest = [&](const fs::path& dir, const wchar_t* prefix, const wchar_t* ext, size_t n, const char* zipDir) {
        std::vector<std::pair<std::string, unsigned long long>> found;
        std::error_code ec;
        for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            const std::wstring nm = it->path().filename().wstring();
            if (nm.rfind(prefix, 0) != 0 || it->path().extension() != ext) { continue; }
            found.push_back({W2U(it->path().wstring()), static_cast<unsigned long long>(fs::last_write_time(it->path(), ec).time_since_epoch().count())});
        }
        for (const auto& p : PickNewest(found, n)) { files.push_back({std::string(zipDir) + "/" + W2U(fs::u8path(p).filename().wstring()), fs::u8path(p)}); }
    };
    newest(DocumentsDir() / L"DyingLight" / L"out" / L"logs", L"crash_", L".log", 5, "game_crash_logs");
    if (!s_.game.empty()) {
        newest(s_.game, L"dlvr_", L".log", 20, "mod_logs");
        for (const wchar_t* f : {L"ovr2xr.txt", L"arm_fit.txt", L"aa_method.txt", L"dlss_quality.txt", L"dlss_preset.txt",
                                 L"dlss_sharpness.txt", L"launcher_installed.txt", L"launcher_release.txt"}) {
            if (fs::exists(s_.game / L"dlvr" / f)) { files.push_back({"dlvr/" + W2U(f), s_.game / L"dlvr" / f}); }
        }
    }
    if (fs::exists(VideoScrPath())) { files.push_back({"video.scr", VideoScrPath()}); }
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    // the user's names: %USERNAME%, the profile folder's 8.3 short name
    wchar_t user[256] = {}, prof[MAX_PATH] = {}, shortProf[MAX_PATH] = {};
    GetEnvironmentVariableW(L"USERNAME", user, 256);
    GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH);
    GetShortPathNameW(prof, shortProf, MAX_PATH);
    const auto names = NameForms(W2U(user), W2U(shortProf), {});
    std::vector<std::pair<std::string, std::string>> entries;
    int total = 0;
    std::string summary = "Dying Light VR log collection " + Stamp() + "\r\n";
    summary += "launcher record: " + ReadText(s_.game / L"dlvr" / L"launcher_installed.txt") + "\r\n";
    for (const auto& f : files) {
        std::string data;
        if (!ReadAllBytes(f.second, &data)) { summary += "could not read " + f.first + "\r\n"; continue; }
        int hits = 0;
        const bool binary = f.second.extension() == L".dmp";
        entries.push_back({f.first, binary ? MaskBinary(data, names, &hits) : MaskText(data, names, &hits)});
        total += hits;
    }
    Log("logs: masking " + std::to_string(names.size()) + " name form(s)");
    std::string launcherLog;
    ReadAllBytes(fs::path(exe).parent_path() / L"launcher.log", &launcherLog);
    int lh = 0;
    entries.push_back({"launcher.log", MaskText(launcherLog, names, &lh)});
    total += lh;
    summary += names.empty() ? "user name masking: NO user name found to mask\r\n"
                             : "user name masking: " + std::to_string(total) + " occurrence(s) masked\r\n";
    entries.push_back({"README.txt", summary});
    const fs::path out = DesktopDir() / fs::u8path("DyingLightVR_logs_" + Stamp() + ".zip");
    const std::string z = ZipStore(entries);
    RealOps ops(out.parent_path(), out.parent_path());
    std::string why;
    if (!ops.Write(W2U(out.filename().wstring()), z, &why)) { return {false, "Could not write the zip: " + why}; }
    *zipOut = out;
    Log("logs: " + std::to_string(entries.size()) + " files -> " + W2U(out.filename().wstring()) + ", " + std::to_string(total) + " masked");
    return {true, "Saved " + W2U(out.filename().wstring()) + " to your Desktop (" + std::to_string(entries.size()) + " files, user names masked)."};
}

void LauncherApp::CleanupOldExe()
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const fs::path old = fs::path(exe).replace_extension(L".old.exe");
    std::error_code ec;
    if (OnStartStep(false, fs::exists(old, ec)) == SelfStep::DeleteOld) {
        fs::remove(old, ec);
        Log(std::string("self-update: left-over ") + W2U(old.filename().wstring()) + (ec ? " could not be deleted yet (" + ec.message() + ")" : " deleted"));
    }
}

Result LauncherApp::SelfUpdate(bool* restarting)
{
    *restarting = false;
    if (releaseDir_.empty()) { return {true, ""}; }
    wchar_t exeBuf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
    const fs::path own(exeBuf);
    const fs::path fresh = fs::path(releaseDir_) / L"DyingLightVR_Launcher.exe";
    std::error_code ec;
    if (!fs::exists(fresh, ec)) { return {true, ""}; }
    const std::string freshMd5 = Md5File(fresh);
    if (freshMd5 == Md5File(own)) { return {true, ""}; }
    if (!release_.launcherMd5.empty() && freshMd5 != release_.launcherMd5) {
        return {false, "The new launcher does not match the release (md5 " + freshMd5 + ") - kept this one."};
    }
    const fs::path old = fs::path(own).replace_extension(L".old.exe");
    const fs::path ok = own.parent_path() / L"launcher_update_ok.txt";
    fs::remove(old, ec);
    fs::remove(ok, ec);
    if (!MoveFileExW(own.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) { return {false, "Could not rename this launcher for the update - kept it."}; }
    if (!CopyFileW(fresh.c_str(), own.c_str(), FALSE) || Md5File(own) != freshMd5) {
        DeleteFileW(own.c_str());
        MoveFileExW(old.c_str(), own.c_str(), MOVEFILE_REPLACE_EXISTING);
        return {false, "Could not put the new launcher in place - kept this one."};
    }
    std::wstring cmd = L"\"" + own.wstring() + L"\" --after-update --release \"" + releaseDir_ + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    bool signalled = false;
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, own.parent_path().c_str(), &si, &pi)) {
        for (int i = 0; i < 60 && !signalled; ++i) {   // up to 30 s
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            signalled = fs::exists(ok, ec);
        }
        if (!signalled) { TerminateProcess(pi.hProcess, 1); WaitForSingleObject(pi.hProcess, 5000); }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    if (OnWaitEndStep(signalled) == SelfStep::RestoreOld) {
        DeleteFileW(own.c_str());
        MoveFileExW(old.c_str(), own.c_str(), MOVEFILE_REPLACE_EXISTING);
        Log("self-update: the new launcher did not start within 30 s - this one was restored");
        return {false, "The new launcher did not start - this one was kept."};
    }
    fs::remove(ok, ec);
    Log("self-update: the new launcher (" + freshMd5 + ") is running - this one exits");
    *restarting = true;
    return {true, "Restarting the launcher..."};
}

Result LauncherApp::CheckForUpdates()
{
    if (std::string(kRepoUrl).empty()) { return {false, "Updates are not set up yet (the mod's repository is not published)."}; }
    std::string body, why;
    if (!HttpGetText(kRepoUrl, &body, &why)) {
        if (why.find("status 404") != std::string::npos) {   // a private repo, or one with no release yet
            return {false, "No public release was found yet - the mod's repository is private or has no release."};
        }
        return {false, "Could not check for updates: " + why};
    }
    LatestInfo li;
    if (!ParseLatestRelease(body, &li, &why)) { return {false, "The latest release is not usable: " + why}; }
    if (haveInstalled_ && !NeedsUpdate(installed_.name, li.tag)) { return {true, "You have the latest version (" + li.tag + ")."}; }
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    const fs::path dir = fs::path(tmp) / L"dlvr_launcher_download" / fs::u8path(li.tag);
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::pair<const std::string*, const wchar_t*> assets[] = {
        {&li.manifestUrl, L"release.txt"}, {&li.notesUrl, L"notes.txt"}, {&li.zipUrl, L"files.zip"},
        {&li.schemaUrl, L"settings_schema.json"}, {&li.launcherUrl, L"DyingLightVR_Launcher.exe"},
        {&li.redistUrl, L"vc_redist.x64.exe"}, {&li.resetsUrl, L"resets.tsv"}, {&li.retiredUrl, L"retired.txt"}};
    for (const auto& a : assets) {
        if (a.first->empty()) { continue; }
        if (!HttpGetToFile(*a.first, dir / a.second, &why)) { return {false, "Download failed: " + why}; }
    }
    releaseDir_ = dir.wstring();
    haveRelease_ = ParseRelease(ReadText(dir / L"release.txt"), ReadText(dir / L"notes.txt"), &release_, &why);
    staged_ok_ = false;
    Refresh();
    return {haveRelease_, haveRelease_ ? "Version " + release_.name + " is ready to install." : "The download is not usable: " + why};
}

}  // namespace dlvr::launcher
