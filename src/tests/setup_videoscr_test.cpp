// Standalone unit test for src/src/setup/setup_videoscr.h - NOT part of any DLL build.
// docs/superpowers/specs/2026-10-17-installer-design.md 5.6; INV-593 (the file this text produces survived a flat
// launch + quit, 1433 bytes). Exit code = number of failures.
#include "setup_videoscr.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace dlvr::setup;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static std::vector<std::string> BodyLines(const std::string& t)
{
    std::vector<std::string> out;
    size_t p = t.find("\r\n\r\n");
    if (p == std::string::npos) {
        return out;
    }
    p += 4;
    while (p < t.size()) {
        const size_t e = t.find("\r\n", p);
        if (e == std::string::npos) {
            out.push_back(t.substr(p));
            break;
        }
        out.push_back(t.substr(p, e - p));
        p = e + 2;
    }
    return out;
}

static int CountLine(const std::string& t, const std::string& line)
{
    int n = 0;
    for (const auto& l : BodyLines(t)) {
        n += (l == line) ? 1 : 0;
    }
    return n;
}

static void header_is_1105_bytes()
{
    const std::string t = VideoScrText();
    const size_t p = t.find("\r\n\r\n");
    CHECK(p != std::string::npos && p + 4 == 1105);
    CHECK(t.compare(0, 18, "!Resolution(i,i)\r\n") == 0);
}
static void no_nvidiaeffects_line()
{
    for (const auto& l : BodyLines(VideoScrText())) {
        CHECK(l.compare(0, 14, "NvidiaEffects(") != 0);
    }
    CHECK(VideoScrText().find("!NvidiaEffects(i,i,i)") != std::string::npos);
}
static void required_lines_present()
{
    const std::string t = VideoScrText();
    const char* req[] = {"MotionBlur(0)", "AntiAliasing(1)", "ShadowMapSize(4096)", "SpotShadowMapSize(4096)",
                         "OculusEnabled()", "VSync(0)", "Version(1)"};
    for (const char* r : req) {
        CHECK(CountLine(t, r) == 1);
    }
}
static void no_machine_lines()
{
    for (const auto& l : BodyLines(VideoScrText())) {
        CHECK(l.compare(0, 11, "Resolution(") != 0);
        CHECK(l.compare(0, 8, "Monitor(") != 0);
    }
}
static void crlf_only()
{
    const std::string t = VideoScrText();
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '\n') {
            CHECK(i > 0 && t[i - 1] == '\r');
        }
    }
    CHECK(t.size() >= 2 && t.compare(t.size() - 2, 2, "\r\n") == 0);
}
static void matches_jay_except_listed()
{
    // Jay's video.scr body, read 2026-10-17 (md5 d7b2b090), in its own order.
    const std::vector<std::string> jay = {
        "Resolution(2560,1440)", "Borderless()", "Monitor(0)", "TextureQuality(\"High\")", "VisRange(3.40, 3.40)",
        "TinyObjectsRange(1.00)", "ShadowMapSize(4096)", "SpotShadowMapSize(8192)", "GammaFloat(1.00)",
        "GrassQuality(0)", "VSync(0)", "ExtraGameFov(20.00)", "OculusEnabled()", "AmbientOcclusion(1)",
        "MotionBlur(0)", "AntiAliasing(1)", "DisableDWM(1)", "3dtvSettings(-0.150000, 0.000000)", "Version(1)"};
    std::vector<std::string> want;
    for (const auto& l : jay) {
        if (l == "Resolution(2560,1440)" || l == "Monitor(0)") {
            continue;
        }
        want.push_back(l == "SpotShadowMapSize(8192)" ? "SpotShadowMapSize(4096)" : l);
    }
    std::vector<std::string> got = BodyLines(VideoScrText());
    // same lines as a set: order is free (the engine rewrites it in its own order on save, INV-593)
    CHECK(got.size() == want.size());
    for (const auto& w : want) {
        CHECK(CountLine(VideoScrText(), w) == 1);
    }
}
static void total_is_inv593_size()
{
    CHECK(VideoScrText().size() == 1433);
}

int main()
{
    header_is_1105_bytes();
    no_nvidiaeffects_line();
    required_lines_present();
    no_machine_lines();
    crlf_only();
    matches_jay_except_listed();
    total_is_inv593_size();
    std::printf("setup_videoscr_test: %d failure(s)\n", g_fail);
    return g_fail;
}
