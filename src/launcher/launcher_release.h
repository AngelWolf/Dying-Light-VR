// A launcher release: its manifest (release.txt), which release is installed, and the launcher's own record
// (spec docs/superpowers/specs/2026-11-15-launcher-design.md sections 10, 11 and 12.6). Pure and header-only - tested by
// src_beta/src/tests/launcher_release_test.cpp.
//
// release.txt is the setup's manifest format (setup_manifest.h: "<md5>  <path>[ dlss-only]") with two '#' header lines
// the setup's parser skips as comments:
//   # release=<name>        required - the version the launcher shows, and the name a tester's log zip carries
//   # launcher=<md5>        the launcher exe of this release (self-update)
// The mod's own logs carry only a build time per DLL (src_beta/src/common/log.h:299-301), so a release is identified by
// the md5 of every installed file, never by a version string.
#pragma once

#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "setup_manifest.h"

namespace dlvr::launcher {

using setup::ManifestEntry;

struct Release {
    std::string name;
    std::string notes;
    std::vector<ManifestEntry> files;
    std::string launcherMd5;
};

namespace detail {

inline std::string LowerPath(std::string s)
{
    for (char& c : s) {
        if (c == '\\') { c = '/'; }
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
    }
    return s;
}

inline bool HeaderValue(const std::string& text, const std::string& key, std::string* out)
{
    const std::string tag = "# " + key + "=";
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) { e = text.size(); }
        std::string line = text.substr(p, e - p);
        p = e + 1;
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        if (line.compare(0, tag.size(), tag) == 0) {
            *out = line.substr(tag.size());
            return true;
        }
    }
    return false;
}

}  // namespace detail

// false = refused, *why names the reason (a missing release name, a malformed line, a duplicate path).
inline bool ParseRelease(const std::string& manifestText, const std::string& notesText, Release* out, std::string* why)
{
    Release r;
    if (!detail::HeaderValue(manifestText, "release", &r.name) || r.name.empty()) {
        *why = "release.txt has no \"# release=<name>\" line";
        return false;
    }
    detail::HeaderValue(manifestText, "launcher", &r.launcherMd5);
    try {
        r.files = setup::ParseManifest(manifestText);
    } catch (const std::exception& e) {
        *why = e.what();
        return false;
    }
    std::set<std::string> seen;
    for (const auto& f : r.files) {
        if (!seen.insert(detail::LowerPath(f.relPath)).second) {
            *why = "release.txt lists a path twice: " + f.relPath;
            return false;
        }
    }
    r.notes = notesText;
    *out = r;
    return true;
}

enum class InstallMatch { None, Exact, Modified, Foreign };

// Shipped files the mod itself rewrites at runtime: the bootstrap's pakgen writes DW\dlvr_pp.mth at every launch with this PC's
// render values (INV-592; the release's copy is only the fallback). A different md5 there is not damage (Jay's install, RUN L1).
inline const char* const kRuntimeRewritten[] = {"dw/dlvr_pp.mth"};

inline bool RewrittenAtRuntime(const std::string& lowerPath)
{
    for (const char* p : kRuntimeRewritten) {
        if (lowerPath == p) { return true; }
    }
    return false;
}

// diskMd5 = lower-case forward-slash path -> md5, for the release's files that exist on disk. A missing dlss-only file
// is not a difference (an AMD install never has it). haveRecord = dlvr\launcher_installed.txt was read.
//   None     - none of the release's files are on disk
//   Exact    - every file present and equal
//   Modified - installed by the launcher, some files differ or are missing (*differing names them: Repair's list)
//   Foreign  - mod files present, no launcher record, and not Exact (a hand or setup install)
inline InstallMatch MatchInstalled(const Release& r, const std::map<std::string, std::string>& diskMd5, bool haveRecord,
                                   std::vector<std::string>* differing)
{
    differing->clear();
    bool any = false;
    for (const auto& f : r.files) {
        const auto it = diskMd5.find(detail::LowerPath(f.relPath));
        if (it == diskMd5.end()) {
            if (!f.dlssOnly) { differing->push_back(f.relPath); }
            continue;
        }
        any = true;
        if (it->second != f.md5 && !RewrittenAtRuntime(detail::LowerPath(f.relPath))) { differing->push_back(f.relPath); }
    }
    if (!any) {
        differing->clear();
        return InstallMatch::None;
    }
    if (differing->empty()) { return InstallMatch::Exact; }
    return haveRecord ? InstallMatch::Modified : InstallMatch::Foreign;
}

// dlvr\launcher_installed.txt. mode = "vr" | "flat"; store = setup::StoreTag's text.
inline std::string ReleaseRecordText(const Release& r, const std::string& mode, const std::string& store)
{
    return "release=" + r.name + "\r\nmode=" + mode + "\r\nstore=" + store + "\r\n";
}

// false when there is no release= line. mode / store are empty when absent.
inline bool ParseReleaseRecord(const std::string& text, std::string* release, std::string* mode, std::string* store)
{
    std::string rel, m, s;
    bool haveRel = false;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) { e = text.size(); }
        std::string line = text.substr(p, e - p);
        p = e + 1;
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        if (line.compare(0, 8, "release=") == 0) { rel = line.substr(8); haveRel = true; }
        else if (line.compare(0, 5, "mode=") == 0) { m = line.substr(5); }
        else if (line.compare(0, 6, "store=") == 0) { s = line.substr(6); }
    }
    if (!haveRel) { return false; }
    *release = rel;
    *mode = m;
    *store = s;
    return true;
}

}  // namespace dlvr::launcher
