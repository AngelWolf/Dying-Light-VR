// calibassets_logic.h - PURE logic for the calibration screen's Build D assets: the 14-slot art table, the game font's
// .fm metrics parser, a padded glyph repack + box-filter mips, text width and the ring-fill triangle fan.
// No Windows / D3D / ImGui here - unit-tested standalone by src/src/tests/calibassets_logic_test.cpp.
// docs/superpowers/plans/2026-10-14-calibration-screen-build-d.md task 3.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace dlvr {
namespace calibart {

// ---- the art table (slot column, enum order) -------------------------------------------------------------------------
enum Img { kPanelFrame, kTitle, kPosePosture, kPoseForward, kPoseSide, kPoseUp, kPoseDown, kPoseDown45, kRingTrack,
           kRingFill, kIconOk, kIconBad, kSkipBar, kFloorGrid, kImgCount };          // 14 slots

inline const char* ImgFile(Img i)
{
    static const char* const k[kImgCount] = {
        "panel_frame.png", "title.png", "pose_posture.png", "pose_forward.png", "pose_side.png", "pose_up.png",
        "pose_down.png", "pose_down45.png", "ring_track.png", "ring_fill.png", "icon_ok.png", "icon_bad.png",
        "skip_bar.png", "floor_grid.png"};
    return ((int)i >= 0 && (int)i < kImgCount) ? k[(int)i] : "";
}
// The refusal limit: a PNG bigger than this on either side is refused by name, never uploaded.
inline int ImgMaxSide(Img) { return 4096; }

// ---- the .fm metrics -------------------------------------------------------------------------------------------------
struct Glyph { int code = 0, adv = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0; };
struct FontMetrics { int fontHeight = 0, mapW = 0, mapH = 0; Glyph g[128]; bool have[128] = {}; int count = 0; };

namespace detail {
inline void SetWhy(char* why, size_t whyN, const char* fmt, int a = 0, int b = 0)
{
    if (!why || !whyN) return;
    std::snprintf(why, whyN, fmt, a, b);
}
// Value of "Key(<int>)" when the line (leading blanks skipped) starts with `key` (which includes the open paren).
inline bool KeyInt(const char* line, const char* key, int* v)
{
    size_t k = std::strlen(key);
    if (std::strncmp(line, key, k) != 0) return false;
    *v = std::atoi(line + k);
    return true;
}
// "  32, 18, 408, 350, 426, 425, 0)" -> c[0..6]; blanks around numbers allowed, false on anything else.
inline bool SevenInts(const char* s, int c[7])
{
    for (int i = 0; i < 7; ++i) {
        while (*s == ' ' || *s == '\t') ++s;
        bool neg = false;
        if (*s == '-') { neg = true; ++s; }
        if (*s < '0' || *s > '9') return false;
        long v = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); if (v > 100000000L) return false; ++s; }
        c[i] = (int)(neg ? -v : v);
        while (*s == ' ' || *s == '\t') ++s;
        if (*s != (i < 6 ? ',' : ')')) return false;
        ++s;
    }
    return true;
}
}  // namespace detail

// Parses the .fm text (MapWidth / MapHeight / FontHeight / Char(code, a, x0, y0, x1, y1, b)); keeps codes 32..126 only.
// Refuses (false + why): no FontHeight / MapWidth / MapHeight, a rect outside the map, a != x1 - x0 (the tally that held
// on 298 / 298 - a new file that breaks it is a new format and must not be drawn). `why` is always written on false.
inline bool ParseFm(const char* text, size_t n, FontMetrics* out, char* why, size_t whyN)
{
    if (why && whyN) why[0] = 0;
    if (!text || !out) { detail::SetWhy(why, whyN, "null input"); return false; }
    *out = FontMetrics();
    size_t p = 0;
    while (p < n) {
        char line[256];
        size_t len = 0;
        while (p < n && text[p] != '\n') {
            if (len + 1 < sizeof line) line[len++] = text[p];
            ++p;
        }
        if (p < n) ++p;   // the newline
        line[len] = 0;
        const char* s = line;
        while (*s == ' ' || *s == '\t' || *s == '\r') ++s;
        int v = 0;
        if (detail::KeyInt(s, "MapWidth(", &v)) { out->mapW = v; continue; }
        if (detail::KeyInt(s, "MapHeight(", &v)) { out->mapH = v; continue; }
        if (detail::KeyInt(s, "FontHeight(", &v)) { out->fontHeight = v; continue; }
        if (std::strncmp(s, "Char(", 5) != 0) continue;
        int c[7] = {};
        if (!detail::SevenInts(s + 5, c)) {
            detail::SetWhy(why, whyN, "malformed Char row (code %d)", c[0]);
            return false;
        }
        if (c[0] < 32 || c[0] > 126) continue;   // not printable ASCII - ignored, not an error
        if (c[1] != c[4] - c[2]) {
            detail::SetWhy(why, whyN, "Char %d: advance != x1 - x0 (new format)", c[0]);
            return false;
        }
        if (out->mapW <= 0 || out->mapH <= 0) {
            detail::SetWhy(why, whyN, "Char %d before MapWidth / MapHeight", c[0]);
            return false;
        }
        if (c[2] < 0 || c[3] < 0 || c[4] <= c[2] || c[5] <= c[3] || c[4] > out->mapW || c[5] > out->mapH) {
            detail::SetWhy(why, whyN, "Char %d: rect outside the map", c[0]);
            return false;
        }
        Glyph& g = out->g[c[0]];
        g.code = c[0]; g.adv = c[1]; g.x0 = c[2]; g.y0 = c[3]; g.x1 = c[4]; g.y1 = c[5];
        if (!out->have[c[0]]) { out->have[c[0]] = true; ++out->count; }
    }
    if (out->fontHeight <= 0) { detail::SetWhy(why, whyN, "no FontHeight"); return false; }
    if (out->mapW <= 0 || out->mapH <= 0) { detail::SetWhy(why, whyN, "no MapWidth / MapHeight"); return false; }
    return true;
}

// ---- the repacked atlas ----------------------------------------------------------------------------------------------
// Every glyph of `m` copied from the full alpha map (m.mapW x m.mapH, single channel, row-major) into a `packW`-wide
// shelf pack with `pad` empty pixels round each glyph. Rows left to right; a new row when x + w + 2*pad > packW; row
// height = tallest glyph in the row + 2*pad. Returns the packed height rounded UP to a multiple of 8 (3 mips = 4 levels)
// and fills `packed` (packW * height, zero-filled). `out[code]` = the glyph's INNER rect (pad excluded) in normalized
// packed coordinates + its pixel size and advance. Returns 0 (packed empty) when a glyph cannot fit packW at all - the
// caller must name that and refuse the font.
struct PackedGlyph { float u0, v0, u1, v1; int w, h, adv; };
inline int PackGlyphs(const FontMetrics& m, const uint8_t* alpha, int packW, int pad, std::vector<uint8_t>& packed,
                      PackedGlyph out[128])
{
    packed.clear();
    for (int i = 0; i < 128; ++i) out[i] = PackedGlyph{0, 0, 0, 0, 0, 0, 0};
    if (!alpha || packW <= 0 || pad < 0) return 0;
    int px[128], py[128];
    int x = 0, y = 0, rowH = 0;
    for (int c = 32; c < 127; ++c) {
        if (!m.have[c]) continue;
        const Glyph& g = m.g[c];
        int w = g.x1 - g.x0, h = g.y1 - g.y0;
        if (w + 2 * pad > packW) return 0;
        if (x + w + 2 * pad > packW) { y += rowH; x = 0; rowH = 0; }
        px[c] = x + pad; py[c] = y + pad;
        x += w + 2 * pad;
        rowH = std::max(rowH, h + 2 * pad);
    }
    int H = (y + rowH + 7) / 8 * 8;
    if (H <= 0) return 0;
    packed.assign((size_t)packW * (size_t)H, 0);
    for (int c = 32; c < 127; ++c) {
        if (!m.have[c]) continue;
        const Glyph& g = m.g[c];
        int w = g.x1 - g.x0, h = g.y1 - g.y0;
        for (int r = 0; r < h; ++r)
            std::memcpy(&packed[(size_t)(py[c] + r) * packW + px[c]], alpha + (size_t)(g.y0 + r) * m.mapW + g.x0, (size_t)w);
        out[c] = PackedGlyph{(float)px[c] / packW, (float)py[c] / H, (float)(px[c] + w) / packW, (float)(py[c] + h) / H,
                             w, h, g.adv};
    }
    return H;
}

// One 2x2 box-filter mip level of a single-channel image (w, h even): (a+b+c+d+2)/4.
inline void MipDown(const uint8_t* src, int w, int h, std::vector<uint8_t>& dst)
{
    int ow = w / 2, oh = h / 2;
    dst.assign((size_t)std::max(ow, 0) * (size_t)std::max(oh, 0), 0);
    for (int y = 0; y < oh; ++y)
        for (int x = 0; x < ow; ++x) {
            const uint8_t* a = src + (size_t)(2 * y) * w + 2 * x;
            dst[(size_t)y * ow + x] = (uint8_t)((a[0] + a[1] + a[w] + a[w + 1] + 2) / 4);
        }
}

// ---- premultiplied alpha (the floor art) -----------------------------------------------------------------------------
// In place over `pixels` RGBA8 sRGB pixels with STRAIGHT alpha: each of R, G, B becomes encode(decode(c) * a / 255) - the
// multiply in LINEAR light, as the compositor blends a premultiplied sRGB layer; A is unchanged. decode = a 256-entry
// float table of the standard sRGB EOTF; encode = the exact standard inverse, rounded to nearest. a = 255 -> unchanged,
// a = 0 -> RGB 0. For the floor slot only (an OpenXR layer is read as premultiplied unless flagged otherwise); the other
// slots stay straight - ImGui draws them. Cost: one pow per non-trivial channel, once per launch on the loader thread.
inline float SrgbToLinear(float s)
{
    return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
}
inline uint8_t LinearToSrgb8(float l)
{
    if (!(l > 0.0f)) return 0;
    if (l >= 1.0f) return 255;
    const double d = l;
    const double s = d <= 0.0031308 ? d * 12.92 : 1.055 * std::pow(d, 1.0 / 2.4) - 0.055;
    const long v = std::lround(s * 255.0);
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}
inline void PremultiplySrgbRgba(uint8_t* rgba, size_t pixels)
{
    if (!rgba) return;
    static float dec[256];
    static bool built = false;   // built on first call; the loader thread is the only caller
    if (!built) {
        for (int i = 0; i < 256; ++i) dec[i] = SrgbToLinear((float)i / 255.0f);
        built = true;
    }
    for (size_t k = 0; k < pixels; ++k) {
        uint8_t* p = rgba + k * 4;
        const uint8_t a = p[3];
        if (a == 255) continue;
        if (a == 0) { p[0] = p[1] = p[2] = 0; continue; }
        const float f = (float)a / 255.0f;
        for (int c = 0; c < 3; ++c) p[c] = LinearToSrgb8(dec[p[c]] * f);
    }
}

// ---- text ------------------------------------------------------------------------------------------------------------
// Text in Boxed at `size` px: width in px (sum of advances * size / fontHeight); a missing code advances a space (0 if
// the font has no space either).
inline float TextWidth(const FontMetrics& m, const char* s, float size)
{
    if (!s || m.fontHeight <= 0) return 0.0f;
    long sum = 0;
    for (; *s; ++s) {
        unsigned char u = (unsigned char)*s;
        if (u < 128 && m.have[u]) sum += m.g[u].adv;
        else if (m.have[32]) sum += m.g[32].adv;
    }
    return (float)sum * size / (float)m.fontHeight;
}

// ---- ring fill -------------------------------------------------------------------------------------------------------
// The fan of a fraction 0..1 of a disc of radius r about (cx, cy), from 12 o'clock clockwise on screen (y DOWN, as
// ImGui), `segs` segments per full turn; point 0 = the centre, point 1 = 12 o'clock (cx, cy - r), then ceil(frac * segs)
// segment ends, the last exactly at the fraction's angle. UV maps the disc's bounding square to [0,1]. Returns the point
// count: 0 for frac <= 0 (or max < 1 / segs < 1); frac > 1 is clamped to 1. Never writes more than `max` points - when
// `max` is too small it returns what fit (a truncated fan) and says nothing; size the buffers for 2 + segs.
inline int RingFan(float frac, float cx, float cy, float r, int segs, float* xy, float* uv, int max)
{
    if (!(frac > 0.0f) || segs < 1 || max < 1 || !xy || !uv || !(r > 0.0f)) return 0;
    if (frac > 1.0f) frac = 1.0f;
    const float twoPi = 6.28318530717958647692f;
    int nseg = (int)std::ceil(frac * (float)segs);
    if (nseg < 1) nseg = 1;
    int n = 0;
    auto put = [&](float x, float y) {
        if (n >= max) return;
        xy[2 * n] = x; xy[2 * n + 1] = y;
        uv[2 * n] = (x - (cx - r)) / (2.0f * r);
        uv[2 * n + 1] = (y - (cy - r)) / (2.0f * r);
        ++n;
    };
    put(cx, cy);
    for (int i = 0; i <= nseg; ++i) {
        float ang = (i == nseg) ? frac * twoPi : frac * twoPi * ((float)i / (float)nseg);
        put(cx + r * std::sin(ang), cy - r * std::cos(ang));
    }
    return n;
}

}  // namespace calibart
}  // namespace dlvr
