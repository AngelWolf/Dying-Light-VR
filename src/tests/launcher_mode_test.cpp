// launcher_mode.h - the VR / flat switch and the executor with rollback (spec
// docs/superpowers/specs/2026-11-15-launcher-design.md section 8; plan Review Focus 3 and 4).
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "launcher_mode.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace dlvr::launcher;

// An in-memory game folder. Paths compare case-insensitively, as on Windows.
struct FakeOps : IFileOps {
    std::map<std::string, std::string> files;   // lower path -> content
    std::map<std::string, std::string> release; // files.zip
    int actions = 0;
    int failAt = -1;          // the Nth file operation fails (0-based)
    int gameStartsAt = -1;    // GameRunning() turns true at the Nth check
    int checks = 0;
    std::string corruptOnMoveTo;   // a Move to this path lands with different bytes

    static std::string L(std::string s) { return detail::LowerPath(s); }
    bool Op(std::string* why) { if (actions++ == failAt) { *why = "Access is denied."; return false; } return true; }
    bool GameRunning() override { return gameStartsAt >= 0 && checks++ >= gameStartsAt; }
    bool CopyFromRelease(const std::string& rel, const std::string& to, std::string* why) override
    {
        if (!Op(why)) { return false; }
        const auto it = release.find(L(rel));
        if (it == release.end()) { *why = "not in the release"; return false; }
        files[L(to)] = it->second;
        return true;
    }
    bool Copy(const std::string& from, const std::string& to, std::string* why) override
    {
        if (!Op(why)) { return false; }
        if (!files.count(L(from))) { *why = "missing"; return false; }
        files[L(to)] = files[L(from)];
        return true;
    }
    bool Move(const std::string& from, const std::string& to, std::string* why) override
    {
        if (!Op(why)) { return false; }
        if (!files.count(L(from))) { *why = "missing"; return false; }
        files[L(to)] = L(to) == L(corruptOnMoveTo) ? std::string("garbage") : files[L(from)];
        files.erase(L(from));
        return true;
    }
    bool Write(const std::string& p, const std::string& text, std::string* why) override
    {
        if (!Op(why)) { return false; }
        files[L(p)] = text;
        return true;
    }
    bool Remove(const std::string& p, std::string* why) override
    {
        if (!Op(why)) { return false; }
        if (!p.empty() && p.back() == '/') {
            for (auto it = files.begin(); it != files.end();) {
                it = it->first.compare(0, p.size(), L(p)) == 0 ? files.erase(it) : std::next(it);
            }
        } else {
            files.erase(L(p));
        }
        return true;
    }
    std::string Md5(const std::string& p) override
    {
        const auto it = files.find(L(p));
        return it == files.end() ? std::string() : "md5:" + it->second;   // the content stands in for its hash
    }
    bool RunVcRedist(std::string*) override { return true; }
};

static Release Rel()
{
    Release r;
    std::string why;
    ParseRelease("# release=R1\n"
                 "11111111111111111111111111111111  LibOVRRT64_1.dll\n"
                 "11111111111111111111111111111111  xinput1_3.dll\n"
                 "11111111111111111111111111111111  DW/Data3.pak\n"
                 "11111111111111111111111111111111  DW_DLC24/DataDLC24_2.pak\n"
                 "11111111111111111111111111111111  dlvr/afw_keep_rift.txt\n",
                 "", &r, &why);
    return r;
}

static FakeOps VrInstall()
{
    FakeOps f;
    f.files = {{"libovrrt64_1.dll", "layer"}, {"xinput1_3.dll", "proxy"}, {"dw/data3.pak", "our data3"},
               {"dw_dlc24/datadlc24_2.pak", "hud pak"}, {"dlvr/afw_keep_rift.txt", "x"}, {"dw/dlvr_binds.scr", "binds"},
               {"dlvr/stock/dw/data3.pak", "stock data3"}, {"dlvr/ovr2xr.txt", "settings"}, {"dlvr/arm_fit.txt", "arms"},
               {"dyinglightgame.exe", "game"}, {"dw/data0.pak", "data0"}};
    return f;
}

static std::set<std::string> Present(const FakeOps& f)
{
    std::set<std::string> s;
    for (const auto& kv : f.files) { s.insert(kv.first); }
    return s;
}

static const char* kScr = "!OculusEnabled()               // enable support for Oculus\r\nVSync(0)\r\nOculusEnabled()\r\nMotionBlur(0)\r\n";

int main()
{
    const Release r = Rel();
    {   // RemoveOculusEnabled drops only the active line; the "!" reference line and everything else stay, CRLF kept
        CHECK(RemoveOculusEnabled(kScr) == "!OculusEnabled()               // enable support for Oculus\r\nVSync(0)\r\nMotionBlur(0)\r\n");
        CHECK(RemoveOculusEnabled("VSync(0)\n  OculusEnabled()  \nX(1)\n") == "VSync(0)\nX(1)\n");
        CHECK(RemoveOculusEnabled("VSync(0)\r\n") == "VSync(0)\r\n");
    }
    {   // to flat: every mod file out to dlvr/flat_off/, the stock Data3 back; dlvr/ settings untouched
        FakeOps f = VrInstall();
        const auto a = PlanToFlat(r, Present(f), {"DW/Data3.pak"}, kScr, "release=R1\r\nmode=flat\r\nstore=STEAM\r\n");
        const ExecResult x = ExecuteWithRollback(a, f);
        CHECK(x.ok);
        CHECK(f.files["dw/data3.pak"] == "stock data3");
        CHECK(!f.files.count("libovrrt64_1.dll") && !f.files.count("xinput1_3.dll") && !f.files.count("dw_dlc24/datadlc24_2.pak"));
        CHECK(f.files["dlvr/flat_off/libovrrt64_1.dll"] == "layer" && f.files["dlvr/flat_off/dw/data3.pak"] == "our data3");
        CHECK(f.files["dlvr/flat_off/dw/dlvr_binds.scr"] == "binds");
        CHECK(f.files["dlvr/ovr2xr.txt"] == "settings" && f.files["dlvr/arm_fit.txt"] == "arms");
        CHECK(f.files["dlvr/afw_keep_rift.txt"] == "x");   // dlvr/ files are not moved
        CHECK(f.files["<docs>/video.scr"].find("\nOculusEnabled()") == std::string::npos);
        CHECK(f.files["dlvr/launcher_installed.txt"].find("mode=flat") != std::string::npos);
        // and back to VR: the exact inverse
        std::set<std::string> flat;
        for (const auto& kv : f.files) {
            if (kv.first.compare(0, 14, "dlvr/flat_off/") == 0) { flat.insert(kv.first.substr(14)); }
        }
        const auto b = PlanToVr(flat, Present(f), "release=R1\r\nmode=vr\r\nstore=STEAM\r\n");
        CHECK(ExecuteWithRollback(b, f).ok);
        FakeOps want = VrInstall();
        for (const auto& kv : want.files) { CHECK(f.files[kv.first] == kv.second); }
        CHECK(f.files["dlvr/launcher_installed.txt"].find("mode=vr") != std::string::npos);
        for (const auto& kv : f.files) { CHECK(kv.first.compare(0, 14, "dlvr/flat_off/") != 0); }
    }
    {   // a failure at action 3 of the switch: stops, the completed moves are reversed, the file and the OS error named
        FakeOps f = VrInstall();
        const auto a = PlanToFlat(r, Present(f), {"DW/Data3.pak"}, "", "release=R1\r\nmode=flat\r\n");
        f.failAt = 2;
        const ExecResult x = ExecuteWithRollback(a, f);
        CHECK(!x.ok);
        CHECK(x.failedPath == a[2].relPath);
        CHECK(x.why.find("Access is denied.") != std::string::npos);
        CHECK(x.rolledBack.size() == 2);
        FakeOps want = VrInstall();
        for (const auto& kv : want.files) { CHECK(f.files[kv.first] == kv.second); }
    }
    {   // the game starts mid-switch (Review Focus 3): stop + roll back, naming DyingLightGame.exe
        FakeOps f = VrInstall();
        const auto a = PlanToFlat(r, Present(f), {"DW/Data3.pak"}, "", "release=R1\r\nmode=flat\r\n");
        f.gameStartsAt = 3;
        const ExecResult x = ExecuteWithRollback(a, f);
        CHECK(!x.ok && x.why.find("DyingLightGame.exe") != std::string::npos);
        FakeOps want = VrInstall();
        for (const auto& kv : want.files) { CHECK(f.files[kv.first] == kv.second); }
    }
    {   // a moved file whose bytes change in the move -> failure, named
        FakeOps f = VrInstall();
        f.corruptOnMoveTo = "dlvr/flat_off/xinput1_3.dll";
        const auto a = PlanToFlat(r, Present(f), {}, "", "release=R1\r\nmode=flat\r\n");
        const ExecResult x = ExecuteWithRollback(a, f);
        CHECK(!x.ok && x.failedPath == "xinput1_3.dll" && x.why.find("md5") != std::string::npos);
    }
    {   // an install copy checks its md5; the 5 s re-check names a vanished DLL (Review Focus 4); the record waits for it
        FakeOps f;
        f.release = {{"libovrrt64_1.dll", "layer"}};
        std::vector<LAction> a = {{LAct::Copy, "LibOVRRT64_1.dll", "LibOVRRT64_1.dll", "md5:layer", "", false},
                                  {LAct::WriteText, "dlvr/launcher_installed.txt", "", "", "release=R1\r\n", true}};
        CHECK(ExecuteWithRollback(a, f).ok);
        CHECK(!f.files.count("dlvr/launcher_installed.txt"));   // afterRecheck: not yet
        f.files.erase("libovrrt64_1.dll");                       // antivirus
        const auto gone = Recheck(a, f);
        CHECK(gone.size() == 1 && gone[0] == "LibOVRRT64_1.dll");
        f.files["libovrrt64_1.dll"] = "layer";
        CHECK(Recheck(a, f).empty());
        CHECK(ExecuteAfterRecheck(a, f).ok && f.files.count("dlvr/launcher_installed.txt"));
        // a copy that lands with the wrong bytes fails at once
        FakeOps g;
        g.release = {{"libovrrt64_1.dll", "other"}};
        CHECK(!ExecuteWithRollback(a, g).ok);
    }
    {   // a failed delete keeps the OS's own reason (S02 item 2)
        FakeOps f = VrInstall();
        f.failAt = 0;
        const ExecResult x = ExecuteWithRollback({{LAct::Delete, "xinput1_3.dll", "", "", "", false}}, f);
        CHECK(!x.ok && x.why.find("Access is denied.") != std::string::npos);
    }
    std::printf("launcher_mode_test: %d failure(s)\n", g_fail);
    return g_fail;
}
