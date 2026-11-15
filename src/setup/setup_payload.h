// The setup program's payload: every file the mod needs, read from resources compiled INTO the exe, or (testing,
// --payload) from a folder. docs/superpowers/specs/2026-10-17-installer-design.md 8.3 - Jay: "I want all the files to be
// baked into the installer itself ... 'Idiot proof' is what we're looking for here."
//
// Resource ids (numeric - relPaths are not safe .rc names; tools/make_setup_pkg.py writes them): the manifest = 100,
// vc_redist.x64.exe = 101, title.png = 102, the keep-install reset table (tools/shipped_defaults.py; absent in an older exe -
// keep installs then move no old defaults) = 103, manifest entry i (in manifest order) = 1000 + i. Every entry's md5 is
// checked (VerifyPayload) BEFORE any action touches the game folder, so a damaged exe fails first, naming the file.
// Md5Hex / VerifyPayload are tested by src/src/tests/setup_payload_test.cpp.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "setup_manifest.h"

#pragma comment(lib, "bcrypt.lib")

namespace dlvr::setup {

constexpr int kResManifest = 100;
constexpr int kResVcRedist = 101;
constexpr int kResTitlePng = 102;
constexpr int kResResets = 103;
constexpr int kResEntryBase = 1000;

inline std::string Md5Hex(const std::string& bytes)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0) != 0) { return ""; }
    std::string hex;
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        unsigned char d[16] = {};
        const ULONG n = static_cast<ULONG>(bytes.size());
        if (BCryptHashData(h, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), n, 0) == 0 &&
            BCryptFinishHash(h, d, 16, 0) == 0) {
            char s[33] = {};
            for (int i = 0; i < 16; ++i) { std::snprintf(s + 2 * i, 3, "%02x", d[i]); }
            hex = s;
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

class Payload {
public:
    virtual bool Read(const std::string& relPath, std::string* bytes, std::string* why) = 0;
    virtual bool Manifest(std::string* text, std::string* why) = 0;
    virtual ~Payload() = default;
};

// Every manifest entry is present and has its md5. False names the FIRST bad one (missing, or md5 got / want).
inline bool VerifyPayload(Payload& p, const std::vector<ManifestEntry>& m, std::string* firstBad)
{
    for (const auto& e : m) {
        std::string bytes, why;
        if (!p.Read(e.relPath, &bytes, &why)) {
            *firstBad = e.relPath + " - " + why;
            return false;
        }
        const std::string got = Md5Hex(bytes);
        if (got != e.md5) {
            *firstBad = e.relPath + " - md5 " + got + ", manifest " + e.md5;
            return false;
        }
    }
    return true;
}

// setup_payload.cpp
std::unique_ptr<Payload> EmbeddedPayload();
std::unique_ptr<Payload> FolderPayload(const std::wstring& dir);
// A raw resource by numeric id (vc_redist, title.png); false if absent.
bool ReadResource(int id, std::string* bytes);

}  // namespace dlvr::setup
