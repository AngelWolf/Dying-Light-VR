// Pure logic for the calibration screen - docs/superpowers/specs/2026-10-14-calibration-screen-design.md section 2,
// build A plan task 2. No OpenXR, no Windows, no logging, so src/src/tests/calibscreen_logic_test.cpp compiles it alone.
// calibscreen.cpp owns everything with side effects (controllers, armfit, settings, the log).
#pragma once

namespace dlvr {
namespace calib {

constexpr int kPoses = 4;                         // forward, side, up, down (armfit.cpp kFwd..kDown)
constexpr int kTarget = 150;                      // accepted samples per arm per pose (armfit.cpp kFitPerPose)
constexpr float kGripOn = 0.5f, kGripOff = 0.3f;  // the same hysteresis as controllers.cpp's Index squeeze
constexpr float kStickFire = 0.7f, kStickRearm = 0.3f;   // snap_logic.h's flick lines
constexpr unsigned long long kSkipHoldMs = 1000;  // B held this long = skip (not the left menu button - VD owns it)
constexpr unsigned long long kPassShowMs = 3000;

enum class Screen { Closed, Posture, SeatedNotice, Pose, Computing, Passed, Failed };
enum Collect { kNoChange = -1, kDiscard = 0, kStart = 1, kKeep = 2 };

struct Input {
    unsigned long long nowMs = 0;
    float gripL = 0.0f, gripR = 0.0f;            // GetControllerSqueeze values
    bool gripValidL = false, gripValidR = false; // false = the action was inactive: NOT held, whatever the value
    bool bDown = false;
    float stickX = 0.0f;                         // either stick, the larger |x|
    int nR = 0, nL = 0;                          // accepted samples this pose (armfit)
    int vR = 0, vL = 0;                          // live verdict per arm: 0 none, 1 OK, 2 not extended, 3 wrong direction, 4 not tracked
    int finish = 0;                              // after a finish request: 1 written, -1 refused, 0 not yet
};

struct Cmd {
    bool startRun = false;    // armfit: start a step-mode run (resets every pose)
    int beginPose = -1;       // armfit: this pose is now the one being collected
    int collect = kNoChange;  // armfit: start / discard / keep the current pose's read
    bool finish = false;      // armfit: fit + write now
    bool cancel = false;      // armfit: cancel the run (nothing written)
    bool closed = false;      // the screen has just closed
    int savePosture = -1;     // settings: write calib_posture = this
};

class Machine {
public:
    // Read-only for the view / the glue.
    Screen screen = Screen::Closed;
    int posture = 0;          // 0 standing, 1 seated - the CHOICE on the posture screen
    int pose = 0;
    bool reading = false;
    int pausedArm = -1;       // while reading: 0 = right out of pose, 1 = left, -1 none
    float ring = 0.0f;        // 0..1, min(nR, nL) / kTarget
    float skipFrac = 0.0f;    // 0..1 of the B hold

    void Open(int savedPosture)
    {
        *this = Machine();
        screen = Screen::Posture;
        posture = savedPosture ? 1 : 0;
        needRelease_ = true;
    }

    Cmd Tick(const Input& in)
    {
        Cmd c;
        if (screen == Screen::Closed) { return c; }
        const bool held = UpdateGrips(in);
        if (needRelease_ && !anyHeld_) { needRelease_ = false; }
        const bool press = held && !needRelease_;

        // Skip: B held kSkipHoldMs on ANY screen.
        if (in.bDown) {
            if (!bArmed_) { bArmed_ = true; bSince_ = in.nowMs; }
            const unsigned long long d = in.nowMs - bSince_;
            skipFrac = d >= kSkipHoldMs ? 1.0f : static_cast<float>(d) / static_cast<float>(kSkipHoldMs);
            if (d >= kSkipHoldMs) {
                c.cancel = runStarted_ && screen != Screen::Passed;
                Close(c);
                return c;
            }
        } else {
            bArmed_ = false;
            skipFrac = 0.0f;
        }

        switch (screen) {
            case Screen::Posture: {
                const float ax = in.stickX < 0.0f ? -in.stickX : in.stickX;
                if (stickArmed_ && ax >= kStickFire) { posture = in.stickX > 0.0f ? 1 : 0; stickArmed_ = false; }
                if (!stickArmed_ && ax < kStickRearm) { stickArmed_ = true; }
                if (press) {
                    c.savePosture = posture;
                    needRelease_ = true;
                    BeginRun(c);   // Build C: seated runs its own set at once (SeatedNotice is no longer entered)
                }
                break;
            }
            case Screen::SeatedNotice:
                if (press) { needRelease_ = true; BeginRun(c); }
                break;
            case Screen::Pose: {
                const int lo = in.nR < in.nL ? in.nR : in.nL;
                ring = lo >= kTarget ? 1.0f : static_cast<float>(lo) / static_cast<float>(kTarget);
                if (!reading) {
                    pausedArm = -1;
                    if (press && in.vR == 1 && in.vL == 1) { reading = true; c.collect = kStart; }
                } else if (!held) {
                    reading = false;
                    pausedArm = -1;
                    ring = 0.0f;
                    c.collect = kDiscard;
                } else if (in.nR >= kTarget && in.nL >= kTarget) {
                    reading = false;
                    pausedArm = -1;
                    ring = 0.0f;
                    c.collect = kKeep;
                    needRelease_ = true;
                    ++pose;
                    if (pose < kPoses) { c.beginPose = pose; } else { c.finish = true; screen = Screen::Computing; }
                } else {
                    pausedArm = in.vR != 1 ? 0 : (in.vL != 1 ? 1 : -1);
                }
                break;
            }
            case Screen::Computing:
                if (in.finish == 1) { screen = Screen::Passed; passedAt_ = in.nowMs; runStarted_ = false; }
                else if (in.finish == -1) { screen = Screen::Failed; needRelease_ = true; runStarted_ = false; }
                break;
            case Screen::Passed:
                if (in.nowMs - passedAt_ >= kPassShowMs) { Close(c); }
                break;
            case Screen::Failed:
                if (press) { needRelease_ = true; BeginRun(c); }
                break;
            case Screen::Closed:
                break;
        }
        return c;
    }

private:
    bool held_[2] = { false, false };
    bool anyHeld_ = false;
    bool needRelease_ = false;
    bool stickArmed_ = true;
    bool bArmed_ = false;
    unsigned long long bSince_ = 0;
    unsigned long long passedAt_ = 0;
    bool runStarted_ = false;

    bool UpdateGrips(const Input& in)
    {
        const float v[2] = { in.gripL, in.gripR };
        const bool ok[2] = { in.gripValidL, in.gripValidR };
        for (int h = 0; h < 2; ++h) {
            if (!ok[h]) { held_[h] = false; continue; }
            held_[h] = held_[h] ? (v[h] >= kGripOff) : (v[h] >= kGripOn);
        }
        anyHeld_ = held_[0] || held_[1];
        return held_[0] && held_[1];
    }
    void BeginRun(Cmd& c)
    {
        c.startRun = true;
        c.beginPose = 0;
        pose = 0;
        reading = false;
        pausedArm = -1;
        ring = 0.0f;
        runStarted_ = true;
        screen = Screen::Pose;
    }
    void Close(Cmd& c)
    {
        c.closed = true;
        screen = Screen::Closed;
        reading = false;
        runStarted_ = false;
    }
};

}  // namespace calib
}  // namespace dlvr
