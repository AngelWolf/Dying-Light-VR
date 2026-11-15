// The payload read from resources compiled into DyingLightVR_Setup.exe, or from a folder (--payload, the tests).
// Ids: setup_payload.h. docs/superpowers/specs/2026-10-17-installer-design.md 8.3.
#include "setup_payload.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace dlvr::setup {

bool ReadResource(int id, std::string* bytes)
{
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!r) { return false; }
    const DWORD n = SizeofResource(nullptr, r);
    if (n == 0) {   // an empty file (e.g. the marker dlvr\no_visrange_watch.txt) - no data to lock
        bytes->clear();
        return true;
    }
    HGLOBAL g = LoadResource(nullptr, r);
    const void* p = g ? LockResource(g) : nullptr;
    if (!p) { return false; }
    bytes->assign(static_cast<const char*>(p), n);
    return true;
}

namespace {

class Embedded : public Payload {
public:
    Embedded()
    {
        std::string text;
        if (ReadResource(kResManifest, &text)) {
            haveManifest_ = true;
            manifest_ = text;
            try {
                entries_ = ParseManifest(text);
            } catch (const std::exception&) {
                entries_.clear();
            }
        }
    }
    bool Read(const std::string& relPath, std::string* bytes, std::string* why) override
    {
        if (relPath == "vc_redist.x64.exe") {   // resource 101: not a manifest entry (it is run, never copied to the game)
            if (ReadResource(kResVcRedist, bytes)) { return true; }
            *why = "the Visual C++ runtime installer is missing from this setup program (resource 101)";
            return false;
        }
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].relPath == relPath) {
                // rc.exe drops a zero-byte file, so an entry whose md5 is the EMPTY md5 has no resource: it is written
                // as an empty file (and its md5 is still checked on disk after writing).
                if (entries_[i].md5 == "d41d8cd98f00b204e9800998ecf8427e") {
                    bytes->clear();
                    return true;
                }
                if (ReadResource(kResEntryBase + static_cast<int>(i), bytes)) { return true; }
                *why = "missing from this setup program (resource " + std::to_string(kResEntryBase + i) + ")";
                return false;
            }
        }
        *why = "not in this setup program's manifest";
        return false;
    }
    bool Manifest(std::string* text, std::string* why) override
    {
        if (!haveManifest_) {
            *why = "this setup program has no file list inside it (resource 100) - the download is damaged";
            return false;
        }
        *text = manifest_;
        return true;
    }

private:
    bool haveManifest_ = false;
    std::string manifest_;
    std::vector<ManifestEntry> entries_;
};

class Folder : public Payload {
public:
    explicit Folder(const std::wstring& dir) : dir_(dir) {}
    bool Read(const std::string& relPath, std::string* bytes, std::string* why) override
    {
        std::filesystem::path p = std::filesystem::path(dir_) / std::filesystem::u8path(relPath);
        p.make_preferred();
        std::ifstream f(p, std::ios::binary);
        if (!f) {
            *why = "not found in the payload folder";
            return false;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        *bytes = ss.str();
        return true;
    }
    bool Manifest(std::string* text, std::string* why) override { return Read("manifest.txt", text, why); }

private:
    std::wstring dir_;
};

}  // namespace

std::unique_ptr<Payload> EmbeddedPayload() { return std::make_unique<Embedded>(); }
std::unique_ptr<Payload> FolderPayload(const std::wstring& dir) { return std::make_unique<Folder>(dir); }

}  // namespace dlvr::setup
