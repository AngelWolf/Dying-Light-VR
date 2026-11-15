// The setup program's stores: which store a Dying Light folder came from, Epic's install manifests, and the
// Program Files refusal (docs/superpowers/specs/2026-10-17-installer-design.md 8.4 / 8.4b). Pure and header-only -
// tested by src/src/tests/setup_store_test.cpp.
//
// Jay 2026-10-17: Epic and GOG are supported ("We did confirm a while back that Epic worked, but it's been a substantial
// amount of time since then"), nobody has an install to check them on ("Unfortunately I don't"), so their versions are
// told they are untested ("Say its untested"); Program Files is refused on every store ("I'd say refuse"). Everything
// here about Epic and GOG follows the launchers' conventions (Epic's Manifests\*.item JSON, GOG's goggame-*.info) and is
// NOT tested on a real install.
#pragma once

#include <cctype>
#include <string>
#include <vector>

namespace dlvr::setup {

enum class Store { Steam, Epic, Gog, Unknown };

inline const char* StoreTag(Store s)
{
    switch (s) {
    case Store::Steam: return "STEAM";
    case Store::Epic: return "EPIC";
    case Store::Gog: return "GOG";
    default: return "UNKNOWN STORE";
    }
}

// What the game folder itself shows (read by setup_sys): a ...\steamapps\common\... path, steam_api64.dll, an .egstore
// folder (Epic), a goggame-*.info file (GOG).
struct FolderFacts {
    bool steamappsPath;
    bool steamApiDll;
    bool egstoreDir;
    bool gogInfo;
};

inline Store StoreOf(const FolderFacts& f)
{
    if (f.gogInfo) { return Store::Gog; }
    if (f.egstoreDir) { return Store::Epic; }
    if (f.steamappsPath || f.steamApiDll) { return Store::Steam; }
    return Store::Unknown;
}

namespace detail {

inline void AppendUtf8(std::string* out, unsigned cp)
{
    if (cp < 0x80) {
        *out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        *out += static_cast<char>(0xC0 | (cp >> 6));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *out += static_cast<char>(0xE0 | (cp >> 12));
        *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        *out += static_cast<char>(0xF0 | (cp >> 18));
        *out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

inline bool Hex4(const std::string& s, size_t p, unsigned* v)
{
    if (p + 4 > s.size()) { return false; }
    *v = 0;
    for (size_t i = p; i < p + 4; ++i) {
        const char c = s[i];
        *v <<= 4;
        if (c >= '0' && c <= '9') { *v |= static_cast<unsigned>(c - '0'); }
        else if (c >= 'a' && c <= 'f') { *v |= static_cast<unsigned>(c - 'a' + 10); }
        else if (c >= 'A' && c <= 'F') { *v |= static_cast<unsigned>(c - 'A' + 10); }
        else { return false; }
    }
    return true;
}

// The string value of "key" (the first occurrence), JSON escapes decoded. False if absent, not a string, or malformed.
inline bool JsonString(const std::string& j, const char* key, std::string* out)
{
    const std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k);
    if (p == std::string::npos) { return false; }
    p += k.size();
    while (p < j.size() && std::isspace(static_cast<unsigned char>(j[p]))) { ++p; }
    if (p >= j.size() || j[p] != ':') { return false; }
    ++p;
    while (p < j.size() && std::isspace(static_cast<unsigned char>(j[p]))) { ++p; }
    if (p >= j.size() || j[p] != '"') { return false; }
    ++p;
    std::string v;
    while (p < j.size()) {
        const char c = j[p];
        if (c == '"') {
            *out = v;
            return true;
        }
        if (c != '\\') {
            v += c;
            ++p;
            continue;
        }
        if (p + 1 >= j.size()) { return false; }
        const char e = j[p + 1];
        p += 2;
        switch (e) {
        case '"': v += '"'; break;
        case '\\': v += '\\'; break;
        case '/': v += '/'; break;
        case 'b': v += '\b'; break;
        case 'f': v += '\f'; break;
        case 'n': v += '\n'; break;
        case 'r': v += '\r'; break;
        case 't': v += '\t'; break;
        case 'u': {
            unsigned cp = 0;
            if (!Hex4(j, p, &cp)) { return false; }
            p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 <= j.size() && j[p] == '\\' && j[p + 1] == 'u') {
                unsigned lo = 0;
                if (Hex4(j, p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
            }
            AppendUtf8(&v, cp);
            break;
        }
        default: return false;
        }
    }
    return false;   // unterminated
}

inline std::string LowerAscii(std::string s)
{
    for (auto& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

}  // namespace detail

// One Epic manifest (%ProgramData%\Epic\EpicGamesLauncher\Data\Manifests\*.item): true, with the folder, only for a
// Dying Light entry with a non-empty InstallLocation. Anything else - another game, a missing or non-string location,
// malformed JSON - is false and leaves *installLocation untouched.
inline bool EpicManifestDyingLight(const std::string& json, std::string* installLocation)
{
    std::string j = json;
    if (j.compare(0, 3, "\xEF\xBB\xBF") == 0) { j.erase(0, 3); }
    std::string name;
    std::string loc;
    if (!detail::JsonString(j, "DisplayName", &name) || !detail::JsonString(j, "InstallLocation", &loc)) { return false; }
    if (detail::LowerAscii(name).find("dying light") == std::string::npos || loc.empty()) { return false; }
    *installLocation = loc;
    return true;
}

// True if `path` is one of `roots` or inside one, after normalising both: '/' -> '\', case folded, trailing separators
// dropped, and a whole-component match ("C:\Program FilesX" is not under "C:\Program Files"). Review finding 13: an Epic
// manifest may hold forward slashes. (setup_sys also canonicalises the path first - junctions, 8.3 names.)
inline bool PathUnder(const std::string& path, const std::vector<std::string>& roots)
{
    auto norm = [](std::string s) {
        for (auto& c : s) {
            if (c == '/') { c = '\\'; }
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        while (!s.empty() && s.back() == '\\') { s.pop_back(); }
        return s;
    };
    const std::string p = norm(path);
    for (const auto& r0 : roots) {
        const std::string r = norm(r0);
        if (r.empty()) { continue; }
        if (p == r || (p.size() > r.size() && p.compare(0, r.size(), r) == 0 && p[r.size()] == '\\')) { return true; }
    }
    return false;
}

// The Find Dying Light card's refusal for a folder under Program Files (spec 8.4b), with the detected store's way to
// move the game. The Epic / GOG wording follows those launchers' conventions and is untested.
inline std::string ProgramFilesRefusal(Store s, const std::string& folder)
{
    std::string t = "Dying Light is installed under Program Files:\n" + folder +
                    "\nWindows blocks the mod's files there, so Setup can't install into this folder. Move the game "
                    "first, then run Setup again.\n";
    switch (s) {
    case Store::Steam:
        t += "Steam: Settings -> Storage -> add a library folder on another drive or outside Program Files, then move "
             "Dying Light to it.";
        break;
    case Store::Epic:
        t += "Epic Games: uninstall Dying Light, then install it again choosing a folder outside Program Files.";
        break;
    case Store::Gog:
        t += "GOG Galaxy: open Dying Light -> the settings button -> Manage installation -> Move, and choose a folder "
             "outside Program Files.";
        break;
    default:
        t += "Use your game store's option to move the game to a folder outside Program Files.";
        break;
    }
    return t;
}

// Spec 8.4: Epic and GOG are told their version is untested with this build. Empty for Steam and an unknown store.
inline std::string UntestedNotice(Store s)
{
    if (s == Store::Epic) { return "Epic Games version: not tested with this build of the mod. Please report how it goes."; }
    if (s == Store::Gog) { return "GOG version: not tested with this build of the mod. Please report how it goes."; }
    return "";
}

}  // namespace dlvr::setup
