// The log zip's privacy pass and file choice (spec docs/superpowers/specs/2026-11-15-launcher-design.md section 11). Jay
// 2026-11-15: "Make sure it masks users names." Every name form - the Windows user name, the profile folder's 8.3 short
// name (C:\Users\ALEXSA~1, Review Focus 2) and any extra name found (an Epic profile name) - is masked in ASCII and in
// UTF-16LE, in any case. Text: "<user>" (UTF-16 runs: the same number of 'x'). Binary (.dmp): the same number of 'x', so
// no offset in the file moves. Pure and header-only - tested by src_beta/src/tests/launcher_logs_test.cpp.
#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace dlvr::launcher {

// A name shorter than 3 characters is never used: it would mask ordinary words.
constexpr size_t kMinNameLen = 3;

inline std::vector<std::string> NameForms(const std::string& user, const std::string& shortProfileDir,
                                          const std::vector<std::string>& extras)
{
    std::vector<std::string> out;
    auto add = [&](const std::string& n) {
        if (n.size() < kMinNameLen) { return; }
        for (const auto& o : out) {
            if (o.size() == n.size() && std::equal(o.begin(), o.end(), n.begin(), [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
                })) {
                return;
            }
        }
        out.push_back(n);
    };
    add(user);
    const size_t s = shortProfileDir.find_last_of("\\/");
    add(s == std::string::npos ? shortProfileDir : shortProfileDir.substr(s + 1));
    for (const auto& e : extras) { add(e); }
    // longest first, so a name that contains another is masked whole
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    return out;
}

namespace detail {

inline bool EqCI(char a, char b)
{
    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
}

// Replace every case-insensitive occurrence of `needle` (stride 1 = ASCII, 2 = UTF-16LE) with `repl(len)`.
template <typename Repl>
inline std::string Replace(const std::string& hay, const std::string& name, int stride, Repl repl, int* hits)
{
    const size_t len = name.size() * static_cast<size_t>(stride);
    std::string out;
    out.reserve(hay.size());
    size_t i = 0;
    while (i < hay.size()) {
        bool m = i + len <= hay.size();
        for (size_t k = 0; m && k < name.size(); ++k) {
            m = EqCI(hay[i + k * stride], name[k]) && (stride == 1 || hay[i + k * stride + 1] == '\0');
        }
        if (m) {
            out += repl(len);
            i += len;
            ++*hits;
        } else {
            out.push_back(hay[i++]);
        }
    }
    return out;
}

inline std::string Xs(size_t n, int stride)
{
    std::string s;
    for (size_t k = 0; k < n / static_cast<size_t>(stride); ++k) {
        s.push_back('x');
        if (stride == 2) { s.push_back('\0'); }
    }
    return s;
}

}  // namespace detail

inline std::string MaskText(const std::string& text, const std::vector<std::string>& names, int* hits)
{
    *hits = 0;
    std::string t = text;
    for (const auto& n : names) {
        t = detail::Replace(t, n, 1, [](size_t) { return std::string("<user>"); }, hits);
        t = detail::Replace(t, n, 2, [](size_t len) { return detail::Xs(len, 2); }, hits);
    }
    return t;
}

inline std::string MaskBinary(const std::string& bytes, const std::vector<std::string>& names, int* hits)
{
    *hits = 0;
    std::string t = bytes;
    for (const auto& n : names) {
        t = detail::Replace(t, n, 1, [](size_t len) { return detail::Xs(len, 1); }, hits);
        t = detail::Replace(t, n, 2, [](size_t len) { return detail::Xs(len, 2); }, hits);
    }
    return t;
}

// The newest n (by the given time, larger = newer), newest first.
inline std::vector<std::string> PickNewest(std::vector<std::pair<std::string, unsigned long long>> files, size_t n)
{
    std::stable_sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::vector<std::string> out;
    for (size_t i = 0; i < files.size() && i < n; ++i) { out.push_back(files[i].first); }
    return out;
}

}  // namespace dlvr::launcher
