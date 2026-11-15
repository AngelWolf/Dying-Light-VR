// Standalone unit test for src/src/setup/setup_store.h - NOT part of any DLL build.
// docs/superpowers/specs/2026-10-17-installer-design.md 8.4 / 8.4b (Epic and GOG, the store tag, the Program Files
// refusal). Exit code = number of failures.
#include "setup_store.h"

#include <cstdio>
#include <vector>

using namespace dlvr::setup;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static bool Has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

static void storeof_each()
{
    CHECK(StoreOf({true, true, false, true}) == Store::Gog);     // goggame info wins
    CHECK(StoreOf({true, false, true, false}) == Store::Epic);   // .egstore before a steam-looking path
    CHECK(StoreOf({true, false, false, false}) == Store::Steam);
    CHECK(StoreOf({false, true, false, false}) == Store::Steam);
    CHECK(StoreOf({false, false, false, false}) == Store::Unknown);
    CHECK(std::string(StoreTag(Store::Steam)) == "STEAM");
    CHECK(std::string(StoreTag(Store::Epic)) == "EPIC");
    CHECK(std::string(StoreTag(Store::Gog)) == "GOG");
    CHECK(std::string(StoreTag(Store::Unknown)) == "UNKNOWN STORE");
}
static void epic_manifest_dl()
{
    std::string loc;
    CHECK(EpicManifestDyingLight(
        "{\n\t\"FormatVersion\": 0,\n\t\"DisplayName\": \"Dying Light\",\n\t\"InstallLocation\": \"D:\\\\Games\\\\DyingLight\"\n}",
        &loc));
    CHECK(loc == "D:\\Games\\DyingLight");
    loc.clear();
    CHECK(EpicManifestDyingLight("{\"InstallLocation\":\"E:/Epic/DyingLight\",\"DisplayName\":\"dying light enhanced edition\"}", &loc));
    CHECK(loc == "E:/Epic/DyingLight");
}
static void epic_manifest_other_game_false()
{
    std::string loc = "unchanged";
    CHECK(!EpicManifestDyingLight("{\"DisplayName\": \"Fortnite\", \"InstallLocation\": \"C:\\\\Fortnite\"}", &loc));
    CHECK(loc == "unchanged");
}
static void epic_manifest_missing_location_false()
{
    std::string loc;
    CHECK(!EpicManifestDyingLight("{\"DisplayName\": \"Dying Light\"}", &loc));
    CHECK(!EpicManifestDyingLight("{\"DisplayName\": \"Dying Light\", \"InstallLocation\": \"\"}", &loc));
    CHECK(!EpicManifestDyingLight("{\"DisplayName\": \"Dying Light\", \"InstallLocation\": 5}", &loc));
}
static void epic_manifest_bom_and_unicode()
{
    std::string loc;
    CHECK(EpicManifestDyingLight("\xEF\xBB\xBF{\"DisplayName\": \"Dying Light\", \"InstallLocation\": \"D:\\\\Caf\\u00e9\\/DL\"}", &loc));
    CHECK(loc == "D:\\Caf\xC3\xA9/DL");
}
static void epic_manifest_garbage_false()
{
    std::string loc;
    CHECK(!EpicManifestDyingLight("", &loc));
    CHECK(!EpicManifestDyingLight("{", &loc));
    CHECK(!EpicManifestDyingLight("{\"DisplayName\": \"Dying Light\", \"InstallLocation\": \"D:\\\\Gam", &loc));
    CHECK(!EpicManifestDyingLight("{\"DisplayName\": \"Dying Light\", \"InstallLocation\": \"bad\\u12\"}", &loc));
}
static void refusal_texts_name_store_and_folder()
{
    const std::string f = "C:\\Program Files\\Epic Games\\DyingLight";
    const std::string s = ProgramFilesRefusal(Store::Steam, f);
    const std::string e = ProgramFilesRefusal(Store::Epic, f);
    const std::string g = ProgramFilesRefusal(Store::Gog, f);
    const std::string u = ProgramFilesRefusal(Store::Unknown, f);
    CHECK(Has(s, "Settings") && Has(s, "Storage") && Has(s, f.c_str()));
    CHECK(Has(e, "install it again") && Has(e, f.c_str()));
    CHECK(Has(g, "Manage installation") && Has(g, "Move") && Has(g, f.c_str()));
    CHECK(Has(u, f.c_str()) && Has(u, "Program Files"));
}
// Review finding 13: the Program Files test normalises the path first - an Epic manifest may hold forward slashes, and a
// trailing separator or different case must not slip past the refusal.
static void pathunder_normalises()
{
    const std::vector<std::string> roots = {"C:\\Program Files", "C:\\Program Files (x86)"};
    CHECK(PathUnder("C:\\Program Files\\Epic Games\\DyingLight", roots));
    CHECK(PathUnder("c:/program files/Epic Games/DyingLight", roots));
    CHECK(PathUnder("C:\\PROGRAM FILES (X86)\\GOG Galaxy\\Games\\Dying Light\\", roots));
    CHECK(PathUnder("C:\\Program Files", roots));
    CHECK(!PathUnder("C:\\Program FilesX\\Game", roots));
    CHECK(!PathUnder("D:\\Program Files\\Game", roots));
    CHECK(!PathUnder("D:\\Games\\Dying Light", roots));
    CHECK(PathUnder("C:\\Program Files\\Game", {"C:/Program Files/"}));
}
static void untested_only_epic_gog()
{
    CHECK(UntestedNotice(Store::Steam).empty());
    CHECK(UntestedNotice(Store::Unknown).empty());
    CHECK(UntestedNotice(Store::Epic) ==
          "Epic Games version: not tested with this build of the mod. Please report how it goes.");
    CHECK(UntestedNotice(Store::Gog) == "GOG version: not tested with this build of the mod. Please report how it goes.");
}

int main()
{
    storeof_each();
    epic_manifest_dl();
    epic_manifest_other_game_false();
    epic_manifest_missing_location_false();
    epic_manifest_bom_and_unicode();
    epic_manifest_garbage_false();
    refusal_texts_name_store_and_folder();
    untested_only_epic_gog();
    pathunder_normalises();
    std::printf("setup_store_test: %d failure(s)\n", g_fail);
    return g_fail;
}
