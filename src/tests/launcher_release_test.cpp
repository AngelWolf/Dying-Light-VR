// launcher_release.h - the release manifest, which release is installed, and the launcher's record
// (spec docs/superpowers/specs/2026-11-15-launcher-design.md sections 10 and 12.6).
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "launcher_release.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace dlvr::launcher;

static const char* kA = "0123456789abcdef0123456789abcdef";
static const char* kB = "fedcba9876543210fedcba9876543210";

static std::string Manifest()
{
    return std::string("# release=DLVR-2026.11.20\r\n# launcher=") + kB + "\r\n" + kA + "  LibOVRRT64_1.dll\r\n" + kB +
           "  xinput1_3.dll\r\n" + kA + "  nvngx_dlss.dll dlss-only\r\n" + kB + "  DW\\Data3.pak\r\n";
}

int main()
{
    {   // a valid release
        Release r;
        std::string why;
        CHECK(ParseRelease(Manifest(), "Fixed the thing.", &r, &why));
        CHECK(r.name == "DLVR-2026.11.20");
        CHECK(r.launcherMd5 == kB);
        CHECK(r.notes == "Fixed the thing.");
        CHECK(r.files.size() == 4);
        CHECK(r.files[3].relPath == "DW/Data3.pak");
        CHECK(r.files[2].dlssOnly);
    }
    {   // no release name -> refused, and it says why
        Release r;
        std::string why;
        CHECK(!ParseRelease(std::string(kA) + "  a.dll\n", "", &r, &why));
        CHECK(why.find("release=") != std::string::npos);
    }
    {   // a duplicate path (any case, either slash) -> refused, named
        Release r;
        std::string why;
        const std::string m = std::string("# release=x\n") + kA + "  DW/Data3.pak\n" + kB + "  dw\\data3.PAK\n";
        CHECK(!ParseRelease(m, "", &r, &why));
        CHECK(why.find("data3") != std::string::npos || why.find("Data3") != std::string::npos);
    }
    {   // a malformed line -> refused, not thrown out of the function
        Release r;
        std::string why;
        CHECK(!ParseRelease("# release=x\nnot a manifest line\n", "", &r, &why));
        CHECK(!why.empty());
    }
    Release r;
    std::string why;
    ParseRelease(Manifest(), "", &r, &why);
    {   // every file equal -> Exact (a missing dlss-only file is fine: an AMD install has none)
        std::map<std::string, std::string> disk = {{"libovrrt64_1.dll", kA}, {"xinput1_3.dll", kB}, {"dw/data3.pak", kB}};
        std::vector<std::string> diff;
        CHECK(MatchInstalled(r, disk, true, &diff) == InstallMatch::Exact);
        CHECK(diff.empty());
    }
    {   // one file differs -> Modified, naming it
        std::map<std::string, std::string> disk = {{"libovrrt64_1.dll", kA}, {"xinput1_3.dll", kA}, {"dw/data3.pak", kB}};
        std::vector<std::string> diff;
        CHECK(MatchInstalled(r, disk, true, &diff) == InstallMatch::Modified);
        CHECK(diff.size() == 1 && diff[0] == "xinput1_3.dll");
    }
    {   // one file missing -> Modified, naming it
        std::map<std::string, std::string> disk = {{"libovrrt64_1.dll", kA}, {"xinput1_3.dll", kB}};
        std::vector<std::string> diff;
        CHECK(MatchInstalled(r, disk, true, &diff) == InstallMatch::Modified);
        CHECK(diff.size() == 1 && diff[0] == "DW/Data3.pak");
    }
    {   // 2026-11-16: the four markers the in-game menu writes (dlss_preset.txt SHIPS) are the user's - changed or deleted is
        // NOT damage; a real file beside them still is (the control)
        Release rm;
        std::string w;
        ParseRelease(std::string("# release=x\n") + kA + "  LibOVRRT64_1.dll\n" + kB + "  dlvr/dlss_preset.txt\n" + kB +
                         "  dlvr/aa_method.txt\n" + kB + "  dlvr/dlss_quality.txt\n" + kB + "  dlvr/dlss_sharpness.txt\n" + kB +
                         "  dlvr/force_oculus.txt\n",
                     "", &rm, &w);
        std::vector<std::string> diff;
        std::map<std::string, std::string> menu = {{"libovrrt64_1.dll", kA}, {"dlvr/dlss_preset.txt", kA},
                                                   {"dlvr/aa_method.txt", kA}, {"dlvr/force_oculus.txt", kB}};   // quality / sharpness deleted
        CHECK(MatchInstalled(rm, menu, true, &diff) == InstallMatch::Exact);
        CHECK(diff.empty());
        menu["dlvr/force_oculus.txt"] = kA;
        CHECK(MatchInstalled(rm, menu, true, &diff) == InstallMatch::Modified);
        CHECK(diff.size() == 1 && diff[0] == "dlvr/force_oculus.txt");
    }
    {   // no mod files at all -> None
        std::map<std::string, std::string> disk;
        std::vector<std::string> diff;
        CHECK(MatchInstalled(r, disk, false, &diff) == InstallMatch::None);
    }
    {   // the mod's DLLs present, differing, and no launcher record -> Foreign (a hand or setup install)
        std::map<std::string, std::string> disk = {{"libovrrt64_1.dll", kB}, {"xinput1_3.dll", kA}};
        std::vector<std::string> diff;
        CHECK(MatchInstalled(r, disk, false, &diff) == InstallMatch::Foreign);
    }
    {   // DW/dlvr_pp.mth is rewritten by the mod at every launch (pakgen): a different md5 is NOT damage; a missing one is
        Release rp;
        std::string w;
        ParseRelease(std::string("# release=x\n") + kA + "  LibOVRRT64_1.dll\n" + kB + "  DW/dlvr_pp.mth\n", "", &rp, &w);
        std::vector<std::string> diff;
        std::map<std::string, std::string> rewritten = {{"libovrrt64_1.dll", kA}, {"dw/dlvr_pp.mth", kA}};
        CHECK(MatchInstalled(rp, rewritten, true, &diff) == InstallMatch::Exact);
        CHECK(diff.empty());
        std::map<std::string, std::string> missing = {{"libovrrt64_1.dll", kA}};
        CHECK(MatchInstalled(rp, missing, true, &diff) == InstallMatch::Modified);
        CHECK(diff.size() == 1 && diff[0] == "DW/dlvr_pp.mth");
    }
    {   // the record round-trips
        const std::string t = ReleaseRecordText(r, "flat", "STEAM");
        std::string rel, mode, store;
        CHECK(ParseReleaseRecord(t, &rel, &mode, &store));
        CHECK(rel == "DLVR-2026.11.20" && mode == "flat" && store == "STEAM");
        CHECK(!ParseReleaseRecord("mode=vr\r\n", &rel, &mode, &store));   // no release line -> not a record
    }
    std::printf("launcher_release_test: %d failure(s)\n", g_fail);
    return g_fail;
}
