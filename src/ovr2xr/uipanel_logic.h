// MENU PANEL - the placement maths, with no engine, OpenXR or Windows in it (docs/SPEC-UI-MENU-PANEL-2026-10-11.md §4.3 /
// §4.4). Tested offline by src/src/tests/uipanel_logic_test.cpp; used by uipanel.cpp.
//
// Room space is OpenXR's: x right, y UP, z toward the viewer (a head looking straight ahead looks along -z). Quaternions
// are (x, y, z, w). A quad's face looks along the +z of its pose, so a panel "in front of the head, facing it" has its
// own -z along the head's flattened forward direction.
#pragma once

#include <cmath>

namespace dlvr {
namespace uipanel {

struct Rect { int x, y, w, h; };

// The GAME's rendered eye frustum, as positive tangents (left, right, up, down) - the FOV the game echoes in ovrLayerEyeFov for
// the image it drew (ovr2xr.cpp g_lmEye[].game, dlvr::GameEyeTan / dlvr_GameEyeTan). NEVER a constant: every headset differs (INV-735).
struct EyeTan { float l, r, u, d; };

// The game's UI camera (INV-529, INV-735): a fixed default - 45 deg vertical (tan 22.5 deg) and the horizontal from the video aspect,
// which is two eye images wide by one high (HmdDesc's resolution). A tester's HUD matrices (1728 x 1824 per eye) give 0.4142 / 0.7848.
constexpr double kUiTanY = 0.41421356237309515;
inline double UiTanX(int W, int H) { return kUiTanY * (2.0 * W / H); }

// The whole eye image (the main menu's screens are placed in the 3D scene - SPEC U3; the stereo panels).
inline Rect Crop(int W, int H) { return Rect{ 0, 0, W, H }; }

// The part of an eye's UI pass that holds the UI plane (the HUD / in-game canvas): centred on the projection axis (tangent 0),
// UiTanX x kUiTanY of the frustum. False (out untouched) for an image under 64 px or a frustum not known yet - callers refuse,
// nothing is guessed. Quest 3 (2688 x 2880, +-1.3764 x +-1.4747) -> (589, 1036) 1510 x 808, the INV-529 box.
inline bool UiCrop(int W, int H, const EyeTan& t, Rect* out)
{
    if (!out || W < 64 || H < 64 || !(t.l > 0.0f) || !(t.r > 0.0f) || !(t.u > 0.0f) || !(t.d > 0.0f)) { return false; }
    const double ux = UiTanX(W, H), uy = kUiTanY;
    int w = static_cast<int>(W * (2.0 * ux) / (t.l + t.r) + 0.5), h = static_cast<int>(H * (2.0 * uy) / (t.u + t.d) + 0.5);
    w &= ~1; h &= ~1;
    if (w > W) { w = W & ~1; }
    if (h > H) { h = H & ~1; }
    const double cx = W * static_cast<double>(t.l) / (t.l + t.r), cy = H * static_cast<double>(t.u) / (t.u + t.d);
    int x = static_cast<int>(cx - w * 0.5 + 0.5), y = static_cast<int>(cy - h * 0.5 + 0.5);
    if (x < 0) { x = 0; }
    if (y < 0) { y = 0; }
    if (x + w > W) { x = W - w; }
    if (y + h > H) { y = H - h; }
    *out = Rect{ x, y, w, h };
    return true;
}

// Selection wheels (plan 2026-10-27-vr-selection-wheels Task 6, D13): the game's item wheel WITH its name, stats, cash and repair
// parts, in the eye image. Our hud.xui (DW_DLC24/DataDLC24_2.pak, which shadows Data0's) puts each wheel group at (471, 191)
// 338 x 338 on the 1280 x 720 canvas, but its children reach x 380..998, y 110..749 (slots, I_Selection frames), G_Cash
// 427..542 / G_RepairParts 738..882 at y 482..506, and G_ItemName (G_Size 256 wide) is moved beside the chosen slot by the game's
// Update - so a long name reaches ~150 / ~1130 (run Q1, Jay: "some parts are cut off, mainly the weapon names & cash / metal
// parts"). The box: (140, 100)-(1140, 620), centred on the canvas centre. The canvas spans UiCrop (INV-529, INV-735).
// False when UiCrop refuses (image too small / frustum not known yet).
inline bool WheelRect(int W, int H, const EyeTan& t, Rect* out)
{
    Rect c{};
    if (!out || !UiCrop(W, H, t, &c)) { return false; }
    int w = static_cast<int>(c.w * 1000.0 / 1280.0 + 0.5), h = static_cast<int>(c.h * 520.0 / 720.0 + 0.5);
    w &= ~1; h &= ~1;
    const int cx = c.x + static_cast<int>(c.w * 640.0 / 1280.0 + 0.5);
    const int cy = c.y + static_cast<int>(c.h * 360.0 / 720.0 + 0.5);
    *out = Rect{ cx - w / 2, cy - h / 2, w, h };
    return true;
}

// ---- the UI headset sweep (Jay HARD RULE 2026-11-13; trace 20261113_2125): numbers that follow the running headset ----
// The UI-screen height the wrist HUD's numbers were tuned on (Jay's Quest 3). Used ONLY as the reference of a ratio, so each value
// below keeps the same SHARE of the HUD on any headset - never as a size on its own.
constexpr double kTunedUiH = 2880.0;
// Metres per UI pixel at ui_wrist_size 1.0: the minimap's 780 of 2880 UI px at ~9 cm, the same share on any UI screen. 0 = unknown.
inline double MetresPerUiPx(double uiH) { return uiH > 0.0 ? 0.000115 * kTunedUiH / uiH : 0.0; }
// A UI-pixel tolerance tuned on the 2880-high UI screen, scaled to this UI screen. 0 = height unknown.
inline float UiPxTol(float tunedPx, float uiH) { return uiH > 0.0f ? tunedPx * uiH / static_cast<float>(kTunedUiH) : 0.0f; }
// A bw x bh box into a sw x sh slot by the smallest integer step s >= 1 (nearest texel): out = ceil(bw / s) x ceil(bh / s), never
// cut. False for an empty box / slot.
inline bool FitStep(int bw, int bh, int sw, int sh, int* w, int* h, int* step)
{
    if (!w || !h || !step || bw < 1 || bh < 1 || sw < 1 || sh < 1) { return false; }
    const int sx = (bw + sw - 1) / sw, sy = (bh + sh - 1) / sh;
    const int s = sx > sy ? sx : sy;
    *step = s;
    *w = (bw + s - 1) / s;
    *h = (bh + s - 1) / s;
    return true;
}
// The UI canvas centre in the eye image (px): UiCrop's centre = the projection axis (the image centre only for a symmetric
// frustum). False when UiCrop refuses.
inline bool CanvasCentre(int W, int H, const EyeTan& t, float* cx, float* cy)
{
    Rect c{};
    if (!cx || !cy || !UiCrop(W, H, t, &c)) { return false; }
    *cx = static_cast<float>(c.x) + 0.5f * static_cast<float>(c.w);
    *cy = static_cast<float>(c.y) + 0.5f * static_cast<float>(c.h);
    return true;
}
// After a wheel closes the game's own HideHud timeline fades it over 10 frames (hud.xui named frames HideHud 11 -> EndHideHud 21);
// the wheel-only view is kept this long past the close so the fade plays on the wheel's placement, not on the HUD (run Q1, Jay:
// "shows the wheel in the base HUD for a split moment upon the regular HUD returning"). The XUI frame rate is not read: 400 ms
// covers 10 frames at 25 fps or faster.
constexpr unsigned long long kWheelFadeHoldMs = 400;

// The panel's size in metres at `distance`, so that it covers the same angle the rectangle covers in the eye image.
// tanL / tanR / tanD / tanU are the tangents of the eye image's declared field of view (left and down negative).
inline void SizeM(const Rect& r, int W, int H, float tanL, float tanR, float tanD, float tanU, float distance, float scale,
                  float* width, float* height)
{
    *width = distance * (tanR - tanL) * (static_cast<float>(r.w) / static_cast<float>(W)) * scale;
    *height = distance * (tanU - tanD) * (static_cast<float>(r.h) / static_cast<float>(H)) * scale;
}

// The head's forward direction flattened onto the floor plane. False when the head looks straight up or down (no heading
// to take): the caller keeps the previous latch or waits a frame.
inline bool FlatForward(const float q[4], float out[3])
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    // the head's -z axis rotated by q
    const float fx = -(2.0f * (x * z + w * y));
    const float fz = -(1.0f - 2.0f * (x * x + y * y));
    const float len = std::sqrt(fx * fx + fz * fz);
    if (!(len > 0.05f)) { return false; }
    out[0] = fx / len; out[1] = 0.0f; out[2] = fz / len;
    return true;
}

// Where the panel goes when a menu opens: `distance` along the head's flattened forward, `heightOffset` above (negative =
// below) the head, upright, facing back at the head.
inline bool Latch(const float headPos[3], const float headQuat[4], float distance, float heightOffset, float outPos[3],
                  float outQuat[4])
{
    float f[3];
    if (!FlatForward(headQuat, f)) { return false; }
    outPos[0] = headPos[0] + f[0] * distance;
    outPos[1] = headPos[1] + heightOffset;
    outPos[2] = headPos[2] + f[2] * distance;
    // a turn about +y by `yaw` takes -z to (-sin yaw, 0, -cos yaw)
    const float yaw = std::atan2(-f[0], -f[2]);
    outQuat[0] = 0.0f; outQuat[1] = std::sin(yaw * 0.5f); outQuat[2] = 0.0f; outQuat[3] = std::cos(yaw * 0.5f);
    return true;
}

// HUD PANEL (docs/SPEC-UI-HUD-PANEL-2026-10-11.md §4.4): Latch, then moved `side` metres to the HEAD's right along the
// floor (negative = left). The panel keeps facing the way Latch turned it - it is slid, not turned.
inline bool LatchSide(const float headPos[3], const float headQuat[4], float distance, float heightOffset, float side,
                      float outPos[3], float outQuat[4])
{
    float f[3];
    if (!FlatForward(headQuat, f) || !Latch(headPos, headQuat, distance, heightOffset, outPos, outQuat)) { return false; }
    // right = forward x up, with up = +y
    outPos[0] += -f[2] * side;
    outPos[2] += f[0] * side;
    return true;
}

// HUD PANEL: whether the panel must be latched again. `lostMs` = how long the HUD had been gone (a load; 0 = it was not),
// `menu` = it was gone only because a menu was open (never a reason), `placed` / `now` = distance, height, side as they
// were at the latch and as they are now (a moved slider re-latches at once, so the slider is seen working).
inline bool HudRelatch(bool latched, bool recentre, unsigned long long lostMs, bool menu, const float placed[3],
                       const float now[3])
{
    if (!latched || recentre) { return true; }
    if (!menu && lostMs > 1000ull) { return true; }
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(placed[i] - now[i]) > 0.0005f) { return true; }
    }
    return false;
}

// HUD PANEL LAZY FOLLOW (ui_hud_follow; docs/superpowers/specs/2026-10-20-hud-panel-lazy-follow-design.md §2). The panel stays
// where it was latched until the head's heading is more than `deg` from the panel's facing, or the head has moved more than
// `driftM` along the floor from where it was at the latch, for `delayMs`; then it glides over `glideMs` to where LatchSide
// would put it NOW, chasing a head that keeps turning, and that pose becomes the latch. Pitch never moves it.
// `pending` (a physical crouch / stand edge, set by uipanel.cpp from PhysCrouchEdges - Jay 2026-10-27, "It was on the physical
// crouch specifically", option A) counts as away until the next glide begins: the head's height alone is never drift.
struct FollowCfg
{
    float              deg;                       // ui_hud_follow_deg
    unsigned long long delayMs;                   // ui_hud_follow_delay_ms
    unsigned long long glideMs;                   // ui_hud_follow_ms
    float              driftM;                    // ui_hud_follow_drift_m
    float              distance, height, side;    // ui_hud_distance_m / ui_hud_height_m / ui_hud_x_m
};

struct FollowState
{
    int                phase;                     // 0 idle, 1 armed, 2 gliding
    unsigned long long armedAt, glideAt;
    float              startYaw;                  // the panel's facing when the glide began, degrees
    float              latchHead[3];              // the head at the latch (or the last follow)
    long               follows, cancels;          // for the log
    float              maxOffDeg;                 // largest |off| seen, for the log
    bool               pending;                   // a physical crouch / stand edge not yet followed
};

enum FollowResult { kFollowIdle, kFollowArmed, kFollowGliding, kFollowSettled, kFollowNoHeading };

inline float WrapDeg(float d)
{
    while (d > 180.0f) { d -= 360.0f; }
    while (d < -180.0f) { d += 360.0f; }
    return d;
}

// At every real latch (the first one, a load, the re-centre button, a runtime recentre, a slider): start clean from here.
inline void HudFollowReset(FollowState& s, const float head[3])
{
    s.phase = 0;
    s.armedAt = s.glideAt = 0;
    s.startYaw = 0.0f;
    s.pending = false;
    for (int i = 0; i < 3; ++i) { s.latchHead[i] = head[i]; }
}

// One submit. panelPos / panelQuat are the latch: read always, written only while gliding and at the settle.
inline FollowResult HudFollowStep(FollowState& s, const float head[3], const float headQuat[4], unsigned long long nowMs,
                                  const FollowCfg& c, float panelPos[3], float panelQuat[4])
{
    float f[3];
    if (!FlatForward(headQuat, f)) { return kFollowNoHeading; }   // looking straight up / down: hold everything
    const float headYaw = std::atan2(-f[0], -f[2]) * 57.29578f;    // the same heading Latch turns the panel to
    const float panelYaw = 2.0f * std::atan2(panelQuat[1], panelQuat[3]) * 57.29578f;
    const float off = WrapDeg(headYaw - panelYaw);
    if (std::fabs(off) > s.maxOffDeg) { s.maxOffDeg = std::fabs(off); }

    if (s.phase != 2) {
        const float dx = head[0] - s.latchHead[0], dz = head[2] - s.latchHead[2];
        const bool away = std::fabs(off) > c.deg || std::sqrt(dx * dx + dz * dz) > c.driftM || s.pending;
        if (!away) {
            if (s.phase == 1) { ++s.cancels; }
            s.phase = 0;
            return kFollowIdle;
        }
        if (s.phase == 0) {
            s.phase = 1;
            s.armedAt = nowMs;
        }
        if (nowMs - s.armedAt < c.delayMs) { return kFollowArmed; }
        s.phase = 2;
        s.glideAt = nowMs;
        s.startYaw = panelYaw;
        s.pending = false;
    }

    // Gliding: ease the facing from where it started toward the head's heading NOW (smoothstep), placed from the head NOW.
    float t = c.glideMs > 0 ? static_cast<float>(nowMs - s.glideAt) / static_cast<float>(c.glideMs) : 1.0f;
    if (t > 1.0f) { t = 1.0f; }
    const float e = t * t * (3.0f - 2.0f * t);
    const float yaw = s.startYaw + WrapDeg(headYaw - s.startYaw) * e;
    const float half = yaw * (3.14159265f / 360.0f);
    const float yq[4] = { 0.0f, std::sin(half), 0.0f, std::cos(half) };
    float p[3], q[4];
    if (!LatchSide(head, yq, c.distance, c.height, c.side, p, q)) { return kFollowNoHeading; }
    for (int i = 0; i < 3; ++i) { panelPos[i] = p[i]; }
    for (int i = 0; i < 4; ++i) { panelQuat[i] = q[i]; }
    if (t < 1.0f) { return kFollowGliding; }
    ++s.follows;
    HudFollowReset(s, head);
    return kFollowSettled;
}

inline void QuatMul(const float a[4], const float b[4], float o[4])
{
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

void Rotate(const float q[4], const float v[3], float out[3]);

// THE ROLL FIX (INV-534 (3)). The game image is RENDERED at eye orientation `qRendered` and DECLARED to the runtime at
// `qDeclared` (the declared pitch, ovr2xr.cpp PitchPose): the runtime shows the whole game world turned by
// M = qDeclared x inverse(qRendered) about the eye. A panel latched in the game's own frame must be shown turned by the
// same M about `pivot` (the head) each frame, or the game image tilts against it as the head turns - a pitch seen off to
// the side is a roll. What must hold: the panel in the DECLARED camera's frame == the latched panel in the RENDERED
// camera's frame.
inline void Redeclare(const float panelPos[3], const float panelQuat[4], const float pivot[3], const float qRendered[4],
                      const float qDeclared[4], float outPos[3], float outQuat[4])
{
    const float inv[4] = { -qRendered[0], -qRendered[1], -qRendered[2], qRendered[3] };
    float m[4];
    QuatMul(qDeclared, inv, m);
    const float d[3] = { panelPos[0] - pivot[0], panelPos[1] - pivot[1], panelPos[2] - pivot[2] };
    float r[3];
    Rotate(m, d, r);
    outPos[0] = pivot[0] + r[0]; outPos[1] = pivot[1] + r[1]; outPos[2] = pivot[2] + r[2];
    QuatMul(m, panelQuat, outQuat);
}

// A vector turned by a quaternion (for the test and the log).
inline void Rotate(const float q[4], const float v[3], float out[3])
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float tx = 2.0f * (y * v[2] - z * v[1]), ty = 2.0f * (z * v[0] - x * v[2]), tz = 2.0f * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

}  // namespace uipanel
}  // namespace dlvr
