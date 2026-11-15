// DyingLightVR_Setup.exe - installs, updates and removes the mod (docs/superpowers/specs/2026-10-17-installer-design.md;
// plans docs/superpowers/plans/2026-10-17-installer.md and ...-installer-rev2.md). Jay 2026-10-17: "Native .exe",
// "This just makes it bulletproof", and for revision 2 "a proper installer UI", everything "baked into the installer
// itself", Epic and GOG supported.
//
// Every DECISION is in the tested headers (setup_plan.h and the ones it includes, setup_record.h, setup_store.h,
// setup_view.h). setup_sys finds things and executes; setup_ui draws the window. This file: the command line, the
// payload (embedded, or --payload <folder> for tests) verified BEFORE anything is touched, and the window-less --auto.
//
// Built with the static CRT: it must run on a PC that does not have the VC++ runtime yet - it is what installs it.
//
// Command line (testing and scripted installs; with none, the window opens):
//   --game-dir <p>  --docs-dir <p>  --steam-userdata <p>  --payload <p>  --log <file>
//   --epic-manifests <dir>  --no-registry          discovery overrides (tests)
//   --gpu "<vendor hex>;<adapter name>"            replaces the detected GPU (tests)
//   --shot <state> <out.png> [--scale <f>] [--shot-height <px>]  render one window state to a PNG and exit
//   --auto "<k=v;...>"  no window; keys: action=install|remove, keep=0|1, armfit=0|1, method=dlss|fsr3|fsr4,
//                       quality=0..3, turn=snap|smooth, follow=on|off, afw=on|off, hud=panel|head. Unanswered questions take the recommendation /
//                       the shipped default.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "setup_gpu.h"
#include "setup_plan.h"
#include "setup_record.h"
#include "setup_sys.h"
#include "setup_ui.h"

using namespace dlvr::setup;

namespace {

// D-156: setting keys are never retired or renamed - EXCEPT these, which Jay retired 2026-11-14 with the probes that read them
// (removal batch 1: relevance, guilist, dlssprobe, mapprobe; "Retire them"; batch 2: camscan, "Go, retire both"; 2026-11-15
// batch 9: the two anim_suppress stubs on the final gate's targets, "Yes"). A keep / reset install drops their lines.
const std::vector<std::string> kRetiredKeys = {
    "diag_relevance_walk", "relevance_disable_husk", "relevance_track_vptr",
    "gui_list_walk", "diag_dlss_probe", "diag_maplock_probe",
    "cam_transpose", "cam_order",
    "anim_suppress_bodymod", "anim_suppress_visupdate2",
    // removal batch 10 (2026-11-15, docs/audit/88_): the item-pose fix + the duck probe
    "item_pose_fix", "item_pose_basis", "item_pose_achieved", "item_pose_max_age_ms", "item_pose_max_jump",
    "item_pose_basis_max_deg", "item_basis_pitch_deg", "item_basis_yaw_deg", "item_basis_roll_deg",
    "item_pose_clear_stale", "item_pose_test_target", "item_pose_test_elem", "item_pose_test_basis_deg",
    "item_pose_test_axis", "item_pose_test_offset_m", "item_local_test_m", "item_local_test_deg", "item_local_test_axis",
    "diag_basis_delta", "diag_item_dirty", "diag_item_parity", "diag_item_holder", "diag_item_xform", "diag_duck_probe",
};

std::map<std::string, std::string> g_auto;

std::string Auto(const std::string& k, const std::string& dflt)
{
    const auto it = g_auto.find(k);
    return it == g_auto.end() ? dflt : it->second;
}

void Message(const std::string& head, const std::string& body) { Log("MESSAGE: " + head + " | " + body); }

std::string PackageOf(const std::string& manifest)
{
    const std::string tag = "# package=";
    size_t p = manifest.find(tag);
    if (p == std::string::npos) { return "unknown package"; }
    p += tag.size();
    size_t e = manifest.find_first_of("\r\n", p);
    return manifest.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

std::string MethodLabel(AaWord m, int q)
{
    static const char* kQ[] = {"DLAA / native", "Quality", "Balanced", "Performance"};
    const char* qn = (q >= 0 && q <= 3) ? kQ[q] : "?";
    if (m == AaWord::Dlss) { return q == 0 ? std::string("DLAA") : std::string("DLSS ") + qn; }
    return std::string(m == AaWord::Fsr4 ? "FSR 4 " : "FSR 3.1 ") + qn;
}

// The window-less flow (v1's, kept for the fake-folder tests and scripted installs).
int RunAuto(UiArgs& u)
{
    fs::path game = u.forcedGame;
    if (game.empty()) {
        const auto c = FindGames(u.discovery);
        if (!c.empty()) { game = c.front().folder; }
        Log("search: " + (game.empty() ? std::string("Dying Light not found") : U(game.wstring())));
    }
    std::error_code ec;
    if (game.empty() || !fs::exists(game / L"DyingLightGame.exe", ec)) {
        Message("Not a Dying Light folder", "DyingLightGame.exe is not in: " + U(game.wstring()));
        return 1;
    }
    const Store store = StoreOfFolder(game);
    if (UnderProgramFiles(game)) {
        Message("The game is under Program Files", ProgramFilesRefusal(store, U(game.wstring())));
        return 1;
    }
    const int run0 = GameRunning();
    if (run0 != 0) {
        Message(run0 > 0 ? "Dying Light is running" : "Could not check whether Dying Light is running",
                "Close the game completely, then run Setup again.");
        return 1;
    }
    Log("game folder: " + U(game.wstring()) + " [" + StoreTag(store) + "]");
    const std::string un = UntestedNotice(store);
    if (!un.empty()) { Log("notice: " + un); }

    DetectedState s = Detect(game);
    const Gpu g = u.gpuOverride.vendor ? u.gpuOverride : BestGpu();
    s.nvidiaGpu = (g.vendor == 0x10DE);
    Log(std::string("mod present: ") + (s.modPresent ? "yes" : "no") + ", files seen: " + std::to_string(s.present.size()) +
        ", GPU: " + (g.name.empty() ? std::string("none found") : g.name) + (s.nvidiaGpu ? " (NVIDIA)" : ""));
    Answers a{};
    bool remove = false;
    if (s.modPresent) {
        remove = Auto("action", "install") == "remove";
        a.keep = Auto("keep", "1") == "1";
        a.keepArmFit = Auto("armfit", "1") == "1";
    }
    std::vector<Action> acts;
    std::vector<Moved> moved;
    std::string layoutWarn;
    if (remove) {
        acts = PlanRemove(s);
    } else {
        const std::vector<std::string> qs = QuestionsToAsk(s, a.keep);
        std::string qlist;
        for (const auto& q : qs) { qlist += (qlist.empty() ? "" : ", ") + q; }
        Log(std::string(a.keep ? "keep" : "fresh / reset") + " - questions: " + (qlist.empty() ? "none" : qlist));
        for (const auto& q : qs) {
            if (q == "graphics") {
                const Recommendation r = Recommend(g.name, g.vendor);
                Log("GPU: " + (g.name.empty() ? std::string("none found") : g.name) + " vendor " + std::to_string(g.vendor) +
                    " -> " + MethodLabel(r.method, r.quality) + ", VD " + r.vdAdvice + (r.recognised ? "" : " (card not in the table)"));
                const std::string m = Auto("method", AaWordText(r.method));
                a.method = m == "dlss" ? AaWord::Dlss : m == "fsr4" ? AaWord::Fsr4 : AaWord::Fsr3;
                const std::string qv = Auto("quality", std::to_string(r.quality));
                a.quality = (qv.size() == 1 && qv[0] >= '0' && qv[0] <= '3') ? qv[0] - '0' : r.quality;
                a.haveGraphics = true;
            } else if (q == "turn") {
                a.snap = Auto("turn", "snap") == "snap";
                a.haveTurn = true;
            } else if (q == "follow") {
                a.follow = Auto("follow", "on") == "on";
                a.haveFollow = true;
            } else if (q == "afw") {
                a.afw = Auto("afw", "off") == "on";
                a.haveAfw = true;
            } else if (q == "hud") {
                a.hudPanel = Auto("hud", "panel") == "panel";
                a.haveHud = true;
            }
        }
        const VcVersion have = InstalledVcRuntime();
        const VcVersion bundled = BundledVcRuntime(*u.payload);
        const bool needRedist = VersionLess(have, bundled) || (have.major == 0);
        Log("VC++ runtime: installed " + VerText(have) + ", bundled " + VerText(bundled) +
            (needRedist ? " -> will install" : " -> up to date"));
        try {
            acts = PlanInstall(s, a, u.manifest, kRetiredKeys, needRedist, RecordText(u.package, u.manifestMd5), u.resets,
                               &moved);
            for (const auto& mv : moved) { Log("reset: " + mv.key + " " + mv.from + " -> " + mv.to); }
        } catch (const std::exception& e) {
            Message("Setup's package is damaged", e.what());
            return 1;
        }
        layoutWarn = LayoutWarning(store, u.steamUserdata);
    }
    const int run1 = GameRunning();
    if (run1 != 0) {
        Message(run1 > 0 ? "Dying Light is running" : "Could not check whether Dying Light is running",
                "Nothing was changed. Close the game completely, then run Setup again.");
        return 1;
    }
    const Outcome o = Execute(acts, game, u.docs, *u.payload, nullptr);
    if (!o.ok) {
        Message("Setup FAILED", "This did not complete: " + o.failedPath + "\n\nThe mod is NOT correctly installed. The full log is:\n" +
                                    U(LogPath()));
        return 2;
    }
    if (remove) {
        Message("The mod was removed", "Now let your store put back the game's own files:\nSteam -> right-click Dying Light -> "
                                       "Properties -> Installed Files -> \"Verify integrity of game files\".");
    } else {
        std::string body = "Every file was checked after copying.";
        if (!moved.empty()) { body += "\n\n" + MovedSummary(moved); }
        if (!layoutWarn.empty()) { body += "\n\n" + layoutWarn; }
        Message("Dying Light VR is installed", body);
    }
    Log("RESULT: OK");
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    UiArgs u;
    std::wstring payloadDir, shotState, shotPng, logPath;
    float shotScale = 1.0f;
    int shotHeight = 0;
    bool autoMode = false;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    logPath = (fs::path(tmp) / L"DyingLightVR_Setup.log").wstring();
    for (int i = 1; i < argc; ++i) {
        const std::wstring k = argv[i];
        const bool hasV = i + 1 < argc;
        if (k == L"--no-registry") { u.discovery.useRegistry = false; continue; }
        if (!hasV) { break; }
        const std::wstring v = argv[++i];
        if (k == L"--game-dir") { u.forcedGame = v; }
        else if (k == L"--docs-dir") { u.docs = v; }
        else if (k == L"--steam-userdata") { u.steamUserdata = v; }
        else if (k == L"--payload") { payloadDir = v; }
        else if (k == L"--log") { logPath = v; }
        else if (k == L"--epic-manifests") { u.discovery.epicManifests = v; }
        else if (k == L"--scale") { shotScale = static_cast<float>(_wtof(v.c_str())); }
        else if (k == L"--shot-height") { shotHeight = _wtoi(v.c_str()); }
        else if (k == L"--gpu") {
            const std::string gv = U(v);
            const size_t semi = gv.find(';');
            u.gpuOverride.vendor = static_cast<unsigned>(std::strtoul(gv.substr(0, semi).c_str(), nullptr, 16));
            u.gpuOverride.name = semi == std::string::npos ? "" : gv.substr(semi + 1);
        } else if (k == L"--shot") {
            shotState = v;
            if (i + 1 < argc) { shotPng = argv[++i]; }
        } else if (k == L"--auto") {
            autoMode = true;
            std::stringstream ss(U(v));
            std::string kv;
            while (std::getline(ss, kv, ';')) {
                const size_t eq = kv.find('=');
                if (eq != std::string::npos) { g_auto[kv.substr(0, eq)] = kv.substr(eq + 1); }
            }
        }
    }
    LocalFree(argv);
    {
        std::error_code ec;
        fs::remove(fs::path(logPath), ec);   // one log per run
    }
    SetLogPath(logPath);
    Log("Dying Light VR Setup - log");

    if (u.docs.empty()) {
        PWSTR d = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &d)) && d) {
            u.docs = d;
            CoTaskMemFree(d);
        }
    }
    if (u.steamUserdata.empty() && u.discovery.useRegistry) {
        wchar_t buf[MAX_PATH * 2] = {};
        DWORD cb = sizeof(buf);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &cb) == ERROR_SUCCESS) {
            std::wstring s = buf;
            for (auto& c : s) { if (c == L'/') { c = L'\\'; } }
            u.steamUserdata = fs::path(s) / L"userdata";
        }
    }

    // The payload: embedded unless --payload; its file list and EVERY file's md5 checked before anything is touched.
    std::unique_ptr<Payload> payload = payloadDir.empty() ? EmbeddedPayload() : FolderPayload(payloadDir);
    std::string mtext, why;
    int rc = 0;
    if (!payload->Manifest(&mtext, &why)) {
        Message("Setup's files are missing", why);
        rc = 1;
    } else {
        try {
            u.manifest = ParseManifest(mtext);
        } catch (const std::exception& e) {
            Message("Setup's package is damaged", e.what());
            rc = 1;
        }
    }
    if (rc == 0 && shotState.empty()) {
        std::string bad;
        if (!VerifyPayload(*payload, u.manifest, &bad)) {
            Message("Setup's package is damaged", "A file inside Setup is missing or damaged: " + bad +
                                                       "\nNothing was changed. Download Setup again.");
            rc = 1;
        } else {
            Log("payload: " + std::to_string(u.manifest.size()) + " files, every md5 checked (" +
                (payloadDir.empty() ? std::string("embedded") : "folder " + U(payloadDir)) + ")");
        }
    }
    u.payload = payload.get();
    u.package = PackageOf(mtext);
    u.manifestMd5 = Md5Hex(mtext);
    u.retiredKeys = kRetiredKeys;
    {   // the keep-install reset table (spec 2026-10-25-keep-settings-old-default-reset-design.md): resource 103, or
        // resets.tsv beside the --payload folder's files. Absent = keep installs move no old defaults, and the log says so.
        std::string tsv;
        std::string why;
        const bool have = payloadDir.empty() ? ReadResource(kResResets, &tsv)
                                             : (payload && payload->Read("resets.tsv", &tsv, &why));
        u.resets = have ? ParseResetTable(tsv) : std::vector<ResetRow>{};
        Log(have ? "reset table: " + std::to_string(u.resets.size()) + " keys"
                 : std::string("reset table: NONE - keep installs move no old defaults"));
    }
    ReadResource(kResTitlePng, &u.titlePng);
    if (u.docs.empty() && rc == 0) {   // never fall back to a relative path for video.scr
        Message("Your Documents folder was not found",
                "Setup writes the game's video settings into Documents\\DyingLight, and Windows did not return the Documents "
                "folder. Nothing was changed.");
        rc = 1;
    }

    if (!shotState.empty()) {
        rc = RenderShot(u, U(shotState), shotPng, shotScale, shotHeight);
    } else if (rc != 0) {
        if (!autoMode) {   // the one place a system message box remains: Setup cannot start its own window without its files
            const std::wstring text = L"Dying Light VR Setup is damaged or incomplete. Download it again.\n\nDetails are in "
                                      L"the log:\n" + LogPath();
            MessageBoxW(nullptr, text.c_str(), L"Dying Light VR Setup", MB_ICONERROR | MB_OK);
        }
    } else if (autoMode) {
        rc = RunAuto(u);
    } else {
        rc = RunUi(u);
    }
    Log(rc == 0 ? "exit 0" : "RESULT: exit " + std::to_string(rc));
    CoUninitialize();
    return rc;
}
