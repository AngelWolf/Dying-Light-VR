// vehlean_logic.h - the buggy lean (2026-11-10, RUN BK1, Jay: "to lean forward towards the wheel, I had to lean backwards in real
// life" -> "Yes, build both"; unit-tested: src/src/tests/vehlean_logic_test.cpp). In a vehicle the engine's head -> eye step
// (+C08100) is off (INV-306: it runs only when !*(*(DI+3008)+16)) and the buggy camera is rigid to the car (CarBuggyCamera FPP
// +34A5D0), so neither the view nor the arm anchor (the eye element) follows the head. The fix adds the head's offset, mapped into
// the world EXACTLY as the arm solve maps controller offsets, to both. Origin = the tracking origin, as +C08100 uses on foot.
#pragma once

namespace dlvr {
namespace vehlean {

// h = the head position in tracking space (the solve's headRep); cby / sby = cos / sin of the solve's bodyYaw. World metres.
inline void HeadOffsetWorld(float cby, float sby, const float h[3], float out[3])
{
    out[0] = h[0] * cby + h[2] * sby;
    out[1] = h[1];
    out[2] = -h[0] * sby + h[2] * cby;
}

}  // namespace vehlean
}  // namespace dlvr
