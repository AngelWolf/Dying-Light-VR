// launcher_json.h + launcher_schema.h - the schema reader, which rows the launcher shows, and ovr2xr.txt edits
// (spec docs/superpowers/specs/2026-11-15-launcher-design.md section 7).
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "launcher_schema.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace dlvr::launcher;

static const char* kSchema = R"({
 "dll_md5": "8e611a321c9e6f1146da6dcaa0fb07f9",
 "settings": [
  {"key": "render_scale", "type": "float", "lo": 0.25, "hi": 2.0, "default": "1", "label": "", "help": "", "items": [],
   "group": "Resolution", "restart": true, "locked": false, "follows": "", "tab": "hidden", "section": "", "settled": true, "show_if": {}},
  {"key": "comfort_turn_mode", "type": "int", "lo": 0, "hi": 1, "default": "1", "label": "Turning", "help": "Smooth or snap \"turning\".",
   "items": ["smooth", "snap"], "group": "VR Comfort", "restart": false, "locked": false, "follows": "", "tab": "comfort",
   "section": "main", "settled": false, "show_if": {}},
  {"key": "comfort_snap_angle", "type": "int", "lo": 0, "hi": 4, "default": "1", "label": "Snap angle", "help": "",
   "items": ["15 deg", "30 deg", "45 deg", "60 deg", "90 deg"], "group": "VR Comfort", "restart": false, "locked": false,
   "follows": "", "tab": "comfort", "section": "main", "settled": false, "show_if": {"comfort_turn_mode": 1}},
  {"key": "comfort_hand_steady_strength", "type": "int", "lo": 0, "hi": 100, "default": "50", "label": "", "help": "",
   "items": [], "group": "VR Comfort", "restart": false, "locked": false, "follows": "", "tab": "comfort",
   "section": "hand_steady", "settled": false, "show_if": {}},
  {"key": "afw_alt_eye", "type": "bool", "lo": 0, "hi": 1, "default": "1", "label": "", "help": "", "items": ["off", "on"],
   "group": "VR Comfort", "restart": true, "locked": true, "follows": "afw_enable", "tab": "hidden", "section": "",
   "settled": false, "show_if": {}},
  {"key": "ui_hud_panel", "type": "bool", "lo": 0, "hi": 1, "default": "1", "label": "HUD on a fixed panel", "help": "",
   "items": ["off", "on"], "group": "VR Comfort", "restart": true, "locked": false, "follows": "", "tab": "hud",
   "section": "hud_panel", "settled": false, "show_if": {}},
  {"key": "ui_wrist_hud", "type": "bool", "lo": 0, "hi": 1, "default": "0", "label": "Wrist HUD", "help": "",
   "items": ["off", "on"], "group": "VR Comfort", "restart": true, "locked": false, "follows": "", "tab": "hud",
   "section": "wrist", "settled": false, "show_if": {}},
  {"key": "ui_wrist_size", "type": "float", "lo": 0.3, "hi": 3.0, "default": "1", "label": "", "help": "", "items": [],
   "group": "VR Comfort", "restart": false, "locked": false, "follows": "", "tab": "hud", "section": "wrist", "settled": false,
   "show_if": {}}
 ],
 "settled": ["render_scale"],
 "tabs": ["comfort", "hud"]
})";

static std::vector<SchemaRow> Rows()
{
    std::vector<SchemaRow> rows;
    std::string md5, why;
    ParseSchema(kSchema, &rows, &md5, &why);
    return rows;
}

int main()
{
    // ---- JSON
    {
        json::Value v;
        std::string why;
        CHECK(json::Parse(R"({"a": [1, -2.5e1, true, null, "xé\n"], "b": {}})", &v, &why));
        CHECK(v.Get("a").Size() == 5);
        CHECK(v.Get("a").At(1).Num() == -25.0);
        CHECK(v.Get("a").At(2).Bool());
        CHECK(v.Get("a").At(4).Str() == "x\xC3\xA9\n");
        CHECK(v.Get("missing").IsNull());
        CHECK(!json::Parse(R"({"a": [1, 2})", &v, &why) && why.find("offset") != std::string::npos);
        CHECK(!json::Parse(R"({"a": 1} trailing)", &v, &why));
        CHECK(!json::Parse("", &v, &why));
    }
    // ---- the schema
    std::vector<SchemaRow> rows;
    std::string md5, why;
    CHECK(ParseSchema(kSchema, &rows, &md5, &why));
    CHECK(md5 == "8e611a321c9e6f1146da6dcaa0fb07f9");
    CHECK(rows.size() == 8);
    CHECK(!ParseSchema("{\"settings\": 3}", &rows, &md5, &why) && !why.empty());
    rows = Rows();
    {   // the locked / follows row and the hidden row are in no tab
        CHECK(RowsIn(rows, "comfort", "main").size() == 2);
        CHECK(RowsIn(rows, "comfort", "hand_steady").size() == 1);
        CHECK(RowsIn(rows, "hud", "wrist").size() == 2);
        for (const auto& t : {"comfort", "hud"}) {
            for (const auto& s : {"main", "hand_steady", "hands_arms", "menu_panel", "hud_panel", "wrist"}) {
                for (int i : RowsIn(rows, t, s)) {
                    CHECK(rows[i].key != "afw_alt_eye" && rows[i].key != "render_scale");
                }
            }
        }
    }
    {   // an empty label shows the key (overlay.cpp:1683)
        CHECK(DisplayName(rows[3]) == "comfort_hand_steady_strength");
        CHECK(DisplayName(rows[1]) == "Turning");
    }
    {   // snap angle shown only on snap
        CHECK(!Visible(rows[2], "comfort_turn_mode=0\r\n", rows));
        CHECK(Visible(rows[2], "comfort_turn_mode=1\r\n", rows));
        CHECK(Visible(rows[2], "", rows));   // absent = the default (1, snap)
    }
    {   // current value: the file's, else the default
        CHECK(CurrentValue(rows[1], "# c\r\ncomfort_turn_mode=0\r\n") == "0");
        CHECK(CurrentValue(rows[1], "") == "1");
    }
    {   // ApplyEdits changes only the edited line, keeps comments and CRLF, appends a missing key
        const std::string in = "# why the value is what it is\r\ncomfort_turn_mode=1\r\nunknown_key=7\r\n";
        const std::string out = ApplyEdits(in, rows, {{"comfort_turn_mode", "0"}, {"comfort_hand_steady_strength", "80"}});
        CHECK(out == "# why the value is what it is\r\ncomfort_turn_mode=0\r\nunknown_key=7\r\ncomfort_hand_steady_strength=80\r\n");
        CHECK(ApplyEdits(in, rows, {}) == in);
    }
    {   // out-of-range values are clamped; floats written as the layer writes them (%g)
        const std::string out = ApplyEdits("", rows, {{"comfort_hand_steady_strength", "250"}, {"ui_wrist_size", "0.05"},
                                                      {"comfort_snap_angle", "-3"}});
        CHECK(out.find("comfort_hand_steady_strength=100\r\n") != std::string::npos);
        CHECK(out.find("ui_wrist_size=0.3\r\n") != std::string::npos);
        CHECK(out.find("comfort_snap_angle=0\r\n") != std::string::npos);
    }
    {   // a locked or unknown key is never written
        const std::string out = ApplyEdits("", rows, {{"afw_alt_eye", "0"}, {"not_a_key", "1"}});
        CHECK(out.empty());
    }
    {   // Wrist HUD on -> HUD panel on (memory wrist-hud-forces-hud-panel; the layer does the same at load, ovr2xr.cpp:6904)
        const std::string out = ApplyEdits("ui_hud_panel=0\r\n", rows, {{"ui_wrist_hud", "1"}});
        CHECK(out.find("ui_hud_panel=1\r\n") != std::string::npos);
        CHECK(out.find("ui_wrist_hud=1\r\n") != std::string::npos);
        // and turning the panel off while the wrist HUD is on is refused (the panel stays on)
        const std::string out2 = ApplyEdits("ui_wrist_hud=1\r\nui_hud_panel=1\r\n", rows, {{"ui_hud_panel", "0"}});
        CHECK(out2.find("ui_hud_panel=1\r\n") != std::string::npos);
    }
    {   // the schema was read from the DLL that is installed?
        CHECK(SchemaMatches("8E611A321C9E6F1146DA6DCAA0FB07F9", "8e611a321c9e6f1146da6dcaa0fb07f9"));
        CHECK(!SchemaMatches("8e611a321c9e6f1146da6dcaa0fb07f9", "00000000000000000000000000000000"));
        CHECK(!SchemaMatches("", ""));
    }
    std::printf("launcher_schema_test: %d failure(s)\n", g_fail);
    return g_fail;
}
