// Melee throw (VR gesture) - docs/superpowers/specs/2026-09-30-melee-throw-design.md §3 / §3a.
// The decision only: no OpenXR, no Windows, no logging, so src/src/tests/meleethrow_logic_test.cpp compiles it alone.
// meleethrow.cpp reads the inputs, makes the game calls this asks for, and logs [THROW].
#pragma once

namespace dlvr {
namespace mthrow {

struct Params {
    float heightY;       // throw_height_y: READY needs the grip's head-yaw-frame Y >= this (metres, +Y up)
    float trigPress;     // throw_trig_press: the trigger counts as pulled at >= this
    float trigRelease;   // throw_trig_release: ... and as let go at <= this (no change in between)
    float releaseSpeed;  // throw_release_speed: forward speed (m/s) at the let-go that makes it a THROW
    float holdoffMs;     // throw_swing_holdoff_ms: the swing stays muted this long after a throw
    float maxVelAgeMs;   // the newest valid forward speed must be at most this old at the let-go, else CANCEL
};

struct Input {
    double tMs = 0.0;           // monotonic ms
    bool   meleeInHand = false; // the right hand holds class WeaponVis
    bool   triggerOk = false;   // the VR trigger was readable this frame
    float  trigger = 0.0f;
    bool   poseOk = false;      // head AND right grip valid
    float  handY = 0.0f;        // poseOk only: the grip's Y in the head-yaw frame
    bool   fwdValid = false;    // the swing detector produced a velocity this frame
    float  fwdSpeed = 0.0f;     // fwdValid only: its forward (-Z head-yaw) component, m/s
    bool   flag = false;        // WeaponMeleeController+788 as read this frame
};

enum class State { Idle, Readying, Ready, Holdoff, WaitRelease };
enum class Event { None, Readying, Ready, AlreadyReady, Refused, Lost, Throw, Cancel, CancelNoMelee };

struct Step {
    Event ev = Event::None;
    State state = State::Idle;  // after this update
    bool  sendReady = false;    // driver: 276 presses one at a time until +788 reads 1, at most 2
    bool  sendCancel = false;   // driver: ONE 276 press, only if +788 reads 1 at that instant
    bool  sendThrow = false;    // driver: CanReadRightInput, then OnRightAttack if it said yes
    bool  muteSwing = false;    // driver: no B1 hit this frame
    bool  relValid = false;     // Throw / Cancel: a fresh forward speed existed at the let-go
    float relSpeed = 0.0f;      // relValid only
    float relAgeMs = -1.0f;     // relValid only: its age
    float readyMs = 0.0f;       // events out of Readying / Ready: ms since READYING began
};

inline const char* EventName(Event e)
{
    switch (e) {
    case Event::Readying:      return "READYING";
    case Event::Ready:         return "READY";
    case Event::AlreadyReady:  return "READY (already)";
    case Event::Refused:       return "REFUSED";
    case Event::Lost:          return "LOST";
    case Event::Throw:         return "THROW";
    case Event::Cancel:        return "CANCEL";
    case Event::CancelNoMelee: return "CANCEL (melee left the hand)";
    default:                   return "none";
    }
}

class Machine {
public:
    Step Update(const Input& in, const Params& p)
    {
        Step st{};
        // Trigger hysteresis. An unreadable trigger counts as let go - and can never throw (relValid needs triggerOk).
        const bool wasDown = down_;
        if (!in.triggerOk) {
            down_ = false;
        } else if (!down_ && in.trigger >= p.trigPress) {
            down_ = true;
        } else if (down_ && in.trigger <= p.trigRelease) {
            down_ = false;
        }
        const bool letGo = wasDown && !down_;
        if (in.fwdValid) {
            lastFwd_ = in.fwdSpeed;
            lastFwdMs_ = in.tMs;
            haveFwd_ = true;
        }

        switch (state_) {
        case State::Idle:
            // Trigger held, melee in hand, hand above the line - whichever happens last.
            if (down_ && in.meleeInHand && in.poseOk && in.handY >= p.heightY) {
                readyStartMs_ = in.tMs;
                if (in.flag) {
                    state_ = State::Ready;   // a 276 now would toggle it OFF (INV-313)
                    st.ev = Event::AlreadyReady;
                } else {
                    state_ = State::Readying;
                    st.sendReady = true;
                    st.ev = Event::Readying;
                }
            }
            break;
        case State::Readying:
            st.readyMs = static_cast<float>(in.tMs - readyStartMs_);
            if (in.flag) {
                state_ = State::Ready;
                st.ev = Event::Ready;
            } else {
                state_ = down_ ? State::WaitRelease : State::Idle;
                st.ev = Event::Refused;
            }
            break;
        case State::Ready:
            st.readyMs = static_cast<float>(in.tMs - readyStartMs_);
            if (!in.flag) {
                // The game cleared it. Send nothing: a 276 while clear only stamps the tap time (INV-313).
                state_ = down_ ? State::WaitRelease : State::Idle;
                st.ev = Event::Lost;
            } else if (!in.meleeInHand) {
                st.sendCancel = true;   // no timeout in the game - never leave it ready
                state_ = down_ ? State::WaitRelease : State::Idle;
                st.ev = Event::CancelNoMelee;
            } else if (letGo) {
                const double age = haveFwd_ ? in.tMs - lastFwdMs_ : -1.0;
                st.relValid = haveFwd_ && in.triggerOk && age >= 0.0 && age <= p.maxVelAgeMs;
                if (st.relValid) {
                    st.relSpeed = lastFwd_;
                    st.relAgeMs = static_cast<float>(age);
                }
                if (st.relValid && lastFwd_ >= p.releaseSpeed) {
                    st.sendThrow = true;
                    holdEndMs_ = in.tMs + p.holdoffMs;
                    state_ = State::Holdoff;
                    st.ev = Event::Throw;
                } else {
                    // Jay 2026-09-30: ANY let-go without throw speed cancels, including before the motion starts.
                    st.sendCancel = true;
                    state_ = State::Idle;
                    st.ev = Event::Cancel;
                }
            }
            break;
        case State::Holdoff:
            if (in.tMs >= holdEndMs_) { state_ = down_ ? State::WaitRelease : State::Idle; }
            break;
        case State::WaitRelease:
            if (!down_) { state_ = State::Idle; }
            break;
        }
        st.state = state_;
        st.muteSwing = state_ == State::Readying || state_ == State::Ready || state_ == State::Holdoff;
        return st;
    }

    State state() const { return state_; }

private:
    State  state_ = State::Idle;
    bool   down_ = false;
    bool   haveFwd_ = false;
    float  lastFwd_ = 0.0f;
    double lastFwdMs_ = 0.0;
    double readyStartMs_ = 0.0;
    double holdEndMs_ = 0.0;
};

// ---- FAST RELEASE (Jay 2026-11-11: "a - release on let go, every weapon") ----
// OUR gamedll 97046bba, re_analysis/throw7_delay_ours.c + throw4 / throw5. After OnRightAttack the throw waits twice:
//   (1) START: sub_180DC3650 starts the queued hit +284 only once  now - +716 > sub_180DBDC80  (= float +904[type] while
//       +804 is 0, which +DB3DC0 sets at OnRightAttack). FastBackdate moves +716 back by that delay (+ a margin), so the
//       throw starts on the next update with every other start condition of the game's still applied. The margin stays
//       well inside the game's own drop window (DB1D10:1886, the queued hit is dropped once elapsed > delay + 0.15 s).
//   (2) RELEASE: in state 18/19 slot 10 sets BYTE +1626 at the state's first event time (table +1088, 5 floats per state);
//       the slot-16 update then runs the release ONCE (sub_180DBC0B0, guarded by +1608 == 0). FastTracker decides when
//       WE set +1626: the throw has started (state 18/19), nothing released yet (+1608 0, +1626 0), and the launch point
//       +324 is already filled by the game's own state-18/19 per-frame (sub_180DC3970) - without it state 19's release
//       would launch from an unset point (throw5:224-235).
// Pure decisions only: meleethrow.cpp reads the fields and does the writes.

constexpr double kFastTimeoutMs = 1500.0;   // armed but no throw state within this: give up (the game keeps its own path)
constexpr float  kFastMaxDelayS = 5.0f;     // a start delay above this is not a value we understand: refuse, write nothing

inline bool FastIsThrowState(int s) { return s == 18 || s == 19; }

// The new +716 stamp. False (write nothing) when the stamp is unset (the game's -1.0) or either value is not sane.
inline bool FastBackdate(float stamp, float delayS, float marginS, float* out)
{
    if (!(stamp >= 0.0f) || !(delayS == delayS) || delayS > kFastMaxDelayS) { return false; }   // NaN fails both tests
    const float d = delayS > 0.0f ? delayS : 0.0f;
    *out = stamp - d - marginS;
    return true;
}

struct FastObs {
    int  state = -1;         // +0x110
    int  nextHit = -1;       // +284
    int  count1608 = 0;      // +1608: the release / event counter (0 = not released)
    bool rel1626 = false;    // BYTE +1626: the game's own release request
    bool launchSet = false;  // the launch point +324 is not (0,0,0)
};

enum class FastEv { None, Started, Released, GameReleased, Dropped, Timeout, NotThrow, Ended };

inline const char* FastEvName(FastEv e)
{
    switch (e) {
    case FastEv::None: return "NONE";
    case FastEv::Started: return "STARTED";
    case FastEv::Released: return "RELEASED";
    case FastEv::GameReleased: return "GAME RELEASED";
    case FastEv::Dropped: return "DROPPED";
    case FastEv::Timeout: return "TIMEOUT";
    case FastEv::NotThrow: return "NOT A THROW";
    case FastEv::Ended: return "ENDED";
    }
    return "?";
}

struct FastStep {
    FastEv ev = FastEv::None;
    bool   writeRelease = false;  // set BYTE +1626 = 1 now
    double startMs = 0.0;         // when the throw state was first seen (valid from Started on)
};

class FastTracker {
public:
    void Arm(double tMs) { armed_ = true; inThrow_ = false; armMs_ = tMs; startMs_ = 0.0; }
    bool active() const { return armed_; }

    FastStep Step(double tMs, const FastObs& o)
    {
        FastStep st;
        if (!armed_) { return st; }
        if (!inThrow_) {
            if (FastIsThrowState(o.state)) {
                inThrow_ = true;
                startMs_ = tMs;
                st.ev = FastEv::Started;
            } else if (o.state != -1) {
                armed_ = false;
                st.ev = FastEv::NotThrow;
                return st;
            } else if (o.nextHit == -1) {
                armed_ = false;
                st.ev = FastEv::Dropped;
                return st;
            } else if (tMs - armMs_ > kFastTimeoutMs) {
                armed_ = false;
                st.ev = FastEv::Timeout;
                return st;
            } else {
                return st;
            }
        }
        st.startMs = startMs_;
        if (!FastIsThrowState(o.state)) {
            armed_ = false;
            st.ev = FastEv::Ended;
            return st;
        }
        if (o.count1608 != 0 || o.rel1626) {
            armed_ = false;
            st.ev = FastEv::GameReleased;
            return st;
        }
        if (o.launchSet) {
            armed_ = false;
            st.ev = FastEv::Released;
            st.writeRelease = true;
        }
        return st;
    }

private:
    bool   armed_ = false;
    bool   inThrow_ = false;
    double armMs_ = 0.0;
    double startMs_ = 0.0;
};

}  // namespace mthrow
}  // namespace dlvr
