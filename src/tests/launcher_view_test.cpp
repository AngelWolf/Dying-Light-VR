// launcher_view.h - what the window allows and says (spec sections 4, 5, 8, 12.8).
#include <cstdio>
#include <string>

#include "launcher_view.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace dlvr::launcher;

static LauncherView Ready()
{
    LauncherView v;
    v.haveGame = true;
    v.match = InstallMatch::Exact;
    v.mode = Mode::Vr;
    return v;
}

int main()
{
    {   // nothing is written while the game runs, a job is busy, there is no game, or the game is under Program Files
        LauncherView v = Ready();
        CHECK(CanWrite(v));
        v.gameRunning = true;
        CHECK(!CanWrite(v));
        v = Ready(); v.busy = true; CHECK(!CanWrite(v));
        v = Ready(); v.haveGame = false; CHECK(!CanWrite(v));
        v = Ready(); v.programFiles = true; CHECK(!CanWrite(v));
    }
    {   // Launch in VR needs the mod installed; Launch Flat works whatever is installed
        LauncherView v = Ready();
        CHECK(CanLaunchVr(v) && CanLaunchFlat(v));
        v.match = InstallMatch::None;
        CHECK(!CanLaunchVr(v) && CanLaunchFlat(v));
        v.match = InstallMatch::Foreign;
        CHECK(!CanLaunchVr(v));
        CHECK(!CanLaunchFlat(v));   // an older install's mod cannot be switched off by the launcher: uninstall first
        v = Ready(); v.gameRunning = true;
        CHECK(!CanLaunchVr(v) && !CanLaunchFlat(v));
    }
    {   // the mode line and the flat button say when the mod is off
        LauncherView v = Ready();
        v.mode = Mode::Flat;
        CHECK(std::string(FlatButtonLabel(v)) == "Launch Flat (mod is off)");
        CHECK(ModeLine(v).find("OFF") != std::string::npos && ModeLine(v).find("Launch in VR") != std::string::npos);
        v.mode = Mode::Vr;
        CHECK(std::string(FlatButtonLabel(v)) == "Launch Flat");
        CHECK(ModeLine(v).find("ON") != std::string::npos);
    }
    {   // the main install button
        LauncherView v = Ready();
        v.match = InstallMatch::None; v.haveRelease = true;
        CHECK(std::string(InstallButtonLabel(v)) == "Install");
        v.match = InstallMatch::Exact; v.newerRelease = true;
        CHECK(std::string(InstallButtonLabel(v)) == "Update");
        v.newerRelease = false;
        CHECK(std::string(InstallButtonLabel(v)) == "Installed");
        v.match = InstallMatch::Modified;
        CHECK(std::string(InstallButtonLabel(v)) == "Repair");
        v.newerRelease = true;   // 2026-11-16 (a tester's report): a modified install with a newer release updates - Repair would refuse
        CHECK(std::string(InstallButtonLabel(v)) == "Update");
        CHECK(OffersUpdate(v));
        v.newerRelease = false;
        CHECK(!OffersUpdate(v));
        v.match = InstallMatch::Exact; v.newerRelease = true;
        CHECK(OffersUpdate(v));
        v.match = InstallMatch::None;
        CHECK(!OffersUpdate(v));
        v.match = InstallMatch::Foreign;
        CHECK(!OffersUpdate(v));
        v.newerRelease = false;
        v.match = InstallMatch::Foreign;
        CHECK(std::string(InstallButtonLabel(v)) == "Uninstall first");
    }
    {   // the record's mode
        CHECK(ModeFromRecord("release=R1\r\nmode=flat\r\n") == Mode::Flat);
        CHECK(ModeFromRecord("release=R1\r\nmode=vr\r\n") == Mode::Vr);
        CHECK(ModeFromRecord("") == Mode::Unknown);
    }
    {   // the controls image: square, fitted, centred, never drawn larger than its own pixels
        const FitRect r = FitSquare(1200.0f, 600.0f, 2000);
        CHECK(r.side == 600.0f && r.x == 300.0f && r.y == 0.0f);
        const FitRect big = FitSquare(4000.0f, 3000.0f, 2000);
        CHECK(big.side == 2000.0f && big.x == 1000.0f && big.y == 500.0f);
        const FitRect none = FitSquare(800.0f, 600.0f, 0);
        CHECK(none.side == 0.0f);
    }
    {   // Jay's controls picture is 2560 x 1440 (16:9): fit by its own shape, centred, never above its own pixels
        const FitRect r = FitImage(1232.0f, 560.0f, 2560, 1440);   // the Controls tab's area at 100%: height-limited
        CHECK(r.h == 560.0f && r.w > 995.0f && r.w < 996.0f && r.y == 0.0f && r.x > 118.0f && r.x < 119.0f);
        const FitRect wide = FitImage(1000.0f, 900.0f, 2560, 1440);  // width-limited
        CHECK(wide.w == 1000.0f && wide.h == 562.5f && wide.x == 0.0f);
        const FitRect big = FitImage(4000.0f, 3000.0f, 2560, 1440);  // never above its own size
        CHECK(big.w == 2560.0f && big.h == 1440.0f);
        CHECK(FitImage(800.0f, 600.0f, 0, 0).w == 0.0f);
    }
    std::printf("launcher_view_test: %d failure(s)\n", g_fail);
    return g_fail;
}
