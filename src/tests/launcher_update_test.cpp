// launcher_update.h + launcher_unzip.h - update source, GitHub release JSON, the release zip reader, staged-release
// checks, update / rollback plans and the self-update decisions (spec section 10; plan Review Focus 5).
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "launcher_update.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace dlvr::launcher;

static Release MakeRel(const std::string& name, const std::string& layerMd5, bool withOld)
{
    std::string m = "# release=" + name + "\n" + layerMd5 + "  LibOVRRT64_1.dll\n" +
                    "22222222222222222222222222222222  xinput1_3.dll\n" +
                    "33333333333333333333333333333333  dlvr/aa_method.txt\n";
    if (withOld) { m += "44444444444444444444444444444444  DW/old_only.pak\n"; }
    Release r;
    std::string why;
    ParseRelease(m, "", &r, &why);
    return r;
}

static int Idx(const std::vector<LAction>& a, LAct k, const std::string& p)
{
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].kind == k && a[i].relPath == p) { return static_cast<int>(i); }
    }
    return -1;
}

static std::string ReadFile(const char* p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int main()
{
    {   // the update source: the repo's latest-release API address (2026-11-15); a local folder still wins
        CHECK(std::string(kRepoUrl) == "https://api.github.com/repos/bymodify-ux/Dying-Light-VR/releases/latest");
        CHECK(UpdateSourceFor(kRepoUrl, L"") == UpdateSource::GitHub);
        CHECK(UpdateSourceFor("", L"") == UpdateSource::None);
        CHECK(UpdateSourceFor(kRepoUrl, L"D:\\rel") == UpdateSource::Local);
        CHECK(UpdateSourceFor("https://github.com/x/y", L"") == UpdateSource::GitHub);
    }
    {   // the release folder: --release wins; else the exe's own folder when release.txt is beside it (Jay: open the exe directly)
        CHECK(ReleaseDirFor(L"D:\\given", L"D:\\exe", true) == L"D:\\given");
        CHECK(ReleaseDirFor(L"", L"D:\\exe", true) == L"D:\\exe");
        CHECK(ReleaseDirFor(L"", L"D:\\exe", false).empty());
    }
    {   // GitHub releases/latest JSON
        const std::string j = R"({"tag_name": "DLVR-2026.12.01", "assets": [
            {"name": "release.txt", "browser_download_url": "https://h/release.txt"},
            {"name": "notes.txt", "browser_download_url": "https://h/notes.txt"},
            {"name": "files.zip", "browser_download_url": "https://h/files.zip"},
            {"name": "settings_schema.json", "browser_download_url": "https://h/s.json"},
            {"name": "DyingLightVR_Launcher.exe", "browser_download_url": "https://h/l.exe"},
            {"name": "retired.txt", "browser_download_url": "https://h/retired.txt"}]})";
        LatestInfo li;
        std::string why;
        CHECK(ParseLatestRelease(j, &li, &why));
        CHECK(li.tag == "DLVR-2026.12.01" && li.zipUrl == "https://h/files.zip" && li.launcherUrl == "https://h/l.exe");
        CHECK(li.notesUrl == "https://h/notes.txt" && li.schemaUrl == "https://h/s.json" && li.manifestUrl == "https://h/release.txt");
        CHECK(li.retiredUrl == "https://h/retired.txt");
        const std::string noZip = R"({"tag_name": "x", "assets": [{"name": "release.txt", "browser_download_url": "u"}]})";
        CHECK(!ParseLatestRelease(noZip, &li, &why) && why.find("files.zip") != std::string::npos);
        CHECK(NeedsUpdate("DLVR-2026.11.20", li.tag) && !NeedsUpdate("DLVR-2026.12.01", "DLVR-2026.12.01") && !NeedsUpdate("a", ""));
    }
    {   // the zip reader, on a DEFLATED zip written by Windows' tar.exe
        { std::ofstream a("upd_a.txt", std::ios::binary); a << std::string(5000, 'a') << "end"; }
        { std::ofstream b("upd_b.txt", std::ios::binary); b << "short"; }
        CHECK(std::system("C:\\Windows\\System32\\tar.exe -a -cf upd_test.zip upd_a.txt upd_b.txt") == 0);
        const std::string z = ReadFile("upd_test.zip");
        std::vector<ZipEntry> es;
        std::string why;
        CHECK(ZipEntries(z, &es, &why));
        CHECK(es.size() == 2);
        std::string data;
        CHECK(es.size() == 2 && ZipExtract(z, es[0], &data, &why) && data == std::string(5000, 'a') + "end");
        CHECK(es.size() == 2 && es[0].method == 8);   // deflated: puff was exercised
        CHECK(es.size() == 2 && ZipExtract(z, es[1], &data, &why) && data == "short");
        // a truncated zip is refused
        std::vector<ZipEntry> es2;
        CHECK(!ZipEntries(z.substr(0, z.size() / 2), &es2, &why) && !why.empty());
        // a flipped byte in the compressed data: CRC refused, entry named
        std::string bad = z;
        if (!es.empty()) { bad[static_cast<size_t>(es[0].dataOffset) + 3] ^= 0x55; }
        CHECK(!es.empty() && !ZipExtract(bad, es[0], &data, &why) && why.find("upd_a.txt") != std::string::npos);
        std::remove("upd_a.txt");
        std::remove("upd_b.txt");
        std::remove("upd_test.zip");
    }
    const Release from = MakeRel("R1", "11111111111111111111111111111111", true);
    const Release to = MakeRel("R2", "99999999999999999999999999999999", false);
    {   // a staged release is checked WHOLE before anything is touched (Review Focus 5)
        std::map<std::string, std::string> staged = {{"libovrrt64_1.dll", "99999999999999999999999999999999"},
                                                     {"xinput1_3.dll", "22222222222222222222222222222222"}};
        auto bad = StagedProblems(to, staged);
        CHECK(bad.size() == 1 && bad[0] == "dlvr/aa_method.txt");
        staged["dlvr/aa_method.txt"] = "33333333333333333333333333333333";
        CHECK(StagedProblems(to, staged).empty());
        staged["xinput1_3.dll"] = "00000000000000000000000000000000";
        CHECK(StagedProblems(to, staged).size() == 1);
    }
    {   // update: only the changed file moves to dlvr/previous/ and is copied; a removed file moves out; a user marker kept;
        // settings kept with retired keys removed and old defaults reset
        UpdateInputs in{from, to, "body_grab=0\r\nold_key=5\r\ncomfort_turn_mode=0\r\n", "", {{"body_grab", "bool", "1", {"0"}}},
                        {"old_key"}, {"libovrrt64_1.dll", "xinput1_3.dll", "dlvr/aa_method.txt", "dw/old_only.pak"}, false};
        const auto a = PlanUpdate(in);
        const int mv = Idx(a, LAct::Move, "LibOVRRT64_1.dll");
        const int cp = Idx(a, LAct::Copy, "LibOVRRT64_1.dll");
        CHECK(mv >= 0 && cp > mv && a[mv].dest == "dlvr/previous/LibOVRRT64_1.dll");
        CHECK(Idx(a, LAct::Copy, "xinput1_3.dll") < 0 && Idx(a, LAct::Move, "xinput1_3.dll") < 0);   // unchanged
        CHECK(Idx(a, LAct::Move, "DW/old_only.pak") >= 0);                                            // gone from R2
        CHECK(Idx(a, LAct::Copy, "dlvr/aa_method.txt") < 0);                                          // the user's marker
        const int st = Idx(a, LAct::WriteText, "dlvr/ovr2xr.txt");
        CHECK(st >= 0 && a[st].text == "body_grab=1\r\ncomfort_turn_mode=0\r\n");
        CHECK(Idx(a, LAct::WriteText, "dlvr/previous/release.txt") >= 0);
        // flat mode: game files go to dlvr/flat_off/, dlvr/ files stay where they are
        in.flatMode = true;
        in.presentLower = {"dlvr/flat_off/libovrrt64_1.dll", "dlvr/flat_off/xinput1_3.dll", "dlvr/aa_method.txt"};
        const auto f = PlanUpdate(in);
        const int fm = Idx(f, LAct::Move, "dlvr/flat_off/LibOVRRT64_1.dll");
        const int fc = Idx(f, LAct::Copy, "LibOVRRT64_1.dll");
        CHECK(fm >= 0 && fc >= 0 && f[fc].dest == "dlvr/flat_off/LibOVRRT64_1.dll");
        // 2026-11-16: a file both releases ship EQUAL but damaged on disk (MatchInstalled's list) is copied too - the update
        // repairs; it is not moved to previous/ (the damaged copy is not kept). The control: without it in the list, no copy.
        in.flatMode = false;
        in.presentLower = {"libovrrt64_1.dll", "xinput1_3.dll", "dlvr/aa_method.txt", "dw/old_only.pak"};
        in.differing = {"xinput1_3.dll"};
        const auto r = PlanUpdate(in);
        const int rc = Idx(r, LAct::Copy, "xinput1_3.dll");
        CHECK(rc >= 0 && r[rc].dest == "xinput1_3.dll" && r[rc].md5 == "22222222222222222222222222222222");
        CHECK(Idx(r, LAct::Move, "xinput1_3.dll") < 0);
        in.differing = {"XINPUT1_3.DLL"};   // case-insensitive like every other path rule
        CHECK(Idx(PlanUpdate(in), LAct::Copy, "xinput1_3.dll") >= 0);
        in.differing.clear();
        CHECK(Idx(PlanUpdate(in), LAct::Copy, "xinput1_3.dll") < 0);
    }
    {   // rollback: the previous files back, the new-only files deleted, the record rewritten
        const Release cur = MakeRel("R2", "99999999999999999999999999999999", false);
        Release cur2 = cur;
        cur2.files.push_back({"55555555555555555555555555555555", "DW/new_only.pak", false});
        const auto a = PlanRollback(from, cur2, {"dlvr/previous/libovrrt64_1.dll", "dlvr/previous/dw/old_only.pak"}, false,
                                    "release=R1\r\nmode=vr\r\n");
        const int b = Idx(a, LAct::Move, "dlvr/previous/LibOVRRT64_1.dll");
        CHECK(b >= 0 && a[b].dest == "LibOVRRT64_1.dll");
        CHECK(Idx(a, LAct::Move, "dlvr/previous/DW/old_only.pak") >= 0);
        CHECK(Idx(a, LAct::Delete, "DW/new_only.pak") >= 0);
        CHECK(!a.empty() && a.back().relPath == "dlvr/launcher_installed.txt" && a.back().text.find("R1") != std::string::npos);
    }
    {   // self-update decisions
        CHECK(OnStartStep(true, true) == SelfStep::DeleteOld);
        CHECK(OnStartStep(false, true) == SelfStep::DeleteOld);
        CHECK(OnStartStep(false, false) == SelfStep::None);
        CHECK(OnWaitEndStep(true) == SelfStep::None);
        CHECK(OnWaitEndStep(false) == SelfStep::RestoreOld);
    }
    std::printf("launcher_update_test: %d failure(s)\n", g_fail);
    return g_fail;
}
