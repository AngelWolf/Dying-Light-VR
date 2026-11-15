// launcher_launch.h - the command that starts the game per store (spec 8, 12.5; Epic / GOG untested).
#include <cstdio>
#include <string>

#include "launcher_launch.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace dlvr::launcher;
using dlvr::setup::Store;

int main()
{
    {
        const LaunchCmd c = LaunchFor(Store::Steam, L"D:\\Steam\\Dying Light", "", "", L"");
        CHECK(c.file == L"steam://rungameid/239140" && c.viaShell && c.args.empty());
    }
    {
        const LaunchCmd c = LaunchFor(Store::Epic, L"D:\\Epic\\DyingLight", "Fox", "", L"");
        CHECK(c.file == L"com.epicgames.launcher://apps/Fox?action=launch&silent=true" && c.viaShell);
    }
    {   // Epic with no AppName: nothing to run, and it says why
        const LaunchCmd c = LaunchFor(Store::Epic, L"D:\\Epic\\DyingLight", "", "", L"");
        CHECK(c.file.empty() && c.why.find("Epic") != std::string::npos);
    }
    {
        const LaunchCmd c = LaunchFor(Store::Gog, L"E:\\GOG\\Dying Light", "", "1207658997", L"C:\\GOG Galaxy\\GalaxyClient.exe");
        CHECK(c.file == L"C:\\GOG Galaxy\\GalaxyClient.exe" && !c.viaShell);
        CHECK(c.args == L"/command=runGame /gameId=1207658997 /path=\"E:\\GOG\\Dying Light\"");
    }
    {   // GOG without Galaxy: the exe itself (GOG builds carry no launcher DRM)
        const LaunchCmd c = LaunchFor(Store::Gog, L"E:\\GOG\\Dying Light", "", "1207658997", L"");
        CHECK(c.file == L"E:\\GOG\\Dying Light\\DyingLightGame.exe" && !c.viaShell);
    }
    {   // unknown store: the exe
        const LaunchCmd c = LaunchFor(Store::Unknown, L"F:\\Games\\DL", "", "", L"");
        CHECK(c.file == L"F:\\Games\\DL\\DyingLightGame.exe");
    }
    {   // Epic manifests: the main game's AppName; a DLC manifest (AppName != MainGameAppName) and other games refused
        std::string app;
        CHECK(EpicAppName(R"({"DisplayName": "Dying Light Enhanced Edition", "AppName": "Fox", "MainGameAppName": "Fox"})", &app));
        CHECK(app == "Fox");
        CHECK(!EpicAppName(R"({"DisplayName": "Dying Light - Hellraid", "AppName": "FoxDLC1", "MainGameAppName": "Fox"})", &app));
        CHECK(!EpicAppName(R"({"DisplayName": "Other Game", "AppName": "X"})", &app));
        CHECK(!EpicAppName("{not json", &app));
        CHECK(EpicAppName("\xEF\xBB\xBF{\"DisplayName\": \"Dying Light\", \"AppName\": \"Fox\"}", &app) && app == "Fox");
    }
    {
        std::string id;
        CHECK(GogIdFromInfoName("goggame-1207658997.info", &id) && id == "1207658997");
        CHECK(!GogIdFromInfoName("goggame-.info", &id));
        CHECK(!GogIdFromInfoName("goggame-12a.info", &id));
        CHECK(!GogIdFromInfoName("readme.txt", &id));
    }
    std::printf("launcher_launch_test: %d failure(s)\n", g_fail);
    return g_fail;
}
