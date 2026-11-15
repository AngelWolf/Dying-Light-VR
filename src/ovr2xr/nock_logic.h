// The bow's MANUAL NOCK - the decisions only (docs/superpowers/specs/2026-10-17-bow-manual-nock-design.md; plan
// docs/superpowers/plans/2026-10-17-bow-manual-nock.md Task 2). Pure: no Windows headers, no allocation, no engine reads.
// bownock.cpp builds an Input from what BowDrawTick read this player update and applies the Output; the game-side mechanism
// is INV-589 (the ammo count +D9BF60 held at 0 = the game holds state -1; state 6 = PutArrow, held while the hand carries).
// Unit test: src/src/tests/nock_logic_test.cpp.
#pragma once

#include <cmath>
#include <cstdint>

namespace dlvr {
namespace nock {

struct Vec3 { float x, y, z; };
enum class Phase { Off, Empty, Carry, Caught, Seated };
enum class Event { None, Take, TakeNoArrows, Catch, CatchWrongWay, DropToCarry, Seat, EarlyRelease, GameMoved, Shot, Disabled };
struct Params { float catchM, holdM, seatM, catchDeg, catchLenM; };

struct Input {
    bool enabled;          // bow_manual_nock && bow_manual_draw && a bow with a readable controller
    bool haveHand;         // the drawn right hand is published this update (notch / tip are meaningless without it)
    double tMs;            // a monotonic clock, ms (the post-seat hold window)
    int  gameState;       // ctrl+32 (INV-540 names: -1 Empty, 0 Stand, 1 Begin, 2 Cancel, 3 Ready, 4 Shoot, 5 Reload, 6 PutArrow)
    bool spawned;          // ctrl+188 != 0 (state 4 has spawned its arrow)
    int  trueCount;        // the real +D9BF60 count (trampoline)
    bool trigDown;         // the REAL right trigger (the game reads 0 while the string is not attached)
    bool pressEdge;        // a new press since the last step
    bool handInShoulder;   // the holster's shoulder zone, with its 1.2x exit
    Vec3 notch;            // the carried arrow_pose origin, world
    Vec3 tip;              // the carried arrow's tip direction, world, unit
    Vec3 lineOrigin;       // the string's rest point (arrow_pose rest), world
    Vec3 lineTip;          // the bow's arrow direction, world, unit (away from the string)
};

struct Output {
    Phase phase; Event ev; float evValue;   // evValue: angle (deg) for CatchWrongWay, the game state for GameMoved, else distance (m)
    bool fakeEmpty;        // the detour returns 0 for this bow
    bool holdPutArrow;     // re-stamp ctrl+72 this update
    bool endPutArrow;      // let state 6 end this update
    bool writeArrow;       // write arrow_pose this update
    bool holdRestPose;     // hold the string / limbs / root at the Stand pose (the game's Empty clip bends them - INV-589 add.)
    bool holdArrowAtRest;  // also put arrow_pose at its rest on the HELD bow (the post-seat blend window)
    bool holdFingers;      // the right hand takes the Reload clip's arrow-holding finger pose (carrying / caught)
    bool onLine;           // write it ON the line at `along` (else at the hand)
    float along;           // metres in front of the string, >= 0
};

inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// INV-590: the left bow rides ONE fixed grip rel (the draw's l_handholder local) instead of item_place_rel's per-frame animated
// socket, so it does not move in the hand between empty / reload / idle / draw / equip (Jay: "Build it, ride bow_manual_nock").
inline bool UseFixedBowRel(bool nockOn, bool drawOn, bool live, bool leftHand, bool isBow)
{
    return nockOn && drawOn && live && leftHand && isBow;
}

// The detour's answer: 0 only for OUR controller while the tick asks for it AND both switches are on - so switching either
// bow_manual_nock or bow_manual_draw off releases the bow at once, even when the tick no longer runs (final review 1).
inline bool ShouldFake(bool fakeFlag, bool sameCtrl, bool nockOn, bool drawOn) { return fakeFlag && sameCtrl && nockOn && drawOn; }

// 0..180 degrees between two directions (unit expected; normalised here so a slightly scaled input is still right).
inline float AngleDeg(Vec3 a, Vec3 b)
{
    const float la = std::sqrt(Dot(a, a)), lb = std::sqrt(Dot(b, b));
    if (!(la > 1.0e-9f) || !(lb > 1.0e-9f)) { return 180.0f; }
    float c = Dot(a, b) / (la * lb);
    c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
    return std::acos(c) * 57.29577951f;
}

class Machine {
public:
    explicit Machine(bool seated) : phase_(seated ? Phase::Seated : Phase::Empty), fromMemory_(seated) {}
    Phase phase() const { return phase_; }

    Output Step(const Input& in, const Params& p)
    {
        Output o{};
        if (!in.enabled) {
            if (phase_ != Phase::Off) { o.ev = Event::Disabled; }
            phase_ = Phase::Off;
            wrongWay_ = false;
            o.phase = phase_;
            return o;
        }
        if (phase_ == Phase::Off) { phase_ = Phase::Empty; }

        // Where the carried notch sits against the bow's line: along it (in front of the string = +) and off it.
        const Vec3 v = { in.notch.x - in.lineOrigin.x, in.notch.y - in.lineOrigin.y, in.notch.z - in.lineOrigin.z };
        const float along = Dot(v, in.lineTip);
        const Vec3 e = { v.x - in.lineTip.x * along, v.y - in.lineTip.y * along, v.z - in.lineTip.z * along };
        const float off = std::sqrt(Dot(e, e));

        switch (phase_) {
        case Phase::Seated:
            // A bow remembered nocked, first update out: the game's activation entered PutArrow (INV-589 f) - end it at once and
            // hold the pose through the blend, so it comes out with "an arrow ready" (Jay) instead of replaying the reload.
            if (fromMemory_) {
                fromMemory_ = false;
                if (in.gameState == 6) { o.endPutArrow = true; seatMs_ = in.tMs; }
            }
            // The shot: Empty only once state 4 has SPAWNED (the game's first block runs before case 4 - INV-589).
            // State 5 (Reload) only ever follows 4, so a tail that missed the spawned window still empties the bow instead of
            // letting the game's own Reload re-nock it (final review 2).
            if ((in.gameState == 4 && in.spawned) || in.gameState == 5) { o.ev = Event::Shot; phase_ = Phase::Empty; }
            else if (in.gameState == -1) { phase_ = Phase::Empty; }   // the game emptied it (the last arrow, a zero count)
            break;
        case Phase::Empty:
            if (in.pressEdge && in.handInShoulder) {
                if (in.trueCount > 0) { o.ev = Event::Take; o.evValue = static_cast<float>(in.trueCount); phase_ = Phase::Carry; wrongWay_ = false; }
                else { o.ev = Event::TakeNoArrows; }
            }
            break;
        case Phase::Carry:
        case Phase::Caught:
            if (!in.trigDown) { o.ev = Event::EarlyRelease; phase_ = Phase::Empty; break; }
            if (in.gameState != -1 && in.gameState != 6) {
                o.ev = Event::GameMoved;
                o.evValue = static_cast<float>(in.gameState);
                phase_ = Phase::Empty;
                break;
            }
            if (!in.haveHand) { break; }   // no published hand: hold the phase - no geometric decision (final review 4)
            if (phase_ == Phase::Carry) {
                const bool inCapsule = along >= -p.seatM && along <= p.catchLenM && off <= p.catchM;
                if (!inCapsule) { wrongWay_ = false; break; }
                const float a = AngleDeg(in.tip, in.lineTip);
                if (a <= p.catchDeg) { o.ev = Event::Catch; o.evValue = off; phase_ = Phase::Caught; wrongWay_ = false; }
                else if (!wrongWay_) { o.ev = Event::CatchWrongWay; o.evValue = a; wrongWay_ = true; }
            } else {
                if (off > p.holdM) { o.ev = Event::DropToCarry; o.evValue = off; phase_ = Phase::Carry; wrongWay_ = false; }
                else if (along <= p.seatM) {
                    o.ev = Event::Seat; o.evValue = along; phase_ = Phase::Seated; o.endPutArrow = true; seatMs_ = in.tMs;
                }
            }
            break;
        case Phase::Off:
            break;
        }

        o.phase = phase_;
        o.fakeEmpty = phase_ == Phase::Empty;
        // The Stand pose is held (Jay 2026-10-17: "Hold the bones ourselves"; run 2: the bow "rotates slightly to the left" on
        // the take and back on the notch - PutArrow's bone_root is 8.55 deg off Stand):
        //   - Empty in the game's Empty state (not while state 4's Shoot clip still plays, not in an ammo-type change's PutArrow);
        //   - the whole carry and catch (the game is in -1 or 6 - anything else already ended them above);
        //   - Seated for the game's PutArrow -> Stand blend (kSeatBlendMs) while still in 6 / 0, with the arrow at rest on the
        //     held bow; a draw (state 1) inside the window ends it - the draw owns the bow.
        const bool seatWindow = phase_ == Phase::Seated && (in.gameState == 0 || in.gameState == 6) && seatMs_ >= 0.0
                                && in.tMs - seatMs_ < kSeatBlendMs;
        o.holdRestPose = (phase_ == Phase::Empty && in.gameState == -1) || phase_ == Phase::Carry || phase_ == Phase::Caught
                         || seatWindow;
        o.holdArrowAtRest = seatWindow;
        o.holdFingers = phase_ == Phase::Carry || phase_ == Phase::Caught;   // Jay: "Use the game's reload pose"
        if (phase_ == Phase::Seated && !seatWindow) { seatMs_ = -1.0; }   // once over, it stays over (a later 2 -> 0 is the game's)
        if (phase_ == Phase::Carry || phase_ == Phase::Caught) {
            o.holdPutArrow = in.gameState == 6;
            o.onLine = phase_ == Phase::Caught;
            o.writeArrow = o.onLine || in.haveHand;   // on the line needs no hand; in the hand needs one
            if (o.onLine) { o.along = along < 0.0f ? 0.0f : (along > p.catchLenM ? p.catchLenM : along); }
        }
        return o;
    }

private:
    static constexpr double kSeatBlendMs = 1000.0;   // OUR m_fpp_bow.scr: SeqTracksBlend("FPP_Bow_PutArrow", "FPP_Bow_Stand", 1.0)
    Phase  phase_;
    bool   wrongWay_ = false;   // CatchWrongWay is said once per entry into the capsule
    double seatMs_ = -1.0;      // when the arrow seated (-1 = no post-seat window)
    bool   fromMemory_;         // started Seated from the memory - the first update may still be the game's PutArrow
};

// What the memory is keyed on: the bow CONTROLLER (one per player; it survives a weapon swap), not the item object - the game
// makes a NEW item object on every equip (bownock_run2: ...E776E640 -> ...C7ED19C0, controller ...F766AFC0 both times). Falls back
// to the item when no controller is known. Consequence (told to Jay): two bows share one remembered arrow.
inline uintptr_t MemoryKey(uintptr_t item, uintptr_t ctrl) { return ctrl ? ctrl : item; }

// The bow remembers its arrow (Jay: "Unless nocked prior"): 8 slots, least-recently-used out. Only Seated is remembered;
// any other phase (a carry included) is stored as not-seated = back to the quiver.
class Memory {
public:
    bool Recall(uintptr_t item)
    {
        for (Slot& s : slots_) {
            if (s.item == item && item != 0) { s.stamp = ++clock_; return s.seated; }
        }
        return false;
    }
    void Store(uintptr_t item, Phase p)
    {
        if (item == 0) { return; }
        Slot* use = nullptr;
        for (Slot& s : slots_) { if (s.item == item) { use = &s; break; } }
        if (!use) {
            use = &slots_[0];
            for (Slot& s : slots_) {
                if (s.item == 0) { use = &s; break; }
                if (s.stamp < use->stamp) { use = &s; }
            }
        }
        use->item = item;
        use->seated = p == Phase::Seated;
        use->stamp = ++clock_;
    }

private:
    struct Slot { uintptr_t item = 0; bool seated = false; unsigned long long stamp = 0; };
    Slot slots_[8];
    unsigned long long clock_ = 0;
};

}  // namespace nock
}  // namespace dlvr
