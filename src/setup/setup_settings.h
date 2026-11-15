// The setup program's dlvr\ovr2xr.txt handling (docs/superpowers/specs/2026-10-17-installer-design.md 5.2).
// Pure and header-only - tested by src/src/tests/setup_settings_test.cpp.
//
// Jay 2026-10-17: "If there's updates to the ovr2xr.txt in terms of features WE have added, then it is added in
// addendum to the file, not replaced. Same if something is removed." So a merge touches ONLY the lines of the keys
// it sets or retires and keeps every other byte - the BOM, comments, blank lines, unknown keys and each line's own
// ending. Keys match the way the layer matches them (ovr2xr.cpp WriteSettingsFile): case-insensitive, the whole key,
// immediately followed by '='. A '#' line is a comment.
#pragma once

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace dlvr::setup {

struct KeyValue {
    std::string key;
    std::string value;
};

namespace detail {

inline bool IEqualsN(const std::string& s, size_t pos, const std::string& key)
{
    if (s.size() < pos + key.size()) {
        return false;
    }
    for (size_t i = 0; i < key.size(); ++i) {
        char a = s[pos + i];
        char b = key[i];
        if (a >= 'A' && a <= 'Z') { a = static_cast<char>(a - 'A' + 'a'); }
        if (b >= 'A' && b <= 'Z') { b = static_cast<char>(b - 'A' + 'a'); }
        if (a != b) {
            return false;
        }
    }
    return true;
}

// True if `content` (one line, no terminator) is a non-comment `key=` line.
inline bool LineIsKey(const std::string& content, const std::string& key)
{
    if (content.empty() || content[0] == '#') {
        return false;
    }
    return IEqualsN(content, 0, key) && content.size() > key.size() && content[key.size()] == '=';
}

struct Line {
    std::string content;
    std::string eol;   // "\r\n", "\n" or "" (the last line without a newline)
};

inline std::vector<Line> SplitLines(const std::string& body)
{
    std::vector<Line> out;
    size_t p = 0;
    while (p < body.size()) {
        const size_t nl = body.find('\n', p);
        if (nl == std::string::npos) {
            out.push_back({body.substr(p), ""});
            break;
        }
        if (nl > p && body[nl - 1] == '\r') {
            out.push_back({body.substr(p, nl - 1 - p), "\r\n"});
        } else {
            out.push_back({body.substr(p, nl - p), "\n"});
        }
        p = nl + 1;
    }
    return out;
}

inline const char* kBom = "\xEF\xBB\xBF";

}  // namespace detail

// True if a non-comment `key=` line exists (case-insensitive key).
inline bool HasKey(const std::string& text, const std::string& key)
{
    std::string body = text;
    if (body.compare(0, 3, detail::kBom) == 0) {
        body.erase(0, 3);
    }
    for (const auto& l : detail::SplitLines(body)) {
        if (detail::LineIsKey(l.content, key)) {
            return true;
        }
    }
    return false;
}

// Sets each `set` key in place (appending any the file lacks, at the end), removes the lines of `retired` keys, and
// keeps every other byte. Appended lines use CRLF if any line of the file has it, else LF if any line has it, else
// CRLF. With an empty `set` and `retired` the input comes back byte-identical.
inline std::string MergeSettings(const std::string& existing, const std::vector<KeyValue>& set,
                                 const std::vector<std::string>& retired)
{
    std::string bom;
    std::string body = existing;
    if (body.compare(0, 3, detail::kBom) == 0) {
        bom = detail::kBom;
        body.erase(0, 3);
    }
    std::vector<detail::Line> lines = detail::SplitLines(body);

    std::string eol = "\r\n";
    bool anyCrlf = false;
    bool anyLf = false;
    for (const auto& l : lines) {
        anyCrlf = anyCrlf || l.eol == "\r\n";
        anyLf = anyLf || l.eol == "\n";
    }
    if (!anyCrlf && anyLf) {
        eol = "\n";
    }

    std::vector<bool> written(set.size(), false);
    std::vector<detail::Line> kept;
    for (auto& l : lines) {
        bool drop = false;
        for (const auto& r : retired) {
            if (detail::LineIsKey(l.content, r)) {
                drop = true;
                break;
            }
        }
        if (drop) {
            continue;
        }
        for (size_t i = 0; i < set.size(); ++i) {
            if (detail::LineIsKey(l.content, set[i].key)) {
                l.content = set[i].key + "=" + set[i].value;
                written[i] = true;
                break;
            }
        }
        kept.push_back(l);
    }

    bool appending = false;
    for (size_t i = 0; i < set.size(); ++i) {
        appending = appending || !written[i];
    }
    if (appending && !kept.empty() && kept.back().eol.empty()) {
        kept.back().eol = eol;
    }
    for (size_t i = 0; i < set.size(); ++i) {
        if (!written[i]) {
            kept.push_back({set[i].key + "=" + set[i].value, eol});
        }
    }

    std::string out = bom;
    for (const auto& l : kept) {
        out += l.content;
        out += l.eol;
    }
    return out;
}

// ---- keep-install old-default reset (docs/superpowers/specs/2026-10-25-keep-settings-old-default-reset-design.md).
// A keep install keeps every line, so a value an OLD package wrote as its default survived (a tester, 2026-10-25: body_grab=0 from
// an older build, the new default 1). tools/shipped_defaults.py lists, per key, every value a shipped package gave it that is
// not today's default; a line still holding one of those moves to today's default. A value no package shipped is the user's.

// One row of the packager's reset table: `key \t type \t current \t old1 \t old2 ...` (type bool | int | float).
struct ResetRow {
    std::string key;
    std::string type;
    std::string current;
    std::vector<std::string> old;
};

// A line the reset moved: the value as found -> today's default.
struct Moved {
    std::string key;
    std::string from;
    std::string to;
};

// '#' lines and rows with fewer than 4 fields are skipped - a damaged row is never a reason to fail the install.
inline std::vector<ResetRow> ParseResetTable(const std::string& tsv)
{
    std::vector<ResetRow> out;
    for (const auto& l : detail::SplitLines(tsv)) {
        if (l.content.empty() || l.content[0] == '#') {
            continue;
        }
        std::vector<std::string> f;
        size_t p = 0;
        for (;;) {
            const size_t t = l.content.find('\t', p);
            f.push_back(l.content.substr(p, t == std::string::npos ? std::string::npos : t - p));
            if (t == std::string::npos) {
                break;
            }
            p = t + 1;
        }
        if (f.size() < 4 || f[0].empty()) {
            continue;
        }
        out.push_back({f[0], f[1], f[2], std::vector<std::string>(f.begin() + 3, f.end())});
    }
    return out;
}

namespace detail {

inline std::string Trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) {
        return "";
    }
    return s.substr(a, s.find_last_not_of(" \t") - a + 1);
}

}  // namespace detail

// bool / int: whole-string integers, equal; float: within 1e-4 relative (the layer writes %g, an old build may differ in
// form - "30" / "30.0" / "3e+01"). A value that does not parse is never equal, so it is kept as found.
inline bool NumEqual(const std::string& a, const std::string& b, const std::string& type)
{
    const std::string x = detail::Trim(a);
    const std::string y = detail::Trim(b);
    if (x.empty() || y.empty()) {
        return false;
    }
    char* ex = nullptr;
    char* ey = nullptr;
    if (type == "float") {
        const double dx = std::strtod(x.c_str(), &ex);
        const double dy = std::strtod(y.c_str(), &ey);
        if (*ex != '\0' || *ey != '\0') {
            return false;
        }
        const double m = std::fmax(1.0, std::fmax(std::fabs(dx), std::fabs(dy)));
        return std::fabs(dx - dy) <= 1e-4 * m;
    }
    const long lx = std::strtol(x.c_str(), &ex, 10);
    const long ly = std::strtol(y.c_str(), &ey, 10);
    return *ex == '\0' && *ey == '\0' && lx == ly;
}

// The value of the LAST non-comment `key=` line (the layer reads the last one); false if the text has none.
inline bool LastValueOf(const std::string& text, const std::string& key, std::string* value)
{
    std::string body = text;
    if (body.compare(0, 3, detail::kBom) == 0) {
        body.erase(0, 3);
    }
    bool found = false;
    for (const auto& l : detail::SplitLines(body)) {
        if (detail::LineIsKey(l.content, key)) {
            *value = l.content.substr(key.size() + 1);
            found = true;
        }
    }
    return found;
}

// The copy of the settings file the setup itself wrote last time (ADDENDUM 1): every install writes it beside ovr2xr.txt.
inline const char* kWrittenCopyPath = "dlvr/setup_settings_written.txt";

// Rewrites `key=<old default>` lines to `key=<current>` and keeps every other byte (BOM, comments, unknown keys, each line's
// own ending, a last line without one). Never appends: a key the file lacks comes up at its default when the layer writes the
// file. `skipKeys` (the setup's answered keys) are never touched. Each rewrite is reported in `moved`, in file order.
// `written` (ADDENDUM 1) = the copy of what the last Setup wrote, nullptr if there is none: a key the copy holds at a DIFFERENT
// value was changed by the user since that Setup and is kept, whatever it equals.
inline std::string ApplyOldDefaultResets(const std::string& existing, const std::vector<ResetRow>& table,
                                         const std::vector<std::string>& skipKeys, std::vector<Moved>* moved,
                                         const std::string* written = nullptr)
{
    std::string bom;
    std::string body = existing;
    if (body.compare(0, 3, detail::kBom) == 0) {
        bom = detail::kBom;
        body.erase(0, 3);
    }
    std::string out = bom;
    for (auto& l : detail::SplitLines(body)) {
        for (const auto& r : table) {
            if (!detail::LineIsKey(l.content, r.key)) {
                continue;
            }
            bool skip = false;
            for (const auto& s : skipKeys) {
                skip = skip || detail::LineIsKey(l.content, s);
            }
            const std::string v = l.content.substr(r.key.size() + 1);
            std::string w;
            if (written && LastValueOf(*written, r.key, &w) && w != v && !NumEqual(v, w, r.type)) {
                skip = true;   // the user changed it after the last Setup
            }
            bool wasOld = false;
            for (const auto& o : r.old) {
                wasOld = wasOld || NumEqual(v, o, r.type);
            }
            if (!skip && wasOld && !NumEqual(v, r.current, r.type)) {
                if (moved) {
                    moved->push_back({r.key, v, r.current});
                }
                l.content = r.key + "=" + r.current;
            }
            break;
        }
        out += l.content;
        out += l.eol;
    }
    return out;
}

// The window's version: at most `maxLines` keys (at least 1), then "...and N more" naming the log, which has every line
// ("reset: "). "" when none moved.
inline std::string MovedSummaryCapped(const std::vector<Moved>& moved, size_t maxLines, const std::string& logPath)
{
    if (moved.empty()) {
        return "";
    }
    const size_t shown = moved.size() <= maxLines ? moved.size() : (maxLines < 1 ? 1 : maxLines);
    std::string s = "Updated " + std::to_string(moved.size()) +
                    " setting(s) to this version's defaults (you had the old default):\n";
    for (size_t i = 0; i < shown; ++i) {
        s += "  " + moved[i].key + ": " + moved[i].from + " -> " + moved[i].to + "\n";
    }
    if (shown < moved.size()) {
        s += "  ...and " + std::to_string(moved.size() - shown) + " more - every one is in " + logPath + "\n";
    }
    return s + "Settings you changed since the last Setup were kept. If you had set one of these on purpose, set it again "
               "in the in-game menu.";
}

// The finish page's block for a keep install that moved keys (every key listed); "" when none moved.
inline std::string MovedSummary(const std::vector<Moved>& moved)
{
    return MovedSummaryCapped(moved, moved.size(), "");
}

// The window's success text: the install line the window shows anyway, then the capped list; "" when none moved (the window
// then shows its own "Installed:" line).
inline std::string InstalledSummary(const std::string& package, const std::vector<Moved>& moved, size_t maxLines,
                                    const std::string& logPath)
{
    if (moved.empty()) {
        return "";
    }
    return "Installed: " + package + " - every file checked.\n\n" + MovedSummaryCapped(moved, maxLines, logPath);
}

// The shipped no-keys file (tools/make_nv_pkg_20261017.py SETTINGS_TEXT, verbatim, CRLF), then the `set` lines.
inline std::string FreshSettings(const std::vector<KeyValue>& set)
{
    std::string out =
        "# Dying Light VR - the mod's settings. This file was shipped with NO settings on purpose: every\r\n"
        "# setting starts at the mod's built-in default, and the mod writes them all in here on the first launch.\r\n"
        "# Change settings in the in-game menu (INSERT), not here. Delete this file to go back to the defaults.\r\n";
    for (const auto& kv : set) {
        out += kv.key + "=" + kv.value + "\r\n";
    }
    return out;
}

}  // namespace dlvr::setup
