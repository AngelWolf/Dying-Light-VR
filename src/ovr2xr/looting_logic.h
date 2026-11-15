// Loot by hand - the per-hand decision (docs/superpowers/specs/2026-10-23-loot-by-hand-design.md, plan Task 2). Pure: no Windows,
// no game memory, no logging, so src/src/tests/looting_logic_test.cpp compiles it on its own. looting.cpp feeds it once per scePad
// poll per hand and acts on the Output (Square while searching, the grip's L1 / R1 withheld while settling or searching).
//
// A press ARMS only on the grip's down EDGE with a body in reach (a grip already held - a weapon two-handed into a body - never
// arms), and only when nothing else owns the grip (holster zones, the two-handed hold) and the other hand is not already looting.
// The hand must stay below stillSpeed for settleS; moving or losing the body first SPENDS the press (no re-arm until the grip opens).
// Once SEARCHING the game owns every cancel (INV-615: the TriggerController's own release / range rules) - this only keeps Square
// down until the grip opens.
//
// Body grab (docs/superpowers/specs/2026-10-23-body-grab-design.md, plan Task 2), only while Params.dragEnabled: a press on the body
// that is NOT on the torso, a torso press that moves before the search, or a torso press while the other hand already loots that body
// (one search per body, spec 6b.4) DRAGS instead (bodygrab.cpp pulls the body toward the hand). A drag holds the grip like a loot and
// ends when the grip opens, the body is gone, or the input is blocked (menu / calibration / loading). dragEnabled off = loot by hand
// exactly as before.
#pragma once

#include <cmath>

namespace looting {

// Distance from p to an axis-aligned box { min xyz, max xyz } (0 inside).
inline float BoxDist(const float b[6], const float p[3])
{
    float d2 = 0.0f;
    for (int k = 0; k < 3; ++k) {
        const float lo = b[k], hi = b[k + 3];
        const float v = p[k] < lo ? lo - p[k] : (p[k] > hi ? p[k] - hi : 0.0f);
        d2 += v * v;
    }
    return std::sqrt(d2);
}

// INV-616: a fresh corpse is the dead AI, with its search container attached at spine2. Whether the AI's own box or the container's
// box tracks a lying corpse is not established, so the hand is measured to the NEARER of the two. *da / *dc = each box's distance,
// -1 where that box is missing. Returns -1 when neither is present.
inline float NearerBoxDist(const float a[6], bool haveA, const float c[6], bool haveC, const float p[3], float* da, float* dc)
{
    *da = haveA ? BoxDist(a, p) : -1.0f;
    *dc = haveC ? BoxDist(c, p) : -1.0f;
    if (*da < 0.0f) { return *dc; }
    if (*dc < 0.0f) { return *da; }
    return *da < *dc ? *da : *dc;
}

// Right grip camouflage block (Jay 10-23 (3), option 1): whether a press is withheld is decided ONCE, at its down edge, and held until
// the grip opens - a target that comes or goes mid-press never makes R1 start or stop half way through a hold. Step returns
// "withhold now". The first sample only learns the grip state (a grip already held at start is not a press).
class PressLatch {
public:
    bool Step(bool down, bool blockNow)
    {
        if (down && !prev_ && seen_) { latched_ = blockNow; }
        if (!down) { latched_ = false; }
        prev_ = down;
        seen_ = true;
        return down && latched_;
    }

private:
    bool prev_ = false;
    bool seen_ = false;
    bool latched_ = false;
};

// Hand speed over a ~100 ms window of grip-pose samples (D-269: the scePad poll runs ~570/s while the pose moves once per display
// frame, so a per-poll delta reads a still hand as moving). Speed() = displacement from the newest sample at least windowS older
// than the newest, over that time; with no sample that old, the oldest held one if it is >= minS old; otherwise -1 (not filled).
class SpeedWindow {
public:
    static constexpr int kCap = 128;   // ~224 ms at 570 polls/s
    void Reset() { n_ = 0; }
    void Push(double t, const float p[3])
    {
        head_ = (head_ + 1) % kCap;
        t_[head_] = t;
        for (int k = 0; k < 3; ++k) { p_[head_][k] = p[k]; }
        if (n_ < kCap) { ++n_; }
    }
    float Speed(double windowS = 0.10, double minS = 0.05) const
    {
        if (n_ < 2) { return -1.0f; }
        const double tn = t_[head_];
        int ref = -1;
        for (int i = 1; i < n_; ++i) {
            const int j = (head_ - i + kCap) % kCap;
            if (tn - t_[j] >= windowS) { ref = j; break; }
        }
        if (ref < 0) {
            const int oldest = (head_ - (n_ - 1) + kCap) % kCap;
            if (tn - t_[oldest] < minS) { return -1.0f; }
            ref = oldest;
        }
        const float dx = p_[head_][0] - p_[ref][0], dy = p_[head_][1] - p_[ref][1], dz = p_[head_][2] - p_[ref][2];
        return std::sqrt(dx * dx + dy * dy + dz * dz) / static_cast<float>(tn - t_[ref]);
    }

private:
    double t_[kCap] = {};
    float p_[kCap][3] = {};
    int head_ = 0;
    int n_ = 0;
};

struct Params {
    float reachM = 0.25f;      // loot_reach_m: hand to the body's world box
    float stillSpeed = 0.30f;  // loot_still_speed: m/s, the hand counts as still below it
    float settleS = 0.30f;     // loot_settle_s: still this long before the search starts
    bool dragEnabled = false;  // body_grab: non-torso / moving presses drag the body instead of spending the press
    // body_grab, 10-24 (2) (RESEARCH-BODY-GRAB "READS FOR THE THREE REFINEMENTS" C): a press edge while the severed-limb search
    // has not yet reported for this squeeze WAITS (grip withheld) until it reports or this long passes, then decides as a fresh
    // press with the limb known. 0 = never wait (loot by hand exactly as before).
    double lateArmS = 0.0;
};

struct Input {
    bool gripDown = false;
    float bodyDist = -1.0f;    // < 0 = no body
    float handSpeed = 0.0f;
    double nowS = 0.0;
    bool gripOwnedElsewhere = false;   // a holster zone or the two-handed hold took this press
    bool otherHandBusy = false;        // the other hand is settling or searching
    bool onTorso = false;              // the hand is on the body's torso (only read while dragEnabled)
    bool blocked = false;              // mod menu / calibration screen / loading screen: nothing starts, a drag ends
    bool limbPending = false;          // the severed-limb search has not reported since this squeeze began (only read with lateArmS)
};

// Waiting = a press edge held for the severed-limb reading (Params.lateArmS); it resolves into an arm, a drag, or a NO-ARM.
enum class State { Idle, Settling, Searching, Spent, Dragging, Waiting };

struct Output {
    State state = State::Idle;
    bool wantSquare = false;
    bool withholdGrip = false;
    bool dragging = false;
    // "ARM" | "SEARCH" | "DRAG" | "END grip opened" | "END moved" | "END body gone" | "END drag released" | "END blocked" | nullptr
    const char* event = nullptr;
    // A press edge that did NOT arm: "blocked" | "grip owned elsewhere" | "other hand looting" | "no body" | "body out of reach" |
    // nullptr
    const char* noArm = nullptr;
    // A press that waited (State::Waiting): how long, in seconds, set on the step it resolved (arm, drag or NO-ARM); -1 = no wait.
    double waitedS = -1.0;
};

class HandMachine {
public:
    Output Step(const Input& in, const Params& p)
    {
        const bool edge = in.gripDown && !prevGrip_ && seen_;
        prevGrip_ = in.gripDown;
        seen_ = true;
        const char* ev = nullptr;
        const char* noArm = nullptr;
        double waited = -1.0;
        switch (state_) {
        case State::Idle:
            if (!edge) { break; }
            // 10-24 (2) (Jay: "Build fix 1"): while the severed-limb reading is still due the press waits for it WHATEVER is in
            // reach - a corpse whose box covers a cut-off limb lying by it must not take the press before the limb is known
            // (hitbox_dump_run1: 5 presses took the corpse with a limb 0.000-0.070 m from the hand, all "waited -1 ms").
            if (in.limbPending && p.lateArmS > 0.0 && !in.blocked && !in.gripOwnedElsewhere) {
                state_ = State::Waiting;
                t0_ = in.nowS;
                break;
            }
            Press(in, p, &ev, &noArm);
            break;
        case State::Waiting:
            waited = in.nowS - t0_;
            if (!in.gripDown) {
                state_ = State::Idle;
                noArm = "grip opened while waiting for the limb reading";
            } else if (in.limbPending && waited < p.lateArmS && !in.blocked && !in.gripOwnedElsewhere) {
                waited = -1.0;   // still due - keep waiting
            } else {
                Press(in, p, &ev, &noArm);
                if (noArm) { state_ = State::Idle; }   // a held grip never edges again: the press is over, as before
            }
            break;
        case State::Settling:
            if (!in.gripDown) {
                state_ = State::Idle;
                ev = "END grip opened";
            } else if (in.handSpeed > p.stillSpeed) {
                state_ = p.dragEnabled ? State::Dragging : State::Spent;
                ev = p.dragEnabled ? "DRAG" : "END moved";
            } else if (in.bodyDist < 0.0f || in.bodyDist > p.reachM) {
                state_ = State::Spent;
                ev = "END body gone";
            } else if (in.nowS - t0_ >= p.settleS) {
                state_ = State::Searching;
                ev = "SEARCH";
            }
            break;
        case State::Searching:
            if (!in.gripDown) {
                state_ = State::Idle;
                ev = "END grip opened";
            }
            break;
        case State::Spent:
            if (!in.gripDown) {
                state_ = State::Idle;
            }
            break;
        case State::Dragging:
            // The hand carries the body, so reach is not checked here - only that the body still exists.
            if (!in.gripDown) {
                state_ = State::Idle;
                ev = "END drag released";
            } else if (in.blocked) {
                state_ = State::Idle;
                ev = "END blocked";
            } else if (in.bodyDist < 0.0f) {
                state_ = State::Idle;
                ev = "END body gone";
            }
            break;
        }
        Output o;
        o.state = state_;
        o.wantSquare = state_ == State::Searching;
        o.dragging = state_ == State::Dragging;
        o.withholdGrip = state_ == State::Settling || state_ == State::Searching || state_ == State::Dragging ||
                         state_ == State::Waiting;
        o.event = ev;
        o.noArm = noArm;
        o.waitedS = waited;
        return o;
    }
    State state() const { return state_; }

private:
    // A press decided now: arms (Settling, "ARM"), drags (Dragging, "DRAG"), or names why not in *noArm. Returns true when the
    // reason is that no body is in reach.
    bool Press(const Input& in, const Params& p, const char** ev, const char** noArm)
    {
        if (in.blocked) {
            *noArm = "blocked";
        } else if (in.gripOwnedElsewhere) {
            *noArm = "grip owned elsewhere";
        } else if (in.otherHandBusy && !p.dragEnabled) {
            *noArm = "other hand looting";
        } else if (in.bodyDist < 0.0f) {
            *noArm = "no body";
            return true;
        } else if (in.bodyDist > p.reachM) {
            *noArm = "body out of reach";
            return true;
        } else if (p.dragEnabled && (!in.onTorso || in.otherHandBusy)) {
            state_ = State::Dragging;
            *ev = "DRAG";
        } else {
            state_ = State::Settling;
            t0_ = in.nowS;
            *ev = "ARM";
        }
        return false;
    }

    State state_ = State::Idle;
    bool prevGrip_ = false;
    bool seen_ = false;   // the first sample sets prevGrip_ only: a grip already down at start is not an edge
    double t0_ = 0.0;
};

}  // namespace looting
