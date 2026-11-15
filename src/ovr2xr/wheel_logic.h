#pragma once
// VR selection wheels - the pure logic (spec docs/superpowers/specs/2026-10-27-vr-selection-wheels-design.md, plan Task 2).
// Header-only, no Windows headers: the holster / bownock polls drive it, wheel_logic_test.cpp tests it.
//
// THE GAME SIDE (INV-640, our gamedll): a wheel opens on its SHOW button, becomes visible after 0.2 s held, commits the picked slot
// on the show button's RELEASE; a release under 0.2 s with no pick = the game's "select next" (the tap). The stick cursor
// accumulates and a pick is sticky - centring never clears it - so a centre release is turned into "steer back to the slot the
// player already has" (D11, TargetSlot).
//
// HoldMachine (one press of a holster grip / the quiver's R2):
//   release before holdMs - leadMs           -> Tap        (the caller sends its own short pulse, as before)
//   holdMs - leadMs reached, still held      -> Lead       (the caller holds the show button from here, so the game's own 0.2 s
//                                                           visibility lands at holdMs)
//   release after Lead, before holdMs        -> TapByGame  (the button was down < leadMs: the game does "select next" itself)
//   holdMs reached, still held               -> Open       (the caller starts steering the game's cursor)
//   release while open                       -> Confirm    (the caller steers to TargetSlot, then lets the button go)
//   Abort() while the button is down         -> Cancel on the next Update; a still-held grip then needs a fresh press edge.
namespace dlvr::wheel {

enum class Slot { None, Up, Right, Down, Left };
enum class Out { Nothing, Tap, Lead, TapByGame, Open, Confirm, Cancel };

class HoldMachine {
public:
    Out Update(bool held, unsigned long long nowMs, unsigned holdMs, unsigned leadMs)
    {
        if (aborted_) {
            const bool wasDown = phase_ != Phase::Idle;
            phase_ = Phase::Idle;
            aborted_ = false;
            waitRelease_ = held;
            if (wasDown) { return Out::Cancel; }
        }
        if (waitRelease_) {
            if (!held) { waitRelease_ = false; }
            return Out::Nothing;
        }
        const unsigned lead = leadMs < holdMs ? holdMs - leadMs : 0;
        switch (phase_) {
        case Phase::Idle:
            if (held) {
                phase_ = Phase::Pressed;
                startMs_ = nowMs;
            }
            return Out::Nothing;
        case Phase::Pressed:
            if (!held) {
                phase_ = Phase::Idle;
                return Out::Tap;
            }
            if (nowMs - startMs_ >= lead) {
                phase_ = Phase::Lead;
                return Out::Lead;
            }
            return Out::Nothing;
        case Phase::Lead:
            if (!held) {
                phase_ = Phase::Idle;
                return Out::TapByGame;
            }
            if (nowMs - startMs_ >= holdMs) {
                phase_ = Phase::Open;
                return Out::Open;
            }
            return Out::Nothing;
        case Phase::Open:
            if (!held) {
                phase_ = Phase::Idle;
                return Out::Confirm;
            }
            return Out::Nothing;
        }
        return Out::Nothing;
    }
    // Menu, pose loss, a second wheel: close it. The next Update returns Cancel (if the button was down) and drops the button.
    void Abort() { aborted_ = true; }
    bool Open() const { return phase_ == Phase::Open; }
    bool ButtonDown() const { return phase_ == Phase::Lead || phase_ == Phase::Open; }
    unsigned long long HeldMs(unsigned long long nowMs) const { return phase_ == Phase::Idle ? 0 : nowMs - startMs_; }

private:
    enum class Phase { Idle, Pressed, Lead, Open };
    Phase phase_ = Phase::Idle;
    unsigned long long startMs_ = 0;
    bool aborted_ = false;
    bool waitRelease_ = false;
};

// The hand's slot from its offset to the body centre (metres, dx right, dy up). Inside centreM = None; else the larger of |dx|,
// |dy| picks the axis (spec C1). A new reading becomes Committed() after kPolls consecutive polls; a lost pose = None at once.
class SlotPicker {
public:
    static constexpr int kPolls = 3;
    Slot Update(bool poseOk, float dx, float dy, float centreM)
    {
        if (!poseOk) {
            committed_ = Slot::None;
            cand_ = Slot::None;
            n_ = 0;
            return committed_;
        }
        const float ax = dx < 0 ? -dx : dx;
        const float ay = dy < 0 ? -dy : dy;
        Slot s = Slot::None;
        if (ax * ax + ay * ay > centreM * centreM) {
            s = ax >= ay ? (dx >= 0 ? Slot::Right : Slot::Left) : (dy >= 0 ? Slot::Up : Slot::Down);
        }
        if (s == cand_) {
            if (n_ < kPolls) { ++n_; }
        } else {
            cand_ = s;
            n_ = 1;
        }
        if (n_ >= kPolls) { committed_ = cand_; }
        return committed_;
    }
    Slot Committed() const { return committed_; }
    void Reset()
    {
        committed_ = cand_ = Slot::None;
        n_ = 0;
    }

private:
    Slot committed_ = Slot::None;
    Slot cand_ = Slot::None;
    int n_ = 0;
};

// Full deflection toward a slot, in the game's own slot directions (our gamedll sub_180511DA0: 0 up, 1 right, 2 down, 3 left).
inline void StickFor(Slot s, float& x, float& y)
{
    x = 0.0f;
    y = 0.0f;
    switch (s) {
    case Slot::Up: y = 1.0f; break;
    case Slot::Right: x = 1.0f; break;
    case Slot::Down: y = -1.0f; break;
    case Slot::Left: x = -1.0f; break;
    case Slot::None: break;
    }
}

// D11: steer to the hand's slot; at the centre, to the player's CURRENT slot (the game's index 0..3, read from PlayerDI by the
// caller - spec 6a). An unreadable current slot -> None (feed nothing; a centre release then commits whatever the cursor holds).
inline Slot TargetSlot(Slot hand, int currentSlot)
{
    if (hand != Slot::None) { return hand; }
    switch (currentSlot) {
    case 0: return Slot::Up;
    case 1: return Slot::Right;
    case 2: return Slot::Down;
    case 3: return Slot::Left;
    default: return Slot::None;
    }
}

inline const char* SlotName(Slot s)
{
    switch (s) {
    case Slot::Up: return "Up";
    case Slot::Right: return "Right";
    case Slot::Down: return "Down";
    case Slot::Left: return "Left";
    case Slot::None: break;
    }
    return "None";
}

}  // namespace dlvr::wheel
