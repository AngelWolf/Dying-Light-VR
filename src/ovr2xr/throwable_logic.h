// Throwing by hand (VR) - docs/superpowers/specs/2026-10-14-throwables-design.md, Build A.
// The decision only: no OpenXR, no Windows, no logging, so src/src/tests/throwable_logic_test.cpp compiles it alone.
// throwable.cpp reads the inputs, makes the game calls each Step asks for, and does the logging.
//
// What it decides: LT pressed at the left hip readies the selected throwable and the layer HOLDS L2 (the player never holds
// LT); RT let-go throws (L2 goes to 0, which makes the game throw); cancel = LT at the hip again, an item switch / weapon
// swap, or a blocking menu. The rule behind every cancel (INV-563..570): the game re-reads L2 only on CHANGE, and L2
// dropping while the game state is 2 THROWS, look-aimed. So a cancel is requestCancel with L2 still held, L2 stays held on
// every following poll until the game state reads back 0, and only then is the hold dropped (a switch is passed on that same
// poll). A cancel that never reads back 0 is retried, never released.
//
// Velocity rule (fastest hand motion in a window, direction from a short stroke around it, the item's own speed scaled by
// min(hand speed / full speed, cap)): after Mamfro's dying-light-games-vr, dl1/src/player/throwing.rs.
#pragma once

#include <cmath>
#include <cstdint>

namespace dlvr {
namespace throwable {

struct Vec3 { float x = 0, y = 0, z = 0; };

struct Params {
    float trigPress = 0.6f, trigRelease = 0.3f;   // RT / LT hysteresis
    float fullSpeed = 5.0f;     // throwable_full_speed: hand m/s that gives the item's own full speed
    float mostShare = 1.25f;    // cap on that share (Mamfro)
    float stillSpeed = 0.05f;   // throwable_still_speed: below = a drop
    float dropSpeed = 0.5f;     // drop velocity, m/s straight down
    double windowMs = 500, strokeBeforeMs = 100, strokeAfterMs = 40;   // velocity window (Mamfro)
    double readyTimeoutMs = 1500;   // Readying -> no game state 1/2 within this = refused, back to Idle
    double releaseTimeoutMs = 1500; // Releasing -> no game release within this = Idle (L2 already 0)
    double cancelTimeoutMs = 500;   // cancelling -> the game still not at state 0 = CancelStuck, request again, KEEP the hold
    double reReadyTimeoutMs = 1000; // after a throwable -> throwable switch passed: the new item not seen within this = ReReadyTimeout
};

enum class State { Idle, Readying, Readied, Releasing };
enum class Event { None, Ready, Readied, Release, Cancel, CancelForSwitch, Refused, Released, ReleaseTimeout, Lost,
                   Withheld, CancelStuck, ReReadyTimeout };

struct Input {
    double tMs = 0;
    bool   throwableSelected = false;  // the selected equipment IsThrowable (Task 1 step 6)
    bool   ltOk = false; float lt = 0;                        // left trigger (no hip meaning since 2026-11-01: only withheld)
    bool   rtOk = false; float rt = 0;                        // right trigger
    bool   rtInRHip = false;      // the right hand inside the right-hip zone (spec 2026-11-01 goal 2: R2 there readies)
    bool   switchPress = false;   // an item-switch or weapon-swap press this poll (D-pad left / right, holsters, EQUIPEMENT_NEXT/PREV)
    bool   switchToThrowable = false;  // switchPress only: the item it switches TO is a throwable (unknown -> false)
    bool   menuBlocking = false;  // dlvr_MenuInputPolicy != 0
    int    gameState = -1;        // [ctrl+0x38], -1 = unreadable
    bool   gameReleased = false;  // [ctrl+0x7E]
    std::uintptr_t selectedItem = 0;  // identity of the selected equipment item, 0 = unknown (a re-ready needs it to CHANGE)
    // INV-574: the game has finished the hand change - the throw controller is inactive, or holds no item, or holds the
    // selected one. A re-ready before that restarts the controller's OLD item (+DD1260) and locks the hand on it.
    bool   switchSettled = true;
};

struct Step {
    Event ev = Event::None; State state = State::Idle;
    bool holdL2 = false;       // write L2 = 255 this poll
    bool withholdL2 = false;   // write L2 = 0 this poll (LT away from the hip, or our own drop for a release)
    bool withholdR2 = false;   // R2 = 0 this poll (a throwable is out)
    bool requestCancel = false;  // driver: set-state(ctrl, 0) + [ctrl+0x70] = -1 THIS poll, before L2 is released
    bool snapshotRelease = false;// driver: freeze the hand position + velocity NOW (the RT let-go)
    bool reReady = false;      // after a throwable -> throwable switch: start Readying again once the game shows idle
    bool passSwitch = true;    // false = hold the switch press back this poll (cancel first)
};

class Machine {
public:
    Step Update(const Input& in, const Params& p)
    {
        Step st{};
        // Trigger hysteresis. An unreadable trigger keeps its last state (no edge).
        const bool ltWas = ltDown_, rtWas = rtDown_;
        ltDown_ = Hyst(ltDown_, in.ltOk, in.lt, p);
        rtDown_ = Hyst(rtDown_, in.rtOk, in.rt, p);
        const bool ltPressed = !ltWas && ltDown_;
        const bool rtPressed = !rtWas && rtDown_;
        const bool rtReleased = rtWas && !rtDown_;
        // A cancel / finish that ended with RT still held keeps that R2 from the game until it is let go (ruling, plan
        // 2026-11-01-manual-reload-pistol Task 3: the gun or melee weapon would otherwise fire as the throwable is put away).
        if (rtHoldOver_ && !rtDown_) { rtHoldOver_ = false; }

        // A cancel in flight overrides everything: hold L2 until the game reads back 0.
        if (cancelling_) {
            st.withholdR2 = true;
            if (in.gameState == -1) {
                if (!unreadable_) { unreadable_ = true; unreadableSinceMs_ = in.tMs; }
            } else {
                unreadable_ = false;
            }
            // Ruling R23: a 0 read before the game ever showed 1/2 may be "the start is still pending" (+DD3500 writes
            // [+0x74] = 1 and a LATER Start turns it into state 1) - dropping L2 then throws at the feet. So a 0 ends the
            // wait only once 1/2 has been shown (since the Ready, or after the cancel), or after readyTimeoutMs since the
            // Ready with no start (it never landed). Each time 1/2 appears, the cancel is asked for again at once.
            const bool shown = in.gameState == 1 || in.gameState == 2;
            const bool wasShown = lastGs_ == 1 || lastGs_ == 2;
            lastGs_ = in.gameState;
            const bool appeared = shown && !wasShown;
            if (shown) { sawStart_ = true; }
            const bool zeroDone = in.gameState == 0 && (sawStart_ || in.tMs - readyAtMs_ >= p.readyTimeoutMs);
            if (zeroDone) {
                cancelling_ = false;
                if (cancelForSwitch_) { reReadyAtMs_ = in.tMs; latchedItem_ = in.selectedItem; }
                else { reReadyWanted_ = false; }
                if (cancelRefused_) { st.ev = Event::Refused; }
                FinishIdle(st, in);
            } else if (unreadable_ && in.tMs - unreadableSinceMs_ >= p.cancelTimeoutMs) {
                // The controller has been unreadable for the whole timeout: it is gone. Nothing left to cancel or hold.
                cancelling_ = false;
                reReadyWanted_ = false;
                st.ev = Event::Lost;
                FinishIdle(st, in);   // passSwitch stays true: any held switch goes through
            } else {
                st.holdL2 = true;
                st.passSwitch = !cancelForSwitch_;
                if (appeared) {
                    st.requestCancel = true;   // R23: the start landed - cancel it now, in 1/2
                    cancelStartMs_ = in.tMs;
                } else if (in.gameState != 0 && in.tMs - cancelStartMs_ >= p.cancelTimeoutMs) {
                    // (a 0 still waiting for a pending start is not "stuck": it ends by R23's rule above)
                    // Never drop L2 here: that would throw at the player's feet. Ask again and keep waiting. The game
                    // refuses an equipment switch while readied (INV-565), so passing a held switch cannot swap mid-throw.
                    st.ev = Event::CancelStuck;
                    st.requestCancel = true;
                    st.passSwitch = true;
                    cancelStartMs_ = in.tMs;
                }
            }
            st.state = state_;
            return st;
        }

        switch (state_) {
        case State::Idle: {
            bool started = false;
            if (reReadyWanted_) {
                if (in.tMs - reReadyAtMs_ >= p.reReadyTimeoutMs) {
                    reReadyWanted_ = false;
                    st.ev = Event::ReReadyTimeout;   // the normal Idle trigger handling below still runs this poll
                } else if (in.throwableSelected && in.gameState == 0 && !in.menuBlocking && in.switchSettled &&
                           in.selectedItem != 0 && latchedItem_ != 0 && in.selectedItem != latchedItem_) {
                    // 0 = unknown is never a change and never a baseline: a latched 0 can only time out.
                    // switchSettled (INV-574): the new item alone is the HUD; the hand changes later - wait for it.
                    reReadyWanted_ = false;
                    StartReadying(st, in);
                    st.reReady = true;
                    started = true;
                }
            }
            if (!started && in.throwableSelected) {
                // R2 at the RIGHT hip readies (spec 2026-11-01 goal 2). gameState -1 (controller unreadable): never start, or
                // the hold could not be cancelled safely.
                if (rtPressed && in.rtInRHip && !in.menuBlocking && in.gameState != -1) {
                    StartReadying(st, in);
                } else if (rtDown_ && in.rtInRHip) {
                    // Jay: "Withheld from whatever the right hand holds" - R2 at the hip with a throwable selected never fires.
                    st.withholdR2 = true;
                }
                if (ltDown_) {
                    // LT with a throwable selected (anywhere): the game's own L2 would use the equipment - never passed.
                    st.withholdL2 = true;
                    if (ltPressed && st.ev == Event::None) { st.ev = Event::Withheld; }
                }
            }
            if (rtHoldOver_) { st.withholdR2 = true; }
            break;
        }
        case State::Readying:
            st.holdL2 = true;
            st.withholdR2 = true;
            if (in.menuBlocking) {
                BeginCancel(st, in, Event::Cancel, false);
            } else if (in.switchPress) {
                BeginCancel(st, in, Event::CancelForSwitch, true);
            } else if (!in.throwableSelected) {
                // Ruling R21: no longer selected (a vehicle entered, the item read lost) = a cancel like a switch to a
                // non-throwable - the same wait for state 0 with L2 held, never a dropped L2 (a throw) or a held one (a brake).
                BeginCancel(st, in, Event::Cancel, false);
            } else if (rtReleased) {
                // RT let go before the item is in hand (plan 2026-11-01 Review Focus 4): a cancel, never a throw.
                BeginCancel(st, in, Event::Cancel, false);
            } else if (in.gameState == 2) {
                state_ = State::Readied;
                st.ev = Event::Readied;
                rtArmed_ = rtDown_;   // the RT that readied it is still held: its let-go is the throw (or the put-back)
            } else {
                if (in.gameState == 1 && !sawOne_) { sawOne_ = true; stateSinceMs_ = in.tMs; }
                if (in.tMs - stateSinceMs_ >= p.readyTimeoutMs) {
                    if (in.gameState == 0) {
                        st.ev = Event::Refused;   // the game never left 0: nothing to cancel, L2 may drop
                        FinishIdle(st, in);
                    } else {
                        // Off 0 (or unreadable): the same cancel wait as any cancel; Refused is reported when 0 reads back.
                        BeginCancel(st, in, Event::None, false);
                        cancelRefused_ = true;
                    }
                }
            }
            break;
        case State::Readied:
            st.holdL2 = true;
            st.withholdR2 = true;
            if (in.gameState == 0) {
                st.ev = Event::Lost;   // the game cancelled by itself: nothing to cancel, nothing to hold
                FinishIdle(st, in);
            } else if (in.menuBlocking) {
                BeginCancel(st, in, Event::Cancel, false);
            } else if (in.switchPress) {
                BeginCancel(st, in, Event::CancelForSwitch, true);
            } else if (!in.throwableSelected) {
                BeginCancel(st, in, Event::Cancel, false);   // ruling R21, as in Readying
            } else if (rtReleased && rtArmed_ && in.rtInRHip) {
                // Let go back at the right hip = put it back (Jay, plan time: "Let go at the hip").
                BeginCancel(st, in, Event::Cancel, false);
            } else {
                if (rtPressed) { rtArmed_ = true; }
                if (rtReleased && rtArmed_) {
                    // L2 -> 0 while the game state is 2 is the throw.
                    state_ = State::Releasing;
                    stateSinceMs_ = in.tMs;
                    st.ev = Event::Release;
                    st.snapshotRelease = true;
                    st.holdL2 = false;
                    st.withholdL2 = true;
                }
            }
            break;
        case State::Releasing:
            st.withholdL2 = true;
            st.withholdR2 = true;
            if (in.gameReleased) {
                st.ev = Event::Released;
                FinishIdle(st, in);
            } else if (in.tMs - stateSinceMs_ >= p.releaseTimeoutMs) {
                st.ev = Event::ReleaseTimeout;   // L2 is already 0: nothing is held
                FinishIdle(st, in);
            }
            break;
        }
        st.state = state_;
        return st;
    }

    State state() const { return state_; }
    // Ruling R24: a throw is in hand or in flight - Readying / Readied / Releasing, or any cancel wait.
    bool busy() const { return state_ != State::Idle || cancelling_; }

private:
    static bool Hyst(bool down, bool ok, float v, const Params& p)
    {
        if (!ok) { return down; }   // no information: keep the last state, so a dropout is never a let-go
        if (!down && v >= p.trigPress) { return true; }
        if (down && v <= p.trigRelease) { return false; }
        return down;
    }

    void StartReadying(Step& st, const Input& in)
    {
        state_ = State::Readying;
        stateSinceMs_ = in.tMs;
        readyAtMs_ = in.tMs;
        sawOne_ = false;
        st.ev = Event::Ready;
        st.holdL2 = true;
        st.withholdR2 = true;
    }

    // Back to Idle with nothing held. A trigger still down must not reach the game (it would ready or throw by itself).
    void FinishIdle(Step& st, const Input& in)
    {
        state_ = State::Idle;
        st.holdL2 = false;
        rtHoldOver_ = rtDown_;
        st.withholdR2 = rtDown_;
        st.withholdL2 = in.throwableSelected && ltDown_;
    }

    void BeginCancel(Step& st, const Input& in, Event ev, bool forSwitch)
    {
        cancelling_ = true;
        cancelForSwitch_ = forSwitch;
        cancelRefused_ = false;
        unreadable_ = false;
        cancelStartMs_ = in.tMs;
        // R23: has the game shown its start (1/2) since the Ready? Readied always has; Readying only if 1 was seen.
        sawStart_ = state_ == State::Readied || sawOne_ || in.gameState == 1 || in.gameState == 2;
        lastGs_ = in.gameState;
        reReadyWanted_ = forSwitch && in.switchToThrowable;
        st.ev = ev;
        st.requestCancel = true;
        st.holdL2 = true;
        st.withholdR2 = true;
        st.passSwitch = !forSwitch;
    }

    State  state_ = State::Idle;
    bool   ltDown_ = false, rtDown_ = false;     // hysteresis, trigPress / trigRelease
    bool   rtArmed_ = false;                     // RT held / pressed while Readied: only its let-go throws (or puts back)
    bool   rtHoldOver_ = false;                  // RT still held after a cancel / finish: withheld until let go
    bool   sawOne_ = false;                      // the game showed state 1 while Readying
    bool   cancelling_ = false;                  // requestCancel issued: hold L2 until the game reads 0
    bool   cancelForSwitch_ = false;             // the cancel holds a switch press back until it finishes
    bool   cancelRefused_ = false;               // this cancel is a Readying timeout: report Refused when it completes
    bool   unreadable_ = false;                  // gameState has read -1 continuously since unreadableSinceMs_
    bool   sawStart_ = false;                    // R23: the game showed 1/2 since the Ready (a 0 then means cancelled)
    int    lastGs_ = -1;                         // R23: the previous poll's gameState in a cancel wait (1/2 edge)
    double readyAtMs_ = 0;                       // R23: when the Ready was issued
    double unreadableSinceMs_ = 0;
    std::uintptr_t latchedItem_ = 0;             // selectedItem on the poll the switch passed
    bool   reReadyWanted_ = false;              // throwable -> throwable switch: ready again once the game shows idle
    double stateSinceMs_ = 0;                    // for readyTimeoutMs / releaseTimeoutMs
    double cancelStartMs_ = 0;                   // for cancelTimeoutMs
    double reReadyAtMs_ = 0;                     // when the switch passed; a re-ready is dropped after readyTimeoutMs
};

// Velocity: positions are WORLD (already mapped). Ring of the last 128 samples.
class HandHistory {
public:
    void Push(double tMs, Vec3 pos)
    {
        buf_[head_] = Sample{ tMs, pos };
        head_ = (head_ + 1) % kCap;
        if (count_ < kCap) { ++count_; }
    }

    void Clear() { head_ = 0; count_ = 0; }

    // The fastest hand motion in [nowMs - windowMs, nowMs]: the velocity over the shortest span of at least kMinSpanMs
    // (per-frame jitter would read as speed over one 11 ms step). Among equal speeds the LATEST wins, so atMs is the end of a
    // flick and the stroke window before it lies inside the flick. false = no usable pair.
    bool Fastest(double nowMs, double windowMs, Vec3& v, double& atMs) const
    {
        const double lo = nowMs - windowMs;
        bool found = false;
        double best2 = 0;
        for (int j = 1; j < count_; ++j) {
            const Sample& b = At(j);
            if (b.t > nowMs || b.t < lo) { continue; }
            for (int i = j - 1; i >= 0; --i) {
                const Sample& a = At(i);
                if (a.t < lo) { break; }
                const double dt = b.t - a.t;
                if (dt < kMinSpanMs) { continue; }
                const double dx = (b.p.x - a.p.x) / dt * 1000.0, dy = (b.p.y - a.p.y) / dt * 1000.0,
                             dz = (b.p.z - a.p.z) / dt * 1000.0;
                const double s2 = dx * dx + dy * dy + dz * dz;
                if (!found || s2 >= best2 * (1.0 - 1e-6)) {
                    found = true;
                    best2 = s2;
                    v = Vec3{ static_cast<float>(dx), static_cast<float>(dy), static_cast<float>(dz) };
                    atMs = b.t;
                }
                break;   // the shortest qualifying span for this end sample
            }
        }
        return found;
    }

    // The direction of travel across [atMs - beforeMs, atMs + afterMs]: first to last sample in the window, normalised.
    bool Stroke(double atMs, double beforeMs, double afterMs, Vec3& dir) const
    {
        const Sample* first = nullptr;
        const Sample* last = nullptr;
        for (int i = 0; i < count_; ++i) {
            const Sample& s = At(i);
            if (s.t < atMs - beforeMs || s.t > atMs + afterMs) { continue; }
            if (!first) { first = &s; }
            last = &s;
        }
        if (!first || first == last) { return false; }
        const double dx = last->p.x - first->p.x, dy = last->p.y - first->p.y, dz = last->p.z - first->p.z;
        const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (len < 1e-6) { return false; }
        dir = Vec3{ static_cast<float>(dx / len), static_cast<float>(dy / len), static_cast<float>(dz / len) };
        return true;
    }

private:
    struct Sample { double t = 0; Vec3 p; };
    static constexpr int kCap = 128;
    static constexpr double kMinSpanMs = 30.0;
    const Sample& At(int i) const { return buf_[(head_ - count_ + i + kCap) % kCap]; }   // 0 = oldest

    Sample buf_[kCap];
    int head_ = 0;
    int count_ = 0;
};

// The velocity sent: the hand's direction, the item's own speed x min(|hand| / fullSpeed, mostShare); still -> straight down at dropSpeed.
inline Vec3 ThrownVelocity(Vec3 hand, float itemFullSpeed, const Params& p)
{
    const float sp = std::sqrt(hand.x * hand.x + hand.y * hand.y + hand.z * hand.z);
    if (sp < p.stillSpeed) { return Vec3{ 0.0f, -p.dropSpeed, 0.0f }; }
    float share = sp / p.fullSpeed;
    if (share > p.mostShare) { share = p.mostShare; }
    const float k = itemFullSpeed * share / sp;
    return Vec3{ hand.x * k, hand.y * k, hand.z * k };
}

}  // namespace throwable
}  // namespace dlvr
