// binds_logic - the Controls tab's bind model: dlvr\binds.txt <-> per-action slots <-> the loose DW\dlvr_binds.scr.
// docs/superpowers/plans/2026-10-28-controls-rebind.md Task 2; spec docs/superpowers/specs/2026-10-28-controls-rebind-design.md.
//
// The FIXED Data3 inputs_pad.scr imports dlvr_binds.scr and reaches its four base layouts through `use DlvrLayout_<name>();`
// (tools/binds_template.py). EmitScr writes those subs: one AddAction line per BOUND slot, in the original file order, a slot 2
// right after its slot 1; an unbound slot is no line at all. Only the four varying AddAction fields change - analog, input,
// reverse, halfscope; target, device and modifiers ({ HoldTime(...) } etc.) come from the generated table, verbatim.
//
// binds.txt: one line per action that differs from its default:  <_ACTION_ID> <slot1> <slot2>
//   slot = <EJoy name>,<analog 0|1>,<reverse 0|1>,<half 0|1>   or   -   (unbound).  '#' starts a comment line.
//
// Header-only, no Windows headers: shared by the bootstrap (writes the .scr), the layer (the tab) and the unit test.
// EmitScr MUST match binds_template.py emit_loose() byte for byte - binds_logic_test.cpp checks it against the generator's file.
#pragma once

#include <cstring>
#include <string>

#include "binds_table.gen.h"

namespace dlvr::binds {

struct Slot {
    bool bound;
    int input;      // index into gen::kInputs
    bool analog;
    bool reverse;
    bool half;
};

struct Binds {
    Slot s[gen::kActionCount][2];
};

inline bool Equal(const Slot& a, const Slot& b)
{
    if (a.bound != b.bound) { return false; }
    if (!a.bound) { return true; }
    return a.input == b.input && a.analog == b.analog && a.reverse == b.reverse && a.half == b.half;
}

inline int FindAction(const char* id)
{
    for (int i = 0; i < gen::kActionCount; ++i) {
        if (std::strcmp(gen::kActions[i].id, id) == 0) { return i; }
    }
    return -1;
}

inline int FindInput(const char* name)
{
    for (int i = 0; i < gen::kInputCount; ++i) {
        if (std::strcmp(gen::kInputs[i], name) == 0) { return i; }
    }
    return -1;
}

// Not listed on the tab and never changed by binds.txt: the mod's own contextual binds (an empty category in the table).
inline bool Hidden(int action) { return gen::kActions[action].category[0] == '\0'; }

inline Binds Defaults()
{
    Binds b{};
    for (int i = 0; i < gen::kLineCount; ++i) {
        const gen::Line& l = gen::kLines[i];
        b.s[l.action][l.slot] = Slot{ l.bound, l.input, l.analog, l.reverse, l.half };
    }
    return b;
}

namespace detail {

inline std::string SlotText(const Slot& s)
{
    if (!s.bound) { return "-"; }
    std::string t = gen::kInputs[s.input];
    t += s.analog ? ",1" : ",0";
    t += s.reverse ? ",1" : ",0";
    t += s.half ? ",1" : ",0";
    return t;
}

inline bool ParseFlag(const std::string& t, bool* out)
{
    if (t == "0") { *out = false; return true; }
    if (t == "1") { *out = true; return true; }
    return false;
}

// "-" or "NAME,a,r,h". On failure *why names the problem.
inline bool ParseSlot(const std::string& t, Slot* out, std::string* why)
{
    if (t == "-") { *out = Slot{ false, 0, false, false, false }; return true; }
    std::string part[4];
    int n = 0;
    size_t start = 0;
    for (size_t i = 0; i <= t.size(); ++i) {
        if (i == t.size() || t[i] == ',') {
            if (n == 4) { *why = "slot '" + t + "' has more than 4 fields"; return false; }
            part[n++] = t.substr(start, i - start);
            start = i + 1;
        }
    }
    if (n != 4) { *why = "slot '" + t + "' needs 4 fields (input,analog,reverse,half) or -"; return false; }
    const int in = FindInput(part[0].c_str());
    if (in < 0) { *why = "unknown input '" + part[0] + "'"; return false; }
    Slot s{ true, in, false, false, false };
    if (!ParseFlag(part[1], &s.analog) || !ParseFlag(part[2], &s.reverse) || !ParseFlag(part[3], &s.half)) {
        *why = "slot '" + t + "': analog / reverse / half must each be 0 or 1";
        return false;
    }
    *out = s;
    return true;
}

}  // namespace detail

// Applies text on top of *out (start from Defaults()). Returns the number of rejected lines; *report gets one line per
// rejection, naming the line number and the reason. Every acceptable line is applied whatever else is wrong.
inline int Parse(const char* text, Binds* out, std::string* report)
{
    int rejected = 0;
    bool seen[gen::kActionCount] = {};
    int lineNo = 0;
    const char* p = text ? text : "";
    while (*p) {
        const char* e = p;
        while (*e && *e != '\n') { ++e; }
        std::string line(p, e);
        p = *e ? e + 1 : e;
        ++lineNo;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) { line.pop_back(); }
        size_t lead = 0;
        while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) { ++lead; }
        line.erase(0, lead);
        if (line.empty() || line[0] == '#') { continue; }
        std::string tok[3];
        int n = 0;
        bool tooMany = false;
        size_t i = 0;
        while (i < line.size()) {
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) { ++i; }
            if (i >= line.size()) { break; }
            size_t j = i;
            while (j < line.size() && line[j] != ' ' && line[j] != '\t') { ++j; }
            if (n == 3) { tooMany = true; break; }
            tok[n++] = line.substr(i, j - i);
            i = j;
        }
        auto reject = [&](const std::string& why) {
            ++rejected;
            *report += "line " + std::to_string(lineNo) + ": " + why + "\n";
        };
        if (n != 3 || tooMany) { reject("needs exactly 3 fields: <_ACTION_ID> <slot1> <slot2>"); continue; }
        const int a = FindAction(tok[0].c_str());
        if (a < 0) { reject("unknown action '" + tok[0] + "'"); continue; }
        if (Hidden(a)) { reject("action '" + tok[0] + "' is the mod's own bind and cannot be changed"); continue; }
        if (seen[a]) { reject("action '" + tok[0] + "' appears twice - the first line stands"); continue; }
        Slot s0, s1;
        std::string why;
        if (!detail::ParseSlot(tok[1], &s0, &why) || !detail::ParseSlot(tok[2], &s1, &why)) {
            reject("action '" + tok[0] + "': " + why);
            continue;
        }
        if (s0.bound && Equal(s0, s1)) { s1.bound = false; }      // the same input twice is stored once
        seen[a] = true;
        out->s[a][0] = s0;
        out->s[a][1] = s1;
    }
    return rejected;
}

// Only the actions that differ from their defaults, one line each, '\n'-terminated. Defaults -> "".
inline std::string Serialize(const Binds& b)
{
    const Binds def = Defaults();
    std::string out;
    for (int a = 0; a < gen::kActionCount; ++a) {
        if (Hidden(a)) { continue; }
        if (Equal(b.s[a][0], def.s[a][0]) && Equal(b.s[a][1], def.s[a][1])) { continue; }
        out += gen::kActions[a].id;
        out += ' ';
        out += detail::SlotText(b.s[a][0]);
        out += ' ';
        out += detail::SlotText(b.s[a][1]);
        out += '\n';
    }
    return out;
}

// The loose DW\dlvr_binds.scr text. Hidden actions always emit their defaults.
inline std::string EmitScr(const Binds& b, const char* nl = "\r\n")
{
    const Binds def = Defaults();
    std::string out;
    auto line = [&](const std::string& s) { out += s; out += nl; };
    line("// GENERATED by DyingLightVR from dlvr\\binds.txt - do not edit; rewritten every launch.");
    line("import \"inputs.def\"");
    line("");
    for (int li = 0; li < gen::kLayoutCount; ++li) {
        line(std::string("sub ") + gen::kLayouts[li].sub + "()");
        line("{");
        line(std::string("    ") + gen::kLayouts[li].head);
        line("    {");
        line("        Preset(\"\")");
        line("        {");
        for (int i = 0; i < gen::kLineCount; ++i) {
            const gen::Line& l = gen::kLines[i];
            if (l.layout != li) { continue; }
            const Slot& s = Hidden(l.action) ? def.s[l.action][l.slot] : b.s[l.action][l.slot];
            if (!s.bound) { continue; }
            std::string t = "            ";
            t += l.prefix;
            t += s.analog ? "true, " : "false, ";
            t += gen::kInputs[s.input];
            t += s.reverse ? ", true, " : ", false, ";
            t += s.half ? "true" : "false";
            t += l.suffix;
            line(t);
        }
        line("        }");
        line("    }");
        line("}");
        line("");
    }
    // binds_template.py joins its lines with nl (no terminator after the last, empty, element): drop the final nl to match
    out.resize(out.size() - std::strlen(nl));
    return out;
}

}  // namespace dlvr::binds
