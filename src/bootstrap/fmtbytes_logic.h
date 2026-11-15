// Bytes per pixel of the colour formats the HUD-layer / panel diagnostics meet. RIFT float (2026-10-21, INV-600): the eye
// targets - and so the HUD layers copied from them - became R16G16B16A16, so a capture that writes 4 bytes a pixel must refuse.
// No Windows headers, no logging, so src/src/tests/fmtbytes_logic_test.cpp compiles it on its own.
#pragma once

#include <dxgiformat.h>

namespace dlvr {
namespace fmtbytes {

// 4 = the 8-bit RGBA / BGRA family and the packed 32-bit formats, 8 = R16G16B16A16, 0 = anything else (not handled).
inline int BytesPerPixel(DXGI_FORMAT f)
{
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R11G11B10_FLOAT:
            return 4;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
            return 8;
        default:
            return 0;
    }
}

}  // namespace fmtbytes
}  // namespace dlvr
