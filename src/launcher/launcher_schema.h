// The launcher's settings model (spec docs/superpowers/specs/2026-11-15-launcher-design.md section 7): the rows of
// settings_schema.json (tools/settings_schema.py, read from the shipped layer DLL and the BETA menu's own tab rules), which
// of them the VR Comfort and HUD tabs show, and edits to dlvr\ovr2xr.txt through the setup's MergeSettings (in place,
// comments and line endings kept). Pure and header-only - tested by src_beta/src/tests/launcher_schema_test.cpp.
#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "launcher_json.h"
#include "setup_settings.h"

namespace dlvr::launcher {

using setup::KeyValue;

struct SchemaRow {
    std::string key, type, label, help, group, def;
    std::string tab, section;          // tab: comfort | hud | hidden; section: main | hands_arms | hand_steady | menu_panel | hud_panel | wrist
    double lo = 0.0, hi = 1.0;
    std::vector<std::string> items;    // names for an enum-valued int (and off / on for a bool)
    bool restart = false, locked = false, settled = false;
    std::string follows;               // the master's key, or empty
    std::string showIfKey;             // shown only while showIfKey's value == showIfValue (comfort_snap_angle)
    int showIfValue = 0;
};

// false = refused (malformed JSON or a row without key / type / tab), *why says which.
inline bool ParseSchema(const std::string& text, std::vector<SchemaRow>* rows, std::string* dllMd5, std::string* why)
{
    json::Value doc;
    if (!json::Parse(text, &doc, why)) { return false; }
    const json::Value& list = doc.Get("settings");
    if (list.kind != json::Value::Kind::Arr || list.Size() == 0) {
        *why = "settings_schema.json has no \"settings\" list";
        return false;
    }
    std::vector<SchemaRow> out;
    for (size_t i = 0; i < list.Size(); ++i) {
        const json::Value& r = list.At(i);
        SchemaRow s;
        s.key = r.Get("key").Str();
        s.type = r.Get("type").Str();
        s.tab = r.Get("tab").Str();
        if (s.key.empty() || (s.type != "bool" && s.type != "int" && s.type != "float") || s.tab.empty()) {
            *why = "settings_schema.json row " + std::to_string(i) + " lacks key / type / tab";
            return false;
        }
        s.label = r.Get("label").Str();
        s.help = r.Get("help").Str();
        s.group = r.Get("group").Str();
        s.def = r.Get("default").Str();
        s.section = r.Get("section").Str();
        s.lo = r.Get("lo").Num();
        s.hi = r.Get("hi").Num();
        const json::Value& it = r.Get("items");
        for (size_t k = 0; k < it.Size(); ++k) { s.items.push_back(it.At(k).Str()); }
        s.restart = r.Get("restart").Bool();
        s.locked = r.Get("locked").Bool();
        s.settled = r.Get("settled").Bool();
        s.follows = r.Get("follows").Str();
        const json::Value& sif = r.Get("show_if");
        if (sif.kind == json::Value::Kind::Obj && sif.Size() == 1) {
            s.showIfKey = sif.obj[0].first;
            s.showIfValue = static_cast<int>(sif.obj[0].second.Num());
        }
        out.push_back(std::move(s));
    }
    *dllMd5 = doc.Get("dll_md5").Str();
    *rows = std::move(out);
    return true;
}

// The rows a tab section draws, in table order. Locked / follows / hidden rows are never in one.
inline std::vector<int> RowsIn(const std::vector<SchemaRow>& rows, const std::string& tab, const std::string& section)
{
    std::vector<int> out;
    for (size_t i = 0; i < rows.size(); ++i) {
        const SchemaRow& r = rows[i];
        if (r.locked || !r.follows.empty() || r.tab != tab || r.section != section) { continue; }
        out.push_back(static_cast<int>(i));
    }
    return out;
}

// The label, or the key when BETA stripped it (overlay.cpp:1683 does the same).
inline std::string DisplayName(const SchemaRow& r) { return r.label.empty() ? r.key : r.label; }

// The file's value (its last key= line, as the layer reads it), else the shipped default.
inline std::string CurrentValue(const SchemaRow& r, const std::string& ovr2xrText)
{
    std::string v;
    if (setup::LastValueOf(ovr2xrText, r.key, &v)) {
        const size_t a = v.find_first_not_of(" \t");
        const size_t b = v.find_last_not_of(" \t");
        return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
    }
    return r.def;
}

inline const SchemaRow* FindRow(const std::vector<SchemaRow>& rows, const std::string& key)
{
    for (const auto& r : rows) {
        if (r.key == key) { return &r; }
    }
    return nullptr;
}

// show_if: comfort_snap_angle is drawn only while comfort_turn_mode is snap (overlay.cpp DrawComfortTab).
inline bool Visible(const SchemaRow& r, const std::string& ovr2xrText, const std::vector<SchemaRow>& rows)
{
    if (r.showIfKey.empty()) { return true; }
    const SchemaRow* m = FindRow(rows, r.showIfKey);
    const std::string v = m ? CurrentValue(*m, ovr2xrText) : std::string();
    return !v.empty() && std::atoi(v.c_str()) == r.showIfValue;
}

namespace detail {

// The value as the layer would hold it after its own clamp on load: false when the text is not a number.
inline bool Normalise(const SchemaRow& r, const std::string& in, std::string* out)
{
    char* end = nullptr;
    const double d = std::strtod(in.c_str(), &end);
    if (in.empty() || !end || *end != '\0' || std::isnan(d)) { return false; }
    if (r.type == "bool") { *out = d != 0.0 ? "1" : "0"; return true; }
    double c = d;
    if (r.hi > r.lo) { c = c < r.lo ? r.lo : (c > r.hi ? r.hi : c); }
    char buf[64];
    if (r.type == "int") {
        std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::lround(c)));
    } else {
        std::snprintf(buf, sizeof(buf), "%g", c);
    }
    *out = buf;
    return true;
}

}  // namespace detail

// New ovr2xr.txt text. Each edit is clamped to its row's range and written in place; a key that is not a row, or is
// locked / follows a master, or is not a number, is NOT written. Wrist HUD on forces HUD panel on (memory
// wrist-hud-forces-hud-panel; the layer does the same at load, ovr2xr.cpp:6904). No edits = the input, byte for byte.
inline std::string ApplyEdits(const std::string& ovr2xrText, const std::vector<SchemaRow>& rows, const std::vector<KeyValue>& edits)
{
    std::vector<KeyValue> set;
    for (const auto& e : edits) {
        const SchemaRow* r = FindRow(rows, e.key);
        if (!r || r->locked || !r->follows.empty()) { continue; }
        std::string v;
        if (!detail::Normalise(*r, e.value, &v)) { continue; }
        bool replaced = false;
        for (auto& s : set) {
            if (s.key == r->key) { s.value = v; replaced = true; }
        }
        if (!replaced) { set.push_back({r->key, v}); }
    }
    const SchemaRow* wrist = FindRow(rows, "ui_wrist_hud");
    const SchemaRow* panel = FindRow(rows, "ui_hud_panel");
    if (wrist && panel) {
        std::string w = CurrentValue(*wrist, ovr2xrText);
        for (const auto& s : set) {
            if (s.key == "ui_wrist_hud") { w = s.value; }
        }
        if (std::atoi(w.c_str()) != 0) {
            bool have = false;
            for (auto& s : set) {
                if (s.key == "ui_hud_panel") { s.value = "1"; have = true; }
            }
            if (!have && CurrentValue(*panel, ovr2xrText) != "1") { set.push_back({"ui_hud_panel", "1"}); }
        }
    }
    if (set.empty()) { return ovr2xrText; }
    return setup::MergeSettings(ovr2xrText, set, {});
}

// The schema was read from this DLL (md5s, any case). An empty or short md5 never matches.
inline bool SchemaMatches(const std::string& schemaMd5, const std::string& layerMd5)
{
    if (schemaMd5.size() != 32 || layerMd5.size() != 32) { return false; }
    for (size_t i = 0; i < 32; ++i) {
        char a = schemaMd5[i], b = layerMd5[i];
        if (a >= 'A' && a <= 'F') { a = static_cast<char>(a - 'A' + 'a'); }
        if (b >= 'A' && b <= 'F') { b = static_cast<char>(b - 'A' + 'a'); }
        if (a != b) { return false; }
    }
    return true;
}

}  // namespace dlvr::launcher
