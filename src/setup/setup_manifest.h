// The setup program's payload manifest (docs/superpowers/specs/2026-10-17-installer-design.md 5.7). Pure and
// header-only - tested by src/src/tests/setup_plan_test.cpp.
//
// payload\manifest.txt, written by tools/make_setup_pkg.py: one file per line, "<32 hex md5>  <path>" with an optional
// trailing " dlss-only" (nvngx_dlss.dll - installed only when the method is DLSS). The path is relative to the game
// folder. '#' lines and blank lines are skipped. A malformed line THROWS, naming it - a manifest the setup cannot read
// must never turn into a partial install.
#pragma once

#include <stdexcept>
#include <string>
#include <vector>

namespace dlvr::setup {

struct ManifestEntry {
    std::string md5;       // lower-case hex
    std::string relPath;   // forward slashes
    bool dlssOnly;
};

inline std::vector<ManifestEntry> ParseManifest(const std::string& text)
{
    std::vector<ManifestEntry> out;
    size_t p = 0;
    while (p <= text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) {
            e = text.size();
        }
        std::string line = text.substr(p, e - p);
        p = e + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            if (e == text.size()) { break; }
            continue;
        }
        bool hex = line.size() > 34;
        for (size_t i = 0; hex && i < 32; ++i) {
            const char c = line[i];
            hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        }
        if (!hex || line[32] != ' ' || line[33] != ' ') {
            throw std::runtime_error("manifest line is not \"<md5>  <path>\": " + line);
        }
        ManifestEntry m{line.substr(0, 32), line.substr(34), false};
        for (auto& c : m.md5) {
            if (c >= 'A' && c <= 'F') { c = static_cast<char>(c - 'A' + 'a'); }
        }
        const std::string tag = " dlss-only";
        if (m.relPath.size() > tag.size() && m.relPath.compare(m.relPath.size() - tag.size(), tag.size(), tag) == 0) {
            m.dlssOnly = true;
            m.relPath.erase(m.relPath.size() - tag.size());
        }
        for (auto& c : m.relPath) {
            if (c == '\\') { c = '/'; }
        }
        if (m.relPath.empty()) {
            throw std::runtime_error("manifest line has no path: " + line);
        }
        out.push_back(m);
        if (e == text.size()) { break; }
    }
    return out;
}

}  // namespace dlvr::setup
