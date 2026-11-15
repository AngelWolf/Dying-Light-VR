// The calibration screen's layout - build A plain shapes (build D replaces them with game / hand-made art).
// docs/superpowers/specs/2026-10-14-calibration-screen-design.md section 2. Render thread, inside an ImGui frame.
// Colours: the game's own menu palette (docs/RESEARCH-CALIB-ASSETS-2026-10-14.md - text 0xffe5e5e5, accent 0xffdd8b1b,
// background 0xff101010). ImGui 1.92.8: AddRect / PathStroke take (thickness, flags) in that order.
#include "calibdraw.h"

#include <cmath>
#include <cstdio>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "calibassets.h"

namespace dlvr {
namespace {

const ImU32 kBg = IM_COL32(0x10, 0x10, 0x10, 235), kText = IM_COL32(0xe5, 0xe5, 0xe5, 255),
            kDim = IM_COL32(0x98, 0x98, 0x98, 255), kAccent = IM_COL32(0xdd, 0x8b, 0x1b, 255),
            kOk = IM_COL32(0x6c, 0xc0, 0x5a, 255), kBad = IM_COL32(0xd0, 0x50, 0x40, 255);

const char* const kPoseText[calib::kPoses] = {
    "BOTH ARMS STRAIGHT OUT IN FRONT - elbows locked",
    "BOTH ARMS STRAIGHT OUT TO THE SIDES - elbows locked",
    "BOTH ARMS STRAIGHT UP - elbows locked",
    "BOTH ARMS STRAIGHT DOWN BY YOUR SIDES - elbows locked",
};
// Build C (Jay 2026-11-15): the seated set's 4th pose (spec section 2, seated set "B").
const char* const kSeated45Text = "BOTH ARMS DOWN AND OUT AT 45 DEGREES - elbows locked";
const char* const kVerdict[5] = { "--", "OK", "not fully extended", "wrong direction", "not tracked" };

// Draw callback: binds the Boxed font's sampler (linear, clamp, MaxLOD 3) on PS slot 0. ImGui's own DX11 backend sets
// a single sampler per frame; this one is bracketed by DrawCallback_ResetRenderState after the text run. Runs while the
// backend renders, so Renderer_RenderState is valid. userdata = the CalibFont's sampler.
void SetFontSampler(const ImDrawList*, const ImDrawCmd* cmd)
{
    ImGui_ImplDX11_RenderState* rs = static_cast<ImGui_ImplDX11_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    ID3D11SamplerState* smp = static_cast<ID3D11SamplerState*>(cmd->UserCallbackData);
    if (rs && rs->DeviceContext && smp) { rs->DeviceContext->PSSetSamplers(0, 1, &smp); }
}

// Text in the game's Boxed font through our own quads when it is loaded, else ImGui's default font (today's path).
// y = the TOP of the text. COST, per Text() call, per frame: 2 draw callbacks + 1 texture push/pop (3 extra draw
// commands) + 4 vertices / 6 indices per glyph (a 50-glyph line = 200 vertices); no allocation beyond ImGui's own
// draw-list growth, no per-frame texture or sampler creation. Nothing runs while the screen is closed.
void Text(ImDrawList* dl, float size, ImVec2 at, ImU32 col, const char* s, bool centre, float w)
{
    if (const CalibFont* bf = CalibArtFont()) {
        const calibart::FontMetrics& m = *bf->m;
        ImVec2 p = at;
        if (centre) { p.x = (w - calibart::TextWidth(m, s, size)) * 0.5f; }
        const float scale = size / bf->fontHeight;
        const float spaceAdv = m.have[32] ? (float)m.g[32].adv * scale : 0.0f;   // a missing code advances like a space
        dl->AddCallback(&SetFontSampler, bf->sampler);
        dl->PushTexture(ImTextureRef((ImTextureID)bf->tex));
        float pen = p.x;
        for (const char* c = s; *c; ++c) {
            const unsigned char u = (unsigned char)*c;
            if (u >= 128 || !m.have[u]) { pen += spaceAdv; continue; }
            const calibart::PackedGlyph& g = bf->g[u];
            if (g.w > 0 && g.h > 0) {
                const float x1 = pen + (float)g.w * scale, y1 = p.y + (float)g.h * scale;
                dl->PrimReserve(6, 4);
                const ImDrawIdx b = (ImDrawIdx)dl->_VtxCurrentIdx;
                dl->PrimWriteVtx(ImVec2(pen, p.y), ImVec2(g.u0, g.v0), col);
                dl->PrimWriteVtx(ImVec2(x1, p.y), ImVec2(g.u1, g.v0), col);
                dl->PrimWriteVtx(ImVec2(x1, y1), ImVec2(g.u1, g.v1), col);
                dl->PrimWriteVtx(ImVec2(pen, y1), ImVec2(g.u0, g.v1), col);
                dl->PrimWriteIdx(b); dl->PrimWriteIdx((ImDrawIdx)(b + 1)); dl->PrimWriteIdx((ImDrawIdx)(b + 2));
                dl->PrimWriteIdx(b); dl->PrimWriteIdx((ImDrawIdx)(b + 2)); dl->PrimWriteIdx((ImDrawIdx)(b + 3));
            }
            pen += (float)g.adv * scale;
        }
        dl->PopTexture();
        dl->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState, nullptr);
        return;
    }
    ImFont* f = ImGui::GetFont();
    ImVec2 p = at;
    if (centre) { p.x = (w - f->CalcTextSizeA(size, 10000.0f, 0.0f, s).x) * 0.5f; }
    dl->AddText(f, size, p, col, s);
}

// Ring fill through RingFan: a triangle fan (centre, p[i], p[i+1]) of the ring_fill image over its 400 x 400 square
// centred (650, 384). The fan's radius 200 covers the whole image; the stroke's own alpha shapes it.
// COST per frame while the screen is open: <= 130 points, 128 triangles, one texture push/pop.
void RingFillArt(ImDrawList* dl, unsigned long long tex, float frac)
{
    float xy[2 * 130], uv[2 * 130];
    const int n = calibart::RingFan(frac, 650.0f, 384.0f, 200.0f, 128, xy, uv, 130);
    if (n < 3) { return; }
    dl->PushTexture(ImTextureRef((ImTextureID)tex));
    dl->PrimReserve(3 * (n - 2), n);
    const ImDrawIdx b = (ImDrawIdx)dl->_VtxCurrentIdx;
    for (int i = 0; i < n; ++i) {
        dl->PrimWriteVtx(ImVec2(xy[2 * i], xy[2 * i + 1]), ImVec2(uv[2 * i], uv[2 * i + 1]), IM_COL32_WHITE);
    }
    for (int i = 1; i + 1 < n; ++i) {
        dl->PrimWriteIdx(b); dl->PrimWriteIdx((ImDrawIdx)(b + i)); dl->PrimWriteIdx((ImDrawIdx)(b + i + 1));
    }
    dl->PopTexture();
}

// Art element at a rect, or false = not loaded (the caller draws today's shape).
bool Art(ImDrawList* dl, calibart::Img i, ImVec2 a, ImVec2 b)
{
    int tw = 0, th = 0;
    const unsigned long long t = CalibArtTex(i, &tw, &th);
    if (t == 0) { return false; }
    dl->AddImage(ImTextureRef((ImTextureID)t), a, b, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE);
    return true;
}

// A stick figure in the pose: head, body, legs, two straight arms. Forward = arms foreshortened toward the viewer.
void Figure(ImDrawList* dl, ImVec2 c, float s, int pose, ImU32 col)
{
    const ImVec2 neck(c.x, c.y - 0.55f * s), hip(c.x, c.y + 0.25f * s);
    dl->AddCircle(ImVec2(c.x, c.y - 0.75f * s), 0.18f * s, col, 32, 4.0f);
    dl->AddLine(neck, hip, col, 4.0f);
    dl->AddLine(hip, ImVec2(c.x - 0.25f * s, c.y + 0.95f * s), col, 4.0f);
    dl->AddLine(hip, ImVec2(c.x + 0.25f * s, c.y + 0.95f * s), col, 4.0f);
    const ImVec2 sh[2] = { ImVec2(c.x - 0.2f * s, neck.y + 0.08f * s), ImVec2(c.x + 0.2f * s, neck.y + 0.08f * s) };
    for (int k = 0; k < 2; ++k) {
        const float sg = k ? 1.0f : -1.0f;
        ImVec2 hand;
        switch (pose) {
            case 0: hand = ImVec2(sh[k].x + sg * 0.12f * s, sh[k].y + 0.05f * s); break;   // forward (foreshortened)
            case 1: hand = ImVec2(sh[k].x + sg * 0.75f * s, sh[k].y); break;              // side
            case 2: hand = ImVec2(sh[k].x + sg * 0.05f * s, sh[k].y - 0.75f * s); break;  // up
            case 4: hand = ImVec2(sh[k].x + sg * 0.53f * s, sh[k].y + 0.53f * s); break;  // Build C: down and out 45 deg
            default: hand = ImVec2(sh[k].x + sg * 0.08f * s, sh[k].y + 0.75f * s); break; // down
        }
        dl->AddLine(sh[k], hand, col, 5.0f);
        dl->AddCircleFilled(hand, 0.06f * s, col, 16);
    }
}

// (The old Ring() helper's circle + arc now sit inline in CalibDraw as the else-branches of ring_track / ring_fill.)

}  // namespace

void CalibDraw(void* drawList, float w, float h, const void* viewPtr)
{
    ImDrawList* dl = static_cast<ImDrawList*>(drawList);
    const CalibView& v = *static_cast<const CalibView*>(viewPtr);
    // Art when its texture is loaded (CalibArtTex != 0), else today's shape. Art is drawn untinted (D6/D8).
    if (!Art(dl, calibart::kPanelFrame, ImVec2(0, 0), ImVec2(1300, 800))) {
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), kBg, 18.0f);
        dl->AddRect(ImVec2(6, 6), ImVec2(w - 6, h - 6), kAccent, 18.0f, 3.0f);
    }
    if (!Art(dl, calibart::kTitle, ImVec2(300, 10), ImVec2(1000, 130))) {
        Text(dl, 52.0f, ImVec2(0, 34), kAccent, "ARM CALIBRATION", true, w);
    }
    const ImVec2 fc(w * 0.5f, h * 0.48f);
    char line[256];
    switch (v.screen) {
        case calib::Screen::Posture:
            Art(dl, calibart::kPosePosture, ImVec2(458, 192), ImVec2(842, 576));   // no art = no figure (today)
            Text(dl, 38.0f, ImVec2(0, 130), kText, "Will you play STANDING or SEATED?", true, w);
            Text(dl, 46.0f, ImVec2(w * 0.20f, h * 0.45f), v.posture == 0 ? kAccent : kDim, "STANDING", false, w);
            Text(dl, 46.0f, ImVec2(w * 0.62f, h * 0.45f), v.posture == 1 ? kAccent : kDim, "SEATED", false, w);
            Text(dl, 30.0f, ImVec2(0, h - 150), kText, "Stick left / right to choose - squeeze BOTH GRIPS to confirm", true, w);
            break;
        case calib::Screen::SeatedNotice:   // Build C: no longer entered (seated runs its own pose set)
            break;
        case calib::Screen::Pose: {
            _snprintf_s(line, sizeof(line), _TRUNCATE, "%d of %d", v.pose + 1, calib::kPoses);
            Text(dl, 28.0f, ImVec2(0, 686), kDim, line, true, w);   // D7: the step line moved to y 686
            const bool seated45 = v.posture == 1 && v.pose == 3;   // Build C: a seated run's 4th pose
            Text(dl, 34.0f, ImVec2(0, 140), kText,
                 seated45 ? kSeated45Text : kPoseText[(v.pose >= 0 && v.pose < calib::kPoses) ? v.pose : 0], true, w);
            if (v.mainMenu) { Text(dl, 28.0f, ImVec2(0, 182), kDim, "Face forward", true, w); }   // build B, ruling B3
            // ring: track art else the grey circle; fill art (fan) else the accent arc
            if (!Art(dl, calibart::kRingTrack, ImVec2(450, 184), ImVec2(850, 584))) {
                dl->AddCircle(fc, h * 0.24f, kDim, 64, 3.0f);
            }
            {
                int tw = 0, th = 0;
                const unsigned long long ft = CalibArtTex(calibart::kRingFill, &tw, &th);
                if (ft != 0) { RingFillArt(dl, ft, v.ring); }
                else if (v.ring > 0.0f) {
                    const float a0 = -1.5707963f, a1 = a0 + v.ring * 6.2831853f;
                    dl->PathArcTo(fc, h * 0.24f, a0, a1, 64);
                    dl->PathStroke(kAccent, 8.0f);
                }
            }
            // pose figure: the art for v.pose 0..3 (forward, side, up, down), else the stick figure. Build C: a seated run's
            // 4th pose draws pose_down45 (else the 45 deg stick figure, Figure pose 4).
            {
                static const calibart::Img kPoseImg[calib::kPoses] = { calibart::kPoseForward, calibart::kPoseSide,
                                                                       calibart::kPoseUp, calibart::kPoseDown };
                const bool inRange = (v.pose >= 0 && v.pose < calib::kPoses);
                const calibart::Img img = seated45 ? calibart::kPoseDown45 : kPoseImg[inRange ? v.pose : 0];
                if (!(inRange && Art(dl, img, ImVec2(458, 192), ImVec2(842, 576)))) {
                    Figure(dl, fc, h * 0.17f, seated45 ? 4 : v.pose, kText);
                }
            }
            const int vr = (v.vR >= 0 && v.vR <= 4) ? v.vR : 0, vl = (v.vL >= 0 && v.vL <= 4) ? v.vL : 0;
            // D8: verdict text at x 120 / x 930 always; icon 40 x 40 at (70,357) / (880,357): ok for verdict 1, bad for 2..4, none for 0.
            if (vl == 1) { Art(dl, calibart::kIconOk, ImVec2(70, 357), ImVec2(110, 397)); }
            else if (vl >= 2) { Art(dl, calibart::kIconBad, ImVec2(70, 357), ImVec2(110, 397)); }
            if (vr == 1) { Art(dl, calibart::kIconOk, ImVec2(880, 357), ImVec2(920, 397)); }
            else if (vr >= 2) { Art(dl, calibart::kIconBad, ImVec2(880, 357), ImVec2(920, 397)); }
            _snprintf_s(line, sizeof(line), _TRUNCATE, "LEFT: %s", kVerdict[vl]);
            Text(dl, 34.0f, ImVec2(120, h * 0.45f), vl == 1 ? kOk : kBad, line, false, w);
            _snprintf_s(line, sizeof(line), _TRUNCATE, "RIGHT: %s", kVerdict[vr]);
            Text(dl, 34.0f, ImVec2(930, h * 0.45f), vr == 1 ? kOk : kBad, line, false, w);
            const char* prompt = v.reading
                ? (v.pausedArm == 0 ? "PAUSED - right arm out of the pose" :
                   v.pausedArm == 1 ? "PAUSED - left arm out of the pose" : "HOLD STILL...")
                : ((v.vR == 1 && v.vL == 1) ? "Squeeze and hold BOTH GRIPS" : "Get into the pose");
            Text(dl, 36.0f, ImVec2(0, h - 160), v.reading ? kAccent : kText, prompt, true, w);
            break;
        }
        case calib::Screen::Computing:
            Text(dl, 38.0f, ImVec2(0, h * 0.45f), kText, "Measuring...", true, w);
            break;
        case calib::Screen::Passed:
            Text(dl, 44.0f, ImVec2(0, h * 0.38f), kOk, "CALIBRATION COMPLETE", true, w);
            Text(dl, 28.0f, ImVec2(0, h * 0.50f), kText, v.resultLine, true, w);
            break;
        case calib::Screen::Failed:
            Text(dl, 40.0f, ImVec2(0, h * 0.32f), kBad, "CALIBRATION DID NOT COMPLETE", true, w);
            Text(dl, 26.0f, ImVec2(0, h * 0.44f), kText, v.resultLine, true, w);
            Text(dl, 30.0f, ImVec2(0, h - 160), kText, "Squeeze BOTH GRIPS to try again", true, w);
            break;
        case calib::Screen::Closed:
            break;
    }
    Text(dl, 26.0f, ImVec2(0, h - 80), kDim, "Hold B to skip", true, w);
    if (v.skipFrac > 0.0f) {
        // skip bar: cropped art (UV 0..skipFrac) over (390,760)-(390 + 520*skipFrac, 770), else the accent rect.
        int tw = 0, th = 0;
        const unsigned long long st = CalibArtTex(calibart::kSkipBar, &tw, &th);
        if (st != 0) {
            const float sf = v.skipFrac > 1.0f ? 1.0f : v.skipFrac;
            dl->AddImage(ImTextureRef((ImTextureID)st), ImVec2(390, 760), ImVec2(390 + 520 * sf, 770), ImVec2(0, 0),
                         ImVec2(sf, 1), IM_COL32_WHITE);
        } else {
            dl->AddRectFilled(ImVec2(w * 0.3f, h - 40), ImVec2(w * (0.3f + 0.4f * v.skipFrac), h - 30), kAccent);
        }
    }
}

// Build B: the floor - a grid in the menu's accent colour at low alpha, fading toward the edge, drawn in the centred
// h x h square that OverlayRenderCalibFloor submits.
void CalibDrawFloor(void* drawList, float w, float h, const void*)
{
    ImDrawList* dl = static_cast<ImDrawList*>(drawList);
    const float x0 = (w - h) * 0.5f;
    const int n = 12;
    for (int i = 0; i <= n; ++i) {
        const float f = static_cast<float>(i) / static_cast<float>(n);
        const float edge = fabsf(f - 0.5f) * 2.0f;                       // 0 centre .. 1 edge
        const int a = static_cast<int>(90.0f * (1.0f - edge * edge));
        const ImU32 c = IM_COL32(0xdd, 0x8b, 0x1b, a);
        dl->AddLine(ImVec2(x0 + f * h, 0), ImVec2(x0 + f * h, h), c, 2.0f);
        dl->AddLine(ImVec2(x0, f * h), ImVec2(x0 + h, f * h), c, 2.0f);
    }
}

}  // namespace dlvr
