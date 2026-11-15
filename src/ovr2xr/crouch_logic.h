// Pure logic for physical crouch - docs/superpowers/specs/2026-10-26-roomscale-physical-crouch-design.md §2.3 (plan
// docs/superpowers/plans/2026-10-27-roomscale-physical-crouch.md Task 8). No OpenXR, no Windows, no logging, so
// src/src/tests/crouch_logic_test.cpp compiles it on its own. Heights in metres; physcrouch.cpp (Task 9) owns the reads and the pad.
//
// THE IDEA (Jay: "The character should crouch in-game when his head drops low enough"; "A bit higher, so it doesn't flicker"):
// the standing head height is LEARNED (no calibration step) as a slowly-updated upper level; the crouch goes ON a set drop below
// it and OFF only a smaller drop below it. The game's own crouch is a TOGGLE on the pad (our Data3 inputs_pad.scr:67,
// _ACTION_DUCK_TOGGLE), so the game's crouch state is the authority: a physical crouch only ever ADDS a crouch, never stands up a
// player who crouched with the button, and never turns a sprint into a slide (Jay: "That's correct") unless physical_crouch_slide
// is on (Jay 2026-11-15: "PLAYER CROUCH TRIGGERS SLIDE - TOGGLE OPTION IN VR COMFORT", default "Off"). While the crouch comes from
// the head, the game's own eye drop is cancelled so it does not stack on the real one.
#pragma once

#include <cmath>

namespace dlvr {
namespace crouch {

const float kUpTauS   = 0.5f;    // the standing level rises toward a taller head with this time constant
const float kDownMps  = 0.02f;   // and falls at most this fast (a slouch), never while crouched

struct Learner {
    bool  has    = false;
    float standM = 0.0f;
};

// One head-height sample. The first seeds the level. Up: exponential toward h. Down: <= kDownMps * dt, only while !crouched and
// h > standM - offDropM (a crouch, or a head that low, never drags it down).
inline void Learn(Learner& L, float h, bool crouched, float dtS, float offDropM)
{
    if (!std::isfinite(h)) { return; }
    if (!L.has) {
        L.has = true;
        L.standM = h;
        return;
    }
    if (dtS <= 0.0f) { return; }
    if (h > L.standM) {
        L.standM += (h - L.standM) * (1.0f - std::exp(-dtS / kUpTauS));
    } else if (!crouched && h > L.standM - offDropM) {
        const float fall = kDownMps * dtS;
        L.standM = (L.standM - h) > fall ? L.standM - fall : h;
    }
}

struct Hyst {
    bool on = false;
};

// ON below standM - onDropM, OFF above standM - offDropM; in between it keeps its state.
inline bool HystStep(Hyst& H, float h, float standM, float onDropM, float offDropM)
{
    if (!H.on && h < standM - onDropM) {
        H.on = true;
    } else if (H.on && h > standM - offDropM) {
        H.on = false;
    }
    return H.on;
}

// The OFF point must sit above the ON point (a smaller drop) and above the standing level's own noise.
inline bool ValidDrops(float onDropM, float offDropM) { return onDropM > offDropM && offDropM > 0.0f; }

struct Arb {
    bool fromHead = false;   // the crouch the game is in (or has been asked for) was made by the head
};
enum class Act { None, Crouch, Stand };

// Crouch: the head wants it, the game is standing, no sprint (or allowSlide - the crouch is sent during a sprint and the game
// slides), and no head crouch is already asked for (the pad's crouch is a toggle
// - a second tap before the game catches up would stand back up). Stand: only a crouch the head made, once the head is back up.
// A head that comes up while the game is standing forgets the request (the tap was lost, or the player stood with the button).
inline Act Decide(Arb& A, bool headWants, bool gameCrouched, bool sprinting, bool allowSlide)
{
    if (headWants) {
        if (!gameCrouched && (!sprinting || allowSlide) && !A.fromHead) {
            A.fromHead = true;
            return Act::Crouch;
        }
        return Act::None;
    }
    if (gameCrouched && A.fromHead) {
        A.fromHead = false;
        return Act::Stand;
    }
    if (!gameCrouched) { A.fromHead = false; }
    return Act::None;
}

// How far to lift the eye while the crouch comes from the head: the game's own drop of the spine (standing minus now), never
// negative; 0 for a button crouch, which keeps today's behaviour (and the 10-25 crouch fix).
inline float EyeCancelY(float spineStandM, float spineNowM, bool fromHead)
{
    if (!fromHead) { return 0.0f; }
    const float d = spineStandM - spineNowM;
    return d > 0.0f ? d : 0.0f;
}

}  // namespace crouch
}  // namespace dlvr
