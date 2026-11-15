// The launcher's Windows side (plan docs/superpowers/plans/2026-11-15-launcher.md Task 6): the real IFileOps over a game
// folder, a staged release folder and Documents; the game-folder scan; md5 of files; the known-folder paths.
#pragma once

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "launcher_mode.h"
#include "setup_plan.h"

namespace dlvr::launcher {

namespace fs = std::filesystem;

// Documents (FOLDERID_Documents - follows a OneDrive / moved Documents folder; Review Focus 1). Empty if unknown.
fs::path DocumentsDir();
fs::path DesktopDir();
fs::path VideoScrPath();   // <Documents>\DyingLight\out\settings\video.scr

std::string Md5File(const fs::path& p);   // "" = absent / unreadable
bool ReadAllBytes(const fs::path& p, std::string* out);
bool GameIsRunning();                      // DyingLightGame.exe in the process list (Toolhelp)
std::string W2U(const std::wstring& w);
std::wstring U2W(const std::string& u);

// The setup's DetectedState for this folder, scanned past any unreadable entry (S02 item 4: the setup stopped at the first).
setup::DetectedState Detect(const fs::path& game, int* unreadable);
// game-relative lower-case path -> md5 for each of these paths that exists
std::map<std::string, std::string> DiskMd5(const fs::path& game, const std::vector<std::string>& relPaths);
// files under a folder, relative to it, forward slashes, original case
std::vector<std::string> ListFiles(const fs::path& dir);

class RealOps : public IFileOps {
public:
    RealOps(fs::path game, fs::path stagedRelease) : game_(std::move(game)), staged_(std::move(stagedRelease)) {}
    bool GameRunning() override;
    bool CopyFromRelease(const std::string& releasePath, const std::string& to, std::string* why) override;
    bool Copy(const std::string& from, const std::string& to, std::string* why) override;
    bool Move(const std::string& from, const std::string& to, std::string* why) override;
    bool Write(const std::string& path, const std::string& text, std::string* why) override;
    bool Remove(const std::string& path, std::string* why) override;
    std::string Md5(const std::string& path) override;
    bool RunVcRedist(std::string* why) override;
    fs::path Resolve(const std::string& rel) const;   // "<docs>/video.scr" -> VideoScrPath()

private:
    fs::path game_, staged_;
};

}  // namespace dlvr::launcher
