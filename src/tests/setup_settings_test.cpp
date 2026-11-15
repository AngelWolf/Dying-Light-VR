// Standalone unit test for src/src/setup/setup_settings.h - NOT part of any DLL build.
// docs/superpowers/specs/2026-10-17-installer-design.md 5.2 (the ovr2xr.txt merge). Exit code = number of failures.
#include "setup_settings.h"

#include <cstdio>

using namespace dlvr::setup;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static const std::vector<std::string> kNone;

static void merge_replaces_in_place()
{
    CHECK(MergeSettings("a=1\r\ncomfort_turn_mode=1\r\nb=2\r\n", {{"comfort_turn_mode", "0"}}, kNone) ==
          "a=1\r\ncomfort_turn_mode=0\r\nb=2\r\n");
}
static void merge_appends_missing()
{
    CHECK(MergeSettings("a=1\r\n", {{"ui_hud_panel", "1"}}, kNone) == "a=1\r\nui_hud_panel=1\r\n");
}
static void merge_case_insensitive_key()
{
    CHECK(MergeSettings("UI_HUD_PANEL=0\n", {{"ui_hud_panel", "1"}}, kNone) == "ui_hud_panel=1\n");
}
static void merge_ignores_comment_and_prefix()
{
    CHECK(MergeSettings("# ui_hud_panel=0\nui_hud_panel_x=3\n", {{"ui_hud_panel", "1"}}, kNone) ==
          "# ui_hud_panel=0\nui_hud_panel_x=3\nui_hud_panel=1\n");
}
static void merge_removes_retired()
{
    CHECK(MergeSettings("old_key=5\r\na=1\r\n", {}, {"old_key"}) == "a=1\r\n");
    CHECK(MergeSettings("OLD_KEY=5\na=1\n", {}, {"old_key"}) == "a=1\n");
}
static void merge_preserves_crlf_and_bom()
{
    const std::string bom = "\xEF\xBB\xBF";
    CHECK(MergeSettings(bom + "a=1\r\nb=2", {{"c", "3"}}, kNone) == bom + "a=1\r\nb=2\r\nc=3\r\n");
    CHECK(MergeSettings(bom + "a=1\r\n", {{"a", "2"}}, kNone) == bom + "a=2\r\n");
    // a BOM-prefixed first key is still matched
    CHECK(MergeSettings(bom + "ui_hud_panel=0\n", {{"ui_hud_panel", "1"}}, kNone) == bom + "ui_hud_panel=1\n");
    // mixed endings: CRLF wins if any line has it
    CHECK(MergeSettings("a=1\nb=2\r\n", {{"c", "3"}}, kNone) == "a=1\nb=2\r\nc=3\r\n");
}
static void merge_empty_file_uses_crlf()
{
    CHECK(MergeSettings("", {{"a", "1"}}, kNone) == "a=1\r\n");
}
static void merge_empty_set_is_identity()
{
    const char* inputs[] = {"", "a=1", "a=1\r\nb=2\r\n", "\xEF\xBB\xBF# c\n\nx=1\n", "a=1\n\n\n"};
    for (const char* in : inputs) {
        CHECK(MergeSettings(in, {}, kNone) == in);
    }
}
static void haskey_cases()
{
    CHECK(HasKey("x=1", "x"));
    CHECK(HasKey("a=2\r\nX=1\r\n", "x"));
    CHECK(!HasKey("# x=1", "x"));
    CHECK(!HasKey("xy=1", "x"));
    CHECK(!HasKey("", "x"));
}
static void fresh_has_header_then_keys()
{
    const std::string f = FreshSettings({{"comfort_turn_mode", "1"}, {"ui_hud_panel", "1"}});
    const std::string head = "# Dying Light VR - the mod's settings.";
    const std::string tail = "comfort_turn_mode=1\r\nui_hud_panel=1\r\n";
    CHECK(f.compare(0, head.size(), head) == 0);
    CHECK(f.size() > tail.size() && f.compare(f.size() - tail.size(), tail.size(), tail) == 0);
    CHECK(f.find("Delete this file to go back to the defaults.\r\n") != std::string::npos);
}

int main()
{
    merge_replaces_in_place();
    merge_appends_missing();
    merge_case_insensitive_key();
    merge_ignores_comment_and_prefix();
    merge_removes_retired();
    merge_preserves_crlf_and_bom();
    merge_empty_file_uses_crlf();
    merge_empty_set_is_identity();
    haskey_cases();
    fresh_has_header_then_keys();
    std::printf("setup_settings_test: %d failure(s)\n", g_fail);
    return g_fail;
}
