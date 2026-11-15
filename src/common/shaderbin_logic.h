// Pure key + lookup logic for the shader precompile (docs/superpowers/specs/2026-10-21-shader-precompile-design.md).
// No D3D here so the unit test and tools/gen_shaderbin.py can share one definition of the key.
//
// Key = FNV-1a 64 over
//   sourceName \0 entry \0 target \0 defines(name=value;...) \0 source bytes [\0 hashExtra]
// - a null sourceName hashes as "" ; a null define value hashes as "name=;"
// - defines are read up to the {nullptr, nullptr} terminator; a null table is the same as an empty one
// - source bytes are exactly srcLen bytes
// - the trailing "\0 hashExtra" is present ONLY when hashExtra != nullptr (so nullptr and "" differ)
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dlvr {
namespace shaderbin {

constexpr uint64_t Fnv1a64(const void* p, size_t n, uint64_t h = 0xcbf29ce484222325ull)
{
    const unsigned char* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

// Same layout as D3D_SHADER_MACRO; the wrapper casts.
struct Define {
    const char* name;
    const char* value;
};

struct Entry {
    uint64_t key;
    const unsigned char* code;
    size_t size;
    const char* site;
    const char* entry;
};

namespace detail {
inline uint64_t Str(uint64_t h, const char* s)
{
    return s ? Fnv1a64(s, std::strlen(s), h) : h;
}
inline uint64_t Nul(uint64_t h)
{
    const unsigned char z = 0;
    return Fnv1a64(&z, 1, h);
}
}  // namespace detail

inline uint64_t MakeKey(const char* sourceName, const char* entry, const char* target,
                        const Define* defines, const void* src, size_t srcLen, const char* hashExtra)
{
    uint64_t h = 0xcbf29ce484222325ull;
    h = detail::Str(h, sourceName);
    h = detail::Nul(h);
    h = detail::Str(h, entry);
    h = detail::Nul(h);
    h = detail::Str(h, target);
    h = detail::Nul(h);
    if (defines) {
        for (const Define* d = defines; d->name; ++d) {  // D3D stops on Name == NULL
            h = detail::Str(h, d->name);
            h = detail::Str(h, "=");
            h = detail::Str(h, d->value);
            h = detail::Str(h, ";");
        }
    }
    h = detail::Nul(h);
    h = Fnv1a64(src, srcLen, h);
    if (hashExtra) {
        h = detail::Nul(h);
        h = detail::Str(h, hashExtra);
    }
    return h;
}

inline const Entry* Find(const Entry* table, size_t n, uint64_t key)
{
    for (size_t i = 0; i < n; ++i)
        if (table[i].key == key) return &table[i];
    return nullptr;
}

// HIT materialisation with an injectable blob factory (testable without D3D).
//   make(size, &blob, &data) -> rc (>= 0 is success); on success blob is the object and data its writable buffer.
//   release(blob) frees a blob that was created but cannot be used.
// Returns true only when rc >= 0 AND blob != nullptr AND data != nullptr: the bytes are copied, *blobOut = blob,
// *errOut (if given) = nullptr. Otherwise returns false, *blobOut = nullptr, any created blob is released, and
// *rc says why (a success rc with a null blob/data is reported as E_FAIL 0x80004005) - the caller then compiles.
template <class Make, class Release>
inline bool Materialise(const Entry& e, Make make, Release release, void** blobOut, void** errOut, long* rc)
{
    void* blob = nullptr;
    void* data = nullptr;
    long r = make(e.size, &blob, &data);
    if (r >= 0 && blob && data) {
        std::memcpy(data, e.code, e.size);
        *blobOut = blob;
        if (errOut) *errOut = nullptr;
        *rc = r;
        return true;
    }
    if (blob) release(blob);
    *blobOut = nullptr;
    *rc = (r < 0) ? r : static_cast<long>(0x80004005);
    return false;
}

}  // namespace shaderbin
}  // namespace dlvr
