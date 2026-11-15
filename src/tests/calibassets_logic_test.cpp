// Standalone unit test for src/src/ovr2xr/calibassets_logic.h - NOT part of the DLL build.
// docs/superpowers/plans/2026-10-14-calibration-screen-build-d.md task 3.
// argv[1] = the oracle dump folder (boxed_semibold_30.fm, atlas_alpha.bin 2048 x 4096). Exit code = failures.
// Checks that need the dump are SKIPPED AND COUNTED when it is absent (named on stdout).
#define _CRT_SECURE_NO_WARNINGS   // test-only: fopen
#include "calibassets_logic.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace dlvr::calibart;

static int g_fail = 0;
static int g_skip = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static bool ReadFile(const std::string& p, std::vector<uint8_t>& out)
{
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize((size_t)n);
    size_t got = n ? std::fread(out.data(), 1, (size_t)n, f) : 0;
    std::fclose(f);
    return got == (size_t)n;
}

static bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

static void file_table()
{
    // the slot column of the plan's art table, enum order
    static const char* const want[kImgCount] = {
        "panel_frame.png", "title.png", "pose_posture.png", "pose_forward.png", "pose_side.png",
        "pose_up.png", "pose_down.png", "pose_down45.png", "ring_track.png", "ring_fill.png",
        "icon_ok.png", "icon_bad.png", "skip_bar.png", "floor_grid.png"};
    static_assert(kImgCount == 14, "14 slots");
    for (int i = 0; i < kImgCount; ++i) {
        CHECK(std::strcmp(ImgFile((Img)i), want[i]) == 0);
        CHECK(ImgMaxSide((Img)i) == 4096);
    }
}

static void parsefm_fixture()
{
    const char* t =
        "//generated\nName(\"X\")\nMapWidth(100)\nMapHeight(200)\nFontHeight(75)\n"
        "Char(  65, 40, 0, 0, 40, 75, 0)\n"
        "Char(66, 30, 40, 0, 70, 75, 0)\n"
        "Char(9000, 10, 0, 75, 10, 150, 0)\n"
        "Char(  32, 18, 70, 0, 88, 75, 0)\n";
    FontMetrics m; char why[128] = "";
    CHECK(ParseFm(t, std::strlen(t), &m, why, sizeof why));
    CHECK(m.fontHeight == 75 && m.mapW == 100 && m.mapH == 200);
    CHECK(m.count == 3);
    CHECK(m.have['A'] && m.g['A'].adv == 40 && m.g['A'].x0 == 0 && m.g['A'].y1 == 75 && m.g['A'].x1 == 40);
    CHECK(m.have['B'] && m.g['B'].x0 == 40 && m.g['B'].x1 == 70);
    CHECK(m.have[' '] && m.g[' '].adv == 18);

    // a != x1 - x0 refused, why names the code
    const char* bad = "MapWidth(100)\nMapHeight(200)\nFontHeight(75)\nChar(  67, 41, 0, 0, 40, 75, 0)\n";
    char w2[128] = "";
    FontMetrics m2;
    CHECK(!ParseFm(bad, std::strlen(bad), &m2, w2, sizeof w2));
    CHECK(std::strstr(w2, "67") != nullptr);

    // no FontHeight
    const char* nofh = "MapWidth(100)\nMapHeight(200)\nChar(65, 40, 0, 0, 40, 75, 0)\n";
    char w3[128] = "";
    CHECK(!ParseFm(nofh, std::strlen(nofh), &m2, w3, sizeof w3));
    CHECK(w3[0] != 0);

    // rect outside the map
    const char* oob = "MapWidth(100)\nMapHeight(200)\nFontHeight(75)\nChar(65, 40, 70, 0, 110, 75, 0)\n";
    char w4[128] = "";
    CHECK(!ParseFm(oob, std::strlen(oob), &m2, w4, sizeof w4));
    CHECK(std::strstr(w4, "65") != nullptr);
}

static void parsefm_real(const std::string& dir)
{
    std::vector<uint8_t> b;
    if (!ReadFile(dir + "\\boxed_semibold_30.fm", b)) {
        std::printf("SKIP parsefm_real: %s\\boxed_semibold_30.fm not readable\n", dir.c_str());
        ++g_skip;
        return;
    }
    FontMetrics m; char why[128] = "";
    CHECK(ParseFm((const char*)b.data(), b.size(), &m, why, sizeof why));
    CHECK(m.count == 94);
    CHECK(m.fontHeight == 75);
    CHECK(m.mapW == 2048 && m.mapH == 4096);
    CHECK(!m.have['~']);
    CHECK(m.have[' '] && m.g[' '].adv == 18);
    CHECK(m.have['!'] && m.g['!'].adv == 15);
    std::printf("real fm: count=%d fontHeight=%d map=%dx%d\n", m.count, m.fontHeight, m.mapW, m.mapH);
}

static void pack_synthetic()
{
    // 2 glyphs from a 20 x 10 map: 'A' = rect (0,0)-(6,5) , 'B' = rect (10,2)-(14,9)
    const int MW = 20, MH = 10;
    std::vector<uint8_t> map(MW * MH);
    for (int i = 0; i < MW * MH; ++i) map[i] = (uint8_t)(1 + (i * 7) % 250);
    FontMetrics m;
    m.fontHeight = 7; m.mapW = MW; m.mapH = MH;
    m.g['A'] = {'A', 6, 0, 0, 6, 5}; m.have['A'] = true;
    m.g['B'] = {'B', 4, 10, 2, 14, 9}; m.have['B'] = true;
    m.count = 2;
    std::vector<uint8_t> packed;
    PackedGlyph pg[128] = {};
    const int W = 16, pad = 4;
    int H = PackGlyphs(m, map.data(), W, pad, packed, pg);
    CHECK(H > 0 && H % 8 == 0);
    CHECK((int)packed.size() == W * H);
    // A: w=6 -> x+w+2pad = 14 <= 16 fits; B: x=14, 14+4+8 = 26 > 16 -> new row
    const int ids[2] = {'A', 'B'};
    for (int k = 0; k < 2; ++k) {
        const Glyph& g = m.g[ids[k]];
        const PackedGlyph& p = pg[ids[k]];
        int px = (int)std::lround(p.u0 * W), py = (int)std::lround(p.v0 * H);
        CHECK(p.w == g.x1 - g.x0 && p.h == g.y1 - g.y0 && p.adv == g.adv);
        CHECK(Near(p.u1 * W, (float)(px + p.w)) && Near(p.v1 * H, (float)(py + p.h)));
        bool same = true;
        for (int y = 0; y < p.h; ++y)
            for (int x = 0; x < p.w; ++x)
                if (packed[(py + y) * W + px + x] != map[(g.y0 + y) * MW + g.x0 + x]) same = false;
        CHECK(same);
        // 4 px border all zero
        bool border = true;
        for (int y = py - pad; y < py + p.h + pad; ++y)
            for (int x = px - pad; x < px + p.w + pad; ++x) {
                bool inside = x >= px && x < px + p.w && y >= py && y < py + p.h;
                if (inside) continue;
                if (x < 0 || x >= W || y < 0 || y >= H) { border = false; continue; }
                if (packed[y * W + x] != 0) border = false;
            }
        CHECK(border);
    }
    // B on the second shelf: below A's shelf (tallest 5 + 8 = 13)
    CHECK((int)std::lround(pg['B'].v0 * H) == 13 + pad);
}

static void pack_real(const std::string& dir)
{
    std::vector<uint8_t> fm, atlas;
    if (!ReadFile(dir + "\\boxed_semibold_30.fm", fm) || !ReadFile(dir + "\\atlas_alpha.bin", atlas)) {
        std::printf("SKIP pack_real: fm / atlas_alpha.bin not readable in %s\n", dir.c_str());
        ++g_skip;
        return;
    }
    FontMetrics m; char why[128] = "";
    CHECK(ParseFm((const char*)fm.data(), fm.size(), &m, why, sizeof why));
    CHECK((int)atlas.size() == m.mapW * m.mapH);
    if ((int)atlas.size() != m.mapW * m.mapH) return;
    std::vector<uint8_t> packed;
    PackedGlyph pg[128] = {};
    const int W = 1024, pad = 4;
    int H = PackGlyphs(m, atlas.data(), W, pad, packed, pg);
    std::printf("real packed atlas: %d x %d (pad %d)\n", W, H, pad);
    CHECK(H > 0 && H % 8 == 0 && H <= 1024);
    CHECK((int)packed.size() == W * H);
    int bad = 0, n = 0;
    for (int c = 32; c < 127; ++c) {
        if (!m.have[c]) continue;
        ++n;
        const Glyph& g = m.g[c];
        const PackedGlyph& p = pg[c];
        int px = (int)std::lround(p.u0 * W), py = (int)std::lround(p.v0 * H);
        for (int y = 0; y < p.h; ++y)
            for (int x = 0; x < p.w; ++x)
                if (packed[(py + y) * W + px + x] != atlas[(g.y0 + y) * m.mapW + g.x0 + x]) { ++bad; goto next; }
    next:;
    }
    std::printf("real glyphs round-tripped: %d, mismatching: %d\n", n, bad);
    CHECK(n == 94 && bad == 0);
}

static void mip()
{
    const uint8_t src[8] = {0, 255, 10, 11, 255, 0, 20, 22};   // 4 x 2
    std::vector<uint8_t> d;
    MipDown(src, 4, 2, d);
    CHECK(d.size() == 2);
    CHECK(d[0] == (0 + 255 + 255 + 0 + 2) / 4);   // 128
    CHECK(d[1] == (10 + 11 + 20 + 22 + 2) / 4);   // 16
}

static void textwidth()
{
    FontMetrics m;
    m.fontHeight = 75;
    m.g['A'].adv = 40; m.have['A'] = true;
    m.g['B'].adv = 30; m.have['B'] = true;
    m.g[' '].adv = 18; m.have[' '] = true;
    CHECK(Near(TextWidth(m, "AB", 37.5f), (40 + 30) / 2.0f));
    // missing code advances a space
    CHECK(Near(TextWidth(m, "A\x7f", 75.0f), 40 + 18));
    CHECK(Near(TextWidth(m, "", 75.0f), 0.0f));
}

static void ringfan()
{
    float xy[2 * 80], uv[2 * 80];
    int n = RingFan(0.25f, 0, 0, 10, 64, xy, uv, 80);
    CHECK(n == 1 + 1 + 16);   // centre + start + ceil(0.25 * 64) segment ends
    CHECK(Near(xy[0], 0) && Near(xy[1], 0) && Near(uv[0], 0.5f) && Near(uv[1], 0.5f));
    CHECK(Near(xy[2], 0) && Near(xy[3], -10) && Near(uv[2], 0.5f) && Near(uv[3], 0.0f));
    CHECK(Near(xy[2 * (n - 1)], 10) && Near(xy[2 * (n - 1) + 1], 0));
    CHECK(Near(uv[2 * (n - 1)], 1.0f) && Near(uv[2 * (n - 1) + 1], 0.5f));
    CHECK(RingFan(0, 0, 0, 10, 64, xy, uv, 80) == 0);
    CHECK(RingFan(-1, 0, 0, 10, 64, xy, uv, 80) == 0);
    int f = RingFan(1, 0, 0, 10, 64, xy, uv, 80);
    CHECK(f == 66);
    CHECK(Near(xy[2 * (f - 1)], xy[2]) && Near(xy[2 * (f - 1) + 1], xy[3]));
    CHECK(RingFan(5, 0, 0, 10, 64, xy, uv, 80) == 66);   // clamps
    // a non-multiple fraction ends exactly at its angle: 0.1 -> 36 degrees clockwise from 12 o'clock
    int g = RingFan(0.1f, 0, 0, 10, 64, xy, uv, 80);
    CHECK(g == 1 + 1 + 7);   // ceil(6.4) = 7
    const float a = 0.1f * 6.2831853f;
    CHECK(Near(xy[2 * (g - 1)], 10 * std::sin(a)) && Near(xy[2 * (g - 1) + 1], -10 * std::cos(a)));
    // max limits the write, returns what fit
    float xs[2 * 5], us[2 * 5];
    CHECK(RingFan(1, 0, 0, 10, 64, xs, us, 5) == 5);
}

static void premultiply()
{
    // alpha 255 -> unchanged (several values, incl. the ruling's (128,64,0,255))
    uint8_t opaque[5 * 4] = {0, 0, 0, 255, 255, 255, 255, 255, 128, 64, 0, 255, 1, 2, 3, 255, 200, 17, 99, 255};
    uint8_t keep[5 * 4];
    std::memcpy(keep, opaque, sizeof keep);
    PremultiplySrgbRgba(opaque, 5);
    CHECK(std::memcmp(opaque, keep, sizeof keep) == 0);

    // alpha 0 -> RGB 0, A stays 0
    uint8_t clear[3 * 4] = {255, 255, 255, 0, 10, 200, 77, 0, 1, 1, 1, 0};
    PremultiplySrgbRgba(clear, 3);
    for (int k = 0; k < 3; ++k) {
        CHECK(clear[k * 4] == 0 && clear[k * 4 + 1] == 0 && clear[k * 4 + 2] == 0 && clear[k * 4 + 3] == 0);
    }

    // white at alpha 128: linear 1.0 * 128/255 = 0.50196 -> sRGB 1.055 * 0.50196^(1/2.4) - 0.055 = 0.73667 -> 187.85 -> 188
    // (premultiplying in sRGB space instead would give 255 * 128/255 = 128)
    uint8_t half[4] = {255, 255, 255, 128};
    PremultiplySrgbRgba(half, 1);
    CHECK(half[0] == 188 && half[1] == 188 && half[2] == 188 && half[3] == 128);

    // never brightens: every result channel <= its input, alpha unchanged, over a sweep
    bool ok = true;
    for (int a = 0; a < 256; a += 5)
        for (int c = 0; c < 256; c += 3) {
            uint8_t p[4] = {(uint8_t)c, (uint8_t)c, (uint8_t)c, (uint8_t)a};
            PremultiplySrgbRgba(p, 1);
            if (p[0] > c || p[3] != a) ok = false;
        }
    CHECK(ok);
}

int main(int argc, char** argv)
{
    std::string dir = argc > 1 ? argv[1] : "";
    file_table();
    parsefm_fixture();
    if (dir.empty()) {
        std::printf("SKIP parsefm_real / pack_real: no oracle dir given (argv[1])\n");
        g_skip += 2;
    } else {
        parsefm_real(dir);
        pack_real(dir);
    }
    pack_synthetic();
    mip();
    textwidth();
    ringfan();
    premultiply();
    std::printf("%d failure(s), %d skipped\n", g_fail, g_skip);
    return g_fail;
}
