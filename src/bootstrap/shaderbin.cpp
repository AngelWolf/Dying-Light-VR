// Shader precompile wrapper. See ../common/shaderbin.h and docs/superpowers/specs/2026-10-21-shader-precompile-design.md.
// Called once per shader compile at start-up - NOT a per-frame path (no per-frame cost to account for).
#include "../common/shaderbin.h"
#include "../common/shaderbin_logic.h"
#include "../common/log.h"
#include "shaderbin.gen.h"

#include <windows.h>
#include <d3dcompiler.h>

#include <cstring>

// Same convention as the rest of the bootstrap (dlssflat.cpp): log.h supplies LogTo, the destination is chosen per translation unit.
#define LOG(...) ::dlvr::LogTo("dlvr_bootstrap.log", __VA_ARGS__)

namespace dlvr {
namespace shaderbin {

static_assert(sizeof(Define) == sizeof(D3D_SHADER_MACRO), "Define must match D3D_SHADER_MACRO");

HRESULT Compile(LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName, const D3D_SHADER_MACRO* pDefines,
                ID3DInclude* pInclude, LPCSTR pEntrypoint, LPCSTR pTarget, UINT Flags1, UINT Flags2,
                ID3DBlob** ppCode, ID3DBlob** ppErrorMsgs, const char* hashExtra)
{
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);

    const uint64_t key = MakeKey(pSourceName, pEntrypoint, pTarget, reinterpret_cast<const Define*>(pDefines),
                                 pSrcData, static_cast<size_t>(SrcDataSize), hashExtra);
    const bool flagsZero = (Flags1 == 0 && Flags2 == 0);
    const Entry* e = flagsZero ? Find(gen::kEntries, gen::kEntryCount, key) : nullptr;

    if (e) {
        void* blobV = nullptr;
        void* errV = nullptr;
        long hb = 0;
        const bool ok = Materialise(
            *e,
            [](size_t n, void** b, void** d) -> long {
                ID3DBlob* x = nullptr;
                const HRESULT h = D3DCreateBlob(n, &x);
                *b = x;
                *d = (SUCCEEDED(h) && x) ? x->GetBufferPointer() : nullptr;
                return static_cast<long>(h);
            },
            [](void* b) { static_cast<ID3DBlob*>(b)->Release(); }, &blobV, &errV, &hb);
        if (ok) {
            if (ppErrorMsgs) *ppErrorMsgs = nullptr;
            *ppCode = static_cast<ID3DBlob*>(blobV);
            QueryPerformanceCounter(&t1);
            // [PROBE: SHADERBIN] gate=none (one line per runtime shader compile, both trees)  site=FLAT  log-only=yes
            LOG("[SHADERBIN] %s %s %s HIT key %016llX %.3f ms", e->site, e->entry, pTarget ? pTarget : "",
                static_cast<unsigned long long>(key), 1000.0 * double(t1.QuadPart - t0.QuadPart) / double(f.QuadPart));
            return S_OK;
        }
        LOG("[SHADERBIN] %s %s HIT but blob create FAILED 0x%08X - compiling", e->site, e->entry,
            static_cast<unsigned>(hb));
    }

    const HRESULT hr = D3DCompile(pSrcData, SrcDataSize, pSourceName, pDefines, pInclude, pEntrypoint, pTarget, Flags1,
                                  Flags2, ppCode, ppErrorMsgs);
    QueryPerformanceCounter(&t1);
    // [PROBE: SHADERBIN] gate=none (one line per runtime shader compile, both trees)  site=FLAT  log-only=yes
    LOG("[SHADERBIN] %s %s %s %s key %016llX %.3f ms hr 0x%08X", pSourceName ? pSourceName : "(null)",
        pEntrypoint ? pEntrypoint : "", pTarget ? pTarget : "", flagsZero ? "MISS" : "MISS (flags)",
        static_cast<unsigned long long>(key), 1000.0 * double(t1.QuadPart - t0.QuadPart) / double(f.QuadPart),
        static_cast<unsigned>(hr));
    return hr;
}

}  // namespace shaderbin
}  // namespace dlvr
