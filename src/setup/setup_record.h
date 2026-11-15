// The setup program's install record (dlvr\setup_installed.txt) and the header state it decides
// (docs/superpowers/specs/2026-10-17-installer-design.md 8.5). Pure and header-only - tested by
// src/src/tests/setup_record_test.cpp.
//
// The header button reads Install on a fresh folder, Update when the mod is present but was put there by another package
// or by hand, and a green Installed when the record matches this exe's package and manifest exactly.
#pragma once

#include <string>

namespace dlvr::setup {

inline std::string RecordText(const std::string& package, const std::string& manifestMd5)
{
    return "package=" + package + "\r\nmanifest=" + manifestMd5 + "\r\n";
}

// Both keys required; order free; a BOM and CRLF/LF tolerated; other lines ignored (older records may carry tester=).
inline bool ParseRecord(const std::string& text, std::string* package, std::string* manifestMd5)
{
    std::string t = text;
    if (t.compare(0, 3, "\xEF\xBB\xBF") == 0) { t.erase(0, 3); }
    std::string p, m;
    bool haveP = false, haveM = false;
    size_t pos = 0;
    while (pos < t.size()) {
        size_t e = t.find('\n', pos);
        if (e == std::string::npos) { e = t.size(); }
        std::string line = t.substr(pos, e - pos);
        pos = e + 1;
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        if (line.compare(0, 8, "package=") == 0) { p = line.substr(8); haveP = true; }
        else if (line.compare(0, 9, "manifest=") == 0) { m = line.substr(9); haveM = true; }
    }
    if (!haveP || !haveM) { return false; }
    *package = p;
    *manifestMd5 = m;
    return true;
}

enum class HeaderState { Install, Update, Installed };

// filesIntact: every manifest file is on disk with its md5 (setup_sys checks it). A matching record over missing or
// changed files - a store verify, a game patch, antivirus - is an Update, so the user can repair without Reset.
inline HeaderState HeaderFor(bool modPresent, bool haveRecord, const std::string& recPackage,
                             const std::string& recManifest, const std::string& myPackage, const std::string& myManifest,
                             bool filesIntact)
{
    if (!modPresent) { return HeaderState::Install; }
    if (haveRecord && recPackage == myPackage && recManifest == myManifest && filesIntact) { return HeaderState::Installed; }
    return HeaderState::Update;
}

}  // namespace dlvr::setup
