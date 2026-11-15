// BETA leak hardening (Jay 2026-10-18, "Addresses + refs"): the runtime-compiled HLSL sources in dlssflat.cpp carry
// comments that cite INV-/D-/U- records and name our instruments. Under DLVR_BETA_LEAN the source literal becomes a
// constexpr array used only in a constant expression - so it is never emitted - and the DLL gets a copy with every
// comment removed, sized exactly. Code, and every newline (so D3DCompile's error line numbers still match the source),
// are kept byte for byte. tests/hlslstrip_test.cpp pins it.
//
//   DLVR_HLSL_DECL(kHlsl) R"(...)";   // in place of: const char kHlsl[] = R"(...)";
//   DLVR_HLSL_USE(kHlsl)              // right after it: defines `const char* const kHlsl` (the stripped text)
#pragma once

#include <cstddef>

namespace dlvr {

template <size_t N>
struct HlslText {
    char s[N];
};

// The walk both functions share: calls emit(c) for every character that is not inside a // or /* */ comment.
template <size_t N, typename F>
constexpr void HlslWalk(const char (&in)[N], F emit)
{
    size_t i = 0;
    while (i + 1 < N && in[i]) {
        if (in[i] == '/' && in[i + 1] == '/') {
            while (i + 1 < N && in[i] && in[i] != '\n') { ++i; }   // the newline itself is kept
            continue;
        }
        if (in[i] == '/' && in[i + 1] == '*') {
            i += 2;
            while (i + 1 < N && in[i] && !(in[i] == '*' && in[i + 1] == '/')) { ++i; }
            if (i + 1 < N && in[i]) { i += 2; }
            continue;
        }
        emit(in[i]);
        ++i;
    }
}

template <size_t N>
constexpr size_t HlslStrippedLen(const char (&in)[N])
{
    size_t n = 0;
    HlslWalk(in, [&n](char) { ++n; });
    return n;
}

template <size_t M, size_t N>
constexpr HlslText<M> StripHlslComments(const char (&in)[N])
{
    HlslText<M> o{};
    size_t j = 0;
    HlslWalk(in, [&o, &j](char c) { o.s[j++] = c; });
    o.s[j] = '\0';
    return o;
}

}  // namespace dlvr

#if defined(DLVR_BETA_LEAN)
#define DLVR_HLSL_DECL(name) constexpr char name##_src[] =
#define DLVR_HLSL_USE(name)                                                                                 \
    constexpr auto name##_strip = ::dlvr::StripHlslComments<::dlvr::HlslStrippedLen(name##_src) + 1>(name##_src); \
    const char* const name = name##_strip.s;
#else
#define DLVR_HLSL_DECL(name) const char name[] =
#define DLVR_HLSL_USE(name)
#endif
