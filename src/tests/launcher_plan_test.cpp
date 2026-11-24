// launcher_plan.h - install / repair / remove plans (spec docs/superpowers/specs/2026-11-15-launcher-design.md
// sections 6, 12.2, 12.4).
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "fixed_data3_md5.h"
#include "launcher_plan.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace dlvr::launcher;
using dlvr::setup::DetectedState;

static const std::string kOurs = "11111111111111111111111111111111";
static const std::string kStock = "22222222222222222222222222222222";
static const std::string kOther = "33333333333333333333333333333333";

static Release Rel()
{
    Release r;
    std::string why;
    const std::string m = "# release=R1\n" + kOurs + "  LibOVRRT64_1.dll\n" + kOurs + "  xinput1_3.dll\n" + kFixedData3Md5 +
                          "  DW/Data3.pak\n" + kOurs + "  DW_DLC24/DataDLC24_2.pak\n" + kOurs + "  dlvr/afw_keep_rift.txt\n" +
                          kOurs + "  nvngx_dlss.dll dlss-only\n";
    ParseRelease(m, "", &r, &why);
    return r;
}

static InstallInputs Fresh(const Release& r, const DetectedState& s)
{
    InstallInputs in{s, {}, r, {}, false, {}, false, "release=R1\r\nmode=vr\r\nstore=STEAM\r\n", {}};
    in.answers.method = dlvr::setup::AaWord::Dlss;
    in.answers.haveGraphics = true;
    return in;
}

static int IndexOf(const std::vector<LAction>& a, LAct k, const std::string& path)
{
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].kind == k && a[i].relPath == path) { return static_cast<int>(i); }
    }
    return -1;
}

int main()
{
    const Release r = Rel();
    {   // a clean, verified game: stock Data3 is saved BEFORE ours is copied; no DataDLC24_2 -> nothing saved for it
        DetectedState s;
        s.present = {"dw/data3.pak", "dyinglightgame.exe"};
        InstallInputs in = Fresh(r, s);
        in.diskMd5 = {{"dw/data3.pak", kStock}};
        std::string refusal;
        const auto a = PlanLauncherInstall(in, &refusal);
        CHECK(refusal.empty());
        const int save = IndexOf(a, LAct::SaveStock, "DW/Data3.pak");
        const int copy = IndexOf(a, LAct::Copy, "DW/Data3.pak");
        CHECK(save >= 0 && copy >= 0 && save < copy);
        CHECK(save >= 0 && a[save].dest == "dlvr/stock/DW/Data3.pak" && a[save].md5 == kStock);
        CHECK(IndexOf(a, LAct::SaveStock, "DW_DLC24/DataDLC24_2.pak") < 0);
        // the record is written last, only after the re-check; the old record deleted first
        CHECK(!a.empty() && a.back().kind == LAct::WriteText && a.back().relPath == "dlvr/launcher_installed.txt" && a.back().afterRecheck);
        CHECK(a.front().kind == LAct::Delete && a.front().relPath == "dlvr/launcher_installed.txt");
        CHECK(IndexOf(a, LAct::Copy, "nvngx_dlss.dll") >= 0);   // DLSS chosen
    }
    {   // a foreign DataDLC24_2.pak (another mod) is saved too
        DetectedState s;
        InstallInputs in = Fresh(r, s);
        in.diskMd5 = {{"dw/data3.pak", kStock}, {"dw_dlc24/datadlc24_2.pak", kOther}};
        std::string refusal;
        const auto a = PlanLauncherInstall(in, &refusal);
        CHECK(refusal.empty());
        CHECK(IndexOf(a, LAct::SaveStock, "DW_DLC24/DataDLC24_2.pak") >= 0);
    }
    {   // mod present, no launcher record (an install from before the launcher) -> refused: uninstall, then verify
        DetectedState s;
        s.modPresent = true;
        s.present = {"libovrrt64_1.dll", "dw/data3.pak"};
        InstallInputs in = Fresh(r, s);
        std::string refusal;
        const auto a = PlanLauncherInstall(in, &refusal);
        CHECK(a.empty());
        CHECK(refusal.find("Uninstall") != std::string::npos && refusal.find("erify") != std::string::npos);
    }
    {   // our Data3 on disk, no mod, no stock copy -> refused: the game is not stock
        DetectedState s;
        InstallInputs in = Fresh(r, s);
        in.diskMd5 = {{"dw/data3.pak", kFixedData3Md5}};
        std::string refusal;
        const auto a = PlanLauncherInstall(in, &refusal);
        CHECK(a.empty());
        CHECK(refusal.find("Data3") != std::string::npos && refusal.find("erify") != std::string::npos);
    }
    {   // an OLDER release's Data3 left on disk (1.0 - 1.065), no mod, no stock copy -> refused too (trace 20261124_2110)
        DetectedState s;
        InstallInputs in = Fresh(r, s);
        in.diskMd5 = {{"dw/data3.pak", "394248e75c65b83ad2f26151a9b3566a"}};
        std::string refusal;
        const auto a = PlanLauncherInstall(in, &refusal);
        CHECK(a.empty());
        CHECK(refusal.find("Data3") != std::string::npos && refusal.find("erify") != std::string::npos);
    }
    {   // any OTHER Data3 (the game's own) -> not refused, saved as stock
        DetectedState s;
        InstallInputs in = Fresh(r, s);
        in.diskMd5 = {{"dw/data3.pak", kOther}};
        std::string refusal;
        const auto a = PlanLauncherInstall(in, &refusal);
        CHECK(refusal.empty());
        bool saved = false;
        for (const auto& x : a) { saved = saved || (x.kind == LAct::SaveStock && x.relPath == "DW/Data3.pak"); }
        CHECK(saved);
    }
    {   // an UPDATE (launcher record present): our older files are NOT saved as stock
        DetectedState s;
        s.modPresent = true;
        s.present = {"libovrrt64_1.dll", "dw/data3.pak", "dlvr/launcher_installed.txt"};
        InstallInputs in = Fresh(r, s);
        in.launcherRecord = true;
        in.answers.keep = true;
        in.diskMd5 = {{"dw/data3.pak", kOther}, {"libovrrt64_1.dll", kOther}};
        std::string refusal;
        const auto a = PlanLauncherInstall(in, &refusal);
        CHECK(refusal.empty());
        for (const auto& x : a) { CHECK(x.kind != LAct::SaveStock); }
        CHECK(IndexOf(a, LAct::Copy, "LibOVRRT64_1.dll") >= 0);
    }
    {   // Repair copies only the differing files; in flat mode into dlvr/flat_off/
        const auto vr = PlanRepair(r, {"xinput1_3.dll"}, false);
        CHECK(vr.size() == 1 && vr[0].kind == LAct::Copy && vr[0].relPath == "xinput1_3.dll" && vr[0].dest == "xinput1_3.dll"
              && vr[0].md5 == kOurs);
        const auto fl = PlanRepair(r, {"xinput1_3.dll"}, true);
        CHECK(fl.size() == 1 && fl[0].dest == "dlvr/flat_off/xinput1_3.dll");
        CHECK(PlanRepair(r, {"not/in/release.dll"}, false).empty());
    }
    {   // Remove: record first; every release file present + runtime-written files; stock moved back; dlvr/ last
        DetectedState s;
        s.modPresent = true;
        s.present = {"libovrrt64_1.dll", "xinput1_3.dll", "dw/data3.pak", "dw_dlc24/datadlc24_2.pak", "dw/dlvr_binds.scr",
                     "dlvr_ovr2xr.log", "dlvr_vehrec.bin", "dlvr_hud_panel_1_eye0_2688x2880.rgba", "dyinglightgame.exe",
                     "dlvr/stock/dw/data3.pak"};
        const auto a = PlanLauncherRemove(s, r, {"dw/data3.pak"}, false);
        CHECK(!a.empty() && a.front().kind == LAct::Delete && a.front().relPath == "dlvr/launcher_installed.txt");
        CHECK(!a.empty() && a.back().kind == LAct::Delete && a.back().relPath == "dlvr/");
        for (const char* p : {"LibOVRRT64_1.dll", "xinput1_3.dll", "DW_DLC24/DataDLC24_2.pak", "DW/dlvr_binds.scr",
                              "dlvr_ovr2xr.log", "dlvr_vehrec.bin", "dlvr_hud_panel_1_eye0_2688x2880.rgba"}) {
            CHECK(IndexOf(a, LAct::Delete, p) >= 0);
        }
        CHECK(IndexOf(a, LAct::Delete, "dlvr_capture/") >= 0);
        CHECK(IndexOf(a, LAct::Delete, "dyinglightgame.exe") < 0 && IndexOf(a, LAct::Delete, "DyingLightGame.exe") < 0);
        const int delOurs = IndexOf(a, LAct::Delete, "DW/Data3.pak");
        const int back = IndexOf(a, LAct::Move, "dlvr/stock/DW/Data3.pak");
        CHECK(delOurs >= 0 && back > delOurs && a[back].dest == "DW/Data3.pak");
        // in flat mode the stock files are already in place: nothing moved back
        const auto f = PlanLauncherRemove(s, r, {"dw/data3.pak"}, true);
        CHECK(IndexOf(f, LAct::Move, "dlvr/stock/DW/Data3.pak") < 0);
        // ...and the game paths hold the STOCK files then: none of them is deleted
        CHECK(IndexOf(f, LAct::Delete, "DW/Data3.pak") < 0 && IndexOf(f, LAct::Delete, "LibOVRRT64_1.dll") < 0);
        CHECK(IndexOf(f, LAct::Delete, "dlvr_ovr2xr.log") >= 0 && f.back().relPath == "dlvr/");
    }
    {   // the scan keeps every folder a release puts a file in (trace 20261124_2135: 1.07's DW\Data files were left by uninstall / flat)
        for (const char* p : {"libovrrt64_1.dll", "dlvr/calib/title.png", "dw/data3.pak", "dw_dlc24/datadlc24_2.pak", "dw/data/common_textures_3_pc.rpack",
                              "dw/data/optimized_dlc29_dx11.mp", "dw/data/dlvr_prompts_pc.rpack", "dw", "dw/data"}) {
            CHECK(detail::ScanWants(p));
        }
        for (const char* p : {"dw/data/sub/file.bin", "dw_dlc24/data/file.bin", "dw_dlc17/datadlc17_2.pak", "other/file.txt", "dw/menu/file.bin"}) {
            CHECK(!detail::ScanWants(p));
        }
        // Remove with a DW/Data release file: ours goes, the game's own neighbour is never named
        Release r2 = r;
        r2.files.push_back({kOurs, "DW/Data/dlvr_prompts_PC.rpack", false});
        DetectedState s;
        s.modPresent = true;
        s.present = {"libovrrt64_1.dll", "dw/data/dlvr_prompts_pc.rpack", "dw/data/menu_common_pc.rpack"};
        const auto a = PlanLauncherRemove(s, r2, {}, false);
        CHECK(IndexOf(a, LAct::Delete, "DW/Data/dlvr_prompts_PC.rpack") >= 0);
        for (const auto& x : a) { CHECK(x.relPath.find("menu_common") == std::string::npos); }
    }
    std::printf("launcher_plan_test: %d failure(s)\n", g_fail);
    return g_fail;
}
