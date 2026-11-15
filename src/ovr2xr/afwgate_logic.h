// Pure logic for two of the AFW rebuild gate's checks - docs/BETA-FINDINGS-2026-10-18.md section 3 candidate A (Jay 2026-10-18:
// "Gate checks position + eye"). The gate (camhook.cpp afwcam::GateImage) only compared HEADING; a stale shared camera
// snapshot (INV-271) keeps the heading, so it passed and the held eye was rebuilt with the wrong camera.
// No Windows, no logging, so src/src/tests/afwgate_logic_test.cpp compiles it on its own.
#pragma once

#include <cmath>
#include <cstring>

namespace dlvr {
namespace afwgate {

// INV-384 (Jay's headset, 23,936 images, moving included): the GPU eye camera sits 0.0318 m (max 0.0319) from the captured
// camera = half the eye separation. 10 mm either side is the tolerance - a stale matrix while walking misses by several cm.
constexpr float kEyePosTolM = 0.010f;
// A separation outside this range is not an eye separation (no headset reading) - the check then has no verdict.
constexpr float kSepMinM = 0.030f;
constexpr float kSepMaxM = 0.100f;

// INV-271's stale snapshot: this image's view-projection is the previous image's, bit for bit. Two eyes never share one
// legitimately (they are an IPD apart), so equality is staleness with no tolerance to choose.
inline bool SameMatrix(const float a[16], const float b[16])
{
    return std::memcmp(a, b, sizeof(float) * 16) == 0;
}

inline bool SepUsable(float sepM)
{
    return sepM > kSepMinM && sepM < kSepMaxM;
}

// The GPU eye camera is half an eye separation from the captured camera, within kEyePosTolM. Unusable separation -> false
// (INV-6: a degraded test fails closed).
inline bool EyeOffsetOk(const float gpuPos[3], const float capPos[3], float sepM)
{
    if (!SepUsable(sepM)) { return false; }
    const float dx = gpuPos[0] - capPos[0], dy = gpuPos[1] - capPos[1], dz = gpuPos[2] - capPos[2];
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    return std::fabs(d - 0.5f * sepM) <= kEyePosTolM;
}

}  // namespace afwgate
}  // namespace dlvr
