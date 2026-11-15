// Shader precompile wrapper: a drop-in for D3DCompile that returns a prebuilt DXBC blob when the key matches, else compiles.
// docs/superpowers/specs/2026-10-21-shader-precompile-design.md. Implemented in bootstrap/shaderbin.cpp.
#pragma once

#include <d3dcompiler.h>

namespace dlvr {
namespace shaderbin {

// Same arguments and result as D3DCompile. hashExtra: extra text folded into the key (NIS passes kNisScalerHlsl for its include).
// HIT only when Flags1 == 0 && Flags2 == 0 and the key is in the table; anything else compiles at runtime and says MISS in the log.
HRESULT Compile(LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName, const D3D_SHADER_MACRO* pDefines,
                ID3DInclude* pInclude, LPCSTR pEntrypoint, LPCSTR pTarget, UINT Flags1, UINT Flags2,
                ID3DBlob** ppCode, ID3DBlob** ppErrorMsgs, const char* hashExtra = nullptr);

}  // namespace shaderbin
}  // namespace dlvr
