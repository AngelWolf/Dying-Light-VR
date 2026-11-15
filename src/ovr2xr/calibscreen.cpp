// The calibration screen's glue - build A (docs/superpowers/plans/2026-10-14-calibration-screen-build-a.md task 5) and
// build B (docs/superpowers/plans/2026-10-14-calibration-screen-build-b.md task 5: the main-menu RAW path, the auto-open,
// the void and the floor). The decisions are calibscreen_logic.h; the measurement is armfit.cpp's step mode (in level: the
// solve + the raw store side by side; main menu: the raw store only - calibraw_logic.h); the drawing is calibdraw.cpp.
// COST: closed and the auto-open decided = one bool test + one volatile read per submit. Before the auto-open is decided:
// one MenuState() level walk per submit (a few dozen pointer reads). Open: controller getters, one xrLocateSpace, ~100 flops
// per hand, one shared-lock progress read, one ImGui frame + one quad (two with the floor) per submit. At the main menu (the
// void): + one 2048 x 2048 copy each for the walls and the ceiling, + 5 quads; their two decoders run only while drawn.
#include "calibscreen.h"

#include <windows.h>

#include <cmath>
#include <cstdio>

#include "../common/log.h"
#include "armfit.h"
#include "boneblend.h"
#include "calibassets.h"
#include "calibdraw.h"
#include "calibraw_logic.h"
#include "calibscreen_logic.h"
#include "calibsound.h"
#include "controllers.h"
#include "headpose.h"
#include "holster_logic.h"   // holster::LocalOffset - the down pose's resting hand in the holsters' frame
#include "overlay.h"
#include "settings.h"
#include "uipin.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

namespace dlvr {

// [DEP: calib_posture] required-for=the calibration screen's standing / seated answer (CalibPosture), asked FIRST (Jay
//      2026-10-14: "The first thing prior to the calibration will be asking if the player intends on playing standing or
//      seated, so we can adjust what we need accordingly"). Written by the screen, a menu control to change it. Build C
//      (2026-11-15): the screen's confirmed choice picks the pose set (seated: the 4th pose is down and out at 45 deg, the hips
//      keep the previous calibration's) - read only by the calibration screen. tree=INERT. User data - Tier D, default 0 = Standing.
int CalibPosture = 0;
// [DEP: calib_screen_auto] required-for=the calibration screen opening BY ITSELF at the first main-menu frame of a launch
//      when there is no valid dlvr\arm_fit.txt (CalibScreenAuto; build B ruling B5). Once per launch. tree=INERT (it only
//      opens a screen the player can skip with B). HEADSET. SHIPS ON - Tier A (Jay 2026-10-14: "Ship it on"; build B run 1).
bool CalibScreenAuto = true;

namespace {
volatile LONG g_open = 0;
calib::Machine g_m;
CalibView g_view;
int g_finish = 0;
bool g_autoDone = false;
bool g_void = false;                // this submit: the screen is up AT THE MAIN MENU (no game image, the floor drawn)
XrSpace g_floorSpace = XR_NULL_HANDLE;
bool g_floorTried = false;
bool g_floorIsApp = false;          // g_floorSpace is appSpace (the fallback) - refreshed each calibration frame, D-253
float g_floorY = 0.0f;              // the floor quad's height in g_floorSpace
const char* g_floorSource = "none";
const float kVoidSizeM = 6.0f;      // the floor's side; the walls are as wide and as tall (Jay 2026-10-16)
bool g_boxLogged = false;           // the first VOID BOX line is unconditional
bool g_boxWallsSaid = false;        // what the last VOID BOX line said was drawn
bool g_boxRoofSaid = false;
const char* const kScreenName[] = { "closed", "posture", "seated notice", "pose", "computing", "passed", "failed" };
// The panel's world anchor (Jay 2026-10-15: option 1, no switch): a pose in appSpace latched from the head at the first located
// frame of each open - 1.6 m along tracking -z, 0.15 m below the eyes, facing +z (ruling B3: where the player must face).
// Re-latched when appSpace changes (a recenter destroys and replaces it, ovr2xr.cpp:8469-8474).
bool g_anchorSet = false;
XrSpace g_anchorSpace = XR_NULL_HANDLE;
XrPosef g_anchor{};
bool g_anchorMissSaid = false;

// The head's yaw off tracking -z, degrees (ruling B3: the calibration's frame assumes the player faces -z).
float YawOffForward(const XrQuaternionf& q)
{
    const float fx = -2.0f * (q.x * q.z + q.y * q.w);
    const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    return atan2f(fx, -fz) * 57.2957795f;
}

void EnsureFloorSpace(XrSession session, XrSpace appSpace)
{
    if (g_floorTried || session == XR_NULL_HANDLE || !xrCreateReferenceSpace_) { return; }
    g_floorTried = true;
    XrReferenceSpaceCreateInfo ci{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    ci.poseInReferenceSpace.orientation.w = 1.0f;
    ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    XrResult rs = xrCreateReferenceSpace_(session, &ci, &g_floorSpace);
    XrResult rl = XR_SUCCESS;
    if (XR_SUCCEEDED(rs)) {
        g_floorSource = "STAGE";
    } else {
        g_floorSpace = XR_NULL_HANDLE;
        ci.referenceSpaceType = static_cast<XrReferenceSpaceType>(1000426000);   // XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR(_EXT)
        rl = xrCreateReferenceSpace_(session, &ci, &g_floorSpace);
        if (XR_SUCCEEDED(rl)) {
            g_floorSource = "LOCAL_FLOOR";
        } else {
            g_floorSpace = appSpace;                                            // the app space at an estimated floor
            g_floorIsApp = true;   // appSpace is recreated by a recentre (absorb or the game's) - CalibScreenFrameBegin refreshes it
            g_floorY = -1.6f;
            g_floorSource = "app space at -1.6 m (estimated - no STAGE / LOCAL_FLOOR)";
        }
    }
    // [PROBE: CALIBSCR] (build B - which floor the void stands on, once)
    LOG("calibscreen: [CALIBSCR] FLOOR: %s (STAGE result %d, LOCAL_FLOOR result %d; 0 = success, not tried if STAGE worked)",
        g_floorSource, static_cast<int>(rs), static_cast<int>(rl));
}
}  // namespace

bool CalibScreenActive() { return g_open != 0; }
bool CalibScreenVoid() { return g_void; }

void CalibScreenOpen()
{
    const int ms = uipin::MenuState();
    const bool mainMenu = ms == 1;
    // Build D: start the one-shot art + font loader thread (idempotent - the second open of a launch is a no-op). COST: one
    // interlocked test after the first call; the file reads / decodes run on the loader thread, never here.
    CalibArtStart();
    ArmFitSetStepSource(mainMenu ? 1 : 0);
    g_m.Open(CalibPosture);
    g_finish = 0;
    g_view.resultLine[0] = '\0';
    g_view.mainMenu = mainMenu;
    g_anchorSet = false;
    g_anchorMissSaid = false;
    InterlockedExchange(&g_open, 1);
    // [PROBE: CALIBSCR] gate=none (the calibration screen)  site=HEADSET  log-only=yes
    LOG("calibscreen: [CALIBSCR] OPENED (logged unconditionally) - saved posture %s; menu state %d -> %s; the game's input is "
        "held off while it is up", CalibPosture ? "seated" : "standing", ms,
        mainMenu ? "MAIN MENU: RAW poses only, the void + floor" : "IN LEVEL (or not the main menu): the solve + raw side by side");
}

void CalibScreenFrameBegin(XrSession session, XrSpace appSpace)
{
    if (!g_autoDone && CalibScreenAuto) {
        if (uipin::MenuState() == 1) {
            g_autoDone = true;
            if (!ArmFitHaveCalibration()) {
                LOG("calibscreen: [CALIBSCR] AUTO: no valid calibration at the first main-menu frame - opened");
                CalibScreenOpen();
            } else {
                LOG("calibscreen: [CALIBSCR] AUTO: a valid calibration is loaded - not opened (calib_screen_auto=1)");
            }
        }
    }
    const bool wantVoid = g_open != 0 && uipin::MenuState() == 1;
    // The fallback floor IS appSpace, whose handle a recentre replaces: take the current one. Inside the screen-up test only, so
    // outside the calibration screen this is the same single branch as before (Jay 2026-10-20: "ONLY for when it's in the
    // calibration screen").
    if (wantVoid) {
        if (!g_floorTried) { EnsureFloorSpace(session, appSpace); }
        if (g_floorIsApp) { g_floorSpace = appSpace; }
    }
    if (wantVoid != g_void) {
        g_void = wantVoid;
        LOG("calibscreen: [CALIBSCR] VOID %s - %s", g_void ? "ON" : "OFF",
            g_void ? "the game image is not submitted while the screen is up at the main menu (VDXR accepts quad-only frames)"
                   : "the game image is back");
    }
    // Build D: create the art / font textures once the loader thread has finished. Here (every submit, BEFORE both
    // CalibScreenRenderFloor and CalibScreenRender in ovr2xr.cpp) so the floor and the panel both see the textures the frame
    // they exist. COST: open only; until the loader finishes one bool test + one interlocked read, afterwards one flag test.
    if (g_open) { CalibArtPump(); }
}

bool CalibScreenRenderFloor(XrCompositionLayerQuad* out)
{
    if (!g_void || g_floorSpace == XR_NULL_HANDLE) { return false; }
    XrPosef pose{};
    pose.orientation = { -0.70710678f, 0.0f, 0.0f, 0.70710678f };   // -90 deg about X: the quad lies flat, facing up
    pose.position = { 0.0f, g_floorY, 0.0f };
    // Build D: Jay's floor texture when it exists and the runtime accepted it; otherwise today's 800 px ImGui grid.
    if (ID3D11Texture2D* t = CalibArtFloorTexture()) {
        if (!OverlayCalibFloorImageRefused()) { return OverlayRenderCalibFloorImage(g_floorSpace, pose, kVoidSizeM, t, out); }
    }
    return OverlayRenderCalibFloor(g_floorSpace, pose, kVoidSizeM, &CalibDrawFloor, nullptr, out);
}

int CalibScreenRenderVoidBox(XrCompositionLayerQuad* out, int max)
{
    if (!g_void || g_floorSpace == XR_NULL_HANDLE || !out || max <= 0) { return 0; }
    // Jay 2026-10-16: a box on the floor's edges - walls kVoidSizeM wide and (the 2048 x 2048 video at the floor's scale) as
    // tall, facing in, the video's top at the wall's top; a flat ceiling on the walls' top edge, facing down. A quad faces its
    // local +z with the image's top along +y; a turn of t about Y sends +z to (sin t, 0, cos t), about X to (0, -sin t, cos t).
    const float h = kVoidSizeM * 0.5f;
    const float s = 0.70710678f;
    XrPosef walls[4] = {};
    walls[0].orientation = { 0.0f, 0.0f, 0.0f, 1.0f };   walls[0].position = { 0.0f, g_floorY + h, -h };   // -z wall, faces +z
    walls[1].orientation = { 0.0f, 1.0f, 0.0f, 0.0f };   walls[1].position = { 0.0f, g_floorY + h, h };    // +z wall, faces -z
    walls[2].orientation = { 0.0f, s, 0.0f, s };         walls[2].position = { -h, g_floorY + h, 0.0f };   // -x wall, faces +x
    walls[3].orientation = { 0.0f, -s, 0.0f, s };        walls[3].position = { h, g_floorY + h, 0.0f };    // +x wall, faces -x
    XrPosef roof{};
    roof.orientation = { s, 0.0f, 0.0f, s };             roof.position = { 0.0f, g_floorY + kVoidSizeM, 0.0f };   // faces -y
    int n = 0;
    bool wallsDrawn = false, roofDrawn = false;
    if (max >= 4) {
        if (ID3D11Texture2D* t = CalibArtVoidTexture(false)) {
            if (OverlayRenderCalibVoidImage(false, g_floorSpace, walls, 4, kVoidSizeM, t, out)) { n = 4; wallsDrawn = true; }
        }
    }
    if (n < max) {
        if (ID3D11Texture2D* t = CalibArtVoidTexture(true)) {
            if (OverlayRenderCalibVoidImage(true, g_floorSpace, &roof, 1, kVoidSizeM, t, out + n)) { ++n; roofDrawn = true; }
        }
    }
    if (!g_boxLogged || wallsDrawn != g_boxWallsSaid || roofDrawn != g_boxRoofSaid) {
        g_boxLogged = true;
        g_boxWallsSaid = wallsDrawn;
        g_boxRoofSaid = roofDrawn;
        // [PROBE: CALIBSCR] (the void box - on every change of what is drawn, the first included)
        LOG("calibscreen: [CALIBSCR] VOID BOX: walls %s, ceiling %s | %.1f m square, floor at y %+.3f in %s, ceiling at y %+.3f "
            "| %d layer slot(s) offered", wallsDrawn ? "DRAWN (4 quads)" : "not drawn", roofDrawn ? "DRAWN" : "not drawn",
            kVoidSizeM, g_floorY, g_floorSource, g_floorY + kVoidSizeM, max);
    }
    return n;
}

bool CalibScreenRender(XrSpace viewSpace, XrSpace appSpace, XrTime time, XrCompositionLayerQuad* out)
{
    if (!g_open) { return false; }
    calib::Input in;
    in.nowMs = GetTickCount64();
    in.gripValidL = GetControllerSqueeze(kLeftHand, in.gripL);
    in.gripValidR = GetControllerSqueeze(kRightHand, in.gripR);
    bool b = false;
    if (GetControllerButton(kButtonB, b)) { in.bDown = b; }
    float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
    GetControllerStick(kLeftHand, lx, ly);
    GetControllerStick(kRightHand, rx, ry);
    in.stickX = (lx * lx > rx * rx) ? lx : rx;

    // Build B: the RAW sample for this submit (both paths - the main menu uses it alone, a level compares it).
    XrQuaternionf headQ{ 0.0f, 0.0f, 0.0f, 1.0f };
    bool headOk = false;
    if (g_m.screen == calib::Screen::Pose && viewSpace != XR_NULL_HANDLE && appSpace != XR_NULL_HANDLE && xrLocateSpace_) {
        XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
        const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (XR_SUCCEEDED(xrLocateSpace_(viewSpace, appSpace, time, &loc)) && (loc.locationFlags & need) == need) {
            headOk = true;
            headQ = loc.pose.orientation;
            const XrVector3f hr = NeckPivotReportedPos(loc.pose);
            const float headRep[3] = { hr.x, hr.y, hr.z };
            for (int side = 0; side < 2; ++side) {
                XrPosef gp{};
                float grip[9];
                const Hand h = side ? kLeftHand : kRightHand;
                // ControllerGripPoseValid = located THIS frame; GetControllerGripPose alone only fails if never located
                // (controllers.h) - a stale pose must read "not tracked", never be sampled (Review Focus 1).
                if (!ControllerGripPoseValid(h) || !GetControllerGripPose(h, gp) || !CalibGripEmptyHand(side, grip)) {
                    ArmFitNoteRawUntracked(side);
                    continue;
                }
                const float ctl[3] = { gp.position.x, gp.position.y, gp.position.z };
                const float q[4] = { gp.orientation.x, gp.orientation.y, gp.orientation.z, gp.orientation.w };
                float T[3], B[9];
                calibraw::RawT(ctl, headRep, q, grip, side, T);
                calibraw::RawHandBasis(q, grip, side, B);
                // holster_hip_from_calib: the same grip point as the holsters measure it - from the RAW head (loc.pose, not
                // headRep: holster.cpp uses GetHeadPose's raw position), head yaw removed. COST: one atan2, a sin, a cos.
                const holster::Vec3 hl = holster::LocalOffset(
                    { loc.pose.position.x, loc.pose.position.y, loc.pose.position.z }, loc.pose.orientation.x,
                    loc.pose.orientation.y, loc.pose.orientation.z, loc.pose.orientation.w,
                    { gp.position.x, gp.position.y, gp.position.z });
                const float hl3[3] = { hl.x, hl.y, hl.z };
                ArmFitNoteRaw(side, T, B, hl3);
            }
        }
    }
    ArmFitPoseProgress(&in.nR, &in.nL, &in.vR, &in.vL);
    in.finish = g_finish;

    const calib::Screen before = g_m.screen;
    const int postureBefore = g_m.posture;   // the CHOICE on the posture screen, read before Tick
    const calib::Cmd c = g_m.Tick(in);
    // Build D sounds (D10): Click on a posture choice change, on the posture confirm, on a read start; PoseDone on a read kept.
    // Nothing on discard / pass / fail / arm OK / seated notice -> pose. At most ONE Click per Tick. COST: event-driven only.
    {
        const bool postureChanged = before == calib::Screen::Posture && g_m.posture != postureBefore;
        const bool confirm = before == calib::Screen::Posture &&
                             (g_m.screen == calib::Screen::Pose || g_m.screen == calib::Screen::SeatedNotice);
        if (postureChanged || confirm || c.collect == calib::kStart) { CalibSoundPlay(CalibSnd::Click); }
        else if (c.collect == calib::kKeep) { CalibSoundPlay(CalibSnd::PoseDone); }
    }
    if (c.savePosture >= 0 && c.savePosture != CalibPosture) {
        CalibPosture = c.savePosture;
        SaveSettingsIfChanged();
    }
    if (c.startRun) { ArmFitStartStep(g_m.posture == 1); g_finish = 0; }   // Build C: the confirmed choice picks the pose set
    // The read's verdict (keep / discard) BEFORE the next pose is set - otherwise the DONE line names the NEW pose with
    // 0 samples (run 2, 2026-10-14: three of four DONE lines were the next pose, "right 0, left 0").
    if (c.collect != calib::kNoChange) { ArmFitCollect(c.collect); }
    if (c.collect == calib::kStart) {
        // [PROBE: CALIBSCR] (build B ruling B3 - how far the head faces off tracking forward at each read start)
        LOG("calibscreen: [CALIBSCR] read start, pose %d: head yaw off tracking forward %+.1f deg%s", g_m.pose,
            headOk ? YawOffForward(headQ) : 0.0f, headOk ? "" : " (head pose NOT located this frame)");
    }
    if (c.beginPose >= 0) { ArmFitBeginPose(c.beginPose); }
    if (c.finish) { g_finish = ArmFitStepFinish(g_view.resultLine, sizeof(g_view.resultLine)); }
    if (c.cancel) { ArmFitCancel(); }
    if (g_m.screen != before) {
        // [PROBE: CALIBSCR] (every screen change, with what the arms read at that moment)
        LOG("calibscreen: [CALIBSCR] %s -> %s | pose %d | right %d (verdict %d) left %d (verdict %d) | posture choice %s",
            kScreenName[static_cast<int>(before)], kScreenName[static_cast<int>(g_m.screen)], g_m.pose, in.nR, in.vR, in.nL,
            in.vL, g_m.posture ? "seated" : "standing");
    }
    if (c.closed) {
        InterlockedExchange(&g_open, 0);
        LOG("calibscreen: [CALIBSCR] CLOSED - %s", c.cancel ? "skipped (B held) during a run - nothing written"
                                                           : (g_finish == 1 ? "calibration written" : "skipped / no run"));
        return false;
    }
    g_view.screen = g_m.screen;
    g_view.posture = g_m.posture;
    g_view.pose = g_m.pose;
    g_view.reading = g_m.reading;
    g_view.pausedArm = g_m.pausedArm;
    g_view.ring = g_m.ring;
    g_view.skipFrac = g_m.skipFrac;
    g_view.vR = in.vR; g_view.vL = in.vL; g_view.nR = in.nR; g_view.nL = in.nL;
    // A2 sizes: 1.6 m ahead, 0.15 m below the eyes, 1.30 m wide (Tier D tuning - build A). WORLD-LOCKED in appSpace at tracking
    // forward (above); head-locked only until the head is first located. COST: one xrLocateSpace per submit until the anchor
    // is latched (once per open / recenter), then nothing.
    if ((!g_anchorSet || g_anchorSpace != appSpace) && viewSpace != XR_NULL_HANDLE && appSpace != XR_NULL_HANDLE &&
        xrLocateSpace_) {
        XrSpaceLocation al{ XR_TYPE_SPACE_LOCATION };
        const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (XR_SUCCEEDED(xrLocateSpace_(viewSpace, appSpace, time, &al)) && (al.locationFlags & need) == need) {
            const bool relatch = g_anchorSet;
            g_anchor = XrPosef{};
            g_anchor.orientation.w = 1.0f;
            g_anchor.position = { al.pose.position.x, al.pose.position.y - 0.15f, al.pose.position.z - 1.6f };
            g_anchorSpace = appSpace;
            g_anchorSet = true;
            // [PROBE: CALIBSCR] (the panel's world anchor, at every open and every recenter)
            LOG("calibscreen: [CALIBSCR] PANEL ANCHOR%s (logged unconditionally): world-locked in the app space at (%+.3f, %+.3f, "
                "%+.3f) m, facing +z | head at (%+.3f, %+.3f, %+.3f), head yaw off tracking forward %+.1f deg",
                relatch ? " RE-LATCHED - the app space changed (recenter)" : "", g_anchor.position.x, g_anchor.position.y,
                g_anchor.position.z, al.pose.position.x, al.pose.position.y, al.pose.position.z, YawOffForward(al.pose.orientation));
        } else if (!g_anchorMissSaid) {
            g_anchorMissSaid = true;
            LOG("calibscreen: [CALIBSCR] PANEL ANCHOR: head NOT located this frame - the panel is HEAD-LOCKED until it is "
                "(retried every submit)");
        }
    }
    if (g_anchorSet && g_anchorSpace == appSpace) {
        return OverlayRenderCalibQuad(appSpace, g_anchor, 1.30f, &CalibDraw, &g_view, out);
    }
    XrPosef pose{};
    pose.orientation.w = 1.0f;
    pose.position = { 0.0f, -0.15f, -1.6f };
    return OverlayRenderCalibQuad(viewSpace, pose, 1.30f, &CalibDraw, &g_view, out);
}

}  // namespace dlvr
